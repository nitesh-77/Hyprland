// Test client for ticket #3 (New hyprtester capture client, verified against today's
// black-box behavior): creates a solid-color toplevel window, then on request performs a
// zwlr_screencopy_unstable_v1 capture of its output and reports back the pixel color at
// requested coordinates over stdout - so the test binary never needs to link Wayland
// client libs directly, matching every other client in this directory (see
// pointer-warp.cpp for the same stdin/stdout request-reply shape this file follows).
//
// Protocol over stdin (one line per command):
//   color <r> <g> <b>              - fills the window's surface with this solid color (0-255 each)
//   capture <overlay_cursor 0|1> <x1> <y1> <x2> <y2> ... - captures the current output via
//                                     wlr-screencopy and replies with one "pixel r g b a"
//                                     line per requested (x,y) coordinate, or "failed"
//   exit                            - terminates the client
//
// Reports "started" over stdout once the window is mapped and ready for commands, exactly
// like the existing pointer-warp/xdg-interactive clients.

#include <cstring>
#include <sys/poll.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <print>
#include <format>
#include <string>
#include <sstream>
#include <vector>

#include <wayland-client.h>
#include <wayland.hpp>
#include <xdg-shell.hpp>
#include <wlr-screencopy-unstable-v1.hpp>

#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/math/Vector2D.hpp>
#include <hyprutils/os/FileDescriptor.hpp>

using Hyprutils::Math::Vector2D;
using namespace Hyprutils::Memory;

struct SPixelColor {
    uint8_t r = 0, g = 0, b = 0, a = 0;
};

struct SWlState {
    wl_display*                  display = nullptr;
    CSharedPointer<CCWlRegistry> registry;

    // protocols
    CSharedPointer<CCWlCompositor>            wlCompositor;
    CSharedPointer<CCWlSeat>                  wlSeat;
    CSharedPointer<CCWlShm>                   wlShm;
    CSharedPointer<CCXdgWmBase>               xdgShell;
    CSharedPointer<CCWlOutput>                wlOutput;
    CSharedPointer<CCZwlrScreencopyManagerV1> screencopyMgr;

    // window surface shm/buffer stuff
    CSharedPointer<CCWlShmPool> shmPool;
    CSharedPointer<CCWlBuffer>  shmBuf;
    int                         shmFd            = -1;
    size_t                      shmBufSize       = 0;
    bool                        xrgb8888_support = false;
    uint8_t*                    shmData          = nullptr;
    uint8_t                     fillR = 0, fillG = 0, fillB = 0;

    // window/toplevel stuff
    CSharedPointer<CCWlSurface>   surf;
    CSharedPointer<CCXdgSurface>  xdgSurf;
    CSharedPointer<CCXdgToplevel> xdgToplevel;
    Vector2D                      geom = {400, 300};
};

static bool debug, started, shouldExit;

template <typename... Args>
//NOLINTNEXTLINE
static void clientLog(std::format_string<Args...> fmt, Args&&... args) {
    std::println("{}", std::format(fmt, std::forward<Args>(args)...));
    std::fflush(stdout);
}

template <typename... Args>
//NOLINTNEXTLINE
static void debugLog(std::format_string<Args...> fmt, Args&&... args) {
    if (!debug)
        return;
    std::println("{}", std::format(fmt, std::forward<Args>(args)...));
    std::fflush(stdout);
}

static bool bindRegistry(SWlState& state) {
    state.registry = makeShared<CCWlRegistry>((wl_proxy*)wl_display_get_registry(state.display));

    state.registry->setGlobal([&](CCWlRegistry* r, uint32_t id, const char* name, uint32_t version) {
        const std::string NAME = name;
        debugLog("registry global: {} v{} id {}", NAME, version, id);
        if (NAME == "wl_compositor") {
            state.wlCompositor = makeShared<CCWlCompositor>((wl_proxy*)wl_registry_bind((wl_registry*)state.registry->resource(), id, &wl_compositor_interface, 6));
        } else if (NAME == "wl_shm") {
            state.wlShm = makeShared<CCWlShm>((wl_proxy*)wl_registry_bind((wl_registry*)state.registry->resource(), id, &wl_shm_interface, 1));
        } else if (NAME == "wl_seat") {
            state.wlSeat = makeShared<CCWlSeat>((wl_proxy*)wl_registry_bind((wl_registry*)state.registry->resource(), id, &wl_seat_interface, 9));
        } else if (NAME == "xdg_wm_base") {
            state.xdgShell = makeShared<CCXdgWmBase>((wl_proxy*)wl_registry_bind((wl_registry*)state.registry->resource(), id, &xdg_wm_base_interface, 1));
        } else if (NAME == "wl_output") {
            // bind the first output only - the headless test setup uses one output per
            // client's own view for this purpose (the client's own toplevel's output).
            if (!state.wlOutput)
                state.wlOutput = makeShared<CCWlOutput>((wl_proxy*)wl_registry_bind((wl_registry*)state.registry->resource(), id, &wl_output_interface, 2));
        } else if (NAME == "zwlr_screencopy_manager_v1") {
            state.screencopyMgr =
                makeShared<CCZwlrScreencopyManagerV1>((wl_proxy*)wl_registry_bind((wl_registry*)state.registry->resource(), id, &zwlr_screencopy_manager_v1_interface, 3));
        }
    });
    state.registry->setGlobalRemove([](CCWlRegistry* r, uint32_t id) { debugLog("Global {} removed", id); });

    wl_display_roundtrip(state.display);

    if (!state.wlCompositor || !state.wlShm || !state.wlSeat || !state.xdgShell || !state.wlOutput || !state.screencopyMgr) {
        clientLog("Failed to get protocols from Hyprland: compositor={} shm={} seat={} xdgShell={} output={} screencopy={}", (bool)state.wlCompositor, (bool)state.wlShm,
                  (bool)state.wlSeat, (bool)state.xdgShell, (bool)state.wlOutput, (bool)state.screencopyMgr);
        return false;
    }

    return true;
}

static void fillShm(SWlState& state) {
    if (!state.shmData)
        return;

    const size_t PIXELS = (size_t)state.geom.x * (size_t)state.geom.y;
    auto*        px     = reinterpret_cast<uint32_t*>(state.shmData);
    // XRGB8888: byte order in memory is B,G,R,X (little-endian 0xXXRRGGBB)
    const uint32_t COLOR = (0xFFu << 24) | ((uint32_t)state.fillR << 16) | ((uint32_t)state.fillG << 8) | (uint32_t)state.fillB;
    for (size_t i = 0; i < PIXELS; ++i)
        px[i] = COLOR;
}

static bool createShm(SWlState& state, Vector2D geom) {
    if (!state.xrgb8888_support)
        return false;

    size_t stride = (size_t)geom.x * 4;
    size_t size   = (size_t)geom.y * stride;

    if (state.shmData) {
        munmap(state.shmData, state.shmBufSize);
        state.shmData = nullptr;
    }

    if (!state.shmPool) {
        const char* name = "/wl-shm-capture-scene";
        state.shmFd      = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (state.shmFd < 0)
            return false;

        if (shm_unlink(name) < 0 || ftruncate(state.shmFd, (off_t)size) < 0) {
            close(state.shmFd);
            return false;
        }

        state.shmPool = makeShared<CCWlShmPool>(state.wlShm->sendCreatePool(state.shmFd, (int32_t)size));
        if (!state.shmPool->resource()) {
            close(state.shmFd);
            state.shmFd = -1;
            state.shmPool.reset();
            return false;
        }
        state.shmBufSize = size;
    } else if (size > state.shmBufSize) {
        if (ftruncate(state.shmFd, (off_t)size) < 0) {
            close(state.shmFd);
            state.shmFd = -1;
            state.shmPool.reset();
            return false;
        }

        state.shmPool->sendResize((int32_t)size);
        state.shmBufSize = size;
    }

    auto buf = makeShared<CCWlBuffer>(state.shmPool->sendCreateBuffer(0, (int32_t)geom.x, (int32_t)geom.y, (int32_t)stride, WL_SHM_FORMAT_XRGB8888));
    if (!buf->resource())
        return false;

    if (state.shmBuf) {
        state.shmBuf->sendDestroy();
        state.shmBuf.reset();
    }

    state.shmBuf = buf;

    state.shmData = static_cast<uint8_t*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, state.shmFd, 0));
    if (state.shmData == MAP_FAILED) {
        state.shmData = nullptr;
        return false;
    }

    fillShm(state);

    return true;
}

static bool setupToplevel(SWlState& state) {
    state.wlShm->setFormat([&](CCWlShm* p, uint32_t format) {
        if (format == WL_SHM_FORMAT_XRGB8888)
            state.xrgb8888_support = true;
    });

    state.xdgShell->setPing([&](CCXdgWmBase* p, uint32_t serial) { state.xdgShell->sendPong(serial); });

    state.surf = makeShared<CCWlSurface>(state.wlCompositor->sendCreateSurface());
    if (!state.surf->resource())
        return false;

    state.xdgSurf = makeShared<CCXdgSurface>(state.xdgShell->sendGetXdgSurface(state.surf->resource()));
    if (!state.xdgSurf->resource())
        return false;

    state.xdgToplevel = makeShared<CCXdgToplevel>(state.xdgSurf->sendGetToplevel());
    if (!state.xdgToplevel->resource())
        return false;

    state.xdgToplevel->setClose([&](CCXdgToplevel* p) { exit(0); });

    state.xdgToplevel->setConfigure([&](CCXdgToplevel* p, int32_t w, int32_t h, wl_array* arr) {
        state.geom = {400, 300};

        if (!createShm(state, state.geom))
            exit(-1);
    });

    state.xdgSurf->setConfigure([&](CCXdgSurface* p, uint32_t serial) {
        if (!state.shmBuf)
            debugLog("xdgSurf configure but no buf made yet?");

        state.xdgSurf->sendSetWindowGeometry(0, 0, (int32_t)state.geom.x, (int32_t)state.geom.y);
        state.surf->sendAttach(state.shmBuf.get(), 0, 0);
        state.surf->sendDamageBuffer(0, 0, (int32_t)state.geom.x, (int32_t)state.geom.y);
        state.surf->sendCommit();

        state.xdgSurf->sendAckConfigure(serial);

        if (!started) {
            started = true;
            clientLog("started");
        }
    });

    state.xdgToplevel->sendSetTitle("capture-scene test client");
    state.xdgToplevel->sendSetAppId("capture-scene");

    state.surf->sendAttach(nullptr, 0, 0);
    state.surf->sendCommit();

    return true;
}

// Performs one screencopy capture of state.wlOutput and returns the pixel colors at the
// requested (x,y) coordinates, or std::nullopt on failure. Blocks (dispatching the wayland
// display) until the capture completes or fails.
static std::optional<std::vector<SPixelColor>> captureAndReadPixels(SWlState& state, bool overlayCursor, const std::vector<std::pair<int, int>>& points) {
    auto frame = makeShared<CCZwlrScreencopyFrameV1>(state.screencopyMgr->sendCaptureOutput(overlayCursor ? 1 : 0, state.wlOutput->resource()));
    if (!frame->resource())
        return std::nullopt;

    struct SFrameBufInfo {
        uint32_t format = 0, width = 0, height = 0, stride = 0;
        bool     haveBufferInfo = false;
        bool     ready = false, failed = false;
    } info;

    frame->setBuffer([&](CCZwlrScreencopyFrameV1* f, uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
        info.format         = format;
        info.width          = width;
        info.height         = height;
        info.stride         = stride;
        info.haveBufferInfo = true;
    });
    frame->setReady([&](CCZwlrScreencopyFrameV1* f, uint32_t, uint32_t, uint32_t) { info.ready = true; });
    frame->setFailed([&](CCZwlrScreencopyFrameV1* f) { info.failed = true; });
    frame->setFlags([&](CCZwlrScreencopyFrameV1* f, uint32_t) { ; });
    frame->setBufferDone([&](CCZwlrScreencopyFrameV1* f) { ; });

    // wait for the buffer event (sync frame creation)
    for (int i = 0; i < 200 && !info.haveBufferInfo && !info.failed; ++i) {
        wl_display_roundtrip(state.display);
        if (!info.haveBufferInfo && !info.failed)
            usleep(5000);
    }

    if (info.failed || !info.haveBufferInfo)
        return std::nullopt;

    if (info.format != WL_SHM_FORMAT_XRGB8888 && info.format != WL_SHM_FORMAT_ARGB8888) {
        clientLog("capture-scene: unexpected buffer format {}", info.format);
        return std::nullopt;
    }

    const size_t size = (size_t)info.stride * info.height;
    const char*  name = "/wl-shm-capture-scene-out";
    int          fd   = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return std::nullopt;
    shm_unlink(name);
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return std::nullopt;
    }

    auto  pool = makeShared<CCWlShmPool>(state.wlShm->sendCreatePool(fd, (int32_t)size));
    auto  buf  = makeShared<CCWlBuffer>(pool->sendCreateBuffer(0, (int32_t)info.width, (int32_t)info.height, (int32_t)info.stride, info.format));

    auto* data = static_cast<uint8_t*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (data == MAP_FAILED) {
        close(fd);
        return std::nullopt;
    }

    frame->sendCopy(buf->resource());

    for (int i = 0; i < 200 && !info.ready && !info.failed; ++i) {
        wl_display_roundtrip(state.display);
        if (!info.ready && !info.failed)
            usleep(5000);
    }

    std::optional<std::vector<SPixelColor>> result;
    if (info.ready) {
        std::vector<SPixelColor> pixels;
        pixels.reserve(points.size());
        for (const auto& [x, y] : points) {
            SPixelColor c;
            if (x >= 0 && y >= 0 && (uint32_t)x < info.width && (uint32_t)y < info.height) {
                const uint32_t* px  = reinterpret_cast<const uint32_t*>(data + (size_t)y * info.stride);
                const uint32_t  val = px[x];
                c.b                 = val & 0xFF;
                c.g                 = (val >> 8) & 0xFF;
                c.r                 = (val >> 16) & 0xFF;
                c.a                 = info.format == WL_SHM_FORMAT_ARGB8888 ? (val >> 24) & 0xFF : 0xFF;
            }
            pixels.push_back(c);
        }
        result = pixels;
    }

    munmap(data, size);
    close(fd);
    buf->sendDestroy();
    pool->sendDestroy();
    frame->sendDestroy();

    return result;
}

static void parseRequest(SWlState& state, const std::string& line) {
    if (line.starts_with("exit")) {
        shouldExit = true;
        return;
    }

    if (line.starts_with("color ")) {
        std::istringstream iss(line.substr(6));
        int                r, g, b;
        iss >> r >> g >> b;
        state.fillR = (uint8_t)r;
        state.fillG = (uint8_t)g;
        state.fillB = (uint8_t)b;
        fillShm(state);
        state.surf->sendDamageBuffer(0, 0, (int32_t)state.geom.x, (int32_t)state.geom.y);
        state.surf->sendCommit();
        wl_display_roundtrip(state.display);
        clientLog("ok");
        return;
    }

    if (line.starts_with("capture ")) {
        std::istringstream iss(line.substr(8));
        int                overlayCursorInt;
        iss >> overlayCursorInt;

        std::vector<std::pair<int, int>> points;
        int                              x, y;
        while (iss >> x >> y)
            points.emplace_back(x, y);

        auto result = captureAndReadPixels(state, overlayCursorInt != 0, points);
        if (!result) {
            clientLog("failed");
            return;
        }

        for (const auto& c : *result)
            clientLog("pixel {} {} {} {}", c.r, c.g, c.b, c.a);
        clientLog("done");
        return;
    }
}

int main(int argc, char** argv) {
    if (argc != 1 && argc != 2)
        clientLog("Only the \"--debug\" switch is allowed, it turns on debug logs.");

    if (argc == 2 && std::string{argv[1]} == "--debug")
        debug = true;

    SWlState state;

    state.display = wl_display_connect(nullptr);
    if (!state.display) {
        clientLog("Failed to connect to wayland display");
        return -1;
    }

    if (!bindRegistry(state) || !setupToplevel(state))
        return -1;

    std::array<char, 65536> readBuf;
    readBuf.fill(0);
    std::string pending;

    wl_display_flush(state.display);

    struct pollfd fds[2] = {{.fd = wl_display_get_fd(state.display), .events = POLLIN | POLLOUT}, {.fd = STDIN_FILENO, .events = POLLIN}};
    while (!shouldExit && poll(fds, 2, 0) != -1) {
        if (fds[0].revents & POLLIN) {
            wl_display_flush(state.display);

            if (wl_display_prepare_read(state.display) == 0) {
                wl_display_read_events(state.display);
                wl_display_dispatch_pending(state.display);
            } else
                wl_display_dispatch(state.display);

            int ret = 0;
            do {
                ret = wl_display_dispatch_pending(state.display);
                wl_display_flush(state.display);
            } while (ret > 0);
        }

        if (fds[1].revents & POLLIN) {
            ssize_t bytesRead = read(fds[1].fd, readBuf.data(), readBuf.size() - 1);
            if (bytesRead == -1)
                continue;
            readBuf[bytesRead] = 0;
            pending += std::string{readBuf.data(), (size_t)bytesRead};

            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                if (!line.empty())
                    parseRequest(state, line);
            }
        }
    }

    wl_display* display = state.display;
    if (state.shmData)
        munmap(state.shmData, state.shmBufSize);
    if (state.shmFd >= 0)
        close(state.shmFd);
    state = {};

    wl_display_disconnect(display);
    return 0;
}

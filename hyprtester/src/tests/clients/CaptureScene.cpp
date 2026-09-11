// Ticket #3 built this hyprtester capture client; ticket #4 (True exclusion render body
// for renderMonitor()) flipped its assertion to the true-exclusion behavior.
//
// This test builds a scene with two windows - a plain "bottom" window and a "top" window
// flagged no_screen_share - and captures the output via zwlr_screencopy_unstable_v1
// (through the capture-scene client, see clients/capture-scene.cpp), then asserts the
// captured pixel color at the flagged window's box.
//
// With ticket #4 landed, renderMonitor() no longer draws an opaque black box over the
// flagged window; it re-composites the workspace with that surface skipped, so whatever is
// behind it shows through. The assertion therefore expects the BOTTOM window's color (the
// surface behind the flagged one), not black and not the flagged window's own color. See
// hyprland-capture-exclusion-investigation.md and spec-true-capture-exclusion.md
// (Testing Decisions / Seam 2) at the repo root.
//
// The kill-switch path (HYPRLAND_DISABLE_CAPTURE_EXCLUSION -> black box) is ticket #5 and
// is asserted separately once that lands, not here.

#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <optional>
#include <string>
#include <sstream>
#include <sys/poll.h>
#include <thread>
#include <unistd.h>

#include <hyprutils/os/Process.hpp>
#include <hyprutils/os/FileDescriptor.hpp>
#include <hyprutils/memory/SharedPtr.hpp>

#include "../../hyprctlCompat.hpp"
#include "../../Log.hpp"
#include "../../shared.hpp"
#include "../shared.hpp"
#include "tests.hpp"
#include "build.hpp"

using namespace Hyprutils::OS;
using namespace Hyprutils::Memory;

#define SP CSharedPointer

namespace {
    struct SCapturedPixel {
        int r = -1, g = -1, b = -1, a = -1;
    };

    // Drives one instance of the capture-scene client over its stdin/stdout pipe, matching
    // the exact request/reply shape already used by CClient in tests/clients/pointer-warp.cpp.
    class CCaptureSceneClient {
        SP<CProcess>           m_proc;
        std::array<char, 8192> m_readBuf;
        CFileDescriptor        m_readFd, m_writeFd;
        struct pollfd          m_fds;
        bool                   m_ok = false;

      public:
        CCaptureSceneClient() {
            m_proc = makeShared<CProcess>(binaryDir + "/capture-scene", std::vector<std::string>{});
            m_proc->addEnv("WAYLAND_DISPLAY", WLDISPLAY);

            int pipeFds1[2], pipeFds2[2];
            if (pipe(pipeFds1) != 0 || pipe(pipeFds2) != 0) {
                NLog::log("{}Unable to open pipe to capture-scene client", Colors::RED);
                return;
            }

            m_writeFd = CFileDescriptor(pipeFds1[1]);
            m_proc->setStdinFD(pipeFds1[0]);

            m_readFd = CFileDescriptor(pipeFds2[0]);
            m_proc->setStdoutFD(pipeFds2[1]);

            const int COUNT_BEFORE = Tests::windowCount();
            m_proc->runAsync();

            close(pipeFds1[0]);
            close(pipeFds2[1]);

            m_fds = {.fd = m_readFd.get(), .events = POLLIN};

            // wait for "started" - it may share the poll with earlier debug lines, so read
            // until we see it or time out, matching pointer-warp.cpp's approach but with a
            // generous retry loop since this client prints a diagnostic line per global.
            std::string all;
            for (int i = 0; i < 100; ++i) {
                if (poll(&m_fds, 1, 100) == 1 && (m_fds.revents & POLLIN)) {
                    m_readBuf.fill(0);
                    ssize_t n = read(m_readFd.get(), m_readBuf.data(), m_readBuf.size() - 1);
                    if (n > 0) {
                        all += std::string{m_readBuf.data(), (size_t)n};
                        if (all.contains("started"))
                            break;
                    }
                }
            }

            if (!all.contains("started")) {
                NLog::log("{}Failed to start capture-scene client, read: {}", Colors::RED, all);
                return;
            }

            int counter = 0;
            while (Tests::processAlive(m_proc->pid()) && Tests::windowCount() == COUNT_BEFORE) {
                counter++;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (counter > 100) {
                    NLog::log("{}capture-scene client took too long to open a window", Colors::YELLOW);
                    return;
                }
            }

            m_ok = true;
        }

        ~CCaptureSceneClient() {
            if (m_proc) {
                std::string cmd = "exit\n";
                if (m_writeFd.get() != -1)
                    write(m_writeFd.get(), cmd.c_str(), cmd.length());
                kill(m_proc->pid(), SIGKILL);
                m_proc.reset();
            }
        }

        bool ok() const {
            return m_ok;
        }

        pid_t pid() const {
            return m_proc ? m_proc->pid() : -1;
        }

        // Sends "color r g b\n" and waits for the "ok" reply.
        bool setColor(int r, int g, int b) {
            std::string cmd = std::format("color {} {} {}\n", r, g, b);
            if ((size_t)write(m_writeFd.get(), cmd.c_str(), cmd.length()) != cmd.length())
                return false;
            return readLineContaining("ok");
        }

        // Sends "capture overlayCursor x1 y1 x2 y2 ...\n" and parses the resulting "pixel r
        // g b a" lines (one per requested point) up to the "done"/"failed" terminator.
        std::optional<std::vector<SCapturedPixel>> capture(bool overlayCursor, const std::vector<std::pair<int, int>>& points) {
            std::ostringstream oss;
            oss << "capture " << (overlayCursor ? 1 : 0);
            for (const auto& [x, y] : points)
                oss << " " << x << " " << y;
            oss << "\n";
            std::string cmd = oss.str();

            if ((size_t)write(m_writeFd.get(), cmd.c_str(), cmd.length()) != cmd.length())
                return std::nullopt;

            std::vector<SCapturedPixel> pixels;
            std::string                 pending;
            for (int i = 0; i < 100; ++i) {
                if (poll(&m_fds, 1, 100) != 1 || !(m_fds.revents & POLLIN))
                    continue;

                m_readBuf.fill(0);
                ssize_t n = read(m_readFd.get(), m_readBuf.data(), m_readBuf.size() - 1);
                if (n <= 0)
                    continue;

                pending += std::string{m_readBuf.data(), (size_t)n};

                size_t nl;
                while ((nl = pending.find('\n')) != std::string::npos) {
                    std::string line = pending.substr(0, nl);
                    pending.erase(0, nl + 1);

                    if (line == "done")
                        return pixels;
                    if (line == "failed")
                        return std::nullopt;

                    if (line.starts_with("pixel ")) {
                        std::istringstream iss(line.substr(6));
                        SCapturedPixel     p;
                        iss >> p.r >> p.g >> p.b >> p.a;
                        pixels.push_back(p);
                    }
                }
            }

            return std::nullopt;
        }

      private:
        bool readLineContaining(const std::string& needle) {
            std::string pending;
            for (int i = 0; i < 50; ++i) {
                if (poll(&m_fds, 1, 100) != 1 || !(m_fds.revents & POLLIN))
                    continue;
                m_readBuf.fill(0);
                ssize_t n = read(m_readFd.get(), m_readBuf.data(), m_readBuf.size() - 1);
                if (n <= 0)
                    continue;
                pending += std::string{m_readBuf.data(), (size_t)n};
                if (pending.contains(needle))
                    return true;
            }
            return false;
        }
    };
}

TEST_CASE(captureExclusionTrueExclusion) {
    // Bottom window: plain, green - what would be "behind" the flagged window. Explicitly
    // floated and positioned/sized so its on-screen box is known exactly, rather than
    // relying on wherever the default tiling layout happens to place it.
    CCaptureSceneClient bottom;
    if (!bottom.ok())
        FAIL_TEST("Couldn't start the bottom capture-scene client");

    EXPECT(bottom.setColor(0, 255, 0), true);
    EXPECT(getFromSocket(std::format("/dispatch hl.dsp.window.float({{ action = 'set', window = 'pid:{}' }})", bottom.pid())), std::string{"ok"});
    EXPECT(getFromSocket(std::format("/dispatch hl.dsp.window.resize({{ x = 400, y = 300, window = 'pid:{}' }})", bottom.pid())), std::string{"ok"});
    EXPECT(getFromSocket(std::format("/dispatch hl.dsp.window.move({{ x = 100, y = 100, window = 'pid:{}' }})", bottom.pid())), std::string{"ok"});

    // Top window: flagged no_screen_share, blue - so a wrong assertion (e.g. testing
    // against the flagged window's real color) would visibly fail rather than pass by
    // accident matching the bottom window's color. Floated and positioned so it fully
    // overlaps the bottom window's box - the sample point below (200,150) falls inside
    // both windows' real, known geometry, guaranteeing the top window's box (flagged) is
    // what's actually sampled, not wherever the tiler happened to place either window.
    CCaptureSceneClient top;
    if (!top.ok())
        FAIL_TEST("Couldn't start the top capture-scene client");

    EXPECT(top.setColor(0, 0, 255), true);
    EXPECT(getFromSocket(std::format("/dispatch hl.dsp.window.float({{ action = 'set', window = 'pid:{}' }})", top.pid())), std::string{"ok"});
    EXPECT(getFromSocket(std::format("/dispatch hl.dsp.window.resize({{ x = 400, y = 300, window = 'pid:{}' }})", top.pid())), std::string{"ok"});
    EXPECT(getFromSocket(std::format("/dispatch hl.dsp.window.move({{ x = 100, y = 100, window = 'pid:{}' }})", top.pid())), std::string{"ok"});

    // Confirm both windows actually landed at the geometry we asked for before trusting
    // the sample point below - if either resize/move silently failed or got clamped, the
    // test should fail here with a clear message, not produce a confusing pixel mismatch
    // caused by unexpected geometry.
    {
        auto clients = getFromSocket("/clients");
        EXPECT_COUNT_STRING(clients, "at: 100,100", 2);
        EXPECT_COUNT_STRING(clients, "size: 400,300", 2);
    }

    EXPECT(getFromSocket(std::format("/dispatch hl.dsp.window.set_prop({{ window = 'pid:{}', prop = 'no_screen_share', value = '1' }})", top.pid())), std::string{"ok"});

    // Focus the top window last so it's the one actually rendered on top in Z-order -
    // Hyprland stacks the most-recently-focused floating window above others.
    EXPECT(getFromSocket(std::format("/dispatch hl.dsp.focus({{ window = 'pid:{}' }})", top.pid())), std::string{"ok"});

    Tests::sync();

    // Sample well inside both windows' shared box (100,100)-(500,400): (200,150) is 100px
    // right and 50px down from the top-left corner, safely away from any border/gap.
    auto result = bottom.capture(/*overlayCursor=*/false, {{200, 150}});
    if (!result)
        FAIL_TEST("Capture failed or timed out");

    if (result->size() != 1)
        FAIL_TEST("Expected exactly one captured pixel, got {}", result->size());

    const auto& PIXEL = result->at(0);

    // True exclusion (ticket #4): the flagged top window (0,0,255) is skipped during the
    // capture composite, so the BOTTOM window behind it (0,255,0) shows through at this
    // sample point. NOT black (0,0,0) - that was the old black-box behavior this ticket
    // replaced - and NOT the flagged window's own blue (0,0,255).
    EXPECT(PIXEL.r, 0);
    EXPECT(PIXEL.g, 255);
    EXPECT(PIXEL.b, 0);
    EXPECT(PIXEL.a, 255);
}

TEST_CASE(captureExclusionCursorVisible) {
    // Cursor-visibility check for ticket #4: confirm the cursor is still visible in the
    // capture, at a point away from any no_screen_share surface. This scene has no flagged
    // surface, so renderMonitor() takes the early-exit (cheap mirror-texture) path - this
    // guards that the cursor overlay wasn't lost as a side effect of the render-body change.
    // The window is explicitly
    // floated and positioned/sized so both the "under the cursor" and "elsewhere" sample
    // points are known to land inside vs. outside it, rather than guessing based on
    // default tiling placement.
    CCaptureSceneClient client;
    if (!client.ok())
        FAIL_TEST("Couldn't start the capture-scene client");

    EXPECT(client.setColor(0, 255, 0), true);
    EXPECT(getFromSocket(std::format("/dispatch hl.dsp.window.float({{ action = 'set', window = 'pid:{}' }})", client.pid())), std::string{"ok"});
    EXPECT(getFromSocket(std::format("/dispatch hl.dsp.window.resize({{ x = 800, y = 600, window = 'pid:{}' }})", client.pid())), std::string{"ok"});
    EXPECT(getFromSocket(std::format("/dispatch hl.dsp.window.move({{ x = 50, y = 50, window = 'pid:{}' }})", client.pid())), std::string{"ok"});

    {
        auto clients = getFromSocket("/clients");
        EXPECT_CONTAINS(clients, "at: 50,50");
        EXPECT_CONTAINS(clients, "size: 800,600");
    }

    // Cursor point: comfortably inside the window's box (50,50)-(850,650).
    EXPECT(getFromSocket("/dispatch hl.dsp.cursor.move({ x = 100, y = 100 })"), std::string{"ok"});
    Tests::sync();

    // "Elsewhere" point: also inside the window's box, but far from the cursor - both
    // points must land inside the same solid-green window so the only expected
    // difference between them is the cursor overlay itself, not window-boundary noise.
    auto result = client.capture(/*overlayCursor=*/true, {{100, 100}, {700, 500}});
    if (!result)
        FAIL_TEST("Capture failed or timed out");

    if (result->size() != 2)
        FAIL_TEST("Expected exactly two captured pixels, got {}", result->size());

    const auto& CURSOR_PIXEL = result->at(0);
    const auto& PLAIN_PIXEL  = result->at(1);

    // At the cursor position, the pixel must differ from the plain background fill -
    // proving the cursor is actually composited into the capture when overlay_cursor is
    // requested, not silently absent.
    const bool CURSOR_VISIBLE = CURSOR_PIXEL.r != 0 || CURSOR_PIXEL.g != 255 || CURSOR_PIXEL.b != 0;
    if (!CURSOR_VISIBLE)
        MARK_TEST_FAILED("Expected cursor pixel to differ from background green, got ({}, {}, {}, {})", CURSOR_PIXEL.r, CURSOR_PIXEL.g, CURSOR_PIXEL.b, CURSOR_PIXEL.a);
    else
        LOG_OK("Cursor pixel differs from background, cursor is visible in capture. Got ({}, {}, {}, {})", CURSOR_PIXEL.r, CURSOR_PIXEL.g, CURSOR_PIXEL.b, CURSOR_PIXEL.a);

    // Away from the cursor, the background should be the plain, untouched fill color.
    EXPECT(PLAIN_PIXEL.r, 0);
    EXPECT(PLAIN_PIXEL.g, 255);
    EXPECT(PLAIN_PIXEL.b, 0);
    EXPECT(PLAIN_PIXEL.a, 255);
}

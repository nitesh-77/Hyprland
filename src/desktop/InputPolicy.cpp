#include "InputPolicy.hpp"

#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <system_error>

using namespace Desktop;

static constexpr double MAX_DIMENSION = 10'000'000.0;
static constexpr double EPSILON       = 0.5;

static std::string_view trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    return value;
}

static std::vector<std::string_view> split(std::string_view value, char separator) {
    std::vector<std::string_view> result;
    size_t                        start = 0;

    while (start <= value.size()) {
        const auto END = value.find(separator, start);
        result.emplace_back(value.substr(start, END == std::string_view::npos ? std::string_view::npos : END - start));
        if (END == std::string_view::npos)
            break;
        start = END + 1;
    }

    return result;
}

static std::expected<uint64_t, std::string> parseUnsigned(std::string_view value) {
    if (!value.empty() && (value.front() == 'v' || value.front() == 'V'))
        value.remove_prefix(1);

    if (value.empty())
        return std::unexpected("version is empty");

    uint64_t    result = 0;
    const auto* begin  = value.data();
    const auto* end    = begin + value.size();
    const auto  parsed = std::from_chars(begin, end, result, 10);

    if (parsed.ec != std::errc{} || parsed.ptr != end)
        return std::unexpected("version must be an unsigned integer");

    return result;
}

static std::expected<double, std::string> parseNumber(std::string_view value) {
    if (value.empty())
        return std::unexpected("rectangle value is empty");

    double      result = 0.0;
    const auto* begin  = value.data();
    const auto* end    = begin + value.size();
    const auto  parsed = std::from_chars(begin, end, result, std::chars_format::general);

    if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(result))
        return std::unexpected("rectangle values must be finite numbers");

    return result;
}

static std::expected<CBox, std::string> parseRectangle(std::string_view value) {
    const auto VALUES = split(value, ',');
    if (VALUES.size() != 4)
        return std::unexpected("each rectangle must contain x,y,width,height");

    std::array<double, 4> numbers;
    for (size_t i = 0; i < VALUES.size(); ++i) {
        auto number = parseNumber(trim(VALUES[i]));
        if (!number)
            return std::unexpected(number.error());
        numbers[i] = *number;
    }

    return CBox{numbers[0], numbers[1], numbers[2], numbers[3]};
}

static std::expected<SInputPolicySnapshot, std::string> parseSnapshot(std::string_view serialized) {
    if (serialized.empty())
        return std::unexpected("input policy snapshot is empty");

    if (serialized.size() > CInputPolicy::MAX_SERIALIZED_SIZE)
        return std::unexpected("input policy snapshot is too large");

    const auto FIELDS = split(serialized, ';');
    if (FIELDS.size() < 3)
        return std::unexpected("input policy snapshot requires version, viewport, and regions");

    const auto VERSION_FIELD = trim(FIELDS[0]);
    if (!VERSION_FIELD.starts_with("version="))
        return std::unexpected("input policy snapshot is missing version");

    auto version = parseUnsigned(trim(VERSION_FIELD.substr(VERSION_FIELD.find('=') + 1)));
    if (!version)
        return std::unexpected(version.error());

    const auto VIEWPORT_FIELD = trim(FIELDS[1]);
    if (!VIEWPORT_FIELD.starts_with("viewport="))
        return std::unexpected("input policy snapshot is missing viewport");

    const auto VIEWPORT_VALUE = trim(VIEWPORT_FIELD.substr(VIEWPORT_FIELD.find('=') + 1));
    const auto VIEWPORT_PARTS = split(VIEWPORT_VALUE, 'x');
    if (VIEWPORT_PARTS.size() != 2)
        return std::unexpected("viewport must have the form width x height");

    auto viewportWidth = parseNumber(trim(VIEWPORT_PARTS[0]));
    if (!viewportWidth)
        return std::unexpected("viewport width must be a finite number");
    auto viewportHeight = parseNumber(trim(VIEWPORT_PARTS[1]));
    if (!viewportHeight)
        return std::unexpected("viewport height must be a finite number");

    SInputPolicySnapshot snapshot;
    snapshot.version  = *version;
    snapshot.viewport = CBox{0.0, 0.0, *viewportWidth, *viewportHeight};

    bool regionsSeen = false;
    for (size_t i = 2; i < FIELDS.size(); ++i) {
        auto field = trim(FIELDS[i]);
        if (field.starts_with("regions=") || field.starts_with("rectangles=")) {
            if (regionsSeen)
                return std::unexpected("input policy snapshot contains duplicate regions");

            regionsSeen = true;
            field       = trim(field.substr(field.find('=') + 1));
            if (field.empty())
                continue;
        } else if (!regionsSeen)
            return std::unexpected("rectangles must follow the regions field");

        if (snapshot.rectangles.size() >= CInputPolicy::MAX_RECTANGLES)
            return std::unexpected("input policy snapshot contains too many rectangles");

        auto rectangle = parseRectangle(field);
        if (!rectangle)
            return std::unexpected(rectangle.error());
        snapshot.rectangles.emplace_back(*rectangle);
    }

    if (!regionsSeen)
        return std::unexpected("input policy snapshot is missing regions");

    return snapshot;
}

bool CInputPolicy::hasPolicy() const {
    return m_snapshot.has_value();
}

const std::optional<SInputPolicySnapshot>& CInputPolicy::snapshot() const {
    return m_snapshot;
}

CRegion CInputPolicy::region() const {
    return m_snapshot ? m_snapshot->region.copy() : CRegion{};
}

CRegion CInputPolicy::effectiveInputRegion(const CRegion& clientRegion, const Vector2D& surfaceSize) const {
    if (!m_snapshot)
        return clientRegion;

    auto effective = clientRegion.copy();
    effective.intersect(m_snapshot->region);
    if (surfaceSize.x > 0.0 && surfaceSize.y > 0.0)
        effective.intersect(CBox{{}, surfaceSize});
    return effective;
}

bool CInputPolicy::accepts(const CRegion& clientRegion, const Vector2D& point, const Vector2D& surfaceSize) const {
    return effectiveInputRegion(clientRegion, surfaceSize).containsPoint(point);
}

bool CInputPolicy::containsSurfacePoint(const Vector2D& point) const {
    return !m_snapshot || m_snapshot->region.containsPoint(point);
}

bool CInputPolicy::viewportMatches(const Vector2D& surfaceSize) const {
    if (!m_snapshot)
        return true;

    return surfaceSize.x > 0.0 && surfaceSize.y > 0.0 && std::abs(m_snapshot->viewport.w - surfaceSize.x) <= EPSILON && std::abs(m_snapshot->viewport.h - surfaceSize.y) <= EPSILON;
}

std::expected<void, std::string> CInputPolicy::setSerialized(std::string_view serialized, const Vector2D& surfaceSize) {
    auto snapshot = parseSnapshot(serialized);
    if (!snapshot)
        return std::unexpected(snapshot.error());

    return setSnapshot(std::move(*snapshot), surfaceSize);
}

std::expected<void, std::string> CInputPolicy::setSnapshot(SInputPolicySnapshot snapshot, const Vector2D& surfaceSize) {
    if (m_snapshot && snapshot.version <= m_snapshot->version)
        return std::unexpected("input policy version is not newer than the current version");

    if (snapshot.version == 0)
        return std::unexpected("input policy version must be greater than zero");

    if (!std::isfinite(snapshot.viewport.x) || !std::isfinite(snapshot.viewport.y) || !std::isfinite(snapshot.viewport.w) || !std::isfinite(snapshot.viewport.h) ||
        snapshot.viewport.x != 0.0 || snapshot.viewport.y != 0.0 || snapshot.viewport.w <= 0.0 || snapshot.viewport.h <= 0.0 || snapshot.viewport.w > MAX_DIMENSION ||
        snapshot.viewport.h > MAX_DIMENSION)
        return std::unexpected("input policy viewport is invalid");

    if (snapshot.rectangles.size() > MAX_RECTANGLES)
        return std::unexpected("input policy snapshot contains too many rectangles");

    if ((surfaceSize.x > 0.0 || surfaceSize.y > 0.0) &&
        (!std::isfinite(surfaceSize.x) || !std::isfinite(surfaceSize.y) || surfaceSize.x <= 0.0 || surfaceSize.y <= 0.0 ||
         std::abs(snapshot.viewport.w - surfaceSize.x) > EPSILON || std::abs(snapshot.viewport.h - surfaceSize.y) > EPSILON))
        return std::unexpected("input policy viewport does not match the surface size");

    CRegion region;
    for (const auto& rectangle : snapshot.rectangles) {
        if (!std::isfinite(rectangle.x) || !std::isfinite(rectangle.y) || !std::isfinite(rectangle.w) || !std::isfinite(rectangle.h) || rectangle.x < 0.0 || rectangle.y < 0.0 ||
            rectangle.w <= 0.0 || rectangle.h <= 0.0 || rectangle.x + rectangle.w > snapshot.viewport.w || rectangle.y + rectangle.h > snapshot.viewport.h)
            return std::unexpected("input policy rectangle is outside the viewport");

        region.add(rectangle);
    }

    snapshot.region = std::move(region);
    m_snapshot      = std::move(snapshot);
    return {};
}

void CInputPolicy::clear() {
    m_snapshot.reset();
}

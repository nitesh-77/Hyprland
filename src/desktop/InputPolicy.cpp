#include "InputPolicy.hpp"

#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>
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

static std::expected<uint64_t, std::string> parseUnsigned(std::string_view value, const char* fieldName) {
    if (value.empty())
        return std::unexpected(std::format("{} is empty", fieldName));

    uint64_t    result = 0;
    const auto* begin  = value.data();
    const auto* end    = begin + value.size();
    const auto  parsed = std::from_chars(begin, end, result, 10);

    if (parsed.ec != std::errc{} || parsed.ptr != end)
        return std::unexpected(std::format("{} must be an unsigned integer", fieldName));

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

    std::array<double, 4> numbers{};
    for (size_t i = 0; i < VALUES.size(); ++i) {
        auto number = parseNumber(trim(VALUES.at(i)));
        if (!number)
            return std::unexpected(number.error());
        numbers.at(i) = *number;
    }

    return CBox{numbers.at(0), numbers.at(1), numbers.at(2), numbers.at(3)};
}

static std::expected<SInputPolicySnapshot, std::string> parseSnapshot(std::string_view serialized) {
    if (serialized.empty())
        return std::unexpected("input policy snapshot is empty");

    if (serialized.size() > CInputPolicy::MAX_SERIALIZED_SIZE)
        return std::unexpected("input policy snapshot is too large");

    const auto FIELDS = split(serialized, ';');
    if (FIELDS.size() < 4)
        return std::unexpected("input policy snapshot requires version, generation, viewport, and regions");

    const auto VERSION_FIELD = trim(FIELDS.at(0));
    if (!VERSION_FIELD.starts_with("version="))
        return std::unexpected("input policy snapshot is missing version");

    auto version = parseUnsigned(trim(VERSION_FIELD.substr(VERSION_FIELD.find('=') + 1)), "version");
    if (!version)
        return std::unexpected(version.error());
    if (*version != 1)
        return std::unexpected("unsupported input policy schema version");

    const auto GENERATION_FIELD = trim(FIELDS.at(1));
    if (!GENERATION_FIELD.starts_with("generation="))
        return std::unexpected("input policy snapshot is missing generation");

    auto generation = parseUnsigned(trim(GENERATION_FIELD.substr(GENERATION_FIELD.find('=') + 1)), "generation");
    if (!generation)
        return std::unexpected(generation.error());
    if (*generation == 0)
        return std::unexpected("input policy generation must be greater than zero");

    const auto VIEWPORT_FIELD = trim(FIELDS.at(2));
    if (!VIEWPORT_FIELD.starts_with("viewport="))
        return std::unexpected("input policy snapshot is missing viewport");

    const auto VIEWPORT_VALUE = trim(VIEWPORT_FIELD.substr(VIEWPORT_FIELD.find('=') + 1));
    const auto VIEWPORT_PARTS = split(VIEWPORT_VALUE, 'x');
    if (VIEWPORT_PARTS.size() != 2)
        return std::unexpected("viewport must have the form width x height");

    auto viewportWidth = parseNumber(trim(VIEWPORT_PARTS.at(0)));
    if (!viewportWidth)
        return std::unexpected("viewport width must be a finite number");
    auto viewportHeight = parseNumber(trim(VIEWPORT_PARTS.at(1)));
    if (!viewportHeight)
        return std::unexpected("viewport height must be a finite number");

    SInputPolicySnapshot snapshot;
    snapshot.version    = 1;
    snapshot.generation = *generation;
    snapshot.viewport   = CBox{0.0, 0.0, *viewportWidth, *viewportHeight};

    const auto REGIONS_FIELD = trim(FIELDS.at(3));
    if (!REGIONS_FIELD.starts_with("regions="))
        return std::unexpected("input policy snapshot is missing regions");

    auto firstRegion = trim(REGIONS_FIELD.substr(REGIONS_FIELD.find('=') + 1));
    if (!firstRegion.empty()) {
        if (snapshot.rectangles.size() >= CInputPolicy::MAX_RECTANGLES)
            return std::unexpected("input policy snapshot contains too many rectangles");

        auto rectangle = parseRectangle(firstRegion);
        if (!rectangle)
            return std::unexpected(rectangle.error());
        snapshot.rectangles.emplace_back(*rectangle);
    }

    for (size_t i = 4; i < FIELDS.size(); ++i) {
        const auto field = trim(FIELDS.at(i));
        if (field.empty())
            return std::unexpected("rectangle fields must not be empty");
        if (field.contains('='))
            return std::unexpected("unexpected field after regions");

        if (snapshot.rectangles.size() >= CInputPolicy::MAX_RECTANGLES)
            return std::unexpected("input policy snapshot contains too many rectangles");

        auto rectangle = parseRectangle(field);
        if (!rectangle)
            return std::unexpected(rectangle.error());
        snapshot.rectangles.emplace_back(*rectangle);
    }

    return snapshot;
}

bool CInputPolicy::hasPolicy() const {
    return m_snapshot.has_value();
}

uint64_t CInputPolicy::generationFloor() const {
    return m_generationFloor;
}

const std::optional<SInputPolicySnapshot>& CInputPolicy::snapshot() const {
    return m_snapshot;
}

bool CInputPolicy::acceptsPoint(const Vector2D& rootPoint, const CRegion& clientRegion, const Vector2D& surfaceLocalPoint, const Vector2D& surfaceSize) const {
    auto effectiveClientRegion = clientRegion.copy();
    if (surfaceSize.x > 0.0 && surfaceSize.y > 0.0)
        effectiveClientRegion.intersect(CBox{{}, surfaceSize});

    if (!effectiveClientRegion.containsPoint(surfaceLocalPoint))
        return false;

    return !m_snapshot || m_region.containsPoint(rootPoint);
}

bool CInputPolicy::containsRootPoint(const Vector2D& rootPoint) const {
    return !m_snapshot || m_region.containsPoint(rootPoint);
}

bool CInputPolicy::viewportMatches(const Vector2D& rootSize) const {
    if (!m_snapshot)
        return true;

    return rootSize.x > 0.0 && rootSize.y > 0.0 && std::abs(m_snapshot->viewport.w - rootSize.x) <= EPSILON && std::abs(m_snapshot->viewport.h - rootSize.y) <= EPSILON;
}

std::expected<void, std::string> CInputPolicy::setSerialized(std::string_view serialized, const Vector2D& rootSize) {
    auto snapshot = parseSnapshot(serialized);
    if (!snapshot)
        return std::unexpected(snapshot.error());

    return setSnapshot(std::move(*snapshot), rootSize);
}

std::expected<void, std::string> CInputPolicy::setSnapshot(SInputPolicySnapshot snapshot, const Vector2D& rootSize) {
    if (snapshot.version != 1)
        return std::unexpected("unsupported input policy schema version");

    if (snapshot.generation == 0)
        return std::unexpected("input policy generation must be greater than zero");

    if (snapshot.generation <= m_generationFloor)
        return std::unexpected("input policy generation is not newer than the current generation");

    if (!std::isfinite(snapshot.viewport.x) || !std::isfinite(snapshot.viewport.y) || !std::isfinite(snapshot.viewport.w) || !std::isfinite(snapshot.viewport.h) ||
        snapshot.viewport.x != 0.0 || snapshot.viewport.y != 0.0 || snapshot.viewport.w <= 0.0 || snapshot.viewport.h <= 0.0 || snapshot.viewport.w > MAX_DIMENSION ||
        snapshot.viewport.h > MAX_DIMENSION)
        return std::unexpected("input policy viewport is invalid");

    if (snapshot.rectangles.size() > MAX_RECTANGLES)
        return std::unexpected("input policy snapshot contains too many rectangles");

    if ((rootSize.x > 0.0 || rootSize.y > 0.0) &&
        (!std::isfinite(rootSize.x) || !std::isfinite(rootSize.y) || rootSize.x <= 0.0 || rootSize.y <= 0.0 || std::abs(snapshot.viewport.w - rootSize.x) > EPSILON ||
         std::abs(snapshot.viewport.h - rootSize.y) > EPSILON))
        return std::unexpected("input policy viewport does not match the surface size");

    CRegion region;
    for (const auto& rectangle : snapshot.rectangles) {
        if (!std::isfinite(rectangle.x) || !std::isfinite(rectangle.y) || !std::isfinite(rectangle.w) || !std::isfinite(rectangle.h) || rectangle.x < 0.0 || rectangle.y < 0.0 ||
            rectangle.w <= 0.0 || rectangle.h <= 0.0 || rectangle.x + rectangle.w > snapshot.viewport.w || rectangle.y + rectangle.h > snapshot.viewport.h)
            return std::unexpected("input policy rectangle is outside the viewport");

        region.add(rectangle);
    }

    const auto GENERATION = snapshot.generation;
    m_region              = std::move(region);
    m_snapshot            = std::move(snapshot);
    m_generationFloor     = GENERATION;
    return {};
}

void CInputPolicy::clear() {
    m_snapshot.reset();
    m_region.clear();
}

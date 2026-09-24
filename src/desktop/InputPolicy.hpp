#pragma once

#include "../helpers/math/Math.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Desktop {
    /*
     * The serialized form is:
     *
     *   version=<uint>;viewport=<width>x<height>;regions=<x>,<y>,<width>,<height>[;<x>,<y>,<width>,<height>...]
     *
     * Coordinates are in the toplevel's content/surface-local coordinate space.
     * Version is a monotonically increasing snapshot generation. An engaged
     * snapshot with no rectangles is an explicitly empty policy; a disengaged
     * snapshot means that the compositor must not apply a policy.
     */
    struct SInputPolicySnapshot {
        uint64_t          version  = 0;
        CBox              viewport = {};
        std::vector<CBox> rectangles;
        CRegion           region;
    };

    class CInputPolicy {
      public:
        static constexpr size_t MAX_SERIALIZED_SIZE = 16 * 1024;
        static constexpr size_t MAX_RECTANGLES      = 256;

        CInputPolicy() = default;

        bool                                       hasPolicy() const;
        const std::optional<SInputPolicySnapshot>& snapshot() const;
        CRegion                                    region() const;
        CRegion                                    effectiveInputRegion(const CRegion& clientRegion, const Vector2D& surfaceSize) const;
        bool                                       accepts(const CRegion& clientRegion, const Vector2D& point, const Vector2D& surfaceSize) const;
        bool                                       containsSurfacePoint(const Vector2D& point) const;
        bool                                       viewportMatches(const Vector2D& surfaceSize) const;

        std::expected<void, std::string>           setSerialized(std::string_view serialized, const Vector2D& surfaceSize);
        std::expected<void, std::string>           setSnapshot(SInputPolicySnapshot snapshot, const Vector2D& surfaceSize);
        void                                       clear();

      private:
        std::optional<SInputPolicySnapshot> m_snapshot;
    };
}

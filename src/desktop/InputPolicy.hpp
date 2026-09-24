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
     *   version=1;generation=<uint>;viewport=<width>x<height>;regions=<x>,<y>,<width>,<height>[;<x>,<y>,<width>,<height>...]
     *
     * Coordinates are in the toplevel/root content-local coordinate space.
     * Version is the schema version. Generation is a monotonically increasing
     * replacement sequence. An engaged snapshot with no rectangles is an
     * explicitly empty policy; a disengaged snapshot means that the compositor
     * must not apply a policy.
     */
    struct SInputPolicySnapshot {
        uint32_t          version    = 0;
        uint64_t          generation = 0;
        CBox              viewport   = {};
        std::vector<CBox> rectangles;
        CRegion           region;
    };

    class CInputPolicy {
      public:
        static constexpr size_t MAX_SERIALIZED_SIZE = static_cast<size_t>(16) * 1024;
        static constexpr size_t MAX_RECTANGLES      = 256;

        CInputPolicy() = default;

        bool                                       hasPolicy() const;
        uint64_t                                   generationFloor() const;
        const std::optional<SInputPolicySnapshot>& snapshot() const;
        CRegion                                    region() const;
        // The region argument must already be translated into root coordinates.
        CRegion                          effectiveInputRegion(const CRegion& clientRegionInRootCoordinates, const Vector2D& rootSize) const;
        bool                             acceptsPoint(const Vector2D& rootPoint, const CRegion& clientRegion, const Vector2D& surfaceLocalPoint, const Vector2D& surfaceSize) const;
        bool                             containsRootPoint(const Vector2D& rootPoint) const;
        bool                             viewportMatches(const Vector2D& rootSize) const;

        std::expected<void, std::string> setSerialized(std::string_view serialized, const Vector2D& rootSize);
        std::expected<void, std::string> setSnapshot(SInputPolicySnapshot snapshot, const Vector2D& rootSize);
        void                             clear();

      private:
        std::optional<SInputPolicySnapshot> m_snapshot;
        uint64_t                            m_generationFloor = 0;
    };
}

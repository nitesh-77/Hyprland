#include <desktop/InputPolicy.hpp>

#include <gtest/gtest.h>

using namespace Desktop;

namespace {
    constexpr auto SURFACE_SIZE = Vector2D{800.0, 600.0};

    CRegion        fullSurfaceRegion() {
        return CBox{{}, SURFACE_SIZE};
    }

    CRegion partialClientRegion() {
        CRegion region;
        region.add(CBox{100.0, 100.0, 200.0, 200.0});
        region.add(CBox{500.0, 400.0, 100.0, 100.0});
        return region;
    }
}

TEST(InputPolicy, unsetPolicyLeavesClientRegionUnchanged) {
    CInputPolicy policy;
    const auto   clientRegion = fullSurfaceRegion();

    EXPECT_FALSE(policy.hasPolicy());
    EXPECT_TRUE(policy.effectiveInputRegion(clientRegion, SURFACE_SIZE).containsPoint({400.0, 300.0}));
    EXPECT_TRUE(policy.accepts(clientRegion, {400.0, 300.0}, SURFACE_SIZE));
}

TEST(InputPolicy, emptyPolicyIsPresentAndBlocksAllInput) {
    CInputPolicy policy;

    const auto   result = policy.setSerialized("version=1;viewport=800x600;regions=", SURFACE_SIZE);

    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_TRUE(policy.hasPolicy());
    EXPECT_TRUE(policy.region().empty());
    EXPECT_TRUE(policy.effectiveInputRegion(fullSurfaceRegion(), SURFACE_SIZE).empty());
    EXPECT_FALSE(policy.accepts(fullSurfaceRegion(), {400.0, 300.0}, SURFACE_SIZE));
}

TEST(InputPolicy, rectanglesFormAUnionAndPreserveGaps) {
    CInputPolicy policy;

    const auto   result = policy.setSerialized("version=7;viewport=800x600;regions=0,0,100,100;200,0,100,100", SURFACE_SIZE);

    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_TRUE(policy.containsSurfacePoint({50.0, 50.0}));
    EXPECT_TRUE(policy.containsSurfacePoint({250.0, 50.0}));
    EXPECT_FALSE(policy.containsSurfacePoint({150.0, 50.0}));
    EXPECT_EQ(policy.snapshot()->rectangles.size(), 2u);
}

TEST(InputPolicy, policyIntersectsInfiniteAndPartialClientRegions) {
    CInputPolicy policy;
    ASSERT_TRUE(policy.setSerialized("version=3;viewport=800x600;regions=100,100,200,200", SURFACE_SIZE).has_value());

    // An infinite wl_surface input region is materialized as the full surface.
    const auto effectiveFull = policy.effectiveInputRegion(fullSurfaceRegion(), SURFACE_SIZE);
    EXPECT_TRUE(effectiveFull.containsPoint({150.0, 150.0}));
    EXPECT_FALSE(effectiveFull.containsPoint({50.0, 50.0}));

    const auto effectivePartial = policy.effectiveInputRegion(partialClientRegion(), SURFACE_SIZE);
    EXPECT_TRUE(effectivePartial.containsPoint({150.0, 150.0}));
    EXPECT_FALSE(effectivePartial.containsPoint({350.0, 350.0}));
    EXPECT_FALSE(effectivePartial.containsPoint({550.0, 450.0}));
}

TEST(InputPolicy, emptyClientRegionRemainsEmptyAfterIntersection) {
    CInputPolicy policy;
    ASSERT_TRUE(policy.setSerialized("version=3;viewport=800x600;regions=0,0,800,600", SURFACE_SIZE).has_value());

    const CRegion emptyClientRegion;
    EXPECT_TRUE(policy.effectiveInputRegion(emptyClientRegion, SURFACE_SIZE).empty());
    EXPECT_FALSE(policy.accepts(emptyClientRegion, {10.0, 10.0}, SURFACE_SIZE));
}

TEST(InputPolicy, invalidReplacementDoesNotReplaceLastValidPolicy) {
    CInputPolicy policy;
    ASSERT_TRUE(policy.setSerialized("version=4;viewport=800x600;regions=10,10,100,100", SURFACE_SIZE).has_value());

    EXPECT_FALSE(policy.setSerialized("version=5;viewport=801x600;regions=0,0,10,10", SURFACE_SIZE).has_value());
    EXPECT_EQ(policy.snapshot()->version, 4u);
    EXPECT_TRUE(policy.containsSurfacePoint({20.0, 20.0}));
    EXPECT_FALSE(policy.containsSurfacePoint({5.0, 5.0}));
    EXPECT_TRUE(policy.viewportMatches(SURFACE_SIZE));
    EXPECT_FALSE(policy.viewportMatches({801.0, 600.0}));

    EXPECT_FALSE(policy.setSerialized("version=5;viewport=800x600;regions=0,0,0,10", SURFACE_SIZE).has_value());
    EXPECT_EQ(policy.snapshot()->version, 4u);
    EXPECT_TRUE(policy.containsSurfacePoint({20.0, 20.0}));

    EXPECT_FALSE(policy.setSerialized("version=3;viewport=800x600;regions=0,0,10,10", SURFACE_SIZE).has_value());
    EXPECT_EQ(policy.snapshot()->version, 4u);
}

TEST(InputPolicy, malformedAndOversizedSnapshotsAreRejected) {
    CInputPolicy policy;

    EXPECT_FALSE(policy.setSerialized("version=1;viewport=800x600;regions=0,0,10", SURFACE_SIZE).has_value());
    EXPECT_FALSE(policy.hasPolicy());

    const std::string oversized(CInputPolicy::MAX_SERIALIZED_SIZE + 1, 'x');
    EXPECT_FALSE(policy.setSerialized(oversized, SURFACE_SIZE).has_value());
    EXPECT_FALSE(policy.hasPolicy());
}

TEST(InputPolicy, clearRestoresUnsetBehavior) {
    CInputPolicy policy;
    ASSERT_TRUE(policy.setSerialized("version=1;viewport=800x600;regions=0,0,10,10", SURFACE_SIZE).has_value());
    ASSERT_TRUE(policy.hasPolicy());

    policy.clear();

    EXPECT_FALSE(policy.hasPolicy());
    EXPECT_TRUE(policy.effectiveInputRegion(fullSurfaceRegion(), SURFACE_SIZE).containsPoint({400.0, 300.0}));
}

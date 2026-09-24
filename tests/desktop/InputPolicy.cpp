#include <desktop/InputPolicy.hpp>
#include <desktop/InputPolicyCapture.hpp>

#include <gtest/gtest.h>

using namespace Desktop;

namespace {
    constexpr auto SURFACE_SIZE = Vector2D{800.0, 600.0};
    constexpr auto CUE_WIRE     = "version=1;generation=42;viewport=800x600;regions=10,20,30,40;50,60,70,80";

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
    EXPECT_TRUE(policy.acceptsPoint({400.0, 300.0}, clientRegion, {400.0, 300.0}, SURFACE_SIZE));
}

TEST(InputPolicy, emptyPolicyIsPresentAndBlocksAllInput) {
    CInputPolicy policy;

    const auto   result = policy.setSerialized("version=1;generation=1;viewport=800x600;regions=", SURFACE_SIZE);

    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_TRUE(policy.hasPolicy());
    EXPECT_FALSE(policy.acceptsPoint({400.0, 300.0}, fullSurfaceRegion(), {400.0, 300.0}, SURFACE_SIZE));
}

TEST(InputPolicy, exactCueWireFormatIsParsed) {
    CInputPolicy policy;

    const auto   result = policy.setSerialized(CUE_WIRE, SURFACE_SIZE);

    ASSERT_TRUE(result.has_value()) << result.error();
    ASSERT_TRUE(policy.hasPolicy());
    EXPECT_EQ(policy.generationFloor(), 42u);
    EXPECT_TRUE(policy.containsRootPoint({20.0, 30.0}));
    EXPECT_TRUE(policy.containsRootPoint({60.0, 70.0}));
    EXPECT_FALSE(policy.containsRootPoint({40.0, 30.0}));
}

TEST(InputPolicy, rectanglesFormAUnionAndPreserveGaps) {
    CInputPolicy policy;

    const auto   result = policy.setSerialized("version=1;generation=7;viewport=800x600;regions=0,0,100,100;200,0,100,100", SURFACE_SIZE);

    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_TRUE(policy.containsRootPoint({50.0, 50.0}));
    EXPECT_TRUE(policy.containsRootPoint({250.0, 50.0}));
    EXPECT_FALSE(policy.containsRootPoint({150.0, 50.0}));
}

TEST(InputPolicy, policyIntersectsInfiniteAndPartialClientRegions) {
    CInputPolicy policy;
    ASSERT_TRUE(policy.setSerialized("version=1;generation=3;viewport=800x600;regions=100,100,200,200", SURFACE_SIZE).has_value());

    // An infinite wl_surface input region is materialized as the full surface.
    EXPECT_TRUE(policy.acceptsPoint({150.0, 150.0}, fullSurfaceRegion(), {150.0, 150.0}, SURFACE_SIZE));
    EXPECT_FALSE(policy.acceptsPoint({50.0, 50.0}, fullSurfaceRegion(), {50.0, 50.0}, SURFACE_SIZE));

    EXPECT_TRUE(policy.acceptsPoint({150.0, 150.0}, partialClientRegion(), {150.0, 150.0}, SURFACE_SIZE));
    EXPECT_FALSE(policy.acceptsPoint({350.0, 350.0}, partialClientRegion(), {350.0, 350.0}, SURFACE_SIZE));
    EXPECT_FALSE(policy.acceptsPoint({550.0, 450.0}, partialClientRegion(), {550.0, 450.0}, SURFACE_SIZE));
}

TEST(InputPolicy, acceptsPointKeepsRootAndSurfaceCoordinatesSeparate) {
    CInputPolicy policy;
    ASSERT_TRUE(policy.setSerialized("version=1;generation=4;viewport=800x600;regions=200,200,100,100", SURFACE_SIZE).has_value());

    CRegion surfaceLocalClientRegion;
    surfaceLocalClientRegion.add(CBox{0.0, 0.0, 50.0, 50.0});

    EXPECT_TRUE(policy.acceptsPoint({250.0, 250.0}, surfaceLocalClientRegion, {25.0, 25.0}, {100.0, 100.0}));
    EXPECT_FALSE(policy.acceptsPoint({250.0, 250.0}, surfaceLocalClientRegion, {75.0, 25.0}, {100.0, 100.0}));
    EXPECT_FALSE(policy.acceptsPoint({100.0, 100.0}, surfaceLocalClientRegion, {25.0, 25.0}, {100.0, 100.0}));
}

TEST(InputPolicy, emptyClientRegionRemainsEmptyAfterIntersection) {
    CInputPolicy policy;
    ASSERT_TRUE(policy.setSerialized("version=1;generation=3;viewport=800x600;regions=0,0,800,600", SURFACE_SIZE).has_value());

    const CRegion emptyClientRegion;
    EXPECT_FALSE(policy.acceptsPoint({10.0, 10.0}, emptyClientRegion, {10.0, 10.0}, SURFACE_SIZE));
}

TEST(InputPolicy, invalidReplacementDoesNotReplaceLastValidPolicy) {
    CInputPolicy policy;
    ASSERT_TRUE(policy.setSerialized("version=1;generation=4;viewport=800x600;regions=10,10,100,100", SURFACE_SIZE).has_value());
    ASSERT_TRUE(policy.hasPolicy());

    EXPECT_FALSE(policy.setSerialized("version=1;generation=5;viewport=801x600;regions=0,0,10,10", SURFACE_SIZE).has_value());
    EXPECT_EQ(policy.generationFloor(), 4u);
    EXPECT_TRUE(policy.containsRootPoint({20.0, 20.0}));
    EXPECT_FALSE(policy.containsRootPoint({5.0, 5.0}));
    EXPECT_TRUE(policy.viewportMatches(SURFACE_SIZE));
    EXPECT_FALSE(policy.viewportMatches({801.0, 600.0}));

    EXPECT_FALSE(policy.setSerialized("version=1;generation=5;viewport=800x600;regions=0,0,0,10", SURFACE_SIZE).has_value());
    EXPECT_EQ(policy.generationFloor(), 4u);
    EXPECT_TRUE(policy.containsRootPoint({20.0, 20.0}));

    EXPECT_TRUE(policy.setSerialized("version=1;generation=5;viewport=800x600;regions=0,0,10,10", SURFACE_SIZE).has_value());
    EXPECT_EQ(policy.generationFloor(), 5u);

    EXPECT_FALSE(policy.setSerialized("version=1;generation=3;viewport=800x600;regions=0,0,10,10", SURFACE_SIZE).has_value());
    EXPECT_EQ(policy.generationFloor(), 5u);
}

TEST(InputPolicy, clearKeepsGenerationFloor) {
    CInputPolicy policy;
    ASSERT_TRUE(policy.setSerialized("version=1;generation=10;viewport=800x600;regions=0,0,10,10", SURFACE_SIZE).has_value());

    policy.clear();

    EXPECT_FALSE(policy.hasPolicy());
    EXPECT_EQ(policy.generationFloor(), 10u);
    EXPECT_FALSE(policy.setSerialized("version=1;generation=9;viewport=800x600;regions=0,0,20,20", SURFACE_SIZE).has_value());
    EXPECT_FALSE(policy.setSerialized("version=1;generation=10;viewport=800x600;regions=0,0,20,20", SURFACE_SIZE).has_value());
    EXPECT_TRUE(policy.setSerialized("version=1;generation=11;viewport=800x600;regions=0,0,20,20", SURFACE_SIZE).has_value());
    EXPECT_TRUE(policy.hasPolicy());
}

TEST(InputPolicy, malformedAndOversizedSnapshotsAreRejected) {
    CInputPolicy policy;

    EXPECT_FALSE(policy.setSerialized("version=1;generation=1;viewport=800x600;regions=0,0,10", SURFACE_SIZE).has_value());
    EXPECT_FALSE(policy.setSerialized("version=2;generation=1;viewport=800x600;regions=0,0,10,10", SURFACE_SIZE).has_value());
    EXPECT_FALSE(policy.setSerialized("version=1;generation=0;viewport=800x600;regions=0,0,10,10", SURFACE_SIZE).has_value());
    EXPECT_FALSE(policy.hasPolicy());

    const std::string oversized(CInputPolicy::MAX_SERIALIZED_SIZE + 1, 'x');
    EXPECT_FALSE(policy.setSerialized(oversized, SURFACE_SIZE).has_value());
    EXPECT_FALSE(policy.hasPolicy());
}

TEST(InputPolicy, policyCaptureDecisionOnlyCancelsSameWindowCapture) {
    const auto SAME_OWNER  = decideInputPolicyCapture(true, true, false);
    const auto OTHER_OWNER = decideInputPolicyCapture(false, true, false);
    const auto NO_BUTTONS  = decideInputPolicyCapture(false, false, false);
    const auto DND_ACTIVE  = decideInputPolicyCapture(true, true, true);

    EXPECT_TRUE(SAME_OWNER.cancelHeldButtons);
    EXPECT_TRUE(SAME_OWNER.forcePolicyRefocus);
    EXPECT_TRUE(SAME_OWNER.pointerOnly);
    EXPECT_FALSE(OTHER_OWNER.cancelHeldButtons);
    EXPECT_FALSE(OTHER_OWNER.forcePolicyRefocus);
    EXPECT_TRUE(OTHER_OWNER.pointerOnly);
    EXPECT_FALSE(NO_BUTTONS.cancelHeldButtons);
    EXPECT_TRUE(NO_BUTTONS.forcePolicyRefocus);
    EXPECT_TRUE(NO_BUTTONS.pointerOnly);
    EXPECT_FALSE(DND_ACTIVE.cancelHeldButtons);
    EXPECT_FALSE(DND_ACTIVE.forcePolicyRefocus);
    EXPECT_TRUE(DND_ACTIVE.pointerOnly);
}

TEST(InputPolicy, clearRestoresUnsetBehavior) {
    CInputPolicy policy;
    ASSERT_TRUE(policy.setSerialized("version=1;generation=1;viewport=800x600;regions=0,0,10,10", SURFACE_SIZE).has_value());
    ASSERT_TRUE(policy.hasPolicy());

    policy.clear();

    EXPECT_FALSE(policy.hasPolicy());
    EXPECT_TRUE(policy.acceptsPoint({400.0, 300.0}, fullSurfaceRegion(), {400.0, 300.0}, SURFACE_SIZE));
}

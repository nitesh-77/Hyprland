#include <render/Renderer.hpp>

#include <gtest/gtest.h>

using namespace Render;

// Renderer.hpp declares Render::shouldExcludeFromCapture(bool captureExclusionPass, bool
// noScreenShare) inline, right above IHyprRenderer - the pure decision logic extracted
// from IHyprRenderer::shouldRenderWindow(PHLWINDOW, PHLMONITOR) and
// IHyprRenderer::renderLayer(). See that declaration's comment for the full contract.

TEST(Renderer, shouldExcludeFromCapture_flagFalse_neverExcludes) {
    // flag=false must never exclude, regardless of noScreenShare - today's behavior
    // (before this feature sets the flag anywhere) must be completely unchanged.
    EXPECT_FALSE(shouldExcludeFromCapture(false, false));
    EXPECT_FALSE(shouldExcludeFromCapture(false, true));
}

TEST(Renderer, shouldExcludeFromCapture_flagTrue_noScreenShareTrue_excludes) {
    EXPECT_TRUE(shouldExcludeFromCapture(true, true));
}

TEST(Renderer, shouldExcludeFromCapture_flagTrue_noScreenShareFalse_doesNotExclude) {
    // flag=true must not affect surfaces that aren't flagged noScreenShare.
    EXPECT_FALSE(shouldExcludeFromCapture(true, false));
}

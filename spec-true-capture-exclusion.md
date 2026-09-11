# Spec: True capture-exclusion for `noScreenShare` windows/layers (monitor/region capture)

## Problem Statement

Today, a Hyprland user who flags a window or layer with `no_screen_share` (the existing
`noscreenshare` window/layer-rule effect) does not get that surface hidden from a
screen-share or recording. Hyprland still captures the surface's real pixels — they're
baked into the same texture used for physical monitor mirroring — and then draws an
opaque black rectangle on top of that surface's on-screen box in the outgoing capture
buffer. This has three consequences a user actually experiences:

1. **It's visibly a black box, not an absence.** Anyone watching the share can see that
   *something* is being hidden, even if they can't see what. For a use case that depends
   on the flagged surface being genuinely undetectable (not just unreadable), this
   defeats the purpose.
2. **It can desync from the window it's supposed to cover.** `hyprwm/Hyprland#13150`
   (GitHub Discussion, filed 2026-01-30, open/unanswered) documents the black rectangle's
   position drifting from the actual floating window during browser/site zoom inside a
   Discord screen-share, briefly revealing the real content underneath.
3. **It looks wrong over transparency and workspace gaps.** `hyprwm/Hyprland#11610`
   (GitHub Discussion) is a maintainer-acknowledged, deliberately-unfixed limitation: the
   black box is opaque and occlusion-unaware, so it paints over transparent regions and
   gaps on special workspaces where the real background should show. Maintainer vaxerski
   confirmed the reason it isn't fixed today: *"you dont want to re-render the entire
   display for screensharing."*

There is no config-only fix for any of this. It requires new compositor-side rendering
behavior.

## Solution

When a window or layer is flagged `no_screen_share` and a monitor/region capture request
comes in (the path used by screen-share in meeting apps and browsers — `SHARE_MONITOR`/
`SHARE_REGION`), Hyprland performs a second, independent full composite of that monitor's
active workspace for the capture buffer only, skipping any surface flagged
`no_screen_share` during that composite. Whatever would be visually behind the flagged
surface — another window, the wallpaper, a gap — shows through naturally, because the
skipped surface was never drawn, not because a box was placed over it afterward. The
user's real, on-screen display is completely unaffected; only the capture buffer differs.

This directly resolves all three problems above: there is no rectangle to be visibly
present (1), no rectangle to desync (2), and no occlusion-unaware overpaint over
transparency or gaps (3) — because nothing is painted over anything; the flagged surface
is simply absent from that one render.

Single-window capture (`SHARE_WINDOW`, `hyprland-toplevel-export-v1`) is unaffected by
this change and keeps its current, different behavior (a permission-denied placeholder
texture, substituted in `CScreenshareFrame::render()` when the captured window itself is
`no_screen_share`-flagged) — see Out of Scope.

## User Stories

1. As a Hyprland user running a screen-share (Discord, Zoom, Google Meet, OBS via
   PipeWire, or any `wlr-screencopy`/`ext-image-copy-capture-v1`/portal-based consumer),
   I want a window I've flagged `no_screen_share` to be completely absent from the shared
   picture, so that viewers cannot tell it exists at all, not just that its content is
   hidden.
2. As a Hyprland user, I want whatever is visually behind a `no_screen_share` window
   (another window, the wallpaper) to show through in the capture exactly as if the
   flagged window weren't there, so that the share looks like a normal desktop rather
   than a desktop with a black hole in it.
3. As a Hyprland user, I want a `no_screen_share`-flagged layer-shell surface (e.g. a
   bar module or overlay) to be excluded the same way a window is, so that layer-shell
   clients get the same guarantee windows do.
4. As a Hyprland user with no `no_screen_share`-flagged surfaces on a captured monitor, I
   want screen-sharing to perform exactly as it does today (same cost, same code path),
   so that this feature costs me nothing when I'm not using it.
5. As a Hyprland user sharing a special workspace that contains a `no_screen_share`
   window alongside transparency or gaps, I want the real background to show through
   those gaps in the capture, not a black rectangle, so that the known cosmetic issue in
   `hyprwm/Hyprland#11610` does not apply to the new exclusion path.
6. As a Hyprland user, I want my mouse cursor to still be visible in the capture when a
   `no_screen_share` surface is present and excluded, so that the share doesn't silently
   lose cursor visibility as a side effect of this feature.
7. As a Hyprland user sharing a workspace where a `no_screen_share` window is
   fullscreened, I want the exclusion to still apply correctly, so that fullscreen and
   tiled/floating windows are treated consistently.
8. As a Hyprland user, I want this feature to require no config-syntax changes beyond the
   `no_screen_share` flag that already exists today, so that I don't have to learn a new
   window-rule vocabulary to get true exclusion instead of the black box.
9. As a Hyprland user capturing via `ext-image-copy-capture-v1` specifically (not just
   `wlr-screencopy`), I want the same true-exclusion guarantee, so that the protocol I
   happen to be using doesn't determine whether I'm actually protected.
10. As a Hyprland packager/distributor, I want a way to fully disable this new render
    path at compositor startup (independent of any window rule), so that if it crashes or
    corrupts output on some hardware/driver combination, there's a documented recovery
    path that doesn't require reverting a commit and rebuilding.
11. As a Hyprland developer reviewing this change later, I want the new render path's
    state handling (transform/damage-simplification flags) to be explicitly saved and
    restored around its own call, so that it can't leak state into the unrelated code
    that runs immediately afterward in the same function.
12. As a Hyprland developer, I want the two currently-open GitHub Discussions this change
    is directly relevant to (`#13150`, `#11610`) referenced from the new code, so that a
    future reader understands why the new path exists and what failure mode it avoids by
    construction, without having to rediscover the history.
13. As `cue-linux` (the first real consumer of this feature, a separate project), I want
    to flag my own overlay window as `no_screen_share` using the exact same, already-
    documented Lua syntax, so that adopting this feature requires no coordination with
    the Hyprland side beyond the flag itself.
14. As a Hyprland user on a locked session (session lock active, `misc:session_lock_xray`
    unset/disabled), I want screen-sharing to behave the same as it does today (workspace
    rendering already short-circuits under session lock), so that this feature doesn't
    change locked-session capture behavior as a side effect.
15. As a Hyprland user with `render:xp_mode` enabled, I want the capture-exclusion render
    to automatically inherit the same background/bottom-layer skip `xp_mode` already
    applies to the normal render — for free, by virtue of both going through the same
    `renderAllClientsForWorkspace()` function, with no new `xp_mode`-specific code
    written for this feature — so that this feature doesn't silently contradict an
    existing render-mode config option.

## Implementation Decisions

**Scope of this patch.** Applies only to the `SHARE_MONITOR`/`SHARE_REGION` capture path
— `CScreenshareFrame::renderMonitor()`. `SHARE_WINDOW` (single-window capture) is
explicitly out of scope; see Out of Scope. `ext-image-copy-capture-v1`
(`src/protocols/ImageCopyCapture.cpp`) requires no separate change: it has no rendering
logic of its own and delegates entirely to the same `Screenshare::mgr()` /
`CScreenshareFrame::share()` entry point `renderMonitor()` already uses, so it inherits
this behavior automatically once `renderMonitor()` is changed.

**New state.** Add one new boolean, `m_bCaptureExclusionPass`, on `IHyprRenderer`
(`src/render/Renderer.hpp`/`.cpp`), following the existing naming convention for
render-mode booleans on that class (e.g. `m_bRenderingSnapshot`, `m_bBlockSurfaceFeedback`
already exist there as precedent for this kind of transient render-state flag). Default
`false`. Set `true` immediately before, and reset `false` immediately after, the new
capture-exclusion composite call inside `renderMonitor()` — on every exit path, not just
the success path.

**Gating check — windows.** `IHyprRenderer::shouldRenderWindow(PHLWINDOW, PHLMONITOR)` —
the two-argument overload — gains one new check near its top: if
`m_bCaptureExclusionPass` is true and the window's rule applicator reports
`noScreenShare().valueOrDefault()` true, return `false`. This is the correct overload to
change: it is the one already used by both `renderWorkspaceWindows()` and
`renderWorkspaceWindowsFullscreen()` (confirmed both call this exact two-arg overload,
not the single-argument `shouldRenderWindow(PHLWINDOW)` overload, which is a distinct
function with its own special-workspace logic, used by unrelated call sites and by
single-window capture's `renderWindow()` path — out of scope for this patch). Because
`renderAllClientsForWorkspace()` dispatches to whichever of those two functions applies
based on `Fullscreen::controller()->hasFullscreen(pWorkspace)`, gating this one overload
covers both the tiled/floating and fullscreen cases without further changes.

**Gating check — layers.** `IHyprRenderer::renderLayer()` gains the equivalent check
immediately after its existing `if (!pLayer->visible()) return;` guard: if
`m_bCaptureExclusionPass` is true and the layer's rule applicator reports
`noScreenShare().valueOrDefault()` true, return. This is a genuinely separate gate from
the window check above — layers are not filtered through `shouldRenderWindow()` at all —
so this step is mandatory and cannot be folded into the window-gating change.

**Render-body replacement.** `CScreenshareFrame::renderMonitor()`
(`src/managers/screenshare/ScreenshareFrame.cpp`) currently: grabs the monitor's mirror
texture (`getMirrorTexture()`), draws it into the capture output, then loops layers and
windows with `noScreenShare()` set, drawing an opaque black `CRectPassElement` over each
one's on-screen box (including their popups, via a `hidePopups` breadth-first walk).
Replace this with: set `m_bCaptureExclusionPass = true`, call `renderWorkspace(PMONITOR,
PMONITOR->m_activeWorkspace, ...)` for the monitor's active workspace (this is the same
single call the normal per-frame render path already makes; it dispatches internally to
`renderAllClientsForWorkspace()`, which itself calls `renderBackground()`, iterates and
renders each shell layer via `renderLayer()`, and dispatches to
`renderWorkspaceWindowsFullscreen()` or `renderWorkspaceWindows()` depending on whether
the workspace has a fullscreen window — both now gated per the checks above), then reset
`m_bCaptureExclusionPass = false`. Remove the black-box drawing loop and the `hidePopups`
helper entirely for this path — a window/layer skipped by the gating checks above is
never drawn at all, and its popups (drawn from the same already-filtered parent object,
not gated separately) are automatically skipped too.

**Special workspaces need no separate handling.** The two-arg `shouldRenderWindow()`
already includes special-workspace windows via its existing
`pMonitor->m_activeSpecialWorkspace == pWindow->m_workspace` check (unchanged by this
patch); the new gating check sits alongside it in the same function, so special-workspace
content is correctly included or excluded by the same call, with no second render pass.

**Session-lock and `xp_mode` interaction (verified this session, not previously
documented).** `renderAllClientsForWorkspace()` — the function the new
`renderWorkspace()` call reaches — has its own early-return guard when the session is
locked (`g_pSessionLockManager->isSessionLocked()` and `misc:session_lock_xray` is not
enabled) and a separate branch controlled by `render:xp_mode` that skips rendering the
background and bottom shell-layer entirely. Both guards apply unchanged to the new
capture-exclusion call, since it goes through the exact same function. This patch does
not need to special-case either — they are pre-existing behaviors of the function being
reused, not something this patch introduces or must work around — but a developer
implementing this should not be surprised that a locked session or `xp_mode`-enabled
config changes what the capture-exclusion render produces, in exactly the same way it
already changes what the normal on-screen render produces.

**State save/restore.** The new `renderWorkspace()` call happens *inside*
`renderMonitor()`, nested within an existing render (not a fresh `beginRender()` cycle).
`renderMonitor()` already sets `g_pHyprRenderer->m_renderData.transformDamage = false`
and `m_renderData.noSimplify = true` before its own draw calls, without restoring them in
that function today — tolerable there only because the *next* `beginRender`/
`beginFullFakeRender` cycle resets them fresh before they're read again. The new call has
no such "next cycle" before the (now much smaller, cursor-only) remainder of
`renderMonitor()` runs in the same function, so both fields must be explicitly saved
before, and restored immediately after, the new `renderWorkspace()` call — following the
existing save/restore pattern already used elsewhere in the same function for
`m_renderData.renderModif.enabled` (`const auto OLD = ...; set new value; draw; restore
OLD;`).

**Cursor.** The mirror-texture path being replaced draws the cursor via
`m_overlayCursor` → `Pointer::mgr()->renderSoftwareCursorsFor(...)`, called from
`renderMonitor()` itself, after the (now-removed) black-box loop. This call continues to
run after the new `renderWorkspace()` call, in the same position relative to the rest of
the function. **Empirical verification (required by this section, not assumed from
reading code alone) found that this call did need one change**: the true-exclusion branch
has no pre-existing mirror texture with the cursor already baked in (unlike the
mirror-texture branch, which relies on the normal per-frame render having drawn it there),
so `renderSoftwareCursorsFor()`'s own screencopy-vs-software-cursor guard silently dropped
the cursor on the new path unless `forceRender=true` is passed — exactly the existing
precedent already used by `renderWindow()`'s equivalent call. The fix passes
`forceRender` only when the true-exclusion branch actually ran, so the mirror-texture
branch's behavior (and its reliance on the guard to avoid double-drawing) is unchanged.
This gap was caught by hyprtester (`captureExclusionCursorVisibleOnExclusionPath`), not
by reading the code — confirming this section's own instruction to verify empirically.

**Early-exit optimization.** Before performing the capture-exclusion render,
`renderMonitor()` checks whether the monitor has any `no_screen_share`-flagged window or
layer at all. If none exist, skip the new render path entirely and keep the existing
cheap mirror-texture draw (with no black-box loop needed either, since nothing is
flagged) — this keeps the cost of this feature at zero for any capture where no surface
is actually flagged, rather than always paying for a second full composite.

**Kill-switch.** One environment variable, `HYPRLAND_DISABLE_CAPTURE_EXCLUSION`, read via
`getenv()` exactly once at compositor startup and cached as a bool — never read per-frame
or per-capture. If set (non-empty/`1`), `renderMonitor()` behaves exactly as it does
today (mirror texture + black-box loop), regardless of `no_screen_share` or the new
gating logic — i.e. this patch's new render path is entirely skipped, as if it didn't
exist. This is a startup-only safety valve: it recovers a broken/crashing build by
restarting Hyprland with the variable set; it does not help mid-session, since there is
no live/runtime toggle in this patch. Full reasoning — including the explicit split
between "logically-wrong output" (already contained by the capture-exclusion render
targeting its own capture buffer, never the real on-screen framebuffer) and
"memory-safety bugs" (not contained by that buffer separation at all, and the actual risk
this switch exists to mitigate) — is recorded in `docs/adr/0001-capture-exclusion-kill-switch.md`
and must be read before modifying or removing this switch.

**Source documentation.** Once this patch lands, add a comment near the new gating logic
and/or the removed black-box code referencing `hyprwm/Hyprland#13150` (the black-box
desync failure mode) and `hyprwm/Hyprland#11610` (the black-box occlusion/transparency
limitation), noting that both are avoided by construction because nothing is drawn to
desync or overpaint. Do not comment on either public Discussion thread as part of this
patch — only once a working fix is actually merged and can be pointed to.

**No config-syntax changes.** The existing `no_screen_share` Lua window-rule/layer-rule
effect (`hl.window_rule({ match = {...}, no_screen_share = true, ... })` /
`hl.layer_rule({ ..., no_screen_share = true })`) is unchanged by this patch. No new
config keys, no new rule effects.

## Testing Decisions

Good tests here assert observable behavior — what gets rendered/skipped, not how the
internal call chain is structured — and avoid re-testing logic this patch doesn't touch
(e.g. the existing special-workspace inclusion check, `shouldRenderMonitor`, or anything
in the mirror-texture/on-screen render path unrelated to capture).

**Seam 1 — unit test, `IHyprRenderer::shouldRenderWindow(PHLWINDOW, PHLMONITOR)` and
`IHyprRenderer::renderLayer()`'s new gating checks.** These are the highest-leverage seam
because the two-arg `shouldRenderWindow` overload takes plain parameters and has no
hidden setup requirement beyond a constructed window/monitor pair — confirmed by reading
its body, it reads only `pWindow`/`pMonitor` state and static config values, nothing
requiring a live render pass. Test: with `m_bCaptureExclusionPass` false, a
`no_screen_share`-flagged window/layer's render-eligibility is unchanged from today's
behavior (regression guard). With it true, a flagged window/layer is excluded regardless
of its other visibility state, and an unflagged window/layer's eligibility is unchanged.
Prior art: `tests/desktop/rule/` already tests window/layer-rule-effect matching and
application in this style (pure-function assertions against rule-applicator state, no
running compositor). This new test belongs in a comparable location — a new
`tests/render/` directory, or alongside `tests/desktop/rule/` since it is fundamentally a
rule-effect interaction — following that directory's existing structure rather than
introducing a new test-organization convention.

**Seam 2 — integration test, a new `hyprtester` capture client.** No existing
`hyprtester` client drives any capture protocol today (`wlr-screencopy`,
`ext-image-copy-capture-v1`, or `hyprland-toplevel-export-v1`) — confirmed by inspecting
`hyprtester/clients/`, which covers pointer/keyboard/surface-scale/shortcut-inhibitor
scenarios but nothing capture-related. A new client is required to test the actual
rendered-pixel behavior end to end, since that cannot be verified by a pure unit test.
The new client should: open a monitor/region capture session against a test scene with
one `no_screen_share`-flagged window layered over a second, differently-colored plain
window; capture a frame; assert the captured pixels at the flagged window's box equal the
second window's color (proving "true exclusion," not black, not the flagged window's
real content). Running the same test with `HYPRLAND_DISABLE_CAPTURE_EXCLUSION` set should
assert the captured pixels are the black-box color instead — this doubles as the
regression check that the kill-switch and the pre-existing black-box behavior both still
work. This single client, run in both configurations, is the minimum needed to cover the
render-body rewrite, the early-exit optimization (run once with zero flagged surfaces and
assert the mirror-texture path — same output as today — is taken), and the cursor
question (assert cursor pixels are present in the captured frame when
`overlay_cursor`/`m_overlayCursor` is requested).

**Scope note for ticket-splitting.** Building this client is a nontrivial, self-contained
piece of work — a real Wayland capture-protocol client, a test scene, and pixel-box
assertions — comparable in size and skill to the compositor render-body change itself,
not a small addition to it. It should be scoped as its own ticket when this spec is
broken down (e.g. via `to-tickets`), not folded into "implement the render-body
replacement." It can be built and reviewed independently of that change: written first
against today's existing black-box behavior (asserting black-box pixels), then reused
unmodified to assert true-exclusion pixels once the render-body change lands. The two
tickets share a test fixture (the client) but are otherwise independent workstreams with
different skills and different failure modes — protocol/test-client code vs. compositor
rendering code.

**Sequencing of verification.** Per the settled implementation order: after the new flag
and the two gating checks are added (no render-body change yet), the flag is always
false, so no test can yet observe different behavior — this stage is verified by a
successful compile and the existing test suite passing unchanged (a true no-op check, not
a behavioral one). Seam 2's `hyprtester` client is only meaningful, and should only be
written to run, once the render-body replacement (`renderMonitor()`'s rewrite) has
landed.

## Out of Scope

- **`SHARE_WINDOW` (single-window capture, `hyprland-toplevel-export-v1`'s underlying
  path, `CScreenshareFrame::renderWindow()`).** Keeps its current, different behavior — a
  permission-denied placeholder texture substituted in `CScreenshareFrame::render()` when
  the captured window itself is `no_screen_share`-flagged. This already fully hides the
  window (a single-window capture has no "behind" to reveal, so black-box vs.
  true-exclusion is not a meaningful distinction there), and is not touched by this spec.
- **Performance budgeting / automatic fallback.** No refresh-rate or window-count-based
  bail-out to the black-box path is designed into this patch, beyond the early-exit
  optimization for the zero-flagged-surface case. Profiling on real hardware, and any
  resulting fallback logic, is explicitly deferred to a follow-up once this feature has
  real operating history (e.g. via the `cue-linux` project actually using it).
- **A mid-session/live kill-switch.** The kill-switch in this spec is startup-only. A
  `hyprctl` debug keyword or a live-reloaded config toggle is a deliberately deferred
  follow-up — see `docs/adr/0001-capture-exclusion-kill-switch.md`'s "Open follow-ups."
- **XWayland-specific behavior and GPU/driver (NVIDIA) variance.** Tracked as known-risk
  follow-up items, not addressed by this patch — neither is meaningfully testable without
  a real client exercising the path (XWayland) or real hardware (GPU/driver variance).
- **Commenting on the public GitHub Discussions** `#13150`/`#11610`. Deferred until a
  working fix exists to point to from those threads.
- **Any change to the app/consumer side.** This spec is compositor-only. The consumer
  project (`cue-linux`) exposing a stable window class and applying the existing
  `no_screen_share` Lua rule to it is that project's own, separately-tracked work — not
  part of this spec.

## Further Notes

- This feature exists because of a specific, named consumer need — a Linux/Wayland
  stealth AI meeting-copilot overlay (`cue-linux`, forked from `Blueturboguy07/cue`) that
  needs true invisibility to screen-share, the way `NSWindowSharingNone`/
  `WDA_EXCLUDEFROMCAPTURE` provide on macOS/Windows. It is not a speculative, generic
  compositor improvement; design choices in this spec (especially the `renderMonitor()`
  -only scope) were made against that real use case. See `CONTEXT.md` at this repo's root
  for the full glossary and the cross-reference to that project.
- The original architecture research — full trace of today's black-box mechanism, the
  mirror-texture call chain, and why a post-processing fix cannot work — lives in
  `hyprland-capture-exclusion-investigation.md` at this repo's root. This spec's
  Implementation Decisions section restates only what's necessary to build from; that
  document has the full "why," including the two corrected wrong assumptions made earlier
  in that investigation (worth reading once, so they aren't reintroduced).
- All source-level claims in this spec were re-verified directly against this repo's
  checked-out `v0.56.2` source (tag `v0.56.2`, commit
  `efb50993780079460b0cbed1363e2166a2de1d9f`) at spec-writing time, not assumed from prior
  summaries — including confirming which of `shouldRenderWindow()`'s two overloads is
  actually on the relevant call path, and the session-lock/`xp_mode` interactions inside
  `renderAllClientsForWorkspace()`, neither of which had been previously documented.

# Context: `true-capture-exclusion` (Hyprland fork)

Glossary for the capture-exclusion feature being built on branch `true-capture-exclusion` (based on `v0.56.2`). This file is a glossary only — no implementation details, no task lists.

## The sibling project — why this feature exists

This fork is one of **two related, deliberately decoupled projects** working toward one
overall goal (a stealth AI copilot invisible to screen-share on Hyprland):

- **This repo** — the compositor feature itself: true capture exclusion.
- **`nitesh-77/cue-linux`**, local path `~/swoord's pc old/workspace/personal projects/cue-linux`
  (fork of `Blueturboguy07/cue`) — the actual consumer app. A Linux-only, single-machine
  AI interview/meeting copilot (capture → STT → prompt → LLM → overlay) being ported to
  run natively on Hyprland/Wayland. See that repo's `CONTEXT.md` for its own glossary.

**Why this matters for anyone working on this fork:** this feature is not a generic
compositor improvement speculatively benefiting all Hyprland users — it exists because a
specific consumer app needs it to be truly invisible to screen-share/recording, the way
`NSWindowSharingNone`/`WDA_EXCLUDEFROMCAPTURE` make windows invisible on macOS/Windows.
Design choices here (e.g. scoping the first patch to `renderMonitor()` only, per
`SHARE_MONITOR`/`SHARE_REGION` being "the actual meeting-screen-share case") are made
against that real consumer's needs, not in the abstract. If a design question comes up
that seems to need more information about *how* the feature will actually be used
(capture frequency, whether multiple windows get flagged at once, whether the overlay
itself ever needs `no_screen_share`, etc.), the answer likely lives in — or should be
asked against — the cue-linux side, not guessed at here in isolation.

**The only real coupling between the two projects, by design:** this fork only reads the
existing `no_screen_share` window/layer-rule flag (already shipped, Lua config, see the
scope-decisions section below for exact syntax) — it has no awareness of cue-linux's
internals. cue-linux's only obligation is to expose a stable window class/app-id
(`omarchy-copilot`) that a user-written Lua window rule can target. Zero code-level
dependency in either direction.

## Terms

**Black-box exclusion** — Hyprland's existing, shipped `noscreenshare` behavior. A window/layer flagged `noScreenShare` is still captured (its real pixels are baked into the post-hoc mirror texture), and an opaque black `CRectPassElement` is drawn on top of its whole on-screen box in the capture buffer, with no occlusion/transparency awareness. The window's real content is hidden, but its *absence* is visible as a black rectangle — capture tools can tell something is being hidden, the box can desync from the window (see `hyprwm/Hyprland#13150` discussion), and it looks wrong over transparency/gaps on special workspaces (see `hyprwm/Hyprland#11610` discussion — maintainer vaxerski confirmed this is a known, accepted limitation, not a bug: *"you dont want to re-render the entire display for screensharing"* is the stated reason it was never fixed properly). This branch's whole premise is paying that re-render cost deliberately.

**True exclusion** — the feature this branch adds. A window/layer flagged `noScreenShare` is never composited into the capture buffer in the first place. Whatever would be visually behind it (another window, the wallpaper) shows through instead, exactly as if the flagged surface didn't exist for that one capture render. Requires an independent second full composite of the scene, not a post-process step on the existing composited frame.

**Capture-exclusion render** (a.k.a. `m_bCaptureExclusionPass`, the `IHyprRenderer` boolean flag implementing it) — the new render pass this branch introduces: a full re-composite of a monitor's active workspace (via `renderWorkspace()`), gated by `m_bCaptureExclusionPass`, that skips any window/layer with `noScreenShare` set at the `shouldRenderWindow()` / `renderLayer()` gates. Distinct from the ordinary per-frame render and from the existing black-box draw.

## Scope decisions affecting terminology

- The first patch scopes **true exclusion** to the `SHARE_MONITOR`/`SHARE_REGION` capture path (`renderMonitor()`) only — i.e. full-screen/region capture, the actual meeting-screen-share case. `SHARE_WINDOW` (single-window capture, `renderWindow()`) keeps its current behavior (permission-denied placeholder texture) for now. Confirmed fact: `ext-image-copy-capture-v1` (`ImageCopyCapture.cpp`) has zero independent rendering logic of its own and delegates entirely to `Screenshare::mgr()` / `CScreenshareFrame::share()` — the same entry point `renderMonitor()` uses — so it gets true exclusion automatically once `renderMonitor()` is patched, with no separate change needed there.
- `m_bCaptureExclusionPass` (confirmed name, `IHyprRenderer` boolean) is a compile-only no-op flag through implementation steps 1-3 (added, but nothing sets it yet); it starts changing actual behavior only once step 4 sets it inside `renderMonitor()`. The regression checkpoint after steps 1-3 is therefore compile + existing test suite, not a behavioral smoke test — the real black-box-vs-true-exclusion manual test (OBS/wf-recorder) happens only after steps 4-5 land.
- `hyprwm/Hyprland#13150` (discussion, not issue — the geometry-desync-during-zoom failure mode of black-box exclusion) gets a source comment linking it once the patch lands, documenting that true exclusion avoids this failure mode by construction (nothing is drawn to desync). Deliberately not commenting on the public discussion itself until there's a working fix to point to.
- The capture-exclusion render's call into `renderWorkspace()` (from inside `renderMonitor()`) must save/restore `m_renderData.transformDamage` and `m_renderData.noSimplify` around just that call, even though `renderMonitor()`'s own surrounding code doesn't bother restoring these fields today — because the capture-exclusion render is nested inside an existing render (not a fresh `beginRender` cycle), skipping restoration would leak state into the black-box code that runs immediately after in the same function.
- Performance of the capture-exclusion render is deliberately left unbenchmarked and un-budgeted through the first patch (no bail-out/fallback-to-black-box logic based on refresh rate or window count). Get it correct first; profile on real hardware once cue-linux is running end-to-end; decide then whether a fallback is needed.
- XWayland behavior and GPU/driver (NVIDIA) variance are tracked as known-risk follow-up items, not blockers on the first patch — neither can be meaningfully tested without a real client (cue-linux, once Wayland-native) or real hardware respectively.
- **Early-exit optimization**: `renderMonitor()` must check whether the captured monitor has zero `noScreenShare` windows/layers *before* running the capture-exclusion render, and if so, skip it entirely and keep the existing cheap mirror-texture path (no black-box loop needed either, since there's nothing flagged). This is a single `if`, directly answers Q9's perf concern by making the expensive path pay-per-use rather than always-on, and is a correctness-adjacent decision (not deferred like Q9's broader profiling/fallback question) because it's cheap to build now and clearly correct regardless of what profiling later shows.
- **Cursor inclusion, open verification item for steps 4-5**: the existing mirror-texture path includes the software/hardware cursor (via `m_overlayCursor` handling in `renderMonitor()`, calling `Pointer::mgr()->renderSoftwareCursorsFor(...)`). A bare `renderWorkspace()` call renders workspace content only and may not draw the cursor by default. Steps 4-5 must explicitly verify the capture-exclusion render still produces a visible cursor in the shared output — if not, the existing cursor-draw call needs to run again (or still run) against the new composite, not just the old mirror texture.
- **Kill-switch**: `HYPRLAND_DISABLE_CAPTURE_EXCLUSION=1`, read once via `getenv()` at compositor startup (never per-frame), forces `renderMonitor()` to keep the existing black-box path regardless of `m_bCaptureExclusionPass`/`noScreenShare`. A startup-only safety valve for a broken/crashing capture-exclusion render — recovers by restarting Hyprland with the var set, does **not** help mid-session. See `docs/adr/0001-capture-exclusion-kill-switch.md` for the full reasoning and explicitly what it does/doesn't cover.
- **Lua windowrule syntax for `noscreenshare`, resolved** (v0.55+ moved config from hyprlang to Lua; the old `windowrulev2 = noscreenshare, class:...` form is invalid in v0.56.2). Canonical form, confirmed against `LuaBindingsInternal.hpp`/`.cpp`, `WindowRuleEffectContainer.cpp`, and `example/hyprland.lua`:
  ```lua
  hl.window_rule({
      name  = "hide-from-screenshare",
      match = { class = "^(your-class-here)$" },
      no_screen_share = true,  -- NOT "noscreenshare" — underscored key, hyprlang alias doesn't exist in Lua
      float           = true,
      pin             = true,
  })
  ```
  A layer-shell equivalent exists as `hl.layer_rule({ ..., no_screen_share = true })` for bars/overlays. This is the rule the `omarchy-copilot` window class (from the cue-linux side) will be matched against once the capture-exclusion patch lands.

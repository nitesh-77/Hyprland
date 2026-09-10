# Investigation: True capture-exclusion for an AI overlay on Hyprland v0.56.2

**Status:** Architecture traced and verified against real source (this doc, §1-10). Every
open item from §9 has since been resolved into a settled decision via a full `grilling`
session — see **§11 (Update — decisions round, 2026-09-10)** below. Design and decisions
are complete. **No code written yet.** Full glossary, ADR, and cross-repo context now
live in `CONTEXT.md` and `docs/adr/0001-capture-exclusion-kill-switch.md` at this repo's
root — read those for the canonical terms and reasoning; §11 here is a pointer/summary,
not a duplicate.
**Repo verified:** `github.com/hyprwm/Hyprland`, tag `v0.56.2`, commit `efb50993780079460b0cbed1363e2166a2de1d9f` (2026-08-05).
**Environment:** Omarchy (Arch-based), Hyprland compositor.
**Original goal:** Run an AI interview/meeting-copilot overlay (like OpenCluely or Cue) that is invisible to screen-share/recording tools, on Linux/Wayland, specifically Hyprland — not macOS/Windows, where this is solved with OS-level flags (`NSWindowSharingNone`, `WDA_EXCLUDEFROMCAPTURE`).

If you are picking this up fresh: **do not re-derive facts already verified below by searching the web or guessing from older blog posts/docs.** Version drift is real and already bit us once mid-investigation (see "Corrections" section). Verify against the actual `v0.56.2` tag, cloned locally, which is the authoritative source. Clone command:
```
git clone --depth 1 --branch v0.56.2 https://github.com/hyprwm/Hyprland.git
```
`raw.githubusercontent.com`, `github.com`, and `codeload.github.com` are reachable from the sandbox — no need to rely on web_search snippets for source code once you can clone directly.

---

## 1. The app side — decided, audited, and in progress (not the current focus of this doc)

**Superseded note (2026-09-10):** this section originally posed OpenCluely vs. cue as an
open, deferred choice. That choice is made. **`cue` was picked, forked as
`nitesh-77/cue-linux`, fully source-audited, and its Linux-port plan is grilled to a
settled task order.** Nothing below in this section is still open — it's kept as
historical framing for why cue was chosen, with a pointer to where the actual current
state lives.

- **Decision:** fork `Blueturboguy07/cue` → `nitesh-77/cue-linux`, local path
  `~/swoord's pc old/workspace/personal projects/cue-linux`. OpenCluely was not chosen.
- **Why:** a full source audit (`cue-linux/grllling/cue-codebase-audit-hyprland.md`,
  2026-09-09, every file read, claims cited by file/line) verdict: *"Fork and build on
  it. This isn't close."* The mic+system-audio→STT→prompt→LLM→overlay pipeline — the
  genuinely hard ~70% of this class of app — is done, dual-channel, decoupled,
  watchdogged, and has 22 test files. What's missing for Linux (system-audio capture,
  a working Wayland screenshot path, wiring up an already-written-but-unwired memory
  layer) are additive/surgical fixes at clean seams, not rewrites. A build-from-scratch
  alternative was estimated at 50-80h to reach cue's current pipeline quality; forking
  cue is ~15-25h to a fully working port.
- **Confirmed relevant to this doc specifically:** the audit's own closing "carry-forward
  open item" states the XWayland-vs-native-Wayland question **interacts directly with
  this compositor patch** — the patch only excludes flagged surfaces from
  *compositor-side* capture renders, so cue must capture through Hyprland's own
  screencopy/portal path (native Wayland + portal, or `grim`) for exclusion to hold at
  all. An XWayland X11 grab bypasses this patch entirely. This is why §9 item 5
  (XWayland) is tracked as a real risk on this side too, not just cue-linux's problem.
- **Current full state of the cue-linux side** (task order, settled decisions on system
  audio/screenshot/identity/cleanup, what's done vs. not-yet-started) lives in
  `cue-linux/CONTEXT.md` and that repo's own grilling-session handoff — not duplicated
  here. As of this writing, **no implementation code has been written on either project**;
  both sides are fully designed/planned but pre-implementation.

## 2. What already exists in Hyprland today (baseline, config-level)

Hyprland ships a real, built-in feature: the `noscreenshare` / `no_screen_share` window rule (windows since v0.50.0, July 2025; layer-shell surfaces since v0.52.0, with a follow-up popup fix in v0.52.2). Applying it needs no source changes — just a window rule matched on window class, plus `float` + `pin` to survive workspace/tiling switches (tiling was never actually a blocker — floating + pinned windows already ignore workspace switches).

**This existing feature draws a black rectangle over the protected surface. It does not make the surface's background show through.** That gap — black box vs. true "not there" transparency — is the entire subject of this investigation.

Also confirmed early on: Aquamarine (Hyprland's GPU/KMS backend library) is irrelevant to this whole problem. It only handles talking to the GPU/output (modesetting, buffer swap). It has no concept of windows/surfaces. All relevant logic lives in Hyprland's own renderer and protocol code, layers above Aquamarine. Don't waste time reading Aquamarine source in future sessions.

## 3. Corrections made mid-investigation (important — read this before trusting older sources)

Earlier in this investigation, general web search / DeepWiki (a third-party doc site indexing some older commit) produced a picture that turned out to be **out of date** once checked against the real `v0.56.2` tag:

| Old belief (from search/DeepWiki) | What the real v0.56.2 source shows |
|---|---|
| Logic lives in `src/protocols/Screencopy.cpp` and `src/protocols/ToplevelExport.cpp`, duplicated per-protocol | Refactored into one shared module: `src/managers/screenshare/ScreenshareFrame.cpp` (+ `ScreenshareManager.cpp`, `ScreenshareSession.cpp`). All three protocols (`Screencopy.cpp`, `ToplevelExport.cpp`, `ImageCopyCapture.cpp`) now just parse the wire protocol and delegate into this shared manager. **The "3 separate implementations" problem is already solved upstream.** |
| `keep_unmodified_copy` / the "mirror texture" is some kind of pre-overlay clean scene we could reuse directly | It's the literal, fully-composited real screen output (same texture used for physical monitor mirroring), with the protected window's real content already baked in. **Not usable as-is for true exclusion.** This was a wrong assumption, corrected by reading `getMirrorTexture()`'s call chain in `Renderer.cpp`. |
| Special workspaces need a second, separate render call to appear in a capture-exclusion render | False. Special-workspace windows are pulled into the *same* single `renderWorkspaceWindows()` call as normal windows, via one line in `shouldRenderWindow()`. No second call needed. This was also a wrong initial assumption, corrected by reading the real render sequence. |

**Lesson for the next session: assumptions from search results / DeepWiki must be re-verified against the cloned tag before being treated as fact.** Two out of the assumptions made this way turned out wrong. The clone-and-grep approach caught both.

Unresolved version-drift item, deliberately parked by the user: Hyprland moved from `hyprland.conf` (hyprlang) to a Lua-based config as of v0.55. Any config-syntax examples given earlier in this conversation (`windowrulev2 = noscreenshare, class:...`) may need Lua translation. **User explicitly said: don't worry about this yet, config syntax is a later problem once the core rendering question is settled.**

## 4. Confirmed architecture of today's `noscreenshare` (v0.56.2, read directly from source)

All of this is read from the real files, not inferred.

- **Flag storage**: per-window/per-layer, via `m_ruleApplicator->noScreenShare()` on `CWindowRuleApplicator` (windows) and the equivalent on `CLayerRuleApplicator` (layer-shell surfaces) — two separate class hierarchies, confirmed still separate in v0.56.2.
- **Correction/new finding, verified 2026-09-10 (see §11): `shouldRenderWindow()` has two overloads with different logic, and only one of them is on the path this investigation cares about.** `IHyprRenderer::shouldRenderWindow(PHLWINDOW, PHLMONITOR)` (two-arg) is the overload actually used by `renderMonitor()`'s existing black-box window loop (`ScreenshareFrame.cpp:267`, calling `shouldRenderWindow(w, PMONITOR)`), and is also the exact overload called by both `renderWorkspaceWindows()` and `renderWorkspaceWindowsFullscreen()` — confirmed by reading all call sites in `Renderer.cpp`. A separate, single-argument `IHyprRenderer::shouldRenderWindow(PHLWINDOW)` overload also exists, with its own, differently-worded special-workspace check (`m->m_activeSpecialWorkspace && pWindow->onSpecialWorkspace()`, vs. the two-arg overload's `pMonitor->m_activeSpecialWorkspace == pWindow->m_workspace`) — this single-arg overload is used by unrelated call sites and by single-window capture's `renderWindow()` path (`ScreenshareFrame.cpp:331`), not by anything this investigation's planned patch touches. Any future gating check (§8 step 2) must be added to the **two-arg overload specifically** — patching the single-arg one would silently do nothing for monitor/region capture.
- **Where the picture comes from** (`src/managers/screenshare/ScreenshareFrame.cpp`, function `renderMonitor()`, used for `SHARE_MONITOR`/`SHARE_REGION`, i.e. full-screen capture — this is the path meeting-app screen share uses):
  1. Line ~182: grabs `g_pHyprRenderer->m_renderData.pMonitor->resources()->getMirrorTexture()` — the already fully-composited real frame, protected window's real pixels included.
  2. Draws that whole texture into the capture output.
  3. **Then**, in a second step, loops every layer (~line 240) and every window (~line 263) with `noScreenShare` set, and paints a literal `CRectPassElement` with `Colors::BLACK` on top, at that surface's current on-screen box (lines ~255, ~291-298).
- **Where the mirror texture itself gets made**: NOT in the screenshare code at all. It's populated in the normal per-frame render, `Renderer.cpp`, right after the real on-screen frame finishes (`endRender()`, ~line 2229), gated by `CMonitor::needsACopyFB()` (true if there's a real monitor mirror OR an active screenshare session — `src/output/Monitor.cpp:2713`, `needsUnmodifiedCopy()`). Confirms it's a plain post-hoc copy of the real screen, not a capture-specific composite.
- **Single-window capture** (`hyprland-toplevel-export-v1`, used for previews, not meeting-app screen share): **correction (verified 2026-09-10 against actual source, see §11):** the permission-denied placeholder-texture substitution is not inside `renderWindow()` as originally written here — it lives in `render()`, gated by `windowShareDenied = m_session->m_type == SHARE_WINDOW && ... noScreenShare().valueOrDefault()`. The behavior described (never renders the real window, substitutes `m_screencopyDeniedTexture`) is otherwise correct. This is a real, already-existing full-skip precedent in the codebase, just not usable for the "reveal what's behind" full-monitor case because a single-window capture has no "behind" to reveal.
- **Live, currently-open bug confirming how fragile the black-box approach is today**: `hyprwm/Hyprland#13150` (filed 2026-01-30, unanswered as of this writing). **Correction (verified 2026-09-10, see §11): this is a GitHub *Discussion*, not an Issue, and describes desync during *browser/site zoom on a Discord screen-share, on floating windows specifically* — not a pinch-zoom gesture.** The black rectangle's position desyncs from the actual window, letting real content show at the wrong screen region. This is direct proof the current implementation tracks window geometry separately/late relative to the real frame, not proof of anything about our planned change specifically, but it's the closest real-world evidence of the geometry-sync failure mode our new code would also have to avoid.
- Two already-fixed historical bugs, confirmed fixed upstream by version, not needing rework: fade/close-animation black-box leak (fixed v0.51.0), a scaling bug (fixed, exact version not pinned down). A developer comment (not yet found as an exact source line) says special-workspace transparency/gaps can make the black box look wrong — status: **resolved 2026-09-10, see §11 — this is `hyprwm/Hyprland#11610`, a real, quoted maintainer (vaxerski) exchange confirming it's a known, accepted, deliberately-unfixed cosmetic limitation, not a coverage bug.**

## 5. Why screenshot tools (flameshot etc.) get caught too — confirmed, not a bug to "fix"

Hyprland has no concept of "this capture request is a screen-share, that one is a personal screenshot." Every capture path — `wlr-screencopy`, `ext-image-copy-capture-v1`, `hyprland-toplevel-export-v1`, and PipeWire/portal-based capture used by browsers/Zoom — ends up calling into the same `Screenshare::mgr()` shared code. The exclusion rule fires on **any** capture request, uniformly. Confirmed: portal-based capture (`xdg-desktop-portal-hyprland`) is a separate project/process, but its own docs confirm it uses these same two Wayland protocols (`zwlr_screencopy_manager_v1`, `hyprland_toplevel_export_v1`) rather than having its own rendering — so portal and direct capture really do converge on the same Hyprland-side code. Practical workaround people use today: a waybar toggle script that comments/uncomments the window rule and reloads config before taking a personal screenshot.

## 6. The core finding: why "just skip the black-box draw" does NOT give transparency

This was the single most important correction in the whole investigation, and it's fully sourced (§4, mirror-texture trace). Because the base texture used for capture is a **post-hoc copy of the already-fully-rendered real screen**, simply not drawing the black rectangle would reveal the protected window's *real content* (already baked into that texture), not whatever is visually behind it. There is no "pre-overlay" scene sitting around anywhere in the current architecture to fall back to.

**Conclusion: true "skip and reveal background" requires a second, independent full composite of the scene that omits the flagged surface(s) during composition itself — not a post-processing step on top of the existing one.**

## 7. Confirmed: the infrastructure for a second full composite already exists and is already used in production

Found by tracing every caller of `beginFullFakeRender()` in the real source (four total):

1. `ScreenshareFrame.cpp::copyShm()` — full second render into a fresh framebuffer, for the SHM screenshot/capture path. **This is the closest existing precedent to what we'd build.**
2. `ScreenshareFrame.cpp::storeTempFB()` — same pattern, snapshotting a frame while a permission-prompt popup is pending.
3. `CursorshareSession.cpp` — cursor-image-only, irrelevant to window content.
4. `PointerManager.cpp` — cursor-image-only, doesn't even call the shared `render()`, draws one texture directly.

Only #1 and #2 matter. Both already do "open a new buffer → render into it → `endRender()`" successfully in production today. The DMA-buf capture path (`copyDmabuf()`, likely what PipeWire/portal-based screen share actually prefers for performance) uses a sibling function, `beginRender(..., RENDER_MODE_TO_BUFFER, ...)`, **not** `beginFullFakeRender()` — but it calls the same shared `render()` afterward, so fixing `render()`'s internals fixes both the SHM and DMA paths without touching either wrapper.

**Also confirmed clean:** the real drawing functions we'd need to call — `renderAllClientsForWorkspace()`, `renderWorkspaceWindows()`, `renderLayer()` — take all their needed state as plain parameters (monitor, workspace, time, layer, etc.). They don't rely on hidden global setup done elsewhere first. `renderWindow()` (called internally) manages its own transient state (`m_renderData.currentWindow`) itself, set at entry and cleared at exit. This means calling these functions from a new context (inside the screenshare code, not the main frame loop) is architecturally sound, not a hack bolted onto an unrelated call path.

**Also confirmed (§ special workspaces, corrected from an earlier wrong guess):** special-workspace windows are included via one line inside `shouldRenderWindow()` itself (`if (pMonitor->m_activeSpecialWorkspace == pWindow->m_workspace) return true;`), not via any second render call. The real per-frame render only ever makes **one** call — `renderWorkspace(pMonitor, pMonitor->m_activeWorkspace, NOW, renderBox)` — and that single call already covers special-workspace content. So the planned capture-only render also only needs **one** call to get special-workspace content for free, correctly included or correctly excluded by the same new check.

**New finding, verified 2026-09-10 (see §11): `renderWorkspace()`'s real body has two more pre-existing behaviors worth knowing before writing the patch.** `renderWorkspace()` is a thin wrapper (computes a translate/scale from the passed geometry, then calls `renderAllClientsForWorkspace()`, confirmed by reading both functions directly). `renderAllClientsForWorkspace()` itself — the function that actually calls `renderBackground()`, iterates and draws each shell layer via `renderLayer()`, and dispatches to `renderWorkspaceWindowsFullscreen()` or `renderWorkspaceWindows()` — has two pre-existing guards neither this doc nor any prior summary had documented:
1. **Session-lock guard**: an early return when `g_pSessionLockManager->isSessionLocked()` is true and `misc:session_lock_xray` is not enabled (and the lock is past the "missing/locked/denied" threshold). A capture-exclusion render reusing this function inherits this guard automatically — locked-session capture behavior is unaffected by the planned patch, since it goes through the identical code path the normal on-screen render already uses.
2. **`render:xp_mode` branch**: when this config value is set, the background and bottom shell-layer are skipped entirely (a different code branch than the normal case, confirmed by reading both the `xp_mode`-true and `xp_mode`-false branches side by side — they're near-duplicates of each other, one with the background/bottom-layer block, one without).

Neither of these is something the planned patch needs to add handling for — they come for free by virtue of reusing `renderAllClientsForWorkspace()` rather than writing a new, parallel render function. They're listed here so a future implementer isn't surprised that a locked session or `xp_mode`-enabled config changes what the new capture-exclusion render produces, in exactly the same way it already changes what the normal on-screen render produces.

## 8. The exact, smallest change identified so far (design only — no code written)

Files/functions, in the order they'd need touching:

1. **`src/render/Renderer.hpp` / `Renderer.cpp`** — add one new boolean state on `IHyprRenderer` (name TBD, e.g. "rendering for capture exclusion").
2. **`Renderer.cpp`, `shouldRenderWindow()`** — add one check near the top: if the new flag is set and the window's `noScreenShare()` is true, return false. (Confirmed this function already gates *every* window path that matters: tiled/floating via `renderWorkspaceWindows()`, and fullscreen via `renderWorkspaceWindowsFullscreen()` — both call it. Popups and subsurfaces ride along for free since they're drawn from the same already-filtered window object, not gated separately.)
3. **`Renderer.cpp`, `renderLayer()`** — add the equivalent check right next to the existing `if (!pLayer->visible()) return;` line. **Layers are a genuinely separate gate from windows — this step cannot be skipped or merged with step 2.**
4. **`src/managers/screenshare/ScreenshareFrame.cpp`, `renderMonitor()`** — replace the current "grab mirror texture, draw black boxes" body with: turn the new flag on, call `renderAllClientsForWorkspace()` (or `renderWorkspace()`) for the monitor's active workspace, turn the flag back off. Remove the now-unneeded black-box loop.
5. **Same function** — add a "block extra frame-done callback to the client" guard around the new draw call. A working precedent for this already exists in the same file, in the single-window (`SHARE_WINDOW`) path: `g_pHyprRenderer->m_bBlockSurfaceFeedback = ...` with the comment "block the feedback to avoid spamming the surface if it's visible." The full-monitor path currently has no such guard because it never touched real windows before — this would be new for that path, copying an existing pattern, not inventing one.

**State that must be saved and restored, confirmed by reading the existing code's own patterns:**
- The new exclusion flag itself — set true right before the draw call, false right after, on every exit path.
- `m_renderData.renderModif.enabled` — already has an explicit save/restore pattern in the existing code (`const auto OLD = ...; set false; [draw]; restore OLD;`). Reuse the same pattern for anything new.
- `m_renderData.transformDamage` and `m_renderData.noSimplify` — the existing code turns these on before drawing, but **restoration was not confirmed found nearby in this investigation.** This needs a direct check before writing any patch — do not assume the existing code restores them correctly; if it doesn't, don't copy that gap into new code.
- `m_renderData.pMonitor` / `m_renderData.currentFB` — already correctly reset elsewhere (`copyShm()` calls `.reset()` after `endRender()`). Keep as-is.

## 9. Open items — not yet resolved, flagged honestly, in priority order for the next session

1. **Does `transformDamage`/`noSimplify` actually get restored today?** Needs a direct read of more of `ScreenshareFrame.cpp` / wherever these fields are consumed, to see if there's a restore further down that wasn't visible in the range already viewed.
2. **`ImageCopyCapture.cpp` (`ext-image-copy-capture-v1`)** — confirmed to exist and confirmed to delegate to `Screenshare::mgr()` like the other two protocols, but not yet read line-by-line the way `ScreenshareFrame.cpp` was. There's a known unrelated NVIDIA color-channel-swap bug on this specific protocol (red/blue swap) — worth checking this path doesn't have other surprises before relying on it.
3. **The developer comment about special workspaces "looking wrong" with `noscreenshare`** — not yet matched to an exact source line. Given the new finding that special workspaces share the same single render call and gate (§7), this is probably a drawing-order/z-stacking cosmetic issue, not a missing-coverage issue, but this is a hypothesis, not confirmed.
4. **Performance under the new design** — the new full second composite (real per-window/per-layer draw, not a cheap texture blit) runs synchronously in `CScreenshareManager::onOutputCommit()`, i.e. after the real frame commits but before the compositor is free to move on, **on the same thread, not overlapped.** This was reasoned about, not benchmarked. Needs actual profiling on real hardware before/after, especially at high refresh rate / high resolution, since that's where the added cost would first become visible.
5. **XWayland behavior** — not tested at all yet. Flagged early as a likely source of extra flakiness (some Electron/meeting-app builds may run under XWayland, which has historically had separate, worse-supported capture behavior — e.g. KDE's `xwaylandvideobridge` workaround exists for exactly this class of problem). No source-level XWayland-specific check has been done on the v0.56.2 screenshare code yet.
6. **GPU/driver variance (NVIDIA especially)** — known from changelog scanning to be a recurring source of screencopy/DMA-buf bugs in this codebase generally. Not checked against the specific new-render-path design. Needs real-hardware testing, not source reading, once code exists.
7. **Config syntax (Lua vs. hyprlang)** — deliberately deferred by the user, see §3. Will matter once there's something to actually configure/test, but not before.
8. **App choice (OpenCluely vs. cue vs. other)** — deliberately deferred by the user until this feasibility question is settled (§1).

## 10. What is now considered actually settled (safe to build on without re-verifying, per this investigation)

- Hyprland's own compositor code is the only place that matters; Aquamarine is irrelevant.
- The three capture protocols already converge on one shared code path (`Screenshare::mgr()` / `ScreenshareFrame.cpp`) in v0.56.2 — fixing one file fixes all three.
- The tooling to do a full independent second render into a separate buffer already exists (`beginFullFakeRender`) and is proven in production (`copyShm()`, `storeTempFB()`).
- The real per-window/per-layer draw functions (`renderAllClientsForWorkspace`, `renderWorkspaceWindows`, `renderLayer`, `renderWindow`) are cleanly parameterized and safe to call from a new context.
- Skipping a window in a back-to-front painter's-algorithm draw loop naturally reveals whatever was drawn before it — confirmed by reading the actual draw-order code in `renderWorkspaceWindows()`, not just assumed from general compositor theory.
- Special-workspace content needs no special handling beyond the one shared gate function — confirmed by reading the real per-frame render sequence.
- Popups and subsurfaces ride along with their parent window/layer automatically — confirmed by how they're drawn (same object, same call, not separately gated).

**Net assessment as of this handoff:** this is a real, buildable compositor feature, scoped to roughly 4-5 specific functions across 2 files, with 3-4 pieces of shared render state that must be handled correctly, one still-unverified restore-state question, and one currently open upstream bug (#13150) whose root cause the new design would need to actively avoid reproducing. It is not a weekend config change (that part — `noscreenshare` — already exists and only gives the black-box behavior). It is also not a rewrite of the compositor. The next concrete step is resolving open item #1 (state restoration) and #2 (reading `ImageCopyCapture.cpp` fully), then writing an actual draft patch against the local `v0.56.2` clone for local testing (e.g. via OBS's PipeWire capture source) before anything touches a real meeting app.

---

## 11. Update — decisions round (2026-09-10)

Everything in §9 has been resolved via a `grilling` session run against this repo. This
section is a pointer/summary — the canonical, full-detail record lives in **`CONTEXT.md`**
(repo root) and **`docs/adr/0001-capture-exclusion-kill-switch.md`**. Don't re-derive
what's written there; read it.

**§9 items, resolved:**

1. **State restoration** — confirmed by direct source read: `renderMonitor()` and
   `renderWindow()` set `m_renderData.transformDamage`/`m_renderData.noSimplify` but do
   not restore them in-function; today's code tolerates this because the *next*
   `beginRender` cycle resets them fresh. Decision: the new capture-exclusion render's
   call into `renderWorkspace()` is nested *inside* `renderMonitor()` (not a fresh render
   cycle), so it must explicitly save/restore both fields around itself — the existing
   gap is not safe to inherit here. See `CONTEXT.md`.
2. **`ImageCopyCapture.cpp`** — read in full. Confirmed zero independent rendering logic,
   zero `noScreenShare`-specific handling, no NVIDIA color-swap issue in this file; it
   purely delegates to `Screenshare::mgr()`/`CScreenshareFrame::share()` — the same entry
   point `renderMonitor()` uses. Consequence: patching `renderMonitor()` alone gives this
   protocol true exclusion for free, no separate change needed.
3. **Special-workspace "looks wrong" developer comment** — found and confirmed:
   `hyprwm/Hyprland#11610` (GitHub Discussion), maintainer vaxerski quoted confirming
   it's a known, deliberately-accepted cosmetic limitation ("you dont want to re-render
   the entire display for screensharing" — the exact cost this project chooses to pay).
   Not a missing-coverage bug; matches the §4/§7 special-workspace finding.
4. **Performance** — decision: leave unbenchmarked and un-budgeted through the first
   patch (no refresh-rate/window-count fallback logic). Correctness first; profile on
   real hardware once cue-linux runs against it end-to-end; decide on a fallback then.
   Additionally decided: an **early-exit** in `renderMonitor()` — skip the
   capture-exclusion render entirely when zero windows/layers are flagged, keeping the
   cheap mirror-texture path — as a cheap, always-correct perf floor independent of any
   future benchmarking.
5. **XWayland** — decision: tracked as a known-risk follow-up, not a blocker; not
   meaningfully testable without a real client (cue-linux, pending its own Wayland-native
   work).
6. **GPU/driver (NVIDIA) variance** — decision: same as XWayland, follow-up not blocker;
   needs real hardware, not source reading.
7. **Config syntax (Lua vs. hyprlang)** — resolved. v0.55+ uses Lua; the old
   `windowrulev2 = noscreenshare, class:...` form is invalid in v0.56.2. Canonical form
   (source-cited in `CONTEXT.md`): `hl.window_rule({ match = { class = "..." },
   no_screen_share = true, float = true, pin = true })`. Key is `no_screen_share`
   (underscored), not `noscreenshare`.
8. **App choice (OpenCluely vs. cue vs. other)** — resolved; see the rewritten §1 above
   for the decision, the audit verdict, and why the app side's XWayland question is
   directly coupled to this patch. Full current state of that side lives in
   `cue-linux/CONTEXT.md`, not duplicated here.

**New decisions surfaced during the grilling round, not originally in §9:**

- **Sequencing**: land the new `IHyprRenderer` boolean flag (named
  `m_bCaptureExclusionPass`) + the `shouldRenderWindow()`/`renderLayer()` gating checks
  (§8 steps 1-3) as a standalone, compile-only checkpoint (the flag is always false, so no
  behavior can change yet) *before* rewriting `renderMonitor()`'s render body (§8 steps
  4-5). The real black-box-vs-true-exclusion behavioral test happens only after 4-5.
- **Scope**: first patch covers `renderMonitor()` (`SHARE_MONITOR`/`SHARE_REGION`) only —
  confirmed via item 2 above that this also covers `ext-image-copy-capture-v1` for free.
  `SHARE_WINDOW` is explicitly out of scope for this pass.
- **Cursor verification**: open item for §8 steps 4-5 — the existing mirror-texture path
  draws the cursor; a bare `renderWorkspace()` call may not. Must be explicitly checked,
  not assumed, once the render body is rewritten.
- **Kill-switch**: `HYPRLAND_DISABLE_CAPTURE_EXCLUSION=1`, read once via `getenv()` at
  compositor startup (never per-frame), forces the existing black-box fallback.
  Startup-only — does not help mid-session. Full reasoning, including the corrected split
  between "logically-wrong output" (contained by buffer separation) and "memory-safety
  bugs" (not contained by buffer separation — the actual risk this switch guards against),
  is in `docs/adr/0001-capture-exclusion-kill-switch.md`.
- **Cross-repo context**: this repo's `CONTEXT.md` now names `nitesh-77/cue-linux` as the
  sibling consumer project and why this feature exists for it specifically; cue-linux's
  own new `CONTEXT.md` names this fork back, so a cue-linux session hitting an apparent
  Wayland/compositor hard-limit knows to check whether it's actually fixable here instead.

**Status as of 2026-09-10: design and decisions complete, frontier empty, nothing left
unresolved from this investigation's own open-items list. Next step is implementation —
§8 steps 1-3 first, per the sequencing decision above — not further research.**

## 12. Two further findings from re-verifying against source while writing the spec (2026-09-10)

These surfaced while writing `spec-true-capture-exclusion.md` (now filed as
`nitesh-77/Hyprland#1`) and re-checking every claim in this doc against the actual
checked-out source rather than trusting the summaries above. Both are now folded into
§4 and §7 in place; this section just flags that they happened and why they matter.

1. **`shouldRenderWindow()` has two overloads, not one — §4 updated in place.** This doc
   previously referred to "`shouldRenderWindow()`" as if it were a single function. It
   isn't: there's a two-argument `(PHLWINDOW, PHLMONITOR)` overload and a separate
   single-argument `(PHLWINDOW)` overload, with different bodies and different
   special-workspace logic. Only the two-arg overload is on `renderMonitor()`'s path
   (confirmed via every call site in `Renderer.cpp`). This matters because §8 step 2's
   gating check must be added to the two-arg overload specifically — a patch that
   modified the single-arg one would compile fine and silently do nothing for
   monitor/region capture.
2. **`renderAllClientsForWorkspace()` has a session-lock guard and an `xp_mode` branch —
   §7 updated in place.** Neither was previously documented anywhere in this
   investigation. Both are pre-existing behaviors of the function the planned patch
   reuses (not something the patch introduces or must handle specially), but they mean a
   locked session or `render:xp_mode`-enabled config changes what the new
   capture-exclusion render produces, in the same way they already change the normal
   on-screen render. Worth knowing before implementation, not worth building extra logic
   around.

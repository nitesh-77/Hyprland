# ADR-0001: Startup-only env var kill-switch for the capture-exclusion render

## Status

Accepted

## Context

The `true-capture-exclusion` branch adds a new render pass (`m_bCaptureExclusionPass`,
see `CONTEXT.md`) that calls `renderWorkspace()` from inside `renderMonitor()` — a
render-function call from a context (`ScreenshareFrame.cpp`) those functions weren't
originally written to be called from. This is compositor code: a crash or framebuffer
corruption here does not fail one process, it can take down the entire Hyprland session
— every window, everything running — with no isolation from an app-level bug.

The feature is reached via a new Lua-exposed window-rule effect (`no_screen_share`,
already existing; the new render behavior activates whenever that flag is set and a
monitor/region capture starts). Because the trigger path runs through the Lua config
bindings, a kill-switch implemented *only* as a Lua config value would be vulnerable to
the same class of bug it exists to guard against — a bug in the Lua bindings themselves
could disable its own escape hatch.

"Not benchmarked yet, decide a fallback later" (a separate, accepted decision — see
`CONTEXT.md`'s performance note) covers the *slow* failure mode. This ADR covers the
*broken* failure mode: correctness bugs (null pointers, windows in unexpected states,
render functions hit outside their normal call sequence) that corrupt output or crash
the compositor outright.

Two distinct kinds of "broken" are in scope here, and they have different blast radii —
conflating them would overstate what this switch, or anything else, actually contains:

1. **Logically-wrong output.** The capture-exclusion render is invoked from inside the
   screenshare code path (`ScreenshareFrame.cpp`), which always targets its own capture
   buffer — `outFB` (SHM), the DMA-buf target, or `m_tempFB` — never the monitor's real
   on-screen framebuffer. A bug that produces wrong pixels or draws the wrong
   windows/layers is confined to that capture buffer: the recording/share looks wrong,
   but the user's actual visible desktop is untouched. This is a real, structural
   containment — not something the kill-switch has to provide.
2. **Memory-safety bugs.** Calling `renderWorkspace()`/`renderLayer()`/`renderWindow()`
   from a new context they weren't originally written to be called from is exactly the
   kind of change that risks out-of-bounds writes, stale pointers, or use-after-free —
   e.g. a window in a state those functions don't expect when invoked from their normal
   single call site. **Buffer separation does nothing to contain this.** An
   out-of-bounds write does not respect which logical buffer it was "supposed" to land
   in; it can corrupt unrelated compositor state, the real framebuffer, or crash the
   process outright. This is the actual crash risk the kill-switch in this ADR exists
   to guard against, and it is a distinct risk from (1), not a subset of it.

## Decision

Add a boolean read once from the environment at compositor startup:

```
HYPRLAND_DISABLE_CAPTURE_EXCLUSION=1
```

Read via `getenv()` exactly once at startup and cached as a bool. If set, the
capture-exclusion render is skipped unconditionally for the lifetime of that compositor
process, and `renderMonitor()` falls back to today's existing black-box behavior — as if
the `m_bCaptureExclusionPass` feature did not exist.

`getenv()` is explicitly **not** called per capture frame — that adds per-frame overhead
and would still not be genuinely runtime-toggleable (environment variables of a running
process aren't observable/mutable from outside in the way this safety mechanism implies).

## What this switch covers, and what it explicitly does not

**Covers:** primarily the memory-safety risk in (2) above — a capture-exclusion render
that corrupts compositor state or crashes at Hyprland launch, or the first time a
capture session triggers it. Reboot with the env var set, and the compositor starts in
the known-good black-box-only state — a real recovery path that doesn't require
reverting the commit and rebuilding. (Category (1), logically-wrong output, is already
contained by buffer separation and doesn't strictly need this switch to stay safe — it
needs it to stay *usable*, since wrong output in a live share is still a real problem
even though it can't crash the session.)

**Does not cover:** the failure happening *mid-session*, e.g. mid-interview, when a
capture-exclusion render is triggered for the first time deep into a running session.
There is no way to disable the feature without restarting Hyprland, which — for this
project's actual use case (an interview copilot, meant to be invisible precisely during
a live call) — means the practical mitigation for a mid-call failure is still "the call
degrades or the session dies," not "flip a switch and continue."

This gap is accepted for the first patch. A true mid-session kill-switch (a `hyprctl`
debug keyword, or a live Lua/config toggle read on every capture rather than once at
startup) is explicitly deferred, not ruled out — see Open Follow-ups below.

## Alternatives considered

- **Lua config value only.** Rejected as the *sole* mechanism: the trigger path for this
  feature runs through the same Lua config bindings, so a bug there could take the
  escape hatch down with it. Still a reasonable *second*, additive toggle later (see
  follow-ups) once the config path itself is trusted, but not a substitute for an
  env-var-based startup switch that exists outside that dependency.
- **`hyprctl` debug keyword (live, mid-session toggle).** Would actually close the
  mid-session gap this ADR accepts. Rejected for the *first* patch as more machinery
  than a minimal safety valve needs before the feature has ever run in practice; revisit
  once there's real operating experience with the capture-exclusion render.
- **No kill-switch; rebuild-and-restart as the recovery path.** Rejected: this is
  compositor-level code affecting the whole session, and "rebuild the compositor" is not
  an acceptable recovery step to reach for after a crash, even for a single-machine
  personal tool with a controlled testing cadence.

## Open follow-ups (not blocking the first patch)

- A true mid-session kill-switch (`hyprctl` debug keyword, or a config value re-read live
  rather than once at startup) if operating experience shows the startup-only gap matters
  in practice.
- Whether a live Lua/config toggle should exist *in addition to* the env var once the
  feature has enough runtime history that its own trigger path is no longer the primary
  suspect in a hypothetical failure.

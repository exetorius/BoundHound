# BoundHound — Usage & Profiling Workflow

`unreal.BoundHoundService` answers "are we CPU- or GPU-bound?" and drives Unreal Insights captures
from Python with no C++ required. Run everything here from the editor's Python console (or any
`execute_python`-style entry point).

| Method | Purpose |
|--------|---------|
| `frame_timing(target_fps=60)` | Game/Render/GPU/RHI thread ms + a robust CPU-vs-GPU `bound` verdict, a `hint`, and a per-thread `budget` pass/fail gate against `target_fps`. **Run FIRST.** |
| `force_hitch(thread="game", milliseconds=250, frames=1)` | **Test/validation helper.** Deliberately induce a hitch on a known thread. CPU paths self-validate in one call (`observed_peak_*_ms` + `verdict_matched_expect`); `gpu` reads back via `frame_timing`. See [Validating the verdict](#-validating-the-verdict-force_hitch). |
| `start_trace(name="mcp_capture", channels="")` | Start an Insights trace to file (default channel set if `channels` empty). |
| `stop_trace()` | Stop the active trace; returns the trace file path + size. |
| `get_trace_status()` | Whether a trace is active and which channels are enabled. |
| `bookmark(name)` | Drop a point-in-time bookmark in the active trace. |
| `region_start(name)` / `region_end(name)` | Begin / end a named region span in the active trace. |
| `analyse(source="both", file="")` | Read back trace and/or log → frame stats, worst frames, hitches, notable log lines. |
| `start_standalone(name, channels)` | Launch the game as a separate standalone process with a trace attached. Writes its own **timestamped log** (`<name>_<YYYYMMDD_HHMMSS>.log`) via `-abslog`, so `analyse("logs")` reads the standalone's log, not the editor's locked one. |
| `stop_standalone()` / `get_standalone_status()` | Control / inspect the standalone capture. |
| `start_pie()` / `stop_pie()` | Start/stop **in-process** Play-In-Editor (opens a PIE window in the editor process). Makes `frame_timing`/`force_hitch` read a live game world. See [PIE vs Standalone](#-pie-vs-standalone). |

All methods return a JSON string. For a representative reading, profile under PIE or a standalone
session, not the bare editor viewport.

## 🚦 STEP 0 — Is it CPU-bound or GPU-bound? (DO THIS FIRST, ALWAYS)

**Never optimise before you know which processor is the bottleneck.** Frame time is roughly
`max(GameThread, RenderThread, RHIThread, GPU)` — these run in parallel, so only the *longest* one sets your FPS.
Cutting GPU cost (shadows, Lumen, post-process) does **nothing** if the frame is game- or
render-thread bound, and vice-versa.

```python
import unreal, json
result = json.loads(unreal.BoundHoundService.frame_timing())        # gate against 60 FPS
result = json.loads(unreal.BoundHoundService.frame_timing(120))     # or any target
print(result)  # ...thread_ms, frame_ms, bound, bound_confidence, contested, hint, budget
```

Start PIE first and park in a representative/worst spot, then call it. It returns the per-thread ms,
a `bound` verdict (`GameThread` / `RenderThread` / `RHIThread` / `GPU`), and a `hint` with what to do next — the
same data the `stat unit` overlay shows, read straight from engine globals.

### The verdict is deliberately cautious

A single frame is noisy, so the verdict tells you *how much to trust it* instead of always declaring
a winner:

- **`bound_confidence`** — `clear` (bottleneck leads by >10%), `moderate` (a CPU thread wins but GPU
  timing was unavailable, so a hidden GPU cost can't be ruled out), `marginal` (top two threads are
  within 10% — a tie), or `none` (no meaningful timing yet).
- **`contested` / `contested_with`** — when the top two are within ~10 %, `contested` is `true` and
  `contested_with` names the runner-up. Frame-to-frame noise can flip which one "wins", so treat both
  as bottlenecks — take several readings or trace both.
- **`margin_ms`** — how far the bottleneck leads the runner-up, in ms.

If `gpu_ms` is `0` the GPU column is dropped from the ranking (its timing is unavailable that frame),
and any CPU verdict is downgraded to `moderate` — confirm it with the `r.ScreenPercentage 50` test.

### The budget gate

`frame_timing(target_fps)` also returns a `budget` block that turns the reading into a **pass/fail
guard**. Because the threads run in parallel, *every* thread must individually finish inside the
per-frame budget, so the gate reports each thread's headroom plus an overall `verdict`:

```jsonc
"budget": {
  "target_fps": 60,
  "budget_ms": 16.67,
  "frame_headroom_ms": -8.44,   // negative = over budget
  "meets_target": false,
  "verdict": "FAIL",            // PASS when frame_ms <= budget_ms
  "threads": {
    "game_thread":   { "ms": 25.1, "headroom_ms": -8.44, "over_budget": true },
    "render_thread": { "ms": 8.4,  "headroom_ms": 8.27,  "over_budget": false },
    "gpu":           { "ms": 11.2, "headroom_ms": 5.47,  "over_budget": false }
  }
}
```

Gate a CI/perf check on `result["budget"]["meets_target"]`, and read `over_budget` per thread to see
exactly which one blew the budget.

## 🧪 Validating the verdict (`force_hitch`)

`force_hitch` is a **test instrument**: it deliberately stalls a known thread so you can confirm the
verdict logic fires against a known ground truth. The CPU paths (`game`/`render`/`both`) are
**self-validating** — the stall is applied synchronously and timed inside the call, so a *single*
response tells you whether it landed. No follow-up `frame_timing` (see the race note below).

```python
import unreal, json
# Force a 250 ms game-thread hitch and read the result from the SAME call:
r = json.loads(unreal.BoundHoundService.force_hitch("game", 250))
# -> {"expect": "bound=GameThread", "observed_peak_game_ms": ~250, "verdict_matched_expect": true, ...}
assert r["verdict_matched_expect"]
```

The validation matrix — force each, expect the paired result:

| `thread` | What it stalls | Self-validates via | Expect |
|----------|----------------|--------------------|--------|
| `game`   | Game thread, `milliseconds`/frame | `observed_peak_game_ms` + `verdict_matched_expect` | game peak ≥ ½ stall & dominates → `true` |
| `render` | Render thread, `milliseconds`/frame | `observed_peak_render_ms` + `verdict_matched_expect` | render peak ≥ ½ stall & dominates → `true` |
| `both`   | Game **and** render, equal size | both peaks + `verdict_matched_expect` | both peaks ≥ ½ stall → `true` |
| `gpu`    | Supersamples via `r.ScreenPercentage` (auto-restored) | *follow-up* `frame_timing` | `bound: "GPU"` *if scene GPU cost tips past CPU — scene-dependent* |

Notes:
- **The CPU paths are race-free (issue #17).** Earlier, confirming a CPU hitch meant a follow-up
  `frame_timing`, but that reader runs on the game thread the stall blocks, so it reliably landed on a
  clean frame and missed the spike (the stall was real — visible in `stat unit` — just unread). Now
  `force_hitch` times its *own* stall and returns `observed_peak_*_ms` + `verdict_matched_expect`, so
  validation needs no second call.
- **`gpu` is the exception** — its cost is scene-dependent and persists across frames, so it can't be
  measured synchronously but *does* read back cleanly: for `gpu` alone, call `frame_timing` on a
  following frame and confirm `bound: "GPU"`.
- A live game world makes the reading representative — `start_pie()` or a standalone — but the CPU
  self-measurement holds even on the bare editor viewport (the stall is induced and timed directly).
- `frames > 1` sustains the hitch across N applications (simulate **jitter**); CPU total is capped at
  ~10 s of synchronous stall so a typo can't lock the editor (response reports `frames_requested` if capped).
- `milliseconds` is clamped to `[1, 5000]` and `frames` to `[1, 600]`.
- `gpu` is best-effort: on a trivial scene even 4× supersampling may not overtake the CPU threads.
  Prefer profiling in a representative/worst spot.

## 🎬 PIE vs Standalone

`frame_timing` and `force_hitch` need a **live game world** — the bare editor viewport isn't one. Two
ways to get one, and they are **not** equivalent:

| | `start_pie()` | `start_standalone()` |
|---|---|---|
| Process | In the editor process (a PIE window) | Separate game process |
| Reads via `frame_timing`/`force_hitch` | ✅ in-process, immediate | ❌ (separate process — use the trace + `analyse`) |
| Shader/PSO caches | **Warm** — reuses the editor's | Cold, like a real build |
| Representative of shipping cost | ❌ hides costs | ✅ closest to real |
| Best for | Quick in-process checks, verdict/hitch logic | **Real stall identification** |

**Rule of thumb: identify stalls in Standalone, not PIE.** PIE shares the editor's already-warm
shader/PSO caches and on-demand cooked data, so it under-reports exactly the hitches you're hunting.
`start_pie()` exists for fast in-process sanity checks (and so `force_hitch` has a world to stall);
when the numbers have to be trusted, capture a `start_standalone()` trace and `analyse()` it.

> This project has a **~30 s load hitch** on standalone startup — give a standalone session 45–60 s
> before stopping so your capture reflects steady-state, not loading.

## ⏱️ Frame-time budgets

FPS is just `1000 / frame_ms`. Because the threads run in parallel, **every** thread must
*individually* finish inside the budget — the slowest one alone sets your FPS.

| Target FPS | Per-frame budget |
|---|---|
| 30 FPS  | 33.33 ms |
| 60 FPS  | 16.66 ms |
| 120 FPS | 8.33 ms  |
| 240 FPS | 4.16 ms  |

If `game_thread_ms = 25`, you are hard-capped at ~40 FPS no matter what you do to the GPU. State the
bottleneck thread's ms next to the target budget so the gap is explicit.

## 🛠️ CVars tune the renderer — they do NOT fix the game thread

`r.*` console variables almost exclusively move **GPU** and **RenderThread** cost. There is **no CVar
that makes your Tick, AI, or animation logic cheaper.** When `bound == GameThread`, the fix lives in
code and Blueprints:

| Symptom (`stat dumpframe -root=gamethread`) | Typical fix |
|---|---|
| Many ticking actors / high `FTickFunctionTask` | Throttle `PrimaryActorTick.TickInterval`, disable tick when idle/far, event-drive |
| High `AnimGameThreadTime` | Enable URO, `VisibilityBasedAnimTickOption = OnlyTickPoseWhenRendered` |
| Expensive Blueprint `ReceiveTick` | Move per-frame logic to timers/events, cache, early-out |

### Decision tree
| `bound` | Where to look next |
|---|---|
| **GameThread** | Tick / Blueprint / AI / animation. `stat dumpframe -ms=0.5 -root=gamethread`. Fix is code/Blueprint, not CVars. |
| **RenderThread** | Draw calls & primitives, dynamic shadow-casting lights. `stat scenerendering`. Instance/merge meshes, Nanite, cut dynamic lights. |
| **RHIThread** | GPU-command submission (distinct from render-thread scene setup). Usually too many draw calls / state changes. `stat rhi`, draw-call count in `stat scenerendering`. Cut draw calls via merging/instancing/HISM, Nanite. *Only ranked when RHI-threading is on; otherwise its cost folds into RenderThread.* |
| **GPU** | *Now* run `ProfileGPU`. Shadows (VSM), Lumen, translucency, resolution. |

## ⚠️ Gotchas

- **`ProfileGPU` tells you WHERE GPU time goes, not WHETHER you are GPU-bound.** Run `frame_timing()`
  first; only reach for `ProfileGPU` once `bound == GPU`.
- **Never run `ProfileGPU` / `stat dumpframe` *during* a trace you intend to average** — each stalls
  the GPU for a readback and poisons the frame-time stats.
- **`analyse()` reports frame-time aggregates** (avg/p95/worst frames, hitches, notable log lines),
  **not** the live CPU/GPU split. Use `frame_timing()` for the per-thread split.
- **Profile under PIE or standalone, not the bare editor viewport** — the empty viewport isn't
  representative.
- **A trace must be active before you bookmark / mark regions.**
- **Trace files are large** — a ~10 s trace with default channels is 30–50 MB, under `Saved/Profiling/`.

## Channel reference

`start_trace` / `start_standalone` accept a comma-separated `channels` string; empty uses the default
set (`frame,cpu,gpu,log,loadtime,object,stats,bookmark,region`). Common presets:

| Investigation | `channels` |
|---|---|
| General (balanced) | `frame,cpu,gpu,stats,log` |
| Memory | `frame,memalloc,memtag,object,loadtime` |
| Animation / character | `frame,cpu,animation,stats` |
| UI / Slate | `frame,cpu,slate,stats` |
| Niagara / VFX | `frame,cpu,gpu,niagara` |
| Multiplayer | `frame,cpu,net,stats` |

Channel naming varies slightly by build; call `get_trace_status()` after `start_trace()` to see what
actually got enabled.

## Opening traces in Unreal Insights

`analyse()` covers most needs from Python, but you can open a `.utrace` for the full timeline UI via
*Tools → Run Unreal Insights*, or:

```
"<UE install>/Engine/Binaries/Win64/UnrealInsights.exe" "<project>/Saved/Profiling/<name>.utrace"
```

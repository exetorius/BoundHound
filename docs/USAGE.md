# BoundHound — Usage & Profiling Workflow

`unreal.BoundHoundService` answers "are we CPU- or GPU-bound?" and drives Unreal Insights captures
from Python with no C++ required. Run everything here from the editor's Python console (or any
`execute_python`-style entry point).

| Method | Purpose |
|--------|---------|
| `frame_timing(target_fps=60)` | Game/Render/GPU/RHI thread ms + a robust CPU-vs-GPU `bound` verdict, a `hint`, and a per-thread `budget` pass/fail gate against `target_fps`. **Run FIRST.** |
| `force_hitch(thread="game", milliseconds=250, frames=1)` | **Test/validation helper.** Deliberately induce a hitch on a known thread (`game`/`render`/`both`/`rhi`/`gpu`). CPU + RHI paths self-validate in one call (`observed_peak_*_ms` + `verdict_matched_expect`); `gpu` reads back via `frame_timing`. See [Validating the verdict](#-validating-the-verdict-force_hitch). |
| `start_trace(name="mcp_capture", channels="")` | Start an Insights trace to file (default channel set if `channels` empty). |
| `stop_trace()` | Stop the active trace; returns the trace file path + size. |
| `get_trace_status()` | Whether a trace is active and which channels are enabled. |
| `bookmark(name)` | Drop a point-in-time bookmark in the active trace. |
| `region_start(name)` / `region_end(name)` | Begin / end a named region span in the active trace. |
| `analyse(source="both", file="")` | Read back trace and/or log → frame stats, worst frames, hitches, notable log lines. For a `start_standalone()` run it also returns the **per-thread split and a CPU-vs-GPU verdict** for the whole run (see [Verdict from a standalone run](#verdict-from-a-standalone-run)). |
| `start_standalone(name, channels)` | Launch the game as a separate standalone process with a trace attached. Writes its trace to `Saved/Profiling/<name>_<YYYYMMDD_HHMMSS>.utrace`, its own log to `Saved/Logs/<name>_<YYYYMMDD_HHMMSS>.log` (`-abslog`), and a CSV-profiler capture with per-frame thread times to `Saved/Profiling/CSV/<name>_<YYYYMMDD_HHMMSS>.csv`, so `analyse` reads this run's files, not the editor's. Spaces and commas in `name` become `_`. The `trace_file` it returns is the file that gets written; `get_standalone_status()` and `stop_standalone()` report the same path. |
| `stop_standalone()` / `get_standalone_status()` | Control / inspect the standalone capture. `stop_standalone()` also returns `trace_file_exists` and `file_size_mb`, or a `trace_warning` if nothing was written. |
| `start_pie()` / `stop_pie()` | Start/stop **in-process** Play-In-Editor (opens a PIE window in the editor process). Makes `frame_timing`/`force_hitch` read a live game world. See [PIE vs Standalone](#-pie-vs-standalone). |

> **One play session at a time.** `start_pie()` and `start_standalone()` refuse to start while another
> play session is running, queued, or **still ending**, and return `error_code: "PLAY_SESSION_ACTIVE"`.
> Stopping PIE only *requests* the end; the teardown happens on the next engine tick. So stop the
> session, end your Python call / MCP request so the engine can tick, and start the next session in a
> new call. (Before v1.0.1, starting a standalone in the same call as a PIE stop crashed the editor.)
| `report(title, source="both", file="")` | Render a **self-contained HTML report** from the live verdict/budget + the most recent capture (frame stats, worst frames, PSO hitches) with a data-driven "fix in this order" list, to `Saved/BoundHound/report_<ts>.html`. See [The HTML report](#-the-html-report-report). |

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
- **`frame_number` / `timestamp`** — which engine frame (`GFrameCounter`) the reading describes, and
  when it was taken (UTC). Use them to spot stale reads.
- **`repeat_read`** — `true` when no new frame has completed since the previous call. The reading is
  then the *same frame again*, so `bound_confidence` is forced to `none` and the `hint` starts with
  `REPEAT READ`.
- **`warming` / `pie_seconds`** — `warming` is `true` for the first ~10 s of PIE, when frames are
  dominated by shader/PSO compiles and streaming. The `hint` starts with `WARMING`; read again later.

> **Sample across separate calls.** `frame_timing` reads the last *completed* frame. While your Python
> call (or MCP request) is running, the game thread is blocked, so no frame completes — calling it five
> times in one script, even with `time.sleep()` between, returns the same frame five times. Take each
> sample in its own call so the engine gets to tick in between.

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
| `rhi`    | RHI thread (render thread enqueues a self-timing lambda + flushes the RHI thread) | `observed_peak_rhi_ms` + `verdict_matched_expect` | rhi peak ≥ ½ stall & dominates both CPU threads → `true` — **only when RHI-threading is on** |
| `gpu`    | Supersamples via `r.ScreenPercentage` (auto-restored) | *follow-up* `frame_timing` | `bound: "GPU"` *if scene GPU cost tips past CPU — scene-dependent* |

Notes:
- **The CPU paths are race-free (issue #17).** Earlier, confirming a CPU hitch meant a follow-up
  `frame_timing`, but that reader runs on the game thread the stall blocks, so it reliably landed on a
  clean frame and missed the spike (the stall was real — visible in `stat unit` — just unread). Now
  `force_hitch` times its *own* stall and returns `observed_peak_*_ms` + `verdict_matched_expect`, so
  validation needs no second call.
- **`rhi` needs a separate RHI thread.** Like the CPU paths it's race-free and self-measured
  (`observed_peak_rhi_ms`), but the stall only lands when RHI-threading is on. When it's off the call
  reports `rhi_threading: false` + an `rhi_warning` and induces **no** stall (mirroring how the verdict
  drops RHI when it isn't a distinct thread) — re-run with `r.RHIThread.Enable 1`, or on a platform
  where it's the default, to validate the `RHIThread` bound.
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
| CPU-vs-GPU verdict | One live frame (`frame_timing`) | Whole run, from the capture (`analyse`) |
| Shader/PSO caches | **Warm** — reuses the editor's | Cold, like a real build |
| Representative of shipping cost | ❌ hides costs | ✅ closest to real |
| Best for | Quick in-process checks, verdict/hitch logic | **Real stall identification** |

**Rule of thumb: identify stalls in Standalone, not PIE.** PIE shares the editor's already-warm
shader/PSO caches and on-demand cooked data, so it under-reports exactly the hitches you're hunting.
`start_pie()` exists for fast in-process sanity checks (and so `force_hitch` has a world to stall);
when the numbers have to be trusted, capture a `start_standalone()` trace and `analyse()` it.

### Verdict from a standalone run

A standalone is a separate process, so `frame_timing` can't read it. Instead, `start_standalone()` runs
a CSV-profiler capture alongside the trace, recording the engine's `GameThreadTime`, `RenderThreadTime`,
`RHIThreadTime` and `GPUTime` for every frame (the same values `frame_timing` reads). `analyse()` finds
that CSV next to the trace and adds:

```jsonc
"thread_split": {
  "available": true,
  "frames": 7117,
  "csv_file": ".../Saved/Profiling/CSV/<name>_<ts>.csv",
  "game_thread":   { "avg_ms": 11.84, "median_ms": 11.2,  "p95_ms": 15.31 },
  "render_thread": { "avg_ms": 14.6,  "median_ms": 7.28,  "p95_ms": 23.72 },
  "rhi_thread":    { "avg_ms": 5.43,  "median_ms": 4.91,  "p95_ms": 5.92 },  // only with RHI-threading on
  "gpu":           { "avg_ms": 6.36,  "median_ms": 6.13,  "p95_ms": 7.42 }
},
"bound": "GameThread", "bound_confidence": "clear", "margin_ms": 3.93, "contested": false,
"bound_share": { "GameThread": 0.9, "RenderThread": 0.1, "RHIThread": 0, "GPU": 0 },
"bound_hint": "Verdict from the per-thread MEDIANS of 7117 captured frames ..."
```

- The verdict uses the same rules as `frame_timing` (contested within 10%, GPU-unavailable →
  `moderate`), applied to the per-thread **medians**, so one huge load frame can't swing it.
- `bound_share` is the fraction of frames each thread was the slowest. If the verdict's thread wins
  fewer than half the frames, the bottleneck alternates: confidence drops to `marginal` and the hint
  starts with `ALTERNATING`.
- The CSV capture starts once the engine is up (`-ExecCmds="CsvProfile StartFile=..., CsvProfile
  Start"`), so it covers the run after load. The load hitch itself is still in the trace's frame stats.
- Traces without a CSV next to them (from `start_trace()` in the editor, or standalone runs before
  v1.1.0) get `thread_split.available: false` with a note, and no verdict.
- `report()` uses this verdict ahead of a single in-process frame (`bound_source: "trace"`).

Why a CSV and not the trace: the engine records these four values with `CSV_CUSTOM_STAT_MINIMAL_GLOBAL`,
which never sends the stat names to Unreal Insights, so in a `.utrace` they're only unnamed counters.

> A cold standalone pays a large **load / first-frame hitch** (this project's fixture spikes ~35–43 s
> at frame 0), and cold caches keep the first minute load-dominated — so give a standalone session
> **90 s+** before stopping if you want steady-state numbers. A shorter capture is still fine for
> *demonstrating* the startup hitch (it's exactly what a warm PIE run hides), just not for hot-path
> averages.

## 📄 The HTML report (`report`)

`report()` turns a session into a **self-contained HTML "printout"** you can open in any browser,
screenshot, or drop into Discord/email — no login, no external assets (the CSS is inlined). It composes
the live `frame_timing` verdict + budget with the most recent capture and writes
`Saved/BoundHound/report_<timestamp>.html`, returning the path:

```python
import unreal, json
# after a StartStandalone / StopStandalone (or a StartTrace / StopTrace):
r = json.loads(unreal.BoundHoundService.report("Proteus perf", "both"))
# -> {"report_file": ".../report_<ts>.html", "included_capture": true, "fix_count": 3, "bound": "GameThread"}
```

What it contains:
- **Contrast tiles** — the warm in-process frame next to the capture's cold worst-frame and steady-state p95.
  The in-process frame is only shown and compared when it's a fresh, settled PIE frame. If PIE is still
  warming up, the read is a repeat, or PIE isn't running, the tile says why (`n/a`). That frame's
  verdict doesn't drive a fix either; the fix list instead starts with how to get a usable reading.
- **Bound verdict** — when the capture has a per-thread split (a `start_standalone()` run), the report
  uses the capture's verdict and shows a **captured-run thread table** (median, p95, share of frames
  slowest). The JSON result's `bound_source` says where the verdict came from: `trace`, `in_process`
  or `none`. The JSON result then has `in_process_representative: false` and an
  `in_process_caveat`.
- **An IN-PROCESS vs REPRESENTATIVE matrix** (only when a capture supplies the cold column) — the same
  PIE-hides-it / standalone-catches-it story as [PIE vs Standalone](#-pie-vs-standalone), auto-filled:
  numbers are live, the "where to improve" column is a fixed teaching layer.
- **Thread breakdown**, **worst frames**, and **PSO-hitch count** from the capture.
- A data-driven **"fix in this order"** list — each fix carries the concrete `stat`/console commands and
  a link to the matching UE 5.8 docs.

`source` is `"trace"` / `"logs"` / `"both"` (default), same as `analyse`; `file` overrides which
capture to summarise (empty = the last trace started/stopped). With no capture available it still writes
a verdict-only report (`included_capture: false`) — the matrix falls back to single-run tiles.

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
- **`analyse()` reports frame-time aggregates** (avg/p95/worst frames, hitches, notable log lines).
  The per-thread split and verdict are only there for `start_standalone()` runs (they come from the
  CSV capture). For an in-editor trace, use `frame_timing()` for the split.
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

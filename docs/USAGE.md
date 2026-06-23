# BoundHound — Usage & Profiling Workflow

`unreal.BoundHoundService` answers "are we CPU- or GPU-bound?" and drives Unreal Insights captures
from Python with no C++ required. Run everything here from the editor's Python console (or any
`execute_python`-style entry point).

| Method | Purpose |
|--------|---------|
| `frame_timing()` | Game/Render/GPU/RHI thread ms + a CPU-vs-GPU `bound` verdict + a `hint`. **Run FIRST.** |
| `start_trace(name="mcp_capture", channels="")` | Start an Insights trace to file (default channel set if `channels` empty). |
| `stop_trace()` | Stop the active trace; returns the trace file path + size. |
| `get_trace_status()` | Whether a trace is active and which channels are enabled. |
| `bookmark(name)` | Drop a point-in-time bookmark in the active trace. |
| `region_start(name)` / `region_end(name)` | Begin / end a named region span in the active trace. |
| `analyse(source="both", file="")` | Read back trace and/or log → frame stats, worst frames, hitches, notable log lines. |
| `start_standalone(name, channels)` | Launch the game as a separate standalone process with a trace attached. |
| `stop_standalone()` / `get_standalone_status()` | Control / inspect the standalone capture. |

All methods return a JSON string. For a representative reading, profile under PIE or a standalone
session, not the bare editor viewport.

## 🚦 STEP 0 — Is it CPU-bound or GPU-bound? (DO THIS FIRST, ALWAYS)

**Never optimise before you know which processor is the bottleneck.** Frame time is roughly
`max(GameThread, RenderThread, GPU)` — these run in parallel, so only the *longest* one sets your FPS.
Cutting GPU cost (shadows, Lumen, post-process) does **nothing** if the frame is game- or
render-thread bound, and vice-versa.

```python
import unreal, json
result = json.loads(unreal.BoundHoundService.frame_timing())
print(result)  # game_thread_ms, render_thread_ms, gpu_ms, rhi_thread_ms, frame_ms, bound, hint
```

Start PIE first and park in a representative/worst spot, then call it. It returns the per-thread ms,
a `bound` verdict (`GameThread` / `RenderThread` / `GPU`), and a `hint` with what to do next — the
same data the `stat unit` overlay shows, read straight from engine globals.

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

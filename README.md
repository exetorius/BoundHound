# 🐕 BoundHound

**Frame-rate triage for Unreal Engine 5.8+ — sniffs out whether you're CPU- or GPU-bound _before_ you waste a session optimizing the wrong thing.**

Unreal 5.8's native AI toolsets ship with **no** performance or tracing tools. BoundHound is a small, self-contained editor plugin that fills the gap: a one-call CPU-vs-GPU **bound verdict**, scripted Unreal Insights trace capture, and trace+log analysis — all callable from Python or the engine's native AI toolset (no C++ required to _use_ it).

> The golden rule of profiling: **never optimize before you know which processor is the bottleneck.** A frame is roughly `max(GameThread, RenderThread, GPU)` — only the longest one sets your FPS. Cutting GPU cost does nothing if you're game-thread bound. BoundHound makes that verdict the _first_ thing you see.

---

## Why it exists

Most profilers tell you *where* time goes. They don't tell you *whether the thing you're about to optimize even matters*. `ProfileGPU` will happily hand you a GPU breakdown on a frame that's actually CPU-bound, and send you optimizing shadows for nothing. BoundHound leads with the verdict, then gives you the tools to drill in once you know where to look.

## What you get

```python
import unreal, json
print(json.loads(unreal.BoundHoundService.frame_timing()))
```
```jsonc
{
  "game_thread_ms": 25.1,
  "render_thread_ms": 8.4,
  "gpu_ms": 11.2,
  "rhi_thread_ms": 1.9,
  "frame_ms": 25.1,
  "fps": 39.8,
  "bound": "GameThread",
  "bound_confidence": "clear",   // clear | moderate | marginal | none
  "margin_ms": 13.9,             // how far the bottleneck leads the runner-up
  "contested": false,            // true when the top two threads are within ~10%
  "hint": "CPU game-thread bound. Usual cause: Tick / Blueprint / AI / animation cost. Run 'stat dumpframe -ms=0.5 -root=gamethread' ... dropping r.ScreenPercentage will NOT help a game-thread-bound frame.",
  "budget": {
    "target_fps": 60,
    "budget_ms": 16.67,
    "frame_headroom_ms": -8.44,  // negative = over budget
    "meets_target": false,
    "verdict": "FAIL",
    "threads": {
      "game_thread":   { "ms": 25.1, "headroom_ms": -8.44, "over_budget": true },
      "render_thread": { "ms": 8.4,  "headroom_ms": 8.27,  "over_budget": false },
      "gpu":           { "ms": 11.2, "headroom_ms": 5.47,  "over_budget": false }
    }
  },
  "pie_running": true
}
```

One call, and you know: **25.1 ms on the game thread caps you at ~40 FPS no matter what you do to the GPU** — and it `FAIL`s a 60 FPS budget on the game thread alone. That's the whole pitch.

Pass a target to gate against a different frame rate: `frame_timing(120)`. When the top two threads are within ~10% the verdict reports `contested: true` and names the runner-up in `contested_with`, so you don't chase a false winner that frame-to-frame noise can flip. When `gpu_ms` is `0` (GPU timing unavailable) a CPU verdict is downgraded to `moderate` confidence — a hidden GPU cost could still be the real bottleneck.

## API

| Method | Purpose |
|--------|---------|
| `frame_timing()` | Game/Render/GPU/RHI thread ms + a CPU-vs-GPU `bound` verdict and `hint`. **Run first.** |
| `force_hitch(thread="game", milliseconds=250, frames=1)` | Test helper: deliberately stall `game`/`render`/`both`/`gpu` so you can confirm `frame_timing` catches it. |
| `start_trace(name, channels)` | Start an Unreal Insights trace to file (default channel set if `channels` empty). |
| `stop_trace()` | Stop the active trace; returns file path + size. |
| `get_trace_status()` | Whether a trace is active and which channels are enabled. |
| `bookmark(name)` | Drop a point-in-time bookmark in the active trace. |
| `region_start(name)` / `region_end(name)` | Begin / end a named region span in the trace. |
| `analyse(source, file)` | Read back a trace and/or log → frame stats, worst frames, hitches, notable log lines. |
| `start_standalone(name, channels)` | Launch the game as a separate standalone process with a trace attached. |
| `stop_standalone()` / `get_standalone_status()` | Control / inspect the standalone capture. |

All methods return a JSON string. Full workflow and gotchas in [`docs/USAGE.md`](docs/USAGE.md).

## AI Assistant skill (zero setup)

The plugin ships a native **AgentSkill** (`UBoundHoundTriageSkill`) that teaches UE 5.8's AI Assistant *how* to use these tools — the strategy layer on top of the per-tool descriptions (frame_timing first → interpret the verdict → act per thread → trace → validate with force_hitch). Because UE discovers skills by scanning `UAgentSkill` subclasses (native classes included) and the default allow/block lists are empty, it is **auto-registered on plugin load** — no content asset, no config. The Assistant finds it via `list_skills` and reads it via `get_skills`.

## Recommended flow

```python
import unreal, json

# 1. CPU vs GPU verdict FIRST (start PIE, park in a representative spot)
print(json.loads(unreal.BoundHoundService.frame_timing()))

# 2. Capture a clean trace around the workload
unreal.BoundHoundService.start_trace("combat_encounter")
unreal.BoundHoundService.region_start("wave_spawn")
# (trigger the gameplay here)
unreal.BoundHoundService.region_end("wave_spawn")
print(json.loads(unreal.BoundHoundService.stop_trace()))

# 3. Summarise without leaving Python
print(json.loads(unreal.BoundHoundService.analyse("both")))
```

## Install

1. Copy this folder into your project's `Plugins/` directory (i.e. `YourProject/Plugins/BoundHound/`).
2. Regenerate project files and rebuild the editor target (it's a code plugin).
3. Enable **BoundHound** in *Edit → Plugins* if it isn't already.

**Requires Unreal Engine 5.8+** — it builds on the native `ToolsetRegistry` and `TraceServices`. Trace files land under `YourProject/Saved/Profiling/`.

## How it works

BoundHound is a single `UToolsetDefinition` subclass exposing static `AICallable` methods. On startup it auto-registers itself with UE 5.8's native `ToolsetRegistry`, so the methods show up both in Python (`unreal.BoundHoundService.*`) and on the engine's AI/MCP toolset surface. The frame split is read straight from engine globals (`GGameThreadTime` / `GRenderThreadTime` / `RHIGetGPUFrameCycles`) — the same data behind the `stat unit` overlay — so there are no screenshots to OCR and it works headless.

## License

MIT — see [LICENSE](LICENSE). Do whatever you like with it.

## Roadmap ideas

- **Baseline + diff** ("game thread +3.2 ms since last capture").
- **Auto drill-down**: when `bound == GameThread`, fire `stat dumpframe` and parse the worst scopes automatically.

PRs and issues welcome.

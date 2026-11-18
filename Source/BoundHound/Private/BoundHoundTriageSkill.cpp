// Copyright (c) 2026 exetorius. Released under the MIT License.

#include "BoundHoundTriageSkill.h"

UBoundHoundTriageSkill::UBoundHoundTriageSkill()
{
	// Shown in ListSkills -- keep it one line so the assistant can decide whether to open the skill.
	Description = TEXT(
		"Frame-rate triage & validation with BoundHound: determine whether the game is CPU (game/render/RHI) "
		"or GPU bound BEFORE optimising, gate against an FPS budget, capture standalone runs and explain "
		"their worst frames, and validate the verdict with deliberately forced hitches.");

	// Read via GetSkills -> GetDetails(). This is the strategy the per-tool descriptions can't carry.
	Instructions = FString(
		TEXT("BoundHound answers 'am I CPU- or GPU-bound?' and explains hitches. All tools return a JSON string.\n")
		TEXT("\n")
		TEXT("# Golden rule\n")
		TEXT("Know the `bound` thread BEFORE suggesting a fix. Optimising the GPU is wasted effort if the frame is game-, render- or RHI-thread bound, and vice-versa.\n")
		TEXT("\n")
		TEXT("# 1. Quick verdict in the editor: frame_timing(target_fps=60)\n")
		TEXT("Start PIE first (start_pie), wait ~10 s for it to settle, park in a representative spot. Read:\n")
		TEXT("- `bound`: GameThread | RenderThread | RHIThread | GPU. `bound_confidence`: clear | moderate | marginal | none -- trust `clear`.\n")
		TEXT("- `contested` / `contested_with`: top two threads within ~10%; treat BOTH as bottlenecks.\n")
		TEXT("- `repeat_read: true`: no new frame since your last call -- the SAME frame again. Frames only advance between calls: take each sample in its own Python call / MCP request; sleeping inside one call does nothing.\n")
		TEXT("- `warming: true`: PIE started under ~10 s ago; the frame is shader/PSO-compile noise. Read again later.\n")
		TEXT("- `pie_running: false`: you are reading the EDITOR viewport, not the game.\n")
		TEXT("- `gpu_ms == 0`: GPU timing unavailable, so a CPU verdict is unconfirmed; confirm with 'r.ScreenPercentage 50'.\n")
		TEXT("- `budget`: per-thread headroom + PASS/FAIL against target_fps.\n")
		TEXT("\n")
		TEXT("# 2. Trustworthy numbers: a standalone run\n")
		TEXT("PIE reuses the editor's warm shader/PSO caches and hides real stalls. For numbers you'd act on:\n")
		TEXT("start_standalone(name, '', res_x=1920, res_y=1080) -> let it run 2+ min past the load (cold loads can take 40 s+) -> stop_standalone() -> analyse('both').\n")
		TEXT("Fix the resolution when you'll compare runs. Only one play session at a time: after stop_pie/stop_standalone, END the call before starting another (else PLAY_SESSION_ACTIVE).\n")
		TEXT("analyse() on a standalone run returns:\n")
		TEXT("- `bound`, `bound_confidence`, `thread_split` (median/p95 per thread), `bound_share`: the verdict for the WHOLE run. Prefer it over a PIE frame. `ALTERNATING` in `bound_hint` = the bottleneck moves between threads.\n")
		TEXT("- `worst_frames[]`: each with `top_scopes` (game-thread self time), `log_lines` and `causes` (garbage_collection, sync_load, pso_compile, shader_compile, niagara_compile, level_streaming, game_thread_waiting) with a `lever` each. game_thread_waiting means look at WHAT it waited for (the other causes), not game code.\n")
		TEXT("- `spikes.periodic` + `interval_s`: a stall on a fixed beat; ~61 s = garbage collection.\n")
		TEXT("Long captures take ~30 s+ to analyse and can time out the Python call. The analysis still finishes and is cached: just call analyse() again (`cached: true`).\n")
		TEXT("\n")
		TEXT("# 3. Act on the verdict\n")
		TEXT("- GPU: ProfileGPU / a 'gpu' trace. Levers: Virtual Shadow Maps, Lumen, translucency, post-process, ScreenPercentage.\n")
		TEXT("- RenderThread: draw calls / primitives / dynamic shadowed lights ('stat scenerendering'). Levers: merge/instance, Nanite, fewer dynamic lights.\n")
		TEXT("- RHIThread: GPU command submission ('stat rhi'). Levers: fewer draw calls and state changes (instancing / HISM / Nanite).\n")
		TEXT("- GameThread: Tick / Blueprint / AI / animation ('stat dumpframe -ms=0.5 -root=gamethread' in PIE). Levers: throttle Tick, fewer ticking actors/AI.\n")
		TEXT("Lowering r.ScreenPercentage only helps a GPU-bound frame.\n")
		TEXT("\n")
		TEXT("# 4. Share it: report(title)\n")
		TEXT("Writes a self-contained HTML report (verdict, thread breakdown, explained worst frames, a 'fix in this order' list with commands + doc links) to Saved/BoundHound/. Run it after a standalone capture so it uses the run's verdict (`bound_source: trace`).\n")
		TEXT("\n")
		TEXT("# 5. Validate the verdict: force_hitch\n")
		TEXT("force_hitch stalls a KNOWN thread and measures the stall itself; check `verdict_matched_expect` in the SAME response (no follow-up frame_timing needed):\n")
		TEXT("- force_hitch('game', 250) / ('render', 250) / ('both', 250) / ('rhi', 250; needs RHI-threading on)\n")
		TEXT("- force_hitch('gpu') is the exception: it supersamples via r.ScreenPercentage (auto-restored); call frame_timing in a LATER call and expect bound == GPU.\n")
		TEXT("Use frames>1 to sustain the hitch and reproduce jitter.\n"));
}

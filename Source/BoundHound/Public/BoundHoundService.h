// Copyright (c) 2026 exetorius. Released under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"
#include "BoundHoundService.generated.h"

/**
 * Performance & tracing service -- frame-timing triage, Unreal Insights trace capture, trace+log
 * analysis, and trace-attached standalone play.
 *
 * Standalone extraction of VibeUE's UPerformanceService. Unreal 5.8's native toolsets have NO
 * performance or tracing tools, so this is net-new capability: it answers "are we CPU- or GPU-bound?"
 * and drives Insights captures the engine's EditorAppToolset (which can start PIE/Simulate) cannot
 * measure.
 *
 * Recommended flow: FrameTiming() FIRST (CPU vs GPU bound verdict) -> StartTrace -> reproduce the
 * workload -> StopTrace -> Analyse. For a representative reading, profile under PIE or a standalone
 * session, not the bare editor viewport. Methods return a JSON string.
 */
UCLASS(BlueprintType)
class BOUNDHOUND_API UBoundHoundService : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/**
	 * Report Game/Render/GPU/RHI thread ms + a CPU-vs-GPU bound verdict and hint for the most recently
	 * rendered frame (the same data as the on-screen "stat unit"). RUN THIS FIRST in any frame-rate
	 * investigation -- optimising the GPU does nothing if the frame is game- or render-thread bound.
	 *
	 * The verdict is robust: when the top two threads are within a small margin it reports the frame as
	 * "contested" with a confidence level rather than declaring a false winner, and it flags when GPU
	 * timing is unavailable (gpu_ms == 0) so you don't trust a CPU verdict that GPU could overturn.
	 *
	 * Also reports a per-thread budget breakdown against a target frame rate: each thread's headroom
	 * against the per-frame budget and a pass/fail gate, so the same call that triages can also guard.
	 * @param TargetFPS Frame-rate budget to gate against (e.g. 60, 120). Defaults to 60.
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "BoundHound|Performance")
	static FString FrameTiming(float TargetFPS = 60.0f);

	/**
	 * Start an Unreal Insights trace to file.
	 * @param Name Trace file name (without extension).
	 * @param Channels Comma-separated trace channels; empty uses the default set (frame,cpu,gpu,log,...).
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "BoundHound|Performance")
	static FString StartTrace(const FString& Name = TEXT("mcp_capture"), const FString& Channels = TEXT(""));

	/** Stop the active trace. Returns the trace file path and size. */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "BoundHound|Performance")
	static FString StopTrace();

	/** Report whether a trace is active and which channels are enabled. */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "BoundHound|Performance")
	static FString GetTraceStatus();

	/** Drop a named bookmark in the active trace. */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "BoundHound|Performance")
	static FString Bookmark(const FString& Name);

	/** Begin a named region in the active trace. */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "BoundHound|Performance")
	static FString RegionStart(const FString& Name);

	/** End a named region in the active trace. */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "BoundHound|Performance")
	static FString RegionEnd(const FString& Name);

	/**
	 * Read back a trace and/or the log and return a perf summary (frame stats, worst frames, notable
	 * log lines, hitches).
	 * @param Source "trace", "logs", or "both" (default).
	 * @param File Optional override path; empty uses the last trace started/stopped.
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "BoundHound|Performance")
	static FString Analyse(const FString& Source = TEXT("both"), const FString& File = TEXT(""));

	/**
	 * Launch the game as a separate standalone process with a trace attached (representative readings
	 * that the editor viewport can't give). Connects back to the editor's Unreal Trace Server.
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "BoundHound|Performance")
	static FString StartStandalone(const FString& Name = TEXT("standalone_capture"), const FString& Channels = TEXT(""));

	/** Stop the standalone process and finalise its trace/log. */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "BoundHound|Performance")
	static FString StopStandalone();

	/** Report whether a standalone session is running and which trace/log it is writing. */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "BoundHound|Performance")
	static FString GetStandaloneStatus();
};

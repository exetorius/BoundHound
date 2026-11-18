// Copyright (c) 2026 exetorius. Released under the MIT License.

#pragma once

#include "CoreMinimal.h"

// Pure, side-effect-free logic for explaining WHY a captured frame spiked (issue #25): which game-thread
// scopes ate the time, which log lines fired at that moment, what cause they point to, and whether spikes
// repeat on a fixed interval. Kept free of TraceServices so automation tests can run it headless;
// AnalyseTrace feeds it events read from the trace.
namespace BoundHoundDrilldown
{
	// One CPU timing scope on a thread timeline: [Start, End] in seconds, nesting depth, timer id.
	struct FScopeEvent
	{
		double Start = 0.0, End = 0.0;
		uint32 Depth = 0;
		uint32 TimerId = 0;
	};

	// Self ("exclusive") time per timer inside [WinStart, WinEnd]: each scope's clipped duration minus the
	// clipped durations of its direct children. Self time says where the ms actually went; inclusive time
	// would just crown the root scope (FEngineLoop::Tick) every frame. Events must be in start order with
	// parents before children (how a timeline enumerates). Returns the top N timers, largest first, in ms.
	inline TArray<TPair<uint32, double>> TopSelfTimes(const TArray<FScopeEvent>& Events, double WinStart, double WinEnd, int32 N)
	{
		TMap<uint32, double> Self;
		struct FOpen { uint32 Depth; uint32 TimerId; double End; };
		TArray<FOpen> Stack;
		for (const FScopeEvent& E : Events)
		{
			const double ClippedMs = (FMath::Min(E.End, WinEnd) - FMath::Max(E.Start, WinStart)) * 1000.0;
			if (ClippedMs <= 0.0) continue;
			while (Stack.Num() > 0 && (Stack.Last().Depth >= E.Depth || Stack.Last().End <= E.Start)) Stack.Pop();
			if (Stack.Num() > 0 && Stack.Last().Depth + 1 == E.Depth)
			{
				Self.FindOrAdd(Stack.Last().TimerId) -= ClippedMs; // a child's time isn't its parent's self time
			}
			Self.FindOrAdd(E.TimerId) += ClippedMs;
			Stack.Add({ E.Depth, E.TimerId, E.End });
		}
		TArray<TPair<uint32, double>> Out;
		for (const TPair<uint32, double>& KV : Self) if (KV.Value > 0.0) Out.Add(KV);
		Out.Sort([](const TPair<uint32, double>& A, const TPair<uint32, double>& B) { return A.Value > B.Value; });
		if (Out.Num() > N) Out.SetNum(N);
		return Out;
	}

	// A recognised spike cause, with what to do about it.
	struct FCause
	{
		FString Tag;    // garbage_collection | sync_load | pso_compile | shader_compile | niagara_compile |
		                // level_streaming | game_thread_waiting | hitch_logged
		FString Why;    // what the evidence means
		FString Lever;  // what to try
	};

	// Map the evidence around a spike (log lines + the names of its biggest scopes) to known causes. Each cause
	// appears at most once. Matching is on names the engine actually emits, case-insensitive.
	inline TArray<FCause> ClassifyCauses(const TArray<FString>& LogLines, const TArray<FString>& ScopeNames)
	{
		auto Any = [&](std::initializer_list<const TCHAR*> InLogs, std::initializer_list<const TCHAR*> InScopes) -> bool
		{
			for (const FString& L : LogLines)   for (const TCHAR* P : InLogs)   if (L.Contains(P, ESearchCase::IgnoreCase)) return true;
			for (const FString& S : ScopeNames) for (const TCHAR* P : InScopes) if (S.Contains(P, ESearchCase::IgnoreCase)) return true;
			return false;
		};
		TArray<FCause> Out;
		if (Any({ TEXT("CSVEvent \"GC\""), TEXT("Collecting garbage"), TEXT("LogGarbage") },
		        { TEXT("CollectGarbage"), TEXT("GarbageCollection"), TEXT("IncrementalPurge"), TEXT("ConditionalCollectGarbage") }))
		{
			Out.Add({ TEXT("garbage_collection"),
				TEXT("Garbage collection ran in this frame."),
				TEXT("Reduce UObject churn (pool spawned actors / projectiles), or spread the cost: gc.IncrementalBeginDestroyEnabled 1, gc.TimeBetweenPurgingPendingKillObjects, or call GC at a quiet moment (level transition, menu).") });
		}
		if (Any({ TEXT("FlushAsyncLoading"), TEXT("Flushing async loaders"), TEXT("LoadPackage"), TEXT("StaticLoadObject") },
		        { TEXT("FlushAsyncLoading"), TEXT("LoadPackage"), TEXT("StaticLoadObject"), TEXT("LoadSynchronous") }))
		{
			Out.Add({ TEXT("sync_load"),
				TEXT("The game thread blocked on loading (FlushAsyncLoading / a synchronous load)."),
				TEXT("Find the hard reference or LoadSynchronous call behind it and load asynchronously ahead of time (soft references + async load / AssetManager preloading). GC also flushes async loading, so check for garbage_collection too.") });
		}
		if (Any({ TEXT("PSO creation hitch"), TEXT("PSO cache miss"), TEXT("RTPSO"), TEXT("Waited for PSO creation"), TEXT("LogPSOHitching") },
		        { TEXT("PipelineState"), TEXT("CreateGraphicsPipeline"), TEXT("PSOPrecache"), TEXT("FRHICommandSetGraphicsPipelineState") }))
		{
			Out.Add({ TEXT("pso_compile"),
				TEXT("A pipeline state object was compiled on first use."),
				TEXT("Keep PSO precaching on and ship a bundled PSO cache gathered from a representative playthrough.") });
		}
		if (Any({ TEXT("LogShaderCompilers"), TEXT("Compiling shader") }, { TEXT("ShaderCompil"), TEXT("CompileShader") }))
		{
			Out.Add({ TEXT("shader_compile"),
				TEXT("Shaders were compiled at runtime (missing from the cooked shader cache / DDC)."),
				TEXT("Make sure the material permutations used here are cooked; check for materials changed at runtime (dynamic parameters that force new permutations).") });
		}
		if (Any({ TEXT("Compiling System NiagaraSystem"), TEXT("LogNiagara: Compiling") }, { TEXT("NiagaraSystem_Compile"), TEXT("FNiagaraSystemCompile") }))
		{
			Out.Add({ TEXT("niagara_compile"),
				TEXT("Niagara systems were compiled at runtime."),
				TEXT("Make sure the systems are compiled and saved in the editor and cooked; a system that compiles in a running game was edited or is missing its compiled data.") });
		}
		if (Any({ TEXT("LogLevelStreaming"), TEXT("LogWorldPartition") }, { TEXT("UpdateLevelStreaming"), TEXT("LevelStreaming"), TEXT("WorldPartition") }))
		{
			Out.Add({ TEXT("level_streaming"),
				TEXT("Level / World Partition streaming work landed in this frame."),
				TEXT("Increase streaming budgets spread (s.LevelStreamingComponentsRegistrationGranularity, s.UnregisterComponentsTimeLimit) or stream earlier with larger loading ranges.") });
		}
		// The frame's biggest self-time scope is the game thread idling until other work finishes: the cost is
		// in what it waited for, not in game code.
		static const TCHAR* WaitScopes[] = { TEXT("GameThreadWaitForTask"), TEXT("WaitForTasks"), TEXT("FrameSync"), TEXT("WaitForRenderThread"), TEXT("FlushRenderingCommands") };
		bool bTopIsWait = false;
		if (ScopeNames.Num() > 0) for (const TCHAR* W : WaitScopes) if (ScopeNames[0].Contains(W)) bTopIsWait = true;
		if (bTopIsWait)
		{
			Out.Add({ TEXT("game_thread_waiting"),
				FString::Printf(TEXT("The game thread spent most of this frame blocked in %s, waiting for other work."), *ScopeNames[0]),
				TEXT("Look at what it waited for: the other causes listed here, or the render / RHI / worker threads for this frame in Unreal Insights. Optimising game code won't shorten a wait.") });
		}
		if (Any({ TEXT("Hitch detected"), TEXT("hitch detected") }, {}))
		{
			Out.Add({ TEXT("hitch_logged"),
				TEXT("The engine's own hitch detector flagged this frame."),
				TEXT("Read the scopes above: the hitch detector only confirms the stall, it doesn't explain it.") });
		}
		return Out;
	}

	// Collapse spike timestamps that are less than MergeGapS apart into one event (a hitch often spans a
	// couple of consecutive slow frames). Input in any order; output sorted.
	inline TArray<double> MergeSpikes(TArray<double> Times, double MergeGapS = 1.0)
	{
		Times.Sort();
		TArray<double> Out;
		for (double T : Times) if (Out.IsEmpty() || T - Out.Last() >= MergeGapS) Out.Add(T);
		return Out;
	}

	struct FPeriodicity
	{
		bool   bPeriodic = false;
		double IntervalS = 0.0;  // median spacing of the periodic spikes
		int32  Count = 0;        // spikes that fit the period
		FString Likely;          // a known engine cadence it matches, if any
	};

	static constexpr double MIN_PERIOD_S = 5.0;        // shorter cadences are jitter, not a schedule
	static constexpr double PERIOD_TOLERANCE_S = 1.5;  // how far a spike may drift from the beat
	static constexpr double MIN_CHAIN_SHARE = 0.25;    // the beat must explain this share of all spikes
	static constexpr int32  MAX_SPIKES_SCANNED = 200;  // the search is cubic; a run with more spikes is just jittery
	static constexpr double GC_INTERVAL_S = 61.1;      // gc.TimeBetweenPurgingPendingKillObjects default

	// Do the (merged, sorted) spikes contain a fixed beat? Unrelated spikes sit between the beats (e.g. a burst
	// of PSO compiles at 70-78 s around GC spikes at 78 / 139 / 200 s), so consecutive gaps don't work. Instead,
	// try every spacing between two spikes as a period and walk the chain t, t+P, t+2P... (each within
	// tolerance of the previous beat). Longest chain wins (shorter period on ties); it counts when it has 3+
	// spikes and covers MIN_CHAIN_SHARE of all spikes, so chance chains in a noisy run don't qualify.
	// A period near 61 s is named as garbage collection, the engine's default purge interval.
	inline FPeriodicity DetectPeriodicSpikes(const TArray<double>& MergedTimes)
	{
		FPeriodicity P;
		const int32 N = FMath::Min(MergedTimes.Num(), MAX_SPIKES_SCANNED);
		if (N < 3) return P;
		int32 BestLen = 0; double BestPeriod = 0.0; double BestSpan = 0.0;
		for (int32 i = 0; i < N; ++i)
		{
			for (int32 j = i + 1; j < N; ++j)
			{
				const double Period = MergedTimes[j] - MergedTimes[i];
				if (Period < MIN_PERIOD_S) continue;
				int32 Len = 2; double Last = MergedTimes[j]; int32 K = j + 1;
				while (true)
				{
					const double Expected = Last + Period;
					while (K < N && MergedTimes[K] < Expected - PERIOD_TOLERANCE_S) ++K;
					if (K >= N || MergedTimes[K] > Expected + PERIOD_TOLERANCE_S) break;
					Last = MergedTimes[K]; ++Len; ++K;
				}
				if (Len > BestLen || (Len == BestLen && Period < BestPeriod))
				{
					BestLen = Len; BestPeriod = Period; BestSpan = Last - MergedTimes[i];
				}
			}
		}
		if (BestLen < 3 || BestLen < MIN_CHAIN_SHARE * N) return P;
		P.bPeriodic = true;
		P.IntervalS = BestSpan / (BestLen - 1); // average beat over the whole chain
		P.Count = BestLen;
		if (FMath::Abs(P.IntervalS - GC_INTERVAL_S) <= 3.0)
		{
			P.Likely = TEXT("garbage collection: matches gc.TimeBetweenPurgingPendingKillObjects (default 61.1 s)");
		}
		return P;
	}

	// Log lines that are boot noise, not evidence: config/plugin/module/font setup and CSV metadata. Used to
	// keep notable_lines about the run itself.
	inline bool IsBootNoise(const FString& Line)
	{
		static const TCHAR* Noise[] = {
			TEXT("LogConfig:"), TEXT("LogPluginManager:"), TEXT("LogInit:"), TEXT("LogModuleManager:"),
			TEXT("ini files took"), TEXT("Metadata set :"), TEXT("LogSlate: Using FreeType"), TEXT("LogFont"),
			TEXT("LogDevObjectVersion:"), TEXT("LogStudioTelemetry"), TEXT("LogAssetRegistry:"),
			TEXT("lazily loaded font"),
		};
		for (const TCHAR* N : Noise) if (Line.Contains(N)) return true;
		return false;
	}

	// The engine's log prefix is "[YYYY.MM.DD-HH.MM.SS:mmm][frame%1000]". Returns that frame tag, or -1 for a
	// line without one (early boot). The tag wraps every 1000 frames, so callers should treat every line after
	// the first non-zero tag as in-run, not test each line for != 0.
	inline int32 LogFrameTag(const FString& Line)
	{
		if (!Line.StartsWith(TEXT("["))) return -1;
		const int32 Second = Line.Find(TEXT("]["));
		if (Second == INDEX_NONE) return -1;
		const int32 Close = Line.Find(TEXT("]"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Second + 2);
		if (Close == INDEX_NONE) return -1;
		const FString FrameTag = Line.Mid(Second + 2, Close - Second - 2).TrimStartAndEnd();
		return FrameTag.IsNumeric() ? FCString::Atoi(*FrameTag) : -1;
	}
}

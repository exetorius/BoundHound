// Copyright (c) 2026 exetorius. Released under the MIT License.

#pragma once

#include "CoreMinimal.h"

// Pure, side-effect-free verdict + budget logic, factored out of UBoundHoundService::FrameTiming so it
// can be exercised headless by automation tests (no editor, no live frame globals). FrameTiming routes
// through these so the tests and the shipping method share one implementation and can never disagree.
namespace BoundHoundVerdict
{
	// Result of classifying which thread bounds the frame. Mirrors the fields FrameTiming emits.
	struct FVerdict
	{
		FString Bound;          // "GameThread" | "RenderThread" | "GPU"
		FString Confidence;     // "clear" | "moderate" | "marginal" | "none"
		double  TopMs = 0.0;    // the bottleneck thread's ms
		double  RunnerMs = 0.0; // the runner-up's ms
		FString RunnerName;     // runner-up thread name ("" when only one thread ranked)
		double  MarginMs = 0.0; // TopMs - RunnerMs
		bool    bContested = false; // top two within CONTESTED_PCT -> a genuine tie
	};

	// Top must lead the runner-up by more than this fraction of its own cost to be a clear win.
	static constexpr double CONTESTED_PCT = 0.10;

	// Rank game/render/GPU (GPU only when its timing is available), then judge how decisive the winner is.
	// RHI is deliberately NOT ranked here -- FrameTiming reports rhi_thread_ms but the verdict is a
	// game/render/GPU decision, matching the shipping behaviour. (Adding RHI to the verdict is issue #2.)
	inline FVerdict Classify(double GameMs, double RenderMs, double GpuMs, bool bGpuAvailable)
	{
		struct FThread { const TCHAR* Name; double Ms; };
		TArray<FThread, TInlineAllocator<3>> Threads;
		Threads.Add({ TEXT("GameThread"),   GameMs });
		Threads.Add({ TEXT("RenderThread"), RenderMs });
		if (bGpuAvailable) Threads.Add({ TEXT("GPU"), GpuMs });
		Threads.Sort([](const FThread& A, const FThread& B) { return A.Ms > B.Ms; });

		FVerdict V;
		V.Bound      = Threads[0].Name;
		V.TopMs      = Threads[0].Ms;
		V.RunnerMs   = Threads.Num() > 1 ? Threads[1].Ms : 0.0;
		V.RunnerName = Threads.Num() > 1 ? FString(Threads[1].Name) : FString();
		V.MarginMs   = V.TopMs - V.RunnerMs;

		V.bContested = V.TopMs > 0.0 && (V.MarginMs / V.TopMs) < CONTESTED_PCT;

		if (V.TopMs <= 0.0)      V.Confidence = TEXT("none");     // no meaningful timing yet
		else if (V.bContested)   V.Confidence = TEXT("marginal"); // a tie
		else if (!bGpuAvailable) V.Confidence = TEXT("moderate"); // CPU winner, GPU unknown
		else                     V.Confidence = TEXT("clear");
		return V;
	}

	// Coerce a caller-supplied target FPS to something sane (NaN/inf/<=0 -> 60).
	inline double SafeTargetFps(float TargetFPS)
	{
		return (FMath::IsFinite(TargetFPS) && TargetFPS > 0.0f) ? (double)TargetFPS : 60.0;
	}

	// A single thread finishes inside the per-frame budget iff its ms does not exceed it.
	inline bool IsOverBudget(double Ms, double BudgetMs)
	{
		return Ms > BudgetMs;
	}

	struct FBudget
	{
		double TargetFps = 60.0;
		double BudgetMs = 0.0;
		double FrameMs = 0.0;
		double FrameHeadroomMs = 0.0;
		bool   bMeetsTarget = false; // PASS iff the frame has real timing AND fits the budget
	};

	inline FBudget ComputeBudget(double FrameMs, float TargetFPS)
	{
		FBudget B;
		B.TargetFps        = SafeTargetFps(TargetFPS);
		B.BudgetMs         = 1000.0 / B.TargetFps;
		B.FrameMs          = FrameMs;
		B.FrameHeadroomMs  = B.BudgetMs - FrameMs;
		B.bMeetsTarget     = FrameMs > 0.0 && FrameMs <= B.BudgetMs;
		return B;
	}
}

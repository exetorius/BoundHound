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
		FString Bound;          // "GameThread" | "RenderThread" | "RHIThread" | "GPU"
		FString Confidence;     // "clear" | "moderate" | "marginal" | "none"
		double  TopMs = 0.0;    // the bottleneck thread's ms
		double  RunnerMs = 0.0; // the runner-up's ms
		FString RunnerName;     // runner-up thread name ("" when only one thread ranked)
		double  MarginMs = 0.0; // TopMs - RunnerMs
		bool    bContested = false; // top two within CONTESTED_PCT -> a genuine tie
	};

	// Top must lead the runner-up by more than this fraction of its own cost to be a clear win.
	static constexpr double CONTESTED_PCT = 0.10;

	// Rank game/render/RHI/GPU, then judge how decisive the winner is. RHI and GPU each count only when
	// their timing is available: GRHIThreadTime is 0 when RHI-threading is off (its cost then folds into the
	// render thread, so ranking it would double-count), and GPU timing isn't always present. RHI IS a real
	// distinct bottleneck -- draw-call submission can saturate it independently of render-thread scene setup
	// (issue #2). Note only GPU-unavailable downgrades confidence to "moderate": a missing GPU number hides a
	// possible bottleneck, whereas missing RHI just means its work was already counted on the render thread.
	inline FVerdict Classify(double GameMs, double RenderMs, double RhiMs, double GpuMs,
	                         bool bRhiAvailable, bool bGpuAvailable)
	{
		struct FThread { const TCHAR* Name; double Ms; };
		TArray<FThread, TInlineAllocator<4>> Threads;
		Threads.Add({ TEXT("GameThread"),   GameMs });
		Threads.Add({ TEXT("RenderThread"), RenderMs });
		if (bRhiAvailable) Threads.Add({ TEXT("RHIThread"), RhiMs });
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

	// Race-free self-validation for ForceHitch (issue #17). Given the peak per-thread ms the hitch
	// actually induced -- measured synchronously by ForceHitch itself, NOT read back via a follow-up
	// FrameTiming that races the stall on the game thread -- did the stall land on the thread(s) the
	// caller asked for? MinExpectedMs is the floor a thread's induced time must clear to count as hit
	// (set below the requested stall to tolerate scheduler slop). "gpu" is not validated here: its cost
	// is scene-dependent and can't be measured synchronously, so it keeps the read-a-following-frame path.
	// "rhi" self-validates only when RHI-threading is on (a distinct RHI thread exists to stall); the peak
	// must clear the floor AND dominate both CPU threads, or the stall folded into the render thread and
	// there's no RHIThread verdict to confirm (issue #2).
	inline bool HitchMatchesExpect(const FString& Mode, double PeakGameMs, double PeakRenderMs,
	                               double MinExpectedMs, double PeakRhiMs = 0.0)
	{
		const bool bGameHit   = PeakGameMs   >= MinExpectedMs;
		const bool bRenderHit = PeakRenderMs >= MinExpectedMs;
		const bool bRhiHit    = PeakRhiMs    >= MinExpectedMs;
		if (Mode == TEXT("game"))   return bGameHit   && PeakGameMs   > PeakRenderMs;
		if (Mode == TEXT("render")) return bRenderHit && PeakRenderMs > PeakGameMs;
		if (Mode == TEXT("both"))   return bGameHit   && bRenderHit;
		if (Mode == TEXT("rhi"))    return bRhiHit    && PeakRhiMs > PeakRenderMs && PeakRhiMs > PeakGameMs;
		return false; // gpu / unknown -- not self-validated by induced CPU peaks
	}

	// Issue #27: FrameTiming reads the most recently completed frame's globals. While a caller (e.g. one
	// Python call) holds the game thread, no new frame completes, so back-to-back reads return the SAME
	// frame. A read is a repeat iff a previous read exists and the engine frame counter hasn't advanced.
	inline bool IsRepeatRead(bool bHasPrevious, uint64 FrameNumber, uint64 PreviousFrameNumber)
	{
		return bHasPrevious && FrameNumber == PreviousFrameNumber;
	}

	// Issue #23: PIE's first seconds are dominated by shader/PSO compiles and streaming, so a single frame
	// read then is not representative. Settled once the PIE world has run SETTLE_SECONDS of real time.
	static constexpr double SETTLE_SECONDS = 10.0;
	inline bool IsWarming(double PieRealSeconds)
	{
		return PieRealSeconds >= 0.0 && PieRealSeconds < SETTLE_SECONDS;
	}

	// Issue #23: describe Ms relative to RefMs as a human ratio. "~0x" for 13 vs 70.6 was unreadable; this
	// says "5.4x faster than" / "2.0x slower than" / "about the same as" (within 10%). Empty when either
	// value is missing, so the caller can drop the comparison entirely.
	inline FString DescribeRatio(double Ms, double RefMs)
	{
		if (Ms <= 0.0 || RefMs <= 0.0) return FString();
		const double R = Ms / RefMs;
		if (R >= 0.9 && R <= 1.1) return TEXT("about the same as");
		return R < 1.0
			? FString::Printf(TEXT("%.1f× faster than"), 1.0 / R)
			: FString::Printf(TEXT("%.1f× slower than"), R);
	}

	// ---- Trace-derived verdict (issue #22) -------------------------------------------------------------
	// A standalone run can't be read live, so its verdict comes from the per-frame thread times the CSV
	// profiler writes into the trace. One sample = one frame's four thread times (ms; 0 = not recorded).
	struct FThreadSample
	{
		double GameMs = 0.0, RenderMs = 0.0, RhiMs = 0.0, GpuMs = 0.0;
	};

	// Nearest-rank percentile (P in [0,1]) of an unsorted array; 0 for an empty array.
	inline double Percentile(TArray<double> Values, double P)
	{
		if (Values.IsEmpty()) return 0.0;
		Values.Sort();
		const int32 Idx = FMath::Clamp((int32)FMath::CeilToDouble(P * Values.Num()) - 1, 0, Values.Num() - 1);
		return Values[Idx];
	}

	struct FThreadStats { double AvgMs = 0.0, MedianMs = 0.0, P95Ms = 0.0; };

	inline FThreadStats SummariseThread(const TArray<double>& Ms)
	{
		FThreadStats S;
		if (Ms.IsEmpty()) return S;
		double Sum = 0.0; for (double V : Ms) Sum += V;
		S.AvgMs    = Sum / Ms.Num();
		S.MedianMs = Percentile(Ms, 0.5);
		S.P95Ms    = Percentile(Ms, 0.95);
		return S;
	}

	struct FTraceVerdict
	{
		FVerdict Verdict;            // from the per-thread MEDIANS (robust to load spikes)
		FThreadStats Game, Render, Rhi, Gpu;
		bool bRhiAvailable = false, bGpuAvailable = false;
		int32 SampleCount = 0;
		// Share of frames where each thread was the slowest -- shows whether the bottleneck alternates.
		double ShareGame = 0.0, ShareRender = 0.0, ShareRhi = 0.0, ShareGpu = 0.0;
		double WinnerShare = 0.0;    // share of frames the median verdict's thread actually won
	};

	// Parse the per-frame thread times out of a CSV-profiler capture file. The engine writes GameThreadTime /
	// RenderThreadTime / RHIThreadTime / GPUTime every frame (UpdateCoreCsvStats_EndFrame). They have to come
	// from the CSV file: those stats are recorded with CSV_CUSTOM_STAT_MINIMAL_GLOBAL, which never sends the
	// stat's name to Unreal Insights, so in a .utrace they're only unnamed "[unknownN]" counters.
	// The CSV grows columns during a capture: stats first seen mid-run are appended as new columns, and the
	// full header is only written at the end (lost when the process is killed). Existing columns never move,
	// so the first-row header's indices stay valid. Rows shorter than that header are skipped (metadata rows),
	// as are non-numeric cells (a repeated header) and an unterminated last line (cut short by the kill).
	struct FCsvThreadColumns { bool bGame = false, bRender = false, bRhi = false, bGpu = false; };

	inline bool ParseCsvThreadSamples(const FString& Content, TArray<FThreadSample>& Out, FCsvThreadColumns& Cols)
	{
		Out.Reset();
		Cols = FCsvThreadColumns();
		TArray<FString> Lines;
		Content.ParseIntoArrayLines(Lines);
		if (Lines.Num() < 2) return false;

		TArray<FString> Header;
		Lines[0].ParseIntoArray(Header, TEXT(","), false);
		const int32 IGame   = Header.IndexOfByKey(TEXT("GameThreadTime"));
		const int32 IRender = Header.IndexOfByKey(TEXT("RenderThreadTime"));
		const int32 IRhi    = Header.IndexOfByKey(TEXT("RHIThreadTime"));
		const int32 IGpu    = Header.IndexOfByKey(TEXT("GPUTime"));
		Cols.bGame = IGame != INDEX_NONE; Cols.bRender = IRender != INDEX_NONE;
		Cols.bRhi = IRhi != INDEX_NONE;   Cols.bGpu = IGpu != INDEX_NONE;
		if (!Cols.bGame || !Cols.bRender) return false; // can't rank without both CPU threads

		auto Read = [](const TArray<FString>& Row, int32 Idx, double& OutMs) -> bool
		{
			if (Idx == INDEX_NONE) { OutMs = 0.0; return true; }
			const FString& Cell = Row[Idx];
			if (Cell.IsEmpty() || !Cell.IsNumeric()) return false;
			OutMs = FCString::Atod(*Cell);
			return FMath::IsFinite(OutMs) && OutMs >= 0.0;
		};

		const bool bLastLineComplete = Content.EndsWith(TEXT("\n"));
		const int32 LastLine = bLastLineComplete ? Lines.Num() : Lines.Num() - 1;
		TArray<FString> Row;
		for (int32 L = 1; L < LastLine; ++L)
		{
			Lines[L].ParseIntoArray(Row, TEXT(","), false);
			if (Row.Num() < Header.Num()) continue; // metadata row
			FThreadSample S;
			if (Read(Row, IGame, S.GameMs) && Read(Row, IRender, S.RenderMs) && Read(Row, IRhi, S.RhiMs) && Read(Row, IGpu, S.GpuMs))
			{
				Out.Add(S);
			}
		}
		return Out.Num() > 0;
	}

	// If the median winner is the slowest thread in fewer than this share of frames, the bottleneck
	// alternates between threads, so the verdict is downgraded to "marginal".
	static constexpr double MIN_WINNER_SHARE = 0.5;

	inline FTraceVerdict ClassifyTrace(const TArray<FThreadSample>& Samples)
	{
		FTraceVerdict T;
		T.SampleCount = Samples.Num();
		if (Samples.IsEmpty())
		{
			T.Verdict.Confidence = TEXT("none");
			return T;
		}

		TArray<double> G, R, Rhi, Gpu;
		for (const FThreadSample& S : Samples) { G.Add(S.GameMs); R.Add(S.RenderMs); Rhi.Add(S.RhiMs); Gpu.Add(S.GpuMs); }
		T.Game = SummariseThread(G); T.Render = SummariseThread(R); T.Rhi = SummariseThread(Rhi); T.Gpu = SummariseThread(Gpu);
		T.bRhiAvailable = T.Rhi.MedianMs > 0.0;
		T.bGpuAvailable = T.Gpu.MedianMs > 0.0;

		T.Verdict = Classify(T.Game.MedianMs, T.Render.MedianMs, T.Rhi.MedianMs, T.Gpu.MedianMs, T.bRhiAvailable, T.bGpuAvailable);

		int32 WG = 0, WR = 0, WRhi = 0, WGpu = 0;
		for (const FThreadSample& S : Samples)
		{
			const FString Win = Classify(S.GameMs, S.RenderMs, S.RhiMs, S.GpuMs, T.bRhiAvailable, T.bGpuAvailable).Bound;
			if      (Win == TEXT("GameThread"))   ++WG;
			else if (Win == TEXT("RenderThread")) ++WR;
			else if (Win == TEXT("RHIThread"))    ++WRhi;
			else if (Win == TEXT("GPU"))          ++WGpu;
		}
		const double N = (double)Samples.Num();
		T.ShareGame = WG / N; T.ShareRender = WR / N; T.ShareRhi = WRhi / N; T.ShareGpu = WGpu / N;
		const FString& B = T.Verdict.Bound;
		T.WinnerShare = B == TEXT("GameThread") ? T.ShareGame : B == TEXT("RenderThread") ? T.ShareRender
		              : B == TEXT("RHIThread") ? T.ShareRhi : T.ShareGpu;

		if (T.Verdict.Confidence != TEXT("none") && T.WinnerShare < MIN_WINNER_SHARE)
		{
			T.Verdict.Confidence = TEXT("marginal");
		}
		return T;
	}

	// Issue #26: display arguments for a standalone run, so runs can be compared at a fixed resolution instead
	// of inheriting the editor's Play window size. Our args go before the editor's on the command line, and
	// the engine takes the first ResX=/ResY= it finds; the editor only adds -windowed if neither -windowed
	// nor -fullscreen is already there. One side given -> the other follows at 16:9 (the engine's rule).
	struct FDisplayArgs
	{
		FString Args;        // appended to the standalone command line ("" = inherit the editor's settings)
		int32 ResX = 0, ResY = 0;
		FString Mode;        // "fullscreen" | "windowed" | "" (inherited)
		FString Error;       // non-empty = reject the request
	};

	static constexpr int32 MIN_RES = 320;
	static constexpr int32 MAX_RES = 16384;

	inline FDisplayArgs BuildDisplayArgs(int32 ResX, int32 ResY, bool bFullscreen)
	{
		FDisplayArgs D;
		if (ResX < 0 || ResY < 0)
		{
			D.Error = TEXT("res_x / res_y must be 0 (inherit) or a positive size.");
			return D;
		}
		if (ResX > 0 && ResY == 0) ResY = FMath::RoundToInt32(ResX * 9.0 / 16.0);
		if (ResY > 0 && ResX == 0) ResX = FMath::RoundToInt32(ResY * 16.0 / 9.0);
		if (ResX > 0 && (ResX < MIN_RES || ResY < MIN_RES || ResX > MAX_RES || ResY > MAX_RES))
		{
			D.Error = FString::Printf(TEXT("Resolution %dx%d is outside %d..%d."), ResX, ResY, MIN_RES, MAX_RES);
			return D;
		}
		D.ResX = ResX; D.ResY = ResY;
		if (ResX > 0) D.Args += FString::Printf(TEXT(" -ResX=%d -ResY=%d"), ResX, ResY);
		if (bFullscreen)       { D.Args += TEXT(" -fullscreen"); D.Mode = TEXT("fullscreen"); }
		else if (ResX > 0)     { D.Args += TEXT(" -windowed");   D.Mode = TEXT("windowed"); }
		return D;
	}

	// Issue #26: read the resolution and window mode a run actually used from its command line (the
	// "LogInit: Command Line:" log line). First ResX=/ResY= wins, as in the engine.
	inline bool ParseRunDisplay(const FString& CommandLine, int32& OutResX, int32& OutResY, FString& OutMode)
	{
		OutResX = OutResY = 0; OutMode.Reset();
		auto ValueAfter = [&](const TCHAR* Key) -> int32
		{
			const int32 At = CommandLine.Find(Key, ESearchCase::IgnoreCase);
			return At == INDEX_NONE ? 0 : FCString::Atoi(*CommandLine + At + FCString::Strlen(Key));
		};
		OutResX = ValueAfter(TEXT("-ResX="));
		OutResY = ValueAfter(TEXT("-ResY="));
		const bool bWindowed = CommandLine.Contains(TEXT("-windowed"), ESearchCase::IgnoreCase);
		const bool bFull     = CommandLine.Contains(TEXT("-fullscreen"), ESearchCase::IgnoreCase);
		OutMode = bWindowed ? TEXT("windowed") : (bFull ? TEXT("fullscreen") : TEXT(""));
		return OutResX > 0 || OutResY > 0 || !OutMode.IsEmpty();
	}

	// Issue #28: starting a play session while another is running, queued or still ending queues a
	// RequestPlaySession on top of the teardown and crashes the editor (TOptional IsSet assert). Returns
	// why the editor is busy, or empty when it's safe to start a new session. Pure so it's testable.
	inline FString PlaySessionBusyReason(bool bEndQueued, bool bRequestQueued, bool bPlayWorld, bool bSessionInProgress)
	{
		if (bEndQueued)         return TEXT("a play session is still ending (end-play queued for next tick)");
		if (bRequestQueued)     return TEXT("a play session request is already queued for next tick");
		if (bPlayWorld)         return TEXT("PIE is running");
		if (bSessionInProgress) return TEXT("a play session is in progress");
		return FString();
	}
}

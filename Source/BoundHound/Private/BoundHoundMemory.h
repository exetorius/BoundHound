// Copyright (c) 2026 exetorius. Released under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "BoundHoundVerdict.h" // Percentile

// Memory / VRAM pressure (issue #8). The CPU-vs-GPU verdict covers compute; a frame can also stutter because
// VRAM is over budget (resources demoted to system memory), the texture streaming pool can't hold what the
// view wants, or the machine is paging. Pure rules over a reading, so they're unit-testable.
namespace BoundHoundMemory
{
	struct FMemReading
	{
		// System RAM (whole machine) and this process.
		double TotalPhysicalMB = 0.0, AvailablePhysicalMB = 0.0;
		double ProcessUsedMB = 0.0, ProcessPeakMB = 0.0;
		// VRAM, from the RHI's OS/driver budget (DXGI on D3D12). Not every RHI reports it.
		bool   bVramValid = false;
		double VramBudgetMB = 0.0, VramUsedMB = 0.0, VramDemotedMB = 0.0;
		// Texture streaming pool.
		bool   bStreamingValid = false;
		double PoolMB = 0.0, RequiredPoolMB = 0.0, OverBudgetMB = 0.0;
	};

	struct FPressure
	{
		FString Tag;       // vram_over_budget | vram_near_budget | streaming_over_budget | system_memory_low
		FString Severity;  // "warn" | "critical"
		FString Why;
		FString Lever;
	};

	static constexpr double VRAM_NEAR_SHARE = 0.90;      // used / budget at or above this = near the budget
	static constexpr double SYS_LOW_SHARE = 0.10;        // available below this share of RAM (or 1 GB) = low
	static constexpr double SYS_LOW_MB = 1024.0;
	static constexpr double SYS_CRITICAL_SHARE = 0.05;   // ...below this share (or 512 MB) = paging likely
	static constexpr double SYS_CRITICAL_MB = 512.0;

	inline TArray<FPressure> ClassifyMemory(const FMemReading& R)
	{
		TArray<FPressure> Out;
		if (R.bVramValid && R.VramDemotedMB > 0.0)
		{
			Out.Add({ TEXT("vram_over_budget"), TEXT("critical"),
				FString::Printf(TEXT("VRAM is over budget: %.0f MB of GPU resources were demoted to system memory, so the GPU reads them over PCIe (stalls, hitches)."), R.VramDemotedMB),
				TEXT("Cut VRAM: lower texture resolution / r.Streaming.PoolSize, shadow resolution (r.Shadow.Virtual.MaxPhysicalPages), screen percentage, or Nanite / Lumen quality. Check what's resident with 'stat RHI' and 'memreport -full'.") });
		}
		else if (R.bVramValid && R.VramBudgetMB > 0.0 && R.VramUsedMB >= VRAM_NEAR_SHARE * R.VramBudgetMB)
		{
			Out.Add({ TEXT("vram_near_budget"), TEXT("warn"),
				FString::Printf(TEXT("VRAM is at %.0f%% of its budget (%.0f of %.0f MB): more content, a higher resolution or another app will push it over."),
					R.VramUsedMB / R.VramBudgetMB * 100.0, R.VramUsedMB, R.VramBudgetMB),
				TEXT("Leave headroom for the target GPU: check 'stat RHI' for the biggest pools and trim texture / render-target / shadow memory.") });
		}
		if (R.bStreamingValid && R.OverBudgetMB > 0.0)
		{
			Out.Add({ TEXT("streaming_over_budget"), TEXT("warn"),
				FString::Printf(TEXT("The texture streaming pool is %.0f MB over budget (wants %.0f MB, pool %.0f MB): textures render at lower mips (blurry) and mips stream in and out."),
					R.OverBudgetMB, R.RequiredPoolMB, R.PoolMB),
				TEXT("Raise r.Streaming.PoolSize if VRAM allows, or cut texture resolution (LOD bias / max texture size per group). 'stat streaming' shows the pool.") });
		}
		if (R.TotalPhysicalMB > 0.0 && R.AvailablePhysicalMB > 0.0)
		{
			const bool bCritical = R.AvailablePhysicalMB < FMath::Max(SYS_CRITICAL_MB, SYS_CRITICAL_SHARE * R.TotalPhysicalMB);
			const bool bLow = R.AvailablePhysicalMB < FMath::Max(SYS_LOW_MB, SYS_LOW_SHARE * R.TotalPhysicalMB);
			if (bLow)
			{
				Out.Add({ TEXT("system_memory_low"), bCritical ? TEXT("critical") : TEXT("warn"),
					FString::Printf(TEXT("Only %.0f MB of %.0f MB system RAM is free: the OS pages memory to disk, which shows up as long, irregular hitches."),
						R.AvailablePhysicalMB, R.TotalPhysicalMB),
					TEXT("Close other apps for a fair measurement; then reduce the game's footprint ('memreport -full', LLM: -llm) or test on a machine with the target RAM.") });
			}
		}
		return Out;
	}

	inline FString OverallPressure(const TArray<FPressure>& P)
	{
		FString Out = TEXT("none");
		for (const FPressure& X : P)
		{
			if (X.Severity == TEXT("critical")) return TEXT("critical");
			Out = TEXT("warn");
		}
		return Out;
	}

	// A memory column over a captured run (CSV PhysicalUsedMB / MemoryFreeMB). Growth compares early-run (25-35%
	// of the way in, past the load) with the last 10%: steady growth through gameplay points at a leak or an
	// unbounded cache. Reported as a fact; the cause needs memreport / LLM.
	struct FSeries
	{
		bool bValid = false;
		double First = 0.0, Last = 0.0, Peak = 0.0, Min = 0.0, Median = 0.0;
		double EarlyMB = 0.0, LateMB = 0.0, GrowthMB = 0.0;
	};

	static constexpr int32 MIN_SERIES = 50;
	static constexpr double GROWTH_FLAG_MB = 200.0;    // growth this big (and...)
	static constexpr double GROWTH_FLAG_SHARE = 0.05;  // ...this share of the early level) is worth a look

	inline FSeries SummariseSeries(const TArray<double>& V)
	{
		FSeries S;
		if (V.Num() < MIN_SERIES) return S;
		S.bValid = true;
		S.First = V[0]; S.Last = V.Last();
		S.Peak = V[0]; S.Min = V[0];
		for (double X : V) { S.Peak = FMath::Max(S.Peak, X); S.Min = FMath::Min(S.Min, X); }
		S.Median = BoundHoundVerdict::Percentile(V, 0.5);
		const int32 N = V.Num();
		auto MedianOf = [&](int32 From, int32 To) { return BoundHoundVerdict::Percentile(TArray<double>(V.GetData() + From, FMath::Max(1, To - From)), 0.5); };
		S.EarlyMB = MedianOf(N * 25 / 100, N * 35 / 100);
		S.LateMB = MedianOf(N * 90 / 100, N);
		S.GrowthMB = S.LateMB - S.EarlyMB;
		return S;
	}

	inline bool IsNotableGrowth(const FSeries& S)
	{
		return S.bValid && S.GrowthMB >= FMath::Max(GROWTH_FLAG_MB, GROWTH_FLAG_SHARE * S.EarlyMB);
	}

	// Several numeric columns of a CSV-profiler file in one pass, with the same row rules as
	// ParseCsvThreadSamples (rows shorter than the first header, non-numeric cells and an unterminated last
	// line are skipped). Columns missing from the header come back empty.
	inline TMap<FString, TArray<double>> ParseCsvColumns(const FString& Content, const TArray<FString>& Names)
	{
		TMap<FString, TArray<double>> Out;
		TArray<FString> Lines;
		Content.ParseIntoArrayLines(Lines);
		if (Lines.Num() < 2) return Out;
		TArray<FString> Header;
		Lines[0].ParseIntoArray(Header, TEXT(","), false);
		TArray<TPair<FString, int32>> Cols;
		for (const FString& N : Names)
		{
			const int32 I = Header.IndexOfByKey(N);
			if (I != INDEX_NONE) { Cols.Add({ N, I }); Out.Add(N); }
		}
		if (Cols.IsEmpty()) return Out;
		const int32 LastLine = Content.EndsWith(TEXT("\n")) ? Lines.Num() : Lines.Num() - 1;
		TArray<FString> Row;
		for (int32 L = 1; L < LastLine; ++L)
		{
			Lines[L].ParseIntoArray(Row, TEXT(","), false);
			if (Row.Num() < Header.Num()) continue;
			for (const TPair<FString, int32>& C : Cols)
			{
				const FString& Cell = Row[C.Value];
				if (!Cell.IsEmpty() && Cell.IsNumeric()) Out[C.Key].Add(FCString::Atod(*Cell));
			}
		}
		return Out;
	}
}

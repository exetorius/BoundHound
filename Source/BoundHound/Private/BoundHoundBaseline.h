// Copyright (c) 2026 exetorius. Released under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// Baseline + diff (issue #6). A baseline is a small, self-contained snapshot of one run: its numbers and
// what it ran on (engine, platform, config, GPU, resolution, map, frame cap). Comparing two snapshots gives
// FACTUAL deltas only: "GPU median +1.3 ms (+13%)". It never says why something changed. And it can't know
// the two runs did the same work: the scenario label is the caller's claim, reported as such, not checked.
// Pure, so the rules are unit-testable.
namespace BoundHoundBaseline
{
	struct FSnapshot
	{
		FString Name;
		FString Kind;                      // "capture" (a standalone run's analysis) | "live" (one FrameTiming frame)
		FString Scenario;                  // caller-asserted label of the workload ("arena wave 3, 2 min")
		FString Created;                   // ISO 8601
		FString Source;                    // trace / log the numbers came from
		FString Bound;                     // the verdict at the time
		TMap<FString, FString> Identity;   // what the run ran on
		TMap<FString, double> Metrics;     // see MetricSpecs()
	};

	struct FMetricSpec { const TCHAR* Key; const TCHAR* Label; bool bCount; };

	// Every metric a snapshot may carry, in display order. A snapshot only holds the ones its kind measures.
	inline TConstArrayView<FMetricSpec> MetricSpecs()
	{
		static const FMetricSpec Specs[] = {
			// capture
			{ TEXT("frame_avg_ms"),      TEXT("average frame"),        false },
			{ TEXT("frame_p95_ms"),      TEXT("p95 frame"),            false },
			{ TEXT("worst_frame_ms"),    TEXT("worst frame"),          false },
			{ TEXT("game_median_ms"),    TEXT("game thread median"),   false },
			{ TEXT("game_p95_ms"),       TEXT("game thread p95"),      false },
			{ TEXT("render_median_ms"),  TEXT("render thread median"), false },
			{ TEXT("render_p95_ms"),     TEXT("render thread p95"),    false },
			{ TEXT("rhi_median_ms"),     TEXT("RHI thread median"),    false },
			{ TEXT("rhi_p95_ms"),        TEXT("RHI thread p95"),       false },
			{ TEXT("gpu_median_ms"),     TEXT("GPU median"),           false },
			{ TEXT("gpu_p95_ms"),        TEXT("GPU p95"),              false },
			{ TEXT("spike_events"),      TEXT("spike events"),         true  },
			{ TEXT("pso_hitches"),       TEXT("PSO hitches"),          true  },
			{ TEXT("log_errors"),        TEXT("log errors"),           true  },
			// live
			{ TEXT("frame_ms"),          TEXT("frame"),                false },
			{ TEXT("game_ms"),           TEXT("game thread"),          false },
			{ TEXT("render_ms"),         TEXT("render thread"),        false },
			{ TEXT("rhi_ms"),            TEXT("RHI thread"),           false },
			{ TEXT("gpu_ms"),            TEXT("GPU"),                  false },
		};
		return Specs;
	}

	// Identity fields. A HARD mismatch means the numbers measure different things (another engine build,
	// platform, build config or project): no deltas unless forced. A SOFT mismatch still compares, with a
	// warning: a different GPU, resolution, map or frame cap changes the numbers for reasons that aren't
	// the change being measured.
	inline bool IsHardField(const FString& Field)
	{
		return Field == TEXT("kind") || Field == TEXT("engineversion") || Field == TEXT("platform")
			|| Field == TEXT("config") || Field == TEXT("project");
	}
	inline TArray<FString> IdentityFields()
	{
		return { TEXT("kind"), TEXT("project"), TEXT("engineversion"), TEXT("platform"), TEXT("config"),
		         TEXT("rhiname"), TEXT("gpu"), TEXT("cpu"), TEXT("deviceprofile"), TEXT("resolution"),
		         TEXT("map"), TEXT("frame_cap"), TEXT("scenario") };
	}

	// How big a change must be to call it a change rather than run-to-run noise. A capture's medians over
	// thousands of frames are stable; a single live frame is not.
	static constexpr double MIN_DELTA_MS = 0.1;
	static constexpr double MIN_DELTA_SHARE_CAPTURE = 0.03;
	static constexpr double MIN_DELTA_SHARE_LIVE = 0.10;

	struct FMismatch { FString Field, Baseline, Current; bool bHard = false; };
	struct FDelta
	{
		FString Key, Label;
		bool bCount = false;
		double Baseline = 0.0, Current = 0.0, Delta = 0.0, DeltaPct = 0.0; // pct = 0 when the baseline is 0
		bool bChanged = false;
		FString Statement;  // "GPU median 9.75 -> 11.02 ms (+1.27 ms, +13.0%)"
	};
	struct FComparison
	{
		TArray<FMismatch> Mismatches;
		bool bHardMismatch = false;
		TArray<FDelta> Deltas;           // empty when a hard mismatch blocks the comparison
		TArray<FString> Warnings;
		bool bBoundChanged = false;
	};

	inline FString FormatMs(double V) { return FString::Printf(TEXT("%.2f"), V); }

	inline FComparison Compare(const FSnapshot& Base, const FSnapshot& Cur, bool bForce)
	{
		FComparison C;
		auto IdOf = [](const FSnapshot& S, const FString& F) -> FString
		{
			if (F == TEXT("kind"))     return S.Kind;
			if (F == TEXT("scenario")) return S.Scenario;
			const FString* V = S.Identity.Find(F);
			return V ? *V : FString();
		};
		for (const FString& F : IdentityFields())
		{
			const FString A = IdOf(Base, F), B = IdOf(Cur, F);
			if (A.IsEmpty() || B.IsEmpty() || A.Equals(B, ESearchCase::IgnoreCase)) continue; // unknown on one side: can't tell
			FMismatch M{ F, A, B, IsHardField(F) };
			C.bHardMismatch |= M.bHard;
			C.Mismatches.Add(M);
		}
		if (C.bHardMismatch && !bForce) return C;

		const double Share = Base.Kind == TEXT("live") ? MIN_DELTA_SHARE_LIVE : MIN_DELTA_SHARE_CAPTURE;
		for (const FMetricSpec& Spec : MetricSpecs())
		{
			const double* A = Base.Metrics.Find(Spec.Key);
			const double* B = Cur.Metrics.Find(Spec.Key);
			if (!A || !B) continue;
			FDelta D;
			D.Key = Spec.Key; D.Label = Spec.Label; D.bCount = Spec.bCount;
			D.Baseline = *A; D.Current = *B; D.Delta = *B - *A;
			D.DeltaPct = *A != 0.0 ? D.Delta / *A * 100.0 : 0.0;
			D.bChanged = Spec.bCount
				? FMath::RoundToInt64(*A) != FMath::RoundToInt64(*B)
				: FMath::Abs(D.Delta) >= FMath::Max(MIN_DELTA_MS, Share * FMath::Abs(*A));
			if (Spec.bCount)
			{
				D.Statement = FString::Printf(TEXT("%s %lld -> %lld (%+lld)"), Spec.Label,
					FMath::RoundToInt64(*A), FMath::RoundToInt64(*B), FMath::RoundToInt64(D.Delta));
			}
			else
			{
				D.Statement = FString::Printf(TEXT("%s %s -> %s ms (%+.2f ms%s)"), Spec.Label, *FormatMs(*A), *FormatMs(*B), D.Delta,
					*A != 0.0 ? *FString::Printf(TEXT(", %+.1f%%"), D.DeltaPct) : TEXT(""));
			}
			C.Deltas.Add(D);
		}

		C.bBoundChanged = !Base.Bound.IsEmpty() && !Cur.Bound.IsEmpty() && Base.Bound != Cur.Bound;
		for (const FMismatch& M : C.Mismatches)
		{
			if (M.Field == TEXT("frame_cap"))
				C.Warnings.Add(TEXT("The frame cap differs: frame-time deltas (average / p95) reflect the cap, compare the thread medians instead."));
			else if (M.Field == TEXT("scenario"))
				C.Warnings.Add(TEXT("The scenario labels differ: these runs may not have done the same work."));
			else if (M.bHard)
				C.Warnings.Add(FString::Printf(TEXT("Forced past a %s mismatch: the deltas compare different builds or setups."), *M.Field));
			else
				C.Warnings.Add(FString::Printf(TEXT("%s differs (%s vs %s): it changes the numbers on its own."), *M.Field, *M.Baseline, *M.Current));
		}
		const FString* BaseCap = Base.Identity.Find(TEXT("frame_cap"));
		if (BaseCap && *BaseCap != TEXT("uncapped") && !C.Mismatches.ContainsByPredicate([](const FMismatch& M) { return M.Field == TEXT("frame_cap"); }))
		{
			C.Warnings.Add(FString::Printf(TEXT("Both runs are capped at %s fps: average / p95 frame measure the cap; the thread medians measure the work."), **BaseCap));
		}
		if (Base.Kind == TEXT("live"))
		{
			C.Warnings.Add(TEXT("Live baselines are single frames: expect noise. A standalone capture (kind 'capture') gives medians over the whole run."));
		}
		return C;
	}

	// ---- reading a run's identity from its log ---------------------------------------------------------

	// "LogCsvProfiler: Display: Metadata set : gpu="NVIDIA GeForce RTX 4070 Ti"" -> ("gpu", "NVIDIA ...").
	inline bool ParseMetadataLine(const FString& Line, FString& OutKey, FString& OutValue)
	{
		static const TCHAR* Marker = TEXT("Metadata set : ");
		const int32 At = Line.Find(Marker);
		if (At == INDEX_NONE) return false;
		const FString Rest = Line.Mid(At + FCString::Strlen(Marker));
		FString Value;
		if (!Rest.Split(TEXT("="), &OutKey, &Value)) return false;
		OutKey.TrimStartAndEndInline();
		Value.TrimStartAndEndInline();
		if (Value.Len() >= 2 && Value.StartsWith(TEXT("\"")) && Value.EndsWith(TEXT("\""))) Value = Value.Mid(1, Value.Len() - 2);
		OutValue = Value.TrimStartAndEnd();
		return !OutKey.IsEmpty();
	}

	// The map and project a run was launched with, from its command line ("... Corvus.uproject" /Game/Maps/X -game).
	inline FString MapFromCommandLine(const FString& Cmd)
	{
		const int32 At = Cmd.Find(TEXT("/Game/"));
		if (At == INDEX_NONE) return FString();
		int32 End = At;
		while (End < Cmd.Len() && !FChar::IsWhitespace(Cmd[End]) && Cmd[End] != TEXT('"')) ++End;
		return Cmd.Mid(At, End - At);
	}
	inline FString ProjectFromCommandLine(const FString& Cmd)
	{
		const int32 Ext = Cmd.Find(TEXT(".uproject"), ESearchCase::IgnoreCase);
		if (Ext == INDEX_NONE) return FString();
		int32 Start = Ext;
		while (Start > 0 && Cmd[Start - 1] != TEXT('/') && Cmd[Start - 1] != TEXT('\\') && Cmd[Start - 1] != TEXT('"')) --Start;
		return Cmd.Mid(Start, Ext - Start);
	}

	// Turn a log's metadata (key -> value, last value wins) into snapshot identity fields.
	inline TMap<FString, FString> IdentityFromMetadata(const TMap<FString, FString>& Meta)
	{
		TMap<FString, FString> Id;
		for (const TCHAR* K : { TEXT("engineversion"), TEXT("platform"), TEXT("config"), TEXT("rhiname"), TEXT("gpu"), TEXT("cpu"), TEXT("deviceprofile") })
		{
			if (const FString* V = Meta.Find(K)) Id.Add(K, *V);
		}
		const FString* X = Meta.Find(TEXT("systemresolution.resx"));
		const FString* Y = Meta.Find(TEXT("systemresolution.resy"));
		if (X && Y) Id.Add(TEXT("resolution"), *X + TEXT("x") + *Y);
		if (const FString* Cmd = Meta.Find(TEXT("commandline")))
		{
			const FString Map = MapFromCommandLine(*Cmd);
			const FString Project = ProjectFromCommandLine(*Cmd);
			if (!Map.IsEmpty()) Id.Add(TEXT("map"), Map);
			if (!Project.IsEmpty()) Id.Add(TEXT("project"), Project);
		}
		return Id;
	}

	// Baseline names become file names: letters, digits, '-', '_' and '.' only.
	inline FString SanitiseName(const FString& In)
	{
		FString Out;
		for (TCHAR Ch : In.TrimStartAndEnd()) Out.AppendChar(FChar::IsAlnum(Ch) || Ch == TEXT('-') || Ch == TEXT('_') || Ch == TEXT('.') ? Ch : TEXT('_'));
		while (Out.StartsWith(TEXT("."))) Out.RemoveAt(0);
		return Out.IsEmpty() ? FString(TEXT("default")) : Out.Left(80);
	}

	// ---- JSON ------------------------------------------------------------------------------------------

	inline TSharedRef<FJsonObject> ToJson(const FSnapshot& S)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("format"), 1);
		O->SetStringField(TEXT("name"), S.Name);
		O->SetStringField(TEXT("kind"), S.Kind);
		O->SetStringField(TEXT("scenario"), S.Scenario);
		O->SetStringField(TEXT("created"), S.Created);
		O->SetStringField(TEXT("source"), S.Source);
		O->SetStringField(TEXT("bound"), S.Bound);
		TSharedRef<FJsonObject> Id = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& KV : S.Identity) Id->SetStringField(KV.Key, KV.Value);
		O->SetObjectField(TEXT("identity"), Id);
		TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
		for (const FMetricSpec& Spec : MetricSpecs())
		{
			if (const double* V = S.Metrics.Find(Spec.Key)) M->SetNumberField(Spec.Key, FMath::RoundToDouble(*V * 1000.0) / 1000.0);
		}
		O->SetObjectField(TEXT("metrics"), M);
		return O;
	}

	inline bool FromJson(const TSharedPtr<FJsonObject>& O, FSnapshot& Out)
	{
		if (!O.IsValid() || !O->HasField(TEXT("metrics"))) return false;
		Out = FSnapshot();
		O->TryGetStringField(TEXT("name"), Out.Name);
		O->TryGetStringField(TEXT("kind"), Out.Kind);
		O->TryGetStringField(TEXT("scenario"), Out.Scenario);
		O->TryGetStringField(TEXT("created"), Out.Created);
		O->TryGetStringField(TEXT("source"), Out.Source);
		O->TryGetStringField(TEXT("bound"), Out.Bound);
		const TSharedPtr<FJsonObject>* Id = nullptr;
		if (O->TryGetObjectField(TEXT("identity"), Id) && Id)
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& KV : (*Id)->Values) Out.Identity.Add(KV.Key, KV.Value->AsString());
		}
		const TSharedPtr<FJsonObject>* M = nullptr;
		if (O->TryGetObjectField(TEXT("metrics"), M) && M)
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& KV : (*M)->Values) Out.Metrics.Add(KV.Key, KV.Value->AsNumber());
		}
		return true;
	}
}

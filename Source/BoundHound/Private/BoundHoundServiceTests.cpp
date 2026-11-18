// Copyright (c) 2026 exetorius. Released under the MIT License.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "BoundHoundVerdict.h"
#include "BoundHoundService.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"

// Pure verdict/budget logic (BoundHoundVerdict.h) is exercised headless -- no editor, no live frame
// globals -- and FrameTiming routes through the same helpers, so green here means the shipping verdict
// is green. The two live smoke tests additionally assert the JSON contract and input validation.

static const EAutomationTestFlags kBHTestFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// ---------------------------------------------------------------------------
// Verdict classification
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBoundHoundVerdictClearTest,
	"BoundHound.Verdict.ClearWinners", kBHTestFlags)
bool FBoundHoundVerdictClearTest::RunTest(const FString&)
{
	using namespace BoundHoundVerdict;

	// Game clearly slowest -> GameThread, clear, runner-up is the next-slowest (GPU at 11).
	{
		const FVerdict V = Classify(25.0, 8.0, /*Rhi*/ 0.0, 11.0, /*bRhi*/ false, /*bGpuAvailable*/ true);
		TestEqual(TEXT("game bound"), V.Bound, FString(TEXT("GameThread")));
		TestEqual(TEXT("game confidence"), V.Confidence, FString(TEXT("clear")));
		TestFalse(TEXT("game not contested"), V.bContested);
		TestEqual(TEXT("game runner-up"), V.RunnerName, FString(TEXT("GPU")));
		TestEqual(TEXT("game margin"), V.MarginMs, 14.0, 1e-6);
	}
	// Render clearly slowest.
	{
		const FVerdict V = Classify(8.0, 25.0, 0.0, 11.0, false, true);
		TestEqual(TEXT("render bound"), V.Bound, FString(TEXT("RenderThread")));
		TestEqual(TEXT("render confidence"), V.Confidence, FString(TEXT("clear")));
	}
	// GPU clearly slowest.
	{
		const FVerdict V = Classify(8.0, 11.0, 0.0, 25.0, false, true);
		TestEqual(TEXT("gpu bound"), V.Bound, FString(TEXT("GPU")));
		TestEqual(TEXT("gpu confidence"), V.Confidence, FString(TEXT("clear")));
	}
	// RHI clearly slowest (RHI-threading on) -> a distinct RHI-bound verdict.
	{
		const FVerdict V = Classify(8.0, 11.0, /*Rhi*/ 25.0, 9.0, /*bRhi*/ true, true);
		TestEqual(TEXT("rhi bound"), V.Bound, FString(TEXT("RHIThread")));
		TestEqual(TEXT("rhi confidence"), V.Confidence, FString(TEXT("clear")));
		TestFalse(TEXT("rhi not contested"), V.bContested);
		TestEqual(TEXT("rhi runner-up is render"), V.RunnerName, FString(TEXT("RenderThread")));
	}
	// RHI available but not the winner -> existing game verdict is unchanged, RHI just joins the ranking.
	{
		const FVerdict V = Classify(25.0, 8.0, /*Rhi*/ 9.0, 11.0, /*bRhi*/ true, true);
		TestEqual(TEXT("still game bound"), V.Bound, FString(TEXT("GameThread")));
		TestEqual(TEXT("still clear"), V.Confidence, FString(TEXT("clear")));
		TestFalse(TEXT("rhi present but not contested"), V.bContested);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBoundHoundVerdictContestedTest,
	"BoundHound.Verdict.Contested", kBHTestFlags)
bool FBoundHoundVerdictContestedTest::RunTest(const FString&)
{
	using namespace BoundHoundVerdict;

	// Top two within 10% -> a tie: contested, confidence downgraded to marginal, runner-up named.
	const FVerdict V = Classify(25.0, 24.0, 0.0, 5.0, false, true);
	TestEqual(TEXT("bound is the nominal top"), V.Bound, FString(TEXT("GameThread")));
	TestTrue(TEXT("contested"), V.bContested);
	TestEqual(TEXT("confidence marginal"), V.Confidence, FString(TEXT("marginal")));
	TestEqual(TEXT("contested_with"), V.RunnerName, FString(TEXT("RenderThread")));

	// marginal (tie) must win over moderate (GPU-unavailable) when both apply.
	const FVerdict M = Classify(10.0, 9.5, 0.0, 0.0, false, /*bGpuAvailable*/ false);
	TestTrue(TEXT("tie contested even w/o gpu"), M.bContested);
	TestEqual(TEXT("marginal beats moderate"), M.Confidence, FString(TEXT("marginal")));

	// RHI can contest the render thread -> contested tie names RHIThread as the runner-up.
	const FVerdict R = Classify(8.0, 25.0, /*Rhi*/ 24.0, 5.0, /*bRhi*/ true, true);
	TestEqual(TEXT("render nominally top"), R.Bound, FString(TEXT("RenderThread")));
	TestTrue(TEXT("render/rhi contested"), R.bContested);
	TestEqual(TEXT("contested_with rhi"), R.RunnerName, FString(TEXT("RHIThread")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBoundHoundVerdictBoundaryTest,
	"BoundHound.Verdict.ContestedBoundary", kBHTestFlags)
bool FBoundHoundVerdictBoundaryTest::RunTest(const FString&)
{
	using namespace BoundHoundVerdict;

	// Exactly 10% margin is NOT contested (strict < CONTESTED_PCT). 10 vs 9 -> 1/10 == 0.10 -> clear.
	const FVerdict V = Classify(10.0, 9.0, 0.0, 3.0, false, true);
	TestFalse(TEXT("exactly 10% is not contested"), V.bContested);
	TestEqual(TEXT("boundary is clear"), V.Confidence, FString(TEXT("clear")));

	// Just inside 10% (9.5/10 -> margin 0.5, 5%) IS contested.
	const FVerdict C = Classify(10.0, 9.5, 0.0, 3.0, false, true);
	TestTrue(TEXT("just under 10% is contested"), C.bContested);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBoundHoundVerdictGpuUnavailableTest,
	"BoundHound.Verdict.GpuUnavailable", kBHTestFlags)
bool FBoundHoundVerdictGpuUnavailableTest::RunTest(const FString&)
{
	using namespace BoundHoundVerdict;

	// GPU timing unavailable: GPU is dropped from the ranking, and a clear CPU winner is downgraded to
	// "moderate" because a hidden GPU cost can't be ruled out.
	const FVerdict V = Classify(25.0, 8.0, 0.0, 0.0, false, /*bGpuAvailable*/ false);
	TestEqual(TEXT("bound game"), V.Bound, FString(TEXT("GameThread")));
	TestEqual(TEXT("confidence moderate"), V.Confidence, FString(TEXT("moderate")));
	TestFalse(TEXT("not contested"), V.bContested);
	TestEqual(TEXT("runner is render (gpu excluded)"), V.RunnerName, FString(TEXT("RenderThread")));

	// RHI unavailable must NOT downgrade confidence (its work was counted on the render thread) -- only a
	// missing GPU number does. GPU present + RHI absent + clear CPU winner stays "clear".
	const FVerdict G = Classify(25.0, 8.0, /*Rhi*/ 0.0, 11.0, /*bRhi*/ false, /*bGpuAvailable*/ true);
	TestEqual(TEXT("rhi-absent stays clear"), G.Confidence, FString(TEXT("clear")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBoundHoundVerdictNoneTest,
	"BoundHound.Verdict.NoTiming", kBHTestFlags)
bool FBoundHoundVerdictNoneTest::RunTest(const FString&)
{
	using namespace BoundHoundVerdict;

	// No meaningful timing yet -> confidence "none", not contested.
	const FVerdict V = Classify(0.0, 0.0, 0.0, 0.0, false, true);
	TestEqual(TEXT("confidence none"), V.Confidence, FString(TEXT("none")));
	TestFalse(TEXT("zero not contested"), V.bContested);
	return true;
}

// ---------------------------------------------------------------------------
// Budget gate
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBoundHoundBudgetGateTest,
	"BoundHound.Budget.Gate", kBHTestFlags)
bool FBoundHoundBudgetGateTest::RunTest(const FString&)
{
	using namespace BoundHoundVerdict;

	// 10 ms frame vs 60 FPS (16.67 ms) -> PASS with positive headroom.
	{
		const FBudget B = ComputeBudget(10.0, 60.0f);
		TestEqual(TEXT("budget ms"), B.BudgetMs, 1000.0 / 60.0, 1e-6);
		TestTrue(TEXT("10ms meets 60"), B.bMeetsTarget);
		TestEqual(TEXT("headroom"), B.FrameHeadroomMs, (1000.0 / 60.0) - 10.0, 1e-6);
	}
	// 25 ms frame vs 60 FPS -> FAIL, negative headroom.
	{
		const FBudget B = ComputeBudget(25.0, 60.0f);
		TestFalse(TEXT("25ms fails 60"), B.bMeetsTarget);
		TestTrue(TEXT("negative headroom"), B.FrameHeadroomMs < 0.0);
	}
	// A zero frame time is not a pass (no real timing).
	{
		const FBudget B = ComputeBudget(0.0, 60.0f);
		TestFalse(TEXT("zero frame not a pass"), B.bMeetsTarget);
	}
	// Other target: 8 ms vs 120 FPS (8.33 ms) -> PASS.
	{
		const FBudget B = ComputeBudget(8.0, 120.0f);
		TestEqual(TEXT("120fps budget"), B.BudgetMs, 1000.0 / 120.0, 1e-6);
		TestTrue(TEXT("8ms meets 120"), B.bMeetsTarget);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBoundHoundBudgetSafeFpsTest,
	"BoundHound.Budget.SafeTargetFps", kBHTestFlags)
bool FBoundHoundBudgetSafeFpsTest::RunTest(const FString&)
{
	using namespace BoundHoundVerdict;

	// Invalid targets fall back to 60 FPS rather than dividing by zero / NaN.
	TestEqual(TEXT("zero -> 60"),     SafeTargetFps(0.0f),  60.0, 1e-9);
	TestEqual(TEXT("negative -> 60"), SafeTargetFps(-5.0f), 60.0, 1e-9);
	TestEqual(TEXT("nan -> 60"),      SafeTargetFps(FMath::Sqrt(-1.0f)), 60.0, 1e-9);
	TestEqual(TEXT("valid passes through"), SafeTargetFps(30.0f), 30.0, 1e-9);

	// ComputeBudget with a bad FPS still yields the 60 FPS budget.
	const FBudget B = ComputeBudget(10.0, 0.0f);
	TestEqual(TEXT("bad fps budget"), B.BudgetMs, 1000.0 / 60.0, 1e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBoundHoundBudgetPerThreadTest,
	"BoundHound.Budget.PerThreadOverBudget", kBHTestFlags)
bool FBoundHoundBudgetPerThreadTest::RunTest(const FString&)
{
	using namespace BoundHoundVerdict;
	const double Budget = 1000.0 / 60.0; // 16.67 ms

	TestTrue(TEXT("20ms over 16.67"),  IsOverBudget(20.0, Budget));
	TestFalse(TEXT("10ms under"),      IsOverBudget(10.0, Budget));
	TestFalse(TEXT("exactly budget is not over"), IsOverBudget(Budget, Budget));
	return true;
}

// ---------------------------------------------------------------------------
// Live smoke tests -- run in-editor, exercise the real AICallable surface
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBoundHoundFrameTimingShapeTest,
	"BoundHound.FrameTiming.JsonShape", kBHTestFlags)
bool FBoundHoundFrameTimingShapeTest::RunTest(const FString&)
{
	const FString Json = UBoundHoundService::FrameTiming(60.0f);

	TSharedPtr<FJsonObject> Obj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
	{
		AddError(FString::Printf(TEXT("FrameTiming did not return valid JSON: %s"), *Json.Left(200)));
		return false;
	}

	TestTrue(TEXT("success"), Obj->GetBoolField(TEXT("success")));

	// Required top-level fields of the verdict contract.
	for (const TCHAR* Field : { TEXT("game_thread_ms"), TEXT("render_thread_ms"), TEXT("rhi_thread_ms"),
		TEXT("gpu_ms"), TEXT("frame_ms"), TEXT("fps"), TEXT("bound"), TEXT("bound_confidence"),
		TEXT("margin_ms"), TEXT("contested"), TEXT("hint"), TEXT("pie_running"),
		TEXT("frame_number"), TEXT("timestamp"), TEXT("repeat_read"), TEXT("warming") })
	{
		TestTrue(FString::Printf(TEXT("has field %s"), Field), Obj->HasField(Field));
	}

	// Budget sub-object contract.
	const TSharedPtr<FJsonObject>* Budget = nullptr;
	if (TestTrue(TEXT("has budget object"), Obj->TryGetObjectField(TEXT("budget"), Budget)) && Budget)
	{
		for (const TCHAR* Field : { TEXT("target_fps"), TEXT("budget_ms"), TEXT("threads"),
			TEXT("frame_headroom_ms"), TEXT("meets_target"), TEXT("verdict") })
		{
			TestTrue(FString::Printf(TEXT("budget has %s"), Field), (*Budget)->HasField(Field));
		}
	}

	// The live bound must be one of the known verdicts (or none when there's no timing).
	const FString Bound = Obj->GetStringField(TEXT("bound"));
	const bool bKnown = Bound == TEXT("GameThread") || Bound == TEXT("RenderThread")
		|| Bound == TEXT("RHIThread") || Bound == TEXT("GPU");
	TestTrue(FString::Printf(TEXT("bound is a known thread: %s"), *Bound), bKnown);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBoundHoundForceHitchValidationTest,
	"BoundHound.ForceHitch.RejectsBadThread", kBHTestFlags)
bool FBoundHoundForceHitchValidationTest::RunTest(const FString&)
{
	// An unknown thread name must be rejected up front (before any stall is scheduled).
	const FString Json = UBoundHoundService::ForceHitch(TEXT("banana"), 1.0f, 1);

	TSharedPtr<FJsonObject> Obj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
	{
		AddError(TEXT("ForceHitch did not return valid JSON"));
		return false;
	}
	TestFalse(TEXT("bad thread not success"), Obj->GetBoolField(TEXT("success")));
	TestEqual(TEXT("error code"), Obj->GetStringField(TEXT("error_code")), FString(TEXT("BAD_THREAD")));
	return true;
}

// Race-free self-validation logic (issue #17): given the peak per-thread ms a hitch induced, decide
// whether it landed on the requested thread(s). Pure -> exercised headless, no live timing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBoundHoundHitchExpectTest,
	"BoundHound.ForceHitch.MatchesExpect", kBHTestFlags)
bool FBoundHoundHitchExpectTest::RunTest(const FString&)
{
	using namespace BoundHoundVerdict;
	const double Min = 125.0; // half of a 250 ms stall

	// game: game peak clears the floor AND dominates render -> match; render untouched must not match "game".
	TestTrue (TEXT("game hit"),          HitchMatchesExpect(TEXT("game"),   250.0,   0.0, Min));
	TestFalse(TEXT("game undershoot"),   HitchMatchesExpect(TEXT("game"),    50.0,   0.0, Min));
	TestFalse(TEXT("game but render bigger"), HitchMatchesExpect(TEXT("game"), 250.0, 300.0, Min));

	// render: symmetric.
	TestTrue (TEXT("render hit"),        HitchMatchesExpect(TEXT("render"),   0.0, 250.0, Min));
	TestFalse(TEXT("render undershoot"), HitchMatchesExpect(TEXT("render"),   0.0,  50.0, Min));

	// both: both threads must clear the floor.
	TestTrue (TEXT("both hit"),          HitchMatchesExpect(TEXT("both"),   250.0, 250.0, Min));
	TestFalse(TEXT("both one short"),    HitchMatchesExpect(TEXT("both"),   250.0,  50.0, Min));

	// rhi: the RHI peak (5th arg) must clear the floor AND dominate both CPU threads (issue #2).
	TestTrue (TEXT("rhi hit"),           HitchMatchesExpect(TEXT("rhi"),      0.0,   0.0, Min, /*Rhi*/ 250.0));
	TestFalse(TEXT("rhi undershoot"),    HitchMatchesExpect(TEXT("rhi"),      0.0,   0.0, Min, /*Rhi*/  50.0));
	TestFalse(TEXT("rhi but render bigger"), HitchMatchesExpect(TEXT("rhi"),  0.0, 300.0, Min, /*Rhi*/ 250.0));
	// rhi with no RHI peak (threading off -> stall folded into render) must not falsely match.
	TestFalse(TEXT("rhi threading off"), HitchMatchesExpect(TEXT("rhi"),      0.0, 250.0, Min, /*Rhi*/   0.0));

	// gpu / unknown are never self-validated by CPU peaks.
	TestFalse(TEXT("gpu not matched here"), HitchMatchesExpect(TEXT("gpu"), 999.0, 999.0, Min));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBoundHoundForceHitchSelfMeasureTest,
	"BoundHound.ForceHitch.SelfMeasure", kBHTestFlags)
bool FBoundHoundForceHitchSelfMeasureTest::RunTest(const FString&)
{
	// A game-thread stall must be induced AND measured within the single call: the response carries the
	// observed peak and verdict_matched_expect=true, with no follow-up FrameTiming (issue #17).
	const FString Json = UBoundHoundService::ForceHitch(TEXT("game"), 20.0f, 1);

	TSharedPtr<FJsonObject> Obj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
	{
		AddError(FString::Printf(TEXT("ForceHitch did not return valid JSON: %s"), *Json.Left(200)));
		return false;
	}

	TestTrue(TEXT("success"), Obj->GetBoolField(TEXT("success")));
	TestTrue(TEXT("has observed_peak_game_ms"), Obj->HasField(TEXT("observed_peak_game_ms")));
	TestTrue(TEXT("has verdict_matched_expect"), Obj->HasField(TEXT("verdict_matched_expect")));

	// The 20 ms sleep should measure at least the 10 ms match floor (sleep never undershoots by half).
	const double Peak = Obj->GetNumberField(TEXT("observed_peak_game_ms"));
	TestTrue(FString::Printf(TEXT("induced >= 10ms (got %.1f)"), Peak), Peak >= 10.0);
	TestTrue(TEXT("verdict matched"), Obj->GetBoolField(TEXT("verdict_matched_expect")));
	return true;
}

// The rhi arm (issue #2) is a ground-truth trigger for the RHIThread verdict. Headless runs under
// -nullrhi with no separate RHI thread, so this asserts the graceful fallback: the mode is accepted
// (not BAD_THREAD), rhi_threading reports false, a warning is surfaced, and nothing was validated. The
// live RHIThread-wins path can only be confirmed in a session with RHI-threading ON.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBoundHoundForceHitchRhiTest,
	"BoundHound.ForceHitch.RhiArm", kBHTestFlags)
bool FBoundHoundForceHitchRhiTest::RunTest(const FString&)
{
	const FString Json = UBoundHoundService::ForceHitch(TEXT("rhi"), 20.0f, 1);

	TSharedPtr<FJsonObject> Obj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
	{
		AddError(FString::Printf(TEXT("ForceHitch rhi did not return valid JSON: %s"), *Json.Left(200)));
		return false;
	}

	// 'rhi' is a valid mode -- accepted, not rejected as BAD_THREAD.
	TestTrue(TEXT("success"), Obj->GetBoolField(TEXT("success")));
	TestEqual(TEXT("forcing rhi"), Obj->GetStringField(TEXT("forcing")), FString(TEXT("rhi")));
	TestTrue(TEXT("has rhi_threading"), Obj->HasField(TEXT("rhi_threading")));

	// Headless nullrhi has no separate RHI thread: expect the flagged fallback, no induced verdict.
	if (!Obj->GetBoolField(TEXT("rhi_threading")))
	{
		TestTrue(TEXT("warns rhi-threading off"), Obj->HasField(TEXT("rhi_warning")));
		TestFalse(TEXT("nothing validated w/o rhi thread"), Obj->GetBoolField(TEXT("verdict_matched_expect")));
	}
	return true;
}

// Report() renders a self-contained HTML "printout" from the live verdict + capture summary. Headless
// (nullrhi, no trace) exercises the no-capture path: the file must still be written and the JSON
// contract (report_file / fix_count / included_capture) honoured. The HTML body itself isn't asserted
// field-by-field -- this guards that the tool writes a real file and reports it, not the layout.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBoundHoundReportSmokeTest,
	"BoundHound.Report.WritesFile", kBHTestFlags)
bool FBoundHoundReportSmokeTest::RunTest(const FString&)
{
	const FString Json = UBoundHoundService::Report(TEXT("Smoke Test"), TEXT("both"), TEXT(""));

	TSharedPtr<FJsonObject> Obj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
	{
		AddError(FString::Printf(TEXT("Report did not return valid JSON: %s"), *Json.Left(200)));
		return false;
	}

	TestTrue(TEXT("success"), Obj->GetBoolField(TEXT("success")));

	// Contract fields.
	TestTrue(TEXT("has report_file"), Obj->HasField(TEXT("report_file")));
	TestTrue(TEXT("has fix_count"), Obj->HasField(TEXT("fix_count")));
	TestTrue(TEXT("has included_capture"), Obj->HasField(TEXT("included_capture")));

	// No trace is available headless, so the capture column must be absent.
	TestFalse(TEXT("no capture headless"), Obj->GetBoolField(TEXT("included_capture")));

	// The reported path must point at a real, non-empty file on disk.
	const FString Path = Obj->GetStringField(TEXT("report_file"));
	TestTrue(TEXT("report_file non-empty"), !Path.IsEmpty());
	IFileManager& FM = IFileManager::Get();
	if (TestTrue(FString::Printf(TEXT("file exists: %s"), *Path), FM.FileExists(*Path)))
	{
		TestTrue(TEXT("file has content"), FM.FileSize(*Path) > 0);
		FM.Delete(*Path); // don't litter the project's Saved dir with test artifacts
	}
	return true;
}

// ---------------------------------------------------------------------------
// v1.0.1 field-report fixes (issues #23, #27, #28)
// ---------------------------------------------------------------------------

// #27: a read is a repeat only when a previous read exists and the frame counter hasn't moved.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBoundHoundRepeatReadLogicTest,
	"BoundHound.Freshness.RepeatReadLogic", kBHTestFlags)
bool FBoundHoundRepeatReadLogicTest::RunTest(const FString&)
{
	TestFalse(TEXT("first read is never a repeat"), BoundHoundVerdict::IsRepeatRead(false, 100, 0));
	TestFalse(TEXT("first read is never a repeat, even at frame 0"), BoundHoundVerdict::IsRepeatRead(false, 0, 0));
	TestTrue (TEXT("same frame -> repeat"),        BoundHoundVerdict::IsRepeatRead(true, 100, 100));
	TestFalse(TEXT("frame advanced -> fresh"),     BoundHoundVerdict::IsRepeatRead(true, 101, 100));
	return true;
}

// #27 live: two FrameTiming calls inside one test (no frame can complete in between) -> the second is a
// repeat read and must NOT carry a confident verdict.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBoundHoundRepeatReadLiveTest,
	"BoundHound.Freshness.RepeatReadLive", kBHTestFlags)
bool FBoundHoundRepeatReadLiveTest::RunTest(const FString&)
{
	auto Parse = [](const FString& Json) { TSharedPtr<FJsonObject> O; FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), O); return O; };
	const TSharedPtr<FJsonObject> A = Parse(UBoundHoundService::FrameTiming(60.0f));
	const TSharedPtr<FJsonObject> B = Parse(UBoundHoundService::FrameTiming(60.0f));
	if (!TestTrue(TEXT("both reads parse"), A.IsValid() && B.IsValid())) return false;

	TestEqual(TEXT("same frame_number"), A->GetNumberField(TEXT("frame_number")), B->GetNumberField(TEXT("frame_number")));
	TestTrue (TEXT("second read flagged repeat_read"), B->GetBoolField(TEXT("repeat_read")));
	TestEqual(TEXT("repeat read confidence is none"), B->GetStringField(TEXT("bound_confidence")), FString(TEXT("none")));
	TestTrue (TEXT("hint explains the repeat"), B->GetStringField(TEXT("hint")).StartsWith(TEXT("REPEAT READ")));
	return true;
}

// #23: PIE warm-up window.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBoundHoundWarmingTest,
	"BoundHound.Freshness.Warming", kBHTestFlags)
bool FBoundHoundWarmingTest::RunTest(const FString&)
{
	TestTrue (TEXT("2 s into PIE is warming"),  BoundHoundVerdict::IsWarming(2.0));
	TestTrue (TEXT("0 s into PIE is warming"),  BoundHoundVerdict::IsWarming(0.0));
	TestFalse(TEXT("at the settle point -> settled"), BoundHoundVerdict::IsWarming(BoundHoundVerdict::SETTLE_SECONDS));
	TestFalse(TEXT("60 s in -> settled"),       BoundHoundVerdict::IsWarming(60.0));
	TestFalse(TEXT("no PIE (-1) -> not warming"), BoundHoundVerdict::IsWarming(-1.0));
	return true;
}

// #23: the p95-vs-in-process ratio must read as a human ratio, never "~0x".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBoundHoundRatioTest,
	"BoundHound.Report.RatioText", kBHTestFlags)
bool FBoundHoundRatioTest::RunTest(const FString&)
{
	const FString Faster = BoundHoundVerdict::DescribeRatio(13.0, 70.6); // the field report's numbers
	TestTrue(FString::Printf(TEXT("13 vs 70.6 reads 5.4x faster: '%s'"), *Faster),
		Faster.Contains(TEXT("5.4")) && Faster.Contains(TEXT("faster")));
	const FString Slower = BoundHoundVerdict::DescribeRatio(20.0, 10.0);
	TestTrue(FString::Printf(TEXT("20 vs 10 reads 2.0x slower: '%s'"), *Slower),
		Slower.Contains(TEXT("2.0")) && Slower.Contains(TEXT("slower")));
	TestEqual(TEXT("within 10% -> about the same"), BoundHoundVerdict::DescribeRatio(10.5, 10.0), FString(TEXT("about the same as")));
	TestTrue(TEXT("missing value -> empty"), BoundHoundVerdict::DescribeRatio(0.0, 10.0).IsEmpty());
	TestTrue(TEXT("missing ref -> empty"),   BoundHoundVerdict::DescribeRatio(10.0, 0.0).IsEmpty());
	return true;
}

// #28: any running / queued / ending play session blocks starting another; idle is safe.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBoundHoundPlaySessionGuardTest,
	"BoundHound.PlaySession.BusyGuard", kBHTestFlags)
bool FBoundHoundPlaySessionGuardTest::RunTest(const FString&)
{
	using BoundHoundVerdict::PlaySessionBusyReason;
	TestTrue (TEXT("idle editor -> safe"), PlaySessionBusyReason(false, false, false, false).IsEmpty());
	TestFalse(TEXT("end-play queued (the #28 crash) -> busy"), PlaySessionBusyReason(true, false, true, true).IsEmpty());
	TestTrue (TEXT("ending is reported as ending, not running"),
		PlaySessionBusyReason(true, false, true, true).Contains(TEXT("ending")));
	TestFalse(TEXT("request queued -> busy"), PlaySessionBusyReason(false, true, false, false).IsEmpty());
	TestFalse(TEXT("PIE world live -> busy"), PlaySessionBusyReason(false, false, true, true).IsEmpty());
	TestFalse(TEXT("session in progress -> busy"), PlaySessionBusyReason(false, false, false, true).IsEmpty());
	return true;
}

// ---------------------------------------------------------------------------
// Trace-derived verdict (issue #22)
// ---------------------------------------------------------------------------

namespace
{
	void AddSamples(TArray<BoundHoundVerdict::FThreadSample>& Out, int32 Count, double Game, double Render, double Rhi, double Gpu)
	{
		for (int32 i = 0; i < Count; ++i) { BoundHoundVerdict::FThreadSample S; S.GameMs = Game; S.RenderMs = Render; S.RhiMs = Rhi; S.GpuMs = Gpu; Out.Add(S); }
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBoundHoundTraceVerdictTest,
	"BoundHound.TraceVerdict.Classify", kBHTestFlags)
bool FBoundHoundTraceVerdictTest::RunTest(const FString&)
{
	using namespace BoundHoundVerdict;

	// Steady game-thread-bound run: clear, wins every frame.
	{
		TArray<FThreadSample> S; AddSamples(S, 100, 20.0, 8.0, 0.0, 10.0);
		const FTraceVerdict T = ClassifyTrace(S);
		TestEqual(TEXT("steady: GameThread"), T.Verdict.Bound, FString(TEXT("GameThread")));
		TestEqual(TEXT("steady: clear"), T.Verdict.Confidence, FString(TEXT("clear")));
		TestEqual(TEXT("steady: 100 samples"), T.SampleCount, 100);
		TestEqual(TEXT("steady: game share 1.0"), T.ShareGame, 1.0);
		TestFalse(TEXT("steady: RHI absent (all zero)"), T.bRhiAvailable);
		TestEqual(TEXT("steady: game median"), T.Game.MedianMs, 20.0);
	}

	// GPU-bound run with one huge load frame on the game thread: the median ignores the spike.
	{
		TArray<FThreadSample> S; AddSamples(S, 99, 10.0, 5.0, 0.0, 14.0); AddSamples(S, 1, 30000.0, 5.0, 0.0, 14.0);
		const FTraceVerdict T = ClassifyTrace(S);
		TestEqual(TEXT("spike: GPU despite a 30 s game-thread frame"), T.Verdict.Bound, FString(TEXT("GPU")));
		TestTrue (TEXT("spike: game avg is dragged up"), T.Game.AvgMs > 300.0);
		TestEqual(TEXT("spike: game p95 unaffected"), T.Game.P95Ms, 10.0);
	}

	// Bottleneck alternates: GPU wins on medians but is the slowest thread in only 30% of frames.
	{
		TArray<FThreadSample> S;
		AddSamples(S, 40, 20.0, 5.0, 0.0, 14.0);  // game-bound frames
		AddSamples(S, 30, 10.0, 20.0, 0.0, 14.0); // render-bound frames
		AddSamples(S, 30, 10.0, 5.0, 0.0, 15.0);  // gpu-bound frames
		const FTraceVerdict T = ClassifyTrace(S);
		TestEqual(TEXT("alternating: GPU on medians"), T.Verdict.Bound, FString(TEXT("GPU")));
		TestEqual(TEXT("alternating: winner share 0.3"), T.WinnerShare, 0.3);
		TestEqual(TEXT("alternating: downgraded to marginal"), T.Verdict.Confidence, FString(TEXT("marginal")));
	}

	// RHI recorded and dominant.
	{
		TArray<FThreadSample> S; AddSamples(S, 50, 8.0, 9.0, 18.0, 10.0);
		const FTraceVerdict T = ClassifyTrace(S);
		TestTrue (TEXT("rhi: available"), T.bRhiAvailable);
		TestEqual(TEXT("rhi: RHIThread"), T.Verdict.Bound, FString(TEXT("RHIThread")));
	}

	// No samples: no verdict.
	{
		const FTraceVerdict T = ClassifyTrace({});
		TestEqual(TEXT("empty: confidence none"), T.Verdict.Confidence, FString(TEXT("none")));
		TestEqual(TEXT("empty: 0 samples"), T.SampleCount, 0);
	}
	return true;
}

// The CSV-profiler file is where the per-thread times come from (issue #22). Mirrors the real layout:
// EVENTS first, the four thread columns somewhere in the middle, a repeated header + metadata row at the end,
// and a last data row cut short because the process was killed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBoundHoundCsvParseTest,
	"BoundHound.TraceVerdict.CsvParse", kBHTestFlags)
bool FBoundHoundCsvParseTest::RunTest(const FString&)
{
	using namespace BoundHoundVerdict;
	const FString Csv =
		TEXT("EVENTS,FrameTime,RenderThreadTime,GameThreadTime,GPUTime,RHIThreadTime,Other\n")
		TEXT(",20.0,6.0,18.5,9.0,2.0,1\n")
		TEXT("GC,21.0,6.5,19.0,9.5,2.1,1\n")
		TEXT(",19.0,5.5,17.5,8.5,1.9,1,7,8\n")                                                    // grew 2 columns mid-run
		TEXT("EVENTS,FrameTime,RenderThreadTime,GameThreadTime,GPUTime,RHIThreadTime,Other,A,B\n") // repeated header
		TEXT("[HasHeaderRowAtEnd],1,[platform],Windows\n")                                        // metadata
		TEXT(",20.0,6.0,18.5,9.0,2.0,1,7,8,9,10,1");                                              // unterminated

	TArray<FThreadSample> S; FCsvThreadColumns Cols;
	TestTrue (TEXT("parses"), ParseCsvThreadSamples(Csv, S, Cols));
	TestEqual(TEXT("3 data rows (wider row kept; header/metadata/unterminated skipped)"), S.Num(), 3);
	TestTrue (TEXT("all four columns found"), Cols.bGame && Cols.bRender && Cols.bRhi && Cols.bGpu);
	if (S.Num() == 3)
	{
		TestEqual(TEXT("row 2 game"),   S[1].GameMs, 19.0);
		TestEqual(TEXT("row 2 render"), S[1].RenderMs, 6.5);
		TestEqual(TEXT("row 2 gpu"),    S[1].GpuMs, 9.5);
		TestEqual(TEXT("row 2 rhi"),    S[1].RhiMs, 2.1);
		TestEqual(TEXT("verdict"), ClassifyTrace(S).Verdict.Bound, FString(TEXT("GameThread")));
	}

	// No RHIThreadTime column (RHI-threading off): RHI reads 0 and isn't ranked.
	const FString NoRhi = TEXT("EVENTS,RenderThreadTime,GameThreadTime,GPUTime\n,5,10,14\n,5,10,14\n");
	TestTrue (TEXT("no-rhi parses"), ParseCsvThreadSamples(NoRhi, S, Cols));
	TestFalse(TEXT("no-rhi: column absent"), Cols.bRhi);
	TestEqual(TEXT("no-rhi: GPU-bound"), ClassifyTrace(S).Verdict.Bound, FString(TEXT("GPU")));

	// Missing a CPU thread column -> can't rank, no samples.
	TestFalse(TEXT("missing GameThreadTime -> false"), ParseCsvThreadSamples(TEXT("EVENTS,RenderThreadTime,GPUTime\n,5,14\n"), S, Cols));
	TestFalse(TEXT("empty -> false"), ParseCsvThreadSamples(FString(), S, Cols));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBoundHoundPercentileTest,
	"BoundHound.TraceVerdict.Percentile", kBHTestFlags)
bool FBoundHoundPercentileTest::RunTest(const FString&)
{
	using BoundHoundVerdict::Percentile;
	const TArray<double> V = { 5.0, 1.0, 4.0, 2.0, 3.0 };
	TestEqual(TEXT("median of 1..5"), Percentile(V, 0.5), 3.0);
	TestEqual(TEXT("p100 is max"), Percentile(V, 1.0), 5.0);
	TestEqual(TEXT("p0 is min"), Percentile(V, 0.0), 1.0);
	TestEqual(TEXT("empty -> 0"), Percentile({}, 0.5), 0.0);
	return true;
}

#endif // WITH_AUTOMATION_TESTS

// Copyright (c) 2026 exetorius. Released under the MIT License.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "BoundHoundVerdict.h"
#include "BoundHoundService.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"

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
		const FVerdict V = Classify(25.0, 8.0, 11.0, /*bGpuAvailable*/ true);
		TestEqual(TEXT("game bound"), V.Bound, FString(TEXT("GameThread")));
		TestEqual(TEXT("game confidence"), V.Confidence, FString(TEXT("clear")));
		TestFalse(TEXT("game not contested"), V.bContested);
		TestEqual(TEXT("game runner-up"), V.RunnerName, FString(TEXT("GPU")));
		TestEqual(TEXT("game margin"), V.MarginMs, 14.0, 1e-6);
	}
	// Render clearly slowest.
	{
		const FVerdict V = Classify(8.0, 25.0, 11.0, true);
		TestEqual(TEXT("render bound"), V.Bound, FString(TEXT("RenderThread")));
		TestEqual(TEXT("render confidence"), V.Confidence, FString(TEXT("clear")));
	}
	// GPU clearly slowest.
	{
		const FVerdict V = Classify(8.0, 11.0, 25.0, true);
		TestEqual(TEXT("gpu bound"), V.Bound, FString(TEXT("GPU")));
		TestEqual(TEXT("gpu confidence"), V.Confidence, FString(TEXT("clear")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBoundHoundVerdictContestedTest,
	"BoundHound.Verdict.Contested", kBHTestFlags)
bool FBoundHoundVerdictContestedTest::RunTest(const FString&)
{
	using namespace BoundHoundVerdict;

	// Top two within 10% -> a tie: contested, confidence downgraded to marginal, runner-up named.
	const FVerdict V = Classify(25.0, 24.0, 5.0, true);
	TestEqual(TEXT("bound is the nominal top"), V.Bound, FString(TEXT("GameThread")));
	TestTrue(TEXT("contested"), V.bContested);
	TestEqual(TEXT("confidence marginal"), V.Confidence, FString(TEXT("marginal")));
	TestEqual(TEXT("contested_with"), V.RunnerName, FString(TEXT("RenderThread")));

	// marginal (tie) must win over moderate (GPU-unavailable) when both apply.
	const FVerdict M = Classify(10.0, 9.5, 0.0, /*bGpuAvailable*/ false);
	TestTrue(TEXT("tie contested even w/o gpu"), M.bContested);
	TestEqual(TEXT("marginal beats moderate"), M.Confidence, FString(TEXT("marginal")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBoundHoundVerdictBoundaryTest,
	"BoundHound.Verdict.ContestedBoundary", kBHTestFlags)
bool FBoundHoundVerdictBoundaryTest::RunTest(const FString&)
{
	using namespace BoundHoundVerdict;

	// Exactly 10% margin is NOT contested (strict < CONTESTED_PCT). 10 vs 9 -> 1/10 == 0.10 -> clear.
	const FVerdict V = Classify(10.0, 9.0, 3.0, true);
	TestFalse(TEXT("exactly 10% is not contested"), V.bContested);
	TestEqual(TEXT("boundary is clear"), V.Confidence, FString(TEXT("clear")));

	// Just inside 10% (9.5/10 -> margin 0.5, 5%) IS contested.
	const FVerdict C = Classify(10.0, 9.5, 3.0, true);
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
	const FVerdict V = Classify(25.0, 8.0, 0.0, /*bGpuAvailable*/ false);
	TestEqual(TEXT("bound game"), V.Bound, FString(TEXT("GameThread")));
	TestEqual(TEXT("confidence moderate"), V.Confidence, FString(TEXT("moderate")));
	TestFalse(TEXT("not contested"), V.bContested);
	TestEqual(TEXT("runner is render (gpu excluded)"), V.RunnerName, FString(TEXT("RenderThread")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBoundHoundVerdictNoneTest,
	"BoundHound.Verdict.NoTiming", kBHTestFlags)
bool FBoundHoundVerdictNoneTest::RunTest(const FString&)
{
	using namespace BoundHoundVerdict;

	// No meaningful timing yet -> confidence "none", not contested.
	const FVerdict V = Classify(0.0, 0.0, 0.0, true);
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
		TEXT("margin_ms"), TEXT("contested"), TEXT("hint"), TEXT("pie_running") })
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
	const bool bKnown = Bound == TEXT("GameThread") || Bound == TEXT("RenderThread") || Bound == TEXT("GPU");
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

#endif // WITH_AUTOMATION_TESTS

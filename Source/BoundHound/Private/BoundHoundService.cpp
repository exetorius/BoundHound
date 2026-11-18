// Copyright (c) 2026 exetorius. Released under the MIT License.

#include "BoundHoundService.h"
#include "BoundHoundVerdict.h"
#include "BoundHoundDrilldown.h"
#include "BoundHoundBaseline.h"
#include "Misc/EngineVersion.h"
#include "Json.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/App.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/IConsoleManager.h"
#include "Containers/Ticker.h"
#include "RenderingThread.h"   // ENQUEUE_RENDER_COMMAND
#include "RHICommandList.h"    // FRHICommandListImmediate
#include "Modules/ModuleManager.h"
#include "ProfilingDebugging/TraceAuxiliary.h"
#include "ProfilingDebugging/MiscTrace.h"
#include "PlayInEditorDataTypes.h"
#include "Editor.h"
#include "TraceServices/ITraceServicesModule.h"
#include "TraceServices/AnalysisService.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Frames.h"
#include "TraceServices/Model/TimingProfiler.h"
#include "TraceServices/Model/Log.h"
#include "TraceServices/Model/Threads.h"
#include "RenderTimer.h"       // GGameThreadTime / GRenderThreadTime / GRHIThreadTime (RenderCore)
#include "RHIGlobals.h"        // RHIGetGPUFrameCycles (RHI)

DEFINE_LOG_CATEGORY_STATIC(LogBoundHound, Log, All);

// ---------------------------------------------------------------------------
// Session state -- persists across MCP requests within an editor session
// ---------------------------------------------------------------------------

static FString GLastTraceFilePath;
static FString GLastLogFilePath;
static FString GLastCsvFilePath;
static bool           GStandaloneRunning = false;
static FProcHandle    GStandaloneProcess;
static FDelegateHandle GStandalonePlayDelegateHandle;

// ---------------------------------------------------------------------------
// JSON response helpers
// ---------------------------------------------------------------------------

static FString OkJson(TSharedPtr<FJsonObject> Obj)
{
	Obj->SetBoolField(TEXT("success"), true);
	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Obj.ToSharedRef(), W);
	return Out;
}

static FString ErrJson(const FString& Code, const FString& Msg)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("success"), false);
	Obj->SetStringField(TEXT("error_code"), Code);
	Obj->SetStringField(TEXT("error"), Msg);
	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Obj.ToSharedRef(), W);
	return Out;
}

static bool IsPIERunning()
{
	return GEditor && GEditor->PlayWorld != nullptr;
}

// Issue #28: why a new play session can't start right now (empty = safe).
static FString PlaySessionBusyReason()
{
	if (!GEditor) return FString();
	return BoundHoundVerdict::PlaySessionBusyReason(
		GEditor->ShouldEndPlayMap(), GEditor->IsPlaySessionRequestQueued(),
		GEditor->PlayWorld != nullptr, GEditor->IsPlayingSessionInEditor());
}

static FString PlaySessionBusyError(const FString& Reason)
{
	return ErrJson(TEXT("PLAY_SESSION_ACTIVE"), FString::Printf(
		TEXT("Can't start a play session: %s. Stop it, then return control to the engine for at least one frame (end this Python call / MCP request) before starting another."),
		*Reason));
}

// Issue #27: last frame FrameTiming read, to flag repeat reads of the same frame.
static bool   GHasLastTimingFrame = false;
static uint64 GLastTimingFrame    = 0;

static const TCHAR* DefaultTraceChannels()
{
	return TEXT("frame,cpu,gpu,log,loadtime,object,stats,bookmark,region");
}

static FString ProjectSavedDirAbs()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
}

static FString BuildTraceFilePath(const FString& Name)
{
	FString Dir = ProjectSavedDirAbs() / TEXT("Profiling");
	IPlatformFile::GetPlatformPhysical().CreateDirectoryTree(*Dir);
	return Dir / Name;
}

// ---------------------------------------------------------------------------
// Trace analysis
// ---------------------------------------------------------------------------

static FString BoundHint(const FString& Bound); // defined with FrameTiming below

// ---------------------------------------------------------------------------
// Worst-frame drill-down (issue #25)
// ---------------------------------------------------------------------------

static constexpr double LOAD_FRAME_MS = 5000.0;  // a frame this long is loading, not gameplay
static constexpr int32  MAX_SPIKE_FRAMES = 10;  // spike events explained one by one (issue #31)
static constexpr double LOG_WINDOW_S = 0.05;     // log lines this close to a frame belong to it
static constexpr int32  MAX_SCOPES = 5;
static constexpr int32  MAX_LOG_LINES = 8;

// Explains one frame from the trace: its top game-thread scopes by self time, the log lines around it,
// and the causes those point to. Must be used inside an FAnalysisSessionReadScope.
class FFrameExplainer
{
public:
	explicit FFrameExplainer(const TraceServices::IAnalysisSession& InSession)
	{
		Timing = TraceServices::ReadTimingProfilerProvider(InSession);
		Log = InSession.ReadProvider<TraceServices::ILogProvider>(TraceServices::GetLogProviderName());
		if (Timing)
		{
			uint32 GameThreadId = 0; bool bFound = false;
			TraceServices::ReadThreadProvider(InSession).EnumerateThreads([&](const TraceServices::FThreadInfo& T)
			{
				if (!bFound && T.Name && FCString::Strcmp(T.Name, TEXT("GameThread")) == 0) { GameThreadId = T.Id; bFound = true; }
			});
			bHasGameTimeline = bFound && Timing->GetCpuThreadTimelineIndex(GameThreadId, GameTimeline);
		}
	}

	bool HasScopes() const { return bHasGameTimeline; }

	// CsvEvents: names from the CSV's EVENTS column at this frame (issue #31). They count as evidence: the CSV
	// names GC even when the trace has no CollectGarbage scope.
	void Explain(double Start, double End, const TSharedPtr<FJsonObject>& Out, const TArray<FString>& CsvEvents = {}) const
	{
		TArray<FString> ScopeNames;
		if (bHasGameTimeline)
		{
			const TraceServices::ITimingProfilerTimerReader& Timers = Timing->GetTimerReader();
			TArray<BoundHoundDrilldown::FScopeEvent> Events;
			Timing->ReadTimeline(GameTimeline, [&](const TraceServices::ITimingProfilerProvider::Timeline& TL)
			{
				TL.EnumerateEvents(Start, End, [&](double S, double E, uint32 Depth, const TraceServices::FTimingProfilerEvent& Ev)
				{
					uint32 Id = Ev.TimerIndex;
					if (int32(Id) < 0) Id = Timers.GetOriginalTimerIdFromMetadata(Id); // merge metadata variants
					Events.Add({ S, E, Depth, Id });
					return TraceServices::EEventEnumerate::Continue;
				});
			});
			TArray<TSharedPtr<FJsonValue>> ScopeArr;
			for (const TPair<uint32, double>& T : BoundHoundDrilldown::TopSelfTimes(Events, Start, End, MAX_SCOPES))
			{
				const TraceServices::FTimingProfilerTimer* Timer = Timers.GetTimer(T.Key);
				const FString Name = (Timer && Timer->Name) ? FString(Timer->Name) : FString::Printf(TEXT("timer %u"), T.Key);
				ScopeNames.Add(Name);
				TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
				O->SetStringField(TEXT("scope"), Name);
				O->SetNumberField(TEXT("self_ms"), FMath::RoundToDouble(T.Value * 100.0) / 100.0);
				ScopeArr.Add(MakeShared<FJsonValueObject>(O));
			}
			Out->SetArrayField(TEXT("top_scopes"), ScopeArr);
		}

		TArray<FString> Lines;
		if (Log)
		{
			Log->EnumerateMessages(Start - LOG_WINDOW_S, End + LOG_WINDOW_S, [&](const TraceServices::FLogMessageInfo& M)
			{
				if (!M.Message) return;
				Lines.Add(FString::Printf(TEXT("%s: %s"), (M.Category && M.Category->Name) ? M.Category->Name : TEXT("Log"), M.Message));
			});
		}

		TArray<FString> Evidence = Lines;
		if (CsvEvents.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> EvArr;
			for (const FString& E : CsvEvents)
			{
				Evidence.Add(FString::Printf(TEXT("CSVEvent \"%s\""), *E));
				EvArr.Add(MakeShared<FJsonValueString>(E));
			}
			Out->SetArrayField(TEXT("csv_events"), EvArr);
		}
		const TArray<BoundHoundDrilldown::FCause> Causes = BoundHoundDrilldown::ClassifyCauses(Evidence, ScopeNames);
		TArray<TSharedPtr<FJsonValue>> CauseArr;
		for (const BoundHoundDrilldown::FCause& C : Causes)
		{
			TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("cause"), C.Tag);
			O->SetStringField(TEXT("why"), C.Why);
			O->SetStringField(TEXT("lever"), C.Lever);
			CauseArr.Add(MakeShared<FJsonValueObject>(O));
		}
		Out->SetArrayField(TEXT("causes"), CauseArr);

		// Keep the most telling lines first (a cause matched it, or it's a warning/error), then the rest.
		TArray<TSharedPtr<FJsonValue>> LineArr;
		auto Telling = [](const FString& L)
		{
			const TArray<FString> One = { L };
			return BoundHoundDrilldown::ClassifyCauses(One, {}).Num() > 0 || L.Contains(TEXT("Warning")) || L.Contains(TEXT("Error"));
		};
		for (int32 Pass = 0; Pass < 2 && LineArr.Num() < MAX_LOG_LINES; ++Pass)
		{
			for (const FString& L : Lines)
			{
				if (LineArr.Num() >= MAX_LOG_LINES) break;
				if ((Pass == 0) == Telling(L)) LineArr.Add(MakeShared<FJsonValueString>(L.Left(300)));
			}
		}
		Out->SetArrayField(TEXT("log_lines"), LineArr);
		if (Lines.Num() > LineArr.Num()) Out->SetNumberField(TEXT("log_lines_total"), Lines.Num());
	}

private:
	const TraceServices::ITimingProfilerProvider* Timing = nullptr;
	const TraceServices::ILogProvider* Log = nullptr;
	uint32 GameTimeline = 0;
	bool bHasGameTimeline = false;
};


// Issue #22: per-thread split + CPU-vs-GPU verdict for a captured run. StartStandalone runs a CSV-profiler
// capture alongside the trace and names it after the trace: <Saved>/Profiling/<base>.utrace pairs with
// <Saved>/Profiling/CSV/<base>.csv. The CSV holds GameThreadTime / RenderThreadTime / RHIThreadTime / GPUTime
// for every frame (the same globals FrameTiming reads). See BoundHoundVerdict::ParseCsvThreadSamples for why
// this comes from the CSV and not the trace.
static FString CsvPathForTrace(const FString& TraceFile)
{
	return FPaths::GetPath(TraceFile) / TEXT("CSV") / (FPaths::GetBaseFilename(TraceFile) + TEXT(".csv"));
}

// What the trace analysis needs back from the CSV (issue #31): the samples (frame times, for the cap check
// and the alignment with the trace), the EVENTS column, and the verdict's per-thread medians.
struct FCsvCapture
{
	bool bOk = false;
	TArray<BoundHoundVerdict::FThreadSample> Samples;
	BoundHoundVerdict::FCsvThreadColumns Cols;
	TArray<BoundHoundVerdict::FCsvEvent> Events;
	BoundHoundVerdict::FTraceVerdict T;

	// The slowest thread's median and its name: the real per-frame work, whatever the frame time says.
	double BottleneckMedianMs(FString* OutName = nullptr) const
	{
		struct { const TCHAR* Name; double Ms; } C[] = {
			{ TEXT("GameThread"), T.Game.MedianMs }, { TEXT("RenderThread"), T.Render.MedianMs },
			{ TEXT("RHIThread"), T.bRhiAvailable ? T.Rhi.MedianMs : 0.0 }, { TEXT("GPU"), T.bGpuAvailable ? T.Gpu.MedianMs : 0.0 } };
		int32 Best = 0;
		for (int32 i = 1; i < 4; ++i) if (C[i].Ms > C[Best].Ms) Best = i;
		if (OutName) *OutName = C[Best].Name;
		return C[Best].Ms;
	}
};

static void AddThreadSplit(const FString& TraceFile, const TSharedPtr<FJsonObject>& Root, FCsvCapture& Cap)
{
	auto Round2 = [](double V) -> double { return FMath::RoundToDouble(V * 100.0) / 100.0; };
	TSharedPtr<FJsonObject> Split = MakeShared<FJsonObject>();
	const FString CsvFile = CsvPathForTrace(TraceFile);
	Split->SetStringField(TEXT("csv_file"), CsvFile);

	auto Unavailable = [&](const FString& Why)
	{
		Split->SetBoolField(TEXT("available"), false);
		Split->SetStringField(TEXT("note"), Why + TEXT(" Per-thread times come from the CSV-profiler file StartStandalone (v1.1.0+) writes next to its trace. Traces from StartTrace in the editor, or older standalone runs, have whole-frame stats only."));
		Root->SetObjectField(TEXT("thread_split"), Split);
	};

	FString Content;
	if (!FPaths::FileExists(CsvFile))
	{
		Unavailable(FPaths::FileExists(CsvFile + TEXT(".gz"))
			? FString(TEXT("The CSV capture is compressed (.csv.gz), which BoundHound can't read; set csv.CompressionMode 0."))
			: FString(TEXT("No CSV capture found for this trace.")));
		return;
	}
	if (!FFileHelper::LoadFileToString(Content, *CsvFile))
	{
		Unavailable(TEXT("The CSV capture exists but couldn't be read (still open by the game?)."));
		return;
	}

	if (!BoundHoundVerdict::ParseCsvThreadSamples(Content, Cap.Samples, Cap.Cols, &Cap.Events))
	{
		Unavailable(TEXT("The CSV capture has no usable GameThreadTime / RenderThreadTime rows."));
		return;
	}
	Content.Empty();

	Cap.T = BoundHoundVerdict::ClassifyTrace(Cap.Samples);
	Cap.bOk = true;
	const BoundHoundVerdict::FTraceVerdict& T = Cap.T;
	auto ThreadObj = [&](const BoundHoundVerdict::FThreadStats& S)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("avg_ms"), Round2(S.AvgMs));
		O->SetNumberField(TEXT("median_ms"), Round2(S.MedianMs));
		O->SetNumberField(TEXT("p95_ms"), Round2(S.P95Ms));
		return O;
	};
	Split->SetBoolField(TEXT("available"), true);
	Split->SetNumberField(TEXT("frames"), T.SampleCount);
	Split->SetObjectField(TEXT("game_thread"), ThreadObj(T.Game));
	Split->SetObjectField(TEXT("render_thread"), ThreadObj(T.Render));
	if (T.bRhiAvailable) Split->SetObjectField(TEXT("rhi_thread"), ThreadObj(T.Rhi));
	if (T.bGpuAvailable) Split->SetObjectField(TEXT("gpu"), ThreadObj(T.Gpu));
	Root->SetObjectField(TEXT("thread_split"), Split);

	// The verdict, with the same fields and rules as FrameTiming, but from the per-thread medians.
	Root->SetStringField(TEXT("bound"), T.Verdict.Bound);
	Root->SetStringField(TEXT("bound_confidence"), T.Verdict.Confidence);
	Root->SetNumberField(TEXT("margin_ms"), Round2(T.Verdict.MarginMs));
	Root->SetBoolField(TEXT("contested"), T.Verdict.bContested);
	if (T.Verdict.bContested && !T.Verdict.RunnerName.IsEmpty()) Root->SetStringField(TEXT("contested_with"), T.Verdict.RunnerName);

	TSharedPtr<FJsonObject> Share = MakeShared<FJsonObject>();
	Share->SetNumberField(TEXT("GameThread"), Round2(T.ShareGame));
	Share->SetNumberField(TEXT("RenderThread"), Round2(T.ShareRender));
	if (T.bRhiAvailable) Share->SetNumberField(TEXT("RHIThread"), Round2(T.ShareRhi));
	if (T.bGpuAvailable) Share->SetNumberField(TEXT("GPU"), Round2(T.ShareGpu));
	Root->SetObjectField(TEXT("bound_share"), Share);

	FString Hint = FString::Printf(TEXT("Verdict from the per-thread MEDIANS of %d captured frames (%s was the slowest thread in %.0f%% of them). %s"),
		T.SampleCount, *T.Verdict.Bound, T.WinnerShare * 100.0, *BoundHint(T.Verdict.Bound));
	if (T.WinnerShare < BoundHoundVerdict::MIN_WINNER_SHARE)
	{
		Hint = FString::Printf(TEXT("ALTERNATING: the slowest thread changes from frame to frame (see bound_share), so treat the top threads as joint bottlenecks. %s"), *Hint);
	}
	else if (!T.bGpuAvailable && T.Verdict.Bound != TEXT("GPU"))
	{
		Hint += TEXT(" NOTE: no GPU timing was recorded, so a GPU bottleneck can't be ruled out.");
	}
	Root->SetStringField(TEXT("bound_hint"), Hint);
}

// Issue #31: the CSV's EVENTS column, summarised: each event name, how often it fired and when (seconds from
// the capture's start). GC shows up here even when the trace has no CollectGarbage scope.
static void AddCsvEventSummary(const FCsvCapture& Csv, const TSharedPtr<FJsonObject>& Root)
{
	if (!Csv.bOk || Csv.Events.IsEmpty()) return;
	TMap<FString, TArray<double>> ByName;
	for (const BoundHoundVerdict::FCsvEvent& E : Csv.Events) ByName.FindOrAdd(E.Name).Add(E.TimeS);
	ByName.ValueSort([](const TArray<double>& A, const TArray<double>& B) { return A.Num() > B.Num(); });
	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const TPair<FString, TArray<double>>& KV : ByName)
	{
		if (Arr.Num() >= 15) break;
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("name"), KV.Key);
		O->SetNumberField(TEXT("count"), KV.Value.Num());
		TArray<TSharedPtr<FJsonValue>> Times;
		for (int32 i = 0; i < FMath::Min(KV.Value.Num(), 20); ++i) Times.Add(MakeShared<FJsonValueNumber>(FMath::RoundToDouble(KV.Value[i] * 10.0) / 10.0));
		O->SetArrayField(TEXT("times_s"), Times);
		Arr.Add(MakeShared<FJsonValueObject>(O));
	}
	Root->SetArrayField(TEXT("csv_events"), Arr);
}

// Analysing a long capture takes a while (a 1.3 GB, 4-minute trace runs past the 30 s MCP Python timeout),
// and Report() analyses again after Analyse(). Results are cached per trace, keyed on the trace's and its
// CSV's size + modified time, so a repeat call -- or a retry after a client timeout, since the first call
// still finishes in the editor -- returns at once. A re-written file changes the key.
struct FAnalysisCacheEntry { FString Key; FString Json; };
static TMap<FString, FAnalysisCacheEntry> GAnalysisCache;

static FString AnalysisCacheKey(const FString& TraceFile)
{
	IFileManager& FM = IFileManager::Get();
	const FString Csv = CsvPathForTrace(TraceFile);
	return FString::Printf(TEXT("%lld|%s|%lld|%s"),
		FM.FileSize(*TraceFile), *FM.GetTimeStamp(*TraceFile).ToString(),
		FM.FileSize(*Csv), *FM.GetTimeStamp(*Csv).ToString());
}

static FString AnalyseTraceUncached(const FString& TraceFile);

static FString AnalyseTrace(const FString& TraceFile)
{
	if (!FPaths::FileExists(TraceFile))
	{
		return ErrJson(TEXT("FILE_NOT_FOUND"), FString::Printf(TEXT("Trace file not found: %s"), *TraceFile));
	}
	const FString Key = AnalysisCacheKey(TraceFile);
	if (const FAnalysisCacheEntry* Hit = GAnalysisCache.Find(TraceFile))
	{
		if (Hit->Key == Key)
		{
			TSharedPtr<FJsonObject> Obj;
			if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Hit->Json), Obj) && Obj.IsValid())
			{
				Obj->SetBoolField(TEXT("cached"), true);
				FString Out;
				FJsonSerializer::Serialize(Obj.ToSharedRef(), TJsonWriterFactory<>::Create(&Out));
				return Out;
			}
		}
	}

	const double T0 = FPlatformTime::Seconds();
	FString Json = AnalyseTraceUncached(TraceFile);
	TSharedPtr<FJsonObject> Obj;
	if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Obj) && Obj.IsValid() && Obj->GetBoolField(TEXT("success")))
	{
		Obj->SetNumberField(TEXT("analysis_seconds"), FMath::RoundToDouble((FPlatformTime::Seconds() - T0) * 10.0) / 10.0);
		Obj->SetBoolField(TEXT("cached"), false);
		Json.Reset();
		FJsonSerializer::Serialize(Obj.ToSharedRef(), TJsonWriterFactory<>::Create(&Json));
		if (GAnalysisCache.Num() >= 16) GAnalysisCache.Reset(); // a session analyses a handful of traces
		GAnalysisCache.Add(TraceFile, { Key, Json });
	}
	return Json;
}

static FString AnalyseTraceUncached(const FString& TraceFile)
{

	ITraceServicesModule* TraceModule = FModuleManager::LoadModulePtr<ITraceServicesModule>("TraceServices");
	if (!TraceModule)
	{
		return ErrJson(TEXT("MODULE_NOT_FOUND"), TEXT("TraceServices module not available."));
	}

	TSharedPtr<TraceServices::IAnalysisService> AnalysisService = TraceModule->GetAnalysisService();
	if (!AnalysisService)
	{
		return ErrJson(TEXT("SERVICE_NOT_FOUND"), TEXT("Could not get TraceServices analysis service."));
	}

	UE_LOG(LogBoundHound, Log, TEXT("Analysing trace: %s"), *TraceFile);
	TSharedPtr<const TraceServices::IAnalysisSession> Session = AnalysisService->Analyze(*TraceFile);
	if (!Session)
	{
		return ErrJson(TEXT("ANALYSIS_FAILED"), TEXT("Failed to analyse trace file."));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("trace_file"), TraceFile);

	// The CSV capture first: its frame times and events feed the spike analysis below (issue #31).
	FCsvCapture Csv;
	AddThreadSplit(TraceFile, Root, Csv);
	AddCsvEventSummary(Csv, Root);

	{
		TraceServices::FAnalysisSessionReadScope Scope(*Session);
		Root->SetNumberField(TEXT("duration_seconds"), Session->GetDurationSeconds());
		const TraceServices::IFrameProvider* Frames = Session->ReadProvider<TraceServices::IFrameProvider>(TraceServices::GetFrameProviderName());

		if (Frames)
		{
			uint64 FrameCount = Frames->GetFrameCount(ETraceFrameType::TraceFrameType_Game);
			Root->SetNumberField(TEXT("frame_count"), (double)FrameCount);

			if (FrameCount > 0)
			{
				double TotalMs = 0.0;
				double MaxMs = 0.0;
				uint64 MaxFrame = 0;
				double MaxFrameTime = 0.0;
				TArray<double> AllMs;
				AllMs.Reserve((int32)FMath::Min(FrameCount, (uint64)10000));

				Frames->EnumerateFrames(ETraceFrameType::TraceFrameType_Game, 0, FrameCount,
					[&](const TraceServices::FFrame& F)
					{
						if (F.EndTime <= F.StartTime) return;
						double DurationMs = (F.EndTime - F.StartTime) * 1000.0;
						if (!FMath::IsFinite(DurationMs) || DurationMs > 120000.0) return;
						AllMs.Add(DurationMs);
						TotalMs += DurationMs;
						if (DurationMs > MaxMs)
						{
							MaxMs = DurationMs;
							MaxFrame = F.Index;
							MaxFrameTime = F.StartTime;
						}
					});

				if (AllMs.IsEmpty())
				{
					Root->SetStringField(TEXT("warning"), TEXT("All frames were filtered (invalid timestamps) -- no frame stats available."));
				}
				else
				{
					double AvgMs = TotalMs / (double)AllMs.Num();
					Root->SetNumberField(TEXT("avg_frame_ms"), FMath::RoundToFloat(AvgMs * 100.0f) / 100.0f);
					Root->SetNumberField(TEXT("avg_fps"), FMath::RoundToFloat(1000.0f / (float)AvgMs * 10.0f) / 10.0f);
					Root->SetNumberField(TEXT("max_frame_ms"), FMath::RoundToFloat(MaxMs * 100.0f) / 100.0f);
					Root->SetNumberField(TEXT("max_frame_index"), (double)MaxFrame);
					Root->SetNumberField(TEXT("max_frame_timestamp"), MaxFrameTime);

					AllMs.Sort();
					int32 P95Idx = FMath::Clamp((int32)(AllMs.Num() * 0.95), 0, AllMs.Num() - 1);
					Root->SetNumberField(TEXT("p95_frame_ms"), FMath::RoundToFloat(AllMs[P95Idx] * 100.0f) / 100.0f);
				}

				// Worst 10 frames, each explained (issue #25): its biggest game-thread scopes by self time, the log
				// lines at that moment, and the causes they point to.
				TArray<TSharedPtr<FJsonValue>> WorstArr;
				struct FFrameEntry { double Ms; uint64 Index; double StartTime; double EndTime; };
				TArray<FFrameEntry> Entries;
				Entries.Reserve((int32)FMath::Min(FrameCount, (uint64)10000));
				Frames->EnumerateFrames(ETraceFrameType::TraceFrameType_Game, 0, FrameCount,
					[&](const TraceServices::FFrame& F)
					{
						if (F.EndTime <= F.StartTime) return;
						double Ms = (F.EndTime - F.StartTime) * 1000.0;
						if (!FMath::IsFinite(Ms) || Ms > 120000.0) return;
						Entries.Add({ Ms, F.Index, F.StartTime, F.EndTime });
					});

				// Issue #31: line the CSV up with the trace's frames, so a trace frame can be matched with the CSV's
				// EVENTS at that frame (GC, FlushAsyncLoading...).
				int32 CsvOffset = INDEX_NONE;
				if (Csv.bOk && Csv.Cols.bFrame)
				{
					TArray<double> TraceMsByIndex;
					TraceMsByIndex.SetNumZeroed((int32)FMath::Min(FrameCount, (uint64)MAX_int32));
					for (const FFrameEntry& E : Entries) if (E.Index < (uint64)TraceMsByIndex.Num()) TraceMsByIndex[(int32)E.Index] = E.Ms;
					TArray<double> CsvMs;
					CsvMs.Reserve(Csv.Samples.Num());
					for (const BoundHoundVerdict::FThreadSample& S : Csv.Samples) CsvMs.Add(S.FrameMs);
					double AlignErr = 0.0;
					CsvOffset = BoundHoundDrilldown::AlignCsvToTrace(TraceMsByIndex, CsvMs, &AlignErr);
					TSharedPtr<FJsonObject> AObj = MakeShared<FJsonObject>();
					AObj->SetBoolField(TEXT("aligned"), CsvOffset != INDEX_NONE);
					if (CsvOffset != INDEX_NONE) AObj->SetNumberField(TEXT("trace_frame_of_first_csv_row"), CsvOffset);
					AObj->SetNumberField(TEXT("mean_error_ms"), FMath::RoundToDouble(AlignErr * 100.0) / 100.0);
					Root->SetObjectField(TEXT("csv_alignment"), AObj);
				}
				auto CsvEventsAt = [&](uint64 FrameIndex) -> TArray<FString>
				{
					if (CsvOffset == INDEX_NONE || Csv.Events.IsEmpty()) return {};
					return BoundHoundDrilldown::CsvEventsNear(Csv.Events, (int32)((int64)FrameIndex - CsvOffset));
				};

				Entries.Sort([](const FFrameEntry& A, const FFrameEntry& B) { return A.Ms > B.Ms; });

				const FFrameExplainer Explainer(*Session);
				int32 WorstCount = FMath::Min(10, Entries.Num());
				for (int32 i = 0; i < WorstCount; ++i)
				{
					TSharedPtr<FJsonObject> FObj = MakeShared<FJsonObject>();
					FObj->SetNumberField(TEXT("frame"), (double)Entries[i].Index);
					FObj->SetNumberField(TEXT("ms"), FMath::RoundToFloat(Entries[i].Ms * 100.0f) / 100.0f);
					FObj->SetNumberField(TEXT("timestamp"), Entries[i].StartTime);
					FObj->SetStringField(TEXT("phase"), (Entries[i].Index == 0 || Entries[i].Ms > LOAD_FRAME_MS) ? TEXT("load") : TEXT("run"));
					Explainer.Explain(Entries[i].StartTime, Entries[i].EndTime, FObj, CsvEventsAt(Entries[i].Index));
					WorstArr.Add(MakeShared<FJsonValueObject>(FObj));
				}
				Root->SetArrayField(TEXT("worst_frames"), WorstArr);
				if (!Explainer.HasScopes())
				{
					Root->SetStringField(TEXT("drilldown_note"), TEXT("No game-thread CPU scopes in this trace (needs the 'cpu' channel), so worst frames carry log lines only."));
				}

				// Issue #31: a frame-rate cap (t.MaxFPS / VSync) pins the frame time, so frame stats measure the cap,
				// not the game. Detected from the CSV: frames sit on one value well above the slowest thread.
				BoundHoundVerdict::FFrameCap Cap;
				FString BottleneckName;
				double BottleneckMs = 0.0;
				if (Csv.bOk)
				{
					BottleneckMs = Csv.BottleneckMedianMs(&BottleneckName);
					TArray<double> FrameSeries;
					if (Csv.Cols.bFrame) { FrameSeries.Reserve(Csv.Samples.Num()); for (const BoundHoundVerdict::FThreadSample& S : Csv.Samples) FrameSeries.Add(S.FrameMs); }
					else FrameSeries = AllMs;
					Cap = BoundHoundVerdict::DetectFrameCap(FrameSeries, BottleneckMs);
					TSharedPtr<FJsonObject> CObj = MakeShared<FJsonObject>();
					CObj->SetBoolField(TEXT("capped"), Cap.bCapped);
					CObj->SetNumberField(TEXT("frame_median_ms"), FMath::RoundToDouble(Cap.FrameMedianMs * 100.0) / 100.0);
					CObj->SetStringField(TEXT("bottleneck"), BottleneckName);
					CObj->SetNumberField(TEXT("bottleneck_median_ms"), FMath::RoundToDouble(BottleneckMs * 100.0) / 100.0);
					if (Cap.bCapped)
					{
						CObj->SetNumberField(TEXT("cap_fps"), Cap.CapFps);
						CObj->SetNumberField(TEXT("cap_budget_ms"), FMath::RoundToDouble(Cap.BudgetMs * 100.0) / 100.0);
						CObj->SetNumberField(TEXT("headroom_ms"), FMath::RoundToDouble(Cap.HeadroomMs * 100.0) / 100.0);
						CObj->SetStringField(TEXT("note"), FString::Printf(
							TEXT("Frame-rate capped at %d fps (t.MaxFPS, VSync or a frame-rate limit setting): avg_fps / p95_frame_ms measure the cap, not the game. The real work is the %s median, %.2f ms, leaving %.2f ms headroom in the %.2f ms cap budget (~%.0f fps of work). Uncap (t.MaxFPS 0, r.VSync 0) to measure the game's own frame rate."),
							Cap.CapFps, *BottleneckName, BottleneckMs, Cap.HeadroomMs, Cap.BudgetMs, BottleneckMs > 0.0 ? 1000.0 / BottleneckMs : 0.0));
					}
					Root->SetObjectField(TEXT("frame_cap"), CObj);
				}

				// Periodic spikes (issue #25): run-phase frames well above the typical frame, merged into events,
				// checked for a fixed interval -- e.g. GC every ~61 s, which is invisible frame by frame.
				if (!AllMs.IsEmpty())
				{
					const double MedianMs = AllMs[AllMs.Num() / 2]; // AllMs was sorted above
					const double SpikeMs = BoundHoundVerdict::SpikeThresholdMs(MedianMs, Cap, BottleneckMs);
					TArray<double> SpikeTimes;
					for (const FFrameEntry& E : Entries)
					{
						if (E.Index != 0 && E.Ms <= LOAD_FRAME_MS && E.Ms >= SpikeMs) SpikeTimes.Add(E.StartTime);
					}
					const TArray<double> Merged = BoundHoundDrilldown::MergeSpikes(SpikeTimes);
					const BoundHoundDrilldown::FPeriodicity P = BoundHoundDrilldown::DetectPeriodicSpikes(Merged);
					TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
					PObj->SetNumberField(TEXT("spike_threshold_ms"), FMath::RoundToDouble(SpikeMs * 100.0) / 100.0);
					PObj->SetStringField(TEXT("spike_threshold_basis"), Cap.bCapped
						? FString::Printf(TEXT("capped run: 3x the %s median, at least one dropped frame at the %d fps cap"), *BottleneckName, Cap.CapFps)
						: FString(TEXT("3x the median frame, at least 33.3 ms")));

					// Issue #31: explain each spike event's worst frame. The worst-frames list is usually all load
					// frames, so a mid-run GC hitch would otherwise never be explained. Worst events first, then
					// listed in time order.
					TArray<const FFrameEntry*> EventWorst;
					for (int32 m = 0; m < FMath::Min(Merged.Num(), 200); ++m)
					{
						for (const FFrameEntry& E : Entries) // sorted worst first
						{
							if (E.Index != 0 && E.Ms <= LOAD_FRAME_MS && E.Ms >= SpikeMs && E.StartTime >= Merged[m] && E.StartTime < Merged[m] + 1.0)
							{
								EventWorst.Add(&E);
								break;
							}
						}
					}
					EventWorst.Sort([](const FFrameEntry& A, const FFrameEntry& B) { return A.Ms > B.Ms; });
					if (EventWorst.Num() > MAX_SPIKE_FRAMES) EventWorst.SetNum(MAX_SPIKE_FRAMES);
					EventWorst.Sort([](const FFrameEntry& A, const FFrameEntry& B) { return A.StartTime < B.StartTime; });
					TArray<TSharedPtr<FJsonValue>> SpikeFrameArr;
					for (const FFrameEntry* E : EventWorst)
					{
						TSharedPtr<FJsonObject> FObj = MakeShared<FJsonObject>();
						FObj->SetNumberField(TEXT("frame"), (double)E->Index);
						FObj->SetNumberField(TEXT("ms"), FMath::RoundToFloat(E->Ms * 100.0f) / 100.0f);
						FObj->SetNumberField(TEXT("timestamp"), E->StartTime);
						FObj->SetStringField(TEXT("phase"), TEXT("run"));
						Explainer.Explain(E->StartTime, E->EndTime, FObj, CsvEventsAt(E->Index));
						SpikeFrameArr.Add(MakeShared<FJsonValueObject>(FObj));
					}
					PObj->SetArrayField(TEXT("spike_frames"), SpikeFrameArr);
					PObj->SetNumberField(TEXT("spike_events"), Merged.Num());
					PObj->SetBoolField(TEXT("periodic"), P.bPeriodic);
					if (P.bPeriodic)
					{
						PObj->SetNumberField(TEXT("interval_s"), FMath::RoundToDouble(P.IntervalS * 10.0) / 10.0);
						PObj->SetNumberField(TEXT("matching_spikes"), P.Count);
						if (!P.Likely.IsEmpty()) PObj->SetStringField(TEXT("likely"), P.Likely);
					}
					TArray<TSharedPtr<FJsonValue>> TimesArr;
					for (int32 i = 0; i < FMath::Min(Merged.Num(), 20); ++i)
					{
						TimesArr.Add(MakeShared<FJsonValueNumber>(FMath::RoundToDouble(Merged[i] * 10.0) / 10.0));
					}
					PObj->SetArrayField(TEXT("spike_times_s"), TimesArr);
					Root->SetObjectField(TEXT("spikes"), PObj);
				}
			}
		}
	}

	return OkJson(Root);
}

// ---------------------------------------------------------------------------
// Log analysis
// ---------------------------------------------------------------------------

static FString AnalyseLogs(const FString& LogFile)
{
	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *LogFile))
	{
		// LoadFileToString fails both when the file is absent AND when it exists but is locked open by
		// another process (e.g. the editor's own live log). Distinguish the two so the caller isn't
		// misled into thinking a present-but-locked log is missing.
		if (FPaths::FileExists(LogFile))
		{
			return ErrJson(TEXT("LOG_LOCKED"), FString::Printf(TEXT("Log file exists but could not be read (likely held open by another process): %s"), *LogFile));
		}
		return ErrJson(TEXT("LOG_NOT_FOUND"), FString::Printf(TEXT("Log file not found: %s"), *LogFile));
	}

	TArray<FString> Lines;
	Content.ParseIntoArrayLines(Lines);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("log_file"), LogFile);
	Root->SetNumberField(TEXT("total_lines"), Lines.Num());

	TArray<TSharedPtr<FJsonValue>> Notable;
	int32 PSOHitches = 0;
	int32 ErrorCount = 0;
	int32 WarningCount = 0;

	static const TArray<FString> NotablePatterns = {
		TEXT("hitch"), TEXT("Hitch"),
		TEXT("PSO"), TEXT("RTPSO"),
		TEXT("blocking load"), TEXT("Blocking Load"),
		TEXT("took "), TEXT(" ms."),
		TEXT("LogShaderCompilers"), TEXT("NiagaraSystem"),
		TEXT("async load"), TEXT("Async Load"),
		TEXT("streamable"), TEXT("Streamable"),
		TEXT("OutOfMemory"), TEXT("out of memory"),
	};

	// Issue #25: notable_lines are about the run, not startup. Lines only count once the first frame has
	// started (first non-zero frame tag in the log prefix), and config/plugin/font setup lines are dropped.
	bool bRunStarted = false;
	int32 BootLinesSkipped = 0;
	FString RunCommandLine;
	TMap<FString, FString> RunMetadata;

	for (const FString& Line : Lines)
	{
		bool bError   = Line.Contains(TEXT("] Error:"))   || Line.Contains(TEXT(":Error:"));
		bool bWarning = Line.Contains(TEXT("] Warning:")) || Line.Contains(TEXT(":Warning:"));
		if (bError)   ++ErrorCount;
		if (bWarning) ++WarningCount;

		if (Line.Contains(TEXT("PSO creation hitch"))) ++PSOHitches;
		if (RunCommandLine.IsEmpty() && Line.Contains(TEXT("LogInit: Command Line:"))) RunCommandLine = Line;
		{
			FString MetaKey, MetaValue; // issue #6: what the run ran on, from the CSV profiler's metadata lines
			if (BoundHoundBaseline::ParseMetadataLine(Line, MetaKey, MetaValue)) RunMetadata.Add(MetaKey.ToLower(), MetaValue);
		}

		if (!bRunStarted && BoundHoundDrilldown::LogFrameTag(Line) > 0) bRunStarted = true;
		if (!bError && (!bRunStarted || BoundHoundDrilldown::IsBootNoise(Line)))
		{
			++BootLinesSkipped;
			continue;
		}

		bool bNotable = bError;
		if (!bNotable)
		{
			for (const FString& Pat : NotablePatterns)
			{
				if (Line.Contains(Pat)) { bNotable = true; break; }
			}
		}
		if (bNotable && Notable.Num() < 40)
		{
			Notable.Add(MakeShared<FJsonValueString>(Line.TrimStartAndEnd()));
		}
	}

	Root->SetNumberField(TEXT("errors"), ErrorCount);
	Root->SetNumberField(TEXT("warnings"), WarningCount);
	Root->SetNumberField(TEXT("pso_hitches"), PSOHitches);
	Root->SetArrayField(TEXT("notable_lines"), Notable);
	Root->SetNumberField(TEXT("startup_lines_skipped"), BootLinesSkipped);

	// Issue #26: the resolution / window mode this run actually used, read from its own command line, so
	// runs can be checked for comparability.
	int32 RunResX = 0, RunResY = 0; FString RunMode;
	if (BoundHoundVerdict::ParseRunDisplay(RunCommandLine, RunResX, RunResY, RunMode))
	{
		TSharedPtr<FJsonObject> D = MakeShared<FJsonObject>();
		if (RunResX > 0 && RunResY > 0) D->SetStringField(TEXT("resolution"), FString::Printf(TEXT("%dx%d"), RunResX, RunResY));
		if (!RunMode.IsEmpty()) D->SetStringField(TEXT("window_mode"), RunMode);
		Root->SetObjectField(TEXT("run_display"), D);
	}

	// Issue #6: what this run ran on (engine, platform, config, GPU, resolution, map...), for baselines.
	const TMap<FString, FString> RunIdentity = BoundHoundBaseline::IdentityFromMetadata(RunMetadata);
	if (RunIdentity.Num() > 0)
	{
		TSharedPtr<FJsonObject> Id = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& KV : RunIdentity) Id->SetStringField(KV.Key, KV.Value);
		Root->SetObjectField(TEXT("run_identity"), Id);
	}

	return OkJson(Root);
}

static FString AnalyseBoth(const FString& TraceFile, const FString& LogFile)
{
	FString TraceResult = AnalyseTrace(TraceFile);
	FString LogResult   = AnalyseLogs(LogFile);

	auto ParseOrError = [](const FString& Json, const FString& Label) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Obj;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(R, Obj) || !Obj.IsValid())
		{
			Obj = MakeShared<FJsonObject>();
			Obj->SetBoolField(TEXT("success"), false);
			Obj->SetStringField(TEXT("error"), FString::Printf(TEXT("%s result was not valid JSON"), *Label));
			Obj->SetStringField(TEXT("raw"), Json.Left(200));
		}
		return Obj;
	};

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("success"), true);
	Root->SetStringField(TEXT("source"), TEXT("both"));
	Root->SetObjectField(TEXT("trace"), ParseOrError(TraceResult, TEXT("trace")));
	Root->SetObjectField(TEXT("logs"),  ParseOrError(LogResult,   TEXT("logs")));

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root.ToSharedRef(), W);
	return Out;
}

// ===========================================================================
// AICallable methods
// ===========================================================================

// Per-thread advice keyed by the winning bottleneck. Kept next to FrameTiming so the verdict and its
// remediation hint stay in lockstep.
static FString BoundHint(const FString& Bound)
{
	if (Bound == TEXT("GPU"))
	{
		return TEXT("GPU-bound. NOW it is worth profiling the GPU: StartTrace (channels frame,gpu,cpu), then 'r.ProfileGPU.ShowUI 0' + 'ProfileGPU' and read the pass breakdown from the log. Levers: shadows (Virtual Shadow Maps), Lumen GI/reflections, translucency, post-process, ScreenPercentage. Confirm with the resolution test: 'r.ScreenPercentage 50' should noticeably raise FPS if truly GPU-bound.");
	}
	if (Bound == TEXT("RenderThread"))
	{
		return TEXT("CPU render-thread bound. Usual cause: too many draw calls / primitives, or many dynamic shadow-casting lights. Check 'stat scenerendering'. Levers: merge/instance meshes, enable Nanite, cut dynamic lights and per-light shadows. NOTE: dropping r.ScreenPercentage will NOT help a render-thread-bound frame.");
	}
	if (Bound == TEXT("RHIThread"))
	{
		return TEXT("CPU RHI-thread bound -- the thread submitting GPU commands is the bottleneck, distinct from render-thread scene setup. Usual cause: too many draw calls / state changes saturating command submission. Check 'stat rhi' and 'stat scenerendering' (draw-call count). Levers: cut draw calls via merging/instancing/HISM, enable Nanite, reduce material/state permutations. NOTE: dropping r.ScreenPercentage will NOT help an RHI-thread-bound frame.");
	}
	return TEXT("CPU game-thread bound. Usual cause: Tick / Blueprint / AI / animation cost. Run 'stat dumpframe -ms=0.5 -root=gamethread' on the PIE world then read the result. Levers: throttle/disable unnecessary Tick, reduce ticking actors & AI, cut expensive Blueprint tick logic. NOTE: dropping r.ScreenPercentage will NOT help a game-thread-bound frame.");
}

FString UBoundHoundService::FrameTiming(float TargetFPS)
{
	const double GameMs   = FPlatformTime::ToMilliseconds(GGameThreadTime);
	const double RenderMs = FPlatformTime::ToMilliseconds(GRenderThreadTime);
	const double RHIMs    = FPlatformTime::ToMilliseconds(GRHIThreadTime);
	const double GpuMs    = FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles());

	auto Round2 = [](double V) -> double { return FMath::RoundToDouble(V * 100.0) / 100.0; };

	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetNumberField(TEXT("game_thread_ms"),   Round2(GameMs));
	R->SetNumberField(TEXT("render_thread_ms"), Round2(RenderMs));
	R->SetNumberField(TEXT("rhi_thread_ms"),    Round2(RHIMs));
	R->SetNumberField(TEXT("gpu_ms"),           Round2(GpuMs));

	const bool bRhiAvailable = RHIMs > 0.0;
	const bool bGpuAvailable = GpuMs > 0.0;
	// Every ranked thread runs in parallel, so the frame is bounded by the slowest of them. RHIMs/GpuMs are
	// 0 when unavailable, so Max is safe -- an absent thread can never be the max.
	const double FrameMs = FMath::Max(FMath::Max3(GameMs, RenderMs, GpuMs), RHIMs);
	R->SetNumberField(TEXT("frame_ms"), Round2(FrameMs));
	R->SetNumberField(TEXT("fps"), FrameMs > 0.0 ? Round2(1000.0 / FrameMs) : 0.0);

	// --- Robust verdict -------------------------------------------------------
	// Rank the candidate threads (GPU only counts when its timing is available) and judge how decisive
	// the winner is. Pure logic lives in BoundHoundVerdict::Classify so it can be unit-tested headless.
	const BoundHoundVerdict::FVerdict Verdict = BoundHoundVerdict::Classify(GameMs, RenderMs, RHIMs, GpuMs, bRhiAvailable, bGpuAvailable);
	const FString Bound      = Verdict.Bound;
	const double  TopMs      = Verdict.TopMs;
	const double  RunnerMs   = Verdict.RunnerMs;
	const FString RunnerName = Verdict.RunnerName;
	const double  MarginMs   = Verdict.MarginMs;
	const bool    bContested = Verdict.bContested;

	// --- Freshness (issue #27) + warm-up (issue #23) ---------------------------------------------------
	// The thread globals describe the last COMPLETED frame. If no frame has completed since the previous
	// read (caller is holding the game thread), this is the same frame again -- not a new sample.
	const uint64 FrameNumber = GFrameCounter;
	const bool bRepeatRead = BoundHoundVerdict::IsRepeatRead(GHasLastTimingFrame, FrameNumber, GLastTimingFrame);
	GHasLastTimingFrame = true;
	GLastTimingFrame    = FrameNumber;
	R->SetNumberField(TEXT("frame_number"), (double)FrameNumber);
	R->SetStringField(TEXT("timestamp"), FDateTime::UtcNow().ToIso8601());
	R->SetBoolField(TEXT("repeat_read"), bRepeatRead);

	const bool bPIE = IsPIERunning();
	const double PieSeconds = bPIE ? GEditor->PlayWorld->GetRealTimeSeconds() : -1.0;
	const bool bWarming = bPIE && BoundHoundVerdict::IsWarming(PieSeconds);
	if (bPIE) R->SetNumberField(TEXT("pie_seconds"), Round2(PieSeconds));
	R->SetBoolField(TEXT("warming"), bWarming);

	R->SetStringField(TEXT("bound"), Bound);
	R->SetStringField(TEXT("bound_confidence"), bRepeatRead ? FString(TEXT("none")) : Verdict.Confidence);
	R->SetNumberField(TEXT("margin_ms"), Round2(MarginMs));
	R->SetBoolField(TEXT("contested"), bContested);

	if (bContested && !RunnerName.IsEmpty())
	{
		R->SetStringField(TEXT("contested_with"), RunnerName);
	}

	FString Hint = BoundHint(Bound);
	if (bRepeatRead)
	{
		Hint = FString::Printf(
			TEXT("REPEAT READ: no new frame since the last call (frame %llu) -- this is the same frame again, not a new sample. Return control to the engine between samples (separate Python calls / MCP requests); sleeping inside one call does not let frames advance."),
			FrameNumber);
	}
	else if (bWarming)
	{
		Hint = FString::Printf(
			TEXT("WARMING: PIE started %.1f s ago -- early frames are dominated by shader/PSO compiles and streaming and are not representative. Read again after ~%.0f s. %s"),
			PieSeconds, BoundHoundVerdict::SETTLE_SECONDS, *BoundHint(Bound));
	}
	else if (bContested && !RunnerName.IsEmpty())
	{
		Hint = FString::Printf(
			TEXT("CONTESTED: %s (%.2f ms) and %s (%.2f ms) are within %.2f ms -- frame-to-frame noise can flip which one 'wins', so treat both as bottlenecks. Take several readings, or trace both. %s"),
			*Bound, TopMs, *RunnerName, RunnerMs, MarginMs, *BoundHint(Bound));
	}
	else if (!bGpuAvailable && Bound != TEXT("GPU"))
	{
		Hint = FString::Printf(
			TEXT("%s NOTE: GPU timing is unavailable this frame, so this CPU verdict is unconfirmed -- a hidden GPU cost could still be the real bottleneck. Confirm with the 'r.ScreenPercentage 50' test."),
			*Hint);
	}
	R->SetStringField(TEXT("hint"), Hint);

	// --- Frame-time budget gate ----------------------------------------------
	// FPS is 1000/frame_ms, and the threads run in parallel, so EACH thread must individually finish
	// inside the per-frame budget. Report every thread's headroom against the target and gate on the
	// bottleneck -- this is what turns a one-shot reading into a pass/fail guard.
	const BoundHoundVerdict::FBudget Gate = BoundHoundVerdict::ComputeBudget(FrameMs, TargetFPS);
	const double BudgetMs = Gate.BudgetMs;

	TSharedPtr<FJsonObject> Budget = MakeShared<FJsonObject>();
	Budget->SetNumberField(TEXT("target_fps"), Round2(Gate.TargetFps));
	Budget->SetNumberField(TEXT("budget_ms"), Round2(BudgetMs));

	auto ThreadBudget = [&](double Ms, bool bAvailable) -> TSharedPtr<FJsonValue>
	{
		TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
		T->SetNumberField(TEXT("ms"), Round2(Ms));
		if (bAvailable)
		{
			T->SetNumberField(TEXT("headroom_ms"), Round2(BudgetMs - Ms));
			T->SetBoolField(TEXT("over_budget"), BoundHoundVerdict::IsOverBudget(Ms, BudgetMs));
		}
		else
		{
			T->SetBoolField(TEXT("available"), false);
		}
		return MakeShared<FJsonValueObject>(T);
	};

	TSharedPtr<FJsonObject> PerThread = MakeShared<FJsonObject>();
	PerThread->SetField(TEXT("game_thread"),   ThreadBudget(GameMs, true));
	PerThread->SetField(TEXT("render_thread"), ThreadBudget(RenderMs, true));
	PerThread->SetField(TEXT("gpu"),           ThreadBudget(GpuMs, bGpuAvailable));
	Budget->SetObjectField(TEXT("threads"), PerThread);

	Budget->SetNumberField(TEXT("frame_headroom_ms"), Round2(Gate.FrameHeadroomMs));
	const bool bMeetsTarget = Gate.bMeetsTarget;
	Budget->SetBoolField(TEXT("meets_target"), bMeetsTarget);
	Budget->SetStringField(TEXT("verdict"), bMeetsTarget ? TEXT("PASS") : TEXT("FAIL"));
	if (!bMeetsTarget && FrameMs > 0.0)
	{
		Budget->SetStringField(TEXT("budget_note"), FString::Printf(
			TEXT("%s is %.2f ms over the %.2f ms budget for %g FPS -- that thread alone caps you at ~%.0f FPS. Fix the bottleneck thread, not whichever is cheapest."),
			*Bound, TopMs - BudgetMs, BudgetMs, Gate.TargetFps, FrameMs > 0.0 ? 1000.0 / FrameMs : 0.0));
	}
	R->SetObjectField(TEXT("budget"), Budget);

	R->SetBoolField(TEXT("pie_running"), bPIE);
	R->SetStringField(TEXT("note"),
		bPIE
		? TEXT("Values are for the most recently rendered PIE frame. Park in a representative/worst spot for a clean read.")
		: TEXT("PIE is NOT running -- these values reflect the EDITOR viewport, not your game. Start PIE (Epic's EditorAppToolset.StartPIE) for a real game-bound reading."));
	if (GpuMs <= 0.0)
	{
		R->SetStringField(TEXT("gpu_note"), TEXT("gpu_ms is 0 (GPU timing unavailable this frame) -- rely on the game vs render comparison, and confirm GPU-bound with the r.ScreenPercentage 50 test."));
	}
	return OkJson(R);
}

FString UBoundHoundService::ForceHitch(const FString& Thread, float Milliseconds, int32 Frames)
{
	const FString Mode = Thread.IsEmpty() ? TEXT("game") : Thread.ToLower();
	const bool bGame   = (Mode == TEXT("game")   || Mode == TEXT("both"));
	const bool bRender = (Mode == TEXT("render") || Mode == TEXT("both"));
	const bool bRhi    = (Mode == TEXT("rhi"));
	const bool bGpu    = (Mode == TEXT("gpu"));

	if (!bGame && !bRender && !bRhi && !bGpu)
	{
		return ErrJson(TEXT("BAD_THREAD"), FString::Printf(
			TEXT("Unknown Thread '%s'. Use 'game', 'render', 'both', 'rhi', or 'gpu'."), *Thread));
	}

	// Clamp so a fat-fingered value can't freeze the editor for a minute or spin forever.
	const float StallMs      = FMath::Clamp(Milliseconds, 1.0f, 5000.0f);
	const int32 HitchFrames  = FMath::Clamp(Frames, 1, 600);
	const float StallSeconds = StallMs / 1000.0f;

	// Tell the caller exactly what to expect from the verdict, so validation is a direct compare.
	const TCHAR* Expect =
		  (Mode == TEXT("both"))   ? TEXT("contested=true (GameThread and RenderThread within margin)")
		: (Mode == TEXT("render"))? TEXT("bound=RenderThread")
		: (Mode == TEXT("rhi"))   ? TEXT("bound=RHIThread WHEN RHI-threading is on; otherwise the stall folds into the render thread (no distinct RHIThread verdict)")
		: (Mode == TEXT("gpu"))   ? TEXT("bound=GPU IF the scene's GPU cost now exceeds the CPU threads (scene-dependent)")
		:                           TEXT("bound=GameThread");

	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("forcing"), Mode);
	R->SetStringField(TEXT("expect"), Expect);

	// GPU forcing stays async: supersample by driving r.ScreenPercentage up, restored when the countdown
	// ends over HitchFrames frames. It's the only scene-agnostic GPU lever we have -- the actual cost still
	// depends on what's on screen, so a trivial PIE scene may not tip the frame GPU-bound (best-effort). GPU
	// cost persists across the frames the reader sees, so unlike the CPU paths a follow-up FrameTiming reads
	// it back cleanly -- which is why gpu keeps the read-a-following-frame flow instead of self-measuring.
	if (bGpu)
	{
		constexpr float GpuScreenPercentage = 400.0f;
		float SavedScreenPercentage = 100.0f;
		IConsoleVariable* ScreenPctCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage"));
		if (ScreenPctCVar)
		{
			SavedScreenPercentage = ScreenPctCVar->GetFloat();
			ScreenPctCVar->Set(GpuScreenPercentage, ECVF_SetByConsole);
		}

		TSharedRef<int32> Remaining = MakeShared<int32>(HitchFrames);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Remaining, ScreenPctCVar, SavedScreenPercentage](float) -> bool
			{
				if (--(*Remaining) > 0)
				{
					return true; // keep the supersample up for the next frame
				}
				if (ScreenPctCVar)
				{
					ScreenPctCVar->Set(SavedScreenPercentage, ECVF_SetByConsole); // restore + unregister
				}
				return false;
			}));

		R->SetNumberField(TEXT("frames"), HitchFrames);
		if (ScreenPctCVar)
		{
			R->SetNumberField(TEXT("screen_percentage"), GpuScreenPercentage);
			R->SetNumberField(TEXT("restore_screen_percentage"), SavedScreenPercentage);
		}
		else
		{
			R->SetStringField(TEXT("gpu_warning"), TEXT("r.ScreenPercentage cvar not found -- GPU load NOT applied."));
		}
		if (!IsPIERunning())
		{
			R->SetStringField(TEXT("note"), TEXT("PIE is not running -- the GPU load lands on the editor viewport frame. Start PIE for a game-representative validation."));
		}
		R->SetStringField(TEXT("hint"), TEXT("GPU cost is scene-dependent and not self-measured -- call FrameTiming on a following frame and confirm bound=GPU. Unlike the CPU paths this reads back cleanly (the GPU cost persists across frames)."));
		return OkJson(R);
	}

	// CPU paths (game/render/both) -- SELF-MEASURING, so validation is race-free (issue #17). The old
	// approach scheduled the stall on a future frame and relied on a follow-up FrameTiming to catch it, but
	// that read runs on the game thread the stall blocks and reliably lands on a clean frame. Here we apply
	// the stall synchronously inside this call and time it directly, then return the induced peak per-thread
	// ms and a verdict_matched_expect flag -- the caller validates from THIS single response, no second call.
	// Cap total synchronous blocking so a fat-fingered frames*ms can't freeze the editor for minutes.
	constexpr double TotalCapMs = 10000.0;
	const int32 EffectiveFrames = FMath::Min(HitchFrames, FMath::Max(1, (int32)(TotalCapMs / StallMs)));

	// "rhi" self-measures only when a separate RHI thread exists to stall. With RHI-threading off the RHI
	// work runs inline on the render thread, so there's no distinct RHIThread to bind the frame -- we skip
	// the stall and flag it, mirroring how the verdict drops RHI from the ranking in that case (issue #2).
	const bool bRhiThreaded = bRhi && IsRunningRHIInSeparateThread();

	double PeakGameMs = 0.0;
	double PeakRenderMs = 0.0;
	double PeakRhiMs = 0.0;
	for (int32 i = 0; i < EffectiveFrames; ++i)
	{
		if (bGame)
		{
			const double T0 = FPlatformTime::Seconds();
			FPlatformProcess::Sleep(StallSeconds);
			PeakGameMs = FMath::Max(PeakGameMs, (FPlatformTime::Seconds() - T0) * 1000.0);
		}
		if (bRender)
		{
			// Stall the render thread and have IT time its own sleep into a shared slot; FlushRenderingCommands
			// blocks the game thread until that command completes, so the measurement is done before we read it.
			TSharedRef<double, ESPMode::ThreadSafe> RtMs = MakeShared<double, ESPMode::ThreadSafe>(0.0);
			const float RtSeconds = StallSeconds;
			ENQUEUE_RENDER_COMMAND(BoundHoundForceHitch)(
				[RtSeconds, RtMs](FRHICommandListImmediate&)
				{
					const double T0 = FPlatformTime::Seconds();
					FPlatformProcess::Sleep(RtSeconds);
					*RtMs = (FPlatformTime::Seconds() - T0) * 1000.0;
				});
			FlushRenderingCommands();
			PeakRenderMs = FMath::Max(PeakRenderMs, *RtMs);
		}
		if (bRhiThreaded)
		{
			// Stall the RHI thread specifically: from the render thread, enqueue a lambda onto the command
			// list (it executes on the RHI thread when threading is on) that times its own sleep, then flush
			// the RHI thread so the game thread blocks until that lambda has run and the slot is filled.
			TSharedRef<double, ESPMode::ThreadSafe> RhMs = MakeShared<double, ESPMode::ThreadSafe>(0.0);
			const float RhSeconds = StallSeconds;
			ENQUEUE_RENDER_COMMAND(BoundHoundForceHitchRHI)(
				[RhSeconds, RhMs](FRHICommandListImmediate& RHICmdList)
				{
					RHICmdList.EnqueueLambda([RhSeconds, RhMs](FRHICommandListBase&)
					{
						const double T0 = FPlatformTime::Seconds();
						FPlatformProcess::Sleep(RhSeconds);
						*RhMs = (FPlatformTime::Seconds() - T0) * 1000.0;
					});
					RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
				});
			FlushRenderingCommands();
			PeakRhiMs = FMath::Max(PeakRhiMs, *RhMs);
		}
	}

	// A thread counts as "hit" if it induced at least half the requested stall (tolerates scheduler slop).
	const double MinExpectedMs = 0.5 * StallMs;
	const bool bMatched = BoundHoundVerdict::HitchMatchesExpect(Mode, PeakGameMs, PeakRenderMs, MinExpectedMs, PeakRhiMs);

	R->SetNumberField(TEXT("frames"), EffectiveFrames);
	if (EffectiveFrames < HitchFrames)
	{
		R->SetNumberField(TEXT("frames_requested"), HitchFrames);
		R->SetStringField(TEXT("frames_note"), FString::Printf(
			TEXT("Capped to %d frame(s) (~%.0f ms total) so the synchronous stall can't freeze the editor."),
			EffectiveFrames, TotalCapMs));
	}
	R->SetNumberField(TEXT("stall_ms_per_frame"), StallMs);
	if (bGame)   { R->SetNumberField(TEXT("observed_peak_game_ms"), PeakGameMs); }
	if (bRender) { R->SetNumberField(TEXT("observed_peak_render_ms"), PeakRenderMs); }
	if (bRhi)
	{
		R->SetBoolField(TEXT("rhi_threading"), bRhiThreaded);
		if (bRhiThreaded)
		{
			R->SetNumberField(TEXT("observed_peak_rhi_ms"), PeakRhiMs);
		}
		else
		{
			R->SetStringField(TEXT("rhi_warning"), TEXT("RHI-threading is OFF -- no separate RHI thread to stall, so no RHIThread verdict was induced. Re-run with RHI-threading on (r.RHIThread.Enable 1, or a platform where it's default) to validate the RHIThread bound."));
		}
	}
	R->SetBoolField(TEXT("verdict_matched_expect"), bMatched);
	if (!IsPIERunning())
	{
		R->SetStringField(TEXT("note"), TEXT("PIE is not running -- the stall was still induced and measured directly on the editor frame, so verdict_matched_expect is valid. Start PIE for a game-representative FrameTiming reading."));
	}
	R->SetStringField(TEXT("hint"), TEXT("Validation is self-contained: 'verdict_matched_expect' compares the stall this call actually induced ('observed_peak_*_ms') against 'expect' -- no follow-up FrameTiming needed (that read races the stall on the game thread and misses it)."));
	return OkJson(R);
}

// ---------------------------------------------------------------------------
// Report -- render a self-contained HTML "printout" from the verdict + capture
// ---------------------------------------------------------------------------

// Verified UE 5.8 documentation URLs, linked per fix so the report tells you where to go next.
namespace BoundHoundDocs
{
	static const TCHAR* PsoPrecache = TEXT("https://dev.epicgames.com/documentation/en-us/unreal-engine/pso-precaching-for-unreal-engine");
	static const TCHAR* PsoCaches   = TEXT("https://dev.epicgames.com/documentation/en-us/unreal-engine/optimizing-rendering-with-pso-caches-in-unreal-engine");
	static const TCHAR* Profiling   = TEXT("https://dev.epicgames.com/documentation/en-us/unreal-engine/introduction-to-performance-profiling-and-configuration-in-unreal-engine");
	static const TCHAR* StatCommands= TEXT("https://dev.epicgames.com/documentation/en-us/unreal-engine/stat-commands-in-unreal-engine");
	static const TCHAR* Insights    = TEXT("https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-insights-in-unreal-engine");
}

// Escape text destined for HTML body/attributes (paths, log lines, verdict strings).
static FString BHHtmlEscape(const FString& In)
{
	FString Out = In;
	Out.ReplaceInline(TEXT("&"), TEXT("&amp;"));
	Out.ReplaceInline(TEXT("<"), TEXT("&lt;"));
	Out.ReplaceInline(TEXT(">"), TEXT("&gt;"));
	Out.ReplaceInline(TEXT("\""), TEXT("&quot;"));
	return Out;
}

// One recommended fix, rendered as a numbered card: what/why + the exact commands + a doc link.
struct FBHFix
{
	FString Title;
	FString Why;
	FString How;      // concrete stat/console commands -- shown in a mono block
	FString DocUrl;
	FString DocLabel;
};

// The report's stylesheet -- inlined so the file is fully self-contained and opens anywhere. Same visual
// identity as the shareable comparison page: mono headings, teal "trace" accent, semantic good/warn/crit,
// and both light + dark themes driven by tokens.
static const TCHAR* BHReportCss = TEXT(R"CSS(<style>
:root{--bg:#eef2f1;--surface:#fff;--surface-2:#f6f9f8;--ink:#13201e;--muted:#586b68;--border:#dde5e3;--border-strong:#c7d2d0;--accent:#0c9a8b;--accent-ink:#0a7c70;--good:#1c8f52;--good-bg:#1c8f5216;--warn:#b1780a;--warn-bg:#b1780a18;--crit:#cc453b;--crit-bg:#cc453b16;--mono:"SFMono-Regular","Cascadia Code","JetBrains Mono","Fira Code",Menlo,Consolas,monospace;--sans:system-ui,-apple-system,"Segoe UI",Roboto,Helvetica,Arial,sans-serif}
@media(prefers-color-scheme:dark){:root{--bg:#0c1312;--surface:#111b1a;--surface-2:#0f1817;--ink:#e7f0ee;--muted:#93a6a3;--border:#22302e;--border-strong:#2d3d3a;--accent:#3ad6c6;--accent-ink:#5ee0d2;--good:#45c07e;--good-bg:#45c07e1f;--warn:#e0a53c;--warn-bg:#e0a53c22;--crit:#ec6f63;--crit-bg:#ec6f6320}}
*{box-sizing:border-box}
body{margin:0}
.wrap{background:var(--bg);color:var(--ink);font-family:var(--sans);line-height:1.5;padding:clamp(20px,4vw,52px);-webkit-font-smoothing:antialiased}
.sheet{max-width:960px;margin:0 auto}
.eyebrow{font-family:var(--mono);font-size:12px;letter-spacing:.18em;text-transform:uppercase;color:var(--accent-ink);display:flex;align-items:center;gap:10px;margin:0 0 14px}
.eyebrow::before{content:"";width:26px;height:2px;background:var(--accent);display:inline-block}
h1{font-family:var(--mono);font-weight:650;font-size:clamp(24px,4vw,36px);letter-spacing:-.01em;line-height:1.1;margin:0 0 10px}
.lede{font-size:15px;color:var(--muted);max-width:64ch;margin:0 0 30px}
.tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:14px;margin:0 0 30px}
.tile{background:var(--surface);border:1px solid var(--border);border-radius:12px;padding:16px 18px;position:relative;overflow:hidden}
.tile::before{content:"";position:absolute;left:0;top:0;bottom:0;width:3px;background:var(--tc,var(--accent))}
.tile.good{--tc:var(--good)}.tile.warn{--tc:var(--warn)}.tile.crit{--tc:var(--crit)}
.tile .k{font-family:var(--mono);font-size:11px;letter-spacing:.1em;text-transform:uppercase;color:var(--muted);margin:0 0 8px}
.tile .v{font-family:var(--mono);font-size:clamp(22px,3vw,29px);font-weight:650;line-height:1;font-variant-numeric:tabular-nums}
.tile .v small{font-size:14px;font-weight:500;color:var(--muted);margin-left:3px}
.tile .s{font-size:12.5px;color:var(--muted);margin:8px 0 0}
thead th.rec{color:var(--accent-ink)}
tbody td{vertical-align:top}
.dim{font-weight:600;color:var(--ink);white-space:nowrap}
.dim small{display:block;font-weight:400;font-family:var(--mono);font-size:10.5px;letter-spacing:.04em;color:var(--muted);margin-top:3px;text-transform:uppercase}
td.warm,td.cold{color:var(--muted)}
td.warm b,td.cold b{color:var(--ink);font-family:var(--mono);font-variant-numeric:tabular-nums;font-weight:600}
td.rec-cell{color:var(--ink);border-left:1px solid var(--border)}
td.rec-cell code,td.warm code,td.cold code{font-family:var(--mono);font-size:12px;background:var(--surface-2);border:1px solid var(--border);padding:1px 5px;border-radius:4px}
.tag{display:inline-block;font-family:var(--mono);font-size:11px;font-weight:600;letter-spacing:.03em;padding:2px 7px;border-radius:5px;white-space:nowrap}
.tag.hidden{color:var(--crit);background:var(--crit-bg)}
.tag.caught{color:var(--good);background:var(--good-bg)}
.tag.warn{color:var(--warn);background:var(--warn-bg)}
h2{font-family:var(--mono);font-size:13px;letter-spacing:.1em;text-transform:uppercase;color:var(--accent-ink);margin:34px 0 14px}
.card{background:var(--surface);border:1px solid var(--border);border-radius:12px;overflow:hidden}
.table-scroll{overflow-x:auto}
table{border-collapse:collapse;width:100%;min-width:520px;font-size:14px}
thead th{text-align:left;font-family:var(--mono);font-size:11px;letter-spacing:.08em;text-transform:uppercase;color:var(--muted);font-weight:600;padding:12px 16px;background:var(--surface-2);border-bottom:1px solid var(--border-strong);white-space:nowrap}
tbody td{padding:11px 16px;border-bottom:1px solid var(--border);font-variant-numeric:tabular-nums}
tbody tr:last-child td{border-bottom:none}
tbody tr.bound td{background:var(--accent-wash,transparent)}
.th-name{font-family:var(--mono);font-weight:600}
.num{font-family:var(--mono)}
.pill{display:inline-block;font-family:var(--mono);font-size:11px;font-weight:600;padding:2px 8px;border-radius:20px;white-space:nowrap}
.pill.good{color:var(--good);background:var(--good-bg)}.pill.warn{color:var(--warn);background:var(--warn-bg)}.pill.crit{color:var(--crit);background:var(--crit-bg)}
.fixes{display:flex;flex-direction:column;gap:14px;counter-reset:fx}
.fix{counter-increment:fx;background:var(--surface);border:1px solid var(--border);border-radius:12px;padding:18px 20px;display:grid;grid-template-columns:auto 1fr;gap:16px}
.fix::before{content:counter(fx);font-family:var(--mono);font-weight:650;font-size:15px;color:var(--accent);width:30px;height:30px;display:grid;place-items:center;border:1px solid var(--border-strong);border-radius:8px;align-self:start}
.fix .ft{font-weight:600;margin:0 0 4px}
.fix .fw{color:var(--muted);font-size:13.5px;margin:0 0 12px}
.fix .how-l{font-family:var(--mono);font-size:10.5px;letter-spacing:.1em;text-transform:uppercase;color:var(--accent-ink);margin:0 0 6px}
.fix pre{font-family:var(--mono);font-size:12.5px;background:var(--surface-2);border:1px solid var(--border);border-radius:7px;padding:10px 12px;margin:0 0 12px;overflow-x:auto;color:var(--ink);white-space:pre-wrap}
.fix a.doc{font-size:13px;color:var(--accent-ink);text-decoration:none;font-weight:600;border-bottom:1px solid var(--accent)}
.fix a.doc:hover{opacity:.8}
.guide{margin:20px 0 0;padding:16px 20px;border:1px dashed var(--border-strong);border-radius:12px;font-size:13.5px;color:var(--muted)}
.guide b{color:var(--ink)}
.worst{list-style:none;margin:0;padding:0;font-family:var(--mono);font-size:13px}
.worst li{display:flex;justify-content:space-between;gap:14px;padding:8px 16px;border-bottom:1px solid var(--border);font-variant-numeric:tabular-nums}
.worst li:last-child{border-bottom:none}
.worst .fm{color:var(--muted);white-space:nowrap}
.worst .why{flex:1;color:var(--muted);font-size:13px;overflow-wrap:anywhere}
footer{margin:28px 2px 0;font-family:var(--mono);font-size:11px;letter-spacing:.03em;color:var(--muted);display:flex;flex-wrap:wrap;gap:6px 18px}
footer b{color:var(--ink);font-weight:600}
a:focus-visible,.doc:focus-visible{outline:2px solid var(--accent);outline-offset:2px}
</style>)CSS");

FString UBoundHoundService::Report(const FString& Title, const FString& Source, const FString& File)
{
	auto Deserialize = [](const FString& Json) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Obj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		FJsonSerializer::Deserialize(Reader, Obj);
		return Obj;
	};
	auto Num = [](const TSharedPtr<FJsonObject>& O, const TCHAR* K, double D = 0.0) -> double
	{ double V = D; if (O.IsValid()) { O->TryGetNumberField(K, V); } return V; };
	auto Str = [](const TSharedPtr<FJsonObject>& O, const TCHAR* K) -> FString
	{ FString V; if (O.IsValid()) { O->TryGetStringField(K, V); } return V; };
	auto ObjField = [](const TSharedPtr<FJsonObject>& O, const TCHAR* K) -> TSharedPtr<FJsonObject>
	{ const TSharedPtr<FJsonObject>* P = nullptr; if (O.IsValid()) { O->TryGetObjectField(K, P); } return P ? *P : TSharedPtr<FJsonObject>(); };

	// --- Gather: live verdict + budget, and the capture summary (best-effort) --------------------------
	const TSharedPtr<FJsonObject> FT = Deserialize(FrameTiming(60.0f));
	if (!FT.IsValid())
	{
		return ErrJson(TEXT("REPORT_NO_TIMING"), TEXT("FrameTiming returned no parseable data to report on."));
	}
	const TSharedPtr<FJsonObject>* BudgetPtr = nullptr;
	FT->TryGetObjectField(TEXT("budget"), BudgetPtr);
	const TSharedPtr<FJsonObject> Budget = BudgetPtr ? *BudgetPtr : nullptr;

	const TSharedPtr<FJsonObject> AN = Deserialize(Analyse(Source, File));
	const TSharedPtr<FJsonObject>* TracePtr = nullptr;
	const TSharedPtr<FJsonObject>* LogsPtr  = nullptr;
	if (AN.IsValid()) { AN->TryGetObjectField(TEXT("trace"), TracePtr); AN->TryGetObjectField(TEXT("logs"), LogsPtr); }
	// Source "trace" returns the trace result itself, not nested under "trace".
	const TSharedPtr<FJsonObject> Trace = TracePtr ? *TracePtr : ((AN.IsValid() && AN->HasField(TEXT("trace_file"))) ? AN : nullptr);
	const TSharedPtr<FJsonObject> Logs  = LogsPtr  ? *LogsPtr  : nullptr;

	const double GameMs   = Num(FT, TEXT("game_thread_ms"));
	const double RenderMs = Num(FT, TEXT("render_thread_ms"));
	const double RhiMs    = Num(FT, TEXT("rhi_thread_ms"));
	const double GpuMs    = Num(FT, TEXT("gpu_ms"));
	const double FrameMs  = Num(FT, TEXT("frame_ms"));
	const double Fps      = Num(FT, TEXT("fps"));
	const FString LiveBound = Str(FT, TEXT("bound"));
	const FString LiveConf  = Str(FT, TEXT("bound_confidence"));
	const double BudgetMs = Num(Budget, TEXT("budget_ms"), 16.67);
	const double TargetFps= Num(Budget, TEXT("target_fps"), 60.0);
	const bool bMeets     = Budget.IsValid() && Budget->GetBoolField(TEXT("meets_target"));
	const bool bPIE       = FT->GetBoolField(TEXT("pie_running"));
	bool bRepeatRead = false, bWarming = false;
	FT->TryGetBoolField(TEXT("repeat_read"), bRepeatRead);
	FT->TryGetBoolField(TEXT("warming"), bWarming);
	const double PieSeconds = Num(FT, TEXT("pie_seconds"));

	// Issue #23: a single in-process frame is only worth comparing against a capture when it's a fresh,
	// settled PIE frame. Otherwise say why it isn't representative instead of printing it as a headline.
	FString InProcCaveat;
	if (bRepeatRead)  InProcCaveat = TEXT("repeat read — no new frame since the last sample");
	else if (!bPIE)   InProcCaveat = TEXT("editor viewport, not your game (PIE not running)");
	else if (bWarming) InProcCaveat = FString::Printf(TEXT("PIE warming up (%.0f s in) — not representative"), PieSeconds);
	const bool bInProcRepresentative = InProcCaveat.IsEmpty();

	const bool bHasTrace = Trace.IsValid() && Num(Trace, TEXT("frame_count")) > 0.0;
	const double MaxFrameMs = Num(Trace, TEXT("max_frame_ms"));
	const double P95Ms      = Num(Trace, TEXT("p95_frame_ms"));
	const int32  FrameCount = (int32)Num(Trace, TEXT("frame_count"));
	const double AvgFps     = Num(Trace, TEXT("avg_fps"));
	const double DurationS  = Num(Trace, TEXT("duration_seconds"));
	const bool bHasLogs  = Logs.IsValid();
	const int32 PsoHitches = (int32)Num(Logs, TEXT("pso_hitches"));
	const int32 LogErrors  = (int32)Num(Logs, TEXT("errors"));
	const int32 LogWarnings= (int32)Num(Logs, TEXT("warnings"));

	// Issue #26: the resolution the captured run used (from its own command line), so readers can tell
	// whether two reports are comparable.
	const TSharedPtr<FJsonObject> RunDisplay = ObjField(Logs, TEXT("run_display"));
	FString RunResolution = Str(RunDisplay, TEXT("resolution"));
	if (!Str(RunDisplay, TEXT("window_mode")).IsEmpty())
	{
		RunResolution += (RunResolution.IsEmpty() ? TEXT("") : TEXT(" ")) + Str(RunDisplay, TEXT("window_mode"));
	}

	// Issue #25: spikes on a fixed interval, and the causes found in run-phase worst frames.
	const TSharedPtr<FJsonObject> Spikes = ObjField(Trace, TEXT("spikes"));
	const bool bPeriodic = Spikes.IsValid() && Spikes->GetBoolField(TEXT("periodic"));
	const int32 PeriodicCount = (int32)Num(Spikes, TEXT("matching_spikes"));
	const double PeriodicInterval = Num(Spikes, TEXT("interval_s"));
	const FString PeriodicLikely = Str(Spikes, TEXT("likely"));
	TMap<FString, TPair<FString, int32>> RunCauses; // cause -> (lever, frames)
	// Issue #31: spike frames (one per spike event) count too, so a mid-run GC hitch is explained even when
	// the worst-frames list is all load frames. A frame in both lists counts once.
	TArray<TSharedPtr<FJsonValue>> ExplainedFrames;
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Trace.IsValid() && Trace->TryGetArrayField(TEXT("worst_frames"), Arr) && Arr) ExplainedFrames.Append(*Arr);
		if (Spikes.IsValid() && Spikes->TryGetArrayField(TEXT("spike_frames"), Arr) && Arr) ExplainedFrames.Append(*Arr);
	}
	{
		TSet<int32> Seen;
		{
			for (const TSharedPtr<FJsonValue>& V : ExplainedFrames)
			{
				const TSharedPtr<FJsonObject> WO = V->AsObject();
				if (!WO.IsValid() || Str(WO, TEXT("phase")) != TEXT("run")) continue; // load causes fold into the startup fix
				bool bAlready = false;
				Seen.Add((int32)Num(WO, TEXT("frame")), &bAlready);
				if (bAlready) continue;
				const TArray<TSharedPtr<FJsonValue>>* CauseArr = nullptr;
				if (!WO->TryGetArrayField(TEXT("causes"), CauseArr) || !CauseArr) continue;
				for (const TSharedPtr<FJsonValue>& C : *CauseArr)
				{
					const TSharedPtr<FJsonObject> CO = C->AsObject();
					TPair<FString, int32>& Entry = RunCauses.FindOrAdd(Str(CO, TEXT("cause")), TPair<FString, int32>(Str(CO, TEXT("lever")), 0));
					++Entry.Value;
				}
			}
		}
	}

	// Issue #31: a frame-rate-capped run. Its frame stats (avg fps, p95) measure the cap, not the game.
	const TSharedPtr<FJsonObject> FrameCap = ObjField(Trace, TEXT("frame_cap"));
	const bool bCapped = FrameCap.IsValid() && FrameCap->GetBoolField(TEXT("capped"));
	const int32 CapFps = (int32)Num(FrameCap, TEXT("cap_fps"));
	const double CapHeadroomMs = Num(FrameCap, TEXT("headroom_ms"));
	const double CapBottleneckMs = Num(FrameCap, TEXT("bottleneck_median_ms"));
	const FString CapBottleneck = Str(FrameCap, TEXT("bottleneck"));
	const FString CapSentence = bCapped
		? FString::Printf(TEXT("Capped at %d fps: frame stats show the cap. Headroom = %.1f ms (the %.1f ms budget minus the %s median, %.1f ms)."),
			CapFps, CapHeadroomMs, Num(FrameCap, TEXT("cap_budget_ms")), *CapBottleneck, CapBottleneckMs)
		: FString();

	// Issue #22: a capture with per-thread frame times carries its own verdict (medians over the whole run).
	// It describes the representative run, so it wins over the single in-process frame.
	const TSharedPtr<FJsonObject> Split = ObjField(Trace, TEXT("thread_split"));
	const FString TraceBound = Str(Trace, TEXT("bound"));
	const bool bTraceVerdict = bHasTrace && !TraceBound.IsEmpty();
	const FString Bound = bTraceVerdict ? TraceBound : LiveBound;
	const FString Conf  = bTraceVerdict ? Str(Trace, TEXT("bound_confidence")) : LiveConf;
	const TCHAR* BoundSource = bTraceVerdict ? TEXT("trace") : (bInProcRepresentative ? TEXT("in_process") : TEXT("none"));

	// --- Build the "fix in this order" list from the data ---------------------------------------------
	TArray<FBHFix> Fixes;
	if (bHasTrace && MaxFrameMs > 250.0)
	{
		Fixes.Add({
			FString::Printf(TEXT("Startup / load hitch — %s ms peak frame"), *FString::FormatAsNumber((int64)MaxFrameMs)),
			TEXT("A representative (cold-cache) run pays load, PSO and shader-compilation costs the editor's warm caches hide. This is almost always the single biggest stall a player feels."),
			TEXT("# Ship a bundled PSO cache + keep precaching on (default):\nr.PSOPrecache.GlobalShaders 1\n# Then async-stream the opening level so the first frame isn't a hard stall."),
			BoundHoundDocs::PsoPrecache, TEXT("PSO Precaching — UE 5.8 docs") });
	}
	// The bound fix comes from the capture's verdict when there is one (issue #22), else from a fresh, settled
	// PIE frame. A warming-up or repeated frame is noise (issue #23: a PIE start-up frame got the run labelled
	// GPU-bound), and an editor-viewport frame measures the editor, not the game. Instead, say how to get one.
	const bool bVerdictUsable = bTraceVerdict || bInProcRepresentative;
	if (!bVerdictUsable)
	{
		Fixes.Add({ TEXT("Get a usable CPU-vs-GPU verdict first"),
			bHasTrace
				? FString::Printf(TEXT("This capture has no per-thread frame times, and the in-process frame isn't representative (%s), so this report doesn't say which thread bounds your game."), *InProcCaveat)
				: FString::Printf(TEXT("The in-process frame isn't representative (%s), so this report doesn't say which thread bounds your game."), *InProcCaveat),
			TEXT("# Best: capture a standalone run (records per-thread times):\nstart_standalone(\"name\", \"\")\n# ...play ~2 min past the load...\nstop_standalone()\nreport(...)\n# Quick: in separate calls, start_pie(), wait ~10 s, then report(...)"),
			BoundHoundDocs::Profiling, TEXT("Performance Profiling — UE 5.8 docs") });
	}
	else if (Bound == TEXT("GameThread"))
	{
		Fixes.Add({ TEXT("Game-thread bound — CPU logic cost"),
			TEXT("Tick / Blueprint / AI / animation dominate the frame. Dropping resolution or GPU cost will not help a game-thread-bound frame."),
			TEXT("stat dumpframe -ms=0.5 -root=gamethread\n# or capture with Unreal Insights, then cut the worst scopes:\n# throttle Tick, reduce ticking actors & AI, trim Blueprint tick logic."),
			BoundHoundDocs::StatCommands, TEXT("Stat Commands — UE 5.8 docs") });
	}
	else if (Bound == TEXT("RenderThread"))
	{
		Fixes.Add({ TEXT("Render-thread bound — scene setup cost"),
			TEXT("Too many draw calls / primitives, or many dynamic shadow-casting lights, saturate render-thread scene setup."),
			TEXT("stat scenerendering\n# Levers: merge/instance meshes, enable Nanite, cut dynamic lights & per-light shadows."),
			BoundHoundDocs::Profiling, TEXT("Performance Profiling — UE 5.8 docs") });
	}
	else if (Bound == TEXT("RHIThread"))
	{
		Fixes.Add({ TEXT("RHI-thread bound — GPU command submission"),
			TEXT("The thread submitting GPU commands is the bottleneck, distinct from render-thread scene setup — usually too many draw calls / state changes."),
			TEXT("stat rhi\nstat scenerendering\n# Levers: cut draw calls via merging / instancing / HISM, enable Nanite, reduce material & state permutations."),
			BoundHoundDocs::Profiling, TEXT("Performance Profiling — UE 5.8 docs") });
	}
	else if (Bound == TEXT("GPU"))
	{
		Fixes.Add({ TEXT("GPU bound — render cost"),
			TEXT("The GPU is the bottleneck. Confirm with the resolution test: r.ScreenPercentage 50 should noticeably raise FPS if truly GPU-bound."),
			TEXT("ProfileGPU\n# Levers: Virtual Shadow Maps, Lumen GI/reflections, translucency, post-process, ScreenPercentage."),
			BoundHoundDocs::Insights, TEXT("Unreal Insights — UE 5.8 docs") });
	}
	// Issue #25: what the worst gameplay frames were caught doing. A periodic pattern leads, because it is a
	// stall the player hits again and again.
	if (bPeriodic)
	{
		Fixes.Add({ FString::Printf(TEXT("Spikes every %.1f s%s"), PeriodicInterval, PeriodicLikely.StartsWith(TEXT("garbage")) ? TEXT(" — garbage collection") : TEXT("")),
			FString::Printf(TEXT("%d spikes landed on a fixed interval. Something scheduled is stalling the game thread, not the content on screen.%s"),
				PeriodicCount, PeriodicLikely.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" It %s."), *PeriodicLikely.Replace(TEXT("garbage collection: "), TEXT("")))),
			TEXT("# Confirm in the trace: open the spike frames in Unreal Insights and look for CollectGarbage on the GameThread.\n# Then reduce UObject churn, or spread / move the cost:\ngc.IncrementalBeginDestroyEnabled 1\ngc.TimeBetweenPurgingPendingKillObjects 61.1"),
			BoundHoundDocs::Insights, TEXT("Unreal Insights — UE 5.8 docs") });
	}
	for (const TPair<FString, TPair<FString, int32>>& C : RunCauses)
	{
		if (bPeriodic && C.Key == TEXT("garbage_collection")) continue; // already covered by the periodic fix
		if (C.Key == TEXT("pso_compile") && PsoHitches > 0) continue;  // the PSO fix below covers it
		if (C.Key == TEXT("hitch_logged")) continue;                    // confirms a stall, explains nothing
		if (C.Key == TEXT("game_thread_waiting")) continue;             // points at the other causes, isn't one
		FString Label = C.Key.Replace(TEXT("_"), TEXT(" "));
		if (!Label.IsEmpty()) Label[0] = FChar::ToUpper(Label[0]);
		Fixes.Add({ FString::Printf(TEXT("%s in %d of the worst gameplay frames"), *Label, C.Value.Value),
			TEXT("Found at the moment these frames spiked: the trace's log lines and biggest game-thread scopes, and the CSV capture's events."),
			C.Value.Key,
			BoundHoundDocs::Insights, TEXT("Unreal Insights — UE 5.8 docs") });
	}
	if (bHasLogs && PsoHitches > 0)
	{
		Fixes.Add({ FString::Printf(TEXT("%d PSO hitch%s — pipeline-state cache misses"), PsoHitches, PsoHitches == 1 ? TEXT("") : TEXT("es")),
			TEXT("A PSO stalls the first time a shader/state combo is drawn. Gathering a bundled cache from a representative playthrough removes them."),
			TEXT("# Enable PSO logging in a build, play a representative pass, then bundle the cache:\nr.ShaderPipelineCache.Enabled 1\nr.PSOPrecache.Components 1"),
			BoundHoundDocs::PsoCaches, TEXT("Optimizing Rendering with PSO Caches — UE 5.8 docs") });
	}

	// --- Compose the HTML -----------------------------------------------------------------------------
	auto ConfClass = [](const FString& C) -> const TCHAR*
	{ return C == TEXT("clear") ? TEXT("good") : (C == TEXT("none") ? TEXT("crit") : TEXT("warn")); };

	FString H;
	H.Reserve(16 * 1024);
	H += FString::Printf(TEXT("<title>%s</title>"), *BHHtmlEscape(Title));
	H += BHReportCss;
	H += TEXT("<div class=\"wrap\"><div class=\"sheet\">");
	H += FString::Printf(TEXT("<p class=\"eyebrow\">BoundHound &middot; %s &middot; %s</p>"),
		*BHHtmlEscape(FApp::GetProjectName()), bPIE ? TEXT("PIE frame") : TEXT("Editor frame"));
	H += FString::Printf(TEXT("<h1>%s</h1>"), *BHHtmlEscape(Title));
	H += FString::Printf(TEXT("<p class=\"lede\">Frame verdict and, where a capture is available, the representative frame stats — with a data-driven fix order. Generated %s.</p>"),
		*BHHtmlEscape(FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M"))));

	// Headline tiles: with a representative capture, frame them as the in-process-vs-representative contrast
	// (warm reading vs the cold worst frame + p95); without one, fall back to the single-run KPIs.
	H += TEXT("<div class=\"tiles\">");
	if (bHasTrace)
	{
		if (bInProcRepresentative)
		{
			H += FString::Printf(TEXT("<div class=\"tile warn\"><p class=\"k\">In-process (warm)</p><p class=\"v\">%.1f<small>ms</small></p><p class=\"s\">%.0f fps &middot; %s &middot; budget %s</p></div>"),
				FrameMs, Fps, *BHHtmlEscape(LiveBound.IsEmpty() ? TEXT("—") : LiveBound), bMeets ? TEXT("PASS") : TEXT("FAIL"));
		}
		else
		{
			H += FString::Printf(TEXT("<div class=\"tile\"><p class=\"k\">In-process (warm)</p><p class=\"v\" style=\"font-size:22px\">n/a</p><p class=\"s\">%s. Sampled %.1f ms — not used for comparison.</p></div>"),
				*BHHtmlEscape(InProcCaveat), FrameMs);
		}
		H += FString::Printf(TEXT("<div class=\"tile crit\"><p class=\"k\">Worst frame (cold)</p><p class=\"v\">%s<small>ms</small></p><p class=\"s\">The frame-0 stall the warm reading hid.</p></div>"),
			*FString::FormatAsNumber((int64)MaxFrameMs));
		if (bCapped)
		{
			// The p95 is the cap; the bottleneck's median is the game's real per-frame work.
			H += FString::Printf(TEXT("<div class=\"tile %s\"><p class=\"k\">Real work (capped at %d fps)</p><p class=\"v\">%.1f<small>ms</small></p><p class=\"s\">%s Frame p95 %.0f ms.</p></div>"),
				CapHeadroomMs < 0.0 ? TEXT("crit") : TEXT("good"), CapFps, CapBottleneckMs, *BHHtmlEscape(CapSentence), P95Ms);
		}
		else
		{
			const FString RatioText = bInProcRepresentative ? BoundHoundVerdict::DescribeRatio(P95Ms, FrameMs) : FString();
			const FString SteadySub = RatioText.IsEmpty()
				? FString::Printf(TEXT("avg %.1f fps over the capture."), AvgFps)
				: FString::Printf(TEXT("%s the %.1f ms in-process frame."), *RatioText, FrameMs);
			H += FString::Printf(TEXT("<div class=\"tile %s\"><p class=\"k\">Steady-state (p95)</p><p class=\"v\">%.0f<small>ms</small></p><p class=\"s\">%s</p></div>"),
				P95Ms > BudgetMs ? TEXT("crit") : TEXT("good"), P95Ms, *BHHtmlEscape(SteadySub));
		}
	}
	else
	{
		H += FString::Printf(TEXT("<div class=\"tile %s\"><p class=\"k\">Frame</p><p class=\"v\">%.1f<small>ms</small></p><p class=\"s\">%.0f fps live%s%s</p></div>"),
			!bVerdictUsable ? TEXT("") : (FrameMs > BudgetMs ? TEXT("crit") : TEXT("good")), FrameMs, Fps,
			bVerdictUsable ? TEXT("") : TEXT(" &middot; "), bVerdictUsable ? TEXT("") : *BHHtmlEscape(InProcCaveat));
		H += FString::Printf(TEXT("<div class=\"tile %s\"><p class=\"k\">Bound</p><p class=\"v\" style=\"font-size:22px\">%s</p><p class=\"s\">%s</p></div>"),
			bVerdictUsable ? ConfClass(Conf) : TEXT(""),
			*BHHtmlEscape(!bVerdictUsable || Bound.IsEmpty() ? TEXT("n/a") : Bound),
			*BHHtmlEscape(bVerdictUsable ? Conf + TEXT(" confidence") : FString(TEXT("not your game's frame"))));
		H += FString::Printf(TEXT("<div class=\"tile %s\"><p class=\"k\">Budget @ %.0f fps</p><p class=\"v\" style=\"font-size:22px\">%s</p><p class=\"s\">%.2f ms/frame budget</p></div>"),
			bMeets ? TEXT("good") : TEXT("crit"), TargetFps, bMeets ? TEXT("PASS") : TEXT("FAIL"), BudgetMs);
	}
	H += TEXT("</div>");

	// The full in-process vs representative matrix -- only when a capture supplies the cold column. Numbers
	// are live; the descriptive cells are the fixed teaching layer (the nature of warm-vs-cold doesn't change
	// run to run). This is the "juice" -- artifact-1's comparison, auto-filled from this run's data.
	if (bHasTrace)
	{
		const TCHAR* SteadyRec =
			  Bound == TEXT("GameThread")   ? TEXT("Frame is <b>game-thread bound</b> &rarr; drill <code>stat dumpframe -root=gamethread</code> (Tick / BP / AI / anim)")
			: Bound == TEXT("RenderThread") ? TEXT("Frame is <b>render-thread bound</b> &rarr; <code>stat scenerendering</code>; merge/instance meshes, cut dynamic lights")
			: Bound == TEXT("RHIThread")    ? TEXT("Frame is <b>RHI-thread bound</b> &rarr; <code>stat rhi</code>; cut draw calls via instancing / HISM / Nanite")
			: Bound == TEXT("GPU")          ? TEXT("Frame is <b>GPU bound</b> &rarr; <code>ProfileGPU</code>; shadows, Lumen, translucency, ScreenPercentage")
			:                                 TEXT("Profile the bottleneck thread with Unreal Insights");

		H += TEXT("<h2>In-process vs Representative</h2><div class=\"card table-scroll\"><table style=\"min-width:720px\">");
		H += TEXT("<thead><tr><th>Dimension</th><th>In-process — warm</th><th>Representative — cold</th><th class=\"rec\">Where to improve</th></tr></thead><tbody>");
		H += TEXT("<tr><td class=\"dim\">Process &amp; caches<small>environment</small></td><td class=\"warm\">Editor/PIE process, <b>warm</b> shader/PSO caches, on-demand cooked data</td><td class=\"cold\">Separate process, <b>cold</b> caches — like a shipping build</td><td class=\"rec-cell\">Trust the representative numbers for anything absolute; in-process is for logic checks</td></tr>");
		H += FString::Printf(TEXT("<tr><td class=\"dim\">Startup hitch<small>the fixture</small></td><td class=\"warm\"><span class=\"tag hidden\">invisible</span></td><td class=\"cold\"><b>%s ms</b> @ frame 0 <span class=\"tag caught\">caught</span></td><td class=\"rec-cell\"><b>Biggest lever.</b> Load / PSO / shader-comp bound at boot — ship a PSO precache, warm the shader pipeline, async-stream the first level</td></tr>"),
			*FString::FormatAsNumber((int64)MaxFrameMs));
		H += FString::Printf(TEXT("<tr><td class=\"dim\">PSO hitches<small>pipeline stalls</small></td><td class=\"warm\">Not surfaced</td><td class=\"cold\"><b>%d</b> from <code>-logPSO</code>%s</td><td class=\"rec-cell\">Bundle a gathered PSO cache with the build — folds into the startup fix</td></tr>"),
			PsoHitches, PsoHitches > 0 ? TEXT(" <span class=\"tag warn\">hitch</span>") : TEXT(""));
		const FString WarmSteady = bInProcRepresentative
			? FString::Printf(TEXT("<b>%.1f ms</b> &middot; %s &middot; %.0f fps"), FrameMs, *BHHtmlEscape(LiveBound.IsEmpty() ? TEXT("—") : LiveBound), Fps)
			: FString::Printf(TEXT("<span class=\"tag warn\">not representative</span> %s"), *BHHtmlEscape(InProcCaveat));
		const FString CapCell = bCapped
			? FString::Printf(TEXT(" <span class=\"tag warn\">capped %d fps</span><br>%s"), CapFps, *BHHtmlEscape(CapSentence))
			: FString();
		H += FString::Printf(TEXT("<tr class=\"bound\"><td class=\"dim\">Steady-state frame<small>hot path</small></td><td class=\"warm\">%s</td><td class=\"cold\"><b>p95 %.0f ms</b> &middot; avg <b>%.1f fps</b> &middot; %d frames%s</td><td class=\"rec-cell\">%s</td></tr>"),
			*WarmSteady, P95Ms, AvgFps, FrameCount, *CapCell, bVerdictUsable ? SteadyRec : TEXT("No usable bound verdict yet (see the fix list)"));
		const double CapRenderMed = Num(ObjField(Split, TEXT("render_thread")), TEXT("median_ms"));
		H += FString::Printf(TEXT("<tr><td class=\"dim\">Render thread<small>attribution</small></td><td class=\"warm\">Reads <b>%.1f ms</b> in-process</td><td class=\"cold\">%s</td><td class=\"rec-cell\">Use a representative trace for render / RHI attribution — in-process render time is not shipping-accurate</td></tr>"),
			RenderMs, *(bTraceVerdict ? FString::Printf(TEXT("Median <b>%.1f ms</b> over the run"), CapRenderMed) : FString(TEXT("Not recorded in this capture"))));
		const FString LiveVerdictCell = bInProcRepresentative
			? FString::Printf(TEXT("<b>%s</b> &middot; %s"), *BHHtmlEscape(LiveBound), *BHHtmlEscape(LiveConf))
			: FString::Printf(TEXT("<span class=\"tag warn\">n/a</span> %s"), *BHHtmlEscape(InProcCaveat));
		const FString TraceVerdictCell = bTraceVerdict
			? FString::Printf(TEXT("<b>%s</b> &middot; %s &middot; median of %d frames <span class=\"tag caught\">used</span>"), *BHHtmlEscape(TraceBound), *BHHtmlEscape(Conf), (int32)Num(Split, TEXT("frames")))
			: FString(TEXT("Not available — no per-thread times in this capture"));
		H += FString::Printf(TEXT("<tr><td class=\"dim\">Bound verdict<small>CPU vs GPU</small></td><td class=\"warm\">%s</td><td class=\"cold\">%s</td><td class=\"rec-cell\">The capture's verdict covers the whole representative run, so it wins over a single in-process frame</td></tr>"),
			*LiveVerdictCell, *TraceVerdictCell);
		H += TEXT("<tr><td class=\"dim\">Best use<small>role</small></td><td class=\"warm\">Quick sanity + verdict/hitch <b>logic</b> checks</td><td class=\"cold\"><b>Real stall identification</b> &amp; regression capture</td><td class=\"rec-cell\">Keep both — they answer different questions</td></tr>");
		H += TEXT("</tbody></table></div>");
	}

	// Per-thread table from the capture (issue #22): median / p95 over the run, and how often each thread
	// was the slowest. Shown ahead of the single in-process frame because it's the representative one.
	if (bTraceVerdict && Split.IsValid())
	{
		const TSharedPtr<FJsonObject> Share = ObjField(Trace, TEXT("bound_share"));
		H += FString::Printf(TEXT("<h2>Thread breakdown &middot; captured run &middot; %d frames</h2><div class=\"card table-scroll\"><table><thead><tr><th>Thread</th><th>median ms</th><th>p95 ms</th><th>slowest in</th><th>median vs budget</th></tr></thead><tbody>"),
			(int32)Num(Split, TEXT("frames")));
		struct FCapRow { const TCHAR* Label; const TCHAR* Key; const TCHAR* BoundName; };
		const FCapRow CapRows[] = {
			{ TEXT("Game"),   TEXT("game_thread"),   TEXT("GameThread") },
			{ TEXT("Render"), TEXT("render_thread"), TEXT("RenderThread") },
			{ TEXT("RHI"),    TEXT("rhi_thread"),    TEXT("RHIThread") },
			{ TEXT("GPU"),    TEXT("gpu"),           TEXT("GPU") },
		};
		for (const FCapRow& Cr : CapRows)
		{
			const TSharedPtr<FJsonObject> T = ObjField(Split, Cr.Key);
			if (!T.IsValid()) continue; // thread not recorded (RHI-threading off / no GPU timing)
			const double Med = Num(T, TEXT("median_ms"));
			const bool bIsBound = TraceBound == Cr.BoundName;
			H += FString::Printf(TEXT("<tr%s><td class=\"th-name\">%s%s</td><td class=\"num\">%.2f</td><td class=\"num\">%.2f</td><td class=\"num\">%.0f%%</td><td>%s</td></tr>"),
				bIsBound ? TEXT(" class=\"bound\"") : TEXT(""), Cr.Label, bIsBound ? TEXT(" ◀") : TEXT(""),
				Med, Num(T, TEXT("p95_ms")), Num(Share, Cr.BoundName) * 100.0,
				Med > BudgetMs ? TEXT("<span class=\"pill crit\">over</span>") : TEXT("<span class=\"pill good\">ok</span>"));
		}
		H += TEXT("</tbody></table></div>");
	}

	// Per-thread table, in-process frame
	H += FString::Printf(TEXT("<h2>Thread breakdown &middot; in-process frame%s</h2><div class=\"card table-scroll\"><table><thead><tr><th>Thread</th><th>ms</th><th>vs budget</th></tr></thead><tbody>"),
		bInProcRepresentative ? TEXT("") : TEXT(" &middot; not representative"));
	struct FRow { const TCHAR* Name; double Ms; bool bShow; };
	const FRow Rows[] = {
		{ TEXT("Game"),   GameMs,   true },
		{ TEXT("Render"), RenderMs, true },
		{ TEXT("RHI"),    RhiMs,    RhiMs > 0.0 },
		{ TEXT("GPU"),    GpuMs,    GpuMs > 0.0 },
	};
	for (const FRow& Rw : Rows)
	{
		if (!Rw.bShow) continue;
		const bool bOver = Rw.Ms > BudgetMs;
		const bool bIsBound = bInProcRepresentative && LiveBound.StartsWith(Rw.Name);
		H += FString::Printf(TEXT("<tr%s><td class=\"th-name\">%s%s</td><td class=\"num\">%.2f</td><td>%s</td></tr>"),
			bIsBound ? TEXT(" class=\"bound\"") : TEXT(""),
			Rw.Name, bIsBound ? TEXT(" ◀") : TEXT(""),
			Rw.Ms,
			bOver ? TEXT("<span class=\"pill crit\">over</span>") : TEXT("<span class=\"pill good\">ok</span>"));
	}
	H += TEXT("</tbody></table></div>");

	// Capture summary (trace worst frames + log health), when present
	if (bHasTrace)
	{
		H += FString::Printf(TEXT("<h2>Representative capture &middot; %.0fs &middot; avg %.1f fps%s%s</h2>"), DurationS, AvgFps,
			bCapped ? *FString::Printf(TEXT(" (capped at %d)"), CapFps) : TEXT(""),
			RunResolution.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" &middot; %s"), *BHHtmlEscape(RunResolution)));
		const TArray<TSharedPtr<FJsonValue>>* Worst = nullptr;
		if (Trace->TryGetArrayField(TEXT("worst_frames"), Worst) && Worst)
		{
			// Each worst frame with what explains it (issue #25): its first recognised cause and biggest scope.
			// Then the spike frames (issue #31): mid-run hitches that the load frames push out of the worst list.
			TArray<TSharedPtr<FJsonObject>> ListFrames;
			TSet<int32> Listed;
			for (const TSharedPtr<FJsonValue>& V : *Worst)
			{
				const TSharedPtr<FJsonObject> WO = V->AsObject();
				if (!WO.IsValid() || ListFrames.Num() >= 6) continue;
				ListFrames.Add(WO); Listed.Add((int32)Num(WO, TEXT("frame")));
			}
			const TArray<TSharedPtr<FJsonValue>>* SpikeFrames = nullptr;
			int32 SpikeShown = 0;
			if (Spikes.IsValid() && Spikes->TryGetArrayField(TEXT("spike_frames"), SpikeFrames) && SpikeFrames)
			{
				for (const TSharedPtr<FJsonValue>& V : *SpikeFrames)
				{
					const TSharedPtr<FJsonObject> WO = V->AsObject();
					if (!WO.IsValid() || Listed.Contains((int32)Num(WO, TEXT("frame"))) || SpikeShown >= 4) continue;
					WO->SetBoolField(TEXT("_spike"), true);
					ListFrames.Add(WO); ++SpikeShown;
				}
			}
			H += TEXT("<div class=\"card\"><ul class=\"worst\">");
			for (const TSharedPtr<FJsonObject>& WO : ListFrames)
			{
				FString Why;
				const TArray<TSharedPtr<FJsonValue>>* CauseArr = nullptr;
				if (WO->TryGetArrayField(TEXT("causes"), CauseArr) && CauseArr && CauseArr->Num() > 0)
				{
					Why = Str((*CauseArr)[0]->AsObject(), TEXT("cause")).Replace(TEXT("_"), TEXT(" "));
				}
				const TArray<TSharedPtr<FJsonValue>>* ScopeArr = nullptr;
				if (WO->TryGetArrayField(TEXT("top_scopes"), ScopeArr) && ScopeArr && ScopeArr->Num() > 0)
				{
					const TSharedPtr<FJsonObject> S0 = (*ScopeArr)[0]->AsObject();
					Why += FString::Printf(TEXT("%s%s %.0f ms"), Why.IsEmpty() ? TEXT("") : TEXT(" · "), *Str(S0, TEXT("scope")), Num(S0, TEXT("self_ms")));
				}
				const bool bSpike = WO->HasField(TEXT("_spike"));
				H += FString::Printf(TEXT("<li><span class=\"fm\">frame %d%s</span><span class=\"why\">%s</span><span>%s ms</span></li>"),
					(int32)WO->GetNumberField(TEXT("frame")),
					Str(WO, TEXT("phase")) == TEXT("load") ? TEXT(" &middot; load")
						: bSpike ? *FString::Printf(TEXT(" &middot; spike @ %.0f s"), Num(WO, TEXT("timestamp"))) : TEXT(""),
					*BHHtmlEscape(Why),
					*FString::FormatAsNumber((int64)WO->GetNumberField(TEXT("ms"))));
			}
			H += TEXT("</ul></div>");
		}
		if (bPeriodic)
		{
			H += FString::Printf(TEXT("<p class=\"lede\" style=\"margin-top:14px\"><b style=\"color:var(--ink)\">Periodic spikes:</b> %d spikes about every %.1f s%s.</p>"),
				PeriodicCount, PeriodicInterval, PeriodicLikely.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" &mdash; %s"), *BHHtmlEscape(PeriodicLikely)));
		}
		if (bHasLogs)
		{
			H += FString::Printf(TEXT("<p class=\"lede\" style=\"margin-top:14px\">Log: %s errors, %s warnings, <b style=\"color:var(--ink)\">%d PSO hitch%s</b>.</p>"),
				*FString::FormatAsNumber(LogErrors), *FString::FormatAsNumber(LogWarnings), PsoHitches, PsoHitches == 1 ? TEXT("") : TEXT("es"));
		}
	}
	else
	{
		H += TEXT("<p class=\"lede\" style=\"margin-top:8px\">No capture analysed — run <b>StartStandalone</b> (or StartTrace → workload → StopTrace), then Report again for frame-level worst-case detail.</p>");
	}

	// Fix list
	if (Fixes.Num() > 0)
	{
		H += TEXT("<h2>Fix in this order</h2><div class=\"fixes\">");
		for (const FBHFix& Fx : Fixes)
		{
			H += TEXT("<div class=\"fix\"><div>");
			H += FString::Printf(TEXT("<p class=\"ft\">%s</p><p class=\"fw\">%s</p>"), *BHHtmlEscape(Fx.Title), *BHHtmlEscape(Fx.Why));
			H += FString::Printf(TEXT("<p class=\"how-l\">How to fix</p><pre>%s</pre>"), *BHHtmlEscape(Fx.How));
			H += FString::Printf(TEXT("<a class=\"doc\" href=\"%s\">%s &rarr;</a>"), *Fx.DocUrl, *BHHtmlEscape(Fx.DocLabel));
			H += TEXT("</div></div>");
		}
		H += TEXT("</div>");
		H += TEXT("<div class=\"guide\"><b>Want a walkthrough?</b> Ask your assistant to guide you through any fix above — it can run the commands, read the results back, and validate the change with ForceHitch.</div>");
	}

	H += FString::Printf(TEXT("<footer><span><b>Project:</b> %s</span>"), *BHHtmlEscape(FApp::GetProjectName()));
	if (bHasTrace) { H += FString::Printf(TEXT("<span><b>Capture:</b> %d frames / %.0fs</span>"), FrameCount, DurationS); }
	H += FString::Printf(TEXT("<span><b>Generated:</b> %s</span><span><b>Tool:</b> BoundHound FrameTiming + Analyse</span></footer>"),
		*BHHtmlEscape(FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S"))));
	H += TEXT("</div></div>");

	// --- Write the self-contained file ----------------------------------------------------------------
	const FString Dir = ProjectSavedDirAbs() / TEXT("BoundHound");
	IPlatformFile::GetPlatformPhysical().CreateDirectoryTree(*Dir);
	const FString Path = Dir / FString::Printf(TEXT("report_%s.html"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	if (!FFileHelper::SaveStringToFile(H, *Path, FFileHelper::EEncodingOptions::ForceUTF8))
	{
		return ErrJson(TEXT("REPORT_WRITE_FAILED"), FString::Printf(TEXT("Could not write report to %s"), *Path));
	}

	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("report_file"), Path);
	R->SetNumberField(TEXT("fix_count"), Fixes.Num());
	R->SetBoolField(TEXT("included_capture"), bHasTrace);
	R->SetStringField(TEXT("bound"), bVerdictUsable ? Bound : FString());
	R->SetStringField(TEXT("bound_source"), BoundSource);
	R->SetBoolField(TEXT("in_process_representative"), bInProcRepresentative);
	if (!bInProcRepresentative) R->SetStringField(TEXT("in_process_caveat"), InProcCaveat);
	R->SetStringField(TEXT("hint"), FString::Printf(
		TEXT("Self-contained HTML written. Open it in any browser or screenshot it to share: %s"), *Path));
	return OkJson(R);
}

FString UBoundHoundService::StartTrace(const FString& Name, const FString& Channels)
{
	const FString TraceName = Name.IsEmpty() ? TEXT("mcp_capture") : Name;
	const FString ChannelSet = Channels.IsEmpty() ? FString(DefaultTraceChannels()) : Channels;
	FString TracePath = BuildTraceFilePath(TraceName);
	GLastTraceFilePath = TracePath + TEXT(".utrace");
	GLastLogFilePath   = ProjectSavedDirAbs() / TEXT("Logs") / (FApp::GetProjectName() + FString(TEXT(".log")));

	if (FTraceAuxiliary::IsConnected())
	{
		return ErrJson(TEXT("ALREADY_TRACING"), TEXT("A trace is already active. Call StopTrace first."));
	}

	bool bOk = FTraceAuxiliary::Start(FTraceAuxiliary::EConnectionType::File, *TracePath, *ChannelSet);
	if (!bOk)
	{
		return ErrJson(TEXT("TRACE_FAILED"), FString::Printf(TEXT("Failed to start trace. Path: %s"), *TracePath));
	}

	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("status"),     TEXT("tracing"));
	R->SetStringField(TEXT("trace_file"), GLastTraceFilePath);
	R->SetStringField(TEXT("channels"),   ChannelSet);
	R->SetStringField(TEXT("hint"),       TEXT("Call StopTrace when done, then Analyse to read results."));
	return OkJson(R);
}

FString UBoundHoundService::StopTrace()
{
	if (!FTraceAuxiliary::IsConnected())
	{
		return ErrJson(TEXT("NOT_TRACING"), TEXT("No trace is active."));
	}
	// Issue #24: ask the engine where the trace is actually going (it may have adjusted the name, e.g.
	// -tracefiletimestamps) instead of guessing "newest .utrace in the trace store", which can pick up an
	// unrelated trace.
	const FString Dest = FTraceAuxiliary::GetTraceDestinationString();
	FTraceAuxiliary::Stop();
	if (!Dest.IsEmpty() && IFileManager::Get().FileExists(*Dest))
	{
		GLastTraceFilePath = FPaths::ConvertRelativePathToFull(Dest);
		FPaths::NormalizeFilename(GLastTraceFilePath);
	}

	int64 FileSizeBytes = GLastTraceFilePath.IsEmpty() ? 0
		: IFileManager::Get().FileSize(*GLastTraceFilePath);

	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("status"),       TEXT("stopped"));
	R->SetStringField(TEXT("trace_file"),   GLastTraceFilePath);
	R->SetNumberField(TEXT("file_size_mb"), FMath::RoundToFloat((float)FileSizeBytes / (1024.f * 1024.f) * 10.f) / 10.f);
	R->SetStringField(TEXT("hint"),         TEXT("Call Analyse to read the results."));
	return OkJson(R);
}

FString UBoundHoundService::GetTraceStatus()
{
	bool bConnected = FTraceAuxiliary::IsConnected();
	FString Dest    = FTraceAuxiliary::GetTraceDestinationString();
	TStringBuilder<512> ChannelSB;
	FTraceAuxiliary::GetActiveChannelsString(ChannelSB);

	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("tracing"),          bConnected);
	R->SetStringField(TEXT("destination"),    Dest);
	R->SetStringField(TEXT("active_channels"),ChannelSB.ToString());
	R->SetStringField(TEXT("last_trace_file"),GLastTraceFilePath);
	return OkJson(R);
}

FString UBoundHoundService::Bookmark(const FString& Name)
{
	if (Name.IsEmpty()) return ErrJson(TEXT("MISSING_NAME"), TEXT("'Name' parameter required."));
	TRACE_BOOKMARK(TEXT("%s"), *Name);
	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("bookmark"), Name);
	R->SetStringField(TEXT("status"), TEXT("ok"));
	return OkJson(R);
}

FString UBoundHoundService::RegionStart(const FString& Name)
{
	if (Name.IsEmpty()) return ErrJson(TEXT("MISSING_NAME"), TEXT("'Name' parameter required."));
	TRACE_BEGIN_REGION(*Name);
	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("region"), Name);
	R->SetStringField(TEXT("status"), TEXT("started"));
	return OkJson(R);
}

FString UBoundHoundService::RegionEnd(const FString& Name)
{
	if (Name.IsEmpty()) return ErrJson(TEXT("MISSING_NAME"), TEXT("'Name' parameter required."));
	TRACE_END_REGION(*Name);
	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("region"), Name);
	R->SetStringField(TEXT("status"), TEXT("ended"));
	return OkJson(R);
}

FString UBoundHoundService::Analyse(const FString& Source, const FString& File)
{
	const FString Src = Source.IsEmpty() ? TEXT("both") : Source.ToLower();

	FString TraceFile = File.IsEmpty() ? GLastTraceFilePath : File;
	FString LogFile   = GLastLogFilePath;

	// A standalone run's files share one name: Saved/Profiling/<base>.utrace, Saved/Logs/<base>.log and
	// Saved/Profiling/CSV/<base>.csv. Pair them by name rather than "whatever ran last", so an older run (or
	// one from before an editor restart, which clears the remembered paths) reads its own log.
	if (File.EndsWith(TEXT(".log"), ESearchCase::IgnoreCase))
	{
		LogFile = File;
		const FString PairedTrace = FPaths::GetPath(FPaths::GetPath(File)) / TEXT("Profiling") / (FPaths::GetBaseFilename(File) + TEXT(".utrace"));
		TraceFile = FPaths::FileExists(PairedTrace) ? PairedTrace : FString();
	}
	else if (!TraceFile.IsEmpty())
	{
		const FString PairedLog = FPaths::GetPath(FPaths::GetPath(TraceFile)) / TEXT("Logs") / (FPaths::GetBaseFilename(TraceFile) + TEXT(".log"));
		if (FPaths::FileExists(PairedLog)) LogFile = PairedLog;
	}
	if (LogFile.IsEmpty())
	{
		LogFile = ProjectSavedDirAbs() / TEXT("Logs") / (FApp::GetProjectName() + FString(TEXT(".log")));
	}

	if (Src == TEXT("trace"))
	{
		if (TraceFile.IsEmpty()) return ErrJson(TEXT("NO_TRACE"), TEXT("No trace file known. Run StartTrace first, or pass File=<path>."));
		return AnalyseTrace(TraceFile);
	}
	if (Src == TEXT("logs"))
	{
		return AnalyseLogs(LogFile);
	}

	// both
	if (TraceFile.IsEmpty()) return AnalyseLogs(LogFile);
	return AnalyseBoth(TraceFile, LogFile);
}

FString UBoundHoundService::StartStandalone(const FString& Name, const FString& Channels, int32 ResX, int32 ResY, bool bFullscreen)
{
	if (!GEditor) return ErrJson(TEXT("NO_EDITOR"), TEXT("GEditor not available."));
	if (GStandaloneRunning) return ErrJson(TEXT("ALREADY_RUNNING"), TEXT("Standalone is already running. Call StopStandalone first."));
	const FString Busy = PlaySessionBusyReason();
	if (!Busy.IsEmpty()) return PlaySessionBusyError(Busy);
	const BoundHoundVerdict::FDisplayArgs Display = BoundHoundVerdict::BuildDisplayArgs(ResX, ResY, bFullscreen);
	if (!Display.Error.IsEmpty()) return ErrJson(TEXT("BAD_RESOLUTION"), Display.Error);

	// The name goes into file names and a console command, so keep it to one token (no spaces or commas).
	FString TraceName = Name.IsEmpty() ? TEXT("standalone_capture") : Name;
	for (TCHAR& C : TraceName) { if (FChar::IsWhitespace(C) || C == TEXT(',') || C == TEXT('"')) C = TEXT('_'); }
	const FString ChannelSet = Channels.IsEmpty() ? FString(DefaultTraceChannels()) : Channels;
	const FString RunStamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));

	// Issue #24: write the trace straight to a known, unique file. We used to pass -tracehost too, but the
	// engine prefers -tracehost over -tracefile, so the trace went to the Unreal Trace Server store under a
	// timestamp name and the path we reported here was never written. File-only = start, status and stop
	// all name the same file (same reasoning as the -abslog log fix below).
	const FString TracePath = BuildTraceFilePath(FString::Printf(TEXT("%s_%s"), *TraceName, *RunStamp)) + TEXT(".utrace");
	GLastTraceFilePath = TracePath;

	// Give the standalone process its own timestamped log via -abslog, rather than sharing the
	// project's default <Project>.log. That file is held open by the running editor, so LoadFileToString
	// fails on it (surfacing as a misleading LOG_NOT_FOUND) and a "newest log in the folder" guess picks
	// the editor's live log instead of the standalone's. A dedicated, unique path is unlocked and exact.
	FString LogDir = ProjectSavedDirAbs() / TEXT("Logs");
	IPlatformFile::GetPlatformPhysical().CreateDirectoryTree(*LogDir);
	FString StandaloneLogPath = LogDir / FString::Printf(TEXT("%s_%s.log"), *TraceName, *RunStamp);
	GLastLogFilePath = StandaloneLogPath;

	// Issue #22: a CSV-profiler capture alongside the trace records GameThreadTime / RenderThreadTime /
	// RHIThreadTime / GPUTime every frame -- the per-thread split a trace can't name (see
	// BoundHoundVerdict::ParseCsvThreadSamples). It's named after the trace so Analyse can find it:
	// Saved/Profiling/CSV/<name>_<stamp>.csv. Started by console command once the engine is up (the capture
	// covers the run after load), and it runs until the process ends.
	const FString CsvBase = FString::Printf(TEXT("%s_%s"), *TraceName, *RunStamp);
	GLastCsvFilePath = CsvPathForTrace(TracePath);
	FString ExtraArgs = FString::Printf(
		TEXT("-trace=%s -tracefile=\"%s\" -abslog=\"%s\" -ExecCmds=\"CsvProfile StartFile=%s, CsvProfile Start\""),
		*ChannelSet, *TracePath, *StandaloneLogPath, *CsvBase);
	ExtraArgs += Display.Args; // issue #26: fixed resolution / window mode, ahead of the editor's own

	FRequestPlaySessionParams P;
	P.SessionDestination = EPlaySessionDestinationType::NewProcess;
	P.WorldType = EPlaySessionWorldType::PlayInEditor;
	P.AdditionalStandaloneCommandLineParameters = ExtraArgs;
	GEditor->RequestPlaySession(P);
	GStandaloneRunning = true;

	GStandalonePlayDelegateHandle = FEditorDelegates::BeginStandaloneLocalPlay.AddLambda([](uint32 PID)
	{
		GStandaloneProcess = FPlatformProcess::OpenProcess(PID);
		FEditorDelegates::BeginStandaloneLocalPlay.Remove(GStandalonePlayDelegateHandle);
		GStandalonePlayDelegateHandle.Reset();
	});

	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("status"), TEXT("standalone start requested"));
	R->SetStringField(TEXT("trace_file"), GLastTraceFilePath);
	R->SetStringField(TEXT("log_file"),   GLastLogFilePath);
	R->SetStringField(TEXT("csv_file"),   GLastCsvFilePath);
	R->SetStringField(TEXT("channels"),   ChannelSet);
	if (Display.ResX > 0) R->SetStringField(TEXT("resolution"), FString::Printf(TEXT("%dx%d"), Display.ResX, Display.ResY));
	R->SetStringField(TEXT("window_mode"), Display.Mode.IsEmpty() ? FString(TEXT("editor play settings")) : Display.Mode);
	R->SetStringField(TEXT("hint"), TEXT("Call StopStandalone when done, then Analyse to read results."));
	return OkJson(R);
}

FString UBoundHoundService::StopStandalone()
{
	if (!GStandaloneRunning) return ErrJson(TEXT("NOT_RUNNING"), TEXT("No standalone session tracked. Did you call StartStandalone?"));
	if (GEditor) GEditor->RequestEndPlayMap();

	if (GStandalonePlayDelegateHandle.IsValid())
	{
		FEditorDelegates::BeginStandaloneLocalPlay.Remove(GStandalonePlayDelegateHandle);
		GStandalonePlayDelegateHandle.Reset();
	}

	if (GStandaloneProcess.IsValid())
	{
		double StartTime = FPlatformTime::Seconds();
		while (FPlatformProcess::IsProcRunning(GStandaloneProcess)
			   && (FPlatformTime::Seconds() - StartTime) < 5.0)
		{
			FPlatformProcess::Sleep(0.2f);
		}
		if (FPlatformProcess::IsProcRunning(GStandaloneProcess))
		{
			FPlatformProcess::TerminateProc(GStandaloneProcess);
		}
		FPlatformProcess::CloseProc(GStandaloneProcess);
	}

	GStandaloneRunning = false;

	// The standalone wrote its trace to the exact file StartStandalone named (-tracefile, issue #24).
	// No "newest trace in the store" guess -- that picks up unrelated traces.
	IFileManager& FM = IFileManager::Get();
	const bool bTraceExists = !GLastTraceFilePath.IsEmpty() && FM.FileExists(*GLastTraceFilePath);

	// GLastLogFilePath was set to a dedicated timestamped file in StartStandalone (-abslog), so it is
	// already the standalone's own log -- no need to guess the newest log in the folder (which would
	// pick the editor's live, locked log).

	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("status"),     TEXT("standalone stop requested"));
	R->SetStringField(TEXT("trace_file"), GLastTraceFilePath);
	R->SetBoolField(TEXT("trace_file_exists"), bTraceExists);
	if (bTraceExists)
	{
		R->SetNumberField(TEXT("file_size_mb"), FMath::RoundToDouble((double)FM.FileSize(*GLastTraceFilePath) / (1024.0 * 1024.0) * 10.0) / 10.0);
	}
	else
	{
		R->SetStringField(TEXT("trace_warning"), TEXT("No trace was written at trace_file. The standalone may have failed to launch, exited before tracing started, or been launched with a -tracehost override."));
	}
	R->SetStringField(TEXT("log_file"),   GLastLogFilePath);
	R->SetStringField(TEXT("csv_file"),   GLastCsvFilePath);
	R->SetBoolField(TEXT("csv_file_exists"), !GLastCsvFilePath.IsEmpty() && FM.FileExists(*GLastCsvFilePath));
	R->SetStringField(TEXT("hint"),       TEXT("Allow a few seconds for the trace file to finalise, then call Analyse."));
	return OkJson(R);
}

FString UBoundHoundService::GetStandaloneStatus()
{
	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("running"), GStandaloneRunning);
	R->SetStringField(TEXT("last_trace_file"), GLastTraceFilePath);
	R->SetStringField(TEXT("last_log_file"),   GLastLogFilePath);
	R->SetStringField(TEXT("last_csv_file"),   GLastCsvFilePath);
	return OkJson(R);
}

FString UBoundHoundService::StartPIE()
{
	if (!GEditor) return ErrJson(TEXT("NO_EDITOR"), TEXT("GEditor not available."));
	if (IsPIERunning() && !GEditor->ShouldEndPlayMap()) return ErrJson(TEXT("ALREADY_RUNNING"), TEXT("PIE is already running. Call StopPIE first."));
	const FString Busy = PlaySessionBusyReason();
	if (!Busy.IsEmpty()) return PlaySessionBusyError(Busy);

	FRequestPlaySessionParams P;
	P.SessionDestination = EPlaySessionDestinationType::InProcess;   // in-process, unlike StartStandalone's NewProcess
	P.WorldType          = EPlaySessionWorldType::PlayInEditor;
	GEditor->RequestPlaySession(P);

	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("status"), TEXT("PIE start requested"));
	R->SetStringField(TEXT("note"),   TEXT("PIE starts on the next editor tick -- call FrameTiming on a FOLLOWING frame (pie_running flips true). Because PIE is IN-PROCESS, ForceHitch game/render stalls now land and FrameTiming reads the live game world."));
	R->SetStringField(TEXT("caveat"), TEXT("PIE is NOT representative for real stall identification -- it reuses the editor's warm shader/PSO caches and on-demand cooked data, hiding costs a standalone/shipping build pays. For trustworthy stall numbers use StartStandalone."));
	R->SetStringField(TEXT("hint"),   TEXT("StartTrace before your workload if you want a capture, then StopPIE and Analyse."));
	return OkJson(R);
}

FString UBoundHoundService::StopPIE()
{
	if (!GEditor) return ErrJson(TEXT("NO_EDITOR"), TEXT("GEditor not available."));
	if (!IsPIERunning()) return ErrJson(TEXT("NOT_RUNNING"), TEXT("PIE is not running."));
	GEditor->RequestEndPlayMap();

	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("status"), TEXT("PIE end requested"));
	R->SetStringField(TEXT("hint"),   TEXT("PIE tears down on the next tick. If you were tracing, StopTrace then Analyse."));
	return OkJson(R);
}

// ---------------------------------------------------------------------------
// Baseline + diff (issue #6)
// ---------------------------------------------------------------------------

static FString BaselineDir()
{
	return ProjectSavedDirAbs() / TEXT("BoundHound") / TEXT("baselines");
}

static TSharedPtr<FJsonObject> BLParse(const FString& Json)
{
	TSharedPtr<FJsonObject> O;
	return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), O) ? O : nullptr;
}
static TSharedPtr<FJsonObject> BLObj(const TSharedPtr<FJsonObject>& O, const TCHAR* K)
{
	const TSharedPtr<FJsonObject>* P = nullptr;
	return (O.IsValid() && O->TryGetObjectField(K, P) && P) ? *P : nullptr;
}

// Build a snapshot of the current run. "capture" = a standalone run's analysis (trace + CSV + log);
// "live" = the current FrameTiming frame. Returns false with an error JSON in OutError.
static bool BuildSnapshot(const FString& Source, const FString& File, BoundHoundBaseline::FSnapshot& S, TArray<FString>& Notes, FString& OutError)
{
	const FString Kind = Source.IsEmpty() ? FString(TEXT("capture")) : Source.ToLower();
	S.Kind = Kind;
	S.Created = FDateTime::UtcNow().ToIso8601();

	if (Kind == TEXT("live"))
	{
		const TSharedPtr<FJsonObject> FT = BLParse(UBoundHoundService::FrameTiming(60.0f));
		if (!FT.IsValid() || !FT->GetBoolField(TEXT("success")))
		{
			OutError = ErrJson(TEXT("NO_READING"), TEXT("FrameTiming returned no reading."));
			return false;
		}
		auto Put = [&](const TCHAR* From, const TCHAR* To) { double V = 0.0; if (FT->TryGetNumberField(From, V) && V > 0.0) S.Metrics.Add(To, V); };
		Put(TEXT("frame_ms"), TEXT("frame_ms"));
		Put(TEXT("game_thread_ms"), TEXT("game_ms"));
		Put(TEXT("render_thread_ms"), TEXT("render_ms"));
		Put(TEXT("rhi_thread_ms"), TEXT("rhi_ms"));
		Put(TEXT("gpu_ms"), TEXT("gpu_ms"));
		FT->TryGetStringField(TEXT("bound"), S.Bound);
		bool bFlag = false;
		if (FT->TryGetBoolField(TEXT("repeat_read"), bFlag) && bFlag) Notes.Add(TEXT("This frame was a repeat read (no new frame since the last sample)."));
		if (FT->TryGetBoolField(TEXT("warming"), bFlag) && bFlag) Notes.Add(TEXT("PIE was still warming up: this frame isn't representative."));
		if (FT->TryGetBoolField(TEXT("pie_running"), bFlag) && !bFlag) Notes.Add(TEXT("PIE isn't running: this frame measures the editor viewport, not the game."));

		S.Identity.Add(TEXT("engineversion"), FEngineVersion::Current().ToString());
		S.Identity.Add(TEXT("platform"), FPlatformProperties::IniPlatformName());
		S.Identity.Add(TEXT("config"), LexToString(FApp::GetBuildConfiguration()));
		S.Identity.Add(TEXT("project"), FApp::GetProjectName());
		FString Rhi = FApp::GetGraphicsRHI(); // "D3D12 (SM6)"; run logs say "D3D12"
		Rhi.Split(TEXT(" ("), &Rhi, nullptr);
		S.Identity.Add(TEXT("rhiname"), Rhi.TrimStartAndEnd());
		UWorld* World = (GEditor && GEditor->PlayWorld) ? GEditor->PlayWorld.Get() : (GEditor ? GEditor->GetEditorWorldContext().World() : nullptr);
		if (World) S.Identity.Add(TEXT("map"), UWorld::RemovePIEPrefix(World->GetOutermost()->GetName()));
		S.Source = TEXT("FrameTiming");
		return true;
	}
	if (Kind != TEXT("capture"))
	{
		OutError = ErrJson(TEXT("BAD_SOURCE"), TEXT("Source must be 'capture' or 'live'."));
		return false;
	}

	const TSharedPtr<FJsonObject> AN = BLParse(UBoundHoundService::Analyse(TEXT("both"), File));
	const TSharedPtr<FJsonObject> Trace = BLObj(AN, TEXT("trace"));
	const TSharedPtr<FJsonObject> Logs = BLObj(AN, TEXT("logs"));
	if (!Trace.IsValid())
	{
		OutError = ErrJson(TEXT("NO_CAPTURE"), TEXT("No capture to read. Run start_standalone / stop_standalone first, or pass File=<trace or log path>."));
		return false;
	}
	if (!Trace->GetBoolField(TEXT("success")))
	{
		FString Msg; Trace->TryGetStringField(TEXT("error"), Msg);
		OutError = ErrJson(TEXT("ANALYSIS_FAILED"), FString::Printf(TEXT("The capture couldn't be analysed: %s"), *Msg));
		return false;
	}
	Trace->TryGetStringField(TEXT("trace_file"), S.Source);
	auto Put = [&](const TSharedPtr<FJsonObject>& O, const TCHAR* From, const FString& To)
	{
		double V = 0.0;
		if (O.IsValid() && O->TryGetNumberField(From, V)) S.Metrics.Add(To, V);
	};
	Put(Trace, TEXT("avg_frame_ms"), TEXT("frame_avg_ms"));
	Put(Trace, TEXT("p95_frame_ms"), TEXT("frame_p95_ms"));
	Put(Trace, TEXT("max_frame_ms"), TEXT("worst_frame_ms"));
	const TSharedPtr<FJsonObject> Split = BLObj(Trace, TEXT("thread_split"));
	struct FThreadKey { const TCHAR* Obj; const TCHAR* Prefix; };
	const FThreadKey Threads[] = {
		{ TEXT("game_thread"), TEXT("game") }, { TEXT("render_thread"), TEXT("render") },
		{ TEXT("rhi_thread"), TEXT("rhi") }, { TEXT("gpu"), TEXT("gpu") } };
	for (const FThreadKey& T : Threads)
	{
		const TSharedPtr<FJsonObject> O = BLObj(Split, T.Obj);
		Put(O, TEXT("median_ms"), FString::Printf(TEXT("%s_median_ms"), T.Prefix));
		Put(O, TEXT("p95_ms"), FString::Printf(TEXT("%s_p95_ms"), T.Prefix));
	}
	if (!Split.IsValid() || !Split->GetBoolField(TEXT("available")))
	{
		Notes.Add(TEXT("No per-thread times in this capture (no CSV next to the trace): only whole-frame stats are compared."));
	}
	Put(BLObj(Trace, TEXT("spikes")), TEXT("spike_events"), TEXT("spike_events"));
	Put(Logs, TEXT("pso_hitches"), TEXT("pso_hitches"));
	Put(Logs, TEXT("errors"), TEXT("log_errors"));
	Trace->TryGetStringField(TEXT("bound"), S.Bound);

	if (const TSharedPtr<FJsonObject> Id = BLObj(Logs, TEXT("run_identity")))
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& KV : Id->Values) S.Identity.Add(KV.Key, KV.Value->AsString());
	}
	else
	{
		Notes.Add(TEXT("The run's log has no CSV-profiler metadata, so engine / GPU / resolution can't be checked from the run itself."));
	}
	if (const TSharedPtr<FJsonObject> Cap = BLObj(Trace, TEXT("frame_cap")))
	{
		S.Identity.Add(TEXT("frame_cap"), Cap->GetBoolField(TEXT("capped")) ? FString::Printf(TEXT("%d"), (int32)Cap->GetNumberField(TEXT("cap_fps"))) : FString(TEXT("uncapped")));
	}
	return true;
}

FString UBoundHoundService::SaveBaseline(const FString& Name, const FString& Scenario, const FString& Source, const FString& File)
{
	BoundHoundBaseline::FSnapshot S;
	TArray<FString> Notes;
	FString Err;
	if (!BuildSnapshot(Source, File, S, Notes, Err)) return Err;
	S.Name = BoundHoundBaseline::SanitiseName(Name);
	S.Scenario = Scenario.TrimStartAndEnd();

	FString Json;
	FJsonSerializer::Serialize(BoundHoundBaseline::ToJson(S), TJsonWriterFactory<>::Create(&Json));
	const FString Path = BaselineDir() / (S.Name + TEXT(".json"));
	IPlatformFile::GetPlatformPhysical().CreateDirectoryTree(*BaselineDir());
	const bool bExisted = FPaths::FileExists(Path);
	if (!FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return ErrJson(TEXT("WRITE_FAILED"), FString::Printf(TEXT("Couldn't write %s"), *Path));
	}

	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("baseline_file"), Path);
	R->SetBoolField(TEXT("overwrote"), bExisted);
	R->SetObjectField(TEXT("baseline"), BoundHoundBaseline::ToJson(S));
	if (S.Scenario.IsEmpty()) Notes.Add(TEXT("No scenario label given. Name the workload (map, route, duration) so a later comparison can say whether the runs claim to do the same thing."));
	TArray<TSharedPtr<FJsonValue>> NoteArr;
	for (const FString& N : Notes) NoteArr.Add(MakeShared<FJsonValueString>(N));
	R->SetArrayField(TEXT("notes"), NoteArr);
	R->SetStringField(TEXT("hint"), FString::Printf(TEXT("After your change, capture the same scenario again, then compare_baseline(\"%s\")."), *S.Name));
	return OkJson(R);
}

FString UBoundHoundService::CompareBaseline(const FString& Name, const FString& Source, const FString& File, bool bForce)
{
	const FString SafeName = BoundHoundBaseline::SanitiseName(Name);
	const FString Path = BaselineDir() / (SafeName + TEXT(".json"));
	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *Path))
	{
		return ErrJson(TEXT("NO_BASELINE"), FString::Printf(TEXT("No baseline '%s' (%s). list_baselines() shows the saved ones."), *SafeName, *Path));
	}
	BoundHoundBaseline::FSnapshot Base;
	if (!BoundHoundBaseline::FromJson(BLParse(Json), Base))
	{
		return ErrJson(TEXT("BAD_BASELINE"), FString::Printf(TEXT("%s isn't a BoundHound baseline."), *Path));
	}

	BoundHoundBaseline::FSnapshot Cur;
	TArray<FString> Notes;
	FString Err;
	if (!BuildSnapshot(Source.IsEmpty() ? Base.Kind : Source, File, Cur, Notes, Err)) return Err;
	if (Base.Kind == TEXT("capture") && !Base.Source.IsEmpty() && Base.Source == Cur.Source)
	{
		Notes.Add(TEXT("The current run is the same capture the baseline was saved from: every delta is zero by construction."));
	}

	const BoundHoundBaseline::FComparison C = BoundHoundBaseline::Compare(Base, Cur, bForce);

	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("baseline"), SafeName);
	R->SetStringField(TEXT("baseline_created"), Base.Created);
	R->SetStringField(TEXT("baseline_source"), Base.Source);
	R->SetStringField(TEXT("current_source"), Cur.Source);
	R->SetStringField(TEXT("scenario"), Base.Scenario);
	R->SetStringField(TEXT("scenario_note"), Base.Scenario.IsEmpty()
		? FString(TEXT("The baseline has no scenario label. BoundHound can't tell whether the two runs did the same work."))
		: FString::Printf(TEXT("Scenario '%s' is the label given when the baseline was saved. BoundHound doesn't verify the current run did the same work."), *Base.Scenario));

	TArray<TSharedPtr<FJsonValue>> MisArr;
	for (const BoundHoundBaseline::FMismatch& M : C.Mismatches)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("field"), M.Field);
		O->SetStringField(TEXT("baseline"), M.Baseline);
		O->SetStringField(TEXT("current"), M.Current);
		O->SetStringField(TEXT("severity"), M.bHard ? TEXT("hard") : TEXT("soft"));
		MisArr.Add(MakeShared<FJsonValueObject>(O));
	}
	R->SetArrayField(TEXT("mismatches"), MisArr);

	if (C.bHardMismatch && !bForce)
	{
		R->SetBoolField(TEXT("success"), false);
		R->SetStringField(TEXT("error_code"), TEXT("BASELINE_MISMATCH"));
		R->SetStringField(TEXT("error"), TEXT("The runs differ in engine version, platform, build config, project or kind, so their numbers measure different things. Save a new baseline for this setup, or pass force=True to compare anyway."));
		FString Out;
		FJsonSerializer::Serialize(R.ToSharedRef(), TJsonWriterFactory<>::Create(&Out));
		return Out;
	}

	R->SetBoolField(TEXT("comparable"), C.Mismatches.IsEmpty());
	TArray<TSharedPtr<FJsonValue>> DeltaArr, Summary;
	for (const BoundHoundBaseline::FDelta& D : C.Deltas)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("metric"), D.Key);
		O->SetNumberField(TEXT("baseline"), FMath::RoundToDouble(D.Baseline * 100.0) / 100.0);
		O->SetNumberField(TEXT("current"), FMath::RoundToDouble(D.Current * 100.0) / 100.0);
		O->SetNumberField(TEXT("delta"), FMath::RoundToDouble(D.Delta * 100.0) / 100.0);
		if (D.Baseline != 0.0) O->SetNumberField(TEXT("delta_pct"), FMath::RoundToDouble(D.DeltaPct * 10.0) / 10.0);
		O->SetBoolField(TEXT("changed"), D.bChanged);
		DeltaArr.Add(MakeShared<FJsonValueObject>(O));
		if (D.bChanged) Summary.Add(MakeShared<FJsonValueString>(D.Statement));
	}
	if (C.bBoundChanged)
	{
		Summary.Insert(MakeShared<FJsonValueString>(FString::Printf(TEXT("bound verdict %s -> %s"), *Base.Bound, *Cur.Bound)), 0);
	}
	if (Summary.IsEmpty()) Summary.Add(MakeShared<FJsonValueString>(TEXT("No metric changed beyond run-to-run noise.")));
	R->SetArrayField(TEXT("deltas"), DeltaArr);
	R->SetArrayField(TEXT("summary"), Summary);
	R->SetStringField(TEXT("bound_baseline"), Base.Bound);
	R->SetStringField(TEXT("bound_current"), Cur.Bound);
	R->SetBoolField(TEXT("bound_changed"), C.bBoundChanged);
	TArray<TSharedPtr<FJsonValue>> WarnArr;
	for (const FString& W : C.Warnings) WarnArr.Add(MakeShared<FJsonValueString>(W));
	for (const FString& N : Notes) WarnArr.Add(MakeShared<FJsonValueString>(N));
	R->SetArrayField(TEXT("warnings"), WarnArr);
	R->SetStringField(TEXT("note"), TEXT("Factual deltas only: BoundHound doesn't say what caused a change. 'changed' = beyond noise (0.1 ms and 3% for a capture, 10% for a live frame; any change for counts)."));
	return OkJson(R);
}

FString UBoundHoundService::ListBaselines()
{
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(BaselineDir() / TEXT("*.json")), true, false);
	Files.Sort();
	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FString& F : Files)
	{
		FString Json;
		BoundHoundBaseline::FSnapshot S;
		if (!FFileHelper::LoadFileToString(Json, *(BaselineDir() / F)) || !BoundHoundBaseline::FromJson(BLParse(Json), S)) continue;
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("name"), FPaths::GetBaseFilename(F));
		O->SetStringField(TEXT("kind"), S.Kind);
		O->SetStringField(TEXT("scenario"), S.Scenario);
		O->SetStringField(TEXT("created"), S.Created);
		O->SetStringField(TEXT("bound"), S.Bound);
		if (const FString* Map = S.Identity.Find(TEXT("map"))) O->SetStringField(TEXT("map"), *Map);
		Arr.Add(MakeShared<FJsonValueObject>(O));
	}
	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("dir"), BaselineDir());
	R->SetArrayField(TEXT("baselines"), Arr);
	return OkJson(R);
}

# BoundHound — Testing

BoundHound ships a headless automation suite over its core logic (the CPU/GPU **bound verdict** and
the **budget gate**). **Run it after any non-trivial change and before promoting a slice** — the whole
point is that "is it still golden?" is one command away.

## Run it

```powershell
# From Plugins/BoundHound/
./RunTests.ps1
```

That boots `UnrealEditor-Cmd` against the host project, runs every test under the `BoundHound` path
**with no editor window and no GPU** (`-nullrhi`), parses the report, prints a PASS/FAIL summary, and
**exits non-zero if anything fails** (so it drops straight into CI). Expected:

```
Result: succeeded=14 failed=0 notRun=0
```

Overridable params: `-EnginePath`, `-Project`, `-Filter`. The raw incantation, if the script isn't handy:

```powershell
& "<UE>\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "<project>.uproject" `
  -ExecCmds="Automation RunTests BoundHound; Quit" `
  -TestExit="Automation Test Queue Empty" -unattended -nullrhi -nosplash -stdout
```

You can also run them from the editor GUI: **Tools → Session Frontend → Automation**, filter `BoundHound`.

> **Build first.** The tests compile into the plugin DLL only in Development/Debug editor builds
> (`WITH_AUTOMATION_TESTS`), so build the editor target before running. If the report says
> `notRun=0 succeeded=0`, the DLL you launched didn't contain the tests — rebuild.

> **Gotcha:** UE writes the report `index.json` as **UTF-16**. `RunTests.ps1` decodes it; if you parse
> it yourself, read it as UTF-16 or `ConvertFrom-Json` will choke.

## What's covered

The verdict/budget logic is factored into pure helpers (`Private/BoundHoundVerdict.h`) that `FrameTiming`
routes through, so the tests exercise the **same code the shipping method runs** — not a copy.

| Area | Cases |
|---|---|
| **Verdict** (`Classify`) | clear GameThread / RenderThread / RHIThread / GPU; RHI ranked only when available (wins / present-but-not-winning / contests render thread / absent-doesn't-downgrade); contested/`marginal` tie (incl. marginal-beats-moderate); the exact-10% boundary (not contested) vs just-inside; GPU-unavailable → `moderate` (GPU dropped from ranking); no-timing → `none` |
| **Budget** (`ComputeBudget` / `IsOverBudget`) | pass/fail gate; zero-frame is not a pass; alternate target FPS; invalid-FPS fallback to 60; per-thread over-budget incl. the exact-budget boundary |
| **Hitch match** (`HitchMatchesExpect`) | race-free self-validation logic (issue #17): game/render/both hit vs undershoot vs wrong-thread-dominates; the `rhi` arm (issue #2: rhi peak clears floor AND dominates both CPU threads → match, else miss); gpu not self-validated by CPU peaks |
| **Drill-down** (v1.1.0, issue #25) | `TopSelfTimes`: self time = scope minus direct children, window clipping, top-N; `ClassifyCauses`: FlushAsyncLoading → `sync_load`, CollectGarbage → `garbage_collection`, PSO lines incl. "Waited for PSO creation" → `pso_compile`, runtime Niagara compile, `game_thread_waiting` only when the wait is the top scope, each cause once; `DetectPeriodicSpikes`: GC every 61 s (merged double-frame spikes), the live Proteus pattern (PSO burst among GC spikes), 2 spikes / irregular / noisy chance chains are not periodic, a 30 s beat isn't labelled GC; log frame-tag parsing + startup-noise filter. |
| **Standalone display** (v1.1.0, issue #26) | `BuildDisplayArgs`: inherit (no args), fixed 1080p windowed, width-only → 16:9, fullscreen at native res, negative / tiny rejected; `ParseRunDisplay`: first `ResX`/`ResY` wins (ours ahead of the editor's), window mode. |
| **Trace verdict** (v1.1.0, issue #22) | `ClassifyTrace`: steady game-bound run → clear; a 30 s load frame doesn't swing the median verdict; alternating bottleneck (winner slowest in <50% of frames) → `marginal`; RHI ranked when recorded; no samples → `none`. `ParseCsvThreadSamples`: real CSV layout (EVENTS first, columns appended mid-run, repeated header + metadata rows, unterminated last row); no RHI column; missing CPU column → no samples. `Percentile` nearest-rank. |
| **Frame cap** (v1.2.0, issue #31) | `ParseCsvThreadSamples` reads `FrameTime` and the `EVENTS` column (`Name##seconds`, several per cell); `CsvEventsNear` finds the GC event one row before the frame-time spike; CSV events feed `ClassifyCauses` (GC → `garbage_collection`, FlushAsyncLoading → `sync_load`); `DetectFrameCap`: the field report (60-capped, GPU median 9.75 ms) → capped at 60 with ~6.9 ms headroom, uncapped / bottleneck fills the frame / wide spread / too few frames → not capped, a 30 fps cap; `SpikeThresholdMs`: a 33 ms frame counts in a 60-capped run, uncapped keeps 3× median with the 33.3 ms floor; `AlignCsvToTrace`: offset found from the load frames, unrelated / too-short sequences rejected. |
| **Baseline + diff** (v1.2.0, issue #6) | `Compare`: same run → no change; a +1.27 ms GPU median → changed, exact statement; +0.05 ms → noise; PSO count change; verdict change; engine mismatch → refused (no deltas) unless forced (with warning); capture vs live → hard; resolution / frame cap / scenario → soft (still compared, warned); a field missing on one side isn't a mismatch; live 5% = noise, capture 5% = change. `ParseMetadataLine` / `IdentityFromMetadata` on the field report's log lines (last resolution wins, map + project from the command line, path with spaces); `SanitiseName`; JSON round trip. |
| **Freshness & play sessions** (v1.0.1) | repeat-read detection logic (issue #27) + live: two `FrameTiming` calls in one tick → second has `repeat_read:true`, `bound_confidence:"none"`, `REPEAT READ` hint; PIE warm-up window (issue #23); report ratio text reads "5.4× faster than", never "~0×" (issue #23); play-session busy guard: idle is safe, ending/queued/running/in-progress all refuse (issue #28) |
| **Live smoke** | `FrameTiming` JSON contract (required fields, `bound` is a known thread); `ForceHitch` input validation (`BAD_THREAD`); `ForceHitch` self-measure (`game` induces & measures its own stall → `observed_peak_game_ms` + `verdict_matched_expect`); `ForceHitch` `rhi` arm (asserts the nullrhi fallback: `rhi_threading:false` + no false match when there's no separate RHI thread); `Report` writes a real self-contained file headless (`included_capture:false` no-capture path, non-empty file on disk, then cleans up) |

Not covered by unit tests (needs a live world / eyeball or a trace): the `ForceHitch` **gpu** mapping
(scene-dependent, reads back via a following-frame `FrameTiming`); the `ForceHitch` **rhi** live
RHIThread-*wins* path (headless `-nullrhi` has no separate RHI thread, so only the pure match logic and
the RHI-off fallback are covered — the live win needs a session with `r.RHIThread.Enable 1`); the
**Report HTML layout** itself (only its JSON contract — `report_file`/`included_capture`/`fix_count` —
is asserted; the rendered page is eyeballed); and bookmark/region trace markers — see the `force_hitch`
notes in [`USAGE.md`](USAGE.md). The CPU render/both stalls are now self-measured, so their pass/fail
logic *is* covered headless.

## Adding a test

Tests live in `Private/BoundHoundServiceTests.cpp`, guarded by `#if WITH_AUTOMATION_TESTS`. Add pure
logic to `BoundHoundVerdict.h` (so it's testable headless) and route `FrameTiming` through it, then:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMyThing, "BoundHound.Area.MyThing", kBHTestFlags)
bool FMyThing::RunTest(const FString&)
{
    TestEqual(TEXT("desc"), Actual, Expected);
    return true;
}
```

Rebuild the editor target, then `./RunTests.ps1`.

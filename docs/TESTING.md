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
Result: succeeded=12 failed=0 notRun=0
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
| **Hitch match** (`HitchMatchesExpect`) | race-free self-validation logic (issue #17): game/render/both hit vs undershoot vs wrong-thread-dominates; gpu not self-validated by CPU peaks |
| **Live smoke** | `FrameTiming` JSON contract (required fields, `bound` is a known thread); `ForceHitch` input validation (`BAD_THREAD`); `ForceHitch` self-measure (`game` induces & measures its own stall → `observed_peak_game_ms` + `verdict_matched_expect`) |

Not covered by unit tests (needs a live world / eyeball or a trace): the `ForceHitch` **gpu** mapping
(scene-dependent, reads back via a following-frame `FrameTiming`) and bookmark/region trace markers —
see the `force_hitch` notes in [`USAGE.md`](USAGE.md). The CPU render/both stalls are now self-measured,
so their pass/fail logic *is* covered headless.

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

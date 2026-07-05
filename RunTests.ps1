<#
.SYNOPSIS
    Run BoundHound's automation tests headless (no editor window, no GPU) and report pass/fail.

.DESCRIPTION
    Boots UnrealEditor-Cmd against the host project, runs every test under the "BoundHound" path
    via the automation controller, exports a report, and prints a PASS/FAIL summary. Exits non-zero
    if any test fails or none run -- so it drops straight into CI.

    The tests are compiled into the plugin DLL only in Development/Debug editor builds
    (WITH_AUTOMATION_TESTS), so build the editor target first (see the plugin README / project memory).

.PARAMETER EnginePath
    UE install root. Defaults to G:\Epic Games\UE_5.8.

.PARAMETER Project
    Path to the .uproject to run against. Defaults to the single .uproject two levels above this
    script (i.e. the project that embeds Plugins/BoundHound/).

.PARAMETER Filter
    Automation test path prefix to run. Defaults to "BoundHound".

.EXAMPLE
    ./RunTests.ps1
.EXAMPLE
    ./RunTests.ps1 -EnginePath "D:\UE_5.8" -Project "D:\MyGame\MyGame.uproject"
#>
param(
    [string]$EnginePath = "G:\Epic Games\UE_5.8",
    [string]$Project,
    [string]$Filter = "BoundHound"
)

$ErrorActionPreference = "Stop"

# Resolve the project: default to the lone .uproject two dirs up (Plugins/BoundHound/ -> project root).
if (-not $Project) {
    $projectRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
    $found = Get-ChildItem -Path $projectRoot -Filter *.uproject -File -ErrorAction SilentlyContinue
    if (-not $found) { throw "No .uproject found under '$projectRoot'. Pass -Project explicitly." }
    if ($found.Count -gt 1) { throw "Multiple .uproject files under '$projectRoot'. Pass -Project explicitly." }
    $Project = $found.FullName
}

$editorCmd = Join-Path $EnginePath "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
if (-not (Test-Path $editorCmd)) { throw "UnrealEditor-Cmd.exe not found at '$editorCmd'. Pass -EnginePath." }
if (-not (Test-Path $Project))   { throw "Project not found at '$Project'." }

$reportDir = Join-Path $env:TEMP ("BoundHoundTests_" + (Get-Date -Format "yyyyMMdd_HHmmss"))
New-Item -ItemType Directory -Path $reportDir -Force | Out-Null

Write-Host "Running '$Filter' tests against $Project (headless, nullrhi)..." -ForegroundColor Cyan

& $editorCmd $Project `
    -ExecCmds="Automation RunTests $Filter; Quit" `
    -TestExit="Automation Test Queue Empty" `
    -ReportExportPath="$reportDir" `
    -unattended -nopause -nosplash -nullrhi -stdout | Out-Null

# UE writes index.json as UTF-16 -- decode accordingly or ConvertFrom-Json chokes.
$reportFile = Join-Path $reportDir "index.json"
if (-not (Test-Path $reportFile)) { Write-Error "No report written at $reportFile -- did the editor build include the tests (WITH_AUTOMATION_TESTS)?"; exit 2 }

$report = Get-Content $reportFile -Encoding Unicode -Raw | ConvertFrom-Json

Write-Host ""
foreach ($t in $report.tests) {
    $ok = $t.state -eq "Success"
    $tag = if ($ok) { "PASS" } else { "FAIL" }
    $color = if ($ok) { "Green" } else { "Red" }
    Write-Host ("  {0}  {1}" -f $tag, $t.fullTestPath) -ForegroundColor $color
}
Write-Host ""
$summaryColor = if ($report.failed -gt 0) { "Red" } else { "Green" }
Write-Host ("Result: succeeded={0} failed={1} notRun={2}" -f $report.succeeded, $report.failed, $report.notRun) `
    -ForegroundColor $summaryColor
Write-Host "Report: $reportFile"

if ($report.failed -gt 0 -or ($report.succeeded + $report.failed) -eq 0) { exit 1 }
exit 0

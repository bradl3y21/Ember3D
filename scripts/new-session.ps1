# new-session.ps1 - "fresh session, chosen model, chosen directory" as a button.
# Item 124, WAVE-5 Track E. Owner: Kiln - Platform (scripts/ is Build's path).
#
# WHY THIS EXISTS (WAVE-5 section 0, measured, not theorised):
#   A long-lived session re-reads ~700k tokens of cached context on EVERY step,
#   so escalating one is unaffordable: a one-word reply cost $3.28 on
#   claude-opus-5-5 vs $0.095 on opencode-go/gpt-6-luna (34x), and across a
#   15-minute window the same work ranged $0.75 vs $4.76 (6.4x). The rule:
#   frontier models go on FRESH sessions with a tight brief - this script makes
#   that a button instead of a workaround.
#
# FRESH BY CONSTRUCTION: never passes --continue; sessions start new unless you
# explicitly hand -SessionId. Resume flags are deliberately absent - resuming
# the old context is the thing this button exists to avoid.
#
# ASCII-ONLY FILE (no BOM): PowerShell 5.1 reads a BOM-less script as ANSI, and
# UTF-8 bytes in comments/strings made this script exit 0 SILENTLY before ever
# reaching its output branches (measured while building item 124). Do not add
# non-ASCII characters here; put fancy text in scripts/README.md instead.
#
# Modes (see scripts/README.md):
#   (default)   interactive mini UI in -Directory with -m <model>  (fresh)
#   -FullTui    full-screen TUI in -Directory (model picked in-session:
#               opencode v2.0.28's root command has NO --model flag - measured)
#   -Run "<p>"  non-interactive one-shot (run -m model, cwd = -Directory)
#   -PrintOnly  print the exact command + cost rationale, launch nothing
#   -ListModels show available provider/model ids and exit

[CmdletBinding()]
param(
    [string]$Model = "",
    [string]$Directory = "",
    [string]$Run = "",
    [switch]$FullTui,
    [switch]$PrintOnly,
    [switch]$ListModels,
    [string]$Agent = "",
    [string]$SessionId = ""     # optional: a NEW session id to create; never resumes
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Directory) { $Directory = Split-Path -Parent $here }   # repo root by default

function Find-OpenCodeCli {
    $cmd = Get-Command opencode -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $cmd = Get-Command opencode-cli -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $candidates = @(
        "$env:LOCALAPPDATA\Programs\@opencodedesktop\resources\opencode-cli.exe"
    )
    foreach ($c in $candidates) { if (Test-Path $c) { return $c } }
    return $null
}

$cli = Find-OpenCodeCli
if (-not $cli) {
    Write-Error "OpenCode CLI not found. Install it or add opencode to PATH (the desktop app ships opencode-cli.exe under resources\)."
}

if ($ListModels) {
    & $cli models
    exit $LASTEXITCODE
}

if (-not (Test-Path -PathType Container $Directory)) {
    Write-Error "Directory does not exist: $Directory"
}
$Directory = (Resolve-Path $Directory).Path

if (-not $Model) {
    Write-Error "Pick a model first: run this script with -ListModels, then -Model provider/model (e.g. opencode-go/gpt-6-luna). A button with no model chosen is the workaround we are removing."
}
if ($Model -notmatch '^[A-Za-z0-9._\-]+/[A-Za-z0-9._\-]+(#[A-Za-z0-9._\-]+)?$') {
    Write-Error "Model must be provider/model (or provider/model#variant), got: $Model"
}

# The measured rationale travels WITH the button (WAVE-5 section 0).
$rationale = @'
fresh-session button (item 124) - why fresh beats escalating:
  one-word reply:  claude-opus-5-5  $3.28   |  opencode-go/gpt-6-luna  $0.095   (34x)
  15-min window:   kimi-k3          $4.76   |  opencode-go/gpt-6-luna  $0.75    (6.4x)
  rule (WAVE-5 s0): frontier model = FRESH session + tight brief, never a long-lived one.
'@

$common = @()
if ($Agent) { $common += @("--agent", $Agent) }

function Format-Cmd([string[]]$argv) {
    return (($argv | ForEach-Object { if ($_ -match '\s') { '"{0}"' -f $_ } else { $_ } }) -join ' ')
}

if ($Run -ne "") {
    # One-shot: fresh session, chosen model, chosen directory (cwd is the session dir).
    $argv = @("run", "-m", $Model, "--title", "fresh: $Model") + $common
    if ($SessionId) { $argv += @("--session", $SessionId) }
    $cmdLine = "opencode-cli " + (Format-Cmd $argv) + " '<prompt>'   # cwd: $Directory"
    if ($PrintOnly) {
        Write-Output $rationale
        Write-Output ""
        Write-Output "COMMAND (cwd: $Directory):"
        Write-Output "  $cmdLine"
        exit 0
    }
    Write-Output $rationale
    Push-Location $Directory
    try {
        & $cli @argv $Run
        exit $LASTEXITCODE
    } finally { Pop-Location }
}
elseif ($FullTui) {
    $argv = @() + $common
    $cmdLine = "opencode-cli $Directory   # fresh TUI session; pick '$Model' in the model picker (root CLI has no --model in v2.0.28)"
    if ($PrintOnly) {
        Write-Output $rationale
        Write-Output ""
        Write-Output "COMMAND:"
        Write-Output "  $cmdLine"
        exit 0
    }
    Write-Output $rationale
    Write-Output "[hint] select $Model in the TUI model picker before the first prompt - pay frontier prices only while the context is small."
    Push-Location $Directory
    try { & $cli @argv $Directory; exit $LASTEXITCODE } finally { Pop-Location }
}
else {
    # Default: interactive mini UI, fresh session, model fixed AT LAUNCH.
    $argv = @("mini", "-m", $Model) + $common
    if ($SessionId) { $argv += @("--session", $SessionId) }
    $cmdLine = "opencode-cli " + (Format-Cmd $argv) + "   # cwd: $Directory"
    if ($PrintOnly) {
        Write-Output $rationale
        Write-Output ""
        Write-Output "COMMAND (cwd: $Directory):"
        Write-Output "  $cmdLine"
        exit 0
    }
    Write-Output $rationale
    Push-Location $Directory
    try { & $cli @argv; exit $LASTEXITCODE } finally { Pop-Location }
}

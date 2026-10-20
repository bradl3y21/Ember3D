# smoke_gate.ps1 - test runner for the runtime smoke tests. Owner: Kiln - Platform.
#
# Why this exists: the smoke must be REACHABLE from a DEFAULT ctest invocation
# (backlog item 37), while tests/README.md's hard rule says the default run must
# stay green with NO OpenGL context. Both hold only if the test is always
# registered but SKIPS itself where there is no GL - and CTest marks a skip via
# a specific exit code (SKIP_RETURN_CODE), which cmake -P script mode cannot
# produce (it exits 0 or 1 only).
#
# Exit codes:
#   0   run passed (and all assertions held, when -Check is given)
#   77  NO GL CONTEXT - CTest records this test as Skipped, not failed
#   else the underlying failure (build/runtime/assertion), preserved verbatim
#
# Note: a genuine GL failure (shader compile, framebuffer, assertion) does NOT
# match the no-context patterns and keeps failing the suite - fail-closed.

param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$Check,     # optional path to a RunSmoke*.cmake assertion script
    [string]$Resize     # value for EMBER_SMOKE_RESIZE, e.g. 1600x900
)

$ErrorActionPreference = "Continue"

if ($Resize) { $env:EMBER_SMOKE_RESIZE = $Resize }

if ($Check) {
    $output = & cmake "-DEMBER_SMOKE_EXE=$Exe" -P $Check 2>&1 | ForEach-Object { "$_" }
    $rc = $LASTEXITCODE
} else {
    $output = & $Exe --frames 60 --exit 2>&1 | ForEach-Object { "$_" }
    $rc = $LASTEXITCODE
}

$output | Write-Host

# No-context signatures from platform/core (Window::create and App::init paths).
$noContext = "glfwInit failed|glfwCreateWindow failed|OpenGL 4\.5 core unavailable|OpenGL 4\.5 core context unavailable|window creation failed"
if ($rc -ne 0 -and (($output -join "`n") -match $noContext)) {
    Write-Host "smoke_gate: NO GL CONTEXT on this machine - reporting SKIP (ctest return code 77)"
    exit 77
}

exit $rc

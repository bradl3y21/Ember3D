# Fail with an actionable diagnostic before the linker tries to replace a
# running Ember3D executable. This script is ASCII-only for Windows PowerShell
# 5.1 compatibility.
param(
    [Parameter(Mandatory = $true)][string]$Exe
)

$ErrorActionPreference = "Stop"
$exePath = [System.IO.Path]::GetFullPath($Exe)

# The first build has no output to lock yet.
if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) {
    exit 0
}

$stream = $null
try {
    # A running image prevents an exclusive read/write open on Windows. This
    # probes the exact configured output, including custom EMBER_OUTPUT_DIRs.
    $stream = [System.IO.File]::Open(
        $exePath,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::ReadWrite,
        [System.IO.FileShare]::None)
}
catch [System.IO.IOException] {
    [Console]::Error.WriteLine("Ember3D build: executable is locked; close Ember3D.exe and retry.")
    exit 2
}
catch [System.UnauthorizedAccessException] {
    [Console]::Error.WriteLine("Ember3D build: executable is not writable; check permissions and retry.")
    exit 2
}
finally {
    if ($null -ne $stream) {
        $stream.Dispose()
    }
}

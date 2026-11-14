# Launch from PowerShell: powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run_windows.ps1 -Mode fault
param(
    [ValidateSet('normal', 'fault', 'overload', 'test', 'benchmark', 'replay', 'qualify')]
    [string]$Mode = 'fault',
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
# PowerShell 5.1 may inherit CP65001 from a prior command.  MinGW GCC on
# Chinese Windows can then pass an incorrectly encoded -o output path to as.exe.
# Use the Windows ANSI code page (e.g. CP936) for compiler invocations.  This
# is intentionally BEFORE build_windows.ps1 and also applies after -SkipBuild.
$compilerEncoding = [System.Text.Encoding]::Default
try { & chcp.com $compilerEncoding.CodePage > $null } catch {}
try { [Console]::OutputEncoding = $compilerEncoding } catch {}
$OutputEncoding = $compilerEncoding
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $root
$bin = Join-Path $root 'build-win'
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build_windows.ps1')
    if ($LASTEXITCODE -and $LASTEXITCODE -ne 0) { throw 'Windows compilation failed.' }
}
# Once compilation has finished, switch back to UTF-8 for SignalFlight's
# UTF-8 JSON/path console output.  This avoids garbled Chinese filenames in
# demos without breaking native MinGW compilation under a Chinese project path.
$utf8 = New-Object System.Text.UTF8Encoding($false)
$OutputEncoding = $utf8
try { [Console]::OutputEncoding = $utf8 } catch {}
try { & chcp.com 65001 > $null } catch {}
$sourceVersionLine = (Get-Content (Join-Path $root 'CMakeLists.txt') -TotalCount 5 | Where-Object { $_ -match 'project\(SignalFlight VERSION' } | Select-Object -First 1)
if (-not $sourceVersionLine -or $sourceVersionLine -notmatch 'VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') { throw 'Cannot parse SignalFlight project version.' }
$sourceVersion = $Matches[1]
$binaryVersion = Join-Path $bin 'version.txt'
if (-not (Test-Path -LiteralPath $binaryVersion) -or (Get-Content -LiteralPath $binaryVersion -Raw).Trim() -ne $sourceVersion) {
    throw "Build version missing or stale: source=$sourceVersion. Run again without -SkipBuild."
}
if (-not (Test-Path (Join-Path $bin 'sf_engine.exe'))) { throw 'Windows binaries not found. Run scripts\build_windows.ps1 first.' }
$env:Path = "$(Split-Path -Parent $bin);$env:Path"
if ($env:SF_CXX -and (Test-Path $env:SF_CXX)) { $env:Path = "$(Split-Path -Parent $env:SF_CXX);$env:Path" }
foreach ($folder in @('E:\msys64\ucrt64\bin', 'C:\msys64\ucrt64\bin', 'D:\msys64\ucrt64\bin')) {
    if (Test-Path $folder) { $env:Path = "$folder;$env:Path"; break }
}
$env:SF_BIN_DIR = $bin
$pyCmd = Get-Command python.exe -ErrorAction SilentlyContinue
$py = $null
$pyArgs = @()
if ($pyCmd) { $py = $pyCmd.Source }
else {
    $pyCmd = Get-Command py.exe -ErrorAction SilentlyContinue
    if ($pyCmd) { $py = $pyCmd.Source; $pyArgs = @('-3') }
}
if (-not $py) { throw 'Python 3 not found. Activate your conda environment or install Python 3.' }
if ($Mode -eq 'qualify') {
    & $py @pyArgs (Join-Path $root 'scripts\qualify.py')
} elseif ($Mode -eq 'replay') {
    & (Join-Path $bin 'sf_replay.exe') '--input' (Join-Path $root 'examples\sample_incident')
} elseif ($Mode -eq 'test') {
    & $py @pyArgs (Join-Path $root 'scripts\verify_all.py')
} elseif ($Mode -eq 'benchmark') {
    & $py @pyArgs (Join-Path $root 'scripts\verify_all.py') '--full'
} else {
    $cmdArgs = @((Join-Path $root 'scripts\run_demo.py'))
    if ($Mode -eq 'fault') { $cmdArgs += '--fault' }
    if ($Mode -eq 'overload') { $cmdArgs += '--overload' }
    & $py @pyArgs @cmdArgs
}
if ($LASTEXITCODE -ne 0) { throw "SignalFlight $Mode failed, exit code=$LASTEXITCODE" }
Write-Host "[SignalFlight] $Mode finished successfully."

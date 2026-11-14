# Native Windows PowerShell build with MSYS2 UCRT64 g++. No WSL, Bash, CMake or Make required.
param(
    [switch]$Clean
)
$ErrorActionPreference = "Stop"
# Build using the system ANSI code page (CP936 on typical Chinese Windows).
# The MinGW/GCC assembler can misdecode non-ASCII -o paths when the console was
# changed to CP65001 before compilation.  Keep UTF-8 output configuration in
# run_windows.ps1 *after* compilation, not here.
$compilerEncoding = [System.Text.Encoding]::Default
try { & chcp.com $compilerEncoding.CodePage > $null } catch {}
try { [Console]::OutputEncoding = $compilerEncoding } catch {}
$OutputEncoding = $compilerEncoding
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$build = Join-Path $root "build-win"
if ($Clean -and (Test-Path $build)) { Remove-Item -LiteralPath $build -Recurse -Force }
New-Item -ItemType Directory -Path (Join-Path $build "obj") -Force | Out-Null
$candidates = @()
if ($env:SF_CXX) { $candidates += $env:SF_CXX }
$candidates += @("E:\msys64\ucrt64\bin\g++.exe", "C:\msys64\ucrt64\bin\g++.exe", "D:\msys64\ucrt64\bin\g++.exe", "E:\msys64\mingw64\bin\g++.exe", "C:\msys64\mingw64\bin\g++.exe")
$found = Get-Command g++.exe -ErrorAction SilentlyContinue
if ($found) { $candidates += $found.Source }
$compiler = $null
foreach ($candidate in $candidates) { if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) { $compiler = $candidate; break } }
if (-not $compiler) { throw 'g++.exe not found. Install MSYS2 UCRT64 GCC or set SF_CXX to the full g++.exe path.' }
$env:Path = "$(Split-Path -Parent $compiler);$env:Path"
Write-Host "[SignalFlight] Compiler: $compiler"
$include = Join-Path $root 'include'
$flags = @('-std=c++17', '-O2', '-Wall', '-Wextra', '-Wpedantic', "-I$include")
$objects = @()
foreach ($unit in @('protocol', 'pipeline', 'analysis', 'recorder', 'engine')) {
    $source = Join-Path $root "src\$unit.cpp"
    $obj = Join-Path $build "obj\$unit.o"
    Write-Host "[compile] $unit.cpp"
    & $compiler @flags '-c' $source '-o' $obj
    if ($LASTEXITCODE -ne 0) { throw "Compile failed: $source" }
    $objects += $obj
}
foreach ($app in @('sf_source', 'sf_engine', 'sf_replay', 'sf_bench', 'sf_crc_bench', 'test_core', 'test_hardening')) {
    if ($app -like 'test_*') { $source = Join-Path $root "tests\$app.cpp" }
    else { $source = Join-Path $root "apps\$app.cpp" }
    $out = Join-Path $build "$app.exe"
    Write-Host "[link] $app.exe"
    & $compiler @flags $source @objects '-o' $out '-lws2_32' '-lshell32'
    if ($LASTEXITCODE -ne 0) { throw "Link failed: $app" }
}
Write-Host "[SignalFlight] Build OK: $build"
& (Join-Path $build 'test_core.exe')
if ($LASTEXITCODE -ne 0) { throw 'C++ core tests failed.' }
Write-Host '[SignalFlight] C++ core tests PASS'

& (Join-Path $build 'test_hardening.exe')
if ($LASTEXITCODE -ne 0) { throw 'C++ hardening tests failed.' }
Write-Host '[SignalFlight] C++ hardening tests PASS'

# Mark the tested binaries with the source version.  Prevent -SkipBuild from
# silently running executables left over from an older unpacked version.
$versionLine = (Get-Content (Join-Path $root 'CMakeLists.txt') -TotalCount 5 | Where-Object { $_ -match 'project\(SignalFlight VERSION' } | Select-Object -First 1)
if (-not $versionLine -or $versionLine -notmatch 'VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') { throw 'Unable to read project version from CMakeLists.txt.' }
[System.IO.File]::WriteAllText((Join-Path $build 'version.txt'), $Matches[1], [System.Text.Encoding]::ASCII)

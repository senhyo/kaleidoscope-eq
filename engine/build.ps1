# Builds the engine with the installed Visual Studio C++ toolset (x64, Release):
#   build\KaleidoscopeEQ.dll   the APO (only OS DLLs as dependencies, so audiodg needs no VC++ redistributable)
#   build\KaleidoscopeEQ.pdb   its symbols (not shipped; the DLL names it without a path)
#   build\keq_test.exe         offline test harness over the same DSP code
#   build\keq_render.exe       offline WAV renderer over the same DSP code (keq_render in.wav out.wav "<cfg>")
# -Test also runs the harness. -Demo writes build\demo\source.wav and its spatial_0/50/100.wav renders.
# Nothing is installed or registered.
param([switch]$Test, [switch]$Demo)
$ErrorActionPreference = 'Stop'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'vswhere.exe not found: Visual Studio Installer is not installed.' }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No Visual Studio instance has the C++ x64 toolset (component Microsoft.VisualStudio.Component.VC.Tools.x86.x64, workload "Desktop development with C++").' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

$src = Join-Path $PSScriptRoot 'src'
$testDir = Join-Path $PSScriptRoot 'test'
$toolDir = Join-Path $PSScriptRoot 'tools'
$out = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Force -Path (Join-Path $out 'obj') | Out-Null

# Why the DLL is small (see README "크기"):
#  - "Hybrid CRT": compile /MT but link the Universal CRT dynamically (ucrt.lib instead of libucrt.lib). The UCRT is
#    part of Windows 10+ itself (System32\ucrtbase.dll behind the api-ms-win-crt-* API sets, serviced by Windows Update),
#    so this adds no dependency beyond OS DLLs. Only the compiler's small vcruntime part (DLL entry, /GS cookie,
#    /guard:cf load config, memcpy) is still linked in statically - that part is NOT in Windows (vcruntime140.dll).
#  - No C++ exceptions or RTTI in the DLL's code, and no STL containers or std::from_chars: fixed buffers instead.
#  - /O1 (size) everywhere, whole-program optimised. The DSP is double-precision arithmetic bound: /O2 measured no
#    faster (both ~0.4% of a core for 10 peaks + reverb + spatial at 48 kHz, within run-to-run noise).
$common = '/nologo /c /std:c++17 /utf-8 /permissive- /W4 /WX /MT /GS /guard:cf /Zi /DNDEBUG /DUNICODE /D_UNICODE /DNOMINMAX /Fo"obj\\" /Fd"obj\\vc.pdb"'
$clDll = "$common /O1 /GL /Gy /Gw /EHs-c- /GR- /D_HAS_EXCEPTIONS=0"
$clTest = "$common /O2 /EHsc"
$link = '/nologo /DEBUG /LTCG /OPT:REF /OPT:ICF /guard:cf /CETCOMPAT /DYNAMICBASE /NXCOMPAT /MACHINE:X64'
$linkDll = "$link /PDBALTPATH:%%_PDB%% /NODEFAULTLIB:libucrt.lib ucrt.lib"
$dllSrc = 'dsp.cpp files.cpp apo.cpp dllmain.cpp' -split ' ' | ForEach-Object { "`"$src\$_`"" }
$script = @"
@echo off
call "$vcvars" >nul 2>&1 || exit /b 1
cd /d "$out" || exit /b 1
if not exist "%WindowsSdkDir%Include\%WindowsSDKVersion%um\audioenginebaseapo.h" (echo Windows SDK with audioenginebaseapo.h not found ^(install a Windows 10/11 SDK component^) 1>&2 & exit /b 2)
cl $clDll $($dllSrc -join ' ') || exit /b 1
rc /nologo /fo obj\KaleidoscopeEQ.res "$src\KaleidoscopeEQ.rc" || exit /b 1
link $linkDll /DLL /DEF:"$src\KaleidoscopeEQ.def" /OUT:KaleidoscopeEQ.dll /PDB:KaleidoscopeEQ.pdb /MAP:obj\KaleidoscopeEQ.map obj\dsp.obj obj\files.obj obj\apo.obj obj\dllmain.obj obj\KaleidoscopeEQ.res ole32.lib advapi32.lib shell32.lib audiomediatypecrt.lib || exit /b 1
cl $clTest "$testDir\harness.cpp" || exit /b 1
link $link /OUT:keq_test.exe /PDB:keq_test.pdb obj\harness.obj obj\dsp.obj obj\files.obj ole32.lib shell32.lib audiomediatypecrt.lib || exit /b 1
cl $clTest "$toolDir\render.cpp" || exit /b 1
link $link /OUT:keq_render.exe /PDB:keq_render.pdb obj\render.obj obj\dsp.obj || exit /b 1
"@
$cmd = Join-Path $out 'obj\build.cmd'
[IO.File]::WriteAllText($cmd, $script)
$ErrorActionPreference = 'Continue'  # compiler output on stderr must not abort Windows PowerShell 5.1
& cmd.exe /d /c "`"$cmd`"" 2>&1 | Write-Host
$ErrorActionPreference = 'Stop'
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
Write-Host "built $out\KaleidoscopeEQ.dll ($((Get-Item (Join-Path $out 'KaleidoscopeEQ.dll')).Length) bytes), $out\keq_test.exe, $out\keq_render.exe"

if ($Test) {
    & (Join-Path $out 'keq_test.exe')
    if ($LASTEXITCODE -ne 0) { throw 'tests failed' }
}

if ($Demo) {
    $demoDir = Join-Path $out 'demo'
    New-Item -ItemType Directory -Force -Path $demoDir | Out-Null
    & (Join-Path $out 'keq_render.exe') --demo $demoDir
    if ($LASTEXITCODE -ne 0) { throw 'demo render failed' }
}

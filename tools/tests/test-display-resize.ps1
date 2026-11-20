param([string]$VisualStudioPath)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (!$VisualStudioPath) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $VisualStudioPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (!$VisualStudioPath) { throw 'MSVC x64 C++ build tools are required.' }
$vcvars = Join-Path $VisualStudioPath 'VC\Auxiliary\Build\vcvars64.bat'
$output = Join-Path $repo 'bin\tests\display-resize'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$driver = Get-Content (Join-Path $repo 'tools/vdd/vdd.cpp') -Raw
$handler = [regex]::Match($driver, '(?s)static BOOL VddHandleResize\(.*?(?=static DWORD WINAPI VddGpuIoThread)')
if (!$handler.Success) { throw 'VDD resize handler not found; update the harness extraction.' }
Set-Content (Join-Path $output 'vdd_resize_handler.inc') $handler.Value -Encoding ascii
$commands = @"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b %errorlevel%
cl /nologo /W4 /WX /TC /Fe:display-resize-test.exe "$repo\tools\tests\display_resize_test.c"
if errorlevel 1 exit /b %errorlevel%
display-resize-test.exe
if errorlevel 1 exit /b %errorlevel%
cl /nologo /W4 /WX /EHsc /std:c++17 /I"$output" /Fe:display-resize-driver-test.exe "$repo\tools\tests\display_resize_driver_test.cpp"
if errorlevel 1 exit /b %errorlevel%
display-resize-driver-test.exe
exit /b %errorlevel%
"@
$script = Join-Path $output 'run.cmd'
Set-Content -LiteralPath $script -Value $commands -Encoding ascii
Push-Location $output
try {
    & $env:ComSpec /d /c $script
    if ($LASTEXITCODE -ne 0) { throw "Display resize tests failed ($LASTEXITCODE)." }
} finally { Pop-Location }


param([string]$VisualStudioPath)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (!$VisualStudioPath) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $VisualStudioPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (!$VisualStudioPath) { throw 'MSVC x64 C++ build tools are required.' }
$vcvars = Join-Path $VisualStudioPath 'VC\Auxiliary\Build\vcvars64.bat'
$output = Join-Path $repo 'bin\tests\fullscreen-controls'
New-Item -ItemType Directory -Force -Path $output | Out-Null
node "$PSScriptRoot/fullscreen_controls_native_source.cjs" "$output/test.c"
if ($LASTEXITCODE -ne 0) { throw 'Could not extract overlay functions.' }
$commands = @"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b %errorlevel%
cl /nologo /W4 /WX /TC /DUNICODE /D_UNICODE /I"$repo\src\core" /Fe:fullscreen-controls-test.exe test.c "$repo\src\core\api_json.c" /link user32.lib
if errorlevel 1 exit /b %errorlevel%
fullscreen-controls-test.exe
exit /b %errorlevel%
"@
$script = Join-Path $output 'run.cmd'
Set-Content -LiteralPath $script -Value $commands -Encoding ascii
Push-Location $output
try {
    & $env:ComSpec /d /c $script
    if ($LASTEXITCODE -ne 0) { throw "Fullscreen controls tests failed ($LASTEXITCODE)." }
} finally { Pop-Location }


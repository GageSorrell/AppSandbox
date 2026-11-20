param([string]$VisualStudioPath)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (!$VisualStudioPath) {
    $vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $VisualStudioPath=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (!$VisualStudioPath) { throw 'MSVC x64 build tools are required.' }
# Generate the actual viewer-function include and run its focused regression first.
& "$PSScriptRoot/test-fullscreen-resolution.ps1" -VisualStudioPath $VisualStudioPath
$vcvars=Join-Path $VisualStudioPath 'VC\Auxiliary\Build\vcvars64.bat'
$output=Join-Path $repo 'bin/tests/display-integration'
New-Item -ItemType Directory -Force $output | Out-Null
$commands=@"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b %errorlevel%
cl /nologo /W4 /WX /TC /DUNICODE /D_UNICODE /I"$repo\bin\tests\fullscreen-resolution" /Fe:display-integration-test.exe "$repo\tools\tests\display_integration_test.c" "$repo\src\app_win\display_resolution_command.c" "$repo\src\backend_win\display_settings.c" "$repo\src\backend_win\display_modes.c" "$repo\src\core\api_json.c" /link user32.lib
exit /b %errorlevel%
"@
$script=Join-Path $output 'build.cmd'
Set-Content $script $commands -Encoding ascii
Push-Location $output
try {
    & $env:ComSpec /d /c $script
    if ($LASTEXITCODE -ne 0) { throw 'Display integration compilation failed.' }
} finally { Pop-Location }
node "$PSScriptRoot/display_integration_test.cjs"
if ($LASTEXITCODE -ne 0) { throw 'Display integration tests failed.' }

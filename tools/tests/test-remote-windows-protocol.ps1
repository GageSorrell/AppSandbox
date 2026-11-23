param([string]$VisualStudioPath)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (!$VisualStudioPath) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $VisualStudioPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (!$VisualStudioPath) { throw 'MSVC x64 C++ build tools are required.' }
$vcvars = Join-Path $VisualStudioPath 'VC\Auxiliary\Build\vcvars64.bat'
$output = Join-Path $repo 'bin\tests\remote-windows-protocol'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$exe = Join-Path $output 'remote-windows-protocol-test.exe'
$commands = @"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b %errorlevel%
cl /nologo /W4 /WX /TC /DUNICODE /D_UNICODE /DASB_API= /Fe:"$exe" "$repo\tools\tests\remote_windows_protocol_test.c" "$repo\tools\transport\asb_remote_windows.c" "$repo\tools\agent\remote_windows_inventory.c" "$repo\src\backend_win\hcs_services.c" /link user32.lib dwmapi.lib version.lib advapi32.lib
exit /b %errorlevel%
"@
$script = Join-Path $output 'build.cmd'
Set-Content -LiteralPath $script -Value $commands -Encoding ascii
Push-Location $output
try {
    & $env:ComSpec /d /c $script
    if ($LASTEXITCODE -ne 0) { throw "Remote Windows protocol test build failed ($LASTEXITCODE)." }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "Remote Windows protocol tests failed ($LASTEXITCODE)." }
} finally { Pop-Location }

param([string]$VisualStudioPath)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (!$VisualStudioPath) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $VisualStudioPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (!$VisualStudioPath) { throw 'MSVC x64 C++ build tools are required.' }
$vcvars = Join-Path $VisualStudioPath 'VC\Auxiliary\Build\vcvars64.bat'
$output = Join-Path $repo 'bin\tests\fullscreen-entry'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$viewer = Get-Content (Join-Path $repo 'src/backend_win/vm_display_idd.c') -Raw
$gate = [regex]::Match($viewer,'(?s)/\* BEGIN FULLSCREEN ENTRY GATE \*/(.*?)/\* END FULLSCREEN ENTRY GATE \*/')
if (!$gate.Success) { throw 'Fullscreen gate extraction failed.' }
$inputGuard = [regex]::Match($viewer,'(?ms)^static BOOL send_input_locked\([^;{]*?\)\s*\{.*?^}')
if (!$inputGuard.Success) { throw 'Input guard extraction failed.' }
$template = Get-Content (Join-Path $PSScriptRoot 'fullscreen_entry_test.c') -Raw
Set-Content (Join-Path $output 'test.c') $template.Replace('/* ACTUAL FULLSCREEN GATE */',$gate.Groups[1].Value).Replace('/* ACTUAL INPUT GUARD */',$inputGuard.Value)
$http = Get-Content (Join-Path $repo 'src/app_win/headless.c') -Raw
$watch = [regex]::Match($http,'(?ms)^static BOOL display_set_fullscreen_for_request\([^;{]*?\)\s*\{.*?^}')
if (!$watch.Success) { throw 'HTTP disconnect watcher extraction failed.' }
$httpTemplate = Get-Content (Join-Path $PSScriptRoot 'fullscreen_http_test.c') -Raw
Set-Content (Join-Path $output 'http-test.c') $httpTemplate.Replace('/* ACTUAL HTTP FULLSCREEN WATCH */',$watch.Value)
$commands = @"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b %errorlevel%
cl /nologo /W4 /WX /TC /DUNICODE /D_UNICODE /I"$repo\src\backend_win" /I"$repo\src\core" /I"$repo\tools\transport" /Fe:fullscreen-entry-test.exe test.c /link user32.lib
if errorlevel 1 exit /b %errorlevel%
fullscreen-entry-test.exe
if errorlevel 1 exit /b %errorlevel%
cl /nologo /W4 /WX /TC /DUNICODE /D_UNICODE /Fe:fullscreen-http-test.exe http-test.c
if errorlevel 1 exit /b %errorlevel%
fullscreen-http-test.exe
exit /b %errorlevel%
"@
$script = Join-Path $output 'run.cmd'
Set-Content -LiteralPath $script -Value $commands -Encoding ascii
Push-Location $output
try {
    & $env:ComSpec /d /c $script
    if ($LASTEXITCODE -ne 0) { throw "Fullscreen entry tests failed ($LASTEXITCODE)." }
} finally { Pop-Location }

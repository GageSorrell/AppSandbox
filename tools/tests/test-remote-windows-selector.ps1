param([string]$VisualStudioPath, [switch]$SkipNative, [string]$Evidence)
# Remote Windows host and guest tray selectors (AppSandbox-eh6.8).
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (!$VisualStudioPath) {
    $vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $VisualStudioPath=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
$output=Join-Path $repo 'bin/tests/remote-windows-selector'
New-Item -ItemType Directory -Force $output | Out-Null
$vcvars=Join-Path $VisualStudioPath 'VC/Auxiliary/Build/vcvars64.bat'
$a="$repo/tools/agent"; $t="$repo/tools/transport"; $b="$repo/src/backend_win"
$evidenceArg = if ($Evidence) { "--evidence `"$Evidence`"" } else { '' }
$native = if ($SkipNative) { 'rem native tests skipped' } else {
    ".\remote-windows-selector-native-test.exe $evidenceArg" + "`r`n" + 'if errorlevel 1 exit /b %errorlevel%'
}
$commands=@"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b %errorlevel%
rem Model suite: placement on every taskbar edge, tray click sequences, row/status texts, duplicate
rem labels, the host session's guest-selector path and the helper's guest requests/icons/model.
cl /nologo /W4 /WX /TC /DUNICODE /D_UNICODE /Fe:selector-test.exe "$repo/tools/tests/remote_windows_selector_test.c" "$b/remote_windows_session.c" "$a/remote_windows_helper.c" "$a/remote_windows_share.c" "$a/remote_windows_flyout.c" "$t/asb_remote_windows.c" /link user32.lib
if errorlevel 1 exit /b %errorlevel%
.\selector-test.exe
if errorlevel 1 exit /b %errorlevel%
rem Native UI: real flyouts (keyboard, pending/rejected toggles, live changes, disabled/empty states,
rem theme, DPI, scrolling, MSAA), tray re-creation on TaskbarCreated and the guest tray thread.
cl /nologo /W4 /WX /TC /DUNICODE /D_UNICODE /Fe:remote-windows-selector-native-test.exe "$repo/tools/tests/remote_windows_selector_native_test.c" "$a/remote_windows_flyout.c" "$a/remote_windows_tray.c" "$a/remote_windows_control_win32.c" "$t/asb_remote_windows.c" /link user32.lib gdi32.lib
if errorlevel 1 exit /b %errorlevel%
$native
exit /b 0
"@
Set-Content "$output/run.cmd" $commands -Encoding ascii
Push-Location $output
try {
    $env:NoDefaultCurrentDirectoryInExePath=$null
    & $env:ComSpec /d /c "$output/run.cmd"
    if ($LASTEXITCODE -ne 0) { throw 'Remote Windows selector tests failed.' }
} finally { Pop-Location }

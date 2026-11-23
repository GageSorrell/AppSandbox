param([string]$VisualStudioPath, [switch]$SkipNative)
# Remote Windows guest helper, window parking and restoration journal (AppSandbox-eh6.4).
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (!$VisualStudioPath) {
    $vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $VisualStudioPath=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
$output=Join-Path $repo 'bin/tests/remote-windows-share'
New-Item -ItemType Directory -Force $output | Out-Null
$vcvars=Join-Path $VisualStudioPath 'VC/Auxiliary/Build/vcvars64.bat'
$a="$repo/tools/agent"; $t="$repo/tools/transport"
$libs='user32.lib ole32.lib uuid.lib shell32.lib setupapi.lib dwmapi.lib version.lib advapi32.lib'
$native = if ($SkipNative) { 'rem native test skipped' } else { '.\remote-windows-share-native-test.exe' + "`r`n" + 'if errorlevel 1 exit /b %errorlevel%' }
$commands=@"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b %errorlevel%
rem Model suite: the real parking/journal core and helper session against a window model.
cl /nologo /W4 /WX /TC /Fe:share-test.exe "$repo/tools/tests/remote_windows_share_test.c" "$a/remote_windows_share.c" "$a/remote_windows_helper.c" "$a/remote_windows_flyout.c" "$t/asb_remote_windows.c" /link user32.lib
if errorlevel 1 exit /b %errorlevel%
.\share-test.exe
if errorlevel 1 exit /b %errorlevel%
rem Native suite: real windows in another process, real Win32 ops (host mode: off-screen private region).
rem The same binary run in a Windows 11 guest with --guest uses the real private display.
cl /nologo /W4 /WX /TC /Fe:remote-windows-share-native-test.exe "$repo/tools/tests/remote_windows_share_native_test.c" "$a/remote_windows_share.c" "$a/remote_windows_share_win32.c" "$a/remote_windows_inventory.c" "$a/remote_windows_display.c" "$t/asb_remote_windows.c" /link $libs
if errorlevel 1 exit /b %errorlevel%
$native
rem The helper itself (guest-only executable) with its capture engine (eh6.5, C++/WinRT).
cl /nologo /c /W4 /WX /std:c++20 /EHsc /permissive- /DUNICODE /D_UNICODE /Fo:capture.obj "$a/remote_windows_capture.cpp"
if errorlevel 1 exit /b %errorlevel%
rem eh6.9 drag-and-drop bridge (C++).
cl /nologo /c /W4 /WX /std:c++20 /EHsc /permissive- /DUNICODE /D_UNICODE /Fo:files.obj "$a/remote_windows_files.cpp"
if errorlevel 1 exit /b %errorlevel%
cl /nologo /c /W4 /WX /std:c++20 /EHsc /permissive- /DUNICODE /D_UNICODE /Fo:files_win32.obj "$a/remote_windows_files_win32.cpp"
if errorlevel 1 exit /b %errorlevel%
cl /nologo /W4 /WX /DUNICODE /D_UNICODE /Fe:appsandbox-remote-windows.exe "$a/appsandbox-remote-windows.c" "$a/remote_windows_tray.c" "$a/remote_windows_flyout.c" "$a/remote_windows_helper.c" "$a/remote_windows_share.c" "$a/remote_windows_share_win32.c" "$a/remote_windows_inventory.c" "$a/remote_windows_display.c" "$a/remote_windows_control_win32.c" "$t/asb_remote_windows.c" "$t/asb_rw_frames.c" "$t/asb_transport.c" "$t/asb_rw_files.c" "$t/asb_clipboard_temp.c" capture.obj files.obj files_win32.obj /link ws2_32.lib bcrypt.lib d3d11.lib dxgi.lib windowsapp.lib shlwapi.lib wtsapi32.lib $libs
exit /b %errorlevel%
"@
Set-Content "$output/run.cmd" $commands -Encoding ascii
Push-Location $output
try {
    $env:NoDefaultCurrentDirectoryInExePath=$null
    & $env:ComSpec /d /c "$output/run.cmd"
    if ($LASTEXITCODE -ne 0) { throw 'Remote Windows share tests failed.' }
} finally { Pop-Location }
node (Join-Path $repo 'tools/tests/remote_windows_registration_test.cjs')
if ($LASTEXITCODE -ne 0) { throw 'Remote Windows registration test failed.' }

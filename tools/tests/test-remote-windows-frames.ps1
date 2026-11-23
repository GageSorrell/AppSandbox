param([string]$VisualStudioPath, [switch]$SkipNative, [string]$Evidence)
# Remote Windows per-window capture and frame delivery (AppSandbox-eh6.5).
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (!$VisualStudioPath) {
    $vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $VisualStudioPath=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
$output=Join-Path $repo 'bin/tests/remote-windows-frames'
New-Item -ItemType Directory -Force $output | Out-Null
$vcvars=Join-Path $VisualStudioPath 'VC/Auxiliary/Build/vcvars64.bat'
$a="$repo/tools/agent"; $t="$repo/tools/transport"; $b="$repo/src/backend_win"
$libs='user32.lib ole32.lib uuid.lib shell32.lib setupapi.lib dwmapi.lib version.lib advapi32.lib'
$evidenceArg = if ($Evidence) { "--evidence `"$Evidence`"" } else { '' }
$native = if ($SkipNative) { 'rem native test skipped' } else { ".\remote-windows-frames-native-test.exe $evidenceArg" + "`r`n" + 'if errorlevel 1 exit /b %errorlevel%' }
$commands=@"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b %errorlevel%
rem Model suite: codec validation, negotiation, generations, bounds, pools, teardown, popup rules.
cl /nologo /W4 /WX /TC /Fe:frames-test.exe "$repo/tools/tests/remote_windows_frames_test.c" "$t/asb_rw_frames.c" "$t/asb_remote_windows.c"
if errorlevel 1 exit /b %errorlevel%
.\frames-test.exe
if errorlevel 1 exit /b %errorlevel%
rem Product objects: guest capture engine (C++/WinRT) and host receiver.
cl /nologo /c /W4 /WX /std:c++20 /EHsc /permissive- /DUNICODE /D_UNICODE /Fo:capture.obj "$a/remote_windows_capture.cpp"
if errorlevel 1 exit /b %errorlevel%
cl /nologo /c /W4 /WX /TC /DASB_API= /DUNICODE /D_UNICODE /Fo:frames-host.obj "$b/remote_windows_frames_host.c"
if errorlevel 1 exit /b %errorlevel%
rem Native suite: real WGC capture, real engine and host receiver over loopback TCP.
cl /nologo /c /W4 /WX /TC /DASB_API= /DUNICODE /D_UNICODE /Fo:native.obj "$repo/tools/tests/remote_windows_frames_native_test.c"
if errorlevel 1 exit /b %errorlevel%
cl /nologo /c /W4 /WX /TC /DASB_API= /DUNICODE /D_UNICODE "$t/asb_rw_frames.c" "$t/asb_remote_windows.c" "$a/remote_windows_inventory.c" "$b/hcs_services.c"
if errorlevel 1 exit /b %errorlevel%
link /nologo /out:remote-windows-frames-native-test.exe native.obj capture.obj frames-host.obj asb_rw_frames.obj asb_remote_windows.obj remote_windows_inventory.obj hcs_services.obj ws2_32.lib d3d11.lib dxgi.lib windowsapp.lib gdi32.lib $libs
if errorlevel 1 exit /b %errorlevel%
$native
rem Guest check tools: host-side VM checker and the guest test app (built only; see tools/tests/README.md).
cl /nologo /W4 /WX /TC /DASB_API= /DUNICODE /D_UNICODE /Fe:remote-windows-frames-vm-check.exe "$repo/tools/tests/remote_windows_frames_vm_check.c" "$b/remote_windows_frames_host.c" "$t/asb_rw_frames.c" "$t/asb_remote_windows.c" "$b/hcs_services.c" /link ws2_32.lib d3d11.lib dxgi.lib ole32.lib user32.lib
if errorlevel 1 exit /b %errorlevel%
cl /nologo /W4 /WX /TC /DUNICODE /D_UNICODE /Fe:remote-windows-capture-test-app.exe "$repo/tools/tests/remote_windows_capture_test_app.c" /link /subsystem:windows
if errorlevel 1 exit /b %errorlevel%
rem The helper with its capture engine (guest-only executable).
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
    if ($LASTEXITCODE -ne 0) { throw 'Remote Windows frame tests failed.' }
} finally { Pop-Location }

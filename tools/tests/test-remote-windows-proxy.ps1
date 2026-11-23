param([string]$VisualStudioPath, [switch]$SkipNative, [switch]$Snap, [string]$Evidence)
# Remote Windows native host proxies (AppSandbox-eh6.6).
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (!$VisualStudioPath) {
    $vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $VisualStudioPath=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
$output=Join-Path $repo 'bin/tests/remote-windows-proxy'
New-Item -ItemType Directory -Force $output | Out-Null
$vcvars=Join-Path $VisualStudioPath 'VC/Auxiliary/Build/vcvars64.bat'
$a="$repo/tools/agent"; $t="$repo/tools/transport"; $b="$repo/src/backend_win"
$libs='user32.lib ole32.lib uuid.lib shell32.lib setupapi.lib dwmapi.lib version.lib advapi32.lib gdi32.lib propsys.lib'
$evidenceArg = if ($Evidence) { "--evidence `"$Evidence`"" } else { '' }
$snapArg = if ($Snap) { '--snap' } else { '' }
$native = if ($SkipNative) { 'rem native tests skipped' } else {
    ".\remote-windows-proxy-native-test.exe $snapArg $evidenceArg" + "`r`n" + 'if errorlevel 1 exit /b %errorlevel%' + "`r`n" +
    ".\remote-windows-proxy-native-test.exe --cpu $evidenceArg" + "`r`n" + 'if errorlevel 1 exit /b %errorlevel%'
}
$commands=@"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b %errorlevel%
rem Model suite: host session (identities/lifetimes, acknowledged geometry, state feedback, modal
rem ownership, close cancellation, hit cache) and the helper's eh6.6 handlers, plus protocol codecs.
cl /nologo /W4 /WX /TC /DUNICODE /D_UNICODE /Fe:proxy-test.exe "$repo/tools/tests/remote_windows_proxy_test.c" "$b/remote_windows_session.c" "$a/remote_windows_helper.c" "$a/remote_windows_flyout.c" "$a/remote_windows_share.c" "$t/asb_remote_windows.c" /link user32.lib
if errorlevel 1 exit /b %errorlevel%
.\proxy-test.exe
if errorlevel 1 exit /b %errorlevel%
rem Product objects: host manager/proxies (C++), host session, frame receiver; guest helper parts.
cl /nologo /c /W4 /WX /std:c++20 /EHsc /permissive- /DUNICODE /D_UNICODE /DASB_API= /Fo:proxy.obj "$b/remote_windows_proxy.cpp"
if errorlevel 1 exit /b %errorlevel%
cl /nologo /c /W4 /WX /std:c++20 /EHsc /permissive- /DUNICODE /D_UNICODE /Fo:capture.obj "$a/remote_windows_capture.cpp"
if errorlevel 1 exit /b %errorlevel%
cl /nologo /c /W4 /WX /TC /DASB_API= /DUNICODE /D_UNICODE "$b/remote_windows_session.c" "$b/remote_windows_frames_host.c" "$b/hcs_services.c" "$t/asb_rw_frames.c" "$t/asb_remote_windows.c" "$a/remote_windows_helper.c" "$a/remote_windows_flyout.c" "$a/remote_windows_share.c" "$a/remote_windows_share_win32.c" "$a/remote_windows_inventory.c" "$a/remote_windows_display.c" "$a/remote_windows_control_win32.c" "$repo/tools/tests/remote_windows_proxy_native_test.c"
if errorlevel 1 exit /b %errorlevel%
link /nologo /out:remote-windows-proxy-native-test.exe remote_windows_proxy_native_test.obj proxy.obj capture.obj remote_windows_session.obj remote_windows_frames_host.obj hcs_services.obj asb_rw_frames.obj asb_remote_windows.obj remote_windows_helper.obj remote_windows_flyout.obj remote_windows_share.obj remote_windows_share_win32.obj remote_windows_inventory.obj remote_windows_display.obj remote_windows_control_win32.obj ws2_32.lib d3d11.lib dxgi.lib dcomp.lib windowsapp.lib $libs
if errorlevel 1 exit /b %errorlevel%
cl /nologo /W4 /WX /TC /DUNICODE /D_UNICODE /Fe:remote-windows-proxy-test-app.exe "$repo/tools/tests/remote_windows_proxy_test_app.c" /link /subsystem:windows user32.lib gdi32.lib
if errorlevel 1 exit /b %errorlevel%
rem Host-side VM checker (real manager over AF_HYPERV; see tools/tests/README.md).
cl /nologo /W4 /WX /TC /DASB_API= /DUNICODE /D_UNICODE /c "$repo/tools/tests/remote_windows_proxy_vm_check.c"
if errorlevel 1 exit /b %errorlevel%
link /nologo /out:remote-windows-proxy-vm-check.exe remote_windows_proxy_vm_check.obj proxy.obj remote_windows_session.obj remote_windows_frames_host.obj hcs_services.obj asb_rw_frames.obj asb_remote_windows.obj ws2_32.lib d3d11.lib dxgi.lib dcomp.lib $libs
if errorlevel 1 exit /b %errorlevel%
$native
exit /b 0
"@
Set-Content "$output/run.cmd" $commands -Encoding ascii
Push-Location $output
try {
    $env:NoDefaultCurrentDirectoryInExePath=$null
    & $env:ComSpec /d /c "$output/run.cmd"
    if ($LASTEXITCODE -ne 0) { throw 'Remote Windows proxy tests failed.' }
} finally { Pop-Location }

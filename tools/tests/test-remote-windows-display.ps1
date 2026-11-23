param([string]$VisualStudioPath)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (!$VisualStudioPath) {
    $vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $VisualStudioPath=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
$output=Join-Path $repo 'bin/tests/remote-windows-display'
New-Item -ItemType Directory -Force $output | Out-Null
$driver=Get-Content "$repo/tools/vdd/vdd.cpp" -Raw
$monitor=[regex]::Match($driver,'(?s)static NTSTATUS VddCreatePrivateMonitor\(.*?(?=#include "private_control.inl")')
$modes=[regex]::Match($driver,'(?s)static UINT VddTargetResolutions\(.*?(?=static void VddNotifyGpuMode)')
$edid=[regex]::Match($driver,'(?s)static void VddChecksum\(.*?(?=/\* =+ \*/\s*/\*  Monitor mode helper)')
$keep=[regex]::Match($driver,'(?s)#define VDD_KEPT_CLIENT_MS.*?(?=static void VddDestroySwapProc)')
$template=[regex]::Match((Get-Content "$repo/tools/vdd/vdd.h" -Raw),'(?s)static const BYTE VDD_EDID\[\] = \{.*?\};')
if (!$monitor.Success -or !$modes.Success -or !$edid.Success -or !$template.Success -or !$keep.Success) { throw 'Actual driver functions not found.' }
Set-Content "$output/vdd_private_monitor.inc" $monitor.Value -Encoding ascii
Set-Content "$output/vdd_target_modes.inc" $modes.Value -Encoding ascii
Set-Content "$output/vdd_keep_client.inc" $keep.Value -Encoding ascii
Set-Content "$output/vdd_edid.inc" ($template.Value + "`r`n" + $edid.Value) -Encoding ascii
$vcvars=Join-Path $VisualStudioPath 'VC/Auxiliary/Build/vcvars64.bat'
$commands=@"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b %errorlevel%
cl /nologo /W4 /WX /EHsc /std:c++17 /I"$output" /Fe:driver-test.exe "$repo/tools/tests/remote_windows_display_driver_test.cpp"
if errorlevel 1 exit /b %errorlevel%
.\driver-test.exe
if errorlevel 1 exit /b %errorlevel%
cl /nologo /W4 /WX /EHsc /std:c++17 /I"$output" /Fe:keep-test.exe "$repo/tools/tests/remote_windows_display_keep_test.cpp"
if errorlevel 1 exit /b %errorlevel%
.\keep-test.exe
if errorlevel 1 exit /b %errorlevel%
cl /nologo /W4 /WX /TC /c "$repo/tools/agent/remote_windows_display.c" "$repo/tools/agent/appsandbox-displays.c"
if errorlevel 1 exit /b %errorlevel%
cl /nologo /W4 /WX /TC /Fe:guest-test.exe "$repo/tools/tests/remote_windows_display_guest_test.c" /link user32.lib setupapi.lib dwmapi.lib
if errorlevel 1 exit /b %errorlevel%
.\guest-test.exe
if errorlevel 1 exit /b %errorlevel%
rem Guest-only native check: built here, run inside a Windows 11 guest as a standard user.
cl /nologo /W4 /WX /TC /Fe:remote-windows-display-native-check.exe "$repo/tools/tests/remote_windows_display_native_check.c" /link user32.lib setupapi.lib bcrypt.lib advapi32.lib
if errorlevel 1 exit /b %errorlevel%
rem Guest-only manual helper: start/stop/status of the private display.
cl /nologo /W4 /WX /TC /Fe:asb-private-display.exe "$repo/tools/tests/remote_windows_display_toggle.c" /link user32.lib setupapi.lib bcrypt.lib
exit /b %errorlevel%
"@
Set-Content "$output/run.cmd" $commands -Encoding ascii
Push-Location $output
try {
    & $env:ComSpec /d /c "$output/run.cmd"
    if ($LASTEXITCODE -ne 0) { throw 'Remote Windows display tests failed.' }
} finally { Pop-Location }

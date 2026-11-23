param([string]$VisualStudioPath, [switch]$SkipNative, [switch]$Verbose)
# Remote Windows two-way copy-only OLE file and folder drag and drop (AppSandbox-eh6.9).
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (!$VisualStudioPath) {
    $vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $VisualStudioPath=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (!$VisualStudioPath) { throw 'MSVC x64 C++ build tools are required.' }
$output=Join-Path $repo 'bin/tests/remote-windows-files'
New-Item -ItemType Directory -Force $output | Out-Null
$vcvars=Join-Path $VisualStudioPath 'VC/Auxiliary/Build/vcvars64.bat'
$a="$repo/tools/agent"; $t="$repo/tools/transport"; $b="$repo/src/backend_win"
$verboseArg = if ($Verbose) { '--verbose' } else { '' }
$native = if ($SkipNative) { 'rem native tests skipped' } else {
    "`"%~dp0remote-windows-files-native-test.exe`" $verboseArg" + "`r`n" + 'if errorlevel 1 exit /b %errorlevel%'
}
$commands=@"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b %errorlevel%
cd /d "%~dp0"
rem Model suite: channel 11 receive rules (ordering, hostile names, orphans/duplicates, data, totals).
cl /nologo /W4 /WX /TC /DUNICODE /D_UNICODE /Fe:remote-windows-files-test.exe "$repo/tools/tests/remote_windows_files_test.c" "$t/asb_rw_files.c" "$t/asb_remote_windows.c"
if errorlevel 1 exit /b %errorlevel%
"%~dp0remote-windows-files-test.exe"
if errorlevel 1 exit /b %errorlevel%
rem Native suite: real sources/staging, guest bridge + host broker over loopback with real OLE objects.
cl /nologo /c /W4 /WX /TC /DUNICODE /D_UNICODE "$t/asb_rw_files.c" "$t/asb_remote_windows.c" "$t/asb_clipboard_temp.c"
if errorlevel 1 exit /b %errorlevel%
cl /nologo /c /W4 /WX /std:c++20 /EHsc /permissive- /DUNICODE /D_UNICODE "$a/remote_windows_files_win32.cpp" "$a/remote_windows_files.cpp" "$b/remote_windows_drag_host.cpp" "$repo/tools/tests/remote_windows_files_native_test.cpp"
if errorlevel 1 exit /b %errorlevel%
link /nologo /out:remote-windows-files-native-test.exe remote_windows_files_native_test.obj remote_windows_files_win32.obj remote_windows_files.obj remote_windows_drag_host.obj asb_rw_files.obj asb_remote_windows.obj asb_clipboard_temp.obj ws2_32.lib user32.lib gdi32.lib ole32.lib uuid.lib shell32.lib shlwapi.lib comctl32.lib dwmapi.lib advapi32.lib bcrypt.lib wtsapi32.lib
if errorlevel 1 exit /b %errorlevel%
$native
exit /b 0
"@
Set-Content "$output/run.cmd" $commands -Encoding ascii
Push-Location $output
try {
    $env:NoDefaultCurrentDirectoryInExePath=$null
    & $env:ComSpec /d /c "$output/run.cmd"
    if ($LASTEXITCODE -ne 0) { throw 'Remote Windows files tests failed.' }
} finally { Pop-Location }

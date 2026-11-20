param([string]$VisualStudioPath)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (!$VisualStudioPath) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $VisualStudioPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (!$VisualStudioPath) { throw 'MSVC x64 C++ build tools are required.' }
$vcvars = Join-Path $VisualStudioPath 'VC\Auxiliary\Build\vcvars64.bat'
$output = Join-Path $repo 'bin\tests\windowed-resolution'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$viewer = Get-Content (Join-Path $repo 'src/backend_win/vm_display_idd.c') -Raw
$functions = foreach ($name in @('idd_display_settings_load', 'idd_resize_available', 'idd_size_window_content', 'idd_update_windowed_resolution', 'idd_windowed_match_available', 'idd_toggle_windowed_match', 'compute_letterbox', 'window_to_vm_coords')) {
    $match = [regex]::Match($viewer, "(?ms)^static (?:void|BOOL) $name\(.*?^}")
    if (!$match.Success) { throw "Viewer function $name not found; update harness extraction." }
    $match.Value
}
Set-Content (Join-Path $output 'windowed_viewer_functions.inc') ($functions -join "`r`n") -Encoding ascii
# Check event integration as well as the compiled policy functions.
$checks = @(
    'case WM_IDD_DISPLAY_SIZE:\s*if \(d && _wcsicmp\(d->os_type,L"windows"\) == 0\) \{\s*idd_update_display_size\(d,FALSE\);\s*return 0;',
    'if \(matched && packet.status == S_OK\) d->guest_size_mode = packet.slot;',
    'if \(\(_wcsicmp\(d->os_type,L"windows"\) == 0 \|\| d->size_update_pending\)',
    'd->size_mode = DISPLAY_SIZE_GUEST; /\* Match is never persisted. \*/',
    'if \(_wcsicmp\(d->os_type,L"windows"\) == 0\) \{\s*idd_toggle_windowed_match\(d\);\s*return 0;',
    'if \(fixed\) idd_size_window_content\(d, d->saved_windowed_width, d->saved_windowed_height\);',
    'UINT width = LOWORD\(frame_size\), height = HIWORD\(frame_size\);',
    'width, height, normalized, &vx, &vy'
)
foreach ($pattern in $checks) {
    if ($viewer -notmatch $pattern) { throw "Viewer event integration missing: $pattern" }
}
if ((Get-Content (Join-Path $repo 'AppSandbox.vcxproj') -Raw) -notmatch 'ClInclude Include="src\\backend_win\\windowed_resolution.h"') {
    throw 'Windowed policy header is missing from the app project.'
}
$commands = @"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b %errorlevel%
cl /nologo /W4 /WX /TC /DUNICODE /D_UNICODE /I"$output" /Fe:windowed-resolution-test.exe "$repo\tools\tests\windowed_resolution_test.c" /link user32.lib
if errorlevel 1 exit /b %errorlevel%
windowed-resolution-test.exe
exit /b %errorlevel%
"@
$script = Join-Path $output 'run.cmd'
Set-Content -LiteralPath $script -Value $commands -Encoding ascii
Push-Location $output
try {
    & $env:ComSpec /d /c $script
    if ($LASTEXITCODE -ne 0) { throw "Windowed resolution tests failed ($LASTEXITCODE)." }
} finally { Pop-Location }

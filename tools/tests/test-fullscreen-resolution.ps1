param([string]$VisualStudioPath)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (!$VisualStudioPath) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $VisualStudioPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (!$VisualStudioPath) { throw 'MSVC x64 C++ build tools are required.' }
$vcvars = Join-Path $VisualStudioPath 'VC\Auxiliary\Build\vcvars64.bat'
$output = Join-Path $repo 'bin\tests\fullscreen-resolution'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$viewer = Get-Content (Join-Path $repo 'src/backend_win/vm_display_idd.c') -Raw
$functions = foreach ($name in @('idd_display_settings_load', 'idd_resize_available', 'idd_update_windowed_resolution', 'idd_update_fullscreen_resolution', 'idd_update_display_size', 'idd_select_fullscreen_resolution', 'idd_fit_fullscreen', 'idd_set_fullscreen', 'compute_letterbox', 'window_to_vm_coords')) {
    $match = [regex]::Match($viewer, "(?ms)^static (?:void|BOOL) $name\([^;{]*?\)\s*\{.*?^}")
    if (!$match.Success) { throw "Viewer function $name not found; update harness extraction." }
    $match.Value
}
Set-Content (Join-Path $output 'fullscreen_viewer_functions.inc') ($functions -join "`r`n") -Encoding ascii
# Events must re-fit/reselect; frames and timer only evaluate cached targets.
$checks = @(
    'case WM_DPICHANGED:\s*if \(d && d->fullscreen\) \{\s*idd_fit_fullscreen\(d,MonitorFromRect',
    'case WM_DISPLAYCHANGE:\s*if \(d && d->fullscreen\) idd_fit_fullscreen',
    'case WM_MOVE:[\s\S]*?if \(target != d->fullscreen_monitor\) idd_fit_fullscreen\(d,target\);',
    'case WM_DEVICECHANGE:[\s\S]*?wp == DBT_DEVNODES_CHANGED[\s\S]*?idd_fit_fullscreen\(d,MonitorFromWindow',
    'if \(!d->input_sizing && d->fullscreen && _wcsicmp\(d->os_type,L"windows"\) == 0\)\s*idd_fit_fullscreen',
    'if \(d->fullscreen\) idd_update_fullscreen_resolution\(d\);\s*else idd_update_windowed_resolution\(d\);',
    'compute_letterbox\(\(UINT\)rc.right, \(UINT\)rc.bottom,\s*d->frame_width, d->frame_height',
    'compute_letterbox\(\(UINT\)rc.right, \(UINT\)rc.bottom, vm_w, vm_h'
)
foreach ($pattern in $checks) {
    if ($viewer -notmatch $pattern) { throw "Fullscreen integration missing: $pattern" }
}
if ($viewer -match 'ChangeDisplaySettings|SetDisplayConfig') { throw 'Viewer must not change host display modes.' }
if ((Get-Content (Join-Path $repo 'AppSandbox.vcxproj') -Raw) -notmatch 'ClInclude Include="src\\backend_win\\fullscreen_resolution.h"') {
    throw 'Fullscreen policy header is missing from the app project.'
}
$commands = @"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b %errorlevel%
cl /nologo /W4 /WX /TC /DUNICODE /D_UNICODE /I"$output" /Fe:fullscreen-resolution-test.exe "$repo\tools\tests\fullscreen_resolution_test.c" "$repo\src\backend_win\display_modes.c" /link user32.lib
if errorlevel 1 exit /b %errorlevel%
fullscreen-resolution-test.exe
exit /b %errorlevel%
"@
$script = Join-Path $output 'run.cmd'
Set-Content -LiteralPath $script -Value $commands -Encoding ascii
Push-Location $output
try {
    & $env:ComSpec /d /c $script
    if ($LASTEXITCODE -ne 0) { throw "Fullscreen resolution tests failed ($LASTEXITCODE)." }
} finally { Pop-Location }

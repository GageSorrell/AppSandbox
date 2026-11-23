# eh6.7 host install (run elevated): stops AppSandbox (no VM may be running), backs up the files it
# replaces as *.pre-eh67 (once), copies this folder's build into C:\Application\AppSandbox and starts
# AppSandbox again. Log: install.log beside this script.
$ErrorActionPreference = 'Stop'
$src = $PSScriptRoot
$dst = 'C:\Application\AppSandbox'
$log = Join-Path $src 'install.log'
"==== $(Get-Date -Format o)" | Set-Content $log
try {
    $p = Get-Process -Name AppSandbox -ErrorAction SilentlyContinue
    if ($p) {
        "stopping AppSandbox pid $($p.Id)" | Add-Content $log
        $p | ForEach-Object { $null = $_.CloseMainWindow() }
        if (-not $p.WaitForExit(10000)) { $p | Stop-Process -Force; Start-Sleep 2 }
    }
    Get-Process -Name AppSandbox -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep 1
    $files = @('AppSandbox.exe', 'appsandbox_core.dll', 'resources\appsandbox-input.exe', 'resources\appsandbox-remote-windows.exe')
    foreach ($f in $files) {
        $target = Join-Path $dst $f
        if ((Test-Path $target) -and -not (Test-Path "$target.pre-eh67")) { Copy-Item $target "$target.pre-eh67" }
        Copy-Item (Join-Path $src $f) $target -Force
        "installed $f $((Get-FileHash $target).Hash)" | Add-Content $log
    }
    Start-Process (Join-Path $dst 'AppSandbox.exe') -WorkingDirectory $dst
    'started AppSandbox' | Add-Content $log
    'OK' | Add-Content $log
} catch {
    "FAILED: $($_.Exception.Message)" | Add-Content $log
    exit 1
}

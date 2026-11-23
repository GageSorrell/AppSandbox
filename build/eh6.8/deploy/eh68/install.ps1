# eh6.8 guest install (run elevated): Remote Windows helper 1.3.0 (guest tray selector) and the
# agent service binary (does not restart a helper the user exited from its tray). Originals are
# kept once as *.eh67-original.
$ErrorActionPreference = 'Continue'
$log = Join-Path $PSScriptRoot 'install.log'
"==== $(Get-Date -Format o)" | Set-Content $log
$svc = Get-CimInstance Win32_Service -Filter "Name='AppSandboxAgent'"
if (-not $svc) { 'AppSandboxAgent service not found' | Tee-Object -Append $log; exit 1 }
$agent = if ($svc.PathName -match '^\s*"([^"]+)"') { $Matches[1] } else { ($svc.PathName -split '\s+')[0] }
$dir = Split-Path $agent
"service: $($svc.PathName)`ndir: $dir" | Tee-Object -Append $log
Stop-Service AppSandboxAgent -Force *>> $log      # graceful: the helper restores shared windows first
foreach ($name in 'appsandbox-remote-windows', 'appsandbox-input') {
    Stop-Process -Name $name -Force -ErrorAction SilentlyContinue
}
Start-Sleep 2
foreach ($file in 'appsandbox-remote-windows.exe', 'appsandbox-agent.exe') {
    $target = Join-Path $dir $file
    if ((Test-Path $target) -and -not (Test-Path "$target.eh67-original")) { Copy-Item $target "$target.eh67-original" }
    Copy-Item (Join-Path $PSScriptRoot $file) $target -Force
    "installed $target $((Get-FileHash $target).Hash)" | Tee-Object -Append $log
}
Start-Service AppSandboxAgent *>> $log
"service: $((Get-Service AppSandboxAgent).Status)" | Tee-Object -Append $log

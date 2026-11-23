# eh6.7 guest install (run elevated): Remote Windows helper 1.2.0 (activation for host input) and
# the input injector with virtual-desktop mouse moves (input mouse version 3). The agent service,
# display watcher and drivers are unchanged. Originals are kept once as *.eh66-original.
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
foreach ($file in 'appsandbox-remote-windows.exe', 'appsandbox-input.exe') {
    $target = Join-Path $dir $file
    if ((Test-Path $target) -and -not (Test-Path "$target.eh66-original")) { Copy-Item $target "$target.eh66-original" }
    Copy-Item (Join-Path $PSScriptRoot $file) $target -Force
    "installed $target $((Get-FileHash $target).Hash)" | Tee-Object -Append $log
}
Start-Service AppSandboxAgent *>> $log
"service: $((Get-Service AppSandboxAgent).Status)" | Tee-Object -Append $log

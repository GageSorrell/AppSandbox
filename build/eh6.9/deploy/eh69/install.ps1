# eh6.9 guest install (run elevated): Remote Windows helper 1.4.0 (two-way copy-only OLE file and
# folder drag and drop on channel 11). Only the helper changes; the original is kept once as
# *.eh68-original. Stopping the service lets the helper restore shared windows first.
$ErrorActionPreference = 'Continue'
$log = Join-Path $PSScriptRoot 'install.log'
"==== $(Get-Date -Format o)" | Set-Content $log
$svc = Get-CimInstance Win32_Service -Filter "Name='AppSandboxAgent'"
if (-not $svc) { 'AppSandboxAgent service not found' | Tee-Object -Append $log; exit 1 }
$agent = if ($svc.PathName -match '^\s*"([^"]+)"') { $Matches[1] } else { ($svc.PathName -split '\s+')[0] }
$dir = Split-Path $agent
"service: $($svc.PathName)`ndir: $dir" | Tee-Object -Append $log
Stop-Service AppSandboxAgent -Force *>> $log
Stop-Process -Name 'appsandbox-remote-windows' -Force -ErrorAction SilentlyContinue
Start-Sleep 2
$target = Join-Path $dir 'appsandbox-remote-windows.exe'
if ((Test-Path $target) -and -not (Test-Path "$target.eh68-original")) { Copy-Item $target "$target.eh68-original" }
Copy-Item (Join-Path $PSScriptRoot 'appsandbox-remote-windows.exe') $target -Force
"installed $target $((Get-FileHash $target).Hash)" | Tee-Object -Append $log
Start-Service AppSandboxAgent *>> $log
"service: $((Get-Service AppSandboxAgent).Status)" | Tee-Object -Append $log

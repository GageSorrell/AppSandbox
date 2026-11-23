# eh6.6 guest install (run elevated): this branch's Remote Windows helper (proxy support: geometry,
# close, hit testing, window state, icons). The service and display watcher are unchanged from eh6.5.
$ErrorActionPreference = 'Continue'
$log = Join-Path $PSScriptRoot 'install.log'
"==== $(Get-Date -Format o)" | Set-Content $log
$svc = Get-CimInstance Win32_Service -Filter "Name='AppSandboxAgent'"
if (-not $svc) { 'AppSandboxAgent service not found' | Tee-Object -Append $log; exit 1 }
$agent = if ($svc.PathName -match '^\s*"([^"]+)"') { $Matches[1] } else { ($svc.PathName -split '\s+')[0] }
$dir = Split-Path $agent
$target = Join-Path $dir 'appsandbox-remote-windows.exe'
"service: $($svc.PathName)`nhelper: $target" | Tee-Object -Append $log
Stop-Service AppSandboxAgent -Force *>> $log      # graceful: the helper restores shared windows first
Stop-Process -Name appsandbox-remote-windows -Force -ErrorAction SilentlyContinue
Start-Sleep 2
if ((Test-Path $target) -and -not (Test-Path "$target.eh65-original")) { Copy-Item $target "$target.eh65-original" }
Copy-Item (Join-Path $PSScriptRoot 'appsandbox-remote-windows.exe') $target -Force
"installed $target $((Get-FileHash $target).Hash)" | Tee-Object -Append $log
Start-Service AppSandboxAgent *>> $log
"service: $((Get-Service AppSandboxAgent).Status)" | Tee-Object -Append $log

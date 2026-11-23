# eh6.5 guest install (run elevated): this branch's service, display watcher and Remote Windows helper.
$ErrorActionPreference = 'Continue'
$log = Join-Path $PSScriptRoot 'install.log'
"==== $(Get-Date -Format o)" | Set-Content $log
$svc = Get-CimInstance Win32_Service -Filter "Name='AppSandboxAgent'"
if (-not $svc) { 'AppSandboxAgent service not found' | Tee-Object -Append $log; exit 1 }
$agent = if ($svc.PathName -match '^\s*"([^"]+)"') { $Matches[1] } else { ($svc.PathName -split '\s+')[0] }
$dir = Split-Path $agent
"service: $($svc.PathName)`nagent: $agent" | Tee-Object -Append $log
if (-not (Test-Path $agent)) { "agent binary not found" | Tee-Object -Append $log; exit 1 }
Stop-Service AppSandboxAgent -Force *>> $log
Stop-Process -Name appsandbox-displays, appsandbox-remote-windows -Force -ErrorAction SilentlyContinue
Start-Sleep 2
foreach ($name in 'appsandbox-agent.exe', 'appsandbox-displays.exe', 'appsandbox-remote-windows.exe') {
    $target = if ($name -eq 'appsandbox-agent.exe') { $agent } else { Join-Path $dir $name }
    if ((Test-Path $target) -and -not (Test-Path "$target.eh64-original")) { Copy-Item $target "$target.eh64-original" }
    Copy-Item (Join-Path $PSScriptRoot $name) $target -Force
    "installed $target $((Get-FileHash $target).Hash)" | Tee-Object -Append $log
}
Start-Service AppSandboxAgent *>> $log
"service: $((Get-Service AppSandboxAgent).Status)" | Tee-Object -Append $log

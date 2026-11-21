# Run elevated after the host disables Secure Boot for this VM and the guest
# has booted with Test Mode enabled. Does not modify boot security settings.
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$package = Join-Path $root 'package'
$drivers = Join-Path $package 'drivers'
$destination = Join-Path $env:SystemRoot 'AppSandbox'
Start-Transcript -Path (Join-Path $root 'install.log') -Append
try {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run this script as Administrator.' }
    $secureBoot = Get-ItemPropertyValue 'HKLM:\SYSTEM\CurrentControlSet\Control\SecureBoot\State' UEFISecureBootEnabled
    if ($secureBoot -ne 0) { throw 'Secure Boot is enabled; the local test-signed display driver cannot be loaded.' }
    $boot = & bcdedit.exe /enum
    if ($LASTEXITCODE -ne 0 -or ($boot -join [Environment]::NewLine) -notmatch '(?im)^testsigning\s+Yes\s*$') { throw 'Test signing is not enabled. Enable it separately and reboot before installing.' }
    $startOptions = Get-ItemPropertyValue 'HKLM:\SYSTEM\CurrentControlSet\Control' SystemStartOptions
    if ($startOptions -notmatch 'TESTSIGNING') { throw 'This boot has not activated Test Mode. Reboot before installing.' }
    $service = Get-CimInstance Win32_Service -Filter "Name='AppSandboxAgent'"
    if ($service.StartName -ne 'LocalSystem' -or $service.PathName.Trim('"') -ne (Join-Path $destination 'appsandbox-agent.exe')) { throw 'Unexpected agent service configuration.' }
    foreach ($entry in (Get-Content (Join-Path $root 'package-hashes.json') -Raw | ConvertFrom-Json)) {
        if ((Get-FileHash -LiteralPath (Join-Path $package $entry.Path)).Hash -ne $entry.Hash) { throw "Staged package changed: $($entry.Path)" }
    }
    $export = Join-Path $root 'backup\driver-store'
    New-Item -ItemType Directory -Path $export -Force | Out-Null
    $oldDriver = Get-CimInstance Win32_PnPSignedDriver | Where-Object { $_.DeviceName -eq 'App Sandbox Virtual Display Adapter' }
    if (@($oldDriver).Count -ne 1) { throw 'Expected exactly one existing display adapter.' }
    & pnputil.exe /export-driver $oldDriver.InfName $export
    if ($LASTEXITCODE -ne 0) { throw 'Could not export the installed display driver.' }
    $certPath = Join-Path $drivers 'AppSandboxVDD.cer'
    $cert = [Security.Cryptography.X509Certificates.X509Certificate2]::new($certPath)
    $catalog = Get-AuthenticodeSignature -LiteralPath (Join-Path $drivers 'AppSandboxVDD.cat')
    if ($catalog.SignerCertificate.Thumbprint -ne $cert.Thumbprint) { throw 'Catalog and certificate do not match.' }
    Import-Certificate -FilePath $certPath -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    Import-Certificate -FilePath $certPath -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
    if ((Get-AuthenticodeSignature -LiteralPath (Join-Path $drivers 'AppSandboxVDD.cat')).Status -ne 'Valid') { throw 'Catalog signature validation failed.' }
    & (Join-Path $drivers 'devcon.exe') update (Join-Path $drivers 'AppSandboxVDD.inf') 'ROOT\AppSandboxVDD'
    $driverExit = $LASTEXITCODE
    if ($driverExit -notin @(0,1)) { throw "Display driver update failed: $driverExit" }
    try {
        Stop-Service AppSandboxAgent -Force
        foreach ($name in @('appsandbox-input','appsandbox-displays','appsandbox-clipboard','appsandbox-clipboard-reader','appsandbox-audio')) {
            Get-Process -Name $name -ErrorAction SilentlyContinue | Stop-Process -Force
        }
        foreach ($name in @('appsandbox-agent.exe','appsandbox-input.exe','appsandbox-displays.exe','appsandbox-clipboard.exe','appsandbox-clipboard-reader.exe','appsandbox-audio.exe')) {
            Copy-Item -LiteralPath (Join-Path $package "resources\$name") -Destination (Join-Path $destination $name) -Force
        }
        foreach ($name in @('AppSandboxVDD.dll','AppSandboxVDD.inf','AppSandboxVDD.cat','AppSandboxVDD.cer')) {
            Copy-Item -LiteralPath (Join-Path $drivers $name) -Destination (Join-Path $destination "drivers\$name") -Force
        }
    } finally { Start-Service AppSandboxAgent }
    Get-CimInstance Win32_PnPSignedDriver | Where-Object { $_.DeviceName -match 'App Sandbox' } | Format-List DeviceName,InfName,DriverVersion,Signer
    Set-Content -LiteralPath (Join-Path $root 'installed.txt') -Value "Installed. devcon exit=$driverExit. Reboot and verify the active display driver and resizing."
} finally { Stop-Transcript }

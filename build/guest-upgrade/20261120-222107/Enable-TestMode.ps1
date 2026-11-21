$ErrorActionPreference = 'Stop'
Start-Transcript -Path (Join-Path $PSScriptRoot 'enable-testmode.log') -Append
try {
    if ((Get-ItemPropertyValue HKLM:\SYSTEM\CurrentControlSet\Control\SecureBoot\State UEFISecureBootEnabled) -ne 0) { throw 'Secure Boot is still enabled.' }
    $volumes = @(Get-CimInstance -Namespace root\CIMV2\Security\MicrosoftVolumeEncryption -ClassName Win32_EncryptableVolume -ErrorAction Stop)
    $systemVolume = $volumes | Where-Object { $_.DriveLetter -eq $env:SystemDrive }
    if ($systemVolume) {
        $protection = Invoke-CimMethod -InputObject $systemVolume -MethodName GetProtectionStatus
        if ($protection.ReturnValue -ne 0 -or $protection.ProtectionStatus -ne 0) { throw 'System volume encryption protection is active or unknown; check recovery access before changing boot settings.' }
    }
    & bcdedit.exe /enum
    & bcdedit.exe /set testsigning on
    if ($LASTEXITCODE -ne 0) { throw "BCDEdit failed: $LASTEXITCODE" }
    & bcdedit.exe /enum
    Set-Content -LiteralPath (Join-Path $PSScriptRoot 'testmode-enabled.txt') -Value 'TESTSIGNING enabled. Guest reboot required before installation.'
} catch {
    $_ | Out-String | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'testmode-error.txt')
    throw
} finally { Stop-Transcript }

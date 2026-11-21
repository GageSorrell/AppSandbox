<#
.SYNOPSIS
Build an extract-and-run Windows x64 development ZIP in the repository root.
.DESCRIPTION
Uses a matching upstream Windows x64 release ZIP for runtime resources, tools,
and signed guest drivers, then overlays this checkout's host binaries and web UI.
Requires the MSVC v143 C++ tools, Windows SDK, WDK with Visual Studio integration,
and the required Spectre-mitigated libraries for the default VDD build.

By default, replaces the released VDD with the local test-signed DLL/INF/CAT
and public certificate. -BuildVdd explicitly requests this default behavior.
Use -NoBuildVdd to retain the upstream VDD instead; the two flags cannot be combined.
The VAD and SHM drivers are retained from the upstream release in either case.
Create Windows guests with Test Mode enabled to use the local VDD.
Existing guests need a separate driver update. No host boot settings are changed.
The publisher's EV token and Microsoft attestation are not used.
.EXAMPLE
.\tools\MakePackageDev.ps1 -BaseZip C:\Downloads\AppSandbox-0.1.13-win-x64.zip
.EXAMPLE
.\tools\MakePackageDev.ps1 -BaseZip C:\Downloads\AppSandbox-0.1.13-win-x64.zip -BuildVdd
.EXAMPLE
.\tools\MakePackageDev.ps1 -BaseZip C:\Downloads\AppSandbox-0.1.13-win-x64.zip -NoBuildVdd
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BaseZip,
    [switch]$BuildVdd,
    [switch]$NoBuildVdd,
    [string]$MSBuildPath
)

$ErrorActionPreference = 'Stop'
if ($BuildVdd -and $NoBuildVdd) {
    throw 'Specify either -BuildVdd or -NoBuildVdd, not both.'
}
$shouldBuildVdd = $BuildVdd -or -not $NoBuildVdd
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$baseArchive = (Resolve-Path -LiteralPath $BaseZip).Path
$outputZip = Join-Path $repo 'AppSandbox-port-win-x64.zip'
if ($baseArchive -eq $outputZip) {
    throw 'BaseZip must be an upstream release ZIP, not the output ZIP.'
}

if (-not $MSBuildPath) {
    $command = Get-Command MSBuild.exe -ErrorAction SilentlyContinue
    if ($command) {
        $MSBuildPath = $command.Source
    } else {
        $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
        if (Test-Path -LiteralPath $vswhere) {
            $installation = & $vswhere -latest -products '*' `
                -requires Microsoft.Component.MSBuild -property installationPath |
                Select-Object -First 1
            if ($installation) {
                foreach ($relative in @('MSBuild\Current\Bin\amd64\MSBuild.exe', 'MSBuild\Current\Bin\MSBuild.exe')) {
                    $candidate = Join-Path $installation $relative
                    if (Test-Path -LiteralPath $candidate) {
                        $MSBuildPath = $candidate
                        break
                    }
                }
            }
        }
    }
}
if (-not $MSBuildPath -or -not (Test-Path -LiteralPath $MSBuildPath -PathType Leaf)) {
    throw 'MSBuild not found. Install the v143 C++ tools and Windows SDK, or pass -MSBuildPath.'
}

function Invoke-DevBuild([string]$Project, [switch]$HostProject) {
        $buildArguments = @(
        (Join-Path $repo $Project)
        "/t:Build"
        "/m"
        "/nologo"
        "/p:Configuration=Release"
        "/p:Platform=x64"
        "/p:WindowsTargetPlatformVersion=10.0.26100.0"
    )
    if (-not $HostProject)
{
    $VddDirectory = (Join-Path $repo "tools\vdd") + "\"

    $buildArguments += "/p:SolutionDir=$VddDirectory"

    # Package driver files ourselves instead of relying
    # on the project's legacy post-build copy commands.
    $buildArguments += "/p:PostBuildEventUseInBuild=false"
}
    if ($HostProject) {
        # Stage release resources and the web UI ourselves, rather than using the
        # project's legacy release\resources post-build copy commands.
        $buildArguments += '/p:PostBuildEventUseInBuild=false'
    }
    & $MSBuildPath @buildArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed: $Project (exit $LASTEXITCODE)."
    }
}


function New-TestSignedVddPackage
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$BuildDirectory,

        [Parameter(Mandatory = $true)]
        [string]$OutputDirectory
    )

    $SdkVersion = "10.0.26100.0"
    $SdkBinDirectory = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin\$SdkVersion"

    $Inf2CatPath = Join-Path $SdkBinDirectory "x86\Inf2Cat.exe"
    $SignToolPath = Join-Path $SdkBinDirectory "x64\signtool.exe"

    foreach ($ToolPath in @($Inf2CatPath, $SignToolPath))
    {
        if (-not (Test-Path -LiteralPath $ToolPath -PathType Leaf))
        {
            throw "Required WDK tool not found: $ToolPath"
        }
    }

    # Stage only the files belonging to this driver package.
    New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

    foreach ($FileName in @("AppSandboxVDD.dll", "AppSandboxVDD.inf"))
    {
        $SourcePath = Join-Path $BuildDirectory $FileName

        if (-not (Test-Path -LiteralPath $SourcePath -PathType Leaf))
        {
            throw "Required VDD build output is missing: $SourcePath"
        }

        Copy-Item -LiteralPath $SourcePath -Destination $OutputDirectory -Force
    }

    $CatalogPath = Join-Path $OutputDirectory "AppSandboxVDD.cat"
    $CertificatePath = Join-Path $OutputDirectory "AppSandboxVDD.cer"

    # Generate the catalog from the final DLL and stamped INF.
    # These identifiers cover Windows 11 x64 releases.
    & $Inf2CatPath `
        "/driver:$OutputDirectory" `
        "/os:10_CO_X64,10_NI_X64,10_GE_X64" `
        "/uselocaltime" `
        "/verbose"

    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $CatalogPath))
    {
        throw "Inf2Cat failed to generate AppSandboxVDD.cat."
    }

    # Reuse a development certificate from the current user's
    # Personal certificate store, creating one if necessary.
    $CertificateSubject = "CN=AppSandbox VDD Development Test"

    $Certificate = Get-ChildItem "Cert:\CurrentUser\My" -CodeSigningCert |
        Where-Object {
            $_.Subject -eq $CertificateSubject -and
            $_.HasPrivateKey -and
            $_.NotBefore -le (Get-Date) -and
            $_.NotAfter -gt (Get-Date).AddDays(30)
        } |
        Sort-Object NotAfter -Descending |
        Select-Object -First 1

    if (-not $Certificate)
    {
        Write-Host "Creating a development code-signing certificate..."

        $Certificate = New-SelfSignedCertificate `
            -Type CodeSigningCert `
            -Subject $CertificateSubject `
            -CertStoreLocation "Cert:\CurrentUser\My" `
            -KeyAlgorithm RSA `
            -KeyLength 3072 `
            -HashAlgorithm SHA256 `
            -KeyExportPolicy NonExportable `
            -NotAfter (Get-Date).AddYears(2)
    }

    # Sign the catalog with the certificate's private key.
    & $SignToolPath sign `
        /v `
        /s My `
        /sha1 $Certificate.Thumbprint `
        /fd SHA256 `
        $CatalogPath

    if ($LASTEXITCODE -ne 0)
    {
        throw "SignTool failed to sign AppSandboxVDD.cat."
    }

    # Confirm the catalog contains the expected signer's certificate.
    $Signature = Get-AuthenticodeSignature -LiteralPath $CatalogPath

    if (
        -not $Signature.SignerCertificate -or
        $Signature.SignerCertificate.Thumbprint -ne $Certificate.Thumbprint
    )
    {
        throw "The catalog does not contain the expected signing certificate."
    }

    # Export only the public certificate; never export the private key.
    Export-Certificate `
        -Cert $Certificate `
        -FilePath $CertificatePath `
        -Force | Out-Null

    Write-Host "VDD catalog signed with certificate $($Certificate.Thumbprint)"
    Write-Host "Signed driver package: $OutputDirectory"
}


# A fresh staging directory prevents old files from leaking into subsequent ZIPs.
$work = Join-Path $repo ('obj\package-dev\' + [guid]::NewGuid().ToString('N'))
$stage = Join-Path $work 'AppSandbox'
New-Item -ItemType Directory -Path $stage -Force | Out-Null
$work = (Resolve-Path -LiteralPath $work).Path
$stagingRoot = [IO.Path]::GetFullPath((Join-Path $repo 'obj\package-dev')) + '\'
if (-not $work.StartsWith($stagingRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Staging path is outside the intended directory: $work"
}
try {
    Expand-Archive -LiteralPath $baseArchive -DestinationPath $stage
    $required = @(
        'AppSandbox.exe', 'appsandbox_core.dll', 'iso-patch.exe',
        'WebView2Loader.dll', 'appsandbox-cuda-updater.exe',
        'resources', 'web\index.html', 'drivers\devcon.exe',
        'drivers\AppSandboxVDD.dll', 'drivers\AppSandboxVDD.inf', 'drivers\AppSandboxVDD.cat',
        'drivers\AppSandboxVAD.sys', 'drivers\AppSandboxVAD.inf', 'drivers\AppSandboxVAD.cat',
        'drivers\AppSandboxSHM.sys', 'drivers\AppSandboxSHM.inf', 'drivers\AppSandboxSHM.cat'
    )
    foreach ($relative in $required) {
        if (-not (Test-Path -LiteralPath (Join-Path $stage $relative))) {
            throw "BaseZip is missing '$relative'. Supply a complete matching Windows x64 release ZIP with AppSandbox.exe at its root."
        }
    }

    Invoke-DevBuild 'AppSandboxCore.vcxproj' -HostProject
    Invoke-DevBuild 'AppSandbox.vcxproj' -HostProject
    foreach ($name in @('AppSandbox.exe', 'appsandbox_core.dll')) {
        Copy-Item -LiteralPath (Join-Path $repo "bin\Release\$name") -Destination $stage -Force
    }
    # Replace the UI folder so removed upstream assets cannot remain in the bundle.
    Remove-Item -LiteralPath (Join-Path $stage 'web') -Recurse -Force
    Copy-Item -LiteralPath (Join-Path $repo 'web') -Destination $stage -Recurse

    # if ($shouldBuildVdd) {
    #     Invoke-DevBuild 'tools\vdd\Package\Package.vcxproj'
    #     # $driverDirectory = Join-Path $repo 'bin\Release\drivers'
    #     $driverDirectory = Join-Path $repo "tools\vdd\x64\Release\AppSandboxVDD"
    #     $catalog = Join-Path $driverDirectory 'AppSandboxVDD.cat'
    #     $signature = Get-AuthenticodeSignature -LiteralPath $catalog
    #     if (-not $signature.SignerCertificate) {
    #         throw 'The local VDD catalog has no signing certificate. Configure WDK test signing before packaging.'
    #     }
    #     foreach ($name in @('AppSandboxVDD.dll', 'AppSandboxVDD.inf', 'AppSandboxVDD.cat')) {
    #         Copy-Item -LiteralPath (Join-Path $driverDirectory $name) `
    #             -Destination (Join-Path $stage "drivers\$name") -Force
    #     }
    #     # Export only the public certificate, directly from this catalog's signer.
    #     [IO.File]::WriteAllBytes((Join-Path $stage 'drivers\AppSandboxVDD.cer'), $signature.SignerCertificate.RawData)
    #     Write-Host 'Including local test-signed VDD. Enable Test Mode when creating Windows guests.'
    # } else {
    #     Write-Host 'Keeping upstream guest drivers because -NoBuildVdd was specified; local VDD changes are not included.'
    # }

if ($shouldBuildVdd)
{
    Invoke-DevBuild "tools\vdd\Package\Package.vcxproj"

    $BuildDirectory = Join-Path $repo "tools\vdd\x64\Release\AppSandboxVDD"
    $SigningDirectory = Join-Path $work "VddSigning"

    New-TestSignedVddPackage `
        -BuildDirectory $BuildDirectory `
        -OutputDirectory $SigningDirectory

    foreach ($FileName in @(
        "AppSandboxVDD.dll",
        "AppSandboxVDD.inf",
        "AppSandboxVDD.cat",
        "AppSandboxVDD.cer"
    ))
    {
        Copy-Item `
            -LiteralPath (Join-Path $SigningDirectory $FileName) `
            -Destination (Join-Path $stage "drivers\$FileName") `
            -Force
    }

    Write-Host "Including locally test-signed VDD."
}
else
{
    Write-Host "Keeping upstream guest drivers because -NoBuildVdd was specified."
}


    $temporaryZip = Join-Path $work 'package.zip'
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $temporaryZip
    Move-Item -LiteralPath $temporaryZip -Destination $outputZip -Force
    Write-Host "Development ZIP: $outputZip"
} finally {
    # Delete only the unique staging directory created by this invocation.
    Remove-Item -LiteralPath $work -Recurse -Force
}

param(
    [string]$VisualStudioPath,
    [string]$WdkRoot,
    [string]$LogPath
)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (!$VisualStudioPath) {
    $vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $VisualStudioPath=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (!$VisualStudioPath) { throw 'MSVC build tools are required.' }
$msbuild=Join-Path $VisualStudioPath 'MSBuild/Current/Bin/MSBuild.exe'
if (!$LogPath) { $LogPath=Join-Path $repo 'build/display-validation/vdd-build.log' }
New-Item -ItemType Directory -Force (Split-Path $LogPath) | Out-Null
$buildArgs=@((Join-Path $repo 'tools/vdd/AppSandboxVDD.vcxproj'),'/t:Rebuild','/p:Configuration=Release','/p:Platform=x64',
    '/p:SignMode=Off','/p:EnableInf2cat=false','/p:EnableDrvCat=true','/v:minimal','/nologo')
if ($WdkRoot) {
    # Reuse explicitly staged SDK/WDK content; this script never downloads or installs it.
    $WdkRoot=(Resolve-Path -LiteralPath $WdkRoot).Path
    $content=Join-Path $WdkRoot 'package/c'
    $targets=Join-Path $WdkRoot 'vc-targets'
    if (!(Test-Path "$targets/Platforms/x64/PlatformToolsets/WindowsUserModeDriver10.0/Toolset.props")) {
        throw "Missing staged WDK build integration under $targets."
    }
    $version=Get-ChildItem "$content/Include" -Directory | Where-Object Name -Match '^10\.0\.\d+\.0$' | Sort-Object {[version]$_.Name} -Descending | Select-Object -First 1 -ExpandProperty Name
    if (!$version -or !(Test-Path "$content/Include/$version/um/iddcx/1.10/IddCx.h")) {
        throw 'The staged WDK lacks the required IddCx headers.'
    }
    $content=$content.Replace('/','\')+'\'
    $targets=$targets.Replace('/','\')+'\'
    $tools="${content}bin\$version\x64\"
    $buildArgs+=@("/p:VCTargetsPath=$targets","/p:WDKContentRoot=$content","/p:WindowsSdkDir=$content",
        "/p:WDKBuildFolder=$version","/p:WindowsTargetPlatformVersion=$version",'/p:WDK_NuGet=true',
        "/p:WDKBinRoot_x86=$tools",'/p:PreferredToolArchitecture=x64','/p:RunCodeAnalysis=false',
        "/p:UCRTContentRoot=$content","/p:UniversalCRTSdkDir=$content","/p:UCRTVersion=$version",
        "/p:WindowsSDKVersionedBinRoot=${content}bin\$version\","/p:InfToolPath=$tools","/p:TraceWppToolPath=$tools",
        '/p:Driver_SpectreMitigation=false')
}
Push-Location $repo
try {
    & $msbuild @buildArgs *> $LogPath
    $result=$LASTEXITCODE
    Get-Content -LiteralPath $LogPath -Tail 25
    if ($result -ne 0) { throw "Unsigned VDD build failed ($result); see $LogPath." }
    Write-Output "PASS unsigned Release x64 VDD build. Log: $LogPath"
} finally { Pop-Location }

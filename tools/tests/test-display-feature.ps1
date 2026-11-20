param([string]$VisualStudioPath,[string]$WdkRoot)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$output=Join-Path $repo 'build/display-validation'
New-Item -ItemType Directory -Force $output | Out-Null
if (!$VisualStudioPath) {
    $vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $VisualStudioPath=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (!$VisualStudioPath) { throw 'MSVC build tools are required.' }
$msbuild=Join-Path $VisualStudioPath 'MSBuild/Current/Bin/MSBuild.exe'
if (!$WdkRoot -and (Test-Path "$repo/bin/wdk/vc-targets")) { $WdkRoot="$repo/bin/wdk" }
$results=[System.Collections.Generic.List[object]]::new()
function Invoke-Check([string]$Name,[string]$Command,[scriptblock]$Action) {
    $watch=[Diagnostics.Stopwatch]::StartNew()
    $log=Join-Path $output "$Name.log"
    $status='passed';$failure=$null
    Write-Host "Checking $Name..."
    try { & $Action 2>&1 | Tee-Object -FilePath $log | Out-Host }
    catch { $status='failed';$failure=$_.Exception.Message;Add-Content $log $failure;Write-Warning "$Name failed: $failure" }
    $results.Add([pscustomobject]@{name=$Name;status=$status;command=$Command;seconds=[math]::Round($watch.Elapsed.TotalSeconds,2);log=$log;failure=$failure})
    [pscustomobject]@{completedUtc=[DateTime]::UtcNow.ToString('o');visualStudio=$VisualStudioPath;wdkRoot=$WdkRoot;
        checks=$results.ToArray();nativeAcceptance='not run; AppSandbox-gfa.10';signedReleasePackage='not produced'} |
        ConvertTo-Json -Depth 6 | Set-Content (Join-Path $output 'results.json')
}
function Invoke-Node([string[]]$NodeArguments) {
    & node @NodeArguments
    if ($LASTEXITCODE -ne 0) { throw "node failed ($LASTEXITCODE): $NodeArguments" }
}
Push-Location $repo
try {
    Invoke-Check 'source-registration' 'node tools/tests/display_registration_test.cjs' { Invoke-Node @('tools/tests/display_registration_test.cjs') }
    foreach ($test in @('display-settings','display-modes','display-resolution-command','display-resize','windowed-resolution','fullscreen-controls','fullscreen-entry','display-integration')) {
        $script="tools/tests/test-$test.ps1"
        Invoke-Check $test ".\$script -VisualStudioPath `"$VisualStudioPath`"" { & "$repo/$script" -VisualStudioPath $VisualStudioPath }
    }
    foreach ($test in @('display_modes_bridge','display_resolution_editor','display_resolution_browser','fullscreen_controls_browser')) {
        $script="tools/tests/${test}_test.cjs"
        Invoke-Check $test "node $script" { Invoke-Node @($script) }
    }
    Invoke-Check 'javascript-syntax' 'node --check web/app.js; node --check web/display-resolution.js' {
        Invoke-Node @('--check','web/app.js');Invoke-Node @('--check','web/display-resolution.js')
    }
    foreach ($project in @('AppSandboxCore','AppSandbox')) {
        $buildArgs=@("$project.vcxproj",'/t:Rebuild','/p:Configuration=Release','/p:Platform=x64','/p:PlatformToolset=v145','/v:minimal','/nologo')
        Invoke-Check "$project-build" "MSBuild $($buildArgs -join ' ')" {
            & $msbuild @buildArgs
            if ($LASTEXITCODE -ne 0) { throw "$project build failed ($LASTEXITCODE)." }
        }
    }
    Invoke-Check 'vdd-build' ".\tools\tests\build-display-driver.ps1 -WdkRoot `"$WdkRoot`"" {
        & "$PSScriptRoot/build-display-driver.ps1" -VisualStudioPath $VisualStudioPath -WdkRoot $WdkRoot -LogPath "$output/vdd-msbuild.log"
    }
    Invoke-Check 'diff-check' 'git diff --check' {
        & git diff --check
        if ($LASTEXITCODE -ne 0) { throw 'Whitespace validation failed.' }
    }
    $failed=@($results | Where-Object status -eq 'failed')
    $results | Format-Table name,status,seconds -AutoSize | Out-Host
    if ($failed.Count) { throw "$($failed.Count) display validation checks failed. See $output/results.json." }
    Write-Output "PASS all $($results.Count) automated/build checks. Native acceptance remains AppSandbox-gfa.10. Results: $output/results.json"
} finally { Pop-Location }

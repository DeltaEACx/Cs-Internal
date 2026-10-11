param([switch]$Package, [switch]$Test)
$ErrorActionPreference='Stop'
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if(!(Test-Path $vswhere)) {throw 'Install Visual Studio 2022 or newer with Desktop development with C++ and a Windows SDK.'}
$msbuild=& $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if(!$msbuild) {throw 'MSBuild / C++ x64 tools were not found.'}
# The IDE keeps its usual obj folder. Script builds use their own compiler PDBs,
# objects and tracking logs so an IDE build cannot contend with this build.
$cliIntermediateRoot = (Join-Path $PSScriptRoot 'obj\command-line').Replace('\','/') + '/'
$cliBuildProperties = @(
    ('/p:CommandLineIntermediateRoot=' + $cliIntermediateRoot),
    ('/p:MainObjectDir=' + $cliIntermediateRoot + 'x64/Release/Cs2 Internal/')
)
& $msbuild (Join-Path $PSScriptRoot 'Cs2 Internal.vcxproj') @cliBuildProperties /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
if($LASTEXITCODE -ne 0) {throw 'C++ DLL build failed.'}
if($Test) {
    foreach($testName in @('cosmetic_state_regression','visual_enhancements','visual_bones_regression','particle_chams_regression','autowall_probe_regression','spread_prediction_regression','surface_square_regression','penetration_surface_regression','penetration_overlay_regression','hvh_math_regression','hvh_hitboxes_regression','hvh_command_regression','hvh_scan_regression','hvh_scan_native_regression','backtrack_regression','combat_context_regression','movement_regression','movement_native_regression','catalog_regression','runtime_paths_regression','view_settings_regression','native_startup_regression','memory_cache_regression')) {
        & $msbuild (Join-Path $PSScriptRoot ('tests\'+$testName+'.vcxproj')) @cliBuildProperties /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
        if($LASTEXITCODE -ne 0) {throw ($testName+' build failed.')}
        & (Join-Path $PSScriptRoot ('bin\x64\Release\tests\'+$testName+'.exe'))
        if($LASTEXITCODE -ne 0) {throw ($testName+' failed.')}
    }
}
if($Package) {
    $packageId=(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8)
    & $msbuild (Join-Path $PSScriptRoot 'tools\assets\asset_tool.vcxproj') @cliBuildProperties /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
    if($LASTEXITCODE -ne 0) {throw 'Native asset tool build failed.'}
    $packageFolder=Join-Path $PSScriptRoot ('dist\staging-'+$packageId+'\DeltaEAC')
    & (Join-Path $PSScriptRoot 'bin\x64\Release\tools\AssetTool.exe') --package $PSScriptRoot $packageFolder (Join-Path $PSScriptRoot 'DLL\DeltaEAC.dll')
    if($LASTEXITCODE -ne 0) {throw 'Portable package failed.'}
    $zip=Join-Path $PSScriptRoot ('dist\DeltaEAC-'+$packageId+'.zip')
    Compress-Archive -LiteralPath $packageFolder -DestinationPath $zip -CompressionLevel Optimal
    Write-Host "Portable release: $zip"
}

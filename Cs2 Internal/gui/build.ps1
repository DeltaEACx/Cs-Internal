param([switch]$SkipTests, [switch]$Standalone)

$ErrorActionPreference = 'Stop'
$guiRoot = $PSScriptRoot
$projectRoot = Split-Path -Parent $guiRoot
$outputDirectory = Join-Path $projectRoot 'dist\DeltaEAC'

function Invoke-BuildCommand {
    param([string]$Program, [string[]]$BuildArguments)
    # Windows PowerShell wraps native stderr in ErrorRecord objects when the
    # caller redirects a build log. Compiler progress on stderr is not failure.
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $Program @BuildArguments 2>&1 | ForEach-Object { Write-Output $_.ToString() }
        $nativeExitCode = $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $previousPreference }
    if ($nativeExitCode -ne 0) { throw "$Program exited with code $nativeExitCode." }
}

Push-Location -LiteralPath $guiRoot
try {
    if (-not $Standalone) {
        # Build a reviewable DLL beside the installed one; applying it is separate.
        $runEmbeddedTests = if ($SkipTests) { 'false' } else { 'true' }
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio with the C++ build tools is required.' }
        $msbuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
        if (-not $msbuild) { throw 'MSBuild and the x64 C++ build tools were not found.' }
        Invoke-BuildCommand -Program $msbuild -BuildArguments @(
            (Join-Path $projectRoot 'Cs2 Internal.vcxproj'), '/p:Configuration=Release', '/p:Platform=x64',
            '/p:TargetName=DeltaEAC Updated', "/p:VeloraEmbeddedRunTests=$runEmbeddedTests", '/v:minimal', '/nologo')
        $destination = Join-Path $projectRoot 'DLL\DeltaEAC Updated.dll'
        if (-not (Test-Path -LiteralPath $destination)) { throw 'The embedded GUI DLL was not produced.' }
        Write-Host "Ready: $destination"
        Write-Host 'Load using Standard (LoadLibrary) mode. Press Insert in-game to open the menu.'
        return
    }
    # Optional standalone companion retained for explicit developer use only.
    if (-not (Test-Path -LiteralPath 'node_modules\@tauri-apps\cli')) {
        Invoke-BuildCommand -Program 'npm.cmd' -BuildArguments @('ci', '--no-audit', '--no-fund')
    }
    if (-not $SkipTests) {
        Invoke-BuildCommand -Program 'cargo' -BuildArguments @('test', '--locked', '--manifest-path', 'src-tauri/Cargo.toml', '--lib')
    }
    Invoke-BuildCommand -Program 'npm.cmd' -BuildArguments @('run', 'desktop:build', '--', '--ci')
    $binary = Join-Path $guiRoot 'src-tauri\target\release\velora-gui.exe'
    if (-not (Test-Path -LiteralPath $binary)) { throw 'The compiled companion executable was not found.' }
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
    $destination = Join-Path $outputDirectory 'DeltaEAC.exe'
    Copy-Item -LiteralPath $binary -Destination $destination -Force
    if ((Get-FileHash -LiteralPath $binary).Hash -ne (Get-FileHash -LiteralPath $destination).Hash) {
        throw 'The packaged executable did not pass its copy verification.'
    }
    Write-Host "Ready: $destination"
}
finally {
    Pop-Location
}

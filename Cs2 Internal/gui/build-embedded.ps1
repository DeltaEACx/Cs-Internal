param([switch]$RunTests, [switch]$BridgeTest)

$ErrorActionPreference = 'Stop'
$guiRoot = $PSScriptRoot
$targetRoot = Join-Path $guiRoot 'src-tauri\target-embedded'
$triple = 'x86_64-pc-windows-msvc'
$archive = Join-Path $targetRoot "$triple\release\velora_gui_lib.lib"
$nativeResponse = Join-Path $targetRoot 'native-static-libs.rsp'
$nativeManifest = Join-Path $targetRoot 'native-static-libs.json'
$frontendStamp = Join-Path $targetRoot 'frontend-build.json'
$rustFlagsBefore = $env:RUSTFLAGS
$encodedFlagsBefore = $env:CARGO_ENCODED_RUSTFLAGS

function Invoke-Checked {
    param([string]$Program, [string[]]$Arguments)
    $preferenceBefore = $ErrorActionPreference
    $lines = [Collections.Generic.List[string]]::new()
    try {
        $ErrorActionPreference = 'Continue'
        & $Program @Arguments 2>&1 | ForEach-Object {
            $line = $_.ToString()
            $lines.Add($line)
            Write-Host $line
        }
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $preferenceBefore }
    if ($code -ne 0) { throw "$Program exited with code $code." }
    return $lines
}
function Save-TextIfChanged {
    param([string]$Path, [string]$Text)
    if ((Test-Path -LiteralPath $Path) -and [IO.File]::ReadAllText($Path) -eq $Text) { return }
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}
function File-Records {
    param([string[]]$Files)
    foreach ($file in ($Files | Sort-Object -Unique)) {
        $item = Get-Item -LiteralPath $file
        [ordered]@{ path = $item.FullName; sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash }
    }
}

New-Item -ItemType Directory -Path $targetRoot -Force | Out-Null
Push-Location -LiteralPath $guiRoot
try {
    # The host DLL uses /MT. Keep the Rust CRT and WebView2 static loader in the
    # same runtime family; --target keeps this flag away from host proc macros.
    $env:RUSTFLAGS = '-C target-feature=+crt-static'
    $env:CARGO_ENCODED_RUSTFLAGS = $null
    $frontendFiles = @('index.html', 'package.json', 'package-lock.json')
    $frontendFiles += @(Get-ChildItem -LiteralPath $guiRoot -File -Filter 'vite.config.*' | Select-Object -ExpandProperty FullName)
    foreach ($folder in @('src', 'public')) {
        if (Test-Path -LiteralPath $folder) {
            $frontendFiles += @(Get-ChildItem -LiteralPath $folder -File -Recurse | Select-Object -ExpandProperty FullName)
        }
    }
    $inputs = @(File-Records -Files $frontendFiles)
    $previous = $null
    if (Test-Path -LiteralPath $frontendStamp) {
        try { $previous = Get-Content -LiteralPath $frontendStamp -Raw | ConvertFrom-Json } catch { $previous = $null }
    }
    $lockHash = (Get-FileHash -LiteralPath 'package-lock.json' -Algorithm SHA256).Hash
    if (-not (Test-Path -LiteralPath 'node_modules\vite\package.json') -or
        ($previous -and $previous.lockHash -ne $lockHash)) {
        Invoke-Checked 'npm.cmd' @('ci', '--no-audit', '--no-fund') | Out-Null
    }
    $frontendFresh = $previous -and
        (($inputs | ConvertTo-Json -Depth 4 -Compress) -eq ($previous.inputs | ConvertTo-Json -Depth 4 -Compress))
    if ($frontendFresh) {
        foreach ($output in $previous.outputs) {
            if (-not (Test-Path -LiteralPath $output.path) -or
                (Get-FileHash -LiteralPath $output.path -Algorithm SHA256).Hash -ne $output.sha256) {
                $frontendFresh = $false; break
            }
        }
        if (-not (Test-Path -LiteralPath 'dist\index.html')) { $frontendFresh = $false }
    }
    if (-not $frontendFresh) {
        Invoke-Checked 'npm.cmd' @('run', 'build') | Out-Null
        $outputs = @(File-Records -Files @(Get-ChildItem -LiteralPath 'dist' -Recurse -File | Select-Object -ExpandProperty FullName))
        Save-TextIfChanged $frontendStamp ([ordered]@{ lockHash=$lockHash; inputs=$inputs; outputs=$outputs } | ConvertTo-Json -Depth 5)
    }
    $features = if ($BridgeTest) { 'embedded,custom-protocol,bridge-test' } else { 'embedded,custom-protocol' }
    $common = @('--release', '--locked', '--lib', '--no-default-features', '--features', $features,
        '--target', $triple, '--target-dir', $targetRoot, '--manifest-path', 'src-tauri/Cargo.toml', '--color', 'never')
    if ($RunTests) { Invoke-Checked 'cargo' (@('test') + $common) | Out-Null }
    $buildOutput = @(Invoke-Checked 'cargo' (@('rustc', '--crate-type', 'staticlib') + $common + @('--', '--print', 'native-static-libs')))
    if (-not (Test-Path -LiteralPath $archive)) { throw 'The embedded Tauri static library was not produced.' }
    $archiveHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
    $libraries = @()
    foreach ($line in $buildOutput) {
        if ($line -match 'native-static-libs:\s*(.+)$') { $libraries = @($Matches[1] -split '\s+' | Where-Object { $_ }) }
    }
    if ($libraries.Count -eq 0) {
        if (-not (Test-Path -LiteralPath $nativeManifest)) { throw 'Rust did not report its native static-library dependencies.' }
        $native = Get-Content -LiteralPath $nativeManifest -Raw | ConvertFrom-Json
        if ($native.archiveHash -ne $archiveHash) { throw 'Native dependency metadata does not match the embedded library.' }
        $libraries = @($native.libraries)
    }
    $libraries = @($libraries | ForEach-Object {
        if ($_ -match '^/defaultlib:libcmt$') {
            'libcmt.lib'
        } else { $_ }
    })
    foreach ($library in $libraries) {
        if ($library -notmatch '^[A-Za-z0-9_.-]+\.lib$') { throw "Unexpected native library dependency: $library" }
    }
    # MSVC does not expand an @response reference nested inside MSBuild's own
    # response file. Keep its explicit library list checked against rustc.
    [xml]$linkProperties = Get-Content -LiteralPath (Join-Path (Split-Path -Parent $guiRoot) 'build\embedded_gui.props') -Raw
    $declaredLibraries = @($linkProperties.Project.PropertyGroup.VeloraEmbeddedNativeLibraries -split ';' | Where-Object { $_ })
    if (($libraries -join ';') -ne ($declaredLibraries -join ';')) {
        throw 'Rust native dependencies changed. Update VeloraEmbeddedNativeLibraries in build/embedded_gui.props to match the compiler report.'
    }
    Save-TextIfChanged $nativeResponse (($libraries | ForEach-Object { '"' + $_ + '"' }) -join [Environment]::NewLine)
    Save-TextIfChanged $nativeManifest ([ordered]@{ target=$triple; crt='static'; features=$features; archiveHash=$archiveHash; libraries=$libraries } | ConvertTo-Json -Depth 4)
    Write-Host "Embedded GUI ready: $archive"
} finally {
    $env:RUSTFLAGS = $rustFlagsBefore
    $env:CARGO_ENCODED_RUSTFLAGS = $encodedFlagsBefore
    Pop-Location
}

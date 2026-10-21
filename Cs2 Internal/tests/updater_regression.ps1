$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\tools\updater\updater_core.ps1')
$checks=0
function Assert-Equal($Actual,$Expected,[string]$Name) {
    if ($Actual -cne $Expected) {throw ($Name+': expected '+$Expected+', got '+$Actual)}
    $script:checks++
}
function Assert-Rejected([scriptblock]$Action,[string]$Name) {
    $rejected=$false
    try { & $Action | Out-Null } catch { $rejected=$true }
    if (!$rejected) {throw ($Name+' was accepted')}
    $script:checks++
}
Assert-Equal (Get-DumpSchemaOffset 844 'health') 844 'cs2-dumper schema'
Assert-Equal (Get-DumpSchemaOffset ([pscustomobject]@{offset=844;type='int32'}) 'health') 844 'ExitScam schema'
Assert-Equal (Get-DumpSchemaOffset 0 'zero') 0 'zero offset'
Assert-Equal (Format-CppRvaPair 0x5E5C90 0x5E5D70) '{0x5E5C90, 0x5E5D70}' 'C++ vtable initializer'
foreach ($invalid in @($null,-1,1048577,1.5,'bad',([pscustomobject]@{type='int32'}))) {
    Assert-Rejected {Get-DumpSchemaOffset $invalid 'invalid'} 'invalid schema offset'
}
$header='namespace cs2_dumper { namespace offsets { namespace client_dll { constexpr std::ptrdiff_t dwGameRules = 0x1; } } }'
$modules=[pscustomobject]@{'client.dll'=[pscustomobject]@{dwGameRules=0x25651D0;unwantedHeapInterface=0xFFFFFFFF}}
Assert-Equal (Get-UpdatedDumpHeader $header $modules) ($header.Replace('0x1','0x25651D0')) 'curated header import'
Assert-Rejected {Get-UpdatedDumpHeader $header ([pscustomobject]@{})} 'missing header constant'
$folder=Join-Path ([IO.Path]::GetTempPath()) ('cs2-updater-test-'+[guid]::NewGuid().ToString('N'))
try {
    [IO.Directory]::CreateDirectory((Join-Path $folder 'csgo')) | Out-Null
    $steamInfo=Join-Path $folder 'csgo\steam.inf'
    [IO.File]::WriteAllText($steamInfo,"PatchVersion=1.41.9.1`r`n")
    $engine=[pscustomobject]@{initial=[uint32]::MaxValue}
    $engine | Add-Member ScriptMethod Contains {param($Rva,$Executable) $Rva -eq 0x1234 -and !$Executable}
    $engine | Add-Member ScriptMethod U32 {param($Rva) $this.initial}
    $offsets=[pscustomobject]@{'engine2.dll'=[pscustomobject]@{dwBuildNumber=0x1234}}
    Assert-DumpBuildIdentity $engine $offsets ([pscustomobject]@{build_number=14191}) $folder
    $checks++
    Assert-Rejected {Assert-DumpBuildIdentity $engine $offsets ([pscustomobject]@{build_number=14190}) $folder} 'stale dump'
    $engine.initial=0
    Assert-DumpBuildIdentity $engine $offsets ([pscustomobject]@{build_number=14191}) $folder
    $checks++
    $engine.initial=14191
    Assert-DumpBuildIdentity $engine $offsets ([pscustomobject]@{build_number=14191}) $folder
    $checks++
    Assert-Rejected {Assert-DumpBuildIdentity $engine $offsets ([pscustomobject]@{build_number=14190}) $folder} 'static build mismatch'
    $offsets.'engine2.dll'.dwBuildNumber=0x9999
    Assert-Rejected {Assert-DumpBuildIdentity $engine $offsets ([pscustomobject]@{build_number=14191}) $folder} 'invalid build storage'
    # Minimal PE64 with a singleton factory, for omitted-interface recovery.
    $binary=[byte[]]::new(0x400)
    function Put32([int]$At,[uint32]$Value) {[BitConverter]::GetBytes($Value).CopyTo($binary,$At)}
    $binary[0]=0x4D;$binary[1]=0x5A;Put32 0x3C 0x80;Put32 0x80 0x4550
    $binary[0x86]=1;$binary[0x94]=0xF0;$binary[0x98]=0x0B;$binary[0x99]=2
    [BitConverter]::GetBytes([uint64]0x180000000).CopyTo($binary,0xB0)
    Put32 0xD0 0x2000
    [Text.Encoding]::ASCII.GetBytes('.text').CopyTo($binary,0x188)
    Put32 0x190 0x200;Put32 0x194 0x1000;Put32 0x198 0x200;Put32 0x19C 0x200;Put32 0x1AC 0x60000020
    ([byte[]]@(0x48,0x8D,0x05,0xF9,0,0,0,0xC3)).CopyTo($binary,0x200)
    $fixture=Join-Path $folder 'fixture.dll';[IO.File]::WriteAllBytes($fixture,$binary)
    # Validate a factory from disk without loading executable code.
    $fixtureRoot=Join-Path $folder 'bin\win64';[IO.Directory]::CreateDirectory($fixtureRoot) | Out-Null
    Move-Item -LiteralPath $fixture -Destination (Join-Path $fixtureRoot 'fixture.dll')
    $proof=[pscustomobject]@{module='fixture.dll';name='Fixture001';module_sha256=(Get-FileHash -LiteralPath (Join-Path $fixtureRoot 'fixture.dll')).Hash.ToLower();factory_rva=0x1000;offset=0x1100}
    $interfaces=[pscustomobject]@{}
    Add-VerifiedInterfaceFallbacks $interfaces @($proof) $folder
    Assert-Equal $interfaces.'fixture.dll'.Fixture001 0x1100 'verified interface fallback'
    $proof.module_sha256='wrong'
    Assert-Rejected {Add-VerifiedInterfaceFallbacks ([pscustomobject]@{}) @($proof) $folder} 'changed interface module'
} finally {
    # The test owns these two exact paths; no recursive deletion is needed.
    if (Test-Path -LiteralPath (Join-Path $folder 'csgo\steam.inf')) {Remove-Item -LiteralPath (Join-Path $folder 'csgo\steam.inf')}
    if (Test-Path -LiteralPath (Join-Path $folder 'csgo')) {Remove-Item -LiteralPath (Join-Path $folder 'csgo')}
    if (Test-Path -LiteralPath (Join-Path $folder 'bin\win64\fixture.dll')) {Remove-Item -LiteralPath (Join-Path $folder 'bin\win64\fixture.dll')}
    if (Test-Path -LiteralPath (Join-Path $folder 'bin\win64')) {Remove-Item -LiteralPath (Join-Path $folder 'bin\win64')}
    if (Test-Path -LiteralPath (Join-Path $folder 'bin')) {Remove-Item -LiteralPath (Join-Path $folder 'bin')}
    if (Test-Path -LiteralPath $folder) {Remove-Item -LiteralPath $folder}
}
Write-Host ('Updater regression: '+$checks+' checks passed.')

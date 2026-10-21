param(
    [string]$GameDirectory='C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive\game',
    [Parameter(Mandatory=$true)][ValidateNotNullOrEmpty()][string]$ReportPath
)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'updater_core.ps1')
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$analyzer=Join-Path $root 'bin\x64\Release\tools\BindingAnalyzer.exe'
if (!(Test-Path -LiteralPath $analyzer)) { throw 'Build tools/updater/binding_analyzer.vcxproj first' }
$audit=Get-Content -LiteralPath $ReportPath -Raw | ConvertFrom-Json
$images=Get-UpdaterImages $GameDirectory
foreach ($module in $audit.modules) {
    if ($images[$module.name].pe.Hash -ne $module.sha256) { throw ('Cannot seed recipes from an unaudited '+$module.name) }
}
$functions=@()
foreach ($binding in $audit.functions) {
    if (!$binding.verified -and !$binding.old_entry_matches) { throw ('Audit did not verify '+$binding.name) }
    $rva=[Convert]::ToUInt32($binding.target_rva.Substring(2),16)
    $fingerprint=Get-Fingerprint $images[$binding.module] $rva $analyzer ([int]$binding.explicit_length)
    if ($fingerprint.match_offset -ne 0 -or !(Test-Fingerprint $images[$binding.module].pe $rva $fingerprint)) { throw ('Invalid function identity: '+$binding.name) }
    $header='';if ($binding.header) {$header='src\game\sdk\'+$binding.header}
    $functions+=[pscustomobject]@{ name=$binding.name; module=$binding.module; header=$header; fingerprint=$fingerprint; links=@() }
}
if (!@($audit.vtables).Count -or !@($audit.references).Count) { throw 'Audit must include reviewed vtables and global references; historical RVAs cannot be reused' }
# Preserve known call/branch relationships, including split .pdata chunks.
foreach ($function in $functions) {
    $decode=Get-Decode $images[$function.module] $function.fingerprint.rva $function.fingerprint.length $analyzer
    foreach ($instruction in $decode.instructions | Where-Object {$_.kind -in @('call','branch') -and $_.width -eq 4}) {
        foreach ($target in $functions | Where-Object {$_.module -eq $function.module}) {
            if ($instruction.target -eq $target.fingerprint.rva) {
                $function.links+=[pscustomobject]@{offset=$instruction.offset;operand=$instruction.operand;length=$instruction.length;function=$target.name}
            }
        }
    }
}
$patterns=@()
foreach ($binding in $audit.patterns) {
    if ($binding.matches -ne 1) { throw 'Cannot seed an unresolved signature' }
    $parts=$binding.expression.Split(':',2); $module=$parts[0]; $expression=$parts[1]
    $rva=[Convert]::ToUInt32($binding.rvas[0].Substring(2),16)
    $fingerprint=$null
    if ($images[$module].pe.Contains($rva,$true) -and $null -ne $images[$module].pe.FunctionAt($rva)) {
        $fingerprint=Get-Fingerprint $images[$module] $rva $analyzer
    }
    $patterns+=[pscustomobject]@{file=$binding.file;module=$module;expression=$expression;rva=$rva;fingerprint=$fingerprint}
}
$profile=[pscustomobject]@{
    version=1; audited=$audit.audited; modules=$audit.modules; functions=$functions; patterns=$patterns
    exports=@(
        @{module='tier0.dll';name='?LoadKV3@@YA_NPEAVKeyValues3@@PEAVCUtlString@@PEBDAEBUKV3ID_t@@2I@Z'},
        @{module='tier0.dll';name='?Insert@CBufferString@@QEAAPEBDHPEBDH_N@Z'},
        @{module='tier0.dll';name='?Purge@CBufferString@@QEAAXH@Z'},
        @{module='tier0.dll';name='CreateInterface'}, @{module='resourcesystem.dll';name='CreateInterface'}
    )
    vtables=$audit.vtables
    references=$audit.references
    schema_fields=$audit.schema_fields
}
Write-UpdaterJson (Join-Path $PSScriptRoot 'build-recipes.json') $profile
Write-Host ('Seeded '+$functions.Count+' function proofs and '+$patterns.Count+' signature locators from the audited build.')

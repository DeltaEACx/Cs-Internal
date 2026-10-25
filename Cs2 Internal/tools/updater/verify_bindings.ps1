param(
    [string]$GameDirectory=(Join-Path ${env:ProgramFiles(x86)} 'Steam\steamapps\common\Counter-Strike Global Offensive\game'),
    [string]$IdaUri='',
    [string]$ReportPath=(Join-Path $PSScriptRoot '..\..\.build-updater\bindings-verification.json')
)
# Read-only verification: never changes bindings, game files, or the IDA database.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'updater_core.ps1')
$root=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$profile=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'build-recipes.json') -Raw | ConvertFrom-Json
$audit=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'bindings-audit.json') -Raw | ConvertFrom-Json
$images=Get-UpdaterImages $GameDirectory
$expectedHashes=@{}
foreach ($proof in $audit.interface_proofs) { $expectedHashes[$proof.module]=$proof.module_sha256 }
foreach ($module in $profile.modules) { $expectedHashes[$module.name]=$module.sha256 }
$checks=[Collections.Generic.List[object]]::new()
function Check([string]$Kind,[string]$Name,[scriptblock]$Action) {
    try { & $Action; $checks.Add([pscustomobject]@{kind=$Kind;name=$Name;passed=$true}) }
    catch { $checks.Add([pscustomobject]@{kind=$Kind;name=$Name;passed=$false;reason=$_.Exception.Message}) }
}
function Require([bool]$Condition,[string]$Reason) { if (!$Condition) { throw $Reason } }
function Constant([string]$File,[string]$Name) {
    $source=Get-Content -LiteralPath (Join-Path $root $File) -Raw
    $matches=[regex]::Matches($source,'\b'+[regex]::Escape($Name)+'\s*=\s*(0x[0-9a-fA-F]+|\d+)u?\b')
    Require ($matches.Count -eq 1) ('Expected one constant: '+$Name)
    return [Convert]::ToUInt32($matches[0].Groups[1].Value.Replace('0x',''),$(if ($matches[0].Groups[1].Value.StartsWith('0x')) {16} else {10}))
}
foreach ($module in $profile.modules) {
    Check 'module' $module.name { Require ($images[$module.name].pe.Hash -ceq $module.sha256) 'Installed SHA256 differs from audited module' }
}
# A current recipe alone does not enable gameplay: the compiled feature gates
# also pin module hashes. Verify those declarations so stale source pins cannot
# pass a binding audit while leaving the menu functional and gameplay disabled.
$sourceHashBindings=@(
    @('client.dll','src\game\sdk\SkinBindings.h','kClientSha256'),
    @('resourcesystem.dll','src\game\sdk\SkinBindings.h','kResourceSha256'),
    @('scenesystem.dll','src\game\sdk\ChamsBindings.h','kSceneSha256'),
    @('materialsystem2.dll','src\game\sdk\ChamsBindings.h','kMaterialSha256'),
    @('tier0.dll','src\game\sdk\ChamsBindings.h','kTier0Sha256'),
    @('meshsystem.dll','src\game\sdk\BacktrackBindings.h','meshSha256'),
    @('materialsystem2.dll','src\game\sdk\PreviewBindings.h','kMaterialSha256'),
    @('rendersystemdx11.dll','src\game\sdk\PreviewBindings.h','kRenderSha256'),
    @('engine2.dll','src\game\sdk\ViewBindings.h','kEngineSha256'),
    @('scenesystem.dll','src\features\visuals\effects\combat.cpp','scene_hash'),
    @('engine2.dll','src\features\visuals\effects\combat.cpp','engine_hash'),
    @('scenesystem.dll','src\features\visuals\effects\world.cpp','scene_hash'),
    @('resourcesystem.dll','src\features\visuals\effects\world.cpp','resource_hash'),
    @('materialsystem2.dll','src\features\visuals\effects\world.cpp','material_hash'),
    @('tier0.dll','src\features\visuals\effects\world.cpp','tier0_hash'),
    @('engine2.dll','src\features\visuals\effects\world.cpp','engine_hash')
)
foreach ($binding in $sourceHashBindings) {
    Check 'source_module_hash' ($binding[1]+':'+$binding[2]) {
        $source=Get-Content -LiteralPath (Join-Path $root $binding[1]) -Raw
        $matches=[regex]::Matches($source,'\b'+[regex]::Escape($binding[2])+'\s*=\s*"([a-f0-9]{64})"')
        Require ($matches.Count -eq 1) 'Expected one source module-hash declaration'
        Require ($matches[0].Groups[1].Value -ceq $images[$binding[0]].pe.Hash) 'Source runtime gate rejects the installed module'
    }
}
foreach ($file in Get-ChildItem -LiteralPath (Join-Path $root 'src') -Recurse -File | Where-Object {$_.Extension -in @('.h','.hpp','.cpp')}) {
    $source=Get-Content -LiteralPath $file.FullName -Raw
    foreach ($match in [regex]::Matches($source,'module_matches\("([\w]+\.dll)",\s*"([a-f0-9]{64})"\)')) {
        Check 'source_module_hash' ($file.Name+':'+$match.Groups[1].Value) {
            Require ($images.ContainsKey($match.Groups[1].Value)) 'Unrecognized literal module-hash gate'
            Require ($match.Groups[2].Value -ceq $images[$match.Groups[1].Value].pe.Hash) 'Source runtime gate rejects the installed module'
        }
    }
}
$functions=@{}
foreach ($binding in $profile.functions) { $functions[$binding.name]=$binding }
foreach ($binding in $profile.functions) {
    Check 'function' $binding.name {
        $pe=$images[$binding.module].pe; $rva=[uint32]$binding.fingerprint.rva
        Require (Test-Fingerprint $pe $rva $binding.fingerprint) 'Function body, chunks, or semantic anchors changed'
        foreach ($link in $binding.links) {
            Require ($pe.Relative($rva+$link.offset,$link.operand,$link.length) -eq $functions[$link.function].fingerprint.rva) 'Caller/callee link changed'
        }
        if ($binding.header) {
            $source=Get-Content -LiteralPath (Join-Path $root $binding.header) -Raw
            $expression='(?s)\bFunction\s+'+[regex]::Escape($binding.name)+'\s*\{\s*(0x[0-9a-fA-F]+)u?\s*,\s*\{([^}]*)\}'
            $matches=[regex]::Matches($source,$expression)
            Require ($matches.Count -eq 1) 'Expected one source function declaration'
            Require ([Convert]::ToUInt32($matches[0].Groups[1].Value.Substring(2),16) -eq $rva) 'Source RVA differs from audited function'
            $entry=@([regex]::Matches($matches[0].Groups[2].Value,'0x([0-9a-fA-F]{2})') | ForEach-Object {[Convert]::ToInt32($_.Groups[1].Value,16)})
            Require ($entry.Count -eq 24 -and $pe.Matches($rva,[int[]]$entry)) 'Source entry bytes differ from installed function'
        }
    }
}
foreach ($binding in $profile.patterns) {
    Check 'signature' ($binding.module+':'+$binding.expression) {
        $pe=$images[$binding.module].pe; $parsed=Convert-Pattern $binding.expression
        $hits=$pe.Find($parsed.bytes,$false)
        Require ($hits.Count -eq 1 -and $hits[0] -eq $binding.rva) 'Signature is no longer unique at the audited location'
        if ($null -ne $binding.fingerprint) {
            Require (Test-Fingerprint $pe $binding.fingerprint.rva $binding.fingerprint) 'Signature containing function changed'
        }
        $null=Resolve-PatternTarget $pe $hits[0] $parsed
        $source=Get-Content -LiteralPath (Join-Path $root $binding.file) -Raw
        $literal='"'+$binding.module+':'+$binding.expression+'"'
        Require ([regex]::Matches($source,[regex]::Escape($literal)).Count -eq 1) 'Signature source location is missing or duplicated'
    }
}
foreach ($table in $profile.vtables) {
    Check 'vtable' $table.type {
        $pe=$images[$table.module].pe
        $hits=@($pe.Vtables($table.type) | Where-Object {$pe.RttiOffset($_) -eq 0})
        Require ($hits.Count -eq 1) 'RTTI does not identify exactly one primary vtable'
        if ($table.constant) { Require ((Constant $table.file $table.constant) -eq $hits[0]) 'Source vtable RVA differs' }
        if ($table.slot -ge 0) {
            Require (($pe.Pointer($hits[0]+$table.slot*8)-$pe.ImageBase) -eq $functions[$table.function].fingerprint.rva) 'Vtable slot points to a different method'
        }
    }
}
foreach ($reference in $profile.references) {
    Check 'global_reference' $reference.constant {
        $pe=$images['client.dll'].pe
        $target=$pe.Relative($functions[$reference.function].fingerprint.rva+$reference.offset,$reference.operand,$reference.length)
        Require ($pe.Contains($target,$false) -and !$pe.Contains($target,$true)) 'Global no longer points to data'
        Require ($target -eq (Constant $reference.file $reference.constant)) 'Source global RVA differs'
    }
}
foreach ($export in $profile.exports) {
    Check 'export' ($export.module+':'+$export.name) {
        $pe=$images[$export.module].pe; $rva=$pe.Export($export.name)
        Require ($rva -ne 0 -and $pe.Contains($rva,$true)) 'Executable export missing'
    }
}
$schemaSource=Get-Content -LiteralPath (Join-Path $root 'src\game\sdk\client_dll.hpp') -Raw
foreach ($field in $profile.schema_fields) {
    Check 'schema_field' ($field.class+'::'+$field.field) {
        $pe=$images['client.dll'].pe
        $className=$pe.Ascii([uint32]($pe.Pointer($field.class_rva)-$pe.ImageBase))
        $fieldName=$pe.Ascii([uint32]($pe.Pointer($field.field_rva)-$pe.ImageBase))
        Require ($className -ceq $field.class -and $fieldName -ceq $field.field) 'Class/field metadata identity changed'
        Require ($pe.U32($field.field_rva+16) -eq $field.offset) 'Static schema offset changed'
        $class=[regex]::Match($schemaSource,'(?s)namespace\s+'+[regex]::Escape($field.class)+'\s*\{([^{}]*)\}')
        $value=[regex]::Match($class.Groups[1].Value,'\b'+[regex]::Escape($field.field)+'\s*=\s*(0x[0-9a-fA-F]+)')
        Require ($value.Success -and [Convert]::ToUInt32($value.Groups[1].Value.Substring(2),16) -eq $field.offset) 'Source schema offset differs'
    }
}
foreach ($module in $audit.offsets.PSObject.Properties) {
    if (!$images.ContainsKey($module.Name)) {
        $path=Join-Path $GameDirectory ('bin\win64\'+$module.Name)
        if (!(Test-Path -LiteralPath $path)) { $path=Join-Path $GameDirectory ('csgo\bin\win64\'+$module.Name) }
        $images[$module.Name]=[pscustomobject]@{path=$path;pe=[PeAudit]::new($path)}
        Check 'offset_module' $module.Name {
            $proof=@($audit.interface_proofs | Where-Object module -eq $module.Name | Select-Object -First 1)
            Require ($proof.Count -eq 1 -and $proof[0].module_sha256 -ceq $images[$module.Name].pe.Hash) 'Offset module differs from audited binary'
        }
    }
    foreach ($constant in $module.Value.PSObject.Properties) {
        Check 'offset_declaration' ($module.Name+':'+$constant.Name) {
            Require ((Constant 'src\game\sdk\offsets.hpp' $constant.Name) -eq $constant.Value) 'Source offset differs from audit'
        }
    }
}
foreach ($proof in $audit.offset_proofs) {
    Check 'offset_locator' ($proof.module+':'+$proof.name) {
        $pe=$images[$proof.module].pe; $parsed=Convert-Pattern $proof.expression
        $hits=$pe.Find($parsed.bytes,$false)
        Require ($hits.Count -eq 1 -and $hits[0] -eq $proof.match_rva) 'Offset locator is no longer unique at its audited location'
        $target=if ($null -ne $proof.operand) { $pe.U32($hits[0]+$proof.operand) } else { Resolve-PatternTarget $pe $hits[0] $parsed }
        Require (($target+$proof.adjustment) -eq $proof.offset) 'Offset locator target changed'
    }
}
$ida=[pscustomobject]@{state='not_requested'}
if ($IdaUri) {
    try {
        $endpoint=[Uri]$IdaUri
        Require ($endpoint.IsLoopback -and $endpoint.Scheme -in @('http','https')) 'IDA endpoint must be local'
        $client=$images['client.dll'].pe
        $spec=[pscustomobject]@{
            sha256=$client.Hash
            functions=@($profile.functions | Where-Object module -eq 'client.dll' | ForEach-Object {
                $rva=[uint32]$_.fingerprint.rva
                $pieces=@([pscustomobject]@{offset=0;length=$_.fingerprint.length})+@($_.fingerprint.fragments)
                [pscustomobject]@{name=$_.name;rva=$rva;pieces=@($pieces | ForEach-Object {
                    [pscustomobject]@{offset=$_.offset;bytes=([BitConverter]::ToString($client.Bytes($rva+$_.offset,$_.length))).Replace('-','').ToLower()}
                })}
            })
        }
        $payload=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes(($spec | ConvertTo-Json -Depth 12 -Compress)))
        $code=@'
import base64, json
import ida_auto, ida_loader
spec = json.loads(base64.b64decode('__PAYLOAD__'))
identity = {
    'input_file': ida_nalt.get_input_file_path(),
    'root_filename': ida_nalt.get_root_filename(),
    'input_sha256': ida_nalt.retrieve_input_file_sha256().hex(),
    'idb_path': ida_loader.get_path(ida_loader.PATH_TYPE_IDB),
    'ida_version': idaapi.get_kernel_version(),
    'analysis_ready': ida_auto.auto_is_ok(),
}
assert identity['root_filename'].lower() == 'client.dll', 'Open the installed client.dll database in IDA'
assert identity['input_sha256'] == spec['sha256'], 'IDA input differs from installed client.dll'
assert identity['analysis_ready'], 'Wait for IDA autoanalysis to complete'
base = ida_nalt.get_imagebase()
results = []
for item in spec['functions']:
    address = base + item['rva']
    function = ida_funcs.get_func(address)
    result = {'name': item['name'], 'rva': item['rva'], 'function_start_matches': function is not None and function.start_ea == address}
    result['all_function_bytes_match'] = all(ida_bytes.get_bytes(address + piece['offset'], len(piece['bytes']) // 2) == bytes.fromhex(piece['bytes']) for piece in item['pieces'])
    results.append(result)
return_value = dict(identity, functions=results)
'@
        $response=Invoke-RestMethod -Uri ($IdaUri.TrimEnd('/')+'/api/execute') -Method Post -ContentType 'application/json' -Body (@{script=$code.Replace('__PAYLOAD__',$payload)} | ConvertTo-Json -Compress) -TimeoutSec 30
        Require ($response.success -and $null -ne $response.return_value) 'IDA did not return verification results'
        $ida=$response.return_value
        $ida | Add-Member -NotePropertyName state -NotePropertyValue 'verified'
        foreach ($function in $ida.functions) {
            Check 'ida_function' $function.name { Require ($function.function_start_matches -and $function.all_function_bytes_match) 'IDA function start or complete function bytes differ' }
        }
        # Some plugin versions retain stale /api/info metadata. Identity above is
        # read directly through ida_nalt inside the current database instead.
        $ida | Add-Member -NotePropertyName identity_method -NotePropertyValue 'Direct ida_nalt input path/root/hash via /api/execute; /api/info is not trusted for database identity.'
    } catch {
        $ida=[pscustomobject]@{state='unavailable';reason=$_.Exception.Message}
        $checks.Add([pscustomobject]@{kind='ida';name='database_verification';passed=$false;reason=$_.Exception.Message})
    }
}
$failures=@($checks | Where-Object {!$_.passed})
$report=[pscustomobject]@{
    version=1;verified_at=[DateTimeOffset]::UtcNow.ToString('o')
    state=$(if ($failures.Count) {'review_required'} else {'verified_unchanged'})
    recipe_audited=$profile.audited
    modules=@($images.Keys | Sort-Object | ForEach-Object {[pscustomobject]@{name=$_;sha256=$images[$_].pe.Hash;matches_audited=($images[$_].pe.Hash -ceq $expectedHashes[$_])}})
    summary=@($checks | Group-Object kind | ForEach-Object {[pscustomobject]@{kind=$_.Name;checked=$_.Count;passed=@($_.Group | Where-Object passed).Count}})
    failures=$failures;ida=$ida
    binding_changes=@();live_gameplay_validated=$false
}
$ReportPath=[IO.Path]::GetFullPath($ReportPath)
[IO.Directory]::CreateDirectory((Split-Path $ReportPath -Parent)) | Out-Null
Write-UpdaterJson $ReportPath $report
Write-Host ($report.state+': '+($checks.Count-$failures.Count)+'/'+$checks.Count+' checks passed. Report: '+$ReportPath)
if ($failures.Count) { $failures | Format-Table kind,name,reason -Wrap; exit 2 }

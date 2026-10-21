$ErrorActionPreference = 'Stop'
if (-not ('PeAudit' -as [type])) { Add-Type -Path (Join-Path $PSScriptRoot 'pe_audit.cs') }

function Get-UpdaterImages([string]$GameDirectory) {
    $images = @{}
    foreach ($name in @('client.dll','engine2.dll','resourcesystem.dll','scenesystem.dll','materialsystem2.dll','rendersystemdx11.dll','tier0.dll','meshsystem.dll')) {
        $relative = 'bin\win64\' + $name
        if ($name -eq 'client.dll') { $relative = 'csgo\bin\win64\client.dll' }
        $path = Join-Path $GameDirectory $relative
        $images[$name] = [pscustomobject]@{ path=$path; pe=[PeAudit]::new($path) }
    }
    return $images
}
function Convert-Pattern([string]$Expression) {
    $bytes = [Collections.Generic.List[int]]::new(); $positions = [Collections.Generic.List[int]]::new()
    $operation=''; $operand=0; $adjustment=0L; $dereference=$false
    for ($i=0; $i -lt $Expression.Length;) {
        $position=$i; $c=$Expression[$i]; $i++
        if ([char]::IsWhiteSpace($c)) { continue }
        if ($c -eq '~') { $dereference=$true; continue }
        if ($c -in @('*','^','>')) {
            if ($operation) { throw 'Multiple signature operations are unsupported' }
            $operation=[string]$c; $operand=$bytes.Count; continue
        }
        if ($c -in @('+','-')) {
            $start=$i; while ($i -lt $Expression.Length -and $Expression[$i] -match '[0-9a-fA-F]') { $i++ }
            if ($i -eq $start) { throw 'Empty signature adjustment' }
            $value=[Convert]::ToInt64($Expression.Substring($start,$i-$start),16)
            if ($c -eq '-') { $value=-$value }; $adjustment+=$value; continue
        }
        if ($c -eq '?') { if ($i -lt $Expression.Length -and $Expression[$i] -eq '?') { $i++ }; $bytes.Add(-1) }
        else {
            if ($i -ge $Expression.Length) { throw 'Incomplete signature byte' }
            $bytes.Add([Convert]::ToInt32(([string]$c)+([string]$Expression[$i]),16)); $i++
        }
        $positions.Add($position)
    }
    return [pscustomobject]@{ bytes=$bytes.ToArray(); positions=$positions.ToArray(); operation=$operation; operand=$operand; adjustment=$adjustment; dereference=$dereference }
}
function Convert-HexPattern([string]$Hex) { return (Convert-Pattern $Hex).bytes }
function Get-ModuleKey($Images) {
    $description=($Images.Keys | Sort-Object | ForEach-Object { $_+':'+$Images[$_].pe.Hash }) -join '|'
    $sha=[Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($description)))).Replace('-','').ToLower() }
    finally { $sha.Dispose() }
}
function Get-Decode($Image, [uint32]$Rva, [int]$Length, [string]$Analyzer) {
    $json = & $Analyzer $Image.path ('0x{0:X}' -f $Rva) $Length
    if ($LASTEXITCODE -ne 0) { throw ('Instruction decoding failed at '+('0x{0:X}' -f $Rva)) }
    return ($json -join "`n" | ConvertFrom-Json)
}
function Get-TargetEvidence($Pe, [uint32]$Target) {
    $type=$Pe.Rtti($Target)
    if ($type) { return [pscustomobject]@{ kind='rtti'; value=$type; object_offset=$Pe.RttiOffset($Target) } }
    try {
        $text=$Pe.Ascii($Target)
        if ($text.Length -ge 8 -and $text.Length -le 200 -and $text -cmatch '^[A-Za-z_][A-Za-z0-9_ ./\\:;=%\-()]+$') {
            return [pscustomobject]@{ kind='string'; value=$text }
        }
        $pointer=$Pe.Pointer($Target)
        if ($pointer -ge $Pe.ImageBase -and $pointer-$Pe.ImageBase -lt $Pe.ImageSize) {
            $text=$Pe.Ascii([uint32]($pointer-$Pe.ImageBase))
            if ($text.Length -ge 8 -and $text.Length -le 200 -and $text -cmatch '^[A-Za-z_][A-Za-z0-9_ ./\\:;=%\-()]+$') {
                return [pscustomobject]@{ kind='indirect_string'; value=$text }
            }
        }
    } catch { }
    return $null
}
function Get-Fingerprint($Image, [uint32]$Rva, [string]$Analyzer, [int]$ExplicitLength=0) {
    $pieces=$Image.pe.FunctionPieces($Rva)
    $range=$null; if ($pieces.Count) { $range=$pieces[0] }
    if ($ExplicitLength) { $start=$Rva; $length=$ExplicitLength }
    elseif ($null -ne $range) { $start=$range.Start; $length=[int]($range.End-$range.Start) }
    else { throw ('No exception-directory function boundary at '+('0x{0:X}' -f $Rva)) }
    if ($length -gt 65536) { throw 'Function is too large for automatic validation' }
    $decoded=Get-Decode $Image $start $length $Analyzer
    $fragments=@()
    if (!$ExplicitLength) {
        foreach ($piece in $pieces | Select-Object -Skip 1) {
            $size=[int]($piece.End-$piece.Start)
            $part=Get-Decode $Image $piece.Start $size $Analyzer
            $fragments+=[pscustomobject]@{offset=([long]$piece.Start-$start);length=$size;pattern=$part.pattern}
        }
    }
    $anchors=@()
    foreach ($instruction in $decoded.instructions) {
        if ($instruction.kind -eq 'rip' -and $instruction.target -ge 0 -and $instruction.target -lt $Image.pe.ImageSize) {
            $evidence=Get-TargetEvidence $Image.pe ([uint32]$instruction.target)
            if ($null -ne $evidence) { $anchors+=[pscustomobject]@{ offset=$instruction.offset; operand=$instruction.operand; length=$instruction.length; evidence=$evidence } }
        }
    }
    return [pscustomobject]@{ rva=$start; length=$length; explicit_range=($ExplicitLength -gt 0); match_offset=([long]$Rva-$start); pattern=$decoded.pattern; decoded=$decoded.decoded; anchors=$anchors; fragments=$fragments }
}
function Test-Fingerprint($Pe, [uint32]$Rva, $Fingerprint) {
    if (-not $Pe.Contains($Rva,$true) -or -not $Pe.Matches($Rva,(Convert-HexPattern $Fingerprint.pattern))) { return $false }
    if (-not $Fingerprint.explicit_range) {
        $pieces=$Pe.FunctionPieces($Rva)
        if ($pieces.Count -ne 1+@($Fingerprint.fragments).Count) { return $false }
        if ($pieces[0].Start -ne $Rva -or $pieces[0].End-$pieces[0].Start -ne $Fingerprint.length) { return $false }
    }
    foreach ($fragment in $Fingerprint.fragments) {
        if (-not $Pe.Matches([uint32]($Rva+$fragment.offset),(Convert-HexPattern $fragment.pattern))) { return $false }
    }
    foreach ($anchor in $Fingerprint.anchors) {
        try {
            $target=$Pe.Relative($Rva+[uint32]$anchor.offset,[int]$anchor.operand,[int]$anchor.length)
            $actual=Get-TargetEvidence $Pe $target
            if ($null -eq $actual -or $actual.kind -ne $anchor.evidence.kind -or $actual.value -cne $anchor.evidence.value) { return $false }
            if ($actual.kind -eq 'rtti' -and $actual.object_offset -ne $anchor.evidence.object_offset) { return $false }
        } catch { return $false }
    }
    return $true
}
function Find-Fingerprint($Pe, $Fingerprint) {
    # A long, normalized body is required; short prologues are never sufficient.
    $candidates=$Pe.Find((Convert-HexPattern $Fingerprint.pattern),$true)
    $valid=@()
    foreach ($rva in $candidates) {
        $range=$Pe.FunctionAt($rva)
        if ($null -ne $range -and ($range.Start -ne $rva -or (-not $Fingerprint.explicit_range -and $range.End-$range.Start -ne $Fingerprint.length))) { continue }
        if (Test-Fingerprint $Pe $rva $Fingerprint) { $valid+=$rva }
    }
    if ($valid.Count -ne 1) { throw ('Expected one validated function body; found '+$valid.Count) }
    return [uint32]$valid[0]
}
function Resolve-PatternTarget($Pe, [uint32]$Match, $Parsed) {
    $target=[long]$Match
    if ($Parsed.operation -in @('*','>')) {
        $operand=[int]$Parsed.operand; if ($Parsed.operation -eq '>') { $operand++ }
        $target=$Pe.Relative($Match,$operand,$operand+4)
    } elseif ($Parsed.operation -eq '^') { $target=[long]$Pe.Pointer($Match+$Parsed.operand)-[long]$Pe.ImageBase }
    $target+=$Parsed.adjustment
    # BSS globals are dereferenced only inside the game; validate their storage here.
    if ($target -lt 0 -or $target -ge $Pe.ImageSize -or -not $Pe.Contains([uint32]$target,$false)) { throw 'Signature resolved outside its module' }
    return [uint32]$target
}
function Get-UpdaterBuildSourceItems([string]$Root) {
    $Root=[IO.Path]::GetFullPath($Root).TrimEnd('\')
    $boundary=$Root+'\'
    # Copy only checked-in build inputs. Frontend and Rust outputs are rebuilt in staging.
    $paths=@(
        'src','third_party','build','Cs2 Internal.vcxproj','build.ps1',
        'gui\index.html','gui\package.json','gui\package-lock.json',
        'gui\build-embedded.ps1','gui\build.ps1','gui\src',
        'gui\src-tauri\Cargo.toml','gui\src-tauri\Cargo.lock','gui\src-tauri\build.rs',
        'gui\src-tauri\tauri.conf.json','gui\src-tauri\tauri.e2e.conf.json',
        'gui\src-tauri\src','gui\src-tauri\capabilities','gui\src-tauri\icons','gui\src-tauri\vendor'
    )
    foreach ($path in @('gui\public','gui\.gitignore','gui\Launch DeltaEAC.cmd','gui\src-tauri\.cargo')) {
        if (Test-Path -LiteralPath (Join-Path $Root $path)) { $paths+=$path }
    }
    $paths+=@(Get-ChildItem -LiteralPath (Join-Path $Root 'gui') -Filter 'vite.config.*' -File | ForEach-Object { 'gui\'+$_.Name })
    [xml]$project=Get-Content -LiteralPath (Join-Path $Root 'Cs2 Internal.vcxproj') -Raw
    foreach ($resource in $project.SelectNodes("//*[local-name()='ResourceCompile'][@Include]")) { $paths+=[string]$resource.Include }
    $items=@()
    foreach ($path in $paths | Select-Object -Unique) {
        $fullPath=[IO.Path]::GetFullPath((Join-Path $Root $path))
        if (!$fullPath.StartsWith($boundary,[StringComparison]::OrdinalIgnoreCase)) { throw ('Build input is outside the repository: '+$path) }
        $item=Get-Item -LiteralPath $fullPath
        if ($item.PSIsContainer) { $items+=@(Get-ChildItem -LiteralPath $fullPath -Recurse -File) }
        else { $items+=$item }
    }
    return $items | Sort-Object FullName -Unique
}
function Copy-UpdaterBuildSources([string]$Root, [string]$Destination) {
    $Root=[IO.Path]::GetFullPath($Root).TrimEnd('\')
    $Destination=[IO.Path]::GetFullPath($Destination).TrimEnd('\')
    $directories=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($item in Get-UpdaterBuildSourceItems $Root) {
        $relative=$item.FullName.Substring($Root.Length+1)
        $target=Join-Path $Destination $relative
        $parent=Split-Path -Parent $target
        if ($directories.Add($parent)) { [IO.Directory]::CreateDirectory($parent) | Out-Null }
        Copy-Item -LiteralPath $item.FullName -Destination $target
    }
}
function Get-UpdaterSourceKey([string]$Root) {
    $Root=[IO.Path]::GetFullPath($Root).TrimEnd('\')
    $items=@(Get-UpdaterBuildSourceItems $Root)
    foreach ($path in @('tools\updater\build-recipes.json','tools\updater\bindings-audit.json','tools\updater\updater_core.ps1','tools\updater\update_build.ps1','tools\updater\pe_audit.cs')) {
        $items+=Get-Item -LiteralPath (Join-Path $Root $path)
    }
    $text=($items | Sort-Object FullName | ForEach-Object { $_.FullName.Substring($Root.Length)+':'+(Get-FileHash -LiteralPath $_.FullName).Hash }) -join '|'
    $sha=[Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text)))).Replace('-','').ToLower() }
    finally { $sha.Dispose() }
}
function Write-UpdaterJson([string]$Path, $Object) {
    $temporary=$Path+'.tmp-'+[Guid]::NewGuid().ToString('N')
    [IO.File]::WriteAllText($temporary,($Object | ConvertTo-Json -Depth 30),[Text.UTF8Encoding]::new($false))
    if ([IO.File]::Exists($Path)) { [IO.File]::Replace($temporary,$Path,[NullString]::Value) } else { [IO.File]::Move($temporary,$Path) }
}

function Get-DumpSchemaOffset($Value,[string]$Name) {
    # cs2-dumper emits integers; ExitScam emits { offset, type } records.
    if ($null -ne $Value -and $null -ne $Value.PSObject.Properties['offset']) { $Value=$Value.offset }
    $offset=0
    if ($null -eq $Value -or ![int]::TryParse([string]$Value,[ref]$offset) -or $offset -lt 0 -or $offset -gt 0x100000) {
        throw ('Missing or invalid schema field '+$Name)
    }
    return $offset
}

function Assert-DumpBuildIdentity($Engine,$Offsets,$Info,[string]$GameRoot) {
    $rva=$Offsets.'engine2.dll'.dwBuildNumber
    if ($null -eq $rva -or !$Engine.Contains([uint32]$rva,$false) -or $Engine.Contains([uint32]$rva,$true)) {
        throw 'Dump build-number storage is outside engine2.dll data'
    }
    $initial=$Engine.U32([uint32]$rva)
    if ($initial -ne [uint32]::MaxValue -and $initial -ne 0) {
        if ($initial -ne $Info.build_number) { throw 'Dump build number does not match the installed engine2.dll' }
        return
    }
    # The native initializer fills this global at runtime. Its on-disk -1/0 is
    # not a build number; ExitScam's info uses PatchVersion without periods.
    $steamInfo=Get-Content -LiteralPath (Join-Path $GameRoot 'csgo\steam.inf') -Raw
    $patch=[regex]::Match($steamInfo,'(?m)^PatchVersion=(\d+(?:\.\d+){3})\s*$')
    if (!$patch.Success -or [long]$patch.Groups[1].Value.Replace('.','') -ne $Info.build_number) {
        throw 'Dump build number does not match the installed steam.inf PatchVersion'
    }
}

function Get-UpdatedDumpHeader([string]$Source,$Modules) {
    # Preserve the curated API: a heap-allocated interface is not a static RVA,
    # and importing a generated header can silently drop required constants.
    return [regex]::Replace($Source,'(?s)(namespace\s+(\w+)\s*\{)([^{}]*)(\})',[Text.RegularExpressions.MatchEvaluator]{param($group)
        $module=$group.Groups[2].Value.Replace('_dll','.dll')
        $values=$Modules.$module
        $body=[regex]::Replace($group.Groups[3].Value,'(constexpr\s+std::ptrdiff_t\s+(\w+)\s*=\s*)0x[0-9a-fA-F]+',[Text.RegularExpressions.MatchEvaluator]{param($field)
            $name=$field.Groups[2].Value;$value=$values.$name
            if ($null -eq $value -or $value -lt 0 -or $value -gt [uint32]::MaxValue) { throw ('Missing or invalid dumped constant '+$module+'::'+$name) }
            $field.Groups[1].Value+('0x{0:X}' -f [uint32]$value)
        })
        $group.Groups[1].Value+$body+$group.Groups[4].Value
    })
}

function Add-VerifiedInterfaceFallbacks($Interfaces,$Proofs,[string]$GameRoot) {
    foreach ($proof in $Proofs) {
        $module=$proof.module;$name=$proof.name
        if ($null -ne $Interfaces.$module.$name) {continue}
        $path=Join-Path $GameRoot ('bin\win64\'+$module)
        if (!(Test-Path -LiteralPath $path)) {$path=Join-Path $GameRoot ('csgo\bin\win64\'+$module)}
        $pe=[PeAudit]::new($path)
        if ($pe.Hash -cne $proof.module_sha256 -or
            !$pe.Matches([uint32]$proof.factory_rva,(Convert-HexPattern '488D05????????C3')) -or
            $pe.Relative([uint32]$proof.factory_rva,3,7) -ne $proof.offset) {
            throw ('Dump omitted '+$module+'::'+$name+' and its audited factory no longer matches')
        }
        if ($null -eq $Interfaces.$module) {$Interfaces | Add-Member -NotePropertyName $module -NotePropertyValue ([pscustomobject]@{})}
        $Interfaces.$module | Add-Member -NotePropertyName $name -NotePropertyValue $proof.offset
    }
}

function Format-CppRvaPair([uint32]$First,[uint32]$Second) {
    return '{'+('0x{0:X}, 0x{1:X}' -f $First,$Second)+'}'
}

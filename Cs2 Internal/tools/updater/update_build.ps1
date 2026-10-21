param(
    [ValidateSet('Check','Update','Watch')][string]$Mode='Update',
    [string]$RepositoryRoot=(Split-Path (Split-Path $PSScriptRoot -Parent) -Parent),
    [string]$GameDirectory='',
    [string]$DumperPath='',
    [string]$DumpDirectory='',
    [switch]$NoBuild,
    [switch]$Force,
    [switch]$RuntimeRequest
)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'updater_core.ps1')
$RepositoryRoot=(Resolve-Path -LiteralPath $RepositoryRoot).Path
$config=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'updater_config.json') -Raw | ConvertFrom-Json
$cacheRoot=Join-Path $RepositoryRoot '.build-updater'
[IO.Directory]::CreateDirectory($cacheRoot) | Out-Null
$statusPath=Join-Path $cacheRoot 'status.json'
$failures=[Collections.Generic.List[object]]::new()
$reportPath=''; $artifact=''; $lock=$null

function Set-UpdateStatus([string]$State,[string]$Message) {
    Write-UpdaterJson $statusPath ([pscustomobject]@{state=$State;message=$Message;updated=[DateTime]::UtcNow.ToString('o');report=$reportPath;artifact=$artifact;pid=$PID})
    Write-Host $Message
}
function Add-UpdateFailure([string]$Binding,[string]$Reason) {
    $failures.Add([pscustomobject]@{binding=$Binding;reason=$Reason})
}
function Assert-Within([string]$Path,[string]$Directory) {
    $full=[IO.Path]::GetFullPath($Path);$parent=[IO.Path]::GetFullPath($Directory).TrimEnd('\')+'\'
    if (!$full.StartsWith($parent,[StringComparison]::OrdinalIgnoreCase)) { throw 'Generated path escapes the updater workspace' }
    return $full
}
function Get-GameRoot {
    $given=$GameDirectory; if (!$given) { $given=$config.game_directory }
    if (!$given) {
        $process=Get-Process -Name cs2 -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($process -and $process.Path) { $given=Split-Path (Split-Path (Split-Path $process.Path -Parent) -Parent) -Parent }
    }
    if (!$given) { $given=Join-Path ${env:ProgramFiles(x86)} 'Steam\steamapps\common\Counter-Strike Global Offensive\game' }
    $given=(Resolve-Path -LiteralPath $given).Path
    if ((Split-Path $given -Leaf) -eq 'csgo') { $given=Split-Path $given -Parent }
    if (!(Test-Path -LiteralPath (Join-Path $given 'csgo\bin\win64\client.dll'))) { throw 'GameDirectory must point to the installed CS2 game folder' }
    return $given
}
function Get-MsBuild {
    $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (!(Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio with Desktop development with C++ to rebuild updates' }
    $path=& $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
    if (!$path) { throw 'MSBuild and the x64 C++ tools were not found' }; return $path
}
function Invoke-FreshDump([string]$Output,[string]$Root) {
    $path=$DumperPath; if (!$path) { $path=$config.dumper_path }
    if (!$path) {
        foreach ($candidate in @((Join-Path $RepositoryRoot 'tools\bin\cs2-dumper.exe'),(Join-Path $env:USERPROFILE 'Downloads\cs2-dumper.exe'))) {
            if (Test-Path -LiteralPath $candidate) { $path=$candidate; break }
        }
    }
    if (!$path) { throw 'Set dumper_path in tools/updater/updater_config.json or put cs2-dumper.exe in tools/bin' }
    if (![IO.Path]::IsPathRooted($path)) { $path=Join-Path $RepositoryRoot $path }
    $path=(Resolve-Path -LiteralPath $path).Path
    $processes=@(Get-Process -Name cs2 -ErrorAction SilentlyContinue)
    if ($processes.Count -ne 1) { throw 'Run one CS2 instance and stay at its main menu to dump this build' }
    $expected=[IO.Path]::GetFullPath((Join-Path $Root 'bin\win64\cs2.exe'))
    if (!$processes[0].Path -or [IO.Path]::GetFullPath($processes[0].Path) -ine $expected) { throw 'Running CS2 belongs to a different installation than GameDirectory' }
    [IO.Directory]::CreateDirectory($Output) | Out-Null
    $stdout=Join-Path $Output 'stdout.txt';$stderr=Join-Path $Output 'stderr.txt'
    $arguments='-f hpp,json -o "'+$Output+'" -p cs2.exe -vv'
    $child=Start-Process -FilePath $path -ArgumentList $arguments -WorkingDirectory $Output -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $timeout=[Math]::Clamp([int]$config.dump_timeout_seconds,10,600)
    if (!$child.WaitForExit($timeout*1000)) {
        $child.Kill();$child.WaitForExit();throw 'Dumper timed out; see stdout.txt and stderr.txt in the report folder'
    }
    $child.Refresh()
    if ($child.ExitCode -ne 0) { throw 'Dumper failed; see stdout.txt and stderr.txt in the report folder' }
}
function Set-Constant([string]$File,[string]$Name,[uint32]$Value) {
    $source=$plan[$File]; if ($null -eq $source) { $source=Get-Content -LiteralPath (Join-Path $RepositoryRoot $File) -Raw }
    $expression='(\b'+[regex]::Escape($Name)+'\s*=\s*)(?:0x[0-9a-fA-F]+|\d+)(u?)\b'
    if ([regex]::Matches($source,$expression).Count -ne 1) { throw ('Cannot locate source constant '+$Name) }
    $source=[regex]::Replace($source,$expression,[Text.RegularExpressions.MatchEvaluator]{param($match) $match.Groups[1].Value+('0x{0:X}' -f $Value)+$match.Groups[2].Value})
    $plan[$File]=$source
}
function Test-Links($Function,[uint32]$Rva,$Resolved,$Images) {
    $pe=$Images[$Function.module].pe
    foreach ($link in $Function.links) {
        if ($Resolved.ContainsKey($link.function)) {
            if ($pe.Relative($Rva+[uint32]$link.offset,[int]$link.operand,[int]$link.length) -ne $Resolved[$link.function]) { return $false }
        }
    }
    return $true
}

if ($Mode -eq 'Watch') {
    # A foreground watcher is opt-in. The DLL launches a single background update instead.
    while ($true) {
        try {
            $root=Get-GameRoot;$key=Get-ModuleKey (Get-UpdaterImages $root)
            if ($key -ne $lastWatchKey) {
                $lastWatchKey=$key
                & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -Mode Update -RepositoryRoot $RepositoryRoot -GameDirectory $root
                if ($LASTEXITCODE -ne 0) { $lastWatchKey=''; Start-Sleep -Seconds 30 }
            }
        } catch { Write-Warning $_.Exception.Message }
        Start-Sleep -Seconds 5
    }
}
try {
    try { $lock=[IO.File]::Open((Join-Path $cacheRoot 'update.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None) }
    catch { Write-Host 'Another updater is already running.'; exit 0 }
    Set-UpdateStatus 'checking' 'Checking the installed game build...'
    $gameRoot=Get-GameRoot
    $images=Get-UpdaterImages $gameRoot
    $moduleKey=Get-ModuleKey $images
    $profile=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'build-recipes.json') -Raw | ConvertFrom-Json
    if ($profile.version -ne 1) { throw 'Unsupported binding recipe format' }
    $sourceKey=Get-UpdaterSourceKey $RepositoryRoot
    $buildFolder=Join-Path $cacheRoot ('cache\'+$moduleKey+'\'+$sourceKey)
    [IO.Directory]::CreateDirectory($buildFolder) | Out-Null
    $reportPath=Join-Path $buildFolder 'report.json'
    $cachePath=Join-Path $buildFolder 'verified-build.json'
    $known=$true
    foreach ($module in $profile.modules) { if ($images[$module.name].pe.Hash -ne $module.sha256) { $known=$false } }
    if ($Mode -eq 'Check') {
        if ($known) { Set-UpdateStatus 'current' 'The installed DLLs match the audited build.'; exit 0 }
        Set-UpdateStatus 'unsupported' 'A game update was detected. Run Update-Build.cmd to dump and validate it.'; exit 2
    }
    if (!$Force -and (Test-Path -LiteralPath $cachePath)) {
        $cached=Get-Content -LiteralPath $cachePath -Raw | ConvertFrom-Json
        $candidate=Assert-Within $cached.artifact $buildFolder
        if ($cached.module_key -eq $moduleKey -and $cached.source_key -eq $sourceKey -and
            (Test-Path -LiteralPath $candidate) -and (Get-FileHash -LiteralPath $candidate).Hash.ToLower() -eq $cached.artifact_sha256) {
            $artifact=$candidate;Set-UpdateStatus 'ready' 'A verified update is ready. Restart CS2 and load the updated DLL.'; exit 0
        }
    }
    if ($RuntimeRequest -and $known) {
        throw 'The DLL files match the audited build, but a runtime binding was rejected. This requires review rather than changing the build hash.'
    }
    if (!$Force -and $RuntimeRequest -and (Test-Path -LiteralPath $reportPath)) {
        $previous=Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
        if ($previous.state -eq 'review_required' -and [DateTime]::UtcNow - [DateTime]::Parse($previous.updated).ToUniversalTime() -lt [TimeSpan]::FromMinutes([int]$config.retry_minutes)) {
            Set-UpdateStatus 'review_required' 'This build still needs review. See the update report, or run Update-Build.cmd -Force after resolving it.'; exit 2
        }
    }
    $runFolder=Join-Path $buildFolder ('run-'+[Guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($runFolder) | Out-Null
    $output=$DumpDirectory
    if (!$output) { $output=Join-Path $runFolder 'dump';Set-UpdateStatus 'dumping' 'Dumping fresh offsets and interfaces...';Invoke-FreshDump $output $gameRoot }
    $output=(Resolve-Path -LiteralPath $output).Path
    foreach ($name in @('info.json','offsets.json','interfaces.json','client_dll.json','offsets.hpp','interfaces.hpp')) {
        if (!(Test-Path -LiteralPath (Join-Path $output $name))) { throw ('Dumper output is incomplete: missing '+$name) }
    }
    # The dumper may log analysis failures and still exit successfully. Validate its data.
    $info=Get-Content -LiteralPath (Join-Path $output 'info.json') -Raw | ConvertFrom-Json
    $offsets=Get-Content -LiteralPath (Join-Path $output 'offsets.json') -Raw | ConvertFrom-Json
    $interfaces=Get-Content -LiteralPath (Join-Path $output 'interfaces.json') -Raw | ConvertFrom-Json
    $classes=(Get-Content -LiteralPath (Join-Path $output 'client_dll.json') -Raw | ConvertFrom-Json).'client.dll'.classes
    Assert-DumpBuildIdentity $images['engine2.dll'].pe $offsets $info $gameRoot
    foreach ($module in @('client.dll','engine2.dll','inputsystem.dll','matchmaking.dll','soundsystem.dll')) {
        if ($null -eq $offsets.$module) { throw ('Dumper omitted offsets for '+$module) }
    }
    foreach ($name in @('dwEntityList','dwLocalPlayerController','dwLocalPlayerPawn','dwViewMatrix','dwGlobalVars','dwCSGOInput','dwViewAngles')) {
        $value=$offsets.'client.dll'.$name
        if ($null -eq $value -or !$value -or !$images['client.dll'].pe.Contains([uint32]$value,$false)) { throw ('Missing or invalid dumped offset '+$name) }
    }
    if ($offsets.'client.dll'.dwViewAngles-$offsets.'client.dll'.dwCSGOInput -ne 0x688) { throw 'Input layout changed; review CombatBindings before enabling this build' }
    foreach ($required in @(@('client.dll','Source2Client002'),@('resourcesystem.dll','ResourceSystem013'),@('tier0.dll','VEngineCvar007'),@('schemasystem.dll','SchemaSystem_001'))) {
        if ($null -eq $interfaces.($required[0]).($required[1])) { throw ('Dumper omitted interface '+$required[1]) }
    }
    if ((Get-ModuleKey (Get-UpdaterImages $gameRoot)) -ne $moduleKey) { throw 'Game DLLs changed during the dump. Retry after the update finishes.' }
    Set-UpdateStatus 'validating' 'Validating function bodies, callers, vtables and signatures...'
    $resolved=@{};$tables=@{};$plan=@{};$candidates=@{}
    foreach ($table in $profile.vtables) {
        try {
            $pe=$images[$table.module].pe
            $hits=@($pe.Vtables($table.type) | Where-Object {$pe.RttiOffset($_) -eq 0})
            if ($hits.Count -ne 1) { throw ('Expected one primary vtable; found '+$hits.Count) }
            $tables[$table.type]=[uint32]$hits[0]
            if ($table.slot -ge 0) {
                $rva=[long]$pe.Pointer($hits[0]+$table.slot*8)-[long]$pe.ImageBase
                $binding=$profile.functions | Where-Object name -eq $table.function
                if ($rva -lt 0 -or !(Test-Fingerprint $pe ([uint32]$rva) $binding.fingerprint)) { throw 'Vtable slot no longer matches its validated method' }
                if ($resolved.ContainsKey($table.function) -and $resolved[$table.function] -ne $rva) { throw 'Descriptor vtables disagree on their shared method' }
                $resolved[$table.function]=[uint32]$rva
            }
            if ($table.constant) { Set-Constant $table.file $table.constant $hits[0] }
        } catch { Add-UpdateFailure $table.type $_.Exception.Message }
    }
    foreach ($binding in $profile.functions) {
        if ($resolved.ContainsKey($binding.name)) { continue }
        $pe=$images[$binding.module].pe
        $sameModule=@($profile.modules | Where-Object {$_.name -eq $binding.module -and $_.sha256 -eq $pe.Hash}).Count -eq 1
        if ($sameModule -and (Test-Fingerprint $pe $binding.fingerprint.rva $binding.fingerprint)) { $resolved[$binding.name]=[uint32]$binding.fingerprint.rva;continue }
        $candidates[$binding.name]=@($pe.Find((Convert-HexPattern $binding.fingerprint.pattern),$true) | Where-Object {Test-Fingerprint $pe $_ $binding.fingerprint})
    }
    do {
        $progress=$false
        foreach ($binding in $profile.functions) {
            if ($resolved.ContainsKey($binding.name) -or !$candidates.ContainsKey($binding.name)) { continue }
            $valid=@($candidates[$binding.name] | Where-Object {Test-Links $binding $_ $resolved $images})
            if ($valid.Count -eq 1) { $resolved[$binding.name]=[uint32]$valid[0];$progress=$true }
        }
    } while ($progress)
    foreach ($binding in $profile.functions) {
        try {
            if (!$resolved.ContainsKey($binding.name)) { throw ('Could not select one verified function body; candidates: '+@($candidates[$binding.name]).Count) }
            $rva=$resolved[$binding.name];$pe=$images[$binding.module].pe
            if (!(Test-Links $binding $rva $resolved $images)) { throw 'A known caller/callee relationship changed' }
            if ($binding.header) {
                $file=$binding.header;$source=$plan[$file];if ($null -eq $source) { $source=Get-Content -LiteralPath (Join-Path $RepositoryRoot $file) -Raw }
                $regex='(?s)(inline\s+constexpr\s+(?:skin_bindings::)?Function\s+'+[regex]::Escape($binding.name)+'\s*)\{\s*(0x[0-9a-fA-F]+|\d+)u?\s*,\s*\{[^}]*\}\s*\}'
                if ([regex]::Matches($source,$regex).Count -ne 1) { throw 'Function declaration is missing or duplicated' }
                $bytes=($pe.Bytes($rva,24) | ForEach-Object {'0x{0:X2}' -f $_}) -join ', '
                $body=('{0}u, {{{1}}}' -f ('0x{0:X}' -f $rva),$bytes)
                $plan[$file]=[regex]::Replace($source,$regex,[Text.RegularExpressions.MatchEvaluator]{param($match) $match.Groups[1].Value+'{'+$body+'}'})
            }
        } catch { Add-UpdateFailure $binding.name $_.Exception.Message }
    }
    foreach ($reference in $profile.references) {
        try {
            if (!$resolved.ContainsKey($reference.function)) { throw 'Containing function is unresolved' }
            $pe=$images['client.dll'].pe
            $target=$pe.Relative($resolved[$reference.function]+[uint32]$reference.offset,[int]$reference.operand,[int]$reference.length)
            if (!$pe.Contains($target,$false) -or $pe.Contains($target,$true)) { throw 'Data reference does not point to module data' }
            Set-Constant $reference.file $reference.constant $target
        } catch { Add-UpdateFailure $reference.constant $_.Exception.Message }
    }
    try {
        $pe=$images['client.dll'].pe;$table=$tables['.?AVCSource2Client@@']
        $getter=[uint32]($pe.Pointer($table+129*8)-$pe.ImageBase)
        if ($pe.Bytes($getter,1)[0] -ne 0xE9 -or $pe.Relative($getter,1,5) -ne $resolved['EconomyGetter']) { throw 'Economy getter slot/thunk changed' }
        Set-Constant 'src\game\sdk\EconBindings.h' 'kSource2ClientInstanceRva' $interfaces.'client.dll'.Source2Client002
        $first=$tables['.?AVCAnimatableSceneObjectDesc@@'];$second=$tables['.?AVCBaseSceneObjectDesc@@']
        $file='src\game\sdk\ChamsBindings.h';$source=$plan[$file]
        $expression='(kModelDescriptorVtables\s*)\{[^}]*\}'
        if ([regex]::Matches($source,$expression).Count -ne 1) { throw 'Scene descriptor table declaration changed' }
        $plan[$file]=[regex]::Replace($source,$expression,[Text.RegularExpressions.MatchEvaluator]{param($match) $match.Groups[1].Value+(Format-CppRvaPair $first $second)})
    } catch { Add-UpdateFailure 'Economy/scene layout' $_.Exception.Message }
    $patternResults=@()
    foreach ($binding in $profile.patterns) {
        try {
            $pe=$images[$binding.module].pe;$parsed=Convert-Pattern $binding.expression
            $hits=$pe.Find($parsed.bytes,$false);$expression=$binding.expression
            if ($null -ne $binding.fingerprint) {
                $start=Find-Fingerprint $pe $binding.fingerprint
                $rva=[uint32]($start+$binding.fingerprint.match_offset)
                if ($hits.Count -ne 1 -or $hits[0] -ne $rva) {
                    # Recreate only a signature whose containing body and semantic anchors passed.
                    $actual=$pe.Bytes($rva,$parsed.bytes.Length)
                    for ($i=$parsed.bytes.Length-1;$i -ge 0;$i--) {
                        if ($parsed.bytes[$i] -ge 0) { $position=$parsed.positions[$i];$expression=$expression.Substring(0,$position)+('{0:X2}' -f $actual[$i])+$expression.Substring($position+2) }
                    }
                    $hits=$pe.Find((Convert-Pattern $expression).bytes,$false)
                }
            } elseif ($hits.Count -eq 1) { $rva=$hits[0] }
            if ($hits.Count -ne 1 -or $hits[0] -ne $rva) { throw ('Signature has '+$hits.Count+' matches after validation') }
            $target=Resolve-PatternTarget $pe $rva (Convert-Pattern $expression)
            $file=$binding.file;$source=$plan[$file];if ($null -eq $source) { $source=Get-Content -LiteralPath (Join-Path $RepositoryRoot $file) -Raw }
            $old='"'+$binding.module+':'+$binding.expression+'"';$new='"'+$binding.module+':'+$expression+'"'
            if ([regex]::Matches($source,[regex]::Escape($old)).Count -ne 1) { throw 'Original signature declaration changed; regenerate the audited recipes' }
            $plan[$file]=$source.Replace($old,$new)
            $patternResults+=[pscustomobject]@{expression=($binding.module+':'+$expression);match_rva=$rva;target_rva=$target;validated=$true}
        } catch { Add-UpdateFailure ($binding.module+':'+$binding.expression) $_.Exception.Message }
    }
    foreach ($export in $profile.exports) {
        try { $pe=$images[$export.module].pe;$rva=$pe.Export($export.name);if (!$rva -or !$pe.Contains($rva,$true)) { throw 'Required executable export is unavailable' } }
        catch { Add-UpdateFailure $export.name $_.Exception.Message }
    }
    try {
        $file='src\game\sdk\client_dll.hpp';$source=Get-Content -LiteralPath (Join-Path $RepositoryRoot $file) -Raw
        $expected=@{};foreach ($field in $profile.schema_fields) { $expected[$field.class+'::'+$field.field]=$true }
        $seen=@{}
        $source=[regex]::Replace($source,'(?s)(namespace\s+(\w+)\s*\{)([^{}]*)(\})',[Text.RegularExpressions.MatchEvaluator]{param($group)
            $class=$group.Groups[2].Value
            $body=[regex]::Replace($group.Groups[3].Value,'(constexpr\s+std::ptrdiff_t\s+(\w+)\s*=\s*)(0x[0-9a-fA-F]+)',[Text.RegularExpressions.MatchEvaluator]{param($field)
                $name=$field.Groups[2].Value
                $value=Get-DumpSchemaOffset $classes.$class.fields.$name ($class+'::'+$name)
                $seen[$class+'::'+$name]=$true
                $field.Groups[1].Value+('0x{0:X}' -f $value)
            })
            $group.Groups[1].Value+$body+$group.Groups[4].Value
        })
        foreach ($name in $expected.Keys) { if (!$seen.ContainsKey($name)) { throw ('Curated schema field disappeared: '+$name) } }
        $plan[$file]=$source
        $rules=@($patternResults | Where-Object {$_.expression -ceq 'client.dll:488B0D*????????4C897010'})
        if ($rules.Count -ne 1) { throw 'Game-rules global was not independently validated' }
        $offsets.'client.dll'.dwGameRules=$rules[0].target_rva
        if ($null -eq $offsets.'engine2.dll'.dwNetworkGameClient_isBackgroundMap) {
            $pe=$images['engine2.dll'].pe
            $hits=$pe.Find((Convert-HexPattern '0FB681????????C3CCCCCCCCCCCCCCCC0FB681????????C3CCCCCCCCCCCCCCCC4883EC'),$true)
            if ($hits.Count -ne 1) { throw 'Background-map getter was not uniquely identified' }
            $offsets.'engine2.dll' | Add-Member -NotePropertyName dwNetworkGameClient_isBackgroundMap -NotePropertyValue $pe.U32($hits[0]+3)
        }
        $interfaceProofs=(Get-Content -LiteralPath (Join-Path $PSScriptRoot 'bindings-audit.json') -Raw | ConvertFrom-Json).interface_proofs
        Add-VerifiedInterfaceFallbacks $interfaces $interfaceProofs $gameRoot
        foreach ($kind in @('offsets','interfaces')) {
            $file='src\game\sdk\'+$kind+'.hpp'
            $values=$offsets;if ($kind -eq 'interfaces') {$values=$interfaces}
            $plan[$file]=Get-UpdatedDumpHeader (Get-Content -LiteralPath (Join-Path $RepositoryRoot $file) -Raw) $values
        }
    } catch { Add-UpdateFailure 'Dumper schema/header validation' $_.Exception.Message }
    $report=[pscustomobject]@{version=1;state='validated';updated=[DateTime]::UtcNow.ToString('o');module_key=$moduleKey;source_key=$sourceKey;build_number=$info.build_number;dump=$output;functions=$resolved;signatures=$patternResults;failures=@($failures.ToArray())}
    if ($failures.Count) { $report.state='review_required';Write-UpdaterJson $reportPath $report;Set-UpdateStatus 'review_required' ('Automatic update needs review: '+$failures.Count+' binding(s) unresolved. See the report.');exit 2 }
    # New hash pins are written only after every required locator and layout proof passes.
    foreach ($file in Get-ChildItem (Join-Path $RepositoryRoot 'src') -Recurse -File | Where-Object {$_.Extension -in @('.h','.hpp','.cpp')}) {
        $relative=$file.FullName.Substring($RepositoryRoot.Length+1);$source=$plan[$relative]
        if ($null -eq $source) { $source=Get-Content -LiteralPath $file.FullName -Raw }
        $original=$source
        foreach ($module in $profile.modules) { $source=$source.Replace($module.sha256,$images[$module.name].pe.Hash) }
        if ($source -cne $original -or $plan.ContainsKey($relative)) { $plan[$relative]=$source }
    }
    $staging=Join-Path $runFolder 'source';[IO.Directory]::CreateDirectory($staging) | Out-Null
    Copy-UpdaterBuildSources $RepositoryRoot $staging
    foreach ($relative in $plan.Keys) {
        $destination=Assert-Within (Join-Path $staging $relative) $staging
        [IO.File]::WriteAllText($destination,$plan[$relative],[Text.UTF8Encoding]::new($false))
    }
    if ((Get-UpdaterSourceKey $RepositoryRoot) -ne $sourceKey) { throw 'Project source changed while staging the update. Retry.' }
    if ($NoBuild) {
        $report | Add-Member -NotePropertyName staged_source -NotePropertyValue $staging
        Write-UpdaterJson $reportPath $report;Set-UpdateStatus 'validated' 'Bindings validated and staged. Rebuild is pending.';exit 0
    }
    Set-UpdateStatus 'building' 'Bindings validated. Building the updated DLL...'
    $msbuild=Get-MsBuild;$log=Join-Path $runFolder 'build.txt'
    & $msbuild (Join-Path $staging 'Cs2 Internal.vcxproj') /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo *> $log
    if ($LASTEXITCODE -ne 0) { throw ('Rebuild failed. See '+$log) }
    if ((Get-ModuleKey (Get-UpdaterImages $gameRoot)) -ne $moduleKey) { throw 'Game DLLs changed during the rebuild; this artifact was not accepted.' }
    $artifact=Join-Path $staging 'DLL\DeltaEAC.dll'
    if (!(Test-Path -LiteralPath $artifact)) { throw 'Build succeeded without producing the expected DLL' }
    $hash=(Get-FileHash -LiteralPath $artifact).Hash.ToLower()
    $report | Add-Member -NotePropertyName artifact -NotePropertyValue $artifact
    $report | Add-Member -NotePropertyName artifact_sha256 -NotePropertyValue $hash
    $report.state='ready';Write-UpdaterJson $reportPath $report
    Write-UpdaterJson $cachePath ([pscustomobject]@{module_key=$moduleKey;source_key=$sourceKey;artifact=$artifact;artifact_sha256=$hash;report=$reportPath;updated=[DateTime]::UtcNow.ToString('o')})
    Set-UpdateStatus 'ready' 'A verified update is ready. Restart CS2 and load the updated DLL.'
    Write-Host ('Updated DLL: '+$artifact)
} catch {
    $reason=$_.Exception.Message
    if ($reportPath) { Write-UpdaterJson $reportPath ([pscustomobject]@{version=1;state='review_required';updated=[DateTime]::UtcNow.ToString('o');failures=@([pscustomobject]@{binding='Updater';reason=$reason})}) }
    Set-UpdateStatus 'review_required' $reason
    Write-Error $reason -ErrorAction Continue
    exit 2
} finally { if ($null -ne $lock) { $lock.Dispose() } }

param([switch]$CheckOnly)
$ErrorActionPreference='Stop'
$project=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$source=Join-Path $project 'DLL\DeltaEAC Updated.dll'
$destination=Join-Path $project 'DLL\DeltaEAC.dll'
$mirror=Join-Path $project 'bin\x64\Release\DeltaEAC.dll'
if(!(Test-Path -LiteralPath $source -PathType Leaf)){throw 'Build DeltaEAC Updated.dll first.'}
$sourceHash=(Get-FileHash -Algorithm SHA256 -LiteralPath $source).Hash
$loaded=Get-Process -Name cs2 -ErrorAction SilentlyContinue
if($CheckOnly){
    [pscustomobject]@{Source=$source;SHA256=$sourceHash;Destination=$destination;Ready=(!$loaded);Reason=$(if($loaded){'Close CS2 before applying the update.'}else{'Ready to apply.'})}
    return
}
if($loaded){throw 'Close CS2 first, then run Apply Latest DLL.cmd again. The game still has the old DLL loaded.'}
if(Test-Path -LiteralPath $destination -PathType Leaf){
    $oldHash=(Get-FileHash -Algorithm SHA256 -LiteralPath $destination).Hash
    $backupDirectory=Join-Path $project 'DLL\Backups'
    New-Item -ItemType Directory -Path $backupDirectory -Force | Out-Null
    $backup=Join-Path $backupDirectory ('DeltaEAC-'+$oldHash.Substring(0,12)+'.dll')
    if(!(Test-Path -LiteralPath $backup)){Copy-Item -LiteralPath $destination -Destination $backup}
}
Copy-Item -LiteralPath $source -Destination $destination -Force
New-Item -ItemType Directory -Path (Split-Path -Parent $mirror) -Force | Out-Null
Copy-Item -LiteralPath $source -Destination $mirror -Force
foreach($file in @($destination,$mirror)){
    if((Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash -ne $sourceHash){throw ('Copied DLL hash mismatch: '+$file)}
}
Write-Host 'Updated DLL\DeltaEAC.dll and the matching bin copy.'
Write-Host 'Your captured visual, movement, HVH and camera preferences are included.'
Write-Host 'Start CS2 and load DLL\DeltaEAC.dll.'

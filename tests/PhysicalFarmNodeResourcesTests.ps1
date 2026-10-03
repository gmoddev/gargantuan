#requires -Version 7.0
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'PhysicalFarmNodeResources.ps1')
$Root=Join-Path ([IO.Path]::GetTempPath()) ('farm-node-resources-'+[Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($Root)
$Path=Join-Path $Root 'node-resources.csv'; $ReceiptPath=Join-Path $Root 'node-run.json'
$RunId=[Guid]::NewGuid().ToString(); $Cases=0
function Observe {
	Read-FarmNodeResources -RunReceiptPath $ReceiptPath -RunReceiptSha256 (Get-FileHash $ReceiptPath).Hash -RunId $RunId
}
function Save {
	$script:Rows | Export-Csv -LiteralPath $Path -NoTypeInformation
	$script:Receipt.ResourceBytes=(Get-Item $Path).Length
	$script:Receipt.ResourceSamples=$script:Rows.Count
	$script:Receipt.ResourceSha256=(Get-FileHash $Path).Hash.ToLowerInvariant()
	$script:Receipt | ConvertTo-Json | Set-Content -LiteralPath $ReceiptPath
}
function Reject([scriptblock]$Mutation) {
	$script:Rows=$script:BaselineRows | ConvertFrom-Json
	$script:Receipt=$script:BaselineReceipt | ConvertFrom-Json -AsHashtable
	& $Mutation
	Save
	$Rejected=$false; try { $null=Observe } catch { $Rejected=$true }
	if (-not $Rejected) { throw 'invalid Node resource evidence accepted' }
	$script:Cases++
}
try {
	$Rows=@(0..2 | ForEach-Object { [ordered]@{
		RunId=$RunId;Pid=123;MonotonicTicks=1000000+1000000*$_;MonotonicFrequency=1000000
		Cpu100ns=100000+10000*$_;WorkingSetBytes=1000000+1000*$_;PrivateBytes=900000+1000*$_;Threads=4;Handles=20
	}})
	$Receipt=[ordered]@{Format='GargantuanFarmNodeRun';Version=1;RunId=$RunId;Pid=123;Reason='STOP_REQUESTED';ChildReaped=$true
		ResourceContract='node_process_resources_v1';ResourcePath=$Path;ResourceBytes=0;ResourceSamples=0;ResourceSha256=''}
	Save
	$BaselineRows=$Rows | ConvertTo-Json; $BaselineReceipt=$Receipt | ConvertTo-Json
	$Observed=Observe
	if ($Observed.State -cne 'MEASURED' -or $Observed.ObservedCpu100ns -ne 20000 -or
		$Observed.ObservedDurationMicroseconds -ne 2000000 -or $Observed.PeakWorkingSetBytes -ne 1002000 -or
		$Observed.HeadroomThreshold -cne 'NOT_DEFINED') { throw 'Node resource diagnostics wrong' }
	$Cases++
	Reject { $script:Rows[1].Pid=124 }
	Reject { $script:Rows[1].RunId='wrong' }
	Reject { $script:Rows[1].MonotonicTicks=1 }
	Reject { $script:Rows[1].MonotonicFrequency=2 }
	Reject { $script:Rows[1].Cpu100ns=1 }
	Reject { $script:Rows[1].WorkingSetBytes=0 }
	Reject { $script:Rows[1].Threads=0 }
	Reject { $script:Rows[1].PrivateBytes=-1 }
	Reject { $script:Rows[1].Cpu100ns='18446744073709551616' }
	Reject { $script:Receipt.ResourceContract='wrong' }
	Reject { $script:Receipt.ResourcePath=Join-Path $Root 'other.csv' }
	Reject { $script:Receipt.Reason='HARD_DEADLINE' }
	Reject { $script:Receipt.ChildReaped=$false }
	Reject { $script:Rows=@($script:Rows[0]) }
	$Rows=$BaselineRows | ConvertFrom-Json; $Receipt=$BaselineReceipt | ConvertFrom-Json -AsHashtable; Save
	[IO.File]::AppendAllText($Path,'tampered')
	$Rejected=$false;try{$null=Observe}catch{$Rejected=$true};if(-not $Rejected){throw 'unsealed CSV accepted'};$Cases++
	foreach($Name in @('ResourceContract','ResourcePath','ResourceBytes','ResourceSamples','ResourceSha256')){
		[void]$Receipt.Remove($Name)
	}
	$Receipt | ConvertTo-Json | Set-Content $ReceiptPath
	if((Observe).State -cne 'NOT_MEASURED'){throw 'historical absence promoted'};$Cases++
	$Receipt.ResourceContract='node_process_resources_v1';$Receipt|ConvertTo-Json|Set-Content $ReceiptPath
	$Rejected=$false;try{$null=Observe}catch{$Rejected=$true};if(-not $Rejected){throw 'partial resource receipt accepted'};$Cases++
	$Tokens=$null;$Errors=$null
	$Ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'PhysicalGameSessionFarmNode.ps1'),[ref]$Tokens,[ref]$Errors)
	if($Errors.Count){throw 'Node supervisor syntax failed'}
	$Function=$Ast.Find({param($N)$N -is [Management.Automation.Language.FunctionDefinitionAst] -and $N.Name -ceq 'Add-NodeResourceSample'},$true)
	. ([scriptblock]::Create($Function.Extent.Text))
	$Samples=[Collections.Generic.List[object]]::new();$Current=Get-Process -Id $PID
	Add-NodeResourceSample -Child $Current -RunId $RunId -Samples $Samples
	Add-NodeResourceSample -Child $Current -RunId $RunId -Samples $Samples
	if($Samples.Count -ne 2 -or $Samples[0].Pid -ne $PID -or $Samples[1].MonotonicTicks -le $Samples[0].MonotonicTicks){throw 'actual process sampling invalid'};$Cases++
	while($Samples.Count -lt 1202){$Samples.Add($Samples[0])}
	$Rejected=$false;try{Add-NodeResourceSample -Child $Current -RunId $RunId -Samples $Samples}catch{$Rejected=$true}
	if(-not $Rejected){throw 'sample cap ignored'};$Cases++
	Write-Output "[Qualification:NodeResources] PASS cases=$Cases"
}finally{
	$Resolved=[IO.Path]::GetFullPath($Root);$Temp=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/')
	if(-not $Resolved.StartsWith($Temp+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -or
		[IO.Path]::GetFileName($Resolved) -cnotmatch '^farm-node-resources-[a-f0-9]{32}$'){throw 'fixture cleanup escaped scratch'}
	Remove-Item -LiteralPath $Resolved -Recurse -Force
}

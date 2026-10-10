# Offline bounded Go provider process diagnostics, independently of Engine/clients.
function Read-FarmNodeResources {
	param([string]$RunReceiptPath, [string]$RunReceiptSha256, [string]$RunId)
	$RunFile = Get-Item -LiteralPath $RunReceiptPath -ErrorAction Stop
	if ($RunFile.Length -gt 1MB -or $RunFile.Attributes.HasFlag([IO.FileAttributes]::ReparsePoint) -or
		(Get-FileHash -LiteralPath $RunReceiptPath -Algorithm SHA256).Hash -ine $RunReceiptSha256) { throw 'Node resource receipt pin invalid' }
	$Run = Get-Content -LiteralPath $RunReceiptPath -Raw | ConvertFrom-Json -AsHashtable
	$Fields = @('ResourceContract','ResourcePath','ResourceBytes','ResourceSamples','ResourceSha256')
	$Present = @($Fields | Where-Object { $Run.Contains($_) }).Count
	if ($Run.RunId -cne $RunId) { throw 'Node resource run identity mismatch' }
	if ($Present -eq 0) { return [ordered]@{ State='NOT_MEASURED'; Reason='historical Node child resources absent' } }
	if ($Present -ne $Fields.Count -or $Run.ResourceContract -cne 'node_process_resources_v1' -or
		$Run.ResourceBytes -isnot [long] -or $Run.ResourceBytes -le 0 -or $Run.ResourceBytes -gt 1MB -or
		$Run.ResourceSamples -isnot [long] -or $Run.ResourceSamples -lt 2 -or $Run.ResourceSamples -gt 1202 -or
		$Run.ResourceSha256 -cnotmatch '^[a-f0-9]{64}$' -or $Run.Pid -isnot [long] -or $Run.Pid -le 0 -or
		$Run.Format -cne 'GargantuanFarmNodeRun' -or $Run.Version -ne 1 -or
		$Run.Reason -cne 'STOP_REQUESTED' -or $Run.ChildReaped -cne $true) { throw 'Node resource schema or owned-child result invalid' }
	$Path = [IO.Path]::GetFullPath([string]$Run.ResourcePath)
	if ($Path -ine (Join-Path $RunFile.DirectoryName 'node-resources.csv')) { throw 'Node resource file escaped run stage' }
	$File = Get-Item -LiteralPath $Path -ErrorAction Stop
	if ($File.PSIsContainer -or $File.Attributes.HasFlag([IO.FileAttributes]::ReparsePoint) -or
		$File.Length -ne $Run.ResourceBytes -or (Get-FileHash -LiteralPath $Path).Hash -ine $Run.ResourceSha256) { throw 'Node resource file size or hash mismatch' }
	$Rows = @(Import-Csv -LiteralPath $Path)
	if ($Rows.Count -ne $Run.ResourceSamples) { throw 'Node resource row count mismatch' }
	$Numeric = @('Pid','MonotonicTicks','MonotonicFrequency','Cpu100ns','WorkingSetBytes','PrivateBytes','Threads','Handles')
	$Previous = $null; $First = $null
	$Peak = @{WorkingSetBytes=0L;PrivateBytes=0L;Threads=0L;Handles=0L}
	foreach ($Row in $Rows) {
		if (($Row.PSObject.Properties.Name -join ',') -cne ('RunId,' + ($Numeric -join ',')) -or $Row.RunId -cne $RunId) { throw 'Node resource CSV schema or identity mismatch' }
		$Value = @{}
		foreach ($Name in $Numeric) {
			[long]$Number = 0
			if ([string]$Row.$Name -cnotmatch '^(0|[1-9][0-9]*)$' -or -not [long]::TryParse($Row.$Name,[ref]$Number)) { throw 'Node resource numeric field invalid' }
			$Value[$Name] = $Number
		}
		if ($Value.Pid -ne $Run.Pid -or $Value.MonotonicTicks -le 0 -or $Value.MonotonicFrequency -le 0 -or
			$Value.WorkingSetBytes -le 0 -or $Value.Threads -le 0) { throw 'Node process resource identity/range invalid' }
		if ($Previous -and ($Value.MonotonicFrequency -ne $Previous.MonotonicFrequency -or
			$Value.MonotonicTicks -le $Previous.MonotonicTicks -or $Value.Cpu100ns -lt $Previous.Cpu100ns)) { throw 'Node process resource chronology regressed' }
		foreach ($Name in @('WorkingSetBytes','PrivateBytes','Threads','Handles')) { $Peak[$Name] = [Math]::Max($Peak[$Name],$Value[$Name]) }
		if (-not $First) { $First = $Value }
		$Previous = $Value
	}
	return [ordered]@{ State='MEASURED'; Scope='NODE_PROVIDER_PROCESS_DIAGNOSTICS'; RunId=$RunId; Pid=$Run.Pid;
		Samples=$Rows.Count; ResourceSha256=$Run.ResourceSha256; NodeRunReceiptSha256=$RunReceiptSha256.ToLowerInvariant();
		AnalyzerSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash.ToLowerInvariant();
		ObservedCpu100ns=$Previous.Cpu100ns-$First.Cpu100ns;
		ObservedDurationMicroseconds=[decimal]($Previous.MonotonicTicks-$First.MonotonicTicks)*1000000/$First.MonotonicFrequency;
		PeakWorkingSetBytes=$Peak.WorkingSetBytes; PeakPrivateBytes=$Peak.PrivateBytes;
		PeakThreads=$Peak.Threads; PeakHandles=$Peak.Handles;
		HeadroomThreshold='NOT_DEFINED'; Sampling='one second plus final pre-termination observation; transient peaks may be missed' }
}

param(
	[Parameter(Mandatory = $true)][string[]]$InputFiles,
	[Parameter(Mandatory = $true)][string]$OutputFile
)
$ErrorActionPreference = 'Stop'
$Rows = [System.Collections.Generic.List[object]]::new()
foreach ($File in $InputFiles) {
	$Remote = @{}
	foreach ($Line in Get-Content -LiteralPath $File) {
		if ($Line -notmatch '\[Content:(Scale|ScaleRemote)\]') { continue }
		$Kind = $Matches[1]
		$Fields = [ordered]@{ Source = [System.IO.Path]::GetFileName($File) }
		foreach ($Pair in [regex]::Matches($Line, '([A-Za-z][A-Za-z0-9_]*)=([^\s]+)')) {
			$Number = 0.0
			$Value = $Pair.Groups[2].Value
			if ([double]::TryParse($Value, [System.Globalization.NumberStyles]::Float,
				[System.Globalization.CultureInfo]::InvariantCulture, [ref]$Number)) { $Value = $Number }
			$Fields[$Pair.Groups[1].Value] = $Value
		}
		if (-not $Fields.Contains('phase')) { continue }
		if ($Kind -eq 'ScaleRemote') { $Remote[$Fields.phase] = $Fields; continue }
		if ($Remote.ContainsKey($Fields.phase)) { $Fields['RemoteFunction'] = $Remote[$Fields.phase] }
		$Rows.Add($Fields)
	}
}
$Rows.ToArray() | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $OutputFile -Encoding utf8
Write-Output "[Content:ScaleAnalysis] phaseRows=$($Rows.Count) output=$OutputFile"

#requires -Version 7.0
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'RecoveryCausalEvidence.ps1')
. (Join-Path $PSScriptRoot 'RecoveryCausalEvidenceFixture.ps1')
$Root = Join-Path ([IO.Path]::GetTempPath()) ('recovery-causal-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($Root)
$Run = [Guid]::NewGuid().ToString(); $Path = Join-Path $Root 'recovery-gameplay.tsv'
$Count = 0
function Check-Replay {
	return Assert-RecoveryCausalEvidence -Path $Path -RunId $Run -Case gameplay `
		-ExpectedConnections @(1..32 | ForEach-Object { "$_`:1" }) -CessationMicroseconds 1000000 -JournalFence 100
}
function Check-Invalid([string[]]$Rows, [string]$Name) {
	[IO.File]::WriteAllLines($Path, $Rows)
	$Rejected = $false
	try { [void](Check-Replay) } catch { $Rejected = $true }
	if (-not $Rejected) { throw "causal replay accepted tampering: $Name" }
	$script:Count++
}
function Change-Field([string[]]$Rows, [int]$Row, [int]$Field, [string]$Value) {
	$Copy = [string[]]$Rows.Clone(); $Fields = $Copy[$Row].Split("`t"); $Fields[$Field] = $Value
	$Copy[$Row] = $Fields -join "`t"; return ,$Copy
}
function Event-Row([int]$Kind, [int]$Reason, [long]$Time, [int]$Token = 0, [int]$Replacement = 0, [string[]]$Extra = @()) {
	return (@('event', $Kind, $Reason, $Time, 1, 1, 3, 0, 0, 100, 100, 0, 0, 0, 0, $Token, $Replacement, 200, 1, 1, 100, 1, 1, 177) + $Extra) -join "`t"
}
try {
	[void](Save-RecoveryCausalFixture -Root $Root -RunId $Run -Case gameplay -Tail 100 -CessationMicroseconds 1000000)
	$Original = [IO.File]::ReadAllLines($Path)
	$Good = Check-Replay
	if ($Good.EventCount -ne 128 -or $Good.QuotedFrames -ne 32 -or $Good.Totals.retired -ne 2464 -or $Good.EarliestConvergedUs -ne 1127) { throw 'valid causal fixture failed' }
	$Count++
	foreach ($Mutation in @(
		@(34, 8, '78', 'accepted bytes'), @(34, 11, '9', 'accepted hash'), @(34, 7, '0', 'accepted token'),
		@(34, 6, '4', 'accepted sequence'), @(35, 13, '76', 'first-send below ACK'), @(35, 14, '76', 'retirement before ACK'),
		@(35, 23, '101', 'wrong cumulative base'), @(35, 21, '2', 'wrong source generation'),
		@(35, 5, '2', 'wrong peer generation'), @(33, 9, '98', 'cursor coverage gap'),
		@(33, 22, '0', 'invalid source event'), @(33, 11, '0', 'partial hash'),
		@(36, 8, '76', 'partial retirement'), @(33, 3, '999999', 'pre-capture event'),
		@(193, 3, '99', 'source cursor mismatch'), @(193, 4, '2', 'source sequence mismatch'),
		@(161, 3, '3', 'reference sequence'), @(161, 4, '0', 'zero reference bytes'),
		@(161, 8, '101', 'reference exceeds fence'), @(225, 1, '127', 'event count'),
		@(1, 16, '99', 'captured cumulative conservation'), @(1, 20, '1', 'unresolved initial planning')
	)) {
		# A single nonzero half is legal; make this a truly missing fingerprint.
		$Rows = Change-Field $Original $Mutation[0] $Mutation[1] $Mutation[2]
		if ($Mutation[3] -eq 'partial hash') { $Rows = Change-Field $Rows 33 12 '0' }
		Check-Invalid $Rows $Mutation[3]
	}
	Check-Invalid @($Original[0..34] + $Original[36..225]) 'missing delivery event'
	Check-Invalid @($Original[0..192] + $Original[194..225]) 'missing final source'
	Check-Invalid (Change-Field $Original 225 2 '1') 'native sink overflow'
	Check-Invalid (Change-Field $Original 33 11 '18446744073709551616') 'uint64 overflow'
	# A later pending obligation does not reopen the immutable completed prefix.
	$Extra = @(Event-Row 4 0 1002000 2; Event-Row 4 0 1002001 1)
	$Live = @($Original[0..160] + $Extra + $Original[161..225])
	$Live[195] += "`t2`t1"; $Live[-1] = "end`t130`t0"
	[IO.File]::WriteAllLines($Path, $Live); $Result = Check-Replay
	if ($Result.Peers['1:1'].Pending.Count -ne 2 -or $Result.Peers['1:1'].Prefix.retired -ne 77) { throw 'later live/out-of-order token work corrupted prefix' }
	$Count++
	Check-Invalid @($Original[0..160] + $Extra + $Original[161..225]) 'missing live source tokens'
	# Reusing even a cancelled token is invalid; callbacks may arrive out of token order.
	$Bad = @($Original[0..160] + @(Event-Row 4 0 1002000 1; Event-Row 5 2 1002001 1; Event-Row 4 0 1002002 1) + $Original[161..225])
	$Bad[-1] = "end`t131`t0"; Check-Invalid $Bad 'reused cancelled pending token'
	# Planning at t0 cannot disappear through an unrelated frame; a valid installation closes it.
	$Planning = Change-Field $Original 1 20 '1'
	$Install = Event-Row 10 0 1002000
	$Planning = @($Planning[0..160] + $Install + $Planning[161..225]); $Planning[-1] = "end`t129`t0"
	[IO.File]::WriteAllLines($Path, $Planning); $Result = Check-Replay
	if ($Result.EarliestConvergedUs -ne 2000) { throw 'planning barrier did not survive until installation' }
	$Count++
	# Decimal parsing preserves the complete uint64 fingerprint, including values above double precision.
	$BigHash = [string[]]$Original.Clone()
	foreach ($Row in @(33, 34, 35)) { $BigHash = Change-Field $BigHash $Row 11 '18446744073709551615' }
	[IO.File]::WriteAllLines($Path, $BigHash); [void](Check-Replay); $Count++
	# An accepted later live grant may still be awaiting native first-send/ACK/retirement.
	$Later = @(
		(@('event', 0, 0, 1002000, 1, 1, 3, 0, 77, 100, 101, 3, 4, 0, 0, 0, 0, 0, 0, 0, 100, 1, 1, 177) -join "`t"),
		(@('event', 1, 0, 1002001, 1, 1, 3, 33, 77, 100, 101, 3, 4, 0, 0, 0, 0, 0, 0, 0, 100, 1, 1, 177) -join "`t"),
		(@('event', 8, 0, 1002002, 1, 1, 3, 33, 77, 0, 0, 3, 4, 20, 0, 0, 0, 0, 0, 0, 100, 1, 1, 177) -join "`t")
	)
	$LaterRows = @($Original[0..160] + $Later + $Original[161..225])
	$LaterRows[196] = "source`t1`t1`t101`t4"; $LaterRows[-1] = "end`t131`t0"
	[IO.File]::WriteAllLines($Path, $LaterRows); $LaterResult = Check-Replay
	if ($LaterResult.Totals.accepted -ne 2541 -or $LaterResult.Totals.first_sent -ne 2484 -or $LaterResult.Peers['1:1'].Prefix.retired -ne 77) { throw 'later debt polluted the baseline prefix' }
	$Count++
	$PreparedRows = @($Original[0..160] + $Later[0] + $Original[161..225]); $PreparedRows[-1] = "end`t129`t0"
	[IO.File]::WriteAllLines($Path, $PreparedRows); [void](Check-Replay); $Count++
	# A replaced pending token retains the original baseline obligation until accepted.
	$Replacement = Change-Field $Original 1 8 '2'; $Replacement[1] += "`t1:200:1:1"
	$Replacement[33] += "`tp:2:200:1:1`te:200:1"
	$Replacement = @($Replacement[0..32] + (Event-Row 6 4 1001000 1 2) + $Replacement[33..225])
	$Replacement[-1] = "end`t129`t0"
	[IO.File]::WriteAllLines($Path, $Replacement); [void](Check-Replay); $Count++
	$Unresolved = [string[]]$Replacement.Clone(); $Unresolved[34] = $Original[33]
	Check-Invalid $Unresolved 'replacement silently erased baseline obligation'
	# t0 accepted debt is seeded independently of later accepted-frame events.
	$DebtRows = [string[]]$Original.Clone()
	$DebtRows[1] = (@('capture', 1, 1, 100, 1, 99, 100, 2, 1, 1000, 1, 77, 1, 2, 20, 10, 177, 120, 110, 100, 0) -join "`t")
	$DebtRows[33] = (@('event', 3, 1, 1001000, 1, 1, 0, 0, 0, 99, 100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 100, 1, 1, 0) -join "`t")
	$DebtRows = Change-Field $DebtRows 35 6 '1'; $DebtRows = Change-Field $DebtRows 35 7 '1000'; $DebtRows = Change-Field $DebtRows 36 7 '1000'
	$DebtRows[193] = "source`t1`t1`t100`t2"
	$DebtRows = @($DebtRows[0..33] + $DebtRows[35..225]); $DebtRows[-1] = "end`t127`t0"
	[IO.File]::WriteAllLines($Path, $DebtRows); $Debt = Check-Replay
	if ($Debt.Totals.retired -ne 2464 -or $Debt.Peers['1:1'].InitialDebt -ne 77 -or $Debt.Peers['1:1'].Cut -ne 1000) { throw 'captured debt failed exact conservation' }
	$Count++
	Write-Output "Recovery causal replay tests passed: $Count"
} finally {
	$Resolved = [IO.Path]::GetFullPath($Root)
	if (-not $Resolved.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()), [StringComparison]::OrdinalIgnoreCase)) { throw 'temporary fixture cleanup escaped temp root' }
	Remove-Item -LiteralPath $Resolved -Recurse -Force
}

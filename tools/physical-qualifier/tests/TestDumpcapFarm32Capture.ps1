# No dumpcap/NIC operation: fixed-profile source and preexisting-file denial.
$ErrorActionPreference = 'Stop'
$ScriptPath = Join-Path $PSScriptRoot '..\DumpcapFarm32Capture.ps1'
$Text = [IO.File]::ReadAllText([IO.Path]::GetFullPath($ScriptPath))
function Assert([bool]$Value, [string]$Detail) { if (-not $Value) { throw $Detail } }
$Tokens = $null; $Errors = $null
$Ast = [System.Management.Automation.Language.Parser]::ParseInput($Text, [ref]$Tokens, [ref]$Errors)
Assert ($Errors.Count -eq 0) 'Farm32 dumpcap script does not parse'
Assert ($Text -match '(?m)^\$CaptureSeconds = 600\r?$') 'Farm32 duration changed'
Assert ($Text -match '(?m)^\$AutostopKilobytes = 16777216\r?$') 'Farm32 filesize autostop changed'
Assert ($Text -match '(?m)^\$CompletenessBytes = 15GB\r?$') 'Farm32 completeness threshold changed'
Assert ($Text -match '(?m)^\$ReservedBytes = 18GB\r?$') 'Farm32 client disk reserve changed'
Assert ($Text.Contains("'Farm32Capture16GiB-v2'")) 'Farm32 versioned profile missing'
Assert ($Text -match "'udp port 39450 and host 10\.253\.3\.2'") 'Farm32 fixed UDP filter changed'
$ExpectedAutostop = @'
'-a', "duration:$CaptureSeconds", '-a', "filesize:$AutostopKilobytes"
'@.Trim()
Assert ($Text.Contains($ExpectedAutostop)) 'Farm32 dual autostop missing'
Assert ($Text -cnotmatch "'-b'|--ring-buffer") 'Farm32 capture must not rotate or overwrite'
Assert ($Text -cnotmatch "'-t'|'-C'|'-N'") 'Farm32 capture threading or queue policy changed'

# Evaluate only the argument array, never the capture script or its commands.
# This checks the actual ProcessStartInfo input without a NIC/dumpcap operation.
$BufferAssignment = $Ast.Find({ param($Node)
    $Node -is [System.Management.Automation.Language.AssignmentStatementAst] -and
        $Node.Left.Extent.Text -ceq '$RequestedBufferMiB'
}, $true)
Assert ($null -ne $BufferAssignment) 'Farm32 requested buffer declaration missing'
$RequestedBufferMiB = & ([scriptblock]::Create($BufferAssignment.Right.Extent.Text))
Assert ($RequestedBufferMiB -eq 64) 'Farm32 requested capture buffer changed'
Assert ($Text -match 'RequestedBufferMiB = \$RequestedBufferMiB') 'Requested buffer is absent from capture marker'
$ArgumentLoop = $Ast.Find({ param($Node)
    $Node -is [System.Management.Automation.Language.ForEachStatementAst] -and
        $Node.Variable.Extent.Text -ceq '$Argument'
}, $true)
Assert ($null -ne $ArgumentLoop) 'Farm32 process argument array missing'
$Device = '\Device\NPF_{TEST}'; $Pcap = 'test.pcapng'
$MarkerValue = @{ Filter = 'udp port 39450 and host 10.253.3.2' }
$CaptureSeconds = 600; $AutostopKilobytes = 16777216
$Arguments = @(& ([scriptblock]::Create($ArgumentLoop.Condition.Extent.Text)))
$ExpectedArguments = @('-i', $Device, '-B', '64', '-f', $MarkerValue.Filter,
    '-a', 'duration:600', '-a', 'filesize:16777216', '-w', $Pcap, '-q')
Assert (($Arguments -join "`n") -ceq ($ExpectedArguments -join "`n")) 'Farm32 exact capture command differs'

# Hosted runner temp paths can be junctions. Use the checkout-local test directory
# so this case reaches the artifact guard instead of the separate reparse guard.
$FixtureParent = [IO.Path]::GetFullPath($PSScriptRoot).TrimEnd([IO.Path]::DirectorySeparatorChar)
$Root = Join-Path $FixtureParent ('farm32-dumpcap-test-' + [guid]::NewGuid())
[void][IO.Directory]::CreateDirectory($Root)
try {
    $RunId = [guid]::NewGuid().ToString('D')
    $RunDir = Join-Path $Root $RunId
    [void][IO.Directory]::CreateDirectory($RunDir)
    $Existing = Join-Path $RunDir 'farm32-client-capture.pcapng'
    [IO.File]::WriteAllText($Existing, 'preserved')
    $Rejected = $false
    $RejectionDetail = '<no exception>'
    try {
        & $ScriptPath -EvidenceRoot $Root -RunId $RunId -DumpcapPath 'C:\missing-dumpcap.exe' `
            -DumpcapSha256 ('0' * 64) | Out-Null
    } catch {
        $RejectionDetail = $_.Exception.Message
        $Rejected = $RejectionDetail -ceq "Farm32 client capture artifact already exists: $Existing"
    }
    Assert $Rejected "Preexisting Farm32 pcap was not rejected at the artifact guard. Actual: $RejectionDetail"
    Assert (([IO.File]::ReadAllText($Existing)) -eq 'preserved') 'Preexisting Farm32 pcap changed'
    Assert (-not (Test-Path -LiteralPath (Join-Path $RunDir 'farm32-client-capture.json'))) 'Denied capture created ownership state'
    'DUMPCAP_FARM32_MOCK_OK'
} finally {
    $Resolved = [IO.Path]::GetFullPath($Root)
    if ((Split-Path $Resolved -Parent) -cne $FixtureParent -or
        (Split-Path $Resolved -Leaf) -notlike 'farm32-dumpcap-test-*') {
        throw 'Unsafe Farm32 test cleanup target.'
    }
    Remove-Item -LiteralPath $Resolved -Recurse -Force
}

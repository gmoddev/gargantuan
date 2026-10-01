# No dumpcap/NIC operation: fixed-profile source and preexisting-file denial.
$ErrorActionPreference = 'Stop'
$ScriptPath = Join-Path $PSScriptRoot '..\DumpcapFarm32Capture.ps1'
$Text = [IO.File]::ReadAllText([IO.Path]::GetFullPath($ScriptPath))
function Assert([bool]$Value, [string]$Detail) { if (-not $Value) { throw $Detail } }
$Tokens = $null; $Errors = $null
[System.Management.Automation.Language.Parser]::ParseInput($Text, [ref]$Tokens, [ref]$Errors) | Out-Null
Assert ($Errors.Count -eq 0) 'Farm32 dumpcap script does not parse'
Assert ($Text -match '(?m)^\$CaptureSeconds = 600\r?$') 'Farm32 duration changed'
Assert ($Text -match '(?m)^\$AutostopKilobytes = 1048576\r?$') 'Farm32 filesize autostop changed'
Assert ($Text -match '(?m)^\$CompletenessBytes = 960MB\r?$') 'Farm32 completeness threshold changed'
Assert ($Text -match "'udp port 39450 and host 10\.253\.3\.2'") 'Farm32 fixed UDP filter changed'
$ExpectedAutostop = @'
'-a', "duration:$CaptureSeconds", '-a', "filesize:$AutostopKilobytes"
'@.Trim()
Assert ($Text.Contains($ExpectedAutostop)) 'Farm32 dual autostop missing'
Assert ($Text -notmatch "'-b'|--ring-buffer") 'Farm32 capture must not rotate or overwrite'

$Root = Join-Path ([IO.Path]::GetTempPath()) ('farm32-dumpcap-test-' + [guid]::NewGuid())
[void][IO.Directory]::CreateDirectory($Root)
try {
    $RunId = [guid]::NewGuid().ToString('D')
    $RunDir = Join-Path $Root $RunId
    [void][IO.Directory]::CreateDirectory($RunDir)
    $Existing = Join-Path $RunDir 'farm32-client-capture.pcapng'
    [IO.File]::WriteAllText($Existing, 'preserved')
    $Rejected = $false
    try {
        & $ScriptPath -EvidenceRoot $Root -RunId $RunId -DumpcapPath 'C:\missing-dumpcap.exe' `
            -DumpcapSha256 ('0' * 64) | Out-Null
    } catch { $Rejected = $_.Exception.Message -match 'already exists' }
    Assert $Rejected 'Preexisting Farm32 pcap was accepted'
    Assert (([IO.File]::ReadAllText($Existing)) -eq 'preserved') 'Preexisting Farm32 pcap changed'
    Assert (-not (Test-Path -LiteralPath (Join-Path $RunDir 'farm32-client-capture.json'))) 'Denied capture created ownership state'
    'DUMPCAP_FARM32_MOCK_OK'
} finally {
    $Resolved = [IO.Path]::GetFullPath($Root)
    if ((Split-Path $Resolved -Parent) -ne ([IO.Path]::GetTempPath().TrimEnd('\')) -or
        (Split-Path $Resolved -Leaf) -notlike 'farm32-dumpcap-test-*') {
        throw 'Unsafe Farm32 test cleanup target.'
    }
    Remove-Item -LiteralPath $Resolved -Recurse -Force
}

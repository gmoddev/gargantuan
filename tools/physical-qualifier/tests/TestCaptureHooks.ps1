# Control-only simulation: functions shadow Windows cmdlets; no NIC/capture changes.
param([string]$HookPath)
$ErrorActionPreference = 'Stop'
$Hook = if ($HookPath) { $HookPath } else { Join-Path $PSScriptRoot '..\worker\PktMonCapture.ps1' }
$HookBlock = [ScriptBlock]::Create([IO.File]::ReadAllText($Hook))
$TestDir = Join-Path ([IO.Path]::GetTempPath()) ('qualifier-hook-test-' + [Guid]::NewGuid())
New-Item -ItemType Directory -Path $TestDir | Out-Null
$global:TestActive = $false
$global:TestFilters = "Packet Filters:`n    None"
$global:TestEtl = ''
$global:TestCalls = [System.Collections.Generic.List[string]]::new()
$global:TestInventory = '[{"Components":[{"Name":"Mellanox ConnectX-4 Lx Ethernet Adapter","Type":"Miniport","Id":13,"DriverName":"mlx5.sys","Properties":[{"Name":"ifIndex","Value":19}]},{"Name":"TCP/IPv4 - L2","Type":"Protocol","Id":75,"SecondaryId":81,"DriverName":"tcpip.sys","Properties":[{"Name":"Miniport ifIndex","Value":19}]}]}]'
function global:pktmon {
    param([Parameter(ValueFromRemainingArguments=$true)][string[]]$Arguments)
    $global:LASTEXITCODE = 0
    $global:TestCalls.Add(($Arguments -join ' '))
    switch ($Arguments[0]) {
        'status' { if ($global:TestActive) { "Packet Monitor is running.`nLog file: $global:TestEtl" } else { 'Packet Monitor is not running.' } }
        'filter' {
            switch ($Arguments[1]) {
                'list' { $global:TestFilters }
                'add' { $global:TestFilters = 'Packet Filters: ' + $Arguments[2] }
                'remove' { $global:TestFilters = "Packet Filters:`n    None" }
                default { throw 'Unexpected filter command' }
            }
        }
        'list' { $global:TestInventory }
        'start' {
            $global:TestEtl = $Arguments[[Array]::IndexOf($Arguments,'--file-name') + 1]
            [IO.File]::WriteAllBytes($global:TestEtl,[byte[]]@(1,2,3))
            $global:TestActive = $true
        }
        'stop' { $global:TestActive = $false }
        'etl2pcap' { [IO.File]::WriteAllBytes($Arguments[3],[byte[]]@(4,5,6)) }
        default { throw 'Unexpected Packet Monitor command' }
    }
}
function global:Get-NetAdapter { [pscustomobject]@{ifIndex=19;Status='Up';LinkSpeed='10 Gbps';MacAddress='0C-42-A1-52-38-A8'} }
function global:Get-NetIPAddress { [pscustomobject]@{IPAddress='10.253.3.2';PrefixLength=30} }
function global:Get-NetIPInterface { [pscustomobject]@{NlMtu=1500;Dhcp='Disabled'} }
function Assert([bool]$Value,[string]$Detail) { if (!$Value) { throw $Detail } }
try {
    & $HookBlock $TestDir Start
    Assert $global:TestActive 'Capture did not start'
    Assert ([bool]($global:TestCalls | Where-Object {$_ -eq ('start --capture --comp 13 --pkt-size 0 --file-name ' + $global:TestEtl + ' --file-size 64 --log-mode circular')})) 'Single Mellanox edge or 64 MiB bound was not preserved'
    Assert ([bool]($global:TestCalls | Where-Object {$_ -eq ('filter add Qualification-' + (Split-Path $TestDir -Leaf) + ' -t UDP -i 10.253.3.1 10.253.3.2 -p 39450')})) 'The peer-address/UDP-port filter changed'
    $Owner = Get-Content (Join-Path $TestDir 'pktmon-owner.json') -Raw | ConvertFrom-Json
    Assert (($Owner.Components -join ',') -eq '13' -and $Owner.ComponentId -eq 13 -and $Owner.MiniportIfIndex -eq 19 -and $Owner.CapturePort -eq 39450) 'Capture ownership marker omitted the selected miniport/default port'
    Assert (($Owner.CaptureLayers -join ',') -eq 'Mellanox miniport') 'Capture layer attribution was incomplete'
    & $HookBlock $TestDir Stop
    Assert (!$global:TestActive) 'Capture did not stop'
    Assert ($global:TestFilters -match 'None') 'Owned filter not removed'
    Assert (Test-Path (Join-Path $TestDir 'worker-capture.pcapng')) 'Export missing'
    Assert ([bool]($global:TestCalls | Where-Object {$_ -eq ('etl2pcap ' + $global:TestEtl + ' --out ' + (Join-Path $TestDir 'worker-capture.pcapng') + ' --component-id 13')})) 'Export did not preserve the selected miniport scope'
    & $HookBlock $TestDir Start 39452
    Assert ([bool]($global:TestCalls | Where-Object {$_ -eq ('filter add Qualification-' + (Split-Path $TestDir -Leaf) + ' -t UDP -i 10.253.3.1 10.253.3.2 -p 39452')})) 'The fixed local synthetic port was not selected'
    & $HookBlock $TestDir Stop
    $Before = $global:TestCalls.Count
    $Rejected = $false
    try { & $HookBlock $TestDir Start 39453 } catch { $Rejected=$true }
    Assert $Rejected 'An unapproved capture port was accepted'
    Assert ($global:TestCalls.Count -eq $Before) 'Invalid capture port invoked Packet Monitor'
    $global:TestActive = $true
    $Before = $global:TestCalls.Count
    $Rejected = $false
    try { & $HookBlock $TestDir Start } catch { $Rejected=$true }
    Assert $Rejected 'Existing session was not protected'
    Assert ($global:TestCalls.Count -eq $Before + 1) 'Existing session was mutated'
    $global:TestActive = $false
    $global:TestFilters = 'Packet Filters: unrelated'
    $Rejected = $false
    try { & $HookBlock $TestDir Start } catch { $Rejected=$true }
    Assert $Rejected 'Existing filters were not protected'
    $global:TestFilters = 'Packet Filters: changed-during-run'
    $Rejected = $false
    try { & $HookBlock $TestDir Stop } catch { $Rejected=$true }
    Assert $Rejected 'Changed filter ownership was not protected'
    Assert ($global:TestFilters -eq 'Packet Filters: changed-during-run') 'Changed filters were removed'
    $global:TestFilters = "Packet Filters:`n    None"
    $global:TestInventory = '[{"Components":[{"Name":"Mellanox ConnectX-4 Lx Ethernet Adapter","Type":"Miniport","Id":13,"DriverName":"wrong.sys","Properties":[{"Name":"ifIndex","Value":19}]}]}]'
    $Before = $global:TestCalls.Count
    $Rejected = $false
    try { & $HookBlock $TestDir Start } catch { $Rejected=$true }
    Assert $Rejected 'Wrong miniport driver was accepted'
    Assert ($global:TestCalls.Count -eq $Before + 3) 'Wrong-driver refusal proceeded beyond inventory validation'
    $global:TestInventory = '[{"Components":[{"Name":"Mellanox A","Type":"Miniport","Id":13,"DriverName":"mlx5.sys","Properties":[{"Name":"ifIndex","Value":19}]},{"Name":"Mellanox B","Type":"Miniport","Id":14,"DriverName":"mlx5.sys","Properties":[{"Name":"ifIndex","Value":19}]}]}]'
    $Before = $global:TestCalls.Count
    $Rejected = $false
    try { & $HookBlock $TestDir Start } catch { $Rejected=$true }
    Assert $Rejected 'Ambiguous miniport inventory was accepted'
    Assert ($global:TestCalls.Count -eq $Before + 3) 'Ambiguous-inventory refusal proceeded beyond validation'
    Write-Output 'Capture hook simulation passed: exact argv, cleanup, ownership refusal.'
} finally {
    Remove-Item Function:\pktmon,Function:\Get-NetAdapter,Function:\Get-NetIPAddress,Function:\Get-NetIPInterface
    $Resolved = [IO.Path]::GetFullPath($TestDir)
    if ((Split-Path $Resolved -Parent) -ne ([IO.Path]::GetTempPath().TrimEnd('\')) -or (Split-Path $Resolved -Leaf) -notlike 'qualifier-hook-test-*') {
        throw 'Refusing cleanup outside the generated test directory.'
    }
    Remove-Item -LiteralPath $TestDir -Recurse -Force
}

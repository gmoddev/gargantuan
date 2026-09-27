param(
    [Parameter(Mandatory=$true)][string]$EvidenceDir,
    [ValidateSet('Start','Stop')][string]$Action = 'Start'
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$Marker = Join-Path $EvidenceDir 'pktmon-owner.json'
$Etl = Join-Path $EvidenceDir 'worker-capture.etl'
$FilterName = 'Qualification-' + (Split-Path $EvidenceDir -Leaf)
function InvokePktMon([string[]]$Arguments) {
    $Output = & pktmon @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) { throw ($Output -join "`n") }
    return ($Output -join "`n")
}
if ($Action -eq 'Start') {
    $Status = InvokePktMon -Arguments @('status')
    if ($Status -notmatch 'Packet Monitor is not running') { throw 'An existing Packet Monitor session is protected.' }
    $Filters = InvokePktMon -Arguments @('filter','list')
    if ($Filters.Trim() -notmatch '^Packet Filters:\s+None$') { throw 'Existing Packet Monitor filters are protected.' }
    $Adapter = Get-NetAdapter -Name 'Ethernet 4'
    $IP = Get-NetIPAddress -InterfaceIndex $Adapter.ifIndex -AddressFamily IPv4 | Where-Object {$_.IPAddress -eq '10.253.3.2' -and $_.PrefixLength -eq 30}
    $If = Get-NetIPInterface -InterfaceIndex $Adapter.ifIndex -AddressFamily IPv4
    if ($Adapter.Status -ne 'Up' -or $Adapter.LinkSpeed -ne '10 Gbps' -or !$IP -or $If.NlMtu -ne 1500 -or $If.Dhcp -ne 'Disabled' -or $Adapter.MacAddress -ne '0C-42-A1-52-38-A8') {
        throw 'Worker Mellanox configuration does not match the qualified direct link.'
    }
    $Inventory = InvokePktMon -Arguments @('list','--json') | ConvertFrom-Json
    $Components = @($Inventory | ForEach-Object {$_.Components} | Where-Object {
        $_.Type -eq 'Miniport' -and ($_.Properties | Where-Object {$_.Name -eq 'ifIndex' -and $_.Value -eq $Adapter.ifIndex})
    })
    if ($Components.Count -ne 1 -or $Components[0].DriverName -ne 'mlx5.sys') { throw 'Cannot attribute the Mellanox capture component.' }
    $IPv4Components = @($Inventory | ForEach-Object {$_.Components} | Where-Object {
        $_.DriverName -eq 'tcpip.sys' -and $_.Name -eq 'TCP/IPv4 - L2' -and
        ($_.Properties | Where-Object {$_.Name -eq 'Miniport ifIndex' -and $_.Value -eq $Adapter.ifIndex})
    })
    if ($IPv4Components.Count -ne 1) { throw 'Cannot attribute the IPv4 capture component to the Mellanox interface.' }
    $CaptureComponents = @($Components[0].Id, $IPv4Components[0].Id) | Sort-Object -Unique
    InvokePktMon -Arguments @('filter','add',$FilterName,'-t','UDP','-i','10.253.3.1','10.253.3.2','-p','39450') | Write-Output
    $OwnedFilters = InvokePktMon -Arguments @('filter','list')
    @{FilterName=$FilterName; FilterList=$OwnedFilters; Etl=$Etl; Components=$CaptureComponents;
      CaptureLayers=@('Mellanox miniport','TCP/IPv4 - L2')} | ConvertTo-Json | Set-Content -LiteralPath $Marker -Encoding UTF8
    $StartArguments = @('start','--capture','--comp') + @($CaptureComponents | ForEach-Object {[string]$_}) +
        @('--pkt-size','0','--file-name',$Etl,'--file-size','64','--log-mode','circular')
    InvokePktMon -Arguments $StartArguments | Write-Output
    $ActiveStatus = InvokePktMon -Arguments @('status')
    if ($ActiveStatus -match 'Packet Monitor is not running' -or !(Test-Path -LiteralPath $Etl)) { throw 'Packet Monitor did not become active.' }
    $ActiveStatus | Set-Content -LiteralPath (Join-Path $EvidenceDir 'capture-active.txt')
} else {
    if (!(Test-Path -LiteralPath $Marker)) { return }
    $Owned = Get-Content -Raw -LiteralPath $Marker | ConvertFrom-Json
    $Status = InvokePktMon -Arguments @('status')
    if ($Status -notmatch 'Packet Monitor is not running') {
        if (!$Status.Contains($Owned.Etl)) { throw 'Current Packet Monitor session differs from the task-owned session.' }
        InvokePktMon -Arguments @('stop') | Write-Output
    }
    $Filters = InvokePktMon -Arguments @('filter','list')
    if ($Filters.Trim() -ne $Owned.FilterList.Trim()) { throw 'Packet Monitor filters changed; preserving them for local reconciliation.' }
    # Startup required zero filters, so the unchanged list contains only this task's filter.
    InvokePktMon -Arguments @('filter','remove') | Write-Output
    if (Test-Path -LiteralPath $Owned.Etl) {
        InvokePktMon -Arguments @('etl2pcap',$Owned.Etl,'--out',(Join-Path $EvidenceDir 'worker-capture.pcapng')) | Write-Output
    }
}

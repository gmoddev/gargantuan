param(
    [Parameter(Mandatory=$true)][ValidateSet('CLIENT', 'WORKER')][string]$EndpointKind,
    [Parameter(Mandatory=$true)][string]$ExpectedEndpointSid,
    [switch]$ValidateOnly
)
$ErrorActionPreference = 'Stop'

# Neither a path nor an ACL is accepted from the caller. These are the only
# qualification roots this task may provision, on their respective machines.
$Roots = @{
    CLIENT = 'C:\Sandbox\Codex\Evidence\physical-qualifier'
    WORKER = 'C:\GargantuanQualification\physical-qualifier-service-evidence'
}
$Root = $Roots[$EndpointKind]
$Item = Get-Item -LiteralPath $Root -ErrorAction Stop
if (-not $Item.PSIsContainer -or ($Item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
    throw '[Qualification:Evidence] Approved root is not a real directory.'
}
$SandboxSid = ([Security.Principal.NTAccount]::new(
    $env:COMPUTERNAME, 'CodexSandboxOffline')).Translate(
        [Security.Principal.SecurityIdentifier]).Value
if ($ExpectedEndpointSid -cne $SandboxSid) {
    throw '[Qualification:Evidence] Endpoint SID does not match the installed offline sandbox identity.'
}
$CurrentIdentity = [Security.Principal.WindowsIdentity]::GetCurrent()
$UserSid = $CurrentIdentity.User.Value
$Acl = Get-Acl -LiteralPath $Root
$OwnerSid = $Acl.GetOwner([Security.Principal.SecurityIdentifier]).Value
$SystemSid = 'S-1-5-18'
$AdminSid = 'S-1-5-32-544'
$Full = [Security.AccessControl.FileSystemRights]::FullControl
$Modify = [Security.AccessControl.FileSystemRights]::Modify
$ExpectedRights = @{$SystemSid = $Full; $AdminSid = $Full; $SandboxSid = $Modify}
if ($EndpointKind -eq 'CLIENT') {
    if ($OwnerSid -cne $UserSid -and $UserSid -cne $SystemSid) {
        throw '[Qualification:Evidence] Client root requires its owner or LocalSystem.'
    }
    $ExpectedRights[$OwnerSid] = $Full
} else {
    $ServiceConfig = Get-Content -LiteralPath 'C:\ProgramData\Gargantuan\PhysicalQualifierCapture\service.json' -Raw | ConvertFrom-Json
    if ($ServiceConfig.EvidenceRoot -cne $Root) {
        throw '[Qualification:Evidence] Installed capture service uses another evidence root.'
    }
    $ServiceSid = ([Security.Principal.NTAccount]::new(
        $env:COMPUTERNAME, 'host')).Translate(
            [Security.Principal.SecurityIdentifier]).Value
    if ($ServiceConfig.AuthorizedSid -cne $ServiceSid) {
        throw '[Qualification:Evidence] Installed capture service SID changed.'
    }
    if ($OwnerSid -cne $AdminSid -and $OwnerSid -cne $SystemSid) {
        throw '[Qualification:Evidence] Worker root owner changed.'
    }
    if ($UserSid -cne $SystemSid -and -not ([Security.Principal.WindowsPrincipal]::new($CurrentIdentity)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw '[Qualification:Evidence] Worker root requires an elevated administrator or LocalSystem.'
    }
    # The installer/service client retains its existing Modify right. The
    # capture service itself runs as LocalSystem and retains FullControl.
    $ExpectedRights[$ServiceSid] = $Modify
}
if ($ValidateOnly) {
    Write-Output "[Qualification:Evidence] $EndpointKind fixed-root and SID request valid."
    return
}

$Inherit = [Security.AccessControl.InheritanceFlags]'ContainerInherit, ObjectInherit'
$Propagate = [Security.AccessControl.PropagationFlags]::None
$Allow = [Security.AccessControl.AccessControlType]::Allow
$Synchronize = [Security.AccessControl.FileSystemRights]::Synchronize
function Test-FixedPolicy($Candidate) {
    if (-not $Candidate.AreAccessRulesProtected -or
        $Candidate.Access.Count -ne $ExpectedRights.Count) { return $false }
    $Seen = @{}
    foreach ($Rule in $Candidate.Access) {
        $Sid = $Rule.IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value
        if (-not $ExpectedRights.ContainsKey($Sid) -or $Seen.ContainsKey($Sid) -or
            $Rule.AccessControlType -ne $Allow -or $Rule.IsInherited -or
            $Rule.InheritanceFlags -ne $Inherit -or $Rule.PropagationFlags -ne $Propagate -or
            [int]$Rule.FileSystemRights -ne [int]($ExpectedRights[$Sid] -bor $Synchronize)) {
            return $false
        }
        $Seen[$Sid] = $true
    }
    return $Seen.Count -eq $ExpectedRights.Count
}
if (Test-FixedPolicy $Acl) {
    Write-Output "[Qualification:Evidence] $EndpointKind fixed-root ACL already provisioned."
    return
}

$Acl.SetAccessRuleProtection($true, $false)
foreach ($Existing in @($Acl.Access)) { $Acl.RemoveAccessRuleSpecific($Existing) }
foreach ($Sid in $ExpectedRights.Keys) {
    $Rule = [Security.AccessControl.FileSystemAccessRule]::new(
        [Security.Principal.SecurityIdentifier]::new($Sid), $ExpectedRights[$Sid],
        $Inherit, $Propagate, $Allow)
    $Acl.AddAccessRule($Rule)
}
Set-Acl -LiteralPath $Root -AclObject $Acl
if (-not (Test-FixedPolicy (Get-Acl -LiteralPath $Root))) {
    throw '[Qualification:Evidence] Installed ACL differs from the fixed policy.'
}
Write-Output "[Qualification:Evidence] $EndpointKind fixed-root ACL provisioned for the exact sandbox SID."

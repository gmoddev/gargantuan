param(
    [Parameter(Mandatory=$true)][string]$EvidenceRoot,
    [Parameter(Mandatory=$true)][string]$ExpectedEndpointSid,
    [switch]$ValidateOnly
)
$ErrorActionPreference = 'Stop'
$ApprovedRoot = 'C:\Sandbox\Codex\Evidence\physical-qualifier'
$Requested = [IO.Path]::GetFullPath($EvidenceRoot).TrimEnd('\')
if ($Requested -cne $ApprovedRoot) { throw '[Qualification:Evidence] Unapproved evidence root.' }
$Item = Get-Item -LiteralPath $ApprovedRoot -ErrorAction Stop
if (-not $Item.PSIsContainer -or ($Item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
    throw '[Qualification:Evidence] Evidence root is not a real directory.'
}
$SandboxAccount = [Security.Principal.NTAccount]::new($env:COMPUTERNAME, 'CodexSandboxOffline')
$ActualSid = $SandboxAccount.Translate([Security.Principal.SecurityIdentifier]).Value
if ($ExpectedEndpointSid -cne $ActualSid) {
    throw '[Qualification:Evidence] Endpoint SID does not match the installed offline sandbox identity.'
}
$UserSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$CurrentAcl = Get-Acl -LiteralPath $ApprovedRoot
$OwnerSid = $CurrentAcl.GetOwner([Security.Principal.SecurityIdentifier]).Value
if ($OwnerSid -cne $UserSid -and $UserSid -ne 'S-1-5-18') {
    throw '[Qualification:Evidence] Only the root owner or LocalSystem may provision this root.'
}
if ($ValidateOnly) {
    Write-Output '[Qualification:Evidence] Fixed-root and endpoint-SID request valid.'
    return
}

$Inherit = [Security.AccessControl.InheritanceFlags]'ContainerInherit, ObjectInherit'
$Propagate = [Security.AccessControl.PropagationFlags]::None
$Allow = [Security.AccessControl.AccessControlType]::Allow
$ExpectedRights = @{
    'S-1-5-18' = [Security.AccessControl.FileSystemRights]::FullControl
    'S-1-5-32-544' = [Security.AccessControl.FileSystemRights]::FullControl
    $UserSid = [Security.AccessControl.FileSystemRights]::FullControl
    $ActualSid = [Security.AccessControl.FileSystemRights]::Modify
}
if ($CurrentAcl.AreAccessRulesProtected -and $CurrentAcl.Access.Count -eq 4) {
    $ExistingSids = @($CurrentAcl.Access | ForEach-Object {
        $Sid = $_.IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value
        if (-not $ExpectedRights.ContainsKey($Sid) -or $_.AccessControlType -ne $Allow -or
            $_.IsInherited -or $_.InheritanceFlags -ne $Inherit -or
            $_.PropagationFlags -ne $Propagate -or
            ($_.FileSystemRights -band $ExpectedRights[$Sid]) -ne $ExpectedRights[$Sid]) {
            return
        }
        $Sid
    })
    if ($ExistingSids.Count -eq 4 -and @($ExistingSids | Select-Object -Unique).Count -eq 4) {
        Write-Output '[Qualification:Evidence] Fixed-root ACL already provisioned.'
        return
    }
}

# Replace the root DACL atomically. Children created later inherit only these
# four task-scoped principals; no arbitrary path or ACL entry comes from input.
$Access = Get-Acl -LiteralPath $ApprovedRoot
$Access.SetAccessRuleProtection($true, $false)
foreach ($Existing in @($Access.Access)) {
    $Access.RemoveAccessRuleSpecific($Existing)
}
foreach ($Grant in @(
    @{Sid='S-1-5-18'; Rights=[Security.AccessControl.FileSystemRights]::FullControl},
    @{Sid='S-1-5-32-544'; Rights=[Security.AccessControl.FileSystemRights]::FullControl},
    @{Sid=$UserSid; Rights=[Security.AccessControl.FileSystemRights]::FullControl},
    @{Sid=$ActualSid; Rights=[Security.AccessControl.FileSystemRights]::Modify}
)) {
    $Identity = [Security.Principal.SecurityIdentifier]::new($Grant.Sid)
    $Rule = [Security.AccessControl.FileSystemAccessRule]::new(
        $Identity, $Grant.Rights, $Inherit, $Propagate, $Allow)
    $Access.AddAccessRule($Rule)
}
Set-Acl -LiteralPath $ApprovedRoot -AclObject $Access
$Installed = Get-Acl -LiteralPath $ApprovedRoot
if (-not $Installed.AreAccessRulesProtected) { throw '[Qualification:Evidence] Root ACL inheritance remained enabled.' }
$InstalledSids = @($Installed.Access | ForEach-Object {
    $Sid = $_.IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value
    if (-not $ExpectedRights.ContainsKey($Sid) -or
        $_.AccessControlType -ne $Allow -or $_.IsInherited -or
        $_.InheritanceFlags -ne $Inherit -or $_.PropagationFlags -ne $Propagate -or
        ($_.FileSystemRights -band $ExpectedRights[$Sid]) -ne $ExpectedRights[$Sid]) {
        throw '[Qualification:Evidence] Installed root ACL differs from the fixed policy.'
    }
    $Sid
})
if ($InstalledSids.Count -ne 4 -or @($InstalledSids | Select-Object -Unique).Count -ne 4) {
    throw '[Qualification:Evidence] Installed root ACL differs from the fixed policy.'
}
Write-Output '[Qualification:Evidence] Fixed root provisioned for owner, administrators, SYSTEM and the offline sandbox SID.'

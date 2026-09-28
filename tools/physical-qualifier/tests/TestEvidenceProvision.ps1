$ErrorActionPreference = 'Stop'
$Provision = Join-Path $PSScriptRoot '..\Provision-ClientEvidenceRoot.ps1'
$Root = 'C:\Sandbox\Codex\Evidence\physical-qualifier'
$Sid = ([Security.Principal.NTAccount]::new($env:COMPUTERNAME, 'CodexSandboxOffline')).Translate(
    [Security.Principal.SecurityIdentifier]).Value
$Unrelated = Join-Path $env:TEMP ('qualification-acl-negative-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $Unrelated | Out-Null
try {
    $Before = (Get-Acl -LiteralPath $Unrelated).Sddl
    $RejectedPath = $false
    try { & $Provision -EvidenceRoot $Unrelated -ExpectedEndpointSid $Sid } catch { $RejectedPath = $_.Exception.Message -match 'Unapproved evidence root' }
    if (-not $RejectedPath) { throw 'arbitrary ACL target was not rejected' }
    if ((Get-Acl -LiteralPath $Unrelated).Sddl -cne $Before) { throw 'unrelated ACL changed' }
    $RejectedSid = $false
    try { & $Provision -EvidenceRoot $Root -ExpectedEndpointSid 'S-1-5-32-545' } catch { $RejectedSid = $_.Exception.Message -match 'Endpoint SID' }
    if (-not $RejectedSid) { throw 'unrelated SID was not rejected' }
    & $Provision -EvidenceRoot $Root -ExpectedEndpointSid $Sid -ValidateOnly | Out-Null
    Write-Output '[Qualification:Evidence] Bounded provisioning denial tests PASS'
} finally {
    Remove-Item -LiteralPath $Unrelated
}

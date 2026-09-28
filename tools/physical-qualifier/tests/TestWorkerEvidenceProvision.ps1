$ErrorActionPreference = 'Stop'
$Provision = Join-Path $PSScriptRoot '..\Ensure-QualificationEvidenceRoot.ps1'
$Root = 'C:\GargantuanQualification\physical-qualifier-service-evidence'
$Sid = ([Security.Principal.NTAccount]::new($env:COMPUTERNAME, 'CodexSandboxOffline')).Translate(
    [Security.Principal.SecurityIdentifier]).Value
$Service = Get-Content -LiteralPath 'C:\ProgramData\Gargantuan\PhysicalQualifierCapture\service.json' -Raw | ConvertFrom-Json
if ($Service.EvidenceRoot -cne $Root) { throw 'capture service evidence confinement changed' }
$Before = (Get-Acl -LiteralPath $Root).Sddl
& $Provision -EndpointKind WORKER -ExpectedEndpointSid $Sid | Out-Null
if ((Get-Acl -LiteralPath $Root).Sddl -cne $Before) { throw 'worker provisioner not idempotent' }
$WrongSid = $false
try { & $Provision -EndpointKind WORKER -ExpectedEndpointSid 'S-1-5-32-545' } catch { $WrongSid = $_.Exception.Message -match 'Endpoint SID' }
if (-not $WrongSid) { throw 'wrong worker SID was not rejected' }
$WrongPath = $false
try { & $Provision -EndpointKind WORKER -ExpectedEndpointSid $Sid -EvidenceRoot 'C:\Windows' } catch { $WrongPath = $_.Exception.Message -match 'EvidenceRoot' }
if (-not $WrongPath) { throw 'arbitrary ACL path was not rejected' }
$Access = @((Get-Acl -LiteralPath $Root).Access)
$Sids = @($Access | ForEach-Object { $_.IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value })
$Allowed = @('S-1-5-18', 'S-1-5-32-544', $Service.AuthorizedSid, $Sid)
if ($Access.Count -ne 4 -or @($Sids | Where-Object { $_ -notin $Allowed }).Count -ne 0) {
    throw 'worker ACL includes unrelated write identities'
}
if ((Get-Acl -LiteralPath $Root).Sddl -cne $Before) { throw 'worker ACL changed during denial tests' }
Write-Output '[Qualification:Evidence] Worker fixed-root confinement and idempotence PASS'

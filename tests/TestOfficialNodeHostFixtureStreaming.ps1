param([Parameter(Mandatory=$true)][string]$SourceRoot)

$ErrorActionPreference='Stop'
$Fixture=Join-Path $SourceRoot 'tests\OfficialNodeHostVertical.ps1'
$Sample=Join-Path $SourceRoot 'samples\FirstCompleteGame\.gargantuan\project.instance.json'
$BeforeHash=(Get-FileHash -LiteralPath $Sample -Algorithm SHA256).Hash
$Tokens=$null
$Errors=$null
$Tree=[Management.Automation.Language.Parser]::ParseFile($Fixture,[ref]$Tokens,[ref]$Errors)
if ($Errors.Count -ne 0) { throw 'Official fixture script does not parse' }
foreach ($Name in @('Get-DocumentObjectCount','Move-AnimatedBeaconToBootstrap')) {
	$Functions=@($Tree.FindAll({param($Node) $Node -is [Management.Automation.Language.FunctionDefinitionAst] -and
		$Node.Name -ceq $Name},$true))
	if ($Functions.Count -ne 1) { throw 'Official fixture function identity differs' }
	. ([ScriptBlock]::Create($Functions[0].Extent.Text))
}

function Get-Sample {
	return Get-Content -LiteralPath $Sample -Raw | ConvertFrom-Json
}

function Get-Workspace($Document) {
	return @($Document.Children | Where-Object { $_.Name -ceq 'Workspace' })[0]
}

function Get-Course($Workspace) {
	return @($Workspace.Children | Where-Object { $_.Name -ceq 'CollectionCourse' })[0]
}

function Deny-Unchanged($Document) {
	$Before=$Document | ConvertTo-Json -Depth 100 -Compress
	$Denied=$false
	try { Move-AnimatedBeaconToBootstrap -ProjectDocument $Document }
	catch { $Denied=$true }
	if (-not $Denied -or ($Document | ConvertTo-Json -Depth 100 -Compress) -cne $Before) {
		throw 'Invalid fixture layout was accepted or mutated'
	}
}

$Document=Get-Sample
$Workspace=Get-Workspace $Document
$Course=Get-Course $Workspace
$Beacon=@($Course.Children | Where-Object { $_.Name -ceq 'AnimatedBeacon' })[0]
$BeaconJson=$Beacon | ConvertTo-Json -Depth 100 -Compress
$StaticJson=@($Course.Children | Where-Object { $_.Name -cne 'AnimatedBeacon' }) |
	ConvertTo-Json -Depth 100 -Compress
$ScriptsJson=@($Document.Children | Where-Object { $_.Name -ceq 'GameScripts' }) |
	ConvertTo-Json -Depth 100 -Compress
$BeforeCount=Get-DocumentObjectCount -Node $Document
Move-AnimatedBeaconToBootstrap -ProjectDocument $Document
$Workspace=Get-Workspace $Document
$Course=Get-Course $Workspace
if (@($Workspace.Children).Count -ne 2 -or @($Course.Children).Count -ne 5 -or
	-not [object]::ReferenceEquals($Workspace.Children[1],$Beacon) -or
	(Get-DocumentObjectCount -Node $Document) -ne $BeforeCount -or
	(Get-DocumentObjectCount -Node $Course) -ne 15 -or
	(Get-DocumentObjectCount -Node $Beacon) -ne 5 -or
	($Beacon | ConvertTo-Json -Depth 100 -Compress) -cne $BeaconJson -or
	(@($Course.Children) | ConvertTo-Json -Depth 100 -Compress) -cne $StaticJson -or
	(@($Document.Children | Where-Object { $_.Name -ceq 'GameScripts' }) |
		ConvertTo-Json -Depth 100 -Compress) -cne $ScriptsJson) {
	throw 'Fixture move lost or changed authored objects'
}

$Lightweight=Get-Sample
Move-AnimatedBeaconToBootstrap -ProjectDocument $Lightweight
$LightWorkspace=Get-Workspace $Lightweight
$LightCourse=Get-Course $LightWorkspace
$Ground=@($LightCourse.Children | Where-Object { $_.Name -ceq 'Ground' })[0]
$LightCourse.Children=@($Ground)
if ((Get-DocumentObjectCount -Node $LightCourse) -ne 2 -or
	(Get-DocumentObjectCount -Node $LightWorkspace.Children[1]) -ne 5) {
	throw 'Lightweight profile discarded the animated bootstrap sibling'
}

$Missing=Get-Sample
$MissingCourse=Get-Course (Get-Workspace $Missing)
$MissingCourse.Children=@($MissingCourse.Children | Where-Object { $_.Name -cne 'AnimatedBeacon' })
Deny-Unchanged $Missing
$Duplicate=Get-Sample
$DuplicateCourse=Get-Course (Get-Workspace $Duplicate)
$DuplicateCourse.Children=@($DuplicateCourse.Children)+@($DuplicateCourse.Children[-1])
Deny-Unchanged $Duplicate
$WrongClass=Get-Sample
(@((Get-Course (Get-Workspace $WrongClass)).Children | Where-Object { $_.Name -ceq 'AnimatedBeacon' })[0]).ClassName='Part'
Deny-Unchanged $WrongClass
$ExtraWorkspace=Get-Sample
$ExtraWorkspaceNode=Get-Workspace $ExtraWorkspace
$ExtraWorkspaceNode.Children=@($ExtraWorkspaceNode.Children)+@($ExtraWorkspaceNode.Children[0])
Deny-Unchanged $ExtraWorkspace
$ExtraCourse=Get-Sample
$ExtraCourseNode=Get-Course (Get-Workspace $ExtraCourse)
$ExtraCourseNode.Children=@($ExtraCourseNode.Children)+@($ExtraCourseNode.Children[0])
Deny-Unchanged $ExtraCourse

if ((Get-FileHash -LiteralPath $Sample -Algorithm SHA256).Hash -cne $BeforeHash) {
	throw 'Original sample bytes changed'
}
'[Content:OfficialHost] isolated fixture move controls passed; package and Player unrun'

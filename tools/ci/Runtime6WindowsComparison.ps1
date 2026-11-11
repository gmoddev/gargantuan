# Copyright (c) 2026 Team Fireworks
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

param(
    [Parameter(Mandatory = $true)][string] $BaselineSource,
    [Parameter(Mandatory = $true)][string] $CandidateSource,
    [Parameter(Mandatory = $true)][ValidateSet('core', 'cframe', 'compact', 'realistic', 'gameplay')][string] $Suite,
    [Parameter(Mandatory = $true)][string] $ShaderCompiler,
    [Parameter(Mandatory = $true)][string] $EvidenceRoot
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
if (-not ('Runtime6Comparison.NativeErrorMode' -as [type])) {
    Add-Type -Namespace Runtime6Comparison -Name NativeErrorMode -MemberDefinition '[DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint Mode);'
}
[void][Runtime6Comparison.NativeErrorMode]::SetErrorMode(0x8003)

if ($env:GITHUB_ACTIONS -cne 'true' -or $env:RUNNER_ENVIRONMENT -cne 'github-hosted' -or
    $env:RUNNER_OS -cne 'Windows') {
    throw '[Runtime6:WindowsCompare] Hosted Windows runner required'
}
foreach ($Source in @($BaselineSource, $CandidateSource)) {
    if ($Source -cnotmatch '^[0-9a-f]{40}$') {
        throw '[Runtime6:WindowsCompare] Both source inputs must be exact lowercase 40-hex commits'
    }
}
$Workspace = [IO.Path]::GetFullPath($env:GITHUB_WORKSPACE)
$EvidenceRoot = [IO.Path]::GetFullPath($EvidenceRoot)
$BuildRoot = Join-Path $Workspace 'build-runtime6'
if (-not $env:HOME) {
    if (-not $env:USERPROFILE) { throw '[Runtime6:WindowsCompare] Lute requires HOME or USERPROFILE' }
    $env:HOME = $env:USERPROFILE
}
if (-not [IO.Path]::IsPathFullyQualified($ShaderCompiler) -or
    -not (Test-Path -LiteralPath $ShaderCompiler -PathType Leaf)) {
    throw '[Runtime6:WindowsCompare] Pinned shader compiler is unavailable'
}
if (Test-Path -LiteralPath $EvidenceRoot) {
    throw '[Runtime6:WindowsCompare] Evidence directory already exists'
}
New-Item -ItemType Directory -Path $EvidenceRoot | Out-Null
@{
    os = [Runtime.InteropServices.RuntimeInformation]::OSDescription
    framework = [Runtime.InteropServices.RuntimeInformation]::FrameworkDescription
    processor = @($env:PROCESSOR_IDENTIFIER)
    logicalProcessors = [Environment]::ProcessorCount
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $EvidenceRoot 'host.json')
& cl /Bv 2>&1 | Set-Content -LiteralPath (Join-Path $EvidenceRoot 'compiler.txt')
& cmake --version | Set-Content -LiteralPath (Join-Path $EvidenceRoot 'cmake.txt')
& ninja --version | Set-Content -LiteralPath (Join-Path $EvidenceRoot 'ninja.txt')
if ($env:SCCACHE_GHA_ENABLED -cne 'on' -or -not (Get-Command sccache.exe -ErrorAction SilentlyContinue)) {
    throw '[Runtime6:WindowsCompare] Pinned sccache action was not initialized'
}
$CompilerDigest = (Get-FileHash -Algorithm SHA256 -LiteralPath (Get-Command cl.exe).Source).Hash.ToLowerInvariant()
$ImageIdentity = if ($env:ImageVersion) { $env:ImageVersion } else {
    [Runtime.InteropServices.RuntimeInformation]::OSDescription
}
& sccache --version | Set-Content -LiteralPath (Join-Path $EvidenceRoot 'sccache-version.txt')
if ($LASTEXITCODE -ne 0) { throw '[Runtime6:WindowsCompare] Pinned sccache is unavailable' }

function Invoke-Checked([string] $Label, [string] $Log, [scriptblock] $Command) {
    Write-Host "[Runtime6:WindowsCompare] $Label"
    & $Command 2>&1 | Tee-Object -FilePath $Log
    if ($LASTEXITCODE -ne 0) { throw "[Runtime6:WindowsCompare] $Label failed: $LASTEXITCODE" }
}

Push-Location $Workspace
try {
    foreach ($Arm in @('baseline', 'candidate')) {
        $Source = if ($Arm -eq 'baseline') { $BaselineSource } else { $CandidateSource }
        $ArmRoot = Join-Path $EvidenceRoot $Arm
        New-Item -ItemType Directory -Path $ArmRoot | Out-Null
        & git cat-file -e "$Source^{commit}"
        if ($LASTEXITCODE -ne 0) { throw "[Runtime6:WindowsCompare] Commit unavailable: $Source" }
        & git checkout --detach $Source
        if ($LASTEXITCODE -ne 0 -or (& git rev-parse HEAD).Trim() -cne $Source) {
            throw "[Runtime6:WindowsCompare] Checkout identity mismatch: $Arm"
        }
        & git submodule update --init --recursive
        if ($LASTEXITCODE -ne 0) { throw "[Runtime6:WindowsCompare] Submodule checkout failed: $Arm" }
        $Submodules = @(& git submodule status --recursive)
        if ($LASTEXITCODE -ne 0 -or @($Submodules | Where-Object { $_ -match '^[\-+U]' }).Count -ne 0) {
            throw "[Runtime6:WindowsCompare] Inexact submodule state: $Arm"
        }
        $Submodules | Set-Content -LiteralPath (Join-Path $ArmRoot 'submodules.txt')
        # The GHA cache stores compiler outputs, never build trees. The digest keeps
        # a compiler/image/dependency/profile namespace stable across source edits.
        $CacheMaterial = (@('runtime6-win25-vs2026-msvc-release-pch-off-gns-off-v1',
            $ImageIdentity, $CompilerDigest, $Workspace) + $Submodules) -join "`n"
        $CacheDigest = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData(
            [Text.Encoding]::UTF8.GetBytes($CacheMaterial))).ToLowerInvariant()
        $env:SCCACHE_GHA_VERSION = "runtime6-win25-msvc-$($CacheDigest.Substring(0, 32))"
        # A daemon reads its backend configuration at startup. Give each arm a
        # separate local endpoint even when their dependency revisions differ.
        $env:SCCACHE_SERVER_PORT = if ($Arm -eq 'baseline') { '4246' } else { '4247' }
        $env:SCCACHE_BASEDIRS = $Workspace
        $env:SCCACHE_CACHE_READ_TIMEOUT_SECS = '15'
        "image=$ImageIdentity`ncompilerSha256=$CompilerDigest`nnamespace=$env:SCCACHE_GHA_VERSION`nprofile=Release;PCH=OFF;GNS=OFF" |
            Set-Content -LiteralPath (Join-Path $ArmRoot 'sccache-namespace.txt')
        & sccache --zero-stats | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "[Runtime6:WindowsCompare] sccache did not start for $Arm" }
        Invoke-Checked "$Arm classgen" (Join-Path $ArmRoot 'classgen.log') { & lute tools/classgen -f }
        & git diff --binary | Set-Content -LiteralPath (Join-Path $ArmRoot 'generated.patch')
        if ($LASTEXITCODE -ne 0) { throw "[Runtime6:WindowsCompare] Generated diff failed: $Arm" }

        $Configure = @('-S', '.', '-B', $BuildRoot, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
            '-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON', '-DBUILD_TESTING=ON', '-DGARGANTUAN_TRACY=OFF',
            '-DGARGANTUAN_WITH_GNS=OFF', '-DGARGANTUAN_BUILD_RENDERER_BENCHMARKS=ON',
            '-DGARGANTUAN_BUILD_RENDERER_ALLOCATION_PROBE=ON',
            '-DGARGANTUAN_BUILD_GLAZE_SERIALIZATION_PROTOTYPE=OFF',
            '-DCMAKE_C_COMPILER_LAUNCHER=sccache', '-DCMAKE_CXX_COMPILER_LAUNCHER=sccache',
            "-DGARGANTUAN_GLSLC_EXECUTABLE=$ShaderCompiler")
        Invoke-Checked "$Arm configure" (Join-Path $ArmRoot 'configure.log') { & cmake @Configure }
        $Targets = @('gargantuan_runtime_foundation6_benchmark',
            'gargantuan_runtime6_extraction_alternatives',
            'gargantuan_runtime6_physics_sync_benchmark',
            'gargantuan_runtime6_allocation_benchmark',
            'gargantuan_foundation_tests', 'gargantuan_prepared_property_tests',
            'gargantuan_renderer_publication_tests', 'gargantuan_physics_backend_tests')
        if ($Suite -eq 'realistic') {
            $Targets += @('gargantuan_runtime6_realistic_publication_probe',
                'gargantuan_asset_foundation_benchmark', 'gargantuan_animation_foundation_benchmark',
                'gargantuan_soft_body_physics_benchmark', 'gargantuan_replication_benchmark')
        }
        if ($Suite -eq 'compact' -and $Arm -eq 'candidate') {
            $Targets += 'gargantuan_runtime6_physics_sync_fault_tests'
        }
        if ($Suite -eq 'gameplay') { $Targets += 'gargantuan_game_session_benchmark' }
        Invoke-Checked "$Arm build" (Join-Path $ArmRoot 'build.log') {
            & cmake --build $BuildRoot --config Release --parallel 3 --target @Targets
        }
        & sccache --show-stats | Set-Content -LiteralPath (Join-Path $ArmRoot 'sccache-stats.txt')
        if ($LASTEXITCODE -ne 0) { throw "[Runtime6:WindowsCompare] sccache stats unavailable for $Arm" }
        & sccache --stop-server | Set-Content -LiteralPath (Join-Path $ArmRoot 'sccache-stop.txt')
        $Tests = @('gargantuan_foundation', 'gargantuan_prepared_property',
            'gargantuan_renderer_publication', 'gargantuan_physics_backend',
            'gargantuan_runtime_foundation6_smoke', 'gargantuan_runtime6_extraction_alternatives',
            'gargantuan_runtime6_physics_sync_smoke')
        if ($Suite -eq 'realistic') { $Tests += 'gargantuan_runtime6_realistic_publication_probe_smoke' }
        if ($Suite -eq 'compact' -and $Arm -eq 'candidate') { $Tests += 'gargantuan_runtime6_physics_sync_fault' }
        $TestCatalog = & ctest --test-dir $BuildRoot --show-only=json-v1 | ConvertFrom-Json
        if ($LASTEXITCODE -ne 0) { throw "[Runtime6:WindowsCompare] CTest catalog failed: $Arm" }
        $Listed = @($TestCatalog.tests | ForEach-Object { $_.name })
        foreach ($TestName in $Tests) {
            if ($Listed -cnotcontains $TestName) { throw "[Runtime6:WindowsCompare] Missing gate: $Arm/$TestName" }
        }
        $TestRegex = '^(' + (($Tests | ForEach-Object { [regex]::Escape($_) }) -join '|') + ')$'
        Invoke-Checked "$Arm CTest" (Join-Path $ArmRoot 'tests.log') {
            & ctest --test-dir $BuildRoot --output-on-failure --parallel 1 -R $TestRegex
        }

        $FreezeTargets = @('gargantuan_runtime_foundation6_benchmark',
            'gargantuan_runtime6_extraction_alternatives',
            'gargantuan_runtime6_physics_sync_benchmark',
            'gargantuan_runtime6_allocation_benchmark', 'gargantuan_prepared_property_tests')
        if ($Suite -eq 'realistic') {
            $FreezeTargets += @('gargantuan_runtime6_realistic_publication_probe',
                'gargantuan_asset_foundation_benchmark', 'gargantuan_animation_foundation_benchmark',
                'gargantuan_soft_body_physics_benchmark', 'gargantuan_replication_benchmark')
        }
        if ($Suite -eq 'gameplay') { $FreezeTargets += 'gargantuan_game_session_benchmark' }
        $Hashes = @{}
        foreach ($Target in $FreezeTargets) {
            $Name = "$Target.exe"
            $From = Join-Path $BuildRoot $Name
            if (-not (Test-Path -LiteralPath $From -PathType Leaf)) {
                throw "[Runtime6:WindowsCompare] Missing built executable: $Arm/$Name"
            }
            $To = Join-Path $ArmRoot $Name
            Copy-Item -LiteralPath $From -Destination $To
            $Hashes[$Name] = (Get-FileHash -Algorithm SHA256 -LiteralPath $To).Hash.ToLowerInvariant()
            & dumpbin /DEPENDENTS $To | Set-Content -LiteralPath (Join-Path $ArmRoot "$Name.dependencies.txt")
            if ($LASTEXITCODE -ne 0) { throw "[Runtime6:WindowsCompare] Dependency audit failed: $Arm/$Name" }
        }
        $Dlls = @(Get-ChildItem -LiteralPath $BuildRoot -Filter '*.dll' -File)
        if (-not @($Dlls | Where-Object { $_.Name -ieq 'SDL3.dll' }).Count) {
            throw "[Runtime6:WindowsCompare] SDL3.dll was not staged for $Arm"
        }
        foreach ($Dll in $Dlls) {
            $To = Join-Path $ArmRoot $Dll.Name
            Copy-Item -LiteralPath $Dll.FullName -Destination $To
            $Hashes[$Dll.Name] = (Get-FileHash -Algorithm SHA256 -LiteralPath $To).Hash.ToLowerInvariant()
        }
        $RuntimeRoot = Join-Path $BuildRoot 'runtime'
        if (Test-Path -LiteralPath $RuntimeRoot -PathType Container) {
            foreach ($File in Get-ChildItem -LiteralPath $RuntimeRoot -Recurse -File) {
                $Relative = 'runtime/' + ([IO.Path]::GetRelativePath($RuntimeRoot, $File.FullName) -replace '\\', '/')
                $To = Join-Path $ArmRoot $Relative
                New-Item -ItemType Directory -Force -Path (Split-Path -Parent $To) | Out-Null
                Copy-Item -LiteralPath $File.FullName -Destination $To
                $Hashes[$Relative] = (Get-FileHash -Algorithm SHA256 -LiteralPath $To).Hash.ToLowerInvariant()
            }
        }
        $SourceInputs = @{}
        if ($Suite -eq 'gameplay') {
            $InputPaths = @(& git ls-files -- samples/FirstCompleteGame)
            if ($LASTEXITCODE -ne 0 -or $InputPaths.Count -eq 0 -or
                $InputPaths -notcontains 'samples/FirstCompleteGame/.gargantuan/assets/catalog.json') {
                throw "[Runtime6:WindowsCompare] Gameplay source inputs are incomplete: $Arm"
            }
            foreach ($Relative in $InputPaths) {
                $From = Join-Path $Workspace $Relative
                $FrozenRelative = 'source-inputs/' + $Relative
                $To = Join-Path $ArmRoot $FrozenRelative
                New-Item -ItemType Directory -Force -Path (Split-Path -Parent $To) | Out-Null
                Copy-Item -LiteralPath $From -Destination $To
                $Digest = (Get-FileHash -Algorithm SHA256 -LiteralPath $To).Hash.ToLowerInvariant()
                $Hashes[$FrozenRelative] = $Digest
                $SourceInputs[$Relative] = $Digest
            }
        }
        @{ source = $Source; suite = $Suite; binaries = $Hashes;
            sourceInputRoot = $Workspace; sourceInputs = $SourceInputs } |
            ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $ArmRoot 'manifest.json')
        $Changed = @(& git diff --name-only)
        if ($LASTEXITCODE -ne 0) { throw "[Runtime6:WindowsCompare] Generated status failed: $Arm" }
        foreach ($Path in $Changed) {
            if ($Path -notin @('include/gargantuan/classes/generated/Animator.hpp',
                    'src/classes/generated/Animator.cpp')) {
                throw "[Runtime6:WindowsCompare] Unexpected tracked source change: $Arm/$Path"
            }
        }
        & git restore -- include/gargantuan/classes/generated/Animator.hpp src/classes/generated/Animator.cpp
        if ($LASTEXITCODE -ne 0) { throw "[Runtime6:WindowsCompare] Generator cleanup failed: $Arm" }
        Write-Host "[Runtime6:WindowsCompare] Froze $Arm source $Source with $($Hashes.Count) files"
    }
} finally {
    Pop-Location
}

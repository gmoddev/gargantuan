$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Add-Type -TypeDefinition 'using System.Runtime.InteropServices; public static class InputTestProcess { [DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint Mode); }'
[InputTestProcess]::SetErrorMode(0x8007) | Out-Null
$env:SDL_VIDEODRIVER = 'dummy'
$env:SDL_AUDIODRIVER = 'dummy'
$InputBuild = 'C:\Sandbox\Codex\Builds\gargantuan-input\msvc1444-release'
$InputLogs = 'C:\Sandbox\Codex\Logs\gargantuan-input'
$InputCtest = 'C:\Sandbox\Codex\Tools\cmake-3.31.10\cmake-3.31.10-windows-x86_64\bin\ctest.exe'
$InputWorkspace = 'C:\Sandbox\Codex\Workspaces\gargantuan-input'
$InputManifest = Get-Content "$InputWorkspace\devdocs\validation\input-action-foundation-20261103\SourceManifest.json" -Raw | ConvertFrom-Json
foreach ($InputSource in @($InputManifest.Sources) + @($InputManifest.Generated)) {
    $InputHash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $InputWorkspace $InputSource.Path)).Hash.ToLowerInvariant()
    if ($InputHash -ne $InputSource.Sha256) { throw "[Input:Qualification] Source mismatch: $($InputSource.Path)" }
}
$InputBinaries = @('gargantuan_input_action_tests.exe','gargantuan_input_action_benchmark.exe','gargantuan_foundation_tests.exe','gargantuan_player_runtime_tests.exe','gargantuan_platform_input_boundary_tests.exe','gargantuan_interaction_foundation_tests.exe','gargantuan_gui_foundation_tests.exe') | ForEach-Object {
    [ordered]@{ Path = $_; Sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $InputBuild $_)).Hash.ToLowerInvariant() }
}
$InputVerification = [ordered]@{ SourceDigest = $InputManifest.SourceDigest; SourceCount = $InputManifest.Sources.Count; GeneratedCount = $InputManifest.Generated.Count; VerifiedUtc = [DateTime]::UtcNow.ToString('o'); Binaries = @($InputBinaries) }
[IO.File]::WriteAllText("$InputLogs\source-verification.json", ($InputVerification | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false))
Write-Output "[Input:Qualification] Verified source digest $($InputManifest.SourceDigest)"
& $InputCtest --test-dir $InputBuild -C Release --parallel 2 --timeout 180 --output-on-failure --no-tests=error -R '^gargantuan_(input_action|input_action_benchmark_smoke|player_runtime|platform_input_boundary|foundation|interaction_foundation|gui_foundation)$' --output-junit "$InputLogs\tests.xml" 2>&1 | Tee-Object -FilePath "$InputLogs\tests.log"
$InputTestResult = $LASTEXITCODE
if ($InputTestResult -ne 0) { exit $InputTestResult }
$InputBenchmark = & "$InputBuild\gargantuan_input_action_benchmark.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
[IO.File]::WriteAllLines("$InputLogs\benchmark.json", $InputBenchmark, [Text.UTF8Encoding]::new($false))
Get-Item "$InputLogs\benchmark.json" | Select-Object FullName,Length

$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'
Add-Type -TypeDefinition 'using System.Runtime.InteropServices; public static class InputAllTestProcess { [DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint Mode); }'
[InputAllTestProcess]::SetErrorMode(0x8007) | Out-Null
$env:SDL_VIDEODRIVER = 'dummy'
$env:SDL_AUDIODRIVER = 'dummy'
$InputBuild = 'C:\Sandbox\Codex\Builds\gargantuan-input\msvc1444-release'
$InputLogs = 'C:\Sandbox\Codex\Logs\gargantuan-input'
$InputCtest = 'C:\Sandbox\Codex\Tools\cmake-3.31.10\cmake-3.31.10-windows-x86_64\bin\ctest.exe'
& $InputCtest --test-dir $InputBuild -C Release --parallel 2 --timeout 300 --output-on-failure --no-tests=error --output-junit "$InputLogs\all-tests.xml" 2>&1 | Tee-Object -FilePath "$InputLogs\all-tests.log"
exit $LASTEXITCODE

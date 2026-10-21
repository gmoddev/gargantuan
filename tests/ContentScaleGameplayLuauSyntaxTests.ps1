#requires -Version 7.0
# Compile the exact embedded client script, including the farm callback tap.
$ErrorActionPreference = 'Stop'
$Compiler = (Get-Command luau-compile.exe -ErrorAction Stop).Source
$Source = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'ContentScaleGameplay.hpp') -Raw
$Client = $Source.IndexOf('auto ClientScript')
$Begin = $Source.IndexOf('R"(', $Client)
$End = $Source.IndexOf(')"', $Begin + 3)
if ($Client -lt 0 -or $Begin -lt 0 -or $End -lt 0) { throw 'embedded client Luau source delimiters are missing' }
$Path = Join-Path ([IO.Path]::GetTempPath()) ('gargantuan-farm-client-' + [Guid]::NewGuid().ToString('N') + '.luau')
try {
	[IO.File]::WriteAllText($Path, $Source.Substring($Begin + 3, $End - $Begin - 3))
	& $Compiler $Path | Out-Null
	if ($LASTEXITCODE -ne 0) { throw "embedded client Luau compilation failed: $LASTEXITCODE" }
	Write-Output '[Qualification:Callback] LUAU_COMPILE_OK'
} finally {
	if (Test-Path -LiteralPath $Path) { Remove-Item -LiteralPath $Path -Force }
}

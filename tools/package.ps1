param(
    [Parameter(Mandatory=$true)][string]$QtDirectory,
    [Parameter(Mandatory=$true)][string]$BinaryDirectory,
    [Parameter(Mandatory=$true)][string]$RuntimeDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
$sourceDirectory = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$qtPath = (Resolve-Path -LiteralPath $QtDirectory).Path
$binaryPath = (Resolve-Path -LiteralPath $BinaryDirectory).Path
$runtimePath = (Resolve-Path -LiteralPath $RuntimeDirectory).Path
if (Test-Path -LiteralPath $OutputDirectory) {
    throw 'OutputDirectory already exists. Choose an empty, new directory.'
}
foreach ($name in @('Live2DRecovery.exe', 'recovery-cli.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $binaryPath $name))) { throw "Missing binary: $name" }
}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$destination = (Resolve-Path -LiteralPath $OutputDirectory).Path
foreach ($name in @('Live2DRecovery.exe', 'recovery-cli.exe')) {
    Copy-Item -LiteralPath (Join-Path $binaryPath $name) -Destination $destination
}

$env:PATH = (Join-Path $qtPath 'bin') + ';' + $env:PATH
& (Join-Path $qtPath 'bin/windeployqt.exe') --release --no-translations --no-opengl-sw --no-system-d3d-compiler --no-system-dxc-compiler --no-compiler-runtime --skip-plugin-types generic,networkinformation,tls (Join-Path $destination 'Live2DRecovery.exe')
if ($LASTEXITCODE -ne 0) { throw 'Qt deployment failed.' }

# App-local runtime files; use the x64 redistributable matching the chosen Qt build.
foreach ($name in @('msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll',
                    'msvcp140_atomic_wait.dll', 'msvcp140_codecvt_ids.dll',
                    'vcruntime140.dll', 'vcruntime140_1.dll', 'concrt140.dll')) {
    $library = Join-Path $runtimePath $name
    if (-not (Test-Path -LiteralPath $library)) { throw "Missing runtime library: $name" }
    Copy-Item -LiteralPath $library -Destination $destination
}
foreach ($name in @('README.md', 'ORIGIN.md', 'LICENSE', 'THIRD_PARTY.md')) {
    Copy-Item -LiteralPath (Join-Path $sourceDirectory $name) -Destination $destination
}
foreach ($name in @('docs', 'licenses')) {
    Copy-Item -LiteralPath (Join-Path $sourceDirectory $name) -Destination $destination -Recurse
}
Write-Output "Portable application: $destination"

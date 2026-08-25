[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot,
    [Parameter(Mandatory = $true)]
    [string]$OutputRoot,
    [Parameter(Mandatory = $true)]
    [string]$Version,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$source = Convert-Path -LiteralPath $SourceRoot
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
$output = Convert-Path -LiteralPath $OutputRoot
$bundleName = "igi1conv-mcp-win-x64-v$Version"
$bundle = Join-Path $output $bundleName
if (Test-Path -LiteralPath $bundle) {
    if (-not $Force) { throw "Bundle already exists: $bundle (use -Force to replace it)" }
    Remove-Item -LiteralPath $bundle -Recurse -Force
}
New-Item -ItemType Directory -Path $bundle -Force | Out-Null

$topLevel = @(
    'igi1conv.exe',
    'igi1conv_tests.exe',
    'D3Dcompiler_47.dll',
    'lame_enc.dll',
    'opengl32sw.dll',
    'vc_redist.x64.exe',
    'Qt6Core.dll',
    'Qt6Gui.dll',
    'Qt6Network.dll',
    'Qt6OpenGL.dll',
    'Qt6OpenGLWidgets.dll',
    'Qt6Svg.dll',
    'Qt6Widgets.dll'
)
$pluginFiles = @(
    'generic\qtuiotouchplugin.dll',
    'iconengines\qsvgicon.dll',
    'imageformats\qgif.dll',
    'imageformats\qico.dll',
    'imageformats\qjpeg.dll',
    'imageformats\qsvg.dll',
    'networkinformation\qnetworklistmanager.dll',
    'platforms\qwindows.dll',
    'styles\qwindowsvistastyle.dll',
    'tls\qcertonlybackend.dll',
    'tls\qopensslbackend.dll',
    'tls\qschannelbackend.dll'
)

foreach ($relative in @($topLevel + $pluginFiles)) {
    $sourceFile = Join-Path $source $relative
    if (-not (Test-Path -LiteralPath $sourceFile -PathType Leaf)) {
        throw "Required release runtime is missing: $sourceFile"
    }
    $destination = Join-Path $bundle $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $sourceFile -Destination $destination -Force
}

$nonBinary = @(Get-ChildItem -LiteralPath $bundle -Recurse -File |
    Where-Object { $_.Extension.ToLowerInvariant() -notin @('.dll', '.exe') })
if ($nonBinary.Count -ne 0) {
    throw "Binary package contains non-binary files: $($nonBinary.FullName -join ', ')"
}

$zip = Join-Path $output "$bundleName.zip"
if (Test-Path -LiteralPath $zip) {
    if (-not $Force) { throw "Archive already exists: $zip (use -Force to replace it)" }
    Remove-Item -LiteralPath $zip -Force
}
Compress-Archive -Path (Join-Path $bundle '*') -DestinationPath $zip -CompressionLevel Optimal -Force

$converterAsset = Join-Path $output "igi1conv-v$Version-win-x64.exe"
$testsAsset = Join-Path $output "igi1conv_tests-v$Version-win-x64.exe"
Copy-Item -LiteralPath (Join-Path $source 'igi1conv.exe') -Destination $converterAsset -Force
Copy-Item -LiteralPath (Join-Path $source 'igi1conv_tests.exe') -Destination $testsAsset -Force

$assets = @(
    $zip,
    $converterAsset,
    $testsAsset
)
$checksums = foreach ($asset in $assets) {
    $hash = Get-FileHash -LiteralPath $asset -Algorithm SHA256
    "{0}  {1}" -f $hash.Hash, (Split-Path -Leaf $asset)
}
$checksumPath = Join-Path $output 'SHA256SUMS.txt'
$checksums | Set-Content -LiteralPath $checksumPath -Encoding ascii

[pscustomobject]@{
    Version = $Version
    Bundle = $zip
    BundleEntries = @(Get-ChildItem -LiteralPath $bundle -Recurse -File).Count
    BundleSha256 = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
    Checksums = $checksumPath
    Converter = $converterAsset
    TestExecutable = $testsAsset
}

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$root = (Resolve-Path (Join-Path $PSScriptRoot "..\")).Path
$readme = Get-Content -LiteralPath (Join-Path $root "README.md") -Raw
$mcp = Get-Content -LiteralPath (Join-Path $root "docs\mcp.md") -Raw
$changelog = Get-Content -LiteralPath (Join-Path $root "CHANGELOG.md") -Raw
$formats = Get-Content -LiteralPath (Join-Path $root "docs\SUPPORTED_FORMATS.md") -Raw

function Assert-Contains([string]$Text, [string]$Needle, [string]$File) {
    if (-not $Text.Contains($Needle)) { throw "$File is missing '$Needle'" }
}

foreach ($needle in @(
    "igi1conv mcp --transport stdio",
    "igi1conv mcp --transport http",
    "game effects only",
    "igi_game_command",
    "igi_game_object_edit",
    "position",
    "rotation",
    "model id",
    "Settings",
    "camera"
)) { Assert-Contains $readme $needle "README.md" }

foreach ($needle in @(
    "2025-11-25",
    "igi1conv://game-capabilities",
    "qsc.edit-object",
    "position",
    "rotation",
    "model_id",
    "team",
    "bone_hierarchy",
    "stand_animation",
    "Origin",
    "--auth-token",
    "Explicitly excluded"
)) { Assert-Contains $mcp $needle "docs/mcp.md" }

Assert-Contains $changelog "Game-facing MCP server" "CHANGELOG.md"
Assert-Contains $formats "game-affecting CLI operations" "docs/SUPPORTED_FORMATS.md"
Write-Output "documentation: PASS"

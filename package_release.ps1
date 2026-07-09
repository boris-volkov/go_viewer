# Assembles a shareable ogs_client distribution into dist/ogs_client/ and zips it.
# Run from anywhere -- paths are resolved relative to this script's own location
# (the repo root), not the caller's current directory.
#
# Included: ogs_client.exe + every DLL next to it, ca-bundle.crt,
#           config.ini.example, the pro game library (games/), and
#           ogs_client/katago/ if present.
# Excluded: config.ini (real credentials), my_games/ (personal games), every log
#           file, and per-session state (adaptive_level.txt, solved_puzzles.txt,
#           settings.txt, puzzle_collections.txt).

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$src  = Join-Path $root "ogs_client"
$dist = Join-Path $root "dist\ogs_client"

$exePath = Join-Path $src "ogs_client.exe"
if (-not (Test-Path $exePath)) {
    Write-Error "ogs_client.exe not found at $exePath -- build it first."
    exit 1
}

if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Force -Path $dist | Out-Null

Copy-Item $exePath $dist
Copy-Item (Join-Path $src "*.dll") $dist
Copy-Item (Join-Path $src "ca-bundle.crt") $dist
Copy-Item (Join-Path $src "config.ini.example") $dist

$katagoSrc = Join-Path $src "katago"
if (Test-Path $katagoSrc) {
    Copy-Item -Recurse $katagoSrc (Join-Path $dist "katago")
    Write-Host "Included KataGo from $katagoSrc"
} else {
    Write-Host "No katago/ folder next to ogs_client.exe -- skipping (recipients set up KataGo themselves)"
}

# Pro game library only -- my_games/ (personal) is never touched by this script.
Copy-Item -Recurse (Join-Path $root "games") (Join-Path $dist "games")
Get-ChildItem -Path (Join-Path $dist "games") -Recurse -Force |
    Where-Object { $_.Name -eq ".go_viewer_index" } |
    Remove-Item -Force

$zipPath = Join-Path $root ("dist\ogs_client_{0}.zip" -f (Get-Date -Format "yyyyMMdd"))
if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
Compress-Archive -Path (Join-Path $dist "*") -DestinationPath $zipPath

Write-Host ""
Write-Host "Packaged: $dist"
Write-Host "Zipped:   $zipPath"
Write-Host ""
Write-Host "Before sharing, double check:"
Write-Host "  - config.ini is NOT in the package (only config.ini.example)"
Write-Host "  - my_games/ is NOT in the package"
Write-Host "  - if katago/ is included, confirm it's a build recipients can actually run"
Write-Host "    (OpenCL/Eigen, not a CUDA build tied to your specific GPU)"

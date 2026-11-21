# Local release (same result as the GitHub "Build" workflow, without Actions):
#   tools\release.ps1 -Version 0.2.0            build, package, publish a pre-release on GitHub
#   tools\release.ps1 -Version 0.2.0 -Final     publish as a normal (latest) release
#   tools\release.ps1 -Version 0.2.0 -DryRun    only build and package (dist\MWTogether-0.2.0.zip)
# A starter career save (end of the prologue: the last race against Razor, then the car lot) in starter-save\ is attached as a second zip.
# Needs: Visual Studio 2022 C++ (x86), and the GitHub CLI (gh) logged in for publishing.
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [switch]$Final,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

if (-not $DryRun) {
    if (git status --porcelain) { throw "Uncommitted changes - commit or stash them first." }
    if (git tag --list "v$Version") { throw "Tag v$Version already exists." }
}

# 1. build with the version stamped in
$env:MWT_VERSION = $Version
& (Join-Path $root "build.bat")
if ($LASTEXITCODE -ne 0) { throw "build failed" }
Remove-Item Env:MWT_VERSION

# 2. package: dinput8.dll (Ultimate ASI Loader, MIT) + scripts\MWTogether.asi/.ini + README
$dist = Join-Path $root 'dist'
$pkg = Join-Path $dist "MWTogether-$Version"
Remove-Item $pkg -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $pkg 'scripts') | Out-Null
Copy-Item build\MWTogether.asi (Join-Path $pkg 'scripts')
# settings ship as a default only: unpacking an update over an install never overwrites the player's MWTogether.ini
Copy-Item MWTogether.ini (Join-Path $pkg 'scripts\MWTogether.default.ini')
Copy-Item README.md $pkg
& C:\Windows\Microsoft.NET\Framework\v4.0.30319\csc.exe /nologo /target:winexe /out:build\MWTogetherLauncher.exe /r:System.Windows.Forms.dll /r:System.Drawing.dll "/resource:docs\banner.png,banner.png" "/resource:docs\launcher-bg.jpg,bg.jpg" launcher\Launcher.cs
if ($LASTEXITCODE -ne 0) { throw "launcher build failed" }
Copy-Item build\MWTogetherLauncher.exe $pkg

$loader = Join-Path $dist 'dinput8.dll'
if (-not (Test-Path $loader)) {
    $tmp = Join-Path $env:TEMP "uasi-$([guid]::NewGuid())"
    New-Item -ItemType Directory $tmp | Out-Null
    Invoke-WebRequest 'https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/latest/download/Ultimate-ASI-Loader.zip' -OutFile "$tmp\uasi.zip"
    Expand-Archive "$tmp\uasi.zip" -DestinationPath "$tmp\x"
    Copy-Item "$tmp\x\dinput8.dll" $loader
    Remove-Item $tmp -Recurse -Force
}
Copy-Item $loader $pkg

$zip = Join-Path $dist "MWTogether-$Version.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path "$pkg\*" -DestinationPath $zip
Write-Host "Packaged $zip"
$assets = @($zip)
$save = Join-Path $root 'starter-save'
if (Test-Path (Join-Path $save 'NFS Most Wanted')) {
    $szip = Join-Path $dist "MWTogether-$Version-StarterSave.zip"
    Remove-Item $szip -ErrorAction SilentlyContinue
    Compress-Archive -Path (Join-Path $save '*') -DestinationPath $szip
    $assets += $szip; Write-Host "Packaged $szip"
}
if ($DryRun) { return }

# 3. tag + GitHub release
git tag "v$Version"
git push origin "v$Version"
$flags = @('--generate-notes', '--title', "NFS MW Together $Version")
if (-not $Final) { $flags += '--prerelease' }
# the release assets always have the same names, so README can link releases/latest/download/MWTogether.zip
$named = @()
foreach ($a in $assets) {
    $fixed = Join-Path $dist ((Split-Path $a -Leaf) -replace "-$([regex]::Escape($Version))", "")
    Copy-Item $a $fixed -Force
    $named += $fixed
}
gh release create "v$Version" @named @flags
Write-Host "Released v$Version"

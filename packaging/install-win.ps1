<#
    Install the SourcePawn-stack Accelerator build onto a CS:GO Legacy server.

      powershell -ExecutionPolicy Bypass -File .\install-win.ps1 -Server C:\steam\csgo
      powershell -ExecutionPolicy Bypass -File .\install-win.ps1 -Server C:\steam\csgo -DryRun

    Existing files move to a timestamped rollback folder first, and every copied
    file is checked against checksums.txt afterwards.
#>
param(
	[Parameter(Mandatory = $true)][string]$Server,
	[switch]$DryRun
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $here "windows-x86\addons\sourcemod"
$sm = Join-Path $Server "addons\sourcemod"

if (-not (Test-Path $sm)) { throw "no addons\sourcemod under $Server - is that the server root?" }
if (-not (Test-Path $src)) { throw "missing $src - unpack the whole release folder, not just this script" }

$files = @(
	"extensions\accelerator.ext.dll",
	"extensions\accelerator.autoload",
	"gamedata\accelerator.games.txt",
	"scripting\include\accelerator.inc",
	"plugins\accelerator_stack_probe.smx"
)

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$rollback = Join-Path $sm "rollback-accelerator-spstack\$stamp"

Write-Host "server   : $Server"
Write-Host "package  : $here"
Write-Host "rollback : $rollback"
if ($DryRun) { Write-Host "(dry run: nothing is written)" }
Write-Host ""

if (-not $DryRun) { New-Item -ItemType Directory -Force -Path $rollback | Out-Null }

$replaced = 0
foreach ($f in $files) {
	$dest = Join-Path $sm $f
	if (Test-Path $dest) {
		$old = (Get-Item $dest).Length
		Write-Host ("replacing  {0,-45} (old size {1})" -f $f, $old)
		if (-not $DryRun) {
			$to = Join-Path $rollback $f
			New-Item -ItemType Directory -Force -Path (Split-Path -Parent $to) | Out-Null
			Move-Item -Force $dest $to
		}
		$replaced++
	} else {
		Write-Host ("installing {0,-45} (new)" -f $f)
	}
	if (-not $DryRun) {
		New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null
		Copy-Item -Force (Join-Path $src $f) $dest
	}
}

if (-not $DryRun) {
	# Verify exactly the files this script installs. checksums.txt also covers the
	# other platform and the source tree, and failing on those would mean failing
	# on something the installer never claims to own.
	$sums = Get-Content (Join-Path $here "checksums.txt")
	foreach ($f in $files) {
		$rel = $f -replace "\\", "/"
		$row = $sums | Where-Object { $_.EndsWith("  windows-x86/addons/sourcemod/$rel") } | Select-Object -First 1
		if ($null -eq $row) { throw "NO CHECKSUM ENTRY: $rel" }
		$expected = ($row -split "\s+")[0]
		$actual = (Get-FileHash -Algorithm SHA256 (Join-Path $sm $f)).Hash.ToLower()
		if ($actual -ne $expected) { throw "CHECKSUM MISMATCH: $rel" }
	}
	Write-Host "all $($files.Count) files verified against checksums.txt"
}

Write-Host ""
Write-Host "next steps, on the server:"
Write-Host "  1. restart the server, or load it now:  sm exts load accelerator.ext"
Write-Host "  2. in the server console: sm plugins list     (accelerator must appear)"
Write-Host "  3. in the server console: sm_accel_stack     (must show at least 2 frames)"
Write-Host "  4. only when 3 looks right: sm_accel_crash confirm   <-- this ends the server"
Write-Host ""
Write-Host "crash reports land in addons\sourcemod\data\dumps\<dump>.dmp.txt and are"
Write-Host "copied to addons\sourcemod\data\dumps\archive\ after upload."
if ($replaced -gt 0) {
	Write-Host ""
	Write-Host "$replaced file(s) were replaced; the previous copies are in $rollback"
}

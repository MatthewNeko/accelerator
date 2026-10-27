# Builds Accelerator for Windows with MSVC.
#
# The x86 (32 bit) target is what a CS:GO Legacy / Source 2013 dedicated server
# loads; x86_64 is offered for 64 bit Source ports.
#
#   powershell -ExecutionPolicy Bypass -File .\build-win.ps1 -Arch x86
#
# Requires: Visual Studio C++ tools, Python with AMBuild 2 installed
# (pip install --user git+https://github.com/alliedmodders/ambuild), and a
# checkout of the SourceMod source tree next to this repository.
param(
	[ValidateSet("x86", "x86_64")]
	[string]$Arch = "x86",
	[string]$SourceMod = "",
	[string]$Toolset = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not $SourceMod) {
	$SourceMod = Join-Path (Split-Path -Parent $root) "sourcemod-1.11"
}
if (-not (Test-Path (Join-Path $SourceMod "public\smsdk_ext.h"))) {
	Write-Error "SourceMod source tree not found at '$SourceMod'. Pass -SourceMod <path>."
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -prerelease -products * `
	-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
	-property installationPath
if (-not $vsPath) {
	Write-Error "No Visual Studio installation with the C++ x86/x64 tools was found."
}

# breakpad's HandleTraceData still uses stdext::checked_array_iterator, which the
# MSVC STL dropped starting with _MSVC_STL_VERSION 145 (Visual Studio 18). Toolset
# numbers do not order that way - 14.44 is STL 143, 14.51 is STL 145 - so read the
# macro out of each installed toolset instead of guessing from its version.
if (-not $Toolset) {
	$candidates = Get-ChildItem (Join-Path $vsPath "VC\Tools\MSVC") -Directory |
		Where-Object { $_.Name -like "14.*" } |
		ForEach-Object {
			$yvals = Join-Path $_.FullName "include\yvals_core.h"
			if (-not (Test-Path $yvals)) { return }
			$stl = Select-String -Path $yvals -Pattern "^\#define _MSVC_STL_VERSION\s+(\d+)" |
				Select-Object -First 1
			if (-not $stl) { return }
			[pscustomobject]@{ Name = $_.Name; Stl = [int]$stl.Matches[0].Groups[1].Value }
		}

	$Toolset = $candidates | Where-Object { $_.Stl -lt 145 } |
		Sort-Object { [version]$_.Name } -Descending |
		Select-Object -First 1 -ExpandProperty Name

	if (-not $Toolset) {
		$seen = ($candidates | ForEach-Object { "$($_.Name)=STL$($_.Stl)" }) -join ", "
		Write-Error ("No installed toolset older than MSVC STL 145 (checked: $seen). breakpad needs " +
			"stdext::checked_array_iterator. Either install a v143/v144 toolset, pass -Toolset <version>, " +
			"or patch src/client/windows/crash_generation/minidump_generator.cc to use a plain pointer.")
	}
}

$vcvarsAll = Join-Path $vsPath "VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $vcvarsAll)) {
	Write-Error "vcvarsall.bat not found at $vcvarsAll"
}

Write-Host "Importing developer environment (target $Arch, toolset $Toolset)"

# MSVC target names differ from AMBuild's: x86_64 is "amd64". The x86 build uses
# the amd64_x86 cross tools. Launch-VsDevShell.ps1 takes no -DevCmdArguments on
# this installation, and VCToolsVersion has to be set before vcvarsall runs, so
# the environment is captured from a cmd session and imported here.
$env:VCToolsVersion = "$Toolset"
$vsArch = if ($Arch -eq "x86") { "amd64_x86" } else { "amd64" }

$dump = & cmd.exe /c "`"$vcvarsAll`" $vsArch > nul 2>&1 && set"
if ($LASTEXITCODE -ne 0) {
	Write-Error "vcvarsall.bat $vsArch failed (toolset $Toolset)"
}
foreach ($line in $dump) {
	if ($line -match '^([^=]+)=(.*)$') {
		Set-Item -Path "Env:\$($matches[1])" -Value $matches[2] -ErrorAction SilentlyContinue
	}
}

$cl = (Get-Command cl.exe -ErrorAction SilentlyContinue).Source
if (-not $cl) { Write-Error "cl.exe is not on PATH after importing the developer environment." }
Write-Host "Using compiler: $cl"

$ambuild = Join-Path $env:APPDATA "Python\Python313\Scripts\ambuild.exe"
if (-not (Test-Path $ambuild)) {
	$ambuild = (Get-Command ambuild.exe -ErrorAction SilentlyContinue).Source
}
if (-not $ambuild) {
	Write-Error "ambuild.exe not found. Install it with: pip install --user git+https://github.com/alliedmodders/ambuild"
}

$build = Join-Path $root "build-win-$Arch"
New-Item -ItemType Directory -Force -Path $build | Out-Null
Set-Location $build

python (Join-Path $root "configure.py") --sm-path "$SourceMod" --enable-optimize --targets $Arch
if ($LASTEXITCODE -ne 0) { Write-Error "configure.py failed" }

& $ambuild . 2>&1 | Out-File -Encoding ascii -Width 4096 (Join-Path $build "build.log")
$ambuildStatus = $LASTEXITCODE
if ($ambuildStatus -ne 0) {
	Write-Host "ambuild failed; errors from $build\build.log:"
	Select-String -Path (Join-Path $build "build.log") -Pattern "error C|error LNK|fatal error|Build failed" |
		Select-Object -First 20 | ForEach-Object { $_.Line }
	Write-Error "ambuild failed; see $build\build.log"
}

Write-Host "---- artifacts ----"
Get-ChildItem -Path $build -Recurse -Filter "accelerator.ext.*dll" |
	ForEach-Object { "{0}  {1:N0} bytes" -f $_.FullName, $_.Length }

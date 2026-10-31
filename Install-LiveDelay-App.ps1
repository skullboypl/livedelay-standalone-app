# LiveDelay app installer. Per-user, no admin.
# Run from the unzipped release, next to livedelay.exe and livedelay.dll.
# Installs to %LOCALAPPDATA%\LiveDelay\App and adds a Start menu shortcut.
# Settings and stream keys in %APPDATA%\LiveDelay are not touched.
#Requires -Version 5.1
[CmdletBinding()]
param(
	[switch]$NoStart
)

$ErrorActionPreference = 'Stop'

function Show-Note([string]$Text, [string]$Title) {
	try {
		Add-Type -AssemblyName System.Windows.Forms | Out-Null
		[void][System.Windows.Forms.MessageBox]::Show($Text, $Title)
	} catch {
		Write-Host $Text
	}
}

$here = $PSScriptRoot
$exe = Join-Path $here 'livedelay.exe'
$dll = Join-Path $here 'livedelay.dll'
if (-not (Test-Path -LiteralPath $exe) -or -not (Test-Path -LiteralPath $dll)) {
	Show-Note "livedelay.exe and livedelay.dll were not found next to this script. Unzip the release and run the script from that folder." "LiveDelay"
	exit 1
}

$dest = Join-Path $env:LOCALAPPDATA 'LiveDelay\App'
New-Item -ItemType Directory -Force -Path $dest | Out-Null

$running = Get-Process -Name livedelay -ErrorAction SilentlyContinue | Where-Object {
	$_.Path -and ($_.Path -like "$dest\*")
}
if ($running) {
	try {
		Add-Type -AssemblyName System.Windows.Forms | Out-Null
		$answer = [System.Windows.Forms.MessageBox]::Show(
			"LiveDelay is running from the install folder. Close it and continue?",
			"LiveDelay",
			[System.Windows.Forms.MessageBoxButtons]::OKCancel)
		if ($answer -ne [System.Windows.Forms.DialogResult]::OK) { exit 0 }
	} catch { }
	$running | Stop-Process -Force
	Start-Sleep -Milliseconds 400
}

Copy-Item -LiteralPath $exe -Destination (Join-Path $dest 'livedelay.exe') -Force
Copy-Item -LiteralPath $dll -Destination (Join-Path $dest 'livedelay.dll') -Force
Copy-Item -LiteralPath $PSCommandPath -Destination (Join-Path $dest 'Install-LiveDelay-App.ps1') -Force
$uninstall = Join-Path $here 'Uninstall-LiveDelay-App.ps1'
if (Test-Path -LiteralPath $uninstall) {
	Copy-Item -LiteralPath $uninstall -Destination (Join-Path $dest 'Uninstall-LiveDelay-App.ps1') -Force
}

$shortcut = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\LiveDelay.lnk'
$shell = New-Object -ComObject WScript.Shell
$link = $shell.CreateShortcut($shortcut)
$link.TargetPath = Join-Path $dest 'livedelay.exe'
$link.WorkingDirectory = $dest
$link.Description = 'LiveDelay'
$link.Save()

Write-Host "Installed to $dest"
if (-not $NoStart) {
	Start-Process -FilePath (Join-Path $dest 'livedelay.exe')
}

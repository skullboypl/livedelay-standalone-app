# Removes the per-user LiveDelay app.
# Keeps %APPDATA%\LiveDelay (settings and stream keys).
#Requires -Version 5.1
$ErrorActionPreference = 'Stop'

$dest = Join-Path $env:LOCALAPPDATA 'LiveDelay\App'
Get-Process -Name livedelay -ErrorAction SilentlyContinue | Where-Object {
	$_.Path -and ($_.Path -like "$dest\*")
} | Stop-Process -Force
Start-Sleep -Milliseconds 400

if (Test-Path -LiteralPath $dest) {
	Remove-Item -LiteralPath $dest -Recurse -Force
}
$shortcut = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\LiveDelay.lnk'
if (Test-Path -LiteralPath $shortcut) {
	Remove-Item -LiteralPath $shortcut -Force
}
Write-Host "Removed $dest"
Write-Host "Settings in $env:APPDATA\LiveDelay were kept."

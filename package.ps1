# Release zip for the standalone app. Build first: build.bat
# Layout is flat: livedelay.exe, livedelay.dll, and the two scripts.
# Users unzip anywhere and run Install-LiveDelay-App.ps1.
$ErrorActionPreference = 'Stop'
$version = (Select-String -Path "$PSScriptRoot\CMakeLists.txt" -Pattern 'project\(livedelay-app VERSION ([0-9.]+)').Matches[0].Groups[1].Value
$name = "livedelay-app-$version-windows-x64"
$stage = Join-Path $PSScriptRoot "dist\$name"
$exe = Join-Path $PSScriptRoot "build\Release\livedelay.exe"
$dll = Join-Path $PSScriptRoot "build\Release\livedelay.dll"
if (-not (Test-Path -LiteralPath $exe)) { throw "Missing $exe. Run build.bat first." }
if (-not (Test-Path -LiteralPath $dll)) { throw "Missing $dll. The engine DLL is copied by build.bat." }
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item -LiteralPath $exe, $dll -Destination $stage
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Install-LiveDelay-App.ps1') -Destination $stage
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Uninstall-LiveDelay-App.ps1') -Destination $stage
$utf8bom = New-Object System.Text.UTF8Encoding $true
foreach ($script in @('Install-LiveDelay-App.ps1', 'Uninstall-LiveDelay-App.ps1')) {
	$p = Join-Path $stage $script
	[System.IO.File]::WriteAllText($p, [System.IO.File]::ReadAllText($p), $utf8bom)
}
@"
LiveDelay $version for Windows x64

This folder is the app, not the OBS plugin. The OBS plugin is a separate download.

Install (no admin)
  1. Unzip this archive anywhere.
  2. Right-click Install-LiveDelay-App.ps1 -> Run with PowerShell.
  3. It copies the app to %LOCALAPPDATA%\LiveDelay\App
     and adds a Start menu shortcut named LiveDelay.

Portable
  Run livedelay.exe from this folder. No install step.

Updates
  The window checks https://livedelay.vxh.pl/version.json
  A banner offers the new app or the new engine DLL.
  Files are replaced in the folder the exe was started from,
  then the app starts again. A stream from this PC stops for that moment.

Uninstall
  Close the app and run Uninstall-LiveDelay-App.ps1.
  Settings and stream keys in %APPDATA%\LiveDelay are kept.

Two versions
  The app version is this exe. The plugin version is livedelay.dll.
  Both are listed under O aplikacji.
"@ | Set-Content -Encoding utf8 (Join-Path $stage 'INSTALL.txt')
$zip = Join-Path $PSScriptRoot "dist\$name.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
& "$env:SystemRoot\System32\tar.exe" -a -c -f $zip -C $stage livedelay.exe livedelay.dll Install-LiveDelay-App.ps1 Uninstall-LiveDelay-App.ps1 INSTALL.txt
if ($LASTEXITCODE -ne 0) { throw "tar failed" }
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
"$hash  $name.zip" | Set-Content -Encoding ascii "$zip.sha256"
Write-Host "$zip"
Write-Host "SHA256 $hash"

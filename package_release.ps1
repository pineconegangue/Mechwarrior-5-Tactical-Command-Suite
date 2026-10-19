<#
  package_release.ps1 - assembles the ready-to-run release folder from a finished build.

  Build first (Visual Studio 2022: open the folder, configuration "x64-Debug" - it builds Release, /MT, so the
  exes need no VC++ runtime), then from PowerShell in this folder:
      .\package_release.ps1
  Output: release\MW5 Tactical Command Suite\
      plugins\    the six UEVR plugin DLLs   -> copy into %APPDATA%\UnrealVRMod\MechWarrior-Win64-Shipping\plugins
                  (TestPilot.dll is a developer tool and is never packaged)
      mods\       EnhancedImaging\ (the vision mode's .pak mod, from pak\EnhancedImaging) -> the game's MW5Mercs\Mods
      apps\       HUDDisplay, VoiceCommand, GlobalSettings + everything they load (voices, Whisper model,
                  dropship outlines, word lists; paperdolls: GlobalSettings > PAPERDOLLS > EXTRACT) - unpack anywhere, run from there.
                  The shipped settings come from release_defaults\ (GlobalSettings.ini, MFDDisplay_default.cfg).
      README.txt, LICENSE.txt, THIRD_PARTY_NOTICES.txt
  Nothing is zipped here: zip release\MW5 Tactical Command Suite yourself for upload.
  Fails (and says what) if anything the package needs is missing.
#>
param(
    [string]$BuildDir = "out\build\poc-x64-Debug",
    [string]$OutDir   = "release\MW5 Tactical Command Suite"
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root
$build = Join-Path $root $BuildDir
$out   = Join-Path $root $OutDir
if (-not (Test-Path $build)) { throw "No build folder at $build - build the project first." }

$missing = @()
function Need($path) { if (-not (Test-Path $path)) { $script:missing += $path } ; $path }

$plugins = 'LidarExport.dll','ScreenExport.dll','VanillaHud.dll','CameraCapture.dll','LanceControl.dll','EnhancedImaging.dll'
$apps    = 'HUDDisplay.exe','VoiceCommand.exe','GlobalSettings.exe',
           'onnxruntime.dll','onnxruntime_providers_shared.dll','sherpa-onnx-c-api.dll'
foreach ($f in $plugins + $apps) { Need (Join-Path $build $f) | Out-Null }
Need (Join-Path $root 'src\voice_command\HeardAs.txt') | Out-Null
Need (Join-Path $root 'src\voice_command\Pronunciations.txt') | Out-Null
Need (Join-Path $root 'assets\dropships') | Out-Null
Need (Join-Path $root 'kokoro-82m\kokoro-multi-lang-v1_0\model.onnx') | Out-Null
Need (Join-Path $root 'models\ggml-base.en.bin') | Out-Null
foreach ($f in 'README.release.txt','THIRD_PARTY_NOTICES.txt','LICENSE') { Need (Join-Path $root $f) | Out-Null }
foreach ($f in 'GlobalSettings.ini','MFDDisplay_default.cfg') { Need (Join-Path $root "release_defaults\$f") | Out-Null }
Need (Join-Path $root 'pak\EnhancedImaging\mod.json') | Out-Null
Need (Join-Path $root 'pak\EnhancedImaging\Paks\EnhancedImaging.pak') | Out-Null
if ($missing.Count) { throw ("Missing - nothing was packaged:`n  " + ($missing -join "`n  ")) }

if (Test-Path $out) { throw "$out already exists - move it aside first (this script never deletes)." }
New-Item -ItemType Directory -Force (Join-Path $out 'plugins'), (Join-Path $out 'apps') | Out-Null

foreach ($f in $plugins) { Copy-Item (Join-Path $build $f) (Join-Path $out 'plugins') }
foreach ($f in $apps)    { Copy-Item (Join-Path $build $f) (Join-Path $out 'apps') }
Copy-Item 'src\voice_command\HeardAs.txt','src\voice_command\Pronunciations.txt' (Join-Path $out 'apps')
# 2026-10-19 (user): paperdolls are NOT bundled - the player extracts them from their own game files
# (GlobalSettings > PAPERDOLLS > EXTRACT; the old bundled set was made by the broken decoder).
New-Item -ItemType Directory -Force (Join-Path $out 'apps\assets') | Out-Null
Copy-Item 'assets\dropships' (Join-Path $out 'apps\assets\dropships') -Recurse
New-Item -ItemType Directory -Force (Join-Path $out 'apps\kokoro-82m'), (Join-Path $out 'apps\models') | Out-Null
Copy-Item 'kokoro-82m\kokoro-multi-lang-v1_0' (Join-Path $out 'apps\kokoro-82m\kokoro-multi-lang-v1_0') -Recurse
Copy-Item 'models\ggml-base.en.bin' (Join-Path $out 'apps\models')
New-Item -ItemType Directory -Force (Join-Path $out 'mods') | Out-Null
Copy-Item 'pak\EnhancedImaging' (Join-Path $out 'mods\EnhancedImaging') -Recurse

# The shipped settings (2026-10-13): the author's chosen defaults, snapshotted into release_defaults\ from the
# build folder - GlobalSettings.ini without machine paths / dev keys (whisper_model and kokoro_dir relative to apps\),
# MFDDisplay_default.cfg without the window position and size (opens at HUDDisplay's default spot).
Copy-Item 'release_defaults\GlobalSettings.ini','release_defaults\MFDDisplay_default.cfg' (Join-Path $out 'apps')

Copy-Item 'README.release.txt' (Join-Path $out 'README.txt')
Copy-Item 'THIRD_PARTY_NOTICES.txt' $out
Copy-Item 'LICENSE' (Join-Path $out 'LICENSE.txt')

Write-Host "Packaged into $out"
Write-Host "  plugins: $($plugins -join ', ')"
Write-Host "  apps:    HUDDisplay, VoiceCommand, GlobalSettings (+ GlobalSettings.ini, MFDDisplay_default.cfg from release_defaults), voices, Whisper model, outlines (paperdolls: extracted by the player)"
Write-Host "  mods:    EnhancedImaging (.pak)"

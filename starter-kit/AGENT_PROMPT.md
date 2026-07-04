# Prompt For A Fresh AI Session

You are working in the MatrixMatsu repository, a Matrix chat client for Tanmatsu
based on `Nicolai-Electronics/tanmatsu-template`.

Important context:

- Main project remote is `matrixmatsu`:
  `https://github.com/daandobber/MatrixMatsu.git`
- Main branch tracks `matrixmatsu/main`.
- Build target is Tanmatsu with ESP-IDF target `esp32p4`.
- BadgeLink install is done with `.\install-badgelink.ps1`.
- BadgeLink test slug is `matrixmatsu`.
- App repository slug is `nl.daandobber.matrixmatsu`.
- Do not blindly stage all files. Always inspect `git status -sb` and stage
  explicit paths only.
- Audio and image media playback/viewing already exist. JPG uses the software
  `esp_jpeg` decoder; do not revert it to the hardware JPEG decoder.
- For a clean machine/fresh folder, use
  `starter-kit\bootstrap-matrixmatsu.ps1` to clone MatrixMatsu and configure the
  original Nicolai template as the `template` remote.

Useful first commands:

```powershell
git status -sb
Get-Content AGENTS.md
Get-Content starter-kit\COMMANDS.md
Get-Content starter-kit\HARDWARE.md
```

Bootstrap from scratch when needed:

```powershell
New-Item -ItemType Directory -Force MatrixMatsu-bootstrap
Set-Location MatrixMatsu-bootstrap
Invoke-WebRequest https://raw.githubusercontent.com/daandobber/MatrixMatsu/main/starter-kit/bootstrap-matrixmatsu.ps1 -OutFile bootstrap-matrixmatsu.ps1
powershell -ExecutionPolicy Bypass -File .\bootstrap-matrixmatsu.ps1 -SetupBadgeLink -SetupSdk -CloneTemplateToo
```

Standard build:

```powershell
$env:PYTHONIOENCODING='utf-8'
$env:PYTHONUTF8='1'
$env:IDF_TOOLS_PATH="$(Get-Location)\esp-idf-tools"
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
. .\esp-idf\export.ps1
idf.py --no-ccache -B build/tanmatsu build -DDEVICE=tanmatsu -DSDKCONFIG_DEFAULTS='sdkconfigs/general;sdkconfigs/tanmatsu' -DSDKCONFIG=sdkconfig_tanmatsu -DIDF_TARGET=esp32p4 -DFAT=0
```

Install already-built firmware:

```powershell
.\install-badgelink.ps1 -NoBuild
```

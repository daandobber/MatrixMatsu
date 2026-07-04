# Commands

Run these from the repository root in PowerShell.

## Bootstrap From Scratch

If you do not have the repository yet, download the bootstrap script first:

```powershell
New-Item -ItemType Directory -Force MatrixMatsu-bootstrap
Set-Location MatrixMatsu-bootstrap
Invoke-WebRequest https://raw.githubusercontent.com/daandobber/MatrixMatsu/main/starter-kit/bootstrap-matrixmatsu.ps1 -OutFile bootstrap-matrixmatsu.ps1
powershell -ExecutionPolicy Bypass -File .\bootstrap-matrixmatsu.ps1 -SetupBadgeLink -SetupSdk -CloneTemplateToo
```

If you already have this starter kit folder locally and want to pull in
everything needed for a MatrixMatsu workspace:

```powershell
powershell -ExecutionPolicy Bypass -File .\starter-kit\bootstrap-matrixmatsu.ps1 -SetupBadgeLink
```

If the machine does not have ESP-IDF yet, include `-SetupSdk`:

```powershell
powershell -ExecutionPolicy Bypass -File .\starter-kit\bootstrap-matrixmatsu.ps1 -SetupBadgeLink -SetupSdk
```

If you also want the original Nicolai template cloned as a separate reference
repo next to MatrixMatsu:

```powershell
powershell -ExecutionPolicy Bypass -File .\starter-kit\bootstrap-matrixmatsu.ps1 -SetupBadgeLink -SetupSdk -CloneTemplateToo
```

The script clones:

```text
https://github.com/daandobber/MatrixMatsu.git
```

It also configures the original template as a git remote named `template`:

```text
https://github.com/Nicolai-Electronics/tanmatsu-template.git
```

## Build Tanmatsu Firmware

```powershell
$env:PYTHONIOENCODING='utf-8'
$env:PYTHONUTF8='1'
$env:IDF_TOOLS_PATH="$(Get-Location)\esp-idf-tools"
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
. .\esp-idf\export.ps1
idf.py --no-ccache -B build/tanmatsu build -DDEVICE=tanmatsu -DSDKCONFIG_DEFAULTS='sdkconfigs/general;sdkconfigs/tanmatsu' -DSDKCONFIG=sdkconfig_tanmatsu -DIDF_TARGET=esp32p4 -DFAT=0
```

## Install With BadgeLink

Use this after a successful build:

```powershell
.\install-badgelink.ps1 -NoBuild
```

Build and install in one step:

```powershell
.\install-badgelink.ps1
```

Install without auto-start:

```powershell
.\install-badgelink.ps1 -NoBuild -NoStart
```

## Package App Repository Entry

```powershell
.\tools\make-emoji-assets.ps1
.\tools\make-emoji-pack.ps1
.\tools\package-app-repository.ps1
```

Output:

```text
dist/app-repository/nl.daandobber.matrixmatsu
dist/matrixmatsu-app-repository-package.zip
```

## Git Publish Pattern

Check what changed:

```powershell
git status -sb
git diff
```

Stage only relevant files:

```powershell
git add path\to\file1 path\to\file2
```

Commit:

```powershell
git commit -m "feat: short description"
```

Push to MatrixMatsu:

```powershell
git push matrixmatsu main
```

Fetch upstream template changes for comparison:

```powershell
git fetch template
git diff template/main..main
```

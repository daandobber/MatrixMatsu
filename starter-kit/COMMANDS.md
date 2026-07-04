# Commands

Run these from the repository root in PowerShell.

## Build Tanmatsu Firmware

```powershell
$env:PYTHONIOENCODING='utf-8'
$env:PYTHONUTF8='1'
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


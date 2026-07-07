# MatrixMatsu Agent Start

This repository is a MatrixMatsu app workspace based on
`Nicolai-Electronics/tanmatsu-template`, with local scripts and project context
added so an AI/coding agent can build, install, package, and publish without
re-learning the workflow.

## Repository Role

- Upstream template: `https://github.com/Nicolai-Electronics/tanmatsu-template`
- Project remote: `matrixmatsu` -> `https://github.com/daandobber/MatrixMatsu.git`
- Main target device: Tanmatsu
- AppFS install slug for BadgeLink testing: `matrixmatsu`
- App repository slug: `nl.daandobber.matrixmatsu`

## Working Tree Safety

- Do not use `git add -A` unless the user explicitly says the whole worktree is
  in scope.
- Known local files may be unrelated to the current task, especially
  `tools/package-app-repository.ps1`, `.claude/`, and experimental files in
  `main/`.
- Stage explicit paths only. Check `git status -sb` and `git diff` before every
  commit.
- Never reset or checkout user changes unless the user clearly asks for that.

## Standard Build

PowerShell build command for Tanmatsu:

```powershell
$env:PYTHONIOENCODING='utf-8'
$env:PYTHONUTF8='1'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
. .\esp-idf\export.ps1
idf.py --no-ccache -B build/tanmatsu build -DDEVICE=tanmatsu -DSDKCONFIG_DEFAULTS='sdkconfigs/general;sdkconfigs/tanmatsu' -DSDKCONFIG=sdkconfig_tanmatsu -DIDF_TARGET=esp32p4 -DFAT=0
```

Build output:

```text
build/tanmatsu/application.bin
```

## Fresh Bootstrap

If a user starts from only the starter kit or from a clean folder, use:

```powershell
New-Item -ItemType Directory -Force MatrixMatsu-bootstrap
Set-Location MatrixMatsu-bootstrap
Invoke-WebRequest https://raw.githubusercontent.com/daandobber/MatrixMatsu/main/starter-kit/bootstrap-matrixmatsu.ps1 -OutFile bootstrap-matrixmatsu.ps1
powershell -ExecutionPolicy Bypass -File .\bootstrap-matrixmatsu.ps1 -SetupBadgeLink -SetupSdk -CloneTemplateToo
```

If this repository is already cloned and the local starter kit exists:

```powershell
powershell -ExecutionPolicy Bypass -File .\starter-kit\bootstrap-matrixmatsu.ps1 -SetupBadgeLink
```

Add `-SetupSdk` when ESP-IDF is missing. Add `-CloneTemplateToo` when the user
wants the original Nicolai template cloned as a sibling reference checkout.

The bootstrap script clones MatrixMatsu and configures the original template as:

```text
template -> https://github.com/Nicolai-Electronics/tanmatsu-template.git
```

## BadgeLink Install

With the Tanmatsu connected in BadgeLink mode:

```powershell
.\install-badgelink.ps1 -NoBuild
```

To build and install in one command:

```powershell
.\install-badgelink.ps1
```

The script uploads `build/tanmatsu/application.bin` as `matrixmatsu` and starts
it unless `-NoStart` is passed.

## App Repository Package

Regenerate emoji assets when needed (only re-run the `gen-emoji-*` steps if
Unicode added new emoji since `tools/emoji-data.json` / `main/emoji_table.h`
were last generated):

```powershell
python tools\gen-emoji-data.py tools\emoji-test.txt tools\twemoji-filenames.txt tools\emoji-data.json
python tools\gen-emoji-table.py tools\emoji-data.json main\emoji_table.h
.\tools\make-emoji-assets.ps1
.\tools\make-emoji-pack.ps1
```

Package the app repository entry:

```powershell
.\tools\package-app-repository.ps1
```

Output:

```text
dist/app-repository/nl.daandobber.matrixmatsu
dist/matrixmatsu-app-repository-package.zip
```

## Feature Context

- Emoji: `main/emoji_table.h` (generated) holds ~1650 emoji (codepoint key, UTF-8
  bytes, label, group). `text_with_emoji_markers()` in `main.c` recognizes any
  emoji in incoming/outgoing text (ZWJ sequences, skin tones and gender/hair
  variants are normalized down to the neutral entry) and encodes it as a 3-byte
  inline marker; `draw_emoji()` resolves that back to an SD-loaded raster asset,
  falling back to a small hand-drawn legacy icon set (~110 concepts) or a plain
  placeholder box when no SD card/asset is available. The emoji picker
  (`render_emoji_picker`) supports live search and TAB-cycled category filters
  over the same table.
- Audio messages are downloaded on demand from Matrix media and played locally.
- Side volume buttons are used by the audio path.
- Image messages are downloaded on demand and viewed through `image_viewer.c`.
- PNG and JPG are supported. JPG uses the software `esp_jpeg` decoder because
  the hardware JPEG path failed on real Matrix images with
  `ESP_ERR_INVALID_STATE`.
- The UI selects chat messages with the existing history navigation. Pressing
  enter on a media message requests or opens the media.

## Before Finishing

For firmware work, at minimum run the Tanmatsu build above. If the user is
testing on hardware, install via BadgeLink after a successful build.

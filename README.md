# MatrixMatsu

MatrixMatsu is a lightweight Matrix chat client for Tanmatsu and Konsool.

It is built for the Tanmatsu keyboard and small display, with a favorites-first room list, cached history, emoji assets loaded from storage, reactions, unread counts, themes, and a keyboard-driven UI.

## Status

This is early software. It can log in to a Matrix homeserver, sync rooms, send plain text messages, show recent history, and display a compact emoji set.

Known limitations:

- encrypted rooms are shown but sending encrypted messages is not supported
- history is cached per opened room, not mirrored for the whole account
- emoji support is asset-backed and intentionally limited for device memory

## Building

This project is based on the Tanmatsu ESP-IDF template.

Build for Tanmatsu:

```powershell
.\esp-idf\export.ps1
idf.py --no-ccache -B build/tanmatsu build -DDEVICE=tanmatsu -DSDKCONFIG_DEFAULTS='sdkconfigs/general;sdkconfigs/tanmatsu' -DSDKCONFIG=sdkconfig_tanmatsu -DIDF_TARGET=esp32p4 -DFAT=0
```

The resulting AppFS binary is:

```text
build/tanmatsu/application.bin
```

## Installing With Badgelink

With the device in Badgelink mode:

```powershell
.\install-badgelink.ps1 -NoBuild
```

By default this installs the app as `matrixmatsu` with the title `MatrixMatsu`.

## Emoji Assets

MatrixMatsu recognizes and can send the full "default" (no skin tone, no gender,
no hair style) Unicode emoji set, ~1650 emoji, sourced from
[Twemoji](https://github.com/twitter/twemoji). Assets are stored outside the
application binary (on the SD card) and loaded on demand, keyed by codepoint.

`tools/emoji-data.json` and `main/emoji_table.h` are generated, checked-in data.
Regenerate them only when Unicode adds new emoji:

```powershell
python tools\gen-emoji-data.py tools\emoji-test.txt tools\twemoji-filenames.txt tools\emoji-data.json
python tools\gen-emoji-table.py tools\emoji-data.json main\emoji_table.h
```

Then (re)build the raster assets and pack them:

```powershell
.\tools\make-emoji-assets.ps1
.\tools\make-emoji-pack.ps1
```

The app repository package uses one packed asset file:

```text
emoji.pak
```

## App Repository Package

After building the firmware and generating `emoji.pak`, create the Tanmatsu app-repository folder:

```powershell
.\tools\package-app-repository.ps1
```

The generated folder is:

```text
dist/app-repository/nl.daandobber.matrixmatsu
```

That folder is meant to be copied into a fork of:

```text
https://github.com/Nicolai-Electronics/app-repository
```

## License

MIT. See [LICENSE](LICENSE).

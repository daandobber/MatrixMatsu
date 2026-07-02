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

Emoji assets are stored outside the application binary and loaded on demand. To regenerate them:

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

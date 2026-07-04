# Project Map

## Core App

- `main/main.c`: UI, input handling, screen rendering, media viewer flow.
- `main/matrix_client.c`: Matrix API, sync, media download, media cache.
- `main/matrix_client.h`: Matrix client public API and constants.
- `main/audio_player.cpp`: audio decode/playback path when present in scope.
- `main/audio_player.h`: audio player API.
- `main/image_viewer.c`: PNG/JPG decode into PAX buffers.
- `main/image_viewer.h`: image viewer API.
- `main/CMakeLists.txt`: component sources and requirements.
- `main/idf_component.yml`: managed ESP-IDF component dependencies.

## Scripts

- `install-badgelink.ps1`: build/install/start app through BadgeLink.
- `tools/make-emoji-assets.ps1`: generate emoji source assets.
- `tools/make-emoji-pack.ps1`: pack emoji assets.
- `tools/package-app-repository.ps1`: create app repository folder and zip.

## Important Generated Or External Folders

- `build/tanmatsu/`: build output.
- `managed_components/`: ESP-IDF managed components.
- `badgelink_v020/`: BadgeLink tooling.
- `esp-idf/`: ESP-IDF checkout.
- `dist/`: generated packages.
- `emoji_assets/`: generated emoji pack files.

## Current Media Behavior

- Chat navigation selects a message.
- Enter on a media message downloads or opens the media on demand.
- Audio files are downloaded and played, with side volume buttons controlling
  playback volume.
- Images are downloaded and opened in a full-screen viewer.
- PNG is decoded locally through the image viewer.
- JPG is decoded locally through `esp_jpeg` software decoding.


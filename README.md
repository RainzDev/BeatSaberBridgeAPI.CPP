# BeatSaberBridgeAPI.CPP

A Discord Rich Presence bridge for Beat Saber on Meta Quest. Receives game events from a Beat Saber mod over HTTP and displays them in Discord via the Discord Social SDK.

## How it works

1. A Beat Saber Quest mod sends JSON payloads to `POST http://localhost:8080/sendData`
2. This bridge parses those events and updates your Discord Rich Presence accordingly

## Prerequisites

| Tool | Windows | macOS | Linux |
|------|---------|-------|-------|
| C++17 compiler | MinGW-w64 or MSVC | Xcode CLT (`xcode-select --install`) | GCC or Clang |
| CMake 3.10+ | [cmake.org](https://cmake.org/download/) | `brew install cmake` | `apt install cmake` |
| Git | [git-scm.com](https://git-scm.com/) | Included with Xcode CLT | `apt install git` |

> **Note for Windows users:** If you use CLion, it ships with bundled MinGW and CMake under `%LOCALAPPDATA%\Programs\CLion\bin\`. You can use those instead of installing separately.

## 1. Obtain the Discord Social SDK

1. Go to the [Discord Developer Portal](https://discord.com/developers/applications) and open (or create) your application.
2. Navigate to **Games → Social SDK → Downloads** in the left sidebar.
   > First-time visitors will be asked to fill in some information about their application before the Social SDK page becomes accessible.
3. Download the **Discord Social SDK** archive for your platform.
4. Extract the archive. Inside you will find a `discord_social_sdk` folder with this structure:

```
discord_social_sdk/
  bin/
    release/
      discord_partner_sdk.dll       # Windows
  lib/
    release/
      discord_partner_sdk.lib       # Windows import lib
      libdiscord_partner_sdk.so     # Linux
      libdiscord_partner_sdk.dylib  # macOS
  include/
    discordpp.h
    cdiscord.h
```

5. Copy that `discord_social_sdk` folder into the `lib/` directory at the root of this repository:

```
BeatSaberBridgeAPI.CPP/
  lib/
    discord_social_sdk/     <-- place it here
      bin/
      lib/
      include/
```

## 2. Set your Discord Application ID *(optional)*

The repository ships with a default application ID. If you want to use your own Discord application (e.g. to show a custom name or icon in Rich Presence), you can override it at runtime without recompiling:

```sh
./BeatSaberBridgeAPI --app-id YOUR_APPLICATION_ID_HERE
```

Alternatively, change the default by editing the `applicationId` variable at the top of `main.cpp`.

### Required art assets

If you use your own application, the following asset keys must be uploaded under **Rich Presence > Art Assets** in the Developer Portal for icons to appear correctly:

| Key | Description |
|-----|-------------|
| `quest` | Small icon shown during gameplay (e.g. the Meta Quest logo) |

If an asset key is missing the bridge will log a warning and automatically retry the presence update without the icon, so Rich Presence will still function.

Skip this step entirely if you are happy using the default application.

## 3. CI setup — SDK distribution for forks

The Discord Social SDK is not publicly redistributable, so CI cannot download it from a public URL. The build workflow in this repository pulls the SDK from a private GitHub repository using a personal access token. If you fork this project and want CI to work, you need to replicate this setup.

### How it works

`scripts/setup-sdk.sh` reads two environment variables injected by the workflow — one from a regular Actions variable, one from a secret:

| Name | Kind | Description |
|------|------|-------------|
| `SDK_DOWNLOAD_URL` | Variable | Direct download URL to the SDK zip in your private release |
| `SDK_ASSET_NAME` | Variable | The file name of the SDK zip in your private release |
| `SDK_DOWNLOAD_TOKEN` | Secret | A GitHub PAT with read access to that private repository |

The script downloads the zip, verifies its SHA-256 against the hash in `scripts/sdk-sha256.txt`, then extracts it. The extracted SDK is cached in CI keyed by that hash, so the download only happens when the SDK version changes.

### Steps to replicate

1. **Create a private GitHub repository** (e.g. `you/discord-sdk-assets`).

2. **Create a release** (e.g. tag `v1.9.15332`) and upload the SDK zip obtained from the Discord Developer Portal as a release asset. The filename must match `ASSET_NAME` in `setup-sdk.sh`.

3. **Create a fine-grained personal access token:**
   - Go to **GitHub → Settings → Developer settings → Personal access tokens → Fine-grained tokens**
   - Set repository access to **only** the private assets repo
   - Grant **Contents: Read-only** — nothing else is needed

4. **Add to your fork** under **Settings → Secrets and variables → Actions:**
   - Under **Variables**: add `SDK_DOWNLOAD_URL` — the release asset download URL, e.g. `https://github.com/you/discord-sdk-assets/releases/download/v1.9.15332/DiscordSocialSdk-1.9.15332.zip`
   - Under **Variables**: add `SDK_ASSET_NAME` — the release asset name, e.g. `DiscordSocialSdk-1.9.15332.zip`
   - Under **Secrets**: add `SDK_DOWNLOAD_TOKEN` — the PAT from the previous step

5. **Create File `scripts/{asset name}-sha256.txt`** if you are using a different SDK version, replace `{asset name}` with your `SDK_ASSET_NAME` **without** the file extension. Compute the SHA-256 of your zip:
   - Linux / macOS: `sha256sum DiscordSocialSdk-*.zip`
   - Windows: `Get-FileHash DiscordSocialSdk-*.zip -Algorithm SHA256`

## 4. Build

All platforms use the same CMake flow. Run from the repository root.

### Windows (MinGW)

```bat
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

If using CLion's bundled toolchain, add its MinGW `bin` to `PATH` first so the assembler and linker are found, then specify the compiler paths explicitly:

```bat
set CLION=%LOCALAPPDATA%\Programs\CLion\bin
set PATH=%CLION%\mingw\bin;%PATH%
cmake -S . -B build ^
  -G "Ninja" ^
  -DCMAKE_MAKE_PROGRAM="%CLION%\ninja\win\x64\ninja.exe" ^
  -DCMAKE_C_COMPILER="%CLION%\mingw\bin\gcc.exe" ^
  -DCMAKE_CXX_COMPILER="%CLION%\mingw\bin\g++.exe" ^
  -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### macOS

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Linux

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The build output lands in `build/`.

## 4. Run

### Windows

The post-build step automatically copies `discord_partner_sdk.dll` next to the executable. You also need the three MinGW runtime DLLs in the same directory. Copy them from your MinGW `bin/` folder once:

```bat
set MINGW=C:\path\to\mingw\bin
copy "%MINGW%\libstdc++-6.dll"      build\
copy "%MINGW%\libgcc_s_seh-1.dll"   build\
copy "%MINGW%\libwinpthread-1.dll"   build\
```

Then run:

```bat
build\BeatSaberBridgeAPI.exe
```

> **Tip:** To avoid shipping the MinGW runtime DLLs, you can link statically by adding `-DCMAKE_EXE_LINKER_FLAGS="-static"` to the CMake configure step. This produces a larger but fully self-contained executable.

### macOS / Linux

The RPATH is set to `$ORIGIN` / `@executable_path` so the shared library is found automatically next to the binary:

```sh
./build/BeatSaberBridgeAPI
```

### Options

| Flag | Default | Description |
|------|---------|-------------|
| `--port <n>` | `8080` | HTTP port to listen on |
| `--app-id <id>` | built-in | Override the Discord application ID |
| `--help` | — | Print usage |

On first run, Discord's OAuth2 flow will open in your browser to authorize the application. After that the bridge connects and begins updating your Rich Presence.

## HTTP API

The bridge exposes a single endpoint that the Beat Saber mod posts to.

**`POST /sendData`**

```json
{
  "type": "BeatmapInitialized",
  "title": "Song Title",
  "author": "Song Author",
  "difficulty": "ExpertPlus",
  "duration": 180,
  "mappers": ["MapperOne", "MapperTwo"]
}
```

### Supported event types

| Event | Description |
|-------|-------------|
| `BeatmapInitialized` | A level has started |
| `BeatmapCleared` | Level completed |
| `BeatmapFailed` | Level failed |
| `BeatmapPaused` | Level paused |
| `BeatmapResumed` | Level resumed |
| `BeatmapRestarted` | Level restarted |
| `BeatmapStatUpdate` | Shows updated stats of current beatmap every 10 seconds |
| `MainMenuInitialized` | Returned to main menu |
| `LevelSelectionMenuInitialized` | In the level selection screen |
| `LobbyPlayerOnConnect` | Player joined a multiplayer lobby |
| `LobbyPlayerOnDisconnect` | Player left a multiplayer lobby |
| `MultiplayerBeatmapInitialized` | Multiplayer level started |
| `HeartbeatReceiver` | Heartbeats to shown sign of life for game |

After 1 minute of no events the Rich Presence is cleared automatically.

# show23-pc launcher

![Night Game artwork](docs/social-preview.png)

A Windows launcher for playing your own copy of MLB The Show 23 (PS4, `CUSA35521`) on PC. It sets the game up from your own game files, starts the bundled native game engine and a small local server that stands in for the retired Diamond Dynasty online service, and can import the special cards from the publisher's public website on request. The launcher never downloads or distributes game files.

The release includes Setup, Play, Diamond Dynasty against the local server and optional special-card import. Read [Known limits](#known-limits) before you start.

## For players

### What you need

- Windows 10 or 11, 64-bit.
- A graphics card with Vulkan support and current drivers.
- Your own, legally obtained copy of MLB The Show 23 for PS4 (`CUSA35521`), version 1.00, either as an unpacked folder or as a package file. The 1.21 update is optional; if you have it, most special-card pictures come from it.
- If you set up from a package file: an unpacker program of your own (`orbis-unpkg.exe` or `ps4-pkg-tool.exe`) and about 60 GB of free disk space for the unpacked copy. An unpacked folder is used where it is and is not copied.

### Download and set up

1. Download the latest `*-win64-native-portable.zip` from the [Releases page](https://github.com/ChronoRixun/show23-pc-launcher/releases) and extract the whole zip to a folder of your choice. Keep `show23-pc.exe` and the `show23-pc` folder next to each other.
2. Run `show23-pc.exe` and choose **Set up**.
3. Choose your base game: an unpacked 1.00 folder, or a package file together with your unpacker. You can add the 1.21 update the same way (folder with folder, package with package). The first time you use an unpacker, the launcher shows its full path and asks before running it.
4. Pick a fresh install folder. The launcher checks the game files, records where they are and copies the bundled game engine and server into the install. It never writes inside an unpacked game folder.

**Verify Files** checks the game folders and the installed runtime when Play is stopped.

The first native release, 0.2.0, used a fresh ZIP and a new game install. Native installs from 0.2.0 can update in place. Earlier releases remain available on the Releases page.

### Play

Press **Play**. The first launch prepares game files and can take a few minutes. The game creates fresh saves and shows its first-launch tutorial. **Stop** cancels preparation or Play. When you close the game, the local server stops too.

Set the local player name, **Fullscreen**, and **Resolution** in Settings. Resolution defaults to **1080p (1920x1080)**. Choose **720p (1280x720)** for a smaller window. Fullscreen uses the desktop dimensions.

Play always starts version 1.00 of the game, also when the 1.21 update is set up. The update is used for card pictures; this release has no way to play 1.21 itself.

Diamond Dynasty uses the local server account. You start with a 26-player squad of Common cards from the game's own roster and 5,000 Stubs. Packs, exchanges, Programs (including Team Affinity), Showdown, Mini Seasons and Conquest all work against the local server. The Community Market and buying Stubs are switched off, and there is no play against other people.

### Import special cards (optional)

Without an import you have the Live roster cards from your game files, with player photos where the game has them. **Import special cards** on the Home page (also offered after setup) adds the special cards:

- Your own PC downloads the card list, and only the pictures your game files lack, from the publisher's public website (`mlb23.theshow.com`). That is about 35 MB and takes about 5 minutes.
- It adds about 1,775 special cards with their designed art, mostly taken from your own 1.21 update files, and 40 event packs (Chase Packs, Diamond Duos and the Lou Gehrig Day pack).
- About 211 special cards are left out because they could not be matched to a player in the game's roster.
- Stop the game first, and turn off Offline mode. You can cancel; finished pages are kept and the next run continues. **Update special cards** runs it again later.

The import is optional and runs only when you press the button. It uses a website run by the publisher, so please be aware of that site's terms before you use it. Nothing is sent to this project.

### Windows command-line play

The portable release includes `manual/README.txt`. From the extracted folder, run `manual\setup.cmd <install> <base> [update]` with unpacked game folders, then `manual\play.cmd <install> [--fullscreen]`. The launcher window is optional. You can select the same native install later in the launcher.

### Where your files are

Inside your install folder:

| What | Where |
|---|---|
| Game saves and settings | `data\profiles\default\engine\` |
| Diamond Dynasty progress (cards, Stubs, modes), this install's certificates | `data\profiles\default\server\` |
| Imported special cards | `data\import-cache\` |
| Preparation, engine, server and card-import logs | `data\logs\` |

Back up `data\profiles` together with the install's `manifests` folder to keep your progress and runtime records. Next to `show23-pc.exe`, the launcher keeps its settings and embedded browser logs under `show23-pc\user\`. **Support → Open logs folder** opens `show23-pc\data\logs\`, which contains the launcher log.

### Known limits

- Frame rate: menus run smoothly, but games have small stutters and do not hold a steady 60 frames a second.
- A full CPU game has not yet been played from start to finish in the release build.
- The Captain card feature is unverified.
- About 211 special cards are left out of the import because they could not be matched to a roster player.
- Choice packs are not supported.
- The game engine targets 64-bit Windows. Trophy tracking is off in this release.
- You need your own legally obtained copy of the game. Nothing here contains game files or card art.

### Reporting problems

Open an issue on [GitHub](https://github.com/ChronoRixun/show23-pc-launcher/issues). Say what you did and what happened, paste the text from **Support → Copy diagnostics** (read it first) and attach the newest logs from `data\logs\` in your install. For version selection and startup issues, also attach `show23-pc\versions\bootstrap.log` from the portable launcher folder. Please never post game files, keys, accounts or links to them.

## For builders

### Build from source

Use Windows x64, Visual Studio 2022 or its Build Tools with the Desktop development with C++ workload, Windows SDK and C++ ATL, Premake 5 with Visual Studio 2022 support, and PowerShell. Put `premake5.exe` on PATH or set `SHOW23_PREMAKE` to its full path. The generator obtains pinned dependency sources and the Chromium Embedded Framework (CEF) runtime, approximately 260 MB. A plain source archive without git history also builds.

~~~bat
generate.bat
msbuild build\show23-pc.sln /m /p:Configuration=Release /p:Platform=x64
build\bin\x64\Release\show23-pc-tests.exe
node src\tests\ui\run.js
~~~

The build produces the real launcher at `build\bin\x64\Release\show23-pc.exe` and the bootstrap at `build\bin\x64\Release\show23-pc-bootstrap.exe`. For an offline runtime staging check, use `tools\stage-runtime.ps1` with the same local inputs as the packager. For a launcher test, extract the portable ZIP and use Setup. In a release ZIP, browser files are kept with their version and the Settings data location control moves user settings only.

For source setup, `generate.bat` fetches the five repositories listed in `.gitmodules` from GitHub when initialized as submodules. In a plain source directory it fetches the pinned source archives from `https://codeload.github.com/<owner>/<repo>/zip/<commit>`. CEF is fetched from `https://cef-builds.spotifycdn.com/<cef-build>.tar.bz2`. These are developer build-time contacts.

### Package the playable release

Set `SHOW23_PACKAGE_VERSION` from `tools/release-version.txt` before building Release x64. After the build, supply the engine package and staged Windows server. If the matching manual script or server licence is outside the server folder, pass its path explicitly.

```powershell
$env:SHOW23_PACKAGE_VERSION = (Get-Content -Raw tools/release-version.txt).Trim()
msbuild build/show23-pc.sln /m /p:Configuration=Release /p:Platform=x64
.\tools\package-release.ps1 -EnginePackage $engine -ServerFolder $server -ManualMode $manual -ServerLicense $serverLicense
```

The engine folder supplies `MANIFEST.tsv`, the executable, DLLs, defaults and preparation scripts. The server folder supplies the Python program, its replies and tables, embedded Python, and optional local Pillow files. The packager reads local files and creates the portable archive, `launcher.json` and `SHA256SUMS.txt`. `package-portable.ps1` accepts the same inputs. To stage a runtime without an archive, use `stage-runtime.ps1` with those inputs and a fresh `-OutDir`.

Before publishing, fill in the corresponding-source link in `tools/engine-source.txt`. Provide the bundled libraries' required licence texts and notices under `LICENSES/` in the engine input and list them in its manifest. For later releases, use `-MinLauncher` to name the oldest compatible native launcher when offering an in-app update.

### Update channels

Stable selects final native releases and Beta includes native pre-releases. The game engine, local server and profile arrive in the launcher download. After updating and restarting the launcher, stop Play and choose **Update game files** on Home or in **Settings > Updates**. Activation keeps your game install, saves, settings, and Diamond Dynasty progress. **Verify Files** checks the selected slot.

**Restore previous game files** in Settings is available while the retained version is compatible with your progress. Starting a newer local server can close that rollback window. **Repair runtime** restores damaged files from a matching bundle. See [Release manifests](docs/release-manifest.md).

## Privacy

The launcher does not send game paths, game files, saves, content packs or diagnostics. Diagnostics are displayed locally and can be copied to the clipboard.

| Address | When |
| --- | --- |
| `https://api.github.com/repos/ChronoRixun/show23-pc-launcher/releases/latest` on Stable, or `https://api.github.com/repos/ChronoRixun/show23-pc-launcher/releases?per_page=30` on Beta | Startup for tagged release builds, a channel change, or a player-requested update check |
| `https://github.com/ChronoRixun/show23-pc-launcher/releases/download/<tag>/<asset>` | After the player approves an update. Assets are `launcher.json`, `SHA256SUMS.txt` and `show23-pc-<version>-win64-native-portable.zip` |
| `https://release-assets.githubusercontent.com/…` and `https://objects.githubusercontent.com/…` | When GitHub redirects a release asset download |
| `https://github.com/ChronoRixun/show23-pc-launcher/releases/tag/v<version>` | Opens the offered version's release page in the default browser when requested from the updater |
| `https://mlb23.theshow.com/apis/items.json?type=mlb_card&page=<n>` and the picture addresses that list names | Only when the player chooses Import special cards. The bundled server makes these requests, at least 750 ms apart, with a generic user agent and nothing about the player |

The embedded interface uses `http://show23-pc.invalid/` internally. CEF serves that origin from local files and native commands. The default browser and Windows handle their own networking, including certificate validation. Play starts the local server on 127.0.0.1 and the game talks only to it. Use `-offline` at startup or enable **Offline mode** in Settings and restart to disable the launcher's release contacts and the card import.

## Legal

This is an unofficial fan project. It is not affiliated with or endorsed by Sony Interactive Entertainment, San Diego Studio, Major League Baseball or the MLB Players Association. *MLB The Show* and related names are trademarks of their owners and are used only to identify what the software works with. You need your own legally obtained copy of the game; this project redistributes no game files and no card art.

## Credits

- [mlb_pc](https://github.com/victorgbd/mlb_pc) and its contributors provided the basis for the native game engine. See `LICENSES/engine/SOURCE.txt` in the portable version folder for the corresponding source.
- This launcher is adapted from [Ultimate Legends](https://github.com/ChronoRixun/ultimate-legends), which descends from the [CB Servers Launcher](https://github.com/CBServers/cb-launcher) and the [XLabs Launcher](https://github.com/XLabsProject/launcher).
- [showdd.io](https://www.showdd.io/), a community card database, was a reference during research for which cards past Chase Packs and Diamond Duos featured. The software does not use it.

### Bundled software

The version folder contains the launcher and engine notices under `LICENSES`. Python and Pillow carry their own notices with the server files.

| Component | Licence | Notice path |
| --- | --- | --- |
| This launcher | GPL-3.0 | `launcher-GPL-3.0.txt` |
| show23-pc server | MIT | `server.txt` |
| Native game engine and preparation scripts | GPL-2.0 | `engine/GPL-2.0.txt`, `engine/SOURCE.txt` |
| Engine dependencies, including FFmpeg (LGPL-2.1-or-later build) | Per library | `engine/DEPENDENCIES.tsv` and bundled notices |
| [Chromium Embedded Framework](https://bitbucket.org/chromiumembedded/cef) 113 with Chromium | BSD-3-Clause; Chromium's components under their own licences | `browser.txt` |
| [libcurl](https://curl.se/) | curl licence | `curl.txt` |
| [RapidJSON](https://github.com/Tencent/rapidjson) | MIT | `rapidjson.txt` |
| [miniz](https://github.com/richgel999/miniz) | MIT | `miniz.txt` |
| [LibTomCrypt](https://github.com/libtom/libtomcrypt) and [LibTomMath](https://github.com/libtom/libtommath) | The LibTom license (public domain) | `libtomcrypt.txt`, `libtommath.txt` |
| [Python](https://www.python.org/) 3.14.7 embeddable | PSF License 2.0, with the notices for its Windows build | `runtime/<version>/server/python/LICENSE.txt` |
| [Pillow](https://python-pillow.org/) 13.0.0 | MIT-CMU, with the notices for the image libraries it bundles | `runtime/<version>/server/vendor/pillow-*.dist-info/licenses/` |
| [Oswald](https://github.com/googlefonts/OswaldFont) font | SIL Open Font License 1.1 | `OFL.txt` |

## Licence

The launcher is licensed under the [GNU General Public License v3.0](LICENSE). The interface bundles Oswald under the [SIL Open Font License 1.1](src/launcher-ui/assets/fonts/OFL.txt). The game engine and the server are separate programs under their own licences.

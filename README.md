# show23-pc launcher

![Night Game artwork](docs/social-preview.png)

A Windows launcher for running your own copy of MLB The Show 23 with the companion emulator build and local card-mode server. The launcher never downloads or distributes game files.

## Status

Set up verifies an unpacked 1.00 base folder and an optional unpacked 1.21 update folder, then records their locations in the selected install folder. The game folders stay where they are. The launcher can identify packages, but you need your own tool to unpack them. Verify Files checks the recorded game folders and any installed runtime. Runtime installation, repair, and starting the server and emulator are pending, so setting up the game does not enable Play. Mods manages local content pack folders under `data/mods/`.

## Build from source

Use Windows x64, Visual Studio 2022 or its Build Tools with the Desktop development with C++ workload, Windows SDK and C++ ATL, Premake 5 with Visual Studio 2022 support, and PowerShell. Put `premake5.exe` on PATH or set `SHOW23_PREMAKE` to its full path. The generator obtains pinned dependency sources and the Chromium Embedded Framework (CEF) runtime, approximately 260 MB. Source snapshots also work from a plain source directory.

~~~bat
generate.bat
msbuild build\show23-pc.sln /m /p:Configuration=Release /p:Platform=x64
build\bin\x64\Release\show23-pc-tests.exe
node src\tests\ui\run.js
~~~

The Release executable is `build\bin\x64\Release\show23-pc.exe`. To run from the build output in installed mode, first stage its runtime with `powershell -File tools\stage-runtime.ps1`. In installed mode the launcher uses `%LOCALAPPDATA%\show23-pc`. To prepare a portable archive locally, run `powershell -File tools\package-portable.ps1 -Version 0.1.0` after building Release. The archive stages `show23-pc.exe` next to its `show23-pc\portable.marker` and browser files. Game data belongs in the separate chosen install layout described in the [design](docs/design.md). The Settings data location control moves launcher settings and the embedded browser between portable and installed modes.

For source setup, `generate.bat` fetches the five repositories listed in `.gitmodules` from GitHub when initialized as submodules. In a plain source directory it fetches the pinned source archives from `https://codeload.github.com/<owner>/<repo>/zip/<commit>`. CEF is fetched from `https://cef-builds.spotifycdn.com/<cef-build>.tar.bz2`. These are developer build-time contacts.

## Update channels

Stable is the default and selects final releases. Beta includes pre-releases. Settings → Updates shows the installed version for the launcher, emulator, server and profile. A pre-release is marked Beta. The launcher asks before switching a portable Beta back to an older Stable release. Keep the beta leaves it installed. Tagged Release builds in installed mode can open the release page for a manual update. When published releases are unavailable the update check reports that fact and stops. Release assets and compatibility rules are documented in [Release manifests](docs/release-manifest.md).

## Privacy

The launcher does not send game paths, game files, saves, content packs or diagnostics. Diagnostics are displayed locally and can be copied to the clipboard.

| Address | When |
| --- | --- |
| `https://api.github.com/repos/ChronoRixun/show23-pc-launcher/releases/latest` on Stable, or `https://api.github.com/repos/ChronoRixun/show23-pc-launcher/releases?per_page=30` on Beta | Startup for tagged release builds, a channel change, or a player-requested update check |
| `https://api.github.com/repos/ChronoRixun/show23-pc-emulator/releases/latest` on Stable, or `https://api.github.com/repos/ChronoRixun/show23-pc-emulator/releases?per_page=30` on Beta | When the player chooses Check for updates |
| `https://api.github.com/repos/ChronoRixun/show23-pc/releases?per_page=30` | When the player chooses Check for updates. The server and profile share the listing |
| `https://github.com/ChronoRixun/show23-pc-launcher/releases/download/<tag>/<asset>` | After the player approves an update. Assets are `launcher.json`, `SHA256SUMS.txt` and `show23-pc-<version>-win64-portable.zip` |
| `https://release-assets.githubusercontent.com/…` and `https://objects.githubusercontent.com/…` | When GitHub redirects a release asset download |
| `https://github.com/ChronoRixun/show23-pc-launcher/releases/latest` | Opens in the default browser when requested from the updater |

The embedded interface uses `http://show23-pc.invalid/` internally. CEF serves that origin from local files and native commands. The default browser and Windows handle their own networking, including certificate validation. The current launcher has no catalogue importer and does not start the local server. Use `-offline` at startup or enable **Offline mode** in Settings and restart to disable the launcher's release contacts.

## Legal

This is an unofficial fan project. It is not affiliated with Sony Interactive Entertainment, San Diego Studio, Major League Baseball or the MLB Players Association. Names are used only to identify what the software works with.

## Credits

This launcher is adapted from [Ultimate Legends](https://github.com/ChronoRixun/ultimate-legends) by the same author. Ultimate Legends is a fork of the [CB Servers Launcher](https://github.com/CBServers/cb-launcher) by CB Servers (© 2026 CB Servers). Its foundation is the [XLabs Launcher](https://github.com/XLabsProject/launcher), originally developed by [momo5502](https://github.com/momo5502) and the XLabs team. The lead author also added the scope guard’s cancellation method.

## Licence

The interface bundles Oswald under the [SIL Open Font License 1.1](src/launcher-ui/assets/fonts/OFL.txt).

The launcher is licensed under the [GNU General Public License v3.0](LICENSE).

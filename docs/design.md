# Launcher design

The desktop interface and update model follow the companion project's [launcher design](https://github.com/ChronoRixun/show23-pc/blob/main/docs/launcher.md), [builder design](https://github.com/ChronoRixun/show23-pc/blob/main/builder/README.md) and [architecture](https://github.com/ChronoRixun/show23-pc/blob/main/docs/architecture.md).

| File | Role |
| --- | --- |
| [setup_run.cpp](../src/launcher/show23/setup_run.cpp) | Setup stages and publication order |
| [setup_verification.cpp](../src/launcher/show23/setup_verification.cpp) | Read-only checks shared by setup and Verify Files |
| [game_record.cpp](../src/launcher/show23/game_record.cpp) | Saved game paths and startup readiness |
| [part_manifest.hpp](../src/launcher/updater/part_manifest.hpp) | Part manifest and install admission |
| [part_store.cpp](../src/launcher/updater/part_store.cpp) | Active runtime metadata, Ready state and Verify Files |
| [release-manifest.md](release-manifest.md) | Release assets, compatibility rules and install records |

Setup uses verified unpacked folders in place. The install folder contains launcher files, including `data/games/CUSA35521/game.json`. Setup writes the game record before `manifests/game-ready.txt`, then remembers the install folder. Startup checks that the marker and record are valid and that each recorded executable still exists at the saved size. Verify Files rechecks both folders and checks any installed runtime against its manifests. The bundled runtime can be staged again if its installed files fail verification. Download-based runtime installation is pending. Mods handles local pack folders and their enabled state. Pack schema validation and loading belong to the runtime.


## Package setup

Package setup extracts into a uniquely named staging folder beneath the selected install's game directory. After the same folder verification used for an in-place install, it moves the verified base and optional update into the game directory, writes a record with mode "installed", and publishes the ready marker last. The previous installed copy is moved aside during publication and the launcher rolls it back if publication fails. The Home removal action applies only to an installed copy and keeps the original packages, profiles, logs and other launcher data.

Choosing a player-supplied unpacker is a trust decision. Before running it, the launcher shows the full path and asks for consent to run the executable with the player's permissions. Consent is remembered for that path and SHA-256. A bundled program uses the release's trust and needs no separate consent. The job controls process lifetime, and the launcher validates the extraction result and checks for direct sibling writes. This is not a filesystem sandbox.

The supplied program takes precedence over a bundled program. For portable releases, put unpacker/orbis-unpkg.exe or unpacker/ps4-pkg-tool.exe in the show23-pc data directory beside the launcher executable. For installed mode, put it in %LOCALAPPDATA%/show23-pc/unpacker. If both are bundled, the launcher tries orbis-unpkg.exe first.


## Play and the bundled runtime

The playable portable release ships the runtime under the launcher's portable data root. The launcher verifies the bundle, stages only manifest-listed runtime files into the chosen install, and activates the version through manifests/runtime.txt after verification. A verified active runtime takes precedence. Setup's data directory stays outside publication. Release discovery for runtime parts is separate from this bundled installation.

Play rechecks the active runtime. The game record supplies the base folder for both installed and in-place mode. If needed, the server generates the local CA with the bundled interpreter. The launcher waits for connections to both loopback ports owned by the server, then starts the emulator with the CA and uses data/profiles/default as its working directory. The emulator's exit, Stop, or confirmed launcher close ends the server job. Play always starts version 1.00, including for a record with an optional offline update.

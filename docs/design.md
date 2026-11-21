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

Setup uses verified unpacked folders in place. The install folder contains launcher files, including `data/games/CUSA35521/game.json`. Setup writes the game record before `manifests/game-ready.txt`, then remembers the install folder. Startup checks that the marker and record are valid and that each recorded executable still exists at the saved size. Verify Files rechecks both folders and checks any installed runtime against its manifests. Repair and runtime installation are pending. Mods handles local pack folders and their enabled state. Pack schema validation and loading belong to the runtime.

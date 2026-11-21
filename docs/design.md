# Launcher design

The desktop interface and update model follow the companion project's [launcher design](https://github.com/ChronoRixun/show23-pc/blob/main/docs/launcher.md), [builder design](https://github.com/ChronoRixun/show23-pc/blob/main/builder/README.md) and [architecture](https://github.com/ChronoRixun/show23-pc/blob/main/docs/architecture.md).

| File | Role |
| --- | --- |
| [setup.hpp](../src/launcher/show23/setup.hpp) | Setup inputs, stage outputs and process-start contracts |
| [part_manifest.hpp](../src/launcher/updater/part_manifest.hpp) | Part manifest and install admission |
| [part_store.cpp](../src/launcher/updater/part_store.cpp) | Active runtime metadata, Ready state and Verify Files |
| [release-manifest.md](release-manifest.md) | Release assets, compatibility rules and install records |

The Setup screen remembers the selected install folder so Mods and Verify Files can locate it. A future completed adoption step publishes the game-ready marker. Verify Files reads and hashes the current runtime file list. Repair, game file verification, and runtime installation are pending. Mods handles local pack folders and their enabled state. Pack schema validation and loading belong to the runtime.

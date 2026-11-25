# Release manifests

Send a UTF-8 JSON manifest with each part release. Each manifest is capped at 256 KiB and uses schema 1.

| Part | Repository | Tag | Manifest |
| --- | --- | --- | --- |
| Launcher | ChronoRixun/show23-pc-launcher | v1.2.3 | launcher.json |
| Game engine | Bundled with the launcher | Launcher version | emulator.json |
| Server | ChronoRixun/show23-pc | server-v1.2.3 | server.json |
| Profile | ChronoRixun/show23-pc | profile-v1.2.3 | profile.json |

A final version uses channel `stable`. A pre-release such as `1.2.3-beta.1` uses channel `beta` and the GitHub pre-release flag. The shared repository uses part-prefixed tags so the server and profile can publish independently, even at identical versions. Discovery reads the shared repository's most recent 30 releases and selects the highest matching version for each part and channel.

Example `server.json` for a three-byte fixture whose contents are `abc`.

~~~json
{
  "schema": 1,
  "part": "server",
  "version": "1.2.3",
  "channel": "stable",
  "min_launcher": "0.1.0",
  "files": [
    {
      "path": "bin/server.exe",
      "size": 3,
      "sha256": "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    }
  ],
  "compatibility": {
    "launcher": ">=0.1.0 <2.0.0",
    "emulator": ">=1.5.0",
    "profile": "=2.0.0"
  }
}
~~~

Versions are three numeric components with an optional lowercase SemVer pre-release. Build metadata is excluded from this schema. Give exactly one compatibility entry for each other part. Use `*` for no dependency, or one or more space-separated comparisons using `<`, `<=`, `=`, `==`, `>=` or `>`. All comparisons must pass. Every present manifest must accept the proposed combination. The candidate's `min_launcher` applies to the launcher doing the installation.

Each `size` is an unsigned byte count and each `sha256` is 64 lowercase hexadecimal digits. Use relative paths with forward slashes and ASCII letters, digits, dots, underscores, hyphens and plus signs. Paths must be unique on Windows. Duplicate and unknown fields are rejected. A launcher manifest lists exactly the downloadable `show23-pc-<version>-win64-native-portable.zip`. Its digest must match the entry for the same file in `SHA256SUMS.txt`, and its size must match the GitHub asset and the downloaded bytes.

The first native release requires a fresh ZIP extraction. Publish the native asset name as written. Older updaters require a differently named ZIP and leave this release alone. The `emulator` manifest key and component path remain as on-disk protocol identifiers.

The install's `manifests/runtime.txt` provides the active release directory name, a version in the same syntax. The corresponding `manifests/<release>/emulator.json`, `server.json` and `profile.json` list files relative to `runtime/<release>/emulator`, `runtime/<release>/server` and `runtime/<release>/tools/profile`, respectively. Verify Files checks these files without writing to the install. The game uses a separate in-place record at `data/games/CUSA35521/game.json` and a `manifests/game-ready.txt` marker containing `CUSA35521`. The launcher writes these during folder setup. Verify Files rechecks the recorded game folders as well as the runtime. A portable launcher update verifies and publishes only the new version directory. It keeps the release manifest beside that directory. The bootstrap confirms the new launch before making it current. Rollback changes the pointer and keeps both version directories in place.

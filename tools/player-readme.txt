show23-pc 0.1.0

Play your own copy of MLB The Show 23 (PS4, CUSA35521) on Windows, with Diamond Dynasty kept alive by a small server that runs on your own PC.

This is an unofficial fan project. It is not affiliated with or endorsed by Sony Interactive Entertainment, San Diego Studio, Major League Baseball or the MLB Players Association. Names are used only to identify what the software works with. You need your own legally obtained copy of the game. This download contains no game files and no card art.

Requirements
Windows 10 or 11 x64, a Vulkan-capable GPU, and your own copy of MLB The Show 23 for PS4, version 1.00: an unpacked folder, or a game package plus an unpacker program of your own (orbis-unpkg.exe or ps4-pkg-tool.exe). The 1.21 update is optional. Unpacking a package needs about 60 GB of free space.

Set up
Extract the entire zip and run show23-pc.exe. Select Set up, choose your game (and the update if you have it), and pick a separate install folder. The launcher checks your files and copies the bundled runtime into that install. An unpacked folder is used where it is; nothing is written inside it.

Play
Select Play. The first time, choose a player name. Play starts the local server and then the game; closing the game stops both. Play always starts game version 1.00, even when the 1.21 update is set up (the update is used for card pictures).
Settings has Fullscreen and Resolution. The default is 1080p (1920x1080). Choose 720p (1280x720) if you prefer. The new size applies the next time you press Play.
In Diamond Dynasty you start with a 26-player squad of Common cards from the game's roster and 5,000 Stubs. Packs, exchanges, programs, Showdown, Mini Seasons and Conquest work. The Community Market and buying Stubs are off, and there is no play against other people.

To set up or play without the launcher, follow manual/README.txt.

Import special cards (optional)
Import special cards on the Home page makes your PC download the card list, and only the pictures your game files lack, from the publisher's public website (about 35 MB, about 5 minutes). It adds about 1,775 special cards with their designed art, mostly from your own 1.21 update files, and 40 event packs (Chase Packs, Diamond Duos). Stop the game and turn off Offline mode first. It runs only when you press the button; please be aware of that website's terms. Without it you have the Live roster cards with player photos.

Your files (inside the install folder)
Saves: data/profiles/default/user
Diamond Dynasty progress: data/profiles/default/server
Logs: data/logs
Back up data/profiles to keep your progress.

Known limits
- Small stutters, and games do not hold a steady 60 frames a second.
- A full CPU game has not yet been played start to finish in this release.
- The Captain card feature is unverified.
- About 211 special cards are left out because they could not be matched to a roster player.
- Choice packs are not supported.
- The launcher's self-update has not been tried yet; if it fails, download the new zip by hand.

Problems
Open an issue at https://github.com/ChronoRixun/show23-pc-launcher/issues with the text from Support > Copy diagnostics and the newest logs. Never post game files, keys or accounts.

Credits
Built on the shadPS4 emulator and its contributors (GPL-2.0-or-later). Thanks to the mlb_pc port of MLB The Show 24, whose fix notes informed our engine fixes. Full credits: the Credits section of the launcher's README at https://github.com/ChronoRixun/show23-pc-launcher

Licences and source: see LICENSES/. Emulator source: @EMULATOR_SOURCE@

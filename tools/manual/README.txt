Windows command-line play

Extract the entire native portable ZIP into a new folder. Use these commands
from the extracted folder, next to show23-pc.exe. Choose a fresh install folder
and use your own unpacked 1.00 game. The unpacked 1.21 update is optional.

  manual\setup.cmd "C:\Games\Show23 install" "C:\Games\Show23 base" "C:\Games\Show23 update"
  manual\import-cards.cmd "C:\Games\Show23 install"
  manual\play.cmd "C:\Games\Show23 install" --fullscreen --resolution 720 --name "Player One"

The import-cards command is optional. Omit --fullscreen for windowed play.
Use --resolution 720 or --resolution 1080 to set the initial window size.
Fullscreen uses the desktop dimensions. The local name defaults to Player.
Names take 3 to 16 letters, digits, spaces, hyphens or underscores. The local
server uses its fixed Diamond Dynasty account.

The first launch can take a few minutes. The game starts with fresh saves
and shows the first-launch tutorial. The local server stops when the game
exits or you press Ctrl+C. This release supports 64-bit Windows.

After installing a launcher update, close Play and use the same setup command
with only your existing native install folder to activate the bundled runtime:

  manual\setup.cmd "C:\Games\Show23 install"

Add --rollback to restore its previous runtime when the local server's
progress version allows it. The install's saves and settings stay in place.

Saves and settings are under data/profiles/default/engine. The local server
keeps progress and certificates under data/profiles/default/server. Session
logs are under data/logs. You can select this native install in the launcher.

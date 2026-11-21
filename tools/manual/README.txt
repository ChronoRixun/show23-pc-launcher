Manual mode (no launcher)

Extract the entire portable ZIP. Run these examples from the extracted folder,
next to show23-pc.exe. Use your own unpacked game folders and choose a separate
install folder. The 1.21 update is optional. Manual mode needs unpacked folders.

Windows
  manual\setup.cmd "C:\Games\Show23 install" "C:\Games\Show23 base" "C:\Games\Show23 update"
  show23-pc\runtime\0.1.0\server\python\python.exe show23-pc\runtime\0.1.0\server\olserver.py import-cards --install "C:\Games\Show23 install" --pictures
  manual\play.cmd "C:\Games\Show23 install" --fullscreen

Linux
  sh manual/setup.sh "$HOME/show23-install" "$HOME/show23-base" "$HOME/show23-update"
  python3 show23-pc/runtime/0.1.0/server/olserver.py import-cards --install "$HOME/show23-install" --pictures
  sh manual/play.sh "$HOME/show23-install" "$HOME/bin/shadPS4" --fullscreen

Use Python 3.10 or later on Linux and supply a Linux shadPS4 build from the
show23-pc-emulator repository's release branch. The portable ZIP includes a
Windows emulator only. Proton and Wine are untested. Image conversion uses
Pillow when available in the Linux Python environment.

The import-cards line is optional. Omit --fullscreen for windowed play.
Add --no-update-art to use the base game's pictures and cached site pictures
without using textures from the update. Play starts the server and stops it
when the emulator exits or you press Ctrl+C.

Saves are under data/profiles/default/user in the install folder. The server
keeps card progress in data/profiles/default/server and writes logs under
data/logs. On Windows you can later select this same install in the launcher.

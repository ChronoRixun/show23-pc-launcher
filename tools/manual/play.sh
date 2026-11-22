#!/bin/sh
set -eu
if [ "$#" -lt 2 ]; then
    echo 'Usage: sh play.sh <install> <emulator> [--fullscreen] [--no-update-art] [--resolution 720|1080] [--name <player name>]' >&2
    exit 2
fi
bundle="$(CDPATH= cd -- "$(dirname -- "$0")/../show23-pc" && pwd)"
IFS= read -r version < "$bundle/manifests/runtime.txt"
server="$bundle/runtime/$version/server/olserver.py"
install=$1
emulator=$2
shift 2
exec python3 -B "$server" play --install "$install" --emulator "$emulator" "$@"

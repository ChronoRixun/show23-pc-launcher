#!/bin/sh
set -eu
if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
    echo 'Usage: sh setup.sh <install> <unpacked base> [unpacked update]' >&2
    exit 2
fi
bundle="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
IFS= read -r version < "$bundle/manifests/runtime.txt"
server="$bundle/runtime/$version/server/olserver.py"
if [ "$#" -eq 3 ]; then
    exec python3 -B "$server" setup --install "$1" --base "$2" --update "$3"
fi
exec python3 -B "$server" setup --install "$1" --base "$2"

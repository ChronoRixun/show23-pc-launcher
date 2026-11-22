#!/bin/sh
set -eu
if [ "$#" -ne 1 ]; then
    echo 'Usage: sh import-cards.sh <install>' >&2
    exit 2
fi
bundle="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
IFS= read -r version < "$bundle/manifests/runtime.txt"
exec python3 -B "$bundle/runtime/$version/server/olserver.py" import-cards --install "$1" --pictures

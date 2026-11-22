#!/bin/sh
set -eu
manual="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
version="$(sh "$manual/select-version.sh")"
exec sh "$manual/../show23-pc/versions/$version/manual/${0##*/}" "$@"

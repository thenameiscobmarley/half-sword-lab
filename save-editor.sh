#!/usr/bin/env bash
# Half Sword Lab, save editor edition: edits your save files only (game closed), never touches game files.
exec "$(dirname "$(readlink -f "$0")")/run.sh" --saves-only "$@"

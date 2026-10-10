#!/bin/sh
set -eu

editor_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
editor="$editor_dir/Editor.bin"

if [ ! -x "$editor" ]; then
	echo "Open Engine could not find its native Editor executable: $editor" >&2
	exit 1
fi

export GE_VK_USE_DESCRIPTOR_BUFFER="${GE_VK_USE_DESCRIPTOR_BUFFER:-0}"

exec "$editor" "$@"

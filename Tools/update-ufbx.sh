#!/usr/bin/env bash
# Refresh the vendored ufbx copy under Engine/Source/ThirdParty/ufbx/.
#
# Usage:   Tools/update-ufbx.sh <tag>
# Example: Tools/update-ufbx.sh v0.22.0
#
# Fetches ufbx.h, ufbx.c, and LICENSE from the requested tag at
# https://github.com/ufbx/ufbx and rewrites the UFBX_VERSION.txt file with the
# resolved commit SHA. Run from anywhere; paths resolve relative to
# the repo root.

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 <tag>"   >&2
    echo "example: $0 v0.22.0" >&2
    exit 2
fi

tag="$1"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dest="$repo_root/Engine/Source/ThirdParty/ufbx"

raw_url="https://raw.githubusercontent.com/ufbx/ufbx/$tag"
api_url="https://api.github.com/repos/ufbx/ufbx/git/refs/tags/$tag"

# Resolve the tag → commit SHA so the UFBX_VERSION.txt file pins exactly what we fetched.
sha="$(curl -fsSL "$api_url" | sed -n 's/.*"sha": "\([^"]*\)".*/\1/p' | head -n1)"
if [[ -z "$sha" ]]; then
    echo "Could not resolve tag $tag to a commit SHA via $api_url" >&2
    exit 1
fi

mkdir -p "$dest"
for f in ufbx.h ufbx.c LICENSE; do
    echo "Fetching $f @ $tag"
    curl -fsSL "$raw_url/$f" -o "$dest/$f"
done

cat > "$dest/UFBX_VERSION.txt" <<EOF
ufbx
====

Vendored copy of the ufbx FBX importer.

Upstream:  https://github.com/ufbx/ufbx
License:   MIT or Public Domain (see LICENSE)
Version:   $tag
Commit:    $sha
Files:     ufbx.h ufbx.c LICENSE

To bump to a newer release, run Tools/update-ufbx.sh <tag>
(for example: Tools/update-ufbx.sh v0.22.0)
EOF

echo "Vendored ufbx $tag ($sha) into $dest"

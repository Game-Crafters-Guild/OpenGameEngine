#!/usr/bin/env bash
# Commits the demo site in <site dir> as one snapshot commit in the git repository there
# (created when missing), byte for byte, because the engine's glue files carry CRLF bytes the
# browser runs as they are: the site's .gitattributes turns off every attribute that rewrites
# bytes on the way in (line-ending conversion, clean filters such as a global Git LFS, ident,
# working-tree-encoding), whatever the machine's git configuration says. publish_web_site.sh
# pushes the result.
#
#   bash Tools/Web/commit_site_snapshot.sh <site dir> "<git user.name>" <git user.email>
set -euo pipefail

site=${1:?site directory}
name=${2:?git user.name}
email=${3:?git user.email}

cd "$site"
[ -d .git ] || git init -q -b main
git config user.name "$name"
git config user.email "$email"
git config core.autocrlf false
printf '* -text -filter -ident -working-tree-encoding\n' > .gitattributes
# GitHub Pages runs Jekyll unless told not to; the site is static files as built.
touch .nojekyll
git add -A
git add --renormalize .
git commit -q -m "Site snapshot" || echo "publish: nothing changed since the last snapshot"

# Every file on disk is in the commit with its bytes as they are; publish_web_site.sh pushes
# nothing when one is not (an attribute the site's .gitattributes does not outrank, such as the
# repository's own info/attributes, or an ignore rule).
differ=()
while IFS= read -r -d '' file; do
    path=${file#./}
    committed=$(git rev-parse -q --verify "HEAD:$path" 2>/dev/null || true)
    [ "$committed" = "$(git hash-object --no-filters -- "$path")" ] || differ+=("$path")
done < <(find . -path ./.git -prune -o -type f -print0)
if [ ${#differ[@]} -ne 0 ]; then
    echo "publish: the snapshot commit does not hold these files as they are on disk; refusing to publish:" >&2
    printf '  %s\n' "${differ[@]}" >&2
    exit 1
fi

#!/usr/bin/env bash
# Publishes the package build_web_package.py --out wrote to npm, under the dist-tag its version
# calls for: a prerelease (2026.10.0-alpha.1, 2026.10.0-beta.2) under its channel (alpha, beta), a
# release under latest. A prerelease never publishes as latest.
#
#   bash Tools/Web/publish_web_package.sh <package dir>              a dry run (the default)
#   bash Tools/Web/publish_web_package.sh <package dir> --publish    the real publish
#
# It refuses a package that build_web_package.py --check does not pass, or that carries the
# engine pack as parts (a --site package: npm would ship neither the pack nor its parts), and it
# reads the file list back from npm's own dry run (npm pack --dry-run) before a real publish;
# the dist-tag comes from the version and is checked against package.json's publishConfig. npm
# must be logged in as an owner of the package's scope. No other argument reaches npm.
set -euo pipefail

package=$(cd "${1:?package directory}" && pwd)
mode=${2:-}
[ $# -le 2 ] && { [ -z "$mode" ] || [ "$mode" = --publish ]; } || {
    echo "publish: usage: publish_web_package.sh <package dir> [--publish]" >&2
    exit 1
}
here=$(cd "$(dirname "$0")" && pwd)

if [ ! -f "$package/opengine-core.gepak" ] || [ -f "$package/opengine-core.gepak.parts.json" ]; then
    echo "publish: $package does not hold the whole engine pack (opengine-core.gepak); build the package with --out, not --site" >&2
    exit 1
fi
python "$here/build_web_package.py" --check --out "$package" > /dev/null

version=$(node -p "require(process.argv[1]).version" "$package/package.json")
channel=$(node -p "(require(process.argv[1]).publishConfig || {}).tag || ''" "$package/package.json")
if [[ "$version" =~ ^[0-9]{4}\.[0-9]{1,2}\.[0-9]+-(alpha|beta)\.[0-9]+$ ]]; then
    tag=${BASH_REMATCH[1]}
elif [[ "$version" =~ ^[0-9]{4}\.[0-9]{1,2}\.[0-9]+$ ]]; then
    tag=latest
else
    echo "publish: version $version is neither a release nor an alpha or beta prerelease; refusing" >&2
    exit 1
fi
if [ "$tag" != latest ] && [ "$channel" != "$tag" ]; then
    echo "publish: $version is a prerelease but package.json's publishConfig.tag is '$channel'; refusing to publish it as latest" >&2
    exit 1
fi

# npm's own account of what it would publish: the files it would ship and the tag it would use.
report=$(npm pack "$package" --dry-run --json)
node -e "
const [entry] = JSON.parse(process.argv[1]);
const files = new Set(entry.files.map((f) => f.path));
for (const name of ['opengine-core.gepak', 'opengine.mjs', 'opengine-core.st.wasm', 'opengine-core.mt.wasm'])
    if (!files.has(name)) { console.error('publish: npm would not ship ' + name + '; refusing'); process.exit(1); }
console.log('publish: npm would ship ' + files.size + ' files, ' + entry.size + ' bytes packed');" "$report"
echo "publish: $version under the '$tag' dist-tag (dry run)"

if [ "$mode" = --publish ]; then
    npm publish "$package" --access public --tag "$tag"
    echo "publish: published $version under '$tag' (npm install @openengine/web@$tag)"
fi

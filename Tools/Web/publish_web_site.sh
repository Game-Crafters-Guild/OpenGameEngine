#!/usr/bin/env bash
# Publishes the demo site that build_web_package.py --site wrote to a public GitHub repository,
# as one snapshot commit, and serves it with GitHub Pages from the branch root.
#
#   GH_CONFIG_DIR=<the public account's gh config> \
#   bash Tools/Web/publish_web_site.sh <site dir> <owner/repo> "<git user.name>" <git user.email>
#
# The repository must exist and be empty, or hold earlier snapshots of this script: each run
# commits the site as it is on disk as a new snapshot commit, byte for byte
# (commit_site_snapshot.sh), and pushes over SSH. It refuses unless
# the ssh agent's key and the gh login (GH_CONFIG_DIR) are both the organization's account.
# Nothing here creates the repository.
# Every file must be under GitHub's 100 MB limit (the site's engine pack is split for this).
set -euo pipefail

site=${1:?site directory}
repo=${2:?owner/repo}
name=${3:?git user.name}
email=${4:?git user.email}
branch=main

# The public repository is pushed and served by the organization's account only: its gh login
# (GH_CONFIG_DIR) and the SSH key the agent offers for github.com must both be that account's.
expectedAccount=GameCraftersGuild
[ -n "${GH_CONFIG_DIR:-}" ] || { echo "publish: set GH_CONFIG_DIR to the $expectedAccount gh login" >&2; exit 1; }
sshGreeting=$(ssh -T -o BatchMode=yes git@github.com 2>&1 || true)
[[ "$sshGreeting" == *"Hi $expectedAccount!"* ]] || {
    echo "publish: ssh to github.com answers \"$sshGreeting\", not as $expectedAccount; refusing to push" >&2
    exit 1
}
ghAccount=$(gh api user --jq .login)
[ "$ghAccount" = "$expectedAccount" ] || { echo "publish: gh is logged in as $ghAccount, not $expectedAccount" >&2; exit 1; }

[ -f "$site/index.html" ] || { echo "publish: $site has no index.html; build it with build_web_package.py --site" >&2; exit 1; }
large=$(find "$site" -path "$site/.git" -prune -o -type f -size +99M -print)
[ -z "$large" ] || { echo "publish: files over GitHub's limit:" >&2; echo "$large" >&2; exit 1; }

bash "$(dirname "$0")/commit_site_snapshot.sh" "$site" "$name" "$email"
cd "$site"
git remote remove origin 2>/dev/null || true
git remote add origin "git@github.com:$repo.git"
git push -u origin "$branch"

# Pages from the branch root; a second run updates the source instead of creating it.
if ! gh api "repos/$repo/pages" >/dev/null 2>&1; then
    gh api -X POST "repos/$repo/pages" -f "source[branch]=$branch" -f "source[path]=/" >/dev/null
else
    gh api -X PUT "repos/$repo/pages" -f "source[branch]=$branch" -f "source[path]=/" >/dev/null
fi
gh api "repos/$repo/pages" --jq '"publish: Pages " + .status + " at " + .html_url'

#!/usr/bin/env bash
set -euo pipefail

# Fetch prebuilt vcpkg binary cache from the repo's GitHub Release into the
# per-user cache the presets consult first (they set VCPKG_BINARY_SOURCES to
# clear;default,readwrite plus any sources the machine environment adds):
# $VCPKG_DEFAULT_BINARY_CACHE if set, else $XDG_CACHE_HOME/vcpkg/archives,
# else ~/.cache/vcpkg/archives. Shared by every clone and worktree.
# Requires GitHub CLI (gh) and authentication (private repo):
#   gh auth login

tag="vcpkg-cache"
triplet=""
repo=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --triplet)
      triplet="$2"; shift 2 ;;
    --tag)
      tag="$2"; shift 2 ;;
    --repo)
      repo="$2"; shift 2 ;;
    -h|--help)
      echo "Usage: Tools/Scripts/fetch-vcpkg-cache.sh [--triplet <triplet>] [--tag <tag>] [--repo <owner/repo>]"
      exit 0
      ;;
    *)
      echo "Unknown arg: $1" >&2
      exit 2
      ;;
  esac
done

if [[ -z "$triplet" ]]; then
  os="$(uname -s)"
  arch="$(uname -m)"

  case "$arch" in
    x86_64) arch_part="x64" ;;
    aarch64|arm64) arch_part="arm64" ;;
    *) echo "Unsupported arch: $arch" >&2; exit 1 ;;
  esac

  case "$os" in
    Linux) triplet="${arch_part}-linux" ;;
    Darwin) triplet="${arch_part}-osx" ;;
    *) echo "Unsupported OS: $os" >&2; exit 1 ;;
  esac
fi

if [[ -z "$repo" ]]; then
  origin_url="$(git remote get-url origin 2>/dev/null || true)"
  # POSIX ERE only: lazy quantifiers ([^/]+?) are PCRE and fail to compile in
  # [[ =~ ]] on macOS/msys regcomp. Match greedily, strip .git afterwards.
  if [[ "$origin_url" =~ ^https://github\.com/([^/]+)/([^/]+)$ ]]; then
    repo="${BASH_REMATCH[1]}/${BASH_REMATCH[2]%.git}"
  elif [[ "$origin_url" =~ ^git@github\.com:([^/]+)/([^/]+)$ ]]; then
    repo="${BASH_REMATCH[1]}/${BASH_REMATCH[2]%.git}"
  else
    echo "Unable to determine GitHub repo from git origin; pass --repo owner/repo" >&2
    exit 1
  fi
fi

command -v gh >/dev/null 2>&1 || { echo "gh not found; install GitHub CLI" >&2; exit 1; }
gh auth status >/dev/null 2>&1 || { echo "Not authenticated; run: gh auth login" >&2; exit 1; }
command -v unzip >/dev/null 2>&1 || { echo "unzip not found" >&2; exit 1; }

zip_name="vcpkg-archives-${triplet}.zip"
tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

gh release download "$tag" --repo "$repo" --pattern "$zip_name" --dir "$tmp_dir" >/dev/null

dest_dir="${VCPKG_DEFAULT_BINARY_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/vcpkg/archives}"
mkdir -p "$dest_dir"
unzip -o -q "$tmp_dir/$zip_name" -d "$dest_dir"

echo "Installed vcpkg cache for '$triplet' into $dest_dir"

#!/bin/sh
# One-time: add your 3D-edition data from your game folder, make the first commit and push to GitHub.
# usage: ./push_to_github.sh /path/to/MW2-game [git-remote-url]
set -e
GAME="${1:?usage: ./push_to_github.sh /path/to/MW2-game [remote-url]}"
REMOTE="${2:-https://github.com/BatMahn/MW2-Port.git}"
cd "$(dirname "$0")"
[ -f "$GAME/3d/models.prj" ] || { echo "no $GAME/3d/models.prj"; exit 1; }
cp -R "$GAME/3d/." gamedata/3d/
rm -f gamedata/3d/.keep
[ -d .git ] || git init -q -b main
git add -A
git commit -m "MechWarrior 2 64-bit port: source, docs, tests and the 3D editions' data"
git remote remove origin 2>/dev/null || true
git remote add origin "$REMOTE"
git push -u origin main

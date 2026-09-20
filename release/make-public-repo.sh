#!/bin/sh
# release/make-public-repo.sh -- write the PUBLIC source snapshot (RELEASE.md, step 2).
#
#   sh release/make-public-repo.sh              # -> ~/Developer/MetalQuake
#   PUBLIC_REPO=/somewhere sh release/make-public-repo.sh
#
# This repository is only READ: the snapshot is `git archive HEAD` (so it can never
# contain an untracked or ignored file -- no game data, no m5/ assets, no fteqw tree),
# minus the private working log, plus the public README and GitHub furniture from
# release/public-repo/. It is written to a SEPARATE folder with its own git history.
# Nothing is pushed and no remote is added: that is Seb's step.
#
# Re-running refreshes the snapshot in place and leaves the target's .git alone, so
# each run can become one new public commit.
set -e
cd "$(dirname "$0")/.."
REPO=$(pwd)
DEST="${PUBLIC_REPO:-$HOME/Developer/MetalQuake}"
MARK=".metalquake-snapshot"

if [ -e "$DEST" ] && [ ! -f "$DEST/$MARK" ]; then
	echo "REFUSING: $DEST exists and is not a MetalQuake snapshot (no $MARK). Nothing touched."
	exit 1
fi
[ -z "$(git status --porcelain --untracked-files=no)" ] \
	|| echo "-- note: uncommitted changes here are NOT in the snapshot (it is HEAD)"

TMP=$(mktemp -d /tmp/mqrepo.XXXXXX)
trap 'rm -rf "$TMP"' EXIT
git archive HEAD | tar -x -C "$TMP"

# --- the private working log stays private ---------------------------------------
for f in CLAUDE.md BALLLIGHTNING.md BEAUTY.md BEAUTYBENCH.md BLUENOISE.md FOGLIGHT.md \
         GIARC.md LIQUIDFOG.md PERFPLAN.md ROADMAP.md RELEASE.md SEPTEMBER.md \
         SEPTEMBER2.md SMAA.md WARCHEST.md WATERSURFACE.md impulse_mapping.txt \
         metal/async-plan.md release/public-repo \
         WATERSURFACE-redesign.patch \
         m5/showreel.cfg m5/showreel_ab.cfg m5/showreel_check.cfg; do
	rm -rf "$TMP/$f"
done

# THE ICON. The private app icon carries id's Quake symbol and Apple's logo -- two
# trademarks that are not ours to publish. The snapshot gets MetalQuake's own icon
# (release/MetalQuake.icns) under the same filename so the Xcode project still builds.
cp "$TMP/release/MetalQuake.icns" "$TMP/QuakeM5.icns"   # our own icon, under the name the Xcode project expects

# --- the public face ------------------------------------------------------------------
mv "$TMP/README.md" "$TMP/README-darkplaces.md"
cp "$REPO/release/public-repo/README.md" "$TMP/README.md"
mkdir -p "$TMP/.github"
cp -R "$REPO/release/public-repo/.github/." "$TMP/.github/"
# the Xcode scheme's working directory is an absolute path on the build machine
sed -i '' 's|customWorkingDirectory = "/Users/[^"]*"|customWorkingDirectory = "$(PROJECT_DIR)"|; s|customWorkingDirectory="/Users/[^"]*"|customWorkingDirectory="$(PROJECT_DIR)"|' \
	"$TMP/QuakeM5.xcodeproj/xcshareddata/xcschemes/QuakeM5.xcscheme"
date "+snapshot of the private tree at $(git rev-parse --short HEAD), %Y-%m-%d" > "$TMP/$MARK"

# --- the sweep: refuse to write a snapshot that carries any of these ----------------------
echo "== sweep"
HITS=$(grep -rIlE "/Users/[a-z]+/|@outlook\.|quantity surveyor|claude-50[0-9]" "$TMP" | grep -v "release/make-public-repo.sh$" || true)   # (this script names the patterns)
BIN=$(find "$TMP" -type f \( -iname '*.pak' -o -iname '*.pk3' -o -iname '*.mdl' -o -iname '*.bsp' \
	-o -iname '*.dem' -o -iname 'config.cfg' \) )
BIG=$(find "$TMP" -type f -size +2048k)
if [ -n "$HITS$BIN" ]; then
	echo "FAIL: the snapshot carries something private or not ours:"; echo "$HITS"; echo "$BIN"
	exit 1
fi
[ -n "$BIG" ] && { echo "-- files over 2 MB (check they belong):"; echo "$BIG"; }
echo "   clean: $(find "$TMP" -type f | wc -l | tr -d ' ') files, $(du -sh "$TMP" | cut -f1)"

# --- write it, leaving the target's own .git alone -----------------------------------------
mkdir -p "$DEST"
rsync -a --delete --exclude '.git' "$TMP/" "$DEST/"
if [ ! -d "$DEST/.git" ]; then
	git -C "$DEST" init -q -b main
	echo "== initialised a fresh git repository (branch main, no remote)"
fi
git -C "$DEST" add -A
echo "== $DEST is ready. Staged, NOT committed, NOT pushed:"
git -C "$DEST" status --short | wc -l | awk '{print "   " $1 " changed paths"}'

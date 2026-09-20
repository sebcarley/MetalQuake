#!/bin/sh
# release/make-release.sh -- build a SELF-CONTAINED MetalQuake.app plus its packs/
# folder, and zip the pair for another Mac.
#
#   sh release/make-release.sh                       # everything, into release/out
#   RELEASE_PACKS=0 sh release/make-release.sh       # the app alone (fast: ~7 MB)
#   RELEASE_GAMEDIRS="id1 m5 ad" sh release/make-release.sh    # pick the packs
#   RELEASE_ZIP=0   sh release/make-release.sh       # assemble but do not zip
#   RELEASE_OUT=/somewhere sh release/make-release.sh
#
# TWO ZIPS, not one, and deliberately: the app is ~7 MB and changes every time
# the engine does, the game data is ~2 GB and essentially never changes. Ship
# the packs zip once to a machine and thereafter only the app zip needs to
# cross the wire.
#
# THE SHIPPED LAYOUT, and the whole reason this script exists:
#
#     <any folder>/MetalQuake.app
#     <any folder>/packs/id1/PAK0.PAK ...
#     <any folder>/packs/m5/...            (the M5 mod: progs.dat + look files)
#     <any folder>/packs/hipnotic/...      (and rogue, dopa, mg1, ad)
#
# The app finds that sibling `packs` folder itself (fs.c's macOS basedir walk
# prefers <dir containing the .app>/packs over a bare gamedir beside the .app),
# so the pair can be dropped in /Applications, in a "Games" folder, on a USB
# stick -- anywhere, together. Nothing is hardcoded and no launcher script sets
# any environment variable.
#
# WHY THE DYLIBS ARE COPIED IN. `make sdl-release` links exactly ONE non-system
# library, Homebrew's sdl2-compat, which in turn loads libSDL3 at runtime; the
# Ogg Vorbis decoder is dlopen'd from /opt/homebrew/lib by absolute path. On a
# Mac without Homebrew -- i.e. any machine but this one -- that is a launch
# failure and silent music respectively. So all four are copied into
# Contents/MacOS beside the binary and the install names rewritten to
# @executable_path. sdl2-compat already searches @executable_path/libSDL3.dylib
# first, and snd_ogg.c carries @executable_path entries for the vorbis pair, so
# the bundle resolves everything from inside itself. `otool -L` on the packaged
# binary is the proof, and this script runs it as an assertion below.
set -e
cd "$(dirname "$0")/.."
REPO=$(pwd)
# RELEASE_PUBLIC=1 -- THE BUILD THAT CAN BE POSTED. Everything above describes the
# private pair (for a friend who owns the game). The public build differs in the one
# way that matters: it contains NOTHING that is not ours. No id/Bethesda data, no
# mission packs, no third-party texture or model packs, no demos, none of Seb's own
# settings verbatim. It assembles into its OWN folder (release/public) so the private
# build in release/out is never touched, packs/ is built from an ALLOW-LIST (a file
# ships because it is named, never because it was not excluded), and the script ends
# by asserting the result: no .pak/.pk3/.mdl/.bsp/.dem/.spr/.wav/.lmp anywhere, and
# nothing in packs/ over 1 MB. One zip, one top-level folder.
PUBLIC="${RELEASE_PUBLIC:-0}"
if [ "$PUBLIC" = "1" ]; then
	OUT="${RELEASE_OUT:-$REPO/release/public}/MetalQuake"
else
	OUT="${RELEASE_OUT:-$REPO/release/out}"
fi
APP="$OUT/MetalQuake.app"
STAMP=$(date +%Y%m%d)

echo "== building the engine (make sdl-release)"
make sdl-release -j8 >/dev/null
[ -x ./darkplaces-sdl ] || { echo "no darkplaces-sdl after build"; exit 1; }
# the QuakeC lane is content, not engine: build it if the compiler is present,
# otherwise ship whatever m5/progs.dat is already there (and say which)
if [ -x tools/fteqcc/fteqcc ]; then
	echo "== building m5/progs.dat (qc/build.sh)"
	sh qc/build.sh >/dev/null
else
	echo "-- fteqcc absent; shipping the existing m5/progs.dat"
fi

echo "== assembling $APP"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp ./darkplaces-sdl "$APP/Contents/MacOS/MetalQuake"

# --- the dylibs, and their install names -----------------------------------
# Resolve through symlinks (Homebrew's opt/ paths are links into Cellar) and
# copy the REAL file, then give each a plain @executable_path id so nothing
# points back at /opt/homebrew.
copylib() {   # $1 source path  $2 basename to install as
	[ -f "$1" ] || { echo "MISSING: $1"; exit 1; }
	cp -L "$1" "$APP/Contents/MacOS/$2"
	chmod u+w "$APP/Contents/MacOS/$2"
	install_name_tool -id "@executable_path/$2" "$APP/Contents/MacOS/$2" 2>/dev/null || true
}
SDL2=$(otool -L ./darkplaces-sdl | awk '/libSDL2/{print $1; exit}')
[ -n "$SDL2" ] || { echo "could not find the SDL2 the binary links"; exit 1; }
copylib "$SDL2" libSDL2-2.0.0.dylib
# sdl2-compat is a shim: it dlopens SDL3, searching @executable_path first
SDL3=$(ls /opt/homebrew/opt/sdl3/lib/libSDL3.0.dylib /opt/homebrew/lib/libSDL3.0.dylib 2>/dev/null | head -1)
[ -n "$SDL3" ] && copylib "$SDL3" libSDL3.dylib
for v in libvorbis libvorbisfile; do
	L=$(ls /opt/homebrew/lib/$v.dylib /usr/local/lib/$v.dylib 2>/dev/null | head -1)
	[ -n "$L" ] && copylib "$L" "$v.dylib"
done
# libpng, so screenshots work on a Mac with no Homebrew: the engine's default is JPEG,
# there is no libjpeg to bundle here, and without either the screenshot key writes
# nothing (the public config selects PNG). img_png's name list already tries
# @executable_path; libpng's own dependency is the system zlib.
PNG=$(ls /opt/homebrew/lib/libpng16.16.dylib /usr/local/lib/libpng16.16.dylib 2>/dev/null | head -1)
[ -n "$PNG" ] && copylib "$PNG" libpng16.16.dylib
# and the executable's own reference to SDL2
install_name_tool -change "$SDL2" "@executable_path/libSDL2-2.0.0.dylib" "$APP/Contents/MacOS/MetalQuake"

# The transitive closure: vorbis needs libogg, vorbisfile needs vorbis. Done as
# a FIXPOINT rather than one pass over a glob, because a dependency copied
# during a pass is not in that pass's already-expanded file list -- which is
# exactly how the first cut of this shipped a libvorbis.0.dylib still pointing
# at /opt/homebrew for its libogg. The dependency check at the end is what
# caught it, and this loop is what makes the check pass honestly.
pass=0
while [ "$pass" -lt 8 ]; do
	changed=0
	for f in "$APP/Contents/MacOS/MetalQuake" "$APP/Contents/MacOS"/*.dylib; do
		[ -f "$f" ] || continue
		for dep in $(otool -L "$f" | tail -n +2 | awk '/\/opt\/homebrew|\/usr\/local/{print $1}'); do
			base=$(basename "$dep")
			[ -f "$APP/Contents/MacOS/$base" ] || copylib "$dep" "$base"
			install_name_tool -change "$dep" "@executable_path/$base" "$f" 2>/dev/null || true
			changed=1
		done
	done
	[ "$changed" -eq 0 ] && break
	pass=$((pass + 1))
done

# --- bundle metadata --------------------------------------------------------
# MetalQuake's own icon (a silver M on slate, Seb's pick 2026-09-20; drawn by
# release/make-icon.swift -- no id or Apple marks in it, which the private Xcode app's
# QuakeM5.icns has and is why that one is never shipped)
if [ -f release/MetalQuake.icns ]; then
	cp release/MetalQuake.icns "$APP/Contents/Resources/MetalQuake.icns"
elif [ -f Darkplaces.app/Contents/Resources/Darkplaces.icns ]; then
	cp Darkplaces.app/Contents/Resources/Darkplaces.icns "$APP/Contents/Resources/MetalQuake.icns"
fi
VER=$(strings ./darkplaces-sdl | grep -m1 "^pre-m5-lightning" || echo "m5")
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple Computer//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleName</key>              <string>MetalQuake</string>
	<key>CFBundleDisplayName</key>       <string>MetalQuake</string>
	<key>CFBundleIdentifier</key>        <string>com.sebcarley.metalquake</string>
	<key>CFBundleExecutable</key>        <string>MetalQuake</string>
	<key>CFBundleIconFile</key>          <string>MetalQuake</string>
	<key>CFBundlePackageType</key>       <string>APPL</string>
	<key>CFBundleShortVersionString</key><string>$STAMP</string>
	<key>CFBundleVersion</key>           <string>$STAMP</string>
	<key>LSApplicationCategoryType</key> <string>public.app-category.action-games</string>
	<key>LSMinimumSystemVersion</key>    <string>27.0</string>
	<key>NSHighResolutionCapable</key>   <true/>
	<key>NSSupportsAutomaticGraphicsSwitching</key> <true/>
</dict>
</plist>
PLIST
printf 'APPL????' > "$APP/Contents/PkgInfo"

# --- the packs folder -------------------------------------------------------
# Copied with -L so the mission packs' symlink farms (which point into the
# Steam install) become REAL files -- a zip of symlinks would arrive on the
# other machine as a fistful of broken links.
if [ "$PUBLIC" = "1" ]; then
	echo "== assembling the PUBLIC $OUT/packs (allow-list)"
	rm -rf "$OUT/packs"; mkdir -p "$OUT/packs/id1" "$OUT/packs/m5"
	# ours, by name: the compiled GPL QuakeC, the effects data, the pack look files,
	# and the recipe cfgs the guide refers to (not the showreels -- they play demos
	# that are not shipped)
	cp "$REPO/m5/progs.dat" "$REPO/m5/effectinfo.txt" "$OUT/packs/m5/"
	for f in "$REPO"/m5/*.cfg; do
		case "$(basename "$f")" in
			config.cfg|showreel*.cfg|ab[0-9]*.cfg|ab_stop.cfg|seb_*.cfg|fixup*.cfg) ;;
			*) cp "$f" "$OUT/packs/m5/" ;;
		esac
	done
	python3 release/make-public-config.py \
		"${RELEASE_CONFIG:-$HOME/Library/Application Support/darkplaces/m5/config.cfg}" \
		"$OUT/packs/m5/config.cfg"
	python3 test/perf/check-tiers.py --config "$OUT/packs/m5/config.cfg" \
		|| { echo "   !! the public config matches no tier"; exit 1; }
	cat > "$OUT/packs/id1/PUT-PAK0-AND-PAK1-HERE.txt" <<'PAKS'
Quake's own game data goes in this folder, and it is not included: it belongs to
id Software, and you need to own the game.

Copy these two files here from the id1 folder of your Quake (Steam or GOG):

    pak0.pak
    pak1.pak

(Upper or lower case both work. The 2021 re-release keeps them in
 <Quake>/id1/ as well; the older Steam version in <Quake>/Id1/.)
PAKS
elif [ "${RELEASE_PACKS:-1}" = "1" ]; then
	echo "== assembling $OUT/packs"
	rm -rf "$OUT/packs"; mkdir -p "$OUT/packs"
	for g in ${RELEASE_GAMEDIRS:-id1 m5 hipnotic rogue dopa mg1 ad}; do
		[ -d "$REPO/$g" ] || { echo "   -- $g absent, skipped"; continue; }
		echo "   .. $g"
		mkdir -p "$OUT/packs/$g"
		# every gamedir wholesale EXCEPT the engine's own writable droppings and
		# build leavings. -h DEREFERENCES: the mission packs are symlink farms
		# into the Steam install, and a zip of symlinks arrives on the other
		# machine as a fistful of broken links.
		( cd "$REPO/$g" && tar -ch --exclude=config.cfg --exclude=benchmark.log \
			--exclude='*.dem' --exclude=screenshots --exclude='*.lno' . ) \
			| ( cd "$OUT/packs/$g" && tar -x )
	done
	# id1 is the one that must be there for anything to run at all
	[ -f "$OUT/packs/id1/PAK0.PAK" ] || [ -f "$OUT/packs/id1/pak0.pak" ] \
		|| { echo "WARNING: packs/id1 has no PAK0 -- the app will not start"; }

	# SEB'S OWN SETTINGS AS THE SHIPPED DEFAULTS.
	#
	# The tar above strips every gamedir's config.cfg (they are the engine's own
	# writable droppings and a stale one is worse than none); this puts ONE back,
	# deliberately, taken from the live userdir rather than the repo -- the repo
	# has no config.cfg at all, because with m5_sharedconfig the engine writes to
	# the userdir.
	#
	# WHY packs/m5 IS THE RIGHT PLACE, and why this cannot fight the player's own
	# settings later: the userdir copy of a gamedir is added to the search path
	# AFTER the basedir copy, and FS_AddGameDirectory PREPENDS, so the userdir is
	# searched FIRST. On a machine with no settings yet the userdir has no
	# config.cfg, the search falls through to this one, and these become the
	# defaults. The moment the player quits, the engine writes THEIR config to
	# the userdir (never here -- fs_gamedir is the userdir path), and from then
	# on the userdir copy wins and this one is inert. So it is a starting point,
	# not an override. Measured both ways round, see the session record.
	CFG="${RELEASE_CONFIG:-$HOME/Library/Application Support/darkplaces/m5/config.cfg}"
	if [ "$CFG" != "none" ] && [ -f "$CFG" ] && [ -d "$OUT/packs/m5" ]; then
		# THE SHIPPED CONFIG MUST MATCH AN M5 QUALITY TIER, and this asserts it
		# rather than hoping. It is a live copy of whatever the build machine's
		# config happens to be at that moment, so a console session that left one
		# lever wandered ships a config that makes the M5 Quality row read
		# "Custom" on a brand new machine -- a poor first impression, and utterly
		# silent. (Found 2026-08-29: a snapshot in out/ was missing
		# r_metalfx_reactive entirely and would have done exactly that.) The
		# checker parses menu.c's own m5_quality_levers[] and resolves absent
		# archived cvars to their compiled defaults, which is what
		# M5_DetectQuality effectively sees.
		if python3 test/perf/check-tiers.py --config "$CFG"; then
			:
		elif [ "${RELEASE_ALLOW_CUSTOM:-0}" = "1" ]; then
			echo "   !! shipping a Custom config anyway (RELEASE_ALLOW_CUSTOM=1)"
		else
			echo "   !! REFUSING: the config above matches no tier, so a fresh"
			echo "      install would read 'Custom' on Options -> M5 Quality."
			echo "      Click a tier in game and quit, point RELEASE_CONFIG at a"
			echo "      different file, or set RELEASE_ALLOW_CUSTOM=1 to ship it."
			exit 1
		fi
		cp "$CFG" "$OUT/packs/m5/config.cfg"
		echo "   .. m5/config.cfg  (shipped as the default settings)"
	elif [ "$CFG" != "none" ]; then
		echo "   -- no config.cfg to ship from $CFG"
	fi
fi

# --- sign, so the other Mac's Gatekeeper has something to check -------------
# Ad-hoc (-) rather than a Developer ID: this is a personal build. The receiving
# Mac will still quarantine a downloaded zip -- the README below says how.
codesign --force --deep --sign - "$APP" >/dev/null 2>&1 \
	&& echo "== ad-hoc signed" || echo "-- codesign unavailable, shipping unsigned"

# --- prove it is self-contained --------------------------------------------
echo "== dependency check (must show NO /opt/homebrew or /usr/local)"
LEAK=$(otool -L "$APP/Contents/MacOS/MetalQuake" "$APP/Contents/MacOS"/*.dylib 2>/dev/null \
	| grep -E "/opt/homebrew|/usr/local" || true)
if [ -n "$LEAK" ]; then
	echo "$LEAK"
	echo "FAIL: the bundle still points at Homebrew -- it will not run on another Mac"
	exit 1
fi
echo "   clean: system frameworks + @executable_path only"

cat > "$OUT/README.txt" <<'TXT'
MetalQuake
=======

FIRST RUN ON A NEW MAC -- do this before anything else
------------------------------------------------------
macOS quarantines downloaded apps. That causes BOTH of the failures you might
see: the app refusing to open at all, or opening but claiming "the required
files were not found" even though the packs folder is right beside it (macOS
secretly runs a hidden copy of the app from a temp folder -- "App
Translocation" -- so it cannot see its neighbours).

One command in Terminal fixes both, once. Open Terminal, paste this INCLUDING
the trailing space, then drag the MetalQuake app onto the Terminal window and
press Enter:

    xattr -dr com.apple.quarantine 

The layout
----------
Keep these two together, in any folder you like (Applications, a Games
folder, an external disk):

    MetalQuake.app
    packs/

`packs` is where the game data lives -- packs/id1 is Quake itself, and the
other folders are the mission packs and Arcane Dimensions. The app looks for
that folder next to itself, so moving the pair anywhere is fine; moving the
app WITHOUT the packs folder is not.

Everything else is inside the app -- SDL and the Ogg Vorbis decoder are
bundled, so no Homebrew or other install is needed.

Your settings
-------------
packs/m5/config.cfg carries Seb's settings, and a machine that has never run
the game picks them up as its DEFAULTS. As soon as you quit, the game saves
your own settings to your home folder instead:

    ~/Library/Application Support/darkplaces/m5/config.cfg

From then on that file wins and the one in packs is ignored -- so the shipped
config is a starting point, never an override. To go back to it, delete your
own copy and start the game again.

Moving just the config between machines (no zips involved)
----------------------------------------------------------
QUIT THE GAME FIRST on both machines -- it rewrites this file when it exits,
so a copy pasted while it is running is overwritten on quit.

  1. In Finder press Shift-Command-G and paste:
         ~/Library/Application Support/darkplaces/m5
  2. Copy config.cfg
  3. On the other Mac, open the same folder the same way (if the game has
     never run there, run it once and quit so the folder exists), and paste,
     replacing the file that is there.

Screen settings travel with it -- the config asks for 1920x1080 exclusive
fullscreen. On a Mac whose display differs, set the resolution again in
Options -> Video.

Adding or removing packs
------------------------
Drop a gamedir into `packs` and it appears on Single Player -> Mission Packs
if the engine knows it. Delete one and it is simply listed as absent.
TXT

# --- the PUBLIC build: its own README, its assertions, its one zip -----------
if [ "$PUBLIC" = "1" ]; then
cat > "$OUT/README.txt" <<'TXT'
MetalQuake
==========
Quake on Apple Silicon: a native Metal renderer with ray-traced lighting and
shadows, volumetric fog that the level's own lights shine through, HDR output
and MetalFX upscaling -- and a "Stock" setting that puts 1996 back in one click.
A fork of the DarkPlaces engine. Free, GPL-2, source on GitHub.

Not affiliated with id Software or Bethesda. Quake is their trademark, and
THE GAME DATA IS NOT INCLUDED: you need to own Quake.

1. Put Quake's data in
----------------------
Copy  pak0.pak  and  pak1.pak  from the id1 folder of your Quake (Steam or
GOG) into:

    packs/id1/

2. First run -- one Terminal command, once
------------------------------------------
macOS quarantines downloaded apps, and this one is not notarised. That causes
BOTH failures you might see: the app refusing to open, or opening but saying
"the required files were not found" with the packs folder right beside it
(macOS runs a hidden copy from a temp folder -- "App Translocation").

Open Terminal, paste this INCLUDING the trailing space, drag the MetalQuake
app onto the Terminal window, press Enter:

    xattr -dr com.apple.quarantine 

3. Keep the pair together
-------------------------
    MetalQuake.app
    packs/

in any folder you like. The app looks for `packs` next to itself.

What Mac?
---------
Apple Silicon, running macOS 27 or later. An M3 or later has hardware ray tracing and is what the
settings were tuned on (an M5). On an M1 or M2 the ray tracing is emulated:
start on Options -> M5 Quality -> Fast, or Stock, and work upwards.
It starts on "Best". If the picture is too dark or too bright on your display,
Options -> Brightness and Gamma.

Optional extras (not included, their authors' own work)
-------------------------------------------------------
Drop any of these into packs/ and they are picked up:
  - the mission packs (hipnotic, rogue, and the re-release's dopa and mg1)
  - Arcane Dimensions (folder "ad")
  - the Quake Revitalization Project textures (.pk3 into packs/m5)
  - Authentic Model Improvements (its progs/ and maps/ into packs/m5 --
    NOT its progs.dat, which would replace this fork's game code)

Your settings are saved to
    ~/Library/Application Support/darkplaces/m5/config.cfg
the first time you quit. Delete that file to return to the shipped defaults.

The field guide and the manual: see the GitHub page.
TXT
	cp "$OUT/README.txt" "$OUT/packs/README.txt"
	cp "$REPO/COPYING" "$OUT/COPYING.txt" 2>/dev/null || true

	echo "== PUBLIC assertions"
	BAD=$(find "$OUT/packs" -type f \( -iname '*.pak' -o -iname '*.pk3' -o -iname '*.mdl' \
		-o -iname '*.bsp' -o -iname '*.dem' -o -iname '*.spr' -o -iname '*.wav' \
		-o -iname '*.lmp' -o -iname '*.ogg' -o -iname '*.tga' -o -iname '*.lit' \) )
	BIG=$(find "$OUT/packs" -type f -size +1024k)
	LINKS=$(find "$OUT/packs" -type l)
	# (progs.dat legitimately carries the NAME m5_horde_best; the config must not carry a value)
	PERSONAL=$(grep -rlE "/Users/|sebcarley" "$OUT/packs" || true)
	PERSONAL="$PERSONAL$(grep -lE '^"(m5_horde_best|sensitivity|vid_width|_cl_name)"' "$OUT/packs/m5/config.cfg" || true)"
	if [ -n "$BAD$BIG$LINKS$PERSONAL" ]; then
		echo "FAIL: the public packs folder contains something it must not:"
		echo "$BAD"; echo "$BIG"; echo "$LINKS"; echo "$PERSONAL"
		exit 1
	fi
	echo "   clean: $(find "$OUT/packs" -type f | wc -l | tr -d ' ') files, $(du -sh "$OUT/packs" | cut -f1), no game data, no third-party assets, nothing personal"

	if [ "${RELEASE_ZIP:-1}" = "1" ]; then
		PUBZIP="$(dirname "$OUT")/MetalQuake-$STAMP.zip"
		rm -f "$PUBZIP"
		# one top-level item (the MetalQuake folder), ditto so the signature survives
		( cd "$(dirname "$OUT")" && ditto -c -k --keepParent MetalQuake "$(basename "$PUBZIP")" )
		echo "== public zip: $(du -h "$PUBZIP" | cut -f1)  $PUBZIP"
		shasum -a 256 "$PUBZIP" | tee "$PUBZIP.sha256"
	fi
	echo "== done (PUBLIC). Nothing has been uploaded anywhere."
	exit 0
fi

# --- zip --------------------------------------------------------------------
# ONE TOP-LEVEL ITEM PER ZIP, and it is a hard rule: macOS Archive Utility
# extracts a single-item zip bare, but wraps a multi-item zip in a folder
# named after the zip -- so an app zip that also carried a README landed the
# .app one level deeper than the packs zip's own folder, and the app could
# not see packs beside it. That shipped once (2026-08-21) and cost a remote
# support round trip. The README therefore travels INSIDE packs/, and a loose
# copy is left in $OUT to send alongside the zips.
cp "$OUT/README.txt" "$OUT/packs/README.txt" 2>/dev/null || true

if [ "${RELEASE_ZIP:-1}" = "1" ]; then
	# ditto rather than zip for the .app: it preserves the bundle's symlinks and
	# the ad-hoc signature, which a plain zip of a signed bundle can disturb.
	# NO --sequesterRsrc: it adds a top-level __MACOSX entry, which is a second
	# top-level item -- the wrapper-folder trap above, again.
	APPZIP="$OUT/MetalQuake-app-$STAMP.zip"
	echo "== zipping the app -> $APPZIP"
	rm -f "$APPZIP"
	( cd "$OUT" && ditto -c -k --keepParent MetalQuake.app "$(basename "$APPZIP")" )
	echo "   $(du -h "$APPZIP" | cut -f1)  $APPZIP"

	if [ -d "$OUT/packs" ]; then
		PACKZIP="$OUT/MetalQuake-packs-$STAMP.zip"
		echo "== zipping the packs -> $PACKZIP  (this is the big one)"
		rm -f "$PACKZIP"
		( cd "$OUT" && zip -qry "$(basename "$PACKZIP")" packs )
		echo "   $(du -h "$PACKZIP" | cut -f1)  $PACKZIP"
	fi
fi
echo "== done.  Copy BOTH zips to the other Mac and unpack them into the same folder."

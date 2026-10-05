#!/bin/sh
# Compile the M5 QuakeC mod into ../m5/progs.dat using the local fteqcc.
#
# The compiler binary lives at ../tools/fteqcc/fteqcc (gitignored).  To
# regenerate it: clone https://github.com/fte-team/fteqw and run
# `make qcc-rel` in fteqw/engine, then copy engine/release/fteqcc here.
#
# Run the game with `-game m5` so the engine loads the compiled progs.
cd "$(dirname "$0")" || exit 1
# the flamethrower's view model (FLAMETHROWER.md) is generated, never committed:
# the public source snapshot refuses any .mdl, and this is ours to build
python3 make_v_flamer.py ../m5/progs/v_flamer.mdl || exit 1
# and its ignition and wind-down sounds (make_flamer_sounds.py), the same way
python3 make_flamer_sounds.py ../m5/sound/m5 || exit 1
exec ../tools/fteqcc/fteqcc -Wall

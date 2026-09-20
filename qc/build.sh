#!/bin/sh
# Compile the M5 QuakeC mod into ../m5/progs.dat using the local fteqcc.
#
# The compiler binary lives at ../tools/fteqcc/fteqcc (gitignored).  To
# regenerate it: clone https://github.com/fte-team/fteqw and run
# `make qcc-rel` in fteqw/engine, then copy engine/release/fteqcc here.
#
# Run the game with `-game m5` so the engine loads the compiled progs.
cd "$(dirname "$0")" || exit 1
exec ../tools/fteqcc/fteqcc -Wall

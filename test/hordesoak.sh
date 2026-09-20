#!/bin/sh
# test/hordesoak.sh -- the headless horde soak with a scripted player (SEPTEMBER2 H).
#
# Boots dm4 windowed and silent with horde mode and the director on, hands the
# player every weapon, and lets him fight as a lawn sprinkler: the thunderbolt
# held down while he turns at SOAK_YAW degrees a second (impulse 9 again every
# 20 s so the cells never run out -- the fight is the subject, not the ammo). No god by default, so
# the director sees a player who takes damage and may die -- that is the point:
# the soak reports how the director read him wave by wave, and how long he
# lasted. SOAK_GOD=1 runs the fat arm (health never drops, the director should
# climb toward its x1.5 ceiling). Not a gate: an instrument for reading the
# director's numbers on a real fight without taking the display.
#
#   SOAK_SECS=180 SOAK_GOD=0 SOAK_YAW=60 sh test/hordesoak.sh
#
# Prints every wave line, every director line and every status report, then a
# one-line summary. The engine's own developer output is left in $SOAK_LOG.
cd "$(dirname "$0")/.." || exit 1
SECS=${SOAK_SECS:-180}
GOD=${SOAK_GOD:-0}
YAW=${SOAK_YAW:-60}
LOG=${SOAK_LOG:-$(mktemp)}
SANDBOX=$(mktemp -d)
CFG=m5/hordesoak_run.cfg
{
	echo "sv_cheats 1"
	echo "cl_yawspeed $YAW"
	[ "$GOD" = 1 ] && echo 'defer 2 "god"'
	echo 'defer 2.5 "impulse 9"'
	echo 'defer 3 "impulse 8"'
	echo 'defer 3.5 "+attack"'
	echo 'defer 3.5 "+left"'
	# Fire is RELEASED around every impulse: W_WeaponFrame returns before its
	# impulse check whenever attack_finished is ahead, and a held thunderbolt
	# keeps it ahead for ever -- impulses typed mid-fire are simply never read.
	t=20
	while [ "$t" -lt "$SECS" ]; do
		echo "defer $t \"-attack\""
		echo "defer $((t + 1)) \"impulse 211\""
		echo "defer $((t + 2)) \"impulse 9\""
		echo "defer $((t + 3)) \"impulse 8\""
		echo "defer $((t + 4)) \"+attack\""
		t=$((t + 20))
	done
	echo "defer $SECS quit"
} > "$CFG"
./darkplaces-sdl -userdir "$SANDBOX" +vid_renderer gl -window -width 640 -height 480 -nosound \
	+developer 1 +m5_stock 0 +m5_horde 1 +m5_horde_director 1 +map dm4 +exec hordesoak_run.cfg >"$LOG" 2>&1
rm -f "$CFG"; rm -rf "$SANDBOX"
grep -E "=== Wave|Wave [0-9]+ cleared|M5 horde: director|^wave [0-9]+:|^director:|You survived|new record" "$LOG"
waves=$(grep -c "=== Wave" "$LOG")
cleared=$(grep -c "cleared" "$LOG")
died=$(grep -c "You survived" "$LOG")
echo "hordesoak: $SECS s, god $GOD, yaw $YAW: $waves waves started, $cleared cleared, died $died (log $LOG)"

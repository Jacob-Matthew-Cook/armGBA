#!/bin/bash
# Runs gamecmp on every game in a list, for each BIOS mode and input seed, several at a time.
# usage: lockstep.sh gamecmp bios.bin games.txt romdir frames "skipbios..." "seeds..." [jobs]

gamecmp=$1
bios=$2
list=$3
romdir=$4
frames=$5
skips=$6
seeds=$7
jobs=${8:-$(grep -c ^processor /proc/cpuinfo)}

out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

i=0
for skip in $skips; do
	for seed in $seeds; do
		while IFS= read -r game; do
			[ -z "$game" ] && continue
			while [ "$(jobs -rp | wc -l)" -ge "$jobs" ]; do
				wait -n
			done
			rom=$(ls "$romdir/$game".gba "$romdir/$game"* 2>/dev/null | head -1)
			n=$(printf %03d $i)
			(
				"$gamecmp" "$bios" "$rom" "$frames" "$skip" "$seed" > "$out/$n.log" 2>&1
				echo "skip $skip seed $seed $game: $(grep -v '^GBA\|^Status' "$out/$n.log" | head -1)" > "$out/$n.sum"
			) &
			i=$((i + 1))
		done < "$list"
	done
done
wait

cat "$out"/*.sum
for log in "$out"/*.log; do
	if ! grep -q "frames match" "$log"; then
		echo "--- $(cat "${log%.log}.sum")"
		grep -v '^GBA\|^Status' "$log" | head -40
	fi
done

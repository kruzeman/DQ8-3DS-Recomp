#!/usr/bin/env bash
# Summarise one save-persistence run: FS/save activity, input edges, guest
# execution, y2r video start-up and RomFS file reads.
#
# usage: tools/persist_summary.sh <label> [<label> ...]
set -u
cd /home/hermes/DQ8 || exit 1

for LABEL in "$@"; do
    L=target/persist-evidence/$LABEL/run.log
    echo "================ $LABEL ================"
    if [ ! -f "$L" ]; then echo "missing $L"; continue; fi
    echo "log lines: $(wc -l < "$L")"
    echo "-- shutdown/timeline"
    sed 's/^/   /' target/persist-evidence/$LABEL/timeline.log
    echo "-- guest execution"
    echo "   guest snapshots : $(grep -c 'guest snapshot' "$L")"
    echo "   escapes         : $(grep -c 'translated PC escaped' "$L")"
    echo "   unsupported ins : $(grep -c 'unsupported' "$L")"
    echo "   abnormal exits  : $(grep -cE 'segmentation fault|assertion|exit\(1[0-9]\)' "$L")"
    echo "-- input (HID edges)"
    grep 'HID buttons' "$L" | sed 's/^/   /' || echo "   (none)"
    echo "-- save FS"
    echo "   FormatSaveData  : $(grep -c 'FS FormatSaveData' "$L")"
    grep 'FS FormatSaveData' "$L" | sed 's/^/   /'
    echo "   ControlArchive  : $(grep -c 'FS ControlArchive' "$L")"
    echo "   native FS opens : $(grep -c 'native FS path=' "$L")"
    grep 'native FS path=' "$L" | sed 's|/home/hermes/DQ8/target/|…/|;s/^/   /'
    echo "   FS Read calls   : $(grep -c 'FS Read handle=' "$L")"
    echo "-- y2r (video, service handle 0x10)"
    echo "   y2r commands    : $(grep -c 'handle=00000010 ' "$L")"
    grep 'handle=00000010 ' "$L" | sed 's/^/   /'
    echo "-- RomFS files read (movies only)"
    python3 tools/romfs_read_report.py target/dq8.romfs "$L" 2>/dev/null | grep -iE 'movie|moflex' | sed 's/^/   /'
    echo "-- userdata contents"
    find "target/userdata-persist-20261021-a" -type f -printf '   %s\t%TY-%Tm-%Td %TH:%TM:%TS\t%p\n' 2>/dev/null | sed 's|/home/hermes/DQ8/target/|…/|'
done

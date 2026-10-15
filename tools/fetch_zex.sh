#!/bin/sh
# Downloads Frank Cringle's ZEXDOC / ZEXALL Z80 instruction exercisers (GPL)
# into tests/data/. Re-run cmake afterwards to register them as tests.
set -eu

dest="$(cd "$(dirname "$0")/.." && pwd)/tests/data"
base="https://raw.githubusercontent.com/anotherlin/z80emu/master/testfiles"

mkdir -p "$dest"
for f in zexdoc.com zexall.com; do
    echo "fetching $f"
    curl -fsSL -o "$dest/$f" "$base/$f"
done
ls -l "$dest"

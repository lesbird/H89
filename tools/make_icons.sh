#!/bin/sh
# Regenerates the app icons from tools/make_icon.swift (macOS only):
# platform/mac/H89.icns and platform/glfw/H89.ico.
set -e
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

swift tools/make_icon.swift "$work/icon.png"

set_dir="$work/H89.iconset"
mkdir "$set_dir"
for s in 16 32 128 256 512; do
    sips -z $s $s "$work/icon.png" --out "$set_dir/icon_${s}x${s}.png" >/dev/null
    d=$((s * 2))
    sips -z $d $d "$work/icon.png" --out "$set_dir/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$set_dir" -o platform/mac/H89.icns

# Windows .ico: PNG-compressed entries, which Windows Vista and later read.
for s in 16 24 32 48 64 128 256; do
    sips -z $s $s "$work/icon.png" --out "$work/ico_$s.png" >/dev/null
done
python3 - "$work" platform/glfw/H89.ico <<'EOF'
import struct, sys
work, out = sys.argv[1], sys.argv[2]
sizes = [16, 24, 32, 48, 64, 128, 256]
images = [open(f"{work}/ico_{s}.png", "rb").read() for s in sizes]
header = struct.pack("<HHH", 0, 1, len(sizes))
offset = len(header) + 16 * len(sizes)
entries = b""
for s, data in zip(sizes, images):
    entries += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), offset)
    offset += len(data)
with open(out, "wb") as f:
    f.write(header + entries + b"".join(images))
EOF
echo "Wrote platform/mac/H89.icns and platform/glfw/H89.ico"

# H89

A Heathkit H89 emulator: portable C++ core, Objective-C++ front end on macOS,
GLFW front end on Windows, OpenGL rendering on both. See [PLAN.md](PLAN.md) for
the architecture and milestones.

**Status:** milestones 0–3, 5 and 6 are done, and 4 is mostly done.
- The Z80 passes ZEXDOC and ZEXALL, including undocumented flags.
- The CPU board boots MTR-89 and MTR-90.
- The H19 terminal is fully emulated: its own Z80 runs the real 444-46
  firmware, with the 6845 CRTC, the 444-29 font, the 8250 serial link and the
  keyboard.
- The H17 floppy controller boots HDOS 2.0 and Heath CP/M 2.2.03. Reading,
  writing and formatting (HDOS INIT) all work.
- The H37 (Z-89-37, WD1797) controller boots HDOS 2.0 and CP/M 2.2.03 from
  single-density disks. Single- and double-density reading, writing and
  formatting (CP/M FORMAT) work.
- The Mac app is interactive. The Windows build is written but untested.

## Running

On first launch the Mac app asks for your ROM folder (see [ROMs](#roms)) and
remembers it; change it with Machine › Choose ROM Folder…. On Windows, pass the
folder on the command line: `H89-glfw.exe path\to\roms` (default `roms`).

### Disks

H17 disk images (`.h8d`, 102,400 bytes) go in drives SY0:–SY2:. H37 disk
images go in drives DK0:–DK3:, either raw sector dumps (`.h37`, optionally
ending in a geometry trailer like `SPT=10 SSZ=0256 TRK=40 SID=1 FM`) or
ImageDisk files (`.imd`).
- **Mac:** use the Disks menu (⌘O inserts into SY0:). Mounted disks are
  remembered between launches. To boot from the H37, tick Machine › Boot from
  H37 (DK0:); this sets SW501 section 4, as on the real board.
- **Windows:** pass images after the ROM folder (`H89-glfw.exe roms hdos.h8d`),
  or drop them on the window. `.h8d` images fill SY0: onwards and others DK0:
  onwards; if the first image is an H37 one, the machine boots from the H37.

Writes go straight back to the image file, like a real disk. Tick "Write
protected" in the Mac open dialog (or use `--ro` with `h89run`) to keep an
image unchanged. To boot, type `B` then Return at the MTR-90 `H:` prompt.

Formatting an H37 disk to a different geometry (for example CP/M FORMAT to
double density) rewrites a raw image with the new geometry once every track
has it; ImageDisk files are rewritten on every change.

### Copying files to and from disks

HDOS 2.0 and Heath CP/M 2.2 disks can be read and changed from the host: H17
disks, and single-sided H37 disks in single density (40 tracks) or double
density (80 tracks, as CP/M FORMAT makes them).
- **Mac:** Disks › SY*n*: › Show Files…, or Disks › Browse Disk Image… (⇧⌘O)
  for any `.h8d`, `.h37` or `.imd` file. Insert Files… (or drop files on the list) copies files onto
  the disk. Extract or Extract All copies them to a folder on the Desktop named
  after the image. Delete removes them.
- **Command line:** `h8dtool list|get|put|rm image.h8d ...` (see below).

Files come out exactly as stored: whole sectors (HDOS, padded with zeros) or
whole 128-byte records (CP/M, padded with ^Z). Host names are converted to
8.3 upper case. Files inserted into HDOS disks are dated today, but in 1999,
because HDOS 2.0 can't show later years. If the disk is in a drive while you
change it, the running OS won't see the change until you DISMOUNT and MOUNT it
(HDOS) or warm boot with Control-C (CP/M).

```sh
build/h8dtool list disks/CPM_2-2-03.H8D
build/h8dtool get  disks/CPM_2-2-03.H8D -d out PIP.COM   # no names: every file
build/h8dtool put  mydisk.h8d hello.asm [--replace]
build/h8dtool rm   mydisk.h8d HELLO.ASM
```

### Keyboard

| H19 key | Host key |
|---|---|
| F1–F5 | F1–F5 |
| ERASE, BLUE, RED, WHITE | F6, F7, F8, F9 |
| BREAK | F10 (held) |
| SCROLL | Page Down |
| Cursor keys (shifted keypad) | Arrow keys, Home |
| Keypad 0–9 . ENTER | Numeric keypad |
| BACK SPACE / DELETE | Backspace (Mac: delete) / Delete (Mac: fn-delete) |

Host shortcuts: Mac ⌘R reset, ⌘1/2/3 phosphor colour; Windows Ctrl+Alt+R and
Ctrl+Alt+1/2/3.

## Headless runner

`h89run` runs the whole machine in a text terminal, showing the H19's screen
and typing through its keyboard:

```sh
build/h89run                                   # boots to the MTR-90 H: prompt
build/h89run --disk0 disks/HDOS_2-0_System.h8d # then type B, Return
```

Press Ctrl-] to quit. With no ROM options, it loads the ROMs by part number
from the first of `--roms <dir>`, `$H89_ROMS`, `./roms`, or `roms/` next to
(or one level above) the executable, so `build/h89run` works from the
project folder or anywhere else.

Options:
- `--disk0/1/2 image.h8d`, `--dk0/1/2/3 image.h37|.imd` (H37 drives), `--ro`
  (mount disks write-protected)
- `--sw501 hex` (default `20`; `30` boots from the H37), `--ram kb`,
  `--trace-io`
- `--rom`, `--h17rom`, `--h19rom`, `--font` load individual ROMs instead.
  Without `--h19rom`/`--font`, the console serial port connects straight to
  your terminal.
- For scripts: `--seconds n` runs that much emulated time as fast as possible
  and prints the final screen. `--step "text|keys"` waits for `text` in the
  output, then types `keys`. `--expect text` sets the exit status.

```sh
build/h89run --ro --disk0 disks/HDOS_2-0_System.h8d --seconds 120 \
    --step 'H: |B\r' --step 'ACTION?|\r' --step ')?|14-Oct-86\r' --step '>|CAT\r'
```

## Building

Requires CMake 3.21+ and a C++20 compiler.

### macOS

```sh
cmake -S . -B build -G Xcode        # or -G Ninja, or omit for Makefiles
cmake --build build --config Release
open build/Release/H89.app          # build/H89.app for single-config generators
```

### Windows

```sh
cmake -S . -B build                 # Visual Studio generator; fetches GLFW 3.4
cmake --build build --config Release
build\Release\H89-glfw.exe
```

Host shortcuts in the GLFW build: Ctrl+Alt+R reset, Ctrl+Alt+1/2/3 phosphor colour.

## Tests

```sh
tools/fetch_zex.sh                  # downloads ZEXDOC/ZEXALL into tests/data/
cmake -S . -B build                 # re-run so the ZEX tests get registered
cmake --build build
ctest --test-dir build -C Release           # all tests (ZEX takes ~40 s each)
ctest --test-dir build -C Release -LE slow  # unit tests only
```

## ROMs

The H89 and H19 ROMs and disk images are copyrighted and are not included. Put
raw binary dumps in a folder; files are recognised by the part number in their
names:

| Part | ROM | |
|---|---|---|
| 444-142 or 444-62 | MTR-90 or MTR-89 monitor | required |
| 444-46 | H19 terminal firmware | required |
| 444-29 | H19 character generator | required |
| 444-19 | H17 floppy boot ROM | optional (needed for disks) |

The repo's `roms/` and `disks/` folders are git-ignored. When ROMs (and
`*HDOS*.h8d` / `*CPM*.h8d` images) are there, CMake also registers boot,
terminal and disk tests that use them. Disk tests mount images read-only.

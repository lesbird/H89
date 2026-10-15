# H89

A Heathkit H89 emulator: portable C++ core, Objective-C++ front end on macOS,
GLFW front end on Windows, OpenGL rendering on both. See [PLAN.md](PLAN.md) for
the architecture and milestones.

**Status:** version 1.0. Every milestone in the plan is done; the Windows
build is built and packaged by CI but has had little hands-on testing.
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
- The Mac app and the Windows program come with the ROMs and with HDOS 2.0
  and CP/M 2.2.03 boot disks for both controllers.

## Installing

Download a release from the
[Releases page](https://github.com/lesbird/H89/releases), or build it (see
[Building](#building)).
- **macOS 11 or later** (Apple silicon or Intel): open
  `H89-<version>-macOS.dmg` and drag H89 to Applications. If the build isn't
  notarized, the first time open it with right-click › Open (or System
  Settings › Privacy & Security › Open Anyway).
- **Windows 10 or later:** run `H89-<version>-Windows.exe` to install
  (Start menu and desktop shortcuts), or unzip `H89-<version>-Windows.zip`
  anywhere and run `H89.exe`.

## Running

H89 starts at the MTR-90 monitor's `H:` prompt with disks already in the
drives: HDOS 2.0 in SY0:, CP/M 2.2.03 in SY1:, and the H37 CP/M and HDOS
disks in DK0: and DK1:. Type `B` then Return to boot HDOS from SY0:. To boot
from the H37 instead, choose it in Settings (Boot from).

The first time it runs, H89 copies its disks to a folder of your own, so
changing them never touches the originals: `~/Library/Application
Support/H89/Disks` on the Mac and `Documents\H89\Disks` on Windows. Restore
Original Disks (Mac: Disks menu or Settings; Windows: Settings › Disks) puts
fresh copies back.

### Settings

- **Mac:** H89 › Settings… (⌘,).
- **Windows:** Ctrl+Alt+P shows the settings panel. It opens by itself the
  first time and lists the keyboard shortcuts under Keys.

Settings cover:
- the ROMs: the built-in ones or a folder of your own (see [ROMs](#roms))
- the monitor (MTR-90 or MTR-89)
- memory (16K to 64K)
- the boot device (H17 or H37), and whether to boot at power-on (SW501
  section 8)
- speed
- phosphor colour and CRT effects
- the bell (the H19's speaker, the system alert sound, or off) and key click

Changing the ROMs, monitor or memory restarts the machine. On Windows the
panel also has tabs for the drives, the serial ports and saved states.
Settings are kept in `%APPDATA%\H89\settings.ini` on Windows.

### Disks

H17 disk images (`.h8d`, 102,400 bytes) go in drives SY0:–SY2:. H37 disk
images go in drives DK0:–DK3:, either raw sector dumps (`.h37`, optionally
ending in a geometry trailer like `SPT=10 SSZ=0256 TRK=40 SID=1 FM`) or
ImageDisk files (`.imd`).
- **Mac:** use the Disks menu (⌘O inserts into SY0:). Mounted disks are
  remembered between launches. To boot from the H37, tick Machine › Boot from
  H37 (DK0:); this sets SW501 section 4, as on the real board.
- **Windows:** use the Disks tab in Settings, drop images on the window, or
  pass them on the command line (`H89.exe hdos.h8d`). Dropped and
  command-line images fill SY0: onwards (`.h8d`) or DK0: onwards (others);
  if the first one is an H37 image, the machine boots from the H37. Disks
  stay in their drives between runs.
- **Mac:** double-clicking an `.h8d`, `.h37` or `.imd` file also inserts it,
  into the first empty drive of its kind.

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

Host shortcuts: Mac ⌘R reset, ⌘1/2/3 phosphor colour, ⌘T turbo, ⌘, settings;
Windows Ctrl+Alt+R, Ctrl+Alt+1/2/3, Ctrl+Alt+T and Ctrl+Alt+P.

The H19's speaker is reproduced: a 2 kHz bell (its 2.048 MHz clock divided
by 1024) and a short click for each key, which the terminal's own firmware
asks for, so the key click obeys the H19's settings. Clicks are muted
above normal speed.

The screen is drawn like a CRT: phosphor persistence (short for white P4,
longer for green and amber), a soft glow, scanlines and a gently curved tube
face. Turn it off with View › CRT Effects (Mac) or Ctrl+Alt+C (Windows) for a
flat, crisp picture.

### Debugger

A Dear ImGui debugger shows the CPU board's Z80 registers (editable while
paused), a disassembly that follows the PC, breakpoints (click an address in
the disassembly to toggle one), a memory view you can edit, and the state of
the disk controllers. Run, Pause, Step and Step Over (which runs a CALL, RST,
DJNZ or LDIR-style instruction to completion) control execution. Hitting a
breakpoint pauses the machine and opens the debugger.
- **Mac:** Machine › Debugger (⌘D) opens it in its own window.
- **Windows:** Ctrl+Alt+D shows it over the screen.

Configure with `-DH89_DEBUGGER=OFF` to build without it (and without
fetching Dear ImGui).

### Printer and serial ports

The serial card's three ports can be connected to the host: LP: (the
printer, at E0h), MODEM (D8h) and AUX (D0h). Each goes to a file (output is
appended), listens on a local TCP port for one client (`telnet localhost
8089` or `nc localhost 8089`), or connects out to a TCP host. Input is
delivered as fast as the H89 reads it, so nothing overruns even at 300 baud.
- **Mac:** Machine › Serial Ports.
- **Windows:** the Serial Ports tab in Settings, or
  `H89.exe --printer printer.txt --modem listen:8089`. Connections are
  remembered.
- **h89run:** `--printer file`, `--modem spec`, `--aux spec`, where spec is a
  path, `listen:PORT` or `connect:HOST:PORT`.

A TCP connection raises CTS, DSR and DCD, as a modem would. A printer file
leaves them low, which Heath CP/M's printer driver reads as "not busy". In
Heath CP/M 2.2.03, `PIP LST:=FILE` prints and `PUN:`/`RDR:` use the MODEM
port, at 300 baud unless CONFIGUR changes it.

### Speed and saved states

The machine normally runs at its real speed (2.048 MHz). Mac: Machine › Speed
offers 2×, 4× and Maximum, and ⌘T toggles Maximum (turbo). Windows:
Ctrl+Alt+T toggles turbo.

A saved state captures the whole machine: memory, both Z80s, the terminal,
the floppy controllers mid-operation, and which disk images are in the drives.
The disks themselves stay in their image files, so loading a state never
overwrites them; you're warned if one has changed or gone missing since the
save. States only load with the ROMs they were saved with.
- **Mac:** Machine › Save State… (⌘S) and Load State… (⌘L).
- **Windows:** Settings › States saves and loads state files. Ctrl+Alt+S and
  Ctrl+Alt+L save and load a quick state, `Documents\H89\H89.h89state`.
- **h89run:** `--save-state file` when the run ends, `--load-state file` to
  start from one.

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
- `--save-state f`, `--load-state f`
- `--printer file`, `--modem spec`, `--aux spec` (serial ports, see above)
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
build\Release\H89.exe
```

The program finds `roms\` and `disks\` next to itself, or in the source tree
when run from the build folder. Host shortcuts: Ctrl+Alt+P settings,
Ctrl+Alt+R reset, Ctrl+Alt+1/2/3 phosphor colour, Ctrl+Alt+C CRT effects,
Ctrl+Alt+D debugger, Ctrl+Alt+T turbo, Ctrl+Alt+S / Ctrl+Alt+L save / load
the quick state. Options `--printer`, `--modem` and `--aux` connect the
serial ports; a folder given on the command line is used for the ROMs.

### Packaging a release

- **macOS:** `tools/package_mac.sh` builds a universal app and writes
  `dist/H89-<version>-macOS.dmg`. Set `H89_SIGN_IDENTITY` to a "Developer ID
  Application" certificate to sign it for distribution, plus
  `H89_NOTARY_PROFILE` (a `notarytool store-credentials` profile) or
  `APPLE_ID`, `APPLE_TEAM_ID` and `APPLE_APP_PASSWORD` to notarize it.
  Without an identity it is signed ad hoc.
- **Windows:** `cpack -C Release` in the build folder writes an NSIS
  installer and a `.zip` (NSIS must be installed).
- **GitHub:** pushing a tag like `v1.0.0` runs the Release workflow, which
  builds both and publishes them as a GitHub release. It signs and
  notarizes the Mac build when the secrets listed at the top of
  `.github/workflows/release.yml` are set.

The version is in `CMakeLists.txt` (`project(H89 VERSION ...)`). The app
icon is drawn by `tools/make_icon.swift`; `tools/make_icons.sh` regenerates
`H89.icns` and `H89.ico` from it.

## Tests

```sh
tools/fetch_zex.sh                  # downloads ZEXDOC/ZEXALL into tests/data/
cmake -S . -B build                 # re-run so the ZEX tests get registered
cmake --build build
ctest --test-dir build -C Release           # all tests (ZEX takes ~40 s each)
ctest --test-dir build -C Release -LE slow  # unit tests only
```

## ROMs

The ROMs in `roms/` are built into the apps. To use others, choose a folder
in Settings (or set `H89_ROMS` on the Mac, or pass the folder to `h89run`
with `--roms`). Put raw binary dumps in it; files are recognised by the part
number in their names:

| Part | ROM | |
|---|---|---|
| 444-142 or 444-62 | MTR-90 or MTR-89 monitor | required |
| 444-46 | H19 terminal firmware | required |
| 444-29 | H19 character generator | required |
| 444-19 | H17 floppy boot ROM | optional (needed for disks) |

CMake also registers boot, terminal and disk tests that use the ROMs in
`roms/` and the `*HDOS*` / `*CPM*` images in `disks/`. Disk tests mount
images read-only.

The ROMs and disk images are Heath/Zenith software, included for preserving
and running these machines.

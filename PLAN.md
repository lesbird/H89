# Heathkit H89 Emulator — Plan

Portable C++ emulation core, Objective-C++ front end on macOS, a thin
Win32/GLFW front end on Windows, and OpenGL for display on both.

Hardware details below are from memory and must be verified against the
Heathkit manuals and schematics before implementing each device.

## 1. Architecture

```
┌──────────────────────────────────────────────────────────┐
│ Platform front ends                                      │
│  macOS: Objective-C++ (.mm)    Windows: Win32/GLFW (C++) │
│  window, menus, file dialogs, keyboard, audio, timing    │
├──────────────────────────────────────────────────────────┤
│ Renderer (C++, OpenGL 3.3 core / 4.1 on macOS)           │
│  CRT texture upload + phosphor shader, ImGui debugger    │
├──────────────────────────────────────────────────────────┤
│ Emulation core (C++20, no platform headers)              │
│  Z80 x2, bus, devices, disks, scheduler, save states     │
└──────────────────────────────────────────────────────────┘
```

The core talks to the outside world only through small interfaces
(`IVideoSink`, `IAudioSink`, `IHostSerial`) implemented by each front end.

## 2. Hardware to emulate

| Subsystem | Details |
|---|---|
| CPU board | Z80 @ 2.048 MHz (4/6 MHz speed mods optional), 16–64K RAM |
| Monitor ROM | MTR-88 / MTR-89 / MTR-90 at `0x0000`, selectable |
| ORG-0 / CP/M mapping | GPP `0xF2` (write) swaps ROM out so RAM sits at address 0 |
| 2 ms timer | Timer interrupt (RST 1), enabled via a GPP bit |
| SW501 | Read on `0xF2`; disk controller addresses and boot device |
| Serial (8250) | `0xE8` console (to terminal board), `0xE0` printer, `0xD8` modem, `0xD0` aux |
| H17 floppy | Ports `0x7C–0x7F`, ROM at `0x1800`, RAM at `0x1400`; hard-sectored 10x256, 40 tracks; `.h8d` |
| H37 / H47 floppy | WD1797 at `0x78–0x7B`; soft-sectored; `.imd` |
| Terminal board (H19) | Own Z80 + terminal ROM, 6845 CRTC, 2K video RAM, char ROM, MM5740 keyboard encoder, bell; 80x24 + status line |

The terminal board is emulated fully (second Z80 running the real terminal
ROM) so escape sequences and quirks are exact.

## 3. Source layout

```
core/       cpu/Z80, Bus, Scheduler, devices/, terminal/, media/, Machine, Config
render/     GLRenderer, shaders, debugger (Dear ImGui)
platform/   mac/ (Objective-C++), win/ (GLFW)
tests/      CPU tests, device tests, boot tests
roms/       user-supplied, git-ignored
```

## 4. Threading and timing

- The emulation thread runs ~1 ms slices, interleaving both Z80s via the Scheduler.
- Throttle against the host clock (or audio buffer); turbo mode runs unthrottled.
- The 6845 renders a 640x250 framebuffer per frame into a triple buffer; the
  render thread uploads the latest as a `GL_R8` texture.
- Host key events go through a lock-free queue drained by the emulation thread.

## 5. Milestones

| # | Milestone | Done when |
|---|---|---|
| 0 ✅ | Scaffolding: CMake, tests, CI for macOS + Windows | Window opens on both platforms |
| 1 ✅ | Z80 core | ZEXDOC/ZEXALL pass under a CP/M test harness |
| 2 ✅ | CPU board, headless: bus, ROM, RAM, GPP, timer, 8250 to stdout | MTR-90 prints its `H:` prompt |
| 3 ✅ | Terminal board: 2nd Z80, terminal ROM, 6845, char ROM | Power-on screen renders to PNG |
| 4 🟡 | Front ends + OpenGL, keyboard maps, bell audio | Interactive monitor on both OSes (Mac done; Windows untested; bell is the system beep) |
| 5 ✅ | H17 + HDOS: hole timing, sync detection, `.h8d` | HDOS 2.0 boots |
| 6 🟡 | H37 + CP/M: WD1797, `.imd`, ORG-0 | CP/M 2.2 boots (done on H17 with ORG-0; H37/WD1797 not started) |
| 7 | Extras: CRT shader, ImGui debugger, printer/serial, save states, speed control | Feature-complete |
| 8 | Packaging: prefs UI, notarized `.app`, Windows installer | Release |

## 6. Testing

- ZEXDOC/ZEXALL plus per-instruction tests.
- Golden-frame tests: run headless N frames, hash the framebuffer.
- Headless HDOS / CP/M boot tests checking for an expected screen string.
- Instruction-trace diffs against MAME's `h89` driver.

## 7. Risks

- macOS OpenGL is deprecated (max 4.1): keep GL behind `GLRenderer` so a Metal
  backend can be added later.
- H17 timing is the hardest part: model 300 RPM rotation and hole pulses in cycles.
- ROMs and disk images are copyrighted: load from a user folder, never bundle.
- High-DPI: render native 640x250, scale in the shader.

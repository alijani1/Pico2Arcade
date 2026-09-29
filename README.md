# PicoGalagino

A port of Till Harbaum's [Galagino](https://github.com/harbaum/galagino) ESP32
arcade emulator to the PicoCartReader hardware — RP2350B (Olimex Pico2XL)
with an ST7789 240×320 TFT, PCA9539 I²C expander for buttons, and a JoyCon
analog stick.

Six games run from **MAME ZIP files dropped onto an SD card at runtime** —
no Python tools, no ROM-conversion step, no rebuild per game:

- Pac-Man
- Ms. Pac-Man (with the Midway F2 aux-board emulated)
- Galaga
- Frogger
- Donkey Kong
- Dig Dug

A custom text menu lets you pick a game at boot; press-and-hold the MENU
button while playing to soft-reboot back to the picker.

## Hardware

- **MCU**: RP2350B on the Olimex Pico2XL (48 GPIO, 200 MHz sys clock)
- **Display**: ST7789 240×320 on SPI0 @ 100 MHz (uses the GMT024-01 panel's
  custom init overlay from PicoCartReader — the file `.pio/libdeps/pico2/TFT_eSPI/TFT_Drivers/ST7789_Init.h` must be replaced with our overlay per pin group memory)
- **SD card**: SPI1 (SCK=GP10, MOSI=GP11, MISO=GP12, CS=GP13)
- **Audio**: PWM out on GP22 → PAM8302 amp (enable on GP4)
- **Input**: PCA9539 on I²C1 @ 0x74 (JoyCon stick + fire buttons) plus
  direct GPIOs for START (GP1), COIN/SELECT (GP0), and MENU (GP37)

## Using it — SD card layout

Drop MAME ZIPs into `/roms/` on the FAT-formatted SD card:

```
/roms/puckman.zip     ← Pac-Man
/roms/mspacman.zip    ← Ms. Pac-Man (needs Midway F2 aux ROMs — see below)
/roms/galaga.zip      ← Galaga
/roms/frogger.zip     ← Frogger
/roms/dkong.zip       ← Donkey Kong
/roms/digdug.zip      ← Dig Dug
```

Only the ZIPs for the games you want are required. Missing ZIPs just cause
the corresponding menu entry to say "LOAD FAILED" if picked; the rest of
the games keep working.

You do **not** need to unpack the ZIPs, run any Python script, or install
any tooling. The firmware reads the ZIP directly off SD, decompresses the
files (STORED + DEFLATE), and converts them in RAM to the format Galagino
expects — this is a full C port of the five Python converters that ship
with upstream (`romconv`, `cmapconv`, `tileconv`, `spriteconv`, `audioconv`).

### Where the ZIPs come from

MAME's parent/clone sets. Files needed inside each ZIP:

| Game | ZIP | Required files |
|------|-----|----------------|
| Pac-Man | `puckman.zip` | `pm1_prg1..8`, `pm1_chg1..4`, `pm1-1.7f`, `pm1-4.4a`, `pm1-3.1m`, `pm1-2.3m` |
| Ms. Pac-Man | `mspacman.zip` | `pacman.6e/6f/6h/6j`, `u5`, `u6`, `u7`, `5e`, `5f`, `82s123.7f`, `82s126.4a`, `82s126.1m`, `82s126.3m` |
| Galaga | `galaga.zip` | `gg1_1b.3p`, `gg1_2b.3m`, `gg1_3.2m`, `gg1_4b.2l`, `gg1_5b.3f`, `gg1_7b.2c`, `gg1_9.4l`, `gg1_11.4d`, `gg1_10.4f`, `prom-5.5n`, `prom-3.1c`, `prom-4.2n`, `prom-1.1d` |
| Frogger | `frogger.zip` | `frogger.26/27`, `frsm3.7`, `frogger.606..610`, `pr-91.6l` |
| Donkey Kong | `dkong.zip` | (see mame_rom_loader.cpp Dkong loader) |
| Dig Dug | `digdug.zip` | `dd1a.1..6`, `dd1.7`, `dd1.10b`, `dd1.9`, `dd1.11`, `dd1.12..15`, `136007.109..113` |

We DO NOT distribute ROMs. Obtain them lawfully.

## Build

Requires PlatformIO with the RP2040 platform + earlephilhower core.

```bash
pio run
pio run -t upload
```

Which games are compiled in is set at the top of [`src/config.h`](src/config.h):

```c
#  define ENABLE_PACMAN
#  define ENABLE_MSPACMAN
#  define ENABLE_GALAGA
#  define ENABLE_FROGGER
#  define ENABLE_DKONG
#  define ENABLE_DIGDUG
```

Comment any line to drop that game — everything else keeps working.

## How the port works

Upstream Galagino was designed to run pre-converted game data compiled
into flash on an ESP32. This port turns it into a **runtime SD loader**
so users don't need Python or any host-side tooling.

### Architecture at a glance

```
┌───────────┐   ┌──────────────────┐   ┌────────────────────┐
│ core 0    │   │ core 1           │   │  hardware          │
│           │   │                  │   │                    │
│ setup()   │   │ emulation_task() │   │ ST7789 (SPI0)      │
│ pico_menu │   │  ├─ prepare_     │   │ SD (SPI1)          │
│  pick&    │──▶│  │  emulation()  │──▶│ PCA9539 (I²C1)     │
│  load()   │   │  └─ while(1)     │   │ PWM audio (GP22)   │
│           │   │      emulate_    │   │ AMP enable (GP4)   │
│ render    │◀──│      frame()     │◀──│ JoyCon (ADC)       │
│  loop     │   │                  │   │                    │
└───────────┘   └──────────────────┘   └────────────────────┘
```

Core 0 handles the menu, the SD load, and the render/scan-out to the TFT
(the "video task" in upstream lingo). Core 1 runs the Z80/i8048 emulator
in a tight `emulate_frame()` loop, gated on a vblank counter that core 0
increments once per screen refresh.

A `g_menu_done` flag holds core 1 in `setup1()` until the user has picked
a game and the runtime ROM pointers are populated — so Galagino's own
attract-mode menu never runs.

### Hitting 60 Hz on the RP2350

Real arcade hardware runs at 60 Hz. Getting Galagino there on the RP2350B
took a chain of specific choices; if any one drops, the emulator halves
its speed and everything drags.

**System / peripheral clocks.** The `board_build.f_cpu = 200000000L` in
[`platformio.ini`](platformio.ini) runs the CPU at 200 MHz (2× default).
SPI runs at 100 MHz (`-DSPI_FREQUENCY=100000000`). Galaga is the tightest
of the six games — its three-Z80 frame takes close to 13 ms of the 16.6
ms budget, so the CPU headroom matters. Ms. Pac-Man's F2 fetch adds a
small per-opcode branch on top of that.

**ST7789 init overlay.** TFT_eSPI's stock ST7789 init sequence is wrong
for the GMT024-01 panel we use — colours invert and gamma is off. The
override lives in `PicoCartReader/tft_espi_override/` (from the sister
project) and must be dropped into
`.pio/libdeps/pico2/TFT_eSPI/TFT_Drivers/ST7789_Init.h` before the first
build. Without it every frame looks bad regardless of speed.

**One `pushImage` per frame.** The render task in
[`src/video.cpp`](src/video.cpp) builds the whole 224×288 game framebuffer
in RAM and hands it to TFT_eSPI as a single `pushImage` per screen
refresh — no per-row DMA restarts, no per-tile flushes. At 100 MHz SPI,
that transfer costs about `224*288*2*8 / 100e6 = 10.3 ms`, leaving ~6 ms
for the render pass. Splitting the transfer smaller was tried and
consistently missed the vblank budget.

**Half-rate display, double-rate emulation.** Config leaves `TFT_SPICLK`
undefined, which selects Galagino's `VIDEO_HALF_RATE` mode: the render
loop calls `xTaskNotifyGive` **twice per screen refresh** and the display
runs at 30 fps while the emulator gets **two vblank pulses per refresh**
and ticks at 60 Hz. Trying a 1:1 sync pinned the emulator to display
rate (~30 fps) and everything felt sluggish; the double-pulse fix keeps
game clocks honest.

**vblank counter must decrement, not clear-all.** Real FreeRTOS's
`ulTaskNotifyTake(pdTRUE, …)` clears the count and returns the old
value. That semantic is wrong for `VIDEO_HALF_RATE`: if two pulses arrive
between `emulate_frame()` calls and we clear, the second pulse is lost
and the emulator drops to half speed. Our stub in
[`src/pico_freertos_stubs.c`](src/pico_freertos_stubs.c) always
decrements by one instead — every pulse is accounted for regardless of
the caller's `clearOnExit` argument.

**Core split.** Core 0 owns the video task (100 MHz SPI + TFT_eSPI is
already close to full-duty on the peripheral side). Core 1 runs
`emulation_task()` — 60 Hz Z80/i8048 stepping plus the audio-render
loop. `pico_serial_log` writes are buffered so debug prints don't stall
either core.

**Result.** Consistent 60 fps emu / 30 fps display across all six games,
verified via the `[emu] 60 takes in ~1000 ms = 60 fps` and `[video]
60 frames in ~2000 ms = 30 fps` counters in the serial log. Galaga is
the closest to the edge but sits comfortably at 59–60 fps in gameplay.

### SD → memory pipeline

Each game has a `*_runtime.cpp` file with a `runtime_load_<game>()`
entrypoint. When the menu picks a game, that function:

1. Slurps the ZIP off SD into a malloc'd buffer.
2. Walks the local-file-header stream, extracts each needed file with our
   in-tree `zip_extract` (STORED + uzlib DEFLATE — see [`src/uzlib`](src/uzlib)).
3. Calls the appropriate loaders in [`src/mame_rom_loader.cpp`](src/mame_rom_loader.cpp)
   to build the exact byte-layouts Galagino expects:
   - `load_<game>_rom_*` — CPU ROMs concatenated + patched
   - `load_<game>_tilemap_*` — 2-bpp planar tile decode
   - `load_<game>_spritemap_*` — 16×16 sprite decode with 4 flip variants
   - `load_<game>_cmap_*` — palette PROM + colormap lookup PROM merged into RGB565
   - `load_<game>_wavetable_*` — Namco WSG3 or AY-3-8910 wave data
4. Reassigns the runtime pointers (`pacman_rom`, `pacman_5e`, etc.)
   originally declared in `*_runtime.cpp`.

Since Galagino was written to reference these as `const` flash arrays,
we replaced the header `#include`s in `pacman.h`/`galaga.h`/etc. with
`extern` pointer declarations, then patched the Z80 core's optimized
`digdug_StepZ80` and `emulation.h`'s `OpZ80_INL` to fetch through the
pointers with per-CPU address masks (bounds-checking against the actual
malloc'd buffer size).

### Ms. Pac-Man's F2 aux board

Ms. Pac-Man ran on stock Pac-Man hardware with a Midway daughterboard
between the Z80 and its ROMs. The board:

- Replaces `pacman.6j` with a decoded copy of `u7` at 0x3000-0x3FFF
- Adds a decoded 8 KB aux ROM at 0x8000-0x9FFF (from `u5`+`u6`)
- Mirrors the aux ROM half at 0xA000-0xBFFF
- Enables the Ms. Pac-Man map on any access in `0x3FF8-0x3FFF` (ENABLE trap)
- Reverts to plain Pac-Man on any access in seven small "disable" windows

We port the GalaginoPlus build-time decoder (bit-swap on data, addr-swap
on A11/A12, plus a 40-entry JMP-stub patch table) to run once at load
time. The runtime bank-switch lives in `mspacman_fetch()` in
[`src/mspacman_runtime.cpp`](src/mspacman_runtime.cpp), called from
`OpZ80_INL` and `pacman_RdZ80` when `machine == MCH_MSPACMAN`.

Two graphics patch tables (`mspacman_til_patch.h` + `mspacman_spr_patch.h`,
20 KB of flash data ported from GalaginoPlus's `.table` files) apply
signed per-position deltas on top of the base decoded tile/sprite data.

### Custom menu

[`src/pico_menu.cpp`](src/pico_menu.cpp) draws a simple text list of the
compiled-in games using TFT_eSPI, waits for UP/DOWN + FIRE/START, then
runs the chosen game's `runtime_load_*()` and sets `machine = MCH_*`.
Only after that does it release the `g_menu_done` gate so core 1 can
start emulating.

The MENU button (GP37) is polled by [`src/pico_input.cpp`](src/pico_input.cpp);
holding it for 250 ms calls `rp2040.reboot()` to return to the picker.

## Directory layout

```
platformio.ini           PIO build config for the pico2 (RP2350B) env
src/
  config.h               ENABLE_<game> flags, pin map, dip switches
  emulation.[ch]         Z80 dispatch, machine dispatch, IRQ handling
  galagino.cpp           video/audio glue (mostly upstream, with our tweaks)
  <game>.h               per-game read/write/render/prepare (upstream)
  <game>_runtime.cpp     per-game SD loader + runtime pointer definitions
  <game>_logo.h          per-game splash image data (upstream)
  mame_rom_loader.cpp    ZIP-to-Galagino-format converters (all 5, in C)
  z80_core.h             selects the Z80 CPU core
  z80_zazu/              superzazu/z80 (MIT) + Galagino patches/adapter
  i8048.[ch]             Intel 8048 emulator for DKong sound (upstream)
  pacman.h ... digdug.h  per-machine memory maps + render logic (upstream)
  pico_audio.[ch]        PWMAudio wrapper (our port)
  pico_input.cpp         PCA9539 + JoyCon + direct-GPIO button reader
  pico_menu.cpp          our runtime game picker
  pico_sd.cpp            SD.begin + file listing
  pico_compat.h          ESP32→Pico shims (Serial, xTaskCreate, etc.)
  pico_freertos_stubs.c  vTaskDelay, ulTaskNotifyTake, notify-give
  pico_core1.cpp         setup1() — waits for menu, then runs emulator
  video.[ch]             TFT_eSPI backend (replaces upstream ESP-IDF SPI)
  driver/i2s.h           ESP-IDF I²S no-op stub, forwards to pico_audio
  uzlib/                 vendored DEFLATE decompressor for ZIP extraction
  mspacman_*_patch.h     20 KB of Ms. Pac-Man gfx delta tables
```

## Credits

- **Till Harbaum** — [Galagino](https://github.com/harbaum/galagino), the
  upstream ESP32 emulator this port is based on.
- **VirtualClaudioBoy** — [GalaginoPlus](https://github.com/VirtualClaudioBoy/GalaginoPlus)
  Ms. Pac-Man F2 aux-board decoder and the two gfx patch tables.
- **Nicolas Allemand (superzazu)** — [z80](https://github.com/superzazu/z80),
  the Z80 CPU core (MIT, see `src/z80_zazu/LICENSE`). It replaced upstream
  Galagino's Z80 core, which is not licensed for commercial use.
- **MAME team** — for documenting the arcade hardware so exhaustively.

## License

Port glue in `src/pico_*.cpp`, `src/*_runtime.cpp`, and `src/mame_rom_loader.cpp`
is under the same terms as upstream Galagino. Upstream Galagino sources
retain their original licenses. See the individual files for details.

# PicoGalagino

Port of Till Harbaum's [Galagino](https://github.com/harbaum/galagino) ESP32 arcade emulator to the PicoCartReader hardware (RP2350B + ST7789 SPI TFT + PCA9539 + JoyCon).

Runs (or intends to run) Pac-Man, Galaga, Donkey Kong, Frogger, Digdug, 1942.

## Status

- **Scaffold in place** — platformio.ini targets `olimex_pico2xl` (same RP2350B config as PicoCartReader), TFT_eSPI is wired for our panel.
- **Source imported** — upstream cloned to `upstream/`, portable files copied into `src/`.
- **Video ported** — `video.cpp` rewritten against TFT_eSPI (replacing upstream's ESP-IDF SPI driver).
- **Config remapped** — `src/config.h` uses our pins (GP0 SELECT = COIN, GP1 = START, etc.).
- **Currently blocked on ROMs / Z80 emulator source** (user-obtainable — see below).

## What you need to obtain

Galagino cannot legally ship the ROMs or the Z80 emulator source, so you (the user) need to download them:

1. **Z80 emulator** — Marat Fayzullin's Z80 source
   Download: https://fms.komkon.org/EMUL8/Z80-081707.zip
   Unzip into `upstream/roms/` so files `Z80.c`, `Z80.h`, `Codes.h`, `CodesCB.h`, `CodesED.h`, `CodesXCB.h`, `CodesXX.h`, `Tables.h` land there.

2. **MAME ROM sets** (start with just Pac-Man for the minimal build)
   Place these ZIPs into `upstream/roms/`:
   - `puckman.zip` — Pac-Man (~24KB total)
   - Later: `galaga.zip`, `dkong.zip`, `frogger.zip`, `digdug.zip`, `1942.zip`

3. **Run the ROM converter** (needs Python)
   ```
   cd upstream/romconv
   python romconv.py  (or per-game scripts)
   ```
   This spits out generated headers (`pacman_rom.h`, `pacman_tilemap.h`, etc.) into `upstream/galagino/`.

4. **Copy generated headers into src/**
   ```bash
   cp upstream/galagino/*_rom*.h upstream/galagino/*_tilemap.h \
      upstream/galagino/*_spritemap.h upstream/galagino/*_cmap*.h \
      upstream/galagino/*_wavetable.h upstream/galagino/*_charmap.h \
      upstream/galagino/Z80.h upstream/galagino/Z80.c \
      src/
   ```

## What's left to port (once ROMs are in place)

- **Audio** — upstream uses ESP32 I2S driver. Need to replace with our PWM audio on GP22 + AMP_EN on GP4 (same as PicoCartReader).
- **Input glue** — upstream reads buttons via raw ESP32 GPIO / Nunchuck I2C. Need a small `pico_glue/input.cpp` that reads our PCA9539 (Port 1 stick + BTN_A/B) and direct GPIO buttons, exposing the state the way `galagino.cpp` expects.
- **Menu integration** — decide whether to boot into a Galagino-style attract-mode menu or hook into the PicoCartReader menu.

## Build

Once ROMs are dropped in and `Z80.h`/`Z80.c` are copied to `src/`:

```bash
pio run
```

Only Pac-Man is enabled initially (via `-DGAL_ONLY_PACMAN` in platformio.ini). Remove that flag to enable all 6 games (needs all 6 ROM sets processed).

## Files

- `platformio.ini` — build config, targets RP2350B (olimex_pico2xl)
- `src/config.h` — pin mapping (replaces upstream's ESP32 config)
- `src/video.h` / `src/video.cpp` — TFT_eSPI backend (replaces upstream)
- `src/*.h`, `src/emulation.c`, `src/i8048.c`, `src/galagino.cpp` — upstream source (unchanged)
- `upstream/` — git clone of harbaum/galagino for reference

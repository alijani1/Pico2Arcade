/* invaders.h — Space Invaders (Midway/Taito 1978).
 *
 * Ported from GalaginoPlus (VirtualClaudioBoy, GPLv3), machines/spaceinvaders,
 * reshaped into upstream Galagino's per-game C layout.
 *
 * Intel 8080 @ 2 MHz, run on the Z80 core (the game doesn't depend on the
 * few flag differences). No tiles or sprites: a 256x224 1bpp bitmap at
 * 0x2400-0x3FFF, shown rotated (224 wide x 256 tall) with the cabinet's
 * coloured cellophane overlay approximated by fixed bands.
 *
 * Memory map (14 address bits, 0x4000-0x7FFF mirrors):
 *   0x0000-0x1FFF  ROM  invaders.h/g/f/e (4 x 2 KB)
 *   0x2000-0x23FF  work RAM          -> memory[0x0000..]
 *   0x2400-0x3FFF  video RAM         -> memory[0x0400..]
 * Ports:
 *   IN 1  coin, starts, P1 fire/left/right     OUT 2  shift amount
 *   IN 2  DIPs, P2 controls                    OUT 3  sounds 1 (UFO, shot, flash, inv. hit)
 *   IN 3  shift register result                OUT 4  shift data
 *                                              OUT 5  sounds 2 (fleet 1-4, UFO hit)
 *                                              OUT 6  watchdog
 */

#ifdef __cplusplus
extern "C" {
#endif
extern unsigned char *invaders_rom;          /* 8 KB, invaders_runtime.cpp */
extern unsigned char  invaders_snd[2];       /* OUT 3 / OUT 5 latches, emulation.c */
#ifdef __cplusplus
}
#endif

#define INVADERS_RAM_BASE   0x2000
#define INVADERS_VRAM_OFS   0x0400           /* 0x2400 in memory[] */

static inline unsigned char invaders_fetch(unsigned short Addr) {
  Addr &= 0x3FFF;
  return (Addr < 0x2000) ? invaders_rom[Addr] : memory[Addr - INVADERS_RAM_BASE];
}

#ifdef CPU_EMULATION

static unsigned short invaders_shift_data = 0;
static unsigned char  invaders_shift_amount = 0;

static inline unsigned char invaders_RdZ80(unsigned short Addr) {
  return invaders_fetch(Addr);
}

static inline void invaders_WrZ80(unsigned short Addr, unsigned char Value) {
  Addr &= 0x3FFF;
  if(Addr < 0x2000) return;                  /* ROM */
  memory[Addr - INVADERS_RAM_BASE] = Value;

  /* first thing drawn into video RAM: the game is up, switch to 60 Hz pacing */
  if(!game_started && Addr >= 0x2400 && Value) game_started = 1;
}

static inline unsigned char invaders_InZ80(unsigned short Port) {
  switch(Port & 0xff) {
    case 0:
      return 0x0e;

    case 1: {                                /* coin, starts, P1 controls */
      unsigned char keymask = buttons_get();
      unsigned char retval = 0x08;           /* bit 3 always set */
      if(keymask & BUTTON_COIN)  retval |= 0x01;
      if(keymask & BUTTON_START) retval |= 0x04;   /* P1 start */
      if(keymask & BUTTON_FIRE)  retval |= 0x10;
      if(keymask & BUTTON_LEFT)  retval |= 0x20;
      if(keymask & BUTTON_RIGHT) retval |= 0x40;
      return retval;
    }

    case 2: {                                /* DIPs + P2 controls (same stick) */
      unsigned char keymask = buttons_get();
      unsigned char retval = INVADERS_DIP;
      if(keymask & BUTTON_FIRE)  retval |= 0x10;
      if(keymask & BUTTON_LEFT)  retval |= 0x20;
      if(keymask & BUTTON_RIGHT) retval |= 0x40;
      return retval;
    }

    case 3:                                  /* MB14241 shifter */
      return (unsigned char)(invaders_shift_data >> (8 - invaders_shift_amount));
  }
  return 0x00;
}

static inline void invaders_OutZ80(unsigned short Port, unsigned char Value) {
  switch(Port & 0xff) {
    case 2: invaders_shift_amount = Value & 0x07; break;
    case 3: invaders_snd[0] = Value; break;
    case 4: invaders_shift_data = (invaders_shift_data >> 8) | ((unsigned short)Value << 8); break;
    case 5: invaders_snd[1] = Value; break;
    /* 6: watchdog */
  }
}

static inline void invaders_run_frame(void) {
  current_cpu = 0;
  /* ~3300 instructions a frame; RST 08 at mid-screen, RST 10 at vblank */
  for(int i = 0; i < 832; i++) {
    StepZ80(&cpu[0]); StepZ80(&cpu[0]); StepZ80(&cpu[0]); StepZ80(&cpu[0]);
    if(i == 415) IntZ80(&cpu[0], INT_RST08);
  }
  IntZ80(&cpu[0], INT_RST10);
}

#endif // CPU_EMULATION

#ifdef IO_EMULATION

#include "invaders_samples.h"

static inline void invaders_prepare_frame(void) {
  active_sprites = 0;
}

/* Overlay band colour for picture line y (0..255, top = score). All band
 * edges are multiples of 8, so one colour per tile row. Byte-swapped RGB565. */
static inline unsigned short invaders_row_color(int y) {
  if(y < 32)  return 0xffff;                 /* score: white */
  if(y < 64)  return 0x00f8;                 /* UFO: red */
  if(y < 184) return 0xffff;                 /* invaders: white */
  if(y < 240) return 0xe007;                 /* player + shields: green */
  return 0xffff;                             /* lives / credit: white */
}

/* The 256 picture lines occupy tile rows 2..33 (16 px black above/below).
 * Picture (px, py) = bitmap (row px, column 255-py). The 8 lines of a tile
 * row are 8 consecutive bitmap columns, i.e. the bits of ONE video RAM byte
 * per px: bit 7 is the top line. */
static inline void invaders_render_row(short row) {
  if(row < 2 || row > 33) return;
  int base_y = (row - 2) * 8;
  const unsigned char *vram = memory + INVADERS_VRAM_OFS + ((255 - base_y) >> 3);
  unsigned short color = invaders_row_color(base_y);

  for(int px = 0; px < 224; px++) {
    unsigned char b = vram[px * 32];
    if(!b) continue;
    unsigned short *dst = frame_buffer + px;
    for(int sr = 0; sr < 8; sr++, dst += 224)
      if(b & (0x80 >> sr)) *dst = color;
  }
}

/* ---- sound: discrete circuits approximated (after MAME mw8080bw_a.cpp),
 * shot and invader-hit from short samples. From GalaginoPlus audio.cpp. ---- */
static unsigned long invaders_noise_rng = 0x1ffff;
static int invaders_noise_clock = 0, invaders_noise_out = 0;
static unsigned long invaders_ufo_sweep = 0, invaders_ufo_cnt = 0;
static int invaders_ufo_toggle = 1;
static int invaders_shot_pos = 0, invaders_shot_playing = 0;
static unsigned long invaders_explo_cnt = 0;
static int invaders_explo_env = 0;
static int invaders_invhit_pos = 0, invaders_invhit_playing = 0;
static unsigned long invaders_fleet_cnt = 0;
static int invaders_fleet_toggle = 1;
static unsigned long invaders_ufohit_cnt = 0, invaders_ufohit_warble = 0;
static int invaders_ufohit_toggle = 1, invaders_ufohit_freq = 0;
static int invaders_fleet_f = 0;

/* once per 64-sample buffer */
static inline void invaders_sound_regs(void) {
  static const int fleet_freq[4] = { 37, 55, 48, 41 };
  unsigned char p5 = invaders_snd[1];
  invaders_fleet_f = 0;
  for(int b = 3; b >= 0; b--)
    if(p5 & (1 << b)) { invaders_fleet_f = fleet_freq[b]; break; }
}

/* one 24 kHz sample, +/- 500 */
static inline short invaders_sound_sample(void) {
  unsigned char p3 = invaders_snd[0];
  unsigned char p5 = invaders_snd[1];
  int value = 0;

  /* 17-bit noise LFSR, taps 4+16, clocked at 7515 Hz */
  invaders_noise_clock += 7515;
  while(invaders_noise_clock >= 24000) {
    invaders_noise_clock -= 24000;
    int bit = ((invaders_noise_rng >> 4) ^ (invaders_noise_rng >> 16)) & 1;
    invaders_noise_rng = ((invaders_noise_rng << 1) | bit) & 0x1ffff;
    invaders_noise_out = (invaders_noise_rng >> 12) & 1;
  }

  /* UFO: SN76477, ~5.3 Hz triangle sweeping a 1220-3700 Hz square */
  if(p3 & 0x01) {
    invaders_ufo_sweep = (invaders_ufo_sweep + 1) % 4528;
    int slf = (invaders_ufo_sweep < 2264) ? invaders_ufo_sweep : (4528 - invaders_ufo_sweep);
    invaders_ufo_cnt += 1220 + (int)((long)slf * 2480 / 2264);
    if(invaders_ufo_cnt >= 12000) { invaders_ufo_cnt -= 12000; invaders_ufo_toggle = -invaders_ufo_toggle; }
    value += invaders_ufo_toggle * 60;
  } else
    invaders_ufo_sweep = 0;

  /* shot: 12 kHz sample, each played twice */
  if(p3 & 0x02) {
    if(!invaders_shot_playing) { invaders_shot_playing = 1; invaders_shot_pos = 0; }
    if((invaders_shot_pos >> 1) < invaders_sample_shot_LEN)
      value += invaders_sample_shot[invaders_shot_pos++ >> 1] * 3;
  } else
    invaders_shot_playing = 0;

  /* player explosion: noise with a slow decay */
  if(p3 & 0x04) {
    if(invaders_explo_env == 0) invaders_explo_env = 120;
    value += (invaders_noise_out ? 1 : -1) * invaders_explo_env;
    if(++invaders_explo_cnt >= 50) {
      invaders_explo_cnt = 0;
      if(invaders_explo_env > 15) invaders_explo_env--;
    }
  } else {
    invaders_explo_env = 0;
    invaders_explo_cnt = 0;
  }

  /* invader hit: 12 kHz sample */
  if(p3 & 0x08) {
    if(!invaders_invhit_playing) { invaders_invhit_playing = 1; invaders_invhit_pos = 0; }
    if((invaders_invhit_pos >> 1) < invaders_sample_invhit_LEN)
      value += invaders_sample_invhit[invaders_invhit_pos++ >> 1] * 3;
  } else
    invaders_invhit_playing = 0;

  /* fleet march: low square while a fleet bit is set */
  if(invaders_fleet_f) {
    invaders_fleet_cnt += invaders_fleet_f;
    if(invaders_fleet_cnt >= 12000) { invaders_fleet_cnt -= 12000; invaders_fleet_toggle = -invaders_fleet_toggle; }
    value += invaders_fleet_toggle * 100;
  }

  /* UFO hit: ~2 kHz falling tone with a 15 Hz warble */
  if(p5 & 0x10) {
    if(invaders_ufohit_freq == 0) invaders_ufohit_freq = 2000;
    invaders_ufohit_warble = (invaders_ufohit_warble + 1) % 1600;
    int w = (invaders_ufohit_warble < 800) ? (int)invaders_ufohit_warble : (int)(1600 - invaders_ufohit_warble);
    int f = invaders_ufohit_freq + (w * 400 / 800 - 200);
    if(f < 100) f = 100;
    invaders_ufohit_cnt += f;
    if(invaders_ufohit_cnt >= 12000) { invaders_ufohit_cnt -= 12000; invaders_ufohit_toggle = -invaders_ufohit_toggle; }
    value += invaders_ufohit_toggle * 100;
    if(invaders_ufohit_freq > 300) invaders_ufohit_freq--;
  } else {
    invaders_ufohit_freq = 0;
    invaders_ufohit_warble = 0;
  }

  if(value >  500) value =  500;
  if(value < -500) value = -500;
  return (short)value;
}

#endif // IO_EMULATION

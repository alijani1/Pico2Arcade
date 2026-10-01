/* pooyan.h — Pooyan (Konami 1982).
 *
 * Ported from GalaginoPlus (VirtualClaudioBoy, GPLv3), machines/pooyan,
 * reshaped into upstream Galagino's per-game C layout. Time Pilot hardware
 * with 4bpp graphics: main Z80 @ 3.072 MHz, sound Z80 @ 1.79 MHz with two
 * AY-3-8910s (timeplt_audio). 256 tiles (pre-rotated for portrait, flip
 * bits per tile), 24 16x16 sprites kept in landscape and drawn transposed,
 * one copy (flipped while drawing) instead of GalaginoPlus's four.
 *
 * Main CPU                               memory[]
 *   0x0000-0x7FFF  ROM (4 x 8 KB)
 *   0x8000-0x83FF  colour RAM            0x0000
 *   0x8400-0x87FF  video RAM             0x0400
 *   0x8800-0x8FFF  work RAM              0x0800
 *   0x9000-0x90FF  sprite RAM bank 0     0x1000   (mirror 0x0B00)
 *   0x9400-0x94FF  sprite RAM bank 1     0x1100   (mirror 0x0B00)
 *   0xA000 r DSW1, 0xA080 IN0, 0xA0A0 IN1, 0xA0C0 IN2, 0xA0E0 DSW0
 *   0xA100 w sound latch, 0xA180-0xA187 w LS259 (Q0 NMI enable, Q1 sound IRQ)
 * Sound CPU
 *   0x0000-0x1FFF  ROM (2 x 4 KB)
 *   0x3000-0x33FF  RAM (mirrored to 0x3FFF)  0x1200
 *   0x4000 / 0x5000  AY#0 data / address (port A = latch, B = LS90 timer)
 *   0x6000 / 0x7000  AY#1 data / address
 *   0x8000+          RC filters (ignored)
 */

#ifdef __cplusplus
extern "C" {
#endif
extern unsigned char  *pooyan_rom_cpu1;
extern unsigned char  *pooyan_rom_cpu2;
extern unsigned long  (*pooyan_tiles)[8];          /* 256 x 8 rows, 8 nibbles, portrait */
extern unsigned char  (*pooyan_sprites)[16][16];   /* 64 x 16 x 16 pens, landscape */
extern unsigned short (*pooyan_char_colors)[16];   /* 16 groups x 16 pens */
extern unsigned short (*pooyan_sprite_colors)[16];
extern unsigned short *pooyan_sprite_transmask;    /* 16 groups: bit n = pen n transparent */
extern unsigned char  ay_env_restart[2];           /* galagino.cpp */
#ifdef __cplusplus
}
#endif

#define POOYAN_COLORRAM  0x0000
#define POOYAN_VIDEORAM  0x0400
#define POOYAN_WORKRAM   0x0800
#define POOYAN_SPRITES0  0x1000
#define POOYAN_SPRITES1  0x1100
#define POOYAN_SNDRAM    0x1200

#ifdef CPU_EMULATION

static unsigned char pooyan_nmi_enable = 0;
static unsigned char pooyan_snd_latch = 0;
static unsigned char pooyan_snd_irq = 0, pooyan_snd_irq_last = 0;
static unsigned char pooyan_ay_addr[2] = { 0, 0 };
static unsigned long pooyan_snd_icnt = 0;

static inline unsigned char pooyan_RdZ80(unsigned short Addr) {
  if(current_cpu == 1) {
    if(Addr < 0x2000) return pooyan_rom_cpu2[Addr];
    switch(Addr & 0xf000) {
      case 0x3000: return memory[POOYAN_SNDRAM + (Addr & 0x3ff)];
      case 0x4000: {                         /* AY#0 data */
        /* LS90 timer (MAME timeplt_timer, total_cycles / 512) */
        static const unsigned char timer[10] = { 0x00,0x10,0x20,0x30,0x40,0x90,0xa0,0xb0,0xa0,0xd0 };
        unsigned char r = pooyan_ay_addr[0];
        if(r == 14) return pooyan_snd_latch;
        if(r == 15) return timer[(pooyan_snd_icnt / 44) % 10];
        return soundregs[r];
      }
      case 0x6000: return (pooyan_ay_addr[1] < 14) ? soundregs[16 + pooyan_ay_addr[1]] : 0xff;
    }
    return 0xff;
  }

  if(Addr < 0x8000) return pooyan_rom_cpu1[Addr];
  if(Addr < 0x9000) return memory[Addr - 0x8000];          /* colour, video, work RAM */
  if((Addr & 0xf400) == 0x9000) return memory[POOYAN_SPRITES0 + (Addr & 0xff)];
  if((Addr & 0xf400) == 0x9400) return memory[POOYAN_SPRITES1 + (Addr & 0xff)];

  unsigned char keymask, retval;
  switch(Addr & 0xa1e0) {
    case 0xa080:                             /* IN0: coin, start */
      keymask = buttons_get(); retval = 0xff;
      if(keymask & BUTTON_COIN)  retval &= ~0x01;
      if(keymask & BUTTON_START) retval &= ~0x08;
      return retval;
    case 0xa0a0:                             /* IN1: P1 up / down / fire */
      keymask = buttons_get(); retval = 0xff;
      if(keymask & BUTTON_UP)   retval &= ~0x04;
      if(keymask & BUTTON_DOWN) retval &= ~0x08;
      if(keymask & BUTTON_FIRE) retval &= ~0x10;
      return retval;
    case 0xa0c0: return 0xff;                /* IN2: P2 */
    case 0xa0e0: return POOYAN_DSW0;
  }
  if((Addr & 0xa180) == 0xa000) return POOYAN_DSW1;
  return 0x00;
}

static inline void pooyan_WrZ80(unsigned short Addr, unsigned char Value) {
  if(current_cpu == 1) {
    int ay;
    switch(Addr & 0xf000) {
      case 0x3000: memory[POOYAN_SNDRAM + (Addr & 0x3ff)] = Value; return;
      case 0x5000: pooyan_ay_addr[0] = Value & 0x0f; return;
      case 0x7000: pooyan_ay_addr[1] = Value & 0x0f; return;
      case 0x4000: ay = 0; break;
      case 0x6000: ay = 1; break;
      default: return;                       /* filters */
    }
    unsigned char r = pooyan_ay_addr[ay];
    if(r < 14) {
      soundregs[16 * ay + r] = Value;
      if(r == 13) ay_env_restart[ay] = 1;
    }
    return;
  }

  if(Addr < 0x8000) return;
  if(Addr < 0x9000) {                        /* colour, video, work RAM */
    if(!game_started && Addr >= 0x8400 && Addr < 0x8800) game_started = 1;
    memory[Addr - 0x8000] = Value;
    return;
  }
  if((Addr & 0xf400) == 0x9000) { memory[POOYAN_SPRITES0 + (Addr & 0xff)] = Value; return; }
  if((Addr & 0xf400) == 0x9400) { memory[POOYAN_SPRITES1 + (Addr & 0xff)] = Value; return; }

  switch(Addr & 0xa180) {
    case 0xa180:                             /* LS259 */
      switch(Addr & 7) {
        case 0: pooyan_nmi_enable = Value & 1; break;
        case 1:                              /* sound IRQ on the rising edge */
          if((Value & 1) && !pooyan_snd_irq_last) pooyan_snd_irq = 1;
          pooyan_snd_irq_last = Value & 1;
          break;
        /* 2 mute, 3/4 coin counters, 7 flip screen: ignored (upright) */
      }
      return;
    case 0xa100: pooyan_snd_latch = Value; return;
    /* 0xa000: watchdog */
  }
}

static inline void pooyan_run_frame(void) {
  for(int i = 0; i < 1280; i++) {
    current_cpu = 0; StepZ80(&cpu[0]); StepZ80(&cpu[0]); StepZ80(&cpu[0]); StepZ80(&cpu[0]);
    current_cpu = 1; StepZ80(&cpu[1]); pooyan_snd_icnt++; StepZ80(&cpu[1]); pooyan_snd_icnt++;

    /* latched until the sound CPU has interrupts enabled (as Frogger) */
    if(pooyan_snd_irq && Z80_IFF1(&cpu[1])) {
      IntZ80(&cpu[1], INT_RST38);
      pooyan_snd_irq = 0;
    }
  }
  if(pooyan_nmi_enable) {
    current_cpu = 0;
    IntZ80(&cpu[0], INT_NMI);
  }
}

/* The game draws the HIGH SCORE digits at the top once, at boot, before a
 * saved table goes in: redraw them from the top score (BCD at 0x88A8-AA,
 * little endian). Digit cells 0x8641..0x85A1 (stride 0x20), chars 0-9,
 * 0x10 = blank, leading zeros blanked. (From GalaginoPlus, checked on HW.)
 * Called by pico_hiscore.cpp after it restores the table. */
void pooyan_hiscore_restored(void) {
  static const unsigned short cells[6] = { 0x641, 0x621, 0x601, 0x5e1, 0x5c1, 0x5a1 };
  unsigned char shown = 0;
  for(int i = 0; i < 6; i++) {
    unsigned char b = memory[0x0aa - i / 2 + 0x800];   /* 0x88AA - i/2 */
    unsigned char d = (i & 1) ? (b & 0x0f) : (b >> 4);
    if(d || i == 5) shown = 1;
    memory[cells[i]] = shown ? d : 0x10;
  }
}

#endif // CPU_EMULATION

#ifdef IO_EMULATION

static inline void pooyan_prepare_frame(void) {
  active_sprites = 0;
  const unsigned char *bank0 = memory + POOYAN_SPRITES0;
  const unsigned char *bank1 = memory + POOYAN_SPRITES1;

  /* drawn in MAME's order (ascending offsets: later ones on top) */
  for(int offs = 0x10; offs <= 0x3e; offs += 2) {
    unsigned char sx_raw = bank0[offs];
    unsigned char sy_raw = bank1[offs + 1];
    unsigned char attr   = bank1[offs];
    int sy = 240 - sy_raw;
    if(sy <= -16 || sy >= 256) continue;

    struct sprite_S spr;
    spr.x = (int)sy_raw - 16;
    spr.y = (int)sx_raw + 16;
    spr.code  = bank0[offs + 1] & 0x3f;
    spr.color = attr & 0x0f;
    /* bit 0: landscape flip Y (bit 7), bit 1: landscape flip X (bit 6, inverted) */
    spr.flags = ((attr >> 7) & 1) | (((~attr >> 6) & 1) << 1);
    if((spr.y > -16) && (spr.y < 304) && (spr.x > -16) && (spr.x < 224))
      sprite[active_sprites++] = spr;
  }
}

static inline void pooyan_blit_tile(short row, char col) {
  unsigned short addr = tileaddr[row][col];
  unsigned char cram = memory[POOYAN_COLORRAM + addr];
  const unsigned long *tile = pooyan_tiles[memory[POOYAN_VIDEORAM + addr]];
  const unsigned short *colors = pooyan_char_colors[cram & 0x0f];
  /* under the 90 deg rotation the axes swap: bit 7 flips across, bit 6 down */
  unsigned char flip_x = (cram >> 7) & 1;
  unsigned char flip_y = (cram >> 6) & 1;

  unsigned short *ptr = frame_buffer + 8 * col;
  for(char r = 0; r < 8; r++, ptr += 224) {
    unsigned long pix = tile[flip_y ? 7 - r : r];
    if(flip_x)
      for(char c = 0; c < 8; c++) ptr[c] = colors[(pix >> ((7 - c) * 4)) & 0x0f];
    else
      for(char c = 0; c < 8; c++, pix >>= 4) ptr[c] = colors[pix & 0x0f];
  }
}

/* Landscape sprite drawn transposed: sprite row r -> screen x (reversed),
 * sprite column c -> screen y. Transparent pens per colour group. */
static inline void pooyan_blit_sprite(short row, unsigned char s) {
  const unsigned char (*spr)[16] = pooyan_sprites[sprite[s].code];
  const unsigned short *colors = pooyan_sprite_colors[sprite[s].color];
  unsigned short transmask = pooyan_sprite_transmask[sprite[s].color];
  unsigned char fy = sprite[s].flags & 1, fx = sprite[s].flags & 2;

  int top = 8 * row;
  int y0 = sprite[s].y > top ? sprite[s].y : top;
  int y1 = (sprite[s].y + 15) < (top + 7) ? (sprite[s].y + 15) : (top + 7);
  if(y0 > y1) return;

  for(int r = 0; r < 16; r++) {
    int x = sprite[s].x + 15 - r;
    if(x < 0 || x >= 224) continue;
    const unsigned char *line = spr[fy ? 15 - r : r];
    for(int y = y0; y <= y1; y++) {
      int c = y - sprite[s].y;
      unsigned char px = line[fx ? 15 - c : c];
      if(!((transmask >> px) & 1)) frame_buffer[(y - top) * 224 + x] = colors[px];
    }
  }
}

static inline void pooyan_render_row(short row) {
  if(row <= 1 || row >= 34) return;
  for(char col = 0; col < 28; col++) pooyan_blit_tile(row, col);
  for(unsigned char s = 0; s < active_sprites; s++)
    if((sprite[s].y < 8 * (row + 1)) && ((sprite[s].y + 16) > 8 * row))
      pooyan_blit_sprite(row, s);
}

#endif // IO_EMULATION

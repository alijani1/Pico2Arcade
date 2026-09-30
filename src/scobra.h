/* scobra.h — Super Cobra (Konami/Stern 1981).
 *
 * Ported from GalaginoPlus (VirtualClaudioBoy, GPLv3), machines/scramble +
 * machines/supercobra, reshaped into upstream Galagino's per-game C layout.
 * Scramble-era Konami hardware, the same family as Frogger: main Z80 @
 * 3.072 MHz, sound Z80 @ 1.79 MHz with two AY-3-8910s, Galaxian-style video
 * (per-column scroll, 8 sprites, 8 bullets, starfield, blue background).
 * The memory map follows MAME (galaxian.cpp, scobra); GalaginoPlus's
 * sprite-RAM read and its audio/RAM2 overlap bugs aren't carried over.
 *
 * Main CPU                               memory[]
 *   0x0000-0x5FFF  ROM (6 x 4 KB)
 *   0x8000-0x87FF  RAM                   0x0000
 *   0x8800-0x8BFF  video RAM (+mirror)   0x0800
 *   0x9000-0x90FF  object RAM (+mirror)  0x0C00  attr/scroll 00-3F,
 *                                                sprites 40-5F, bullets 60-7F
 *   0x9800-0x9803  PPI0: IN0, IN1, IN2
 *   0xA000-0xA003  PPI1: sound latch, sound control, protection
 *   0xA801/3/4     NMI enable, blue background, stars
 *   0xB000         watchdog
 * Sound CPU
 *   0x0000-0x17FF  ROM (3 x 2 KB)
 *   0x8000-0x83FF  RAM (mirrored to 0x8FFF)  0x1000
 *   0x9000-0x9FFF  RC filters (ignored)
 *   ports 0x10/0x20 AY#1 addr/data, 0x40/0x80 AY#0 addr/data
 *   (AY#0 port A = sound latch, port B = LS90 timer)
 */

#ifdef __cplusplus
extern "C" {
#endif
extern unsigned char  *scobra_rom_cpu1;
extern unsigned char  *scobra_rom_cpu2;
extern unsigned short (*scobra_tilemap)[8];
extern unsigned long  (*scobra_sprites)[64][16];
extern unsigned short (*scobra_colormap)[4];
extern unsigned char  scobra_background, scobra_stars_on;   /* emulation.c */
extern unsigned char  ay_env_restart[2];                    /* galagino.cpp */
#ifdef __cplusplus
}
#endif

#define SCOBRA_VRAM  0x0800
#define SCOBRA_OBJ   0x0c00
#define SCOBRA_SRAM  0x1000

#ifdef CPU_EMULATION

static unsigned char  scobra_snd_latch = 0;
static unsigned char  scobra_snd_ctrl = 0;
static unsigned char  scobra_snd_irq = 0;
static unsigned char  scobra_ay_addr[2] = { 0, 0 };
static unsigned long  scobra_snd_icnt = 0;
static unsigned short scobra_prot_state = 0;
static unsigned char  scobra_prot_result = 0;

static inline unsigned char scobra_ppi0_r(unsigned char port) {
  unsigned char keymask = buttons_get();
  unsigned char retval;
  switch(port) {
    case 0:   /* IN0: UP(c), BOMB, SERVICE, LASER, RIGHT, LEFT, COIN2, COIN1 */
      retval = 0xff;
      if(keymask & BUTTON_COIN)  retval &= ~0x80;
      if(keymask & BUTTON_LEFT)  retval &= ~0x20;
      if(keymask & BUTTON_RIGHT) retval &= ~0x10;
      if(keymask & BUTTON_FIRE)  retval &= ~(0x08 | 0x02);   /* laser + bomb */
      return retval;
    case 1:   /* IN1: DIPs (continue, lives), cocktail, START2, START1 */
      retval = SCOBRA_DIP_IN1;
      if(keymask & BUTTON_START) retval &= ~0x80;
      return retval;
    case 2:   /* IN2: DOWN(c), coinage, cabinet, UP, prot, DOWN, prot */
      retval = (scobra_prot_result & 0x80) ? (0xf1 | SCOBRA_DIP_IN2) : (0x51 | SCOBRA_DIP_IN2);
      if(keymask & BUTTON_DOWN) retval &= ~0x40;
      if(keymask & BUTTON_UP)   retval &= ~0x10;
      return retval;
  }
  return 0xff;
}

static inline void scobra_protection_w(unsigned char Value) {
  scobra_prot_state = (scobra_prot_state << 4) | (Value & 0x0f);
  int num1 = (scobra_prot_state >> 8) & 0x0f;
  int num2 = (scobra_prot_state >> 4) & 0x0f;
  switch(scobra_prot_state & 0x0f) {
    case 0x6: scobra_prot_result ^= 0x80; break;
    case 0x9: scobra_prot_result = ((num1 + 1 < 0xf) ? num1 + 1 : 0xf) << 4; break;
    case 0xb: scobra_prot_result = ((num2 > num1) ? num2 - num1 : 0) << 4; break;
    case 0xa: scobra_prot_result = 0x00; break;
    case 0xf: scobra_prot_result = ((num1 > num2) ? num1 - num2 : 0) << 4; break;
  }
}

static inline unsigned char scobra_RdZ80(unsigned short Addr) {
  if(current_cpu == 0) {
    if(Addr < 0x8000) return scobra_rom_cpu1[Addr];
    switch(Addr & 0xf800) {
      case 0x8000: return memory[Addr - 0x8000];
      case 0x8800: return memory[SCOBRA_VRAM + (Addr & 0x3ff)];
      case 0x9000: return memory[SCOBRA_OBJ + (Addr & 0xff)];
      case 0x9800: return scobra_ppi0_r(Addr & 3);
      case 0xa000: return ((Addr & 3) == 2) ? scobra_prot_result : 0xff;
    }
    return 0xff;                             /* watchdog etc. */
  }
  if(Addr < 0x2000) return scobra_rom_cpu2[Addr];
  if((Addr & 0xf000) == 0x8000) return memory[SCOBRA_SRAM + (Addr & 0x3ff)];
  return 0xff;
}

static inline void scobra_WrZ80(unsigned short Addr, unsigned char Value) {
  if(current_cpu == 0) {
    switch(Addr & 0xf800) {
      case 0x8000: memory[Addr - 0x8000] = Value; return;
      case 0x8800: memory[SCOBRA_VRAM + (Addr & 0x3ff)] = Value; return;
      case 0x9000: memory[SCOBRA_OBJ + (Addr & 0xff)] = Value; return;
      case 0xa000:                           /* PPI1 */
        switch(Addr & 3) {
          case 0: scobra_snd_latch = Value; break;
          case 1:                            /* bit 3 falling edge: sound IRQ */
            if((scobra_snd_ctrl & 0x08) && !(Value & 0x08)) scobra_snd_irq = 1;
            scobra_snd_ctrl = Value;
            break;
          case 2: scobra_protection_w(Value); break;
        }
        return;
      case 0xa800:
        switch(Addr & 7) {
          case 1: irq_enable[0] = Value & 1; break;
          case 3: scobra_background = Value & 1; game_started = 1; break;
          case 4: scobra_stars_on = Value & 1; break;
        }
        return;
    }
    return;
  }
  if((Addr & 0xf000) == 0x8000) memory[SCOBRA_SRAM + (Addr & 0x3ff)] = Value;
}

/* sound CPU I/O: AY#1 at 0x10 (addr) / 0x20 (data), AY#0 at 0x40 / 0x80 */
static inline void scobra_OutZ80(unsigned short Port, unsigned char Value) {
  if(current_cpu != 1) return;
  int ay;
  switch(Port & 0xff) {
    case 0x10: scobra_ay_addr[1] = Value & 0x0f; return;
    case 0x40: scobra_ay_addr[0] = Value & 0x0f; return;
    case 0x20: ay = 1; break;
    case 0x80: ay = 0; break;
    default: return;
  }
  unsigned char r = scobra_ay_addr[ay];
  if(r < 14) {
    soundregs[16 * ay + r] = Value;
    if(r == 13) ay_env_restart[ay] = 1;
  }
}

static inline unsigned char scobra_InZ80(unsigned short Port) {
  /* LS90 timer on AY#0 port B (old MAME scramble_portB_r) */
  static const unsigned char timer[10] = { 0x00,0x10,0x20,0x30,0x40,0x90,0xa0,0xb0,0xa0,0xd0 };
  if(current_cpu != 1) return 0xff;
  int ay;
  switch(Port & 0xff) {
    case 0x20: ay = 1; break;
    case 0x80: ay = 0; break;
    default: return 0xff;
  }
  unsigned char r = scobra_ay_addr[ay];
  if(r < 14) return soundregs[16 * ay + r];
  if(ay == 0 && r == 14) return scobra_snd_latch;
  if(ay == 0 && r == 15) return timer[(scobra_snd_icnt / 40) % 10];
  return 0xff;
}

static inline void scobra_run_frame(void) {
  for(int i = 0; i < INST_PER_FRAME; i++) {
    current_cpu = 0; StepZ80(&cpu[0]); StepZ80(&cpu[0]); StepZ80(&cpu[0]); StepZ80(&cpu[0]);
    current_cpu = 1; StepZ80(&cpu[1]); scobra_snd_icnt++; StepZ80(&cpu[1]); scobra_snd_icnt++;

    /* latched until the sound CPU has interrupts enabled (as Frogger) */
    if(scobra_snd_irq && Z80_IFF1(&cpu[1])) {
      IntZ80(&cpu[1], INT_RST38);
      scobra_snd_irq = 0;
    }
  }
  if(irq_enable[0]) {
    current_cpu = 0;
    IntZ80(&cpu[0], INT_NMI);
  }
}

#endif // CPU_EMULATION

#ifdef IO_EMULATION

/* bullets: 7 enemy shells + the player's, single pixels */
static short scobra_bullet_x[8], scobra_bullet_y[8];
static unsigned char scobra_bullets = 0;

/* starfield: MAME's 17-bit LFSR, fixed positions, computed once at load */
#define SCOBRA_MAX_STARS 256
static struct { unsigned char x; unsigned short y; unsigned short color; } scobra_star[SCOBRA_MAX_STARS];
static int scobra_star_count = 0;
static int scobra_star_frames = 0, scobra_star_step = 2;

extern "C" void scobra_stars_init(void) {
  static const unsigned char level[4] = { 0, 150, 200, 255 };
  scobra_star_count = 0;
  uint32_t shiftreg = 0;
  for(int i = 0; i < 131071 && scobra_star_count < SCOBRA_MAX_STARS; i++) {
    if((shiftreg & 0x1fe01) == 0x1fe00) {
      int ci = (~shiftreg >> 3) & 0x3f;
      int x = (i % 512) / 2, y = i / 512;
      if((y ^ (x >> 3)) & 1) {
        int gx = 255 - y, gy = x + 16;
        if(gx >= 0 && gx < 224 && gy >= 16 && gy < 288) {
          unsigned char r = level[((ci >> 4) & 2) | ((ci >> 5) & 1)];
          unsigned char g = level[((ci >> 2) & 2) | ((ci >> 3) & 1)];
          unsigned char b = level[((ci >> 0) & 2) | ((ci >> 1) & 1)];
          unsigned short c = ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
          scobra_star[scobra_star_count].x = gx;
          scobra_star[scobra_star_count].y = gy;
          scobra_star[scobra_star_count].color = (c >> 8) | (c << 8);
          scobra_star_count++;
        }
      }
    }
    shiftreg = (shiftreg >> 1) | ((((shiftreg >> 12) ^ ~shiftreg) & 1) << 16);
  }
}

static inline void scobra_prepare_frame(void) {
  active_sprites = 0;
  for(int idx = 7; idx >= 0; idx--) {
    unsigned char *base = memory + SCOBRA_OBJ + 0x40 + 4 * idx;
    struct sprite_S spr;
    spr.code  = base[1] & 0x3f;
    spr.flags = (base[1] >> 6) & 3;          /* x/y flip -> variant index */
    spr.color = base[2] & 7;
    spr.x = base[0] - 16;
    spr.y = base[3] + 16;
    if(base[3] && (spr.y > -16) && (spr.y < 288) && (spr.x > -16) && (spr.x < 224))
      sprite[active_sprites++] = spr;
  }

  scobra_bullets = 0;
  for(int idx = 0; idx < 8; idx++) {
    unsigned char *b = memory + SCOBRA_OBJ + 0x60 + 4 * idx;
    scobra_bullet_x[idx] = b[1] - 16;
    scobra_bullet_y[idx] = 264 - b[3];
    if(b[1] && b[3] && scobra_bullet_x[idx] >= 0 && scobra_bullet_x[idx] < 224 &&
       scobra_bullet_y[idx] >= 0 && scobra_bullet_y[idx] < 288)
      scobra_bullets |= 1 << idx;
  }

  /* star twinkle: a different subset every second */
  if(++scobra_star_frames >= 30) {           /* prepare_frame runs at 30 Hz */
    scobra_star_frames = 0;
    scobra_star_step = (scobra_star_step >= 5) ? 2 : scobra_star_step + 1;
  }
}

static inline void scobra_blit_tile_scroll(short row, signed char col, unsigned char scroll) {
  unsigned short addr;
  unsigned short mask = 0xffff;
  int sub = scroll & 7;
  if(col >= 0) {
    addr = (tileaddr[row][col] + ((scroll & ~7) << 2)) & 1023;
    if(sub && col == 27) mask = 0xffff >> (2 * sub);
  } else {
    /* leftmost tile, only partly visible */
    addr = (tileaddr[row][0] + 32 + ((scroll & ~7) << 2)) & 1023;
    mask = 0xffff << (2 * (8 - sub));
  }

  const unsigned short *tile = scobra_tilemap[memory[SCOBRA_VRAM + addr]];
  const unsigned short *colors = scobra_colormap[memory[SCOBRA_OBJ + 2 * (addr & 31) + 1] & 7];
  unsigned short *ptr = frame_buffer + 8 * col + sub;

  for(char r = 0; r < 8; r++, ptr += 224 - 8) {
    unsigned short pix = *tile++ & mask;
    for(char c = 0; c < 8; c++, pix >>= 2) {
      if(pix & 3) *ptr = colors[pix & 3];
      ptr++;
    }
  }
}

static inline void scobra_blit_sprite(short row, unsigned char s) {
  const unsigned long *spr = scobra_sprites[sprite[s].flags & 3][sprite[s].code];
  const unsigned short *colors = scobra_colormap[sprite[s].color];

  unsigned long mask = 0xffffffff;
  if(sprite[s].x < 0)        mask <<= -2 * sprite[s].x;
  if(sprite[s].x > 224 - 16) mask >>= 2 * (sprite[s].x - (224 - 16));

  short y_offset = sprite[s].y - 8 * row;
  unsigned char lines2draw = 8;
  if(y_offset < -8) lines2draw = 16 + y_offset;
  unsigned short startline = 0;
  if(y_offset > 0) { startline = y_offset; lines2draw = 8 - y_offset; }
  if(y_offset < 0) spr -= y_offset;

  unsigned short *ptr = frame_buffer + sprite[s].x + 224 * startline;
  for(char r = 0; r < lines2draw; r++, ptr += 224 - 16) {
    unsigned long pix = *spr++ & mask;
    for(char c = 0; c < 16; c++, pix >>= 2) {
      if(pix & 3) *ptr = colors[pix & 3];
      ptr++;
    }
  }
}

static inline void scobra_render_row(short row) {
  if(row <= 1 || row >= 34) return;

  if(scobra_background) memset(frame_buffer, 8, 2 * 224 * 8);   /* dark blue */

  if(scobra_stars_on) {
    short top = 8 * row;
    for(int i = 0; i < scobra_star_count; i += scobra_star_step) {
      short y = scobra_star[i].y - top;
      if(y >= 0 && y < 8) frame_buffer[224 * y + scobra_star[i].x] = scobra_star[i].color;
    }
  }

  /* per-column scroll of the landscape screen = per tile row here */
  unsigned char scroll = memory[SCOBRA_OBJ + 2 * (row - 2)];
  if(scroll & 7) scobra_blit_tile_scroll(row, -1, scroll);
  for(char col = 0; col < 28; col++) scobra_blit_tile_scroll(row, col, scroll);

  for(unsigned char s = 0; s < active_sprites; s++)
    if((sprite[s].y < 8 * (row + 1)) && ((sprite[s].y + 16) > 8 * row))
      scobra_blit_sprite(row, s);

  if(scobra_bullets) {
    short top = 8 * row;
    for(int b = 0; b < 8; b++) {
      if(!(scobra_bullets & (1 << b))) continue;
      short y = scobra_bullet_y[b] - top;
      if(y >= 0 && y < 8) frame_buffer[224 * y + scobra_bullet_x[b]] = 0xe0ff;   /* yellow */
    }
  }
}

#endif // IO_EMULATION

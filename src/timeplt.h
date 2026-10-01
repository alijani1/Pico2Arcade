/* timeplt.h — Time Pilot (Konami 1982).
 *
 * Ported from GalaginoPlus (VirtualClaudioBoy, GPLv3), machines/timeplt,
 * reshaped into upstream Galagino's per-game C layout. Same board family as
 * Pooyan: main Z80 @ 3.072 MHz, sound Z80 @ 1.79 MHz with two AY-3-8910s
 * (timeplt_audio). 2bpp graphics: 512 tiles (pre-rotated for portrait,
 * per-tile flips and priority), 256 16x16 sprites kept in landscape and
 * drawn transposed (one copy, flipped while drawing). The game shows more
 * sprites than the hardware holds by rewriting sprite RAM mid-frame, timed
 * by the scanline counter at 0xC000: the sprite RAM is captured when the
 * game first reads it and both sets are drawn (as GalaginoPlus).
 *
 * Main CPU                               memory[]
 *   0x0000-0x5FFF  ROM (3 x 8 KB)
 *   0xA000-0xA3FF  colour RAM            0x0000
 *   0xA400-0xA7FF  video RAM             0x0400
 *   0xA800-0xAFFF  work RAM              0x0800
 *   0xB000-0xB0FF  sprite RAM bank 0     0x1000   (mirrored to 0xB3FF)
 *   0xB400-0xB4FF  sprite RAM bank 1     0x1100   (mirrored to 0xB7FF)
 *   0xC000 r scanline, w sound latch; 0xC200 r DSW1, w watchdog
 *   0xC300 r IN0, 0xC320 IN1, 0xC340 IN2, 0xC360 DSW0
 *   0xC300-0xC30F w LS259 (A1-A3: Q0 NMI enable, Q2 sound IRQ, Q4 video enable)
 * Sound CPU: as Pooyan (ROM 0x0000-0x0FFF, RAM 0x3000, AYs at 0x4000-0x7000)
 */

#ifdef __cplusplus
extern "C" {
#endif
extern unsigned char  *timeplt_rom_cpu1;
extern unsigned char  *timeplt_rom_cpu2;
extern unsigned short (*timeplt_tiles)[8];          /* 512 x 8 rows, 2bpp, portrait */
extern unsigned long  (*timeplt_sprites)[16];       /* 256 x 16 rows, 2bpp, landscape */
extern unsigned short (*timeplt_char_colors)[4];    /* 32 groups x 4 */
extern unsigned short (*timeplt_sprite_colors)[4];  /* 64 groups x 4 */
extern unsigned char  ay_env_restart[2];            /* galagino.cpp */
extern unsigned char  timeplt_video_enable;         /* emulation.c */
extern unsigned char  timeplt_mux_bank0[0x100], timeplt_mux_bank1[0x100];
#ifdef __cplusplus
}
#endif

#define TIMEPLT_COLORRAM  0x0000
#define TIMEPLT_VIDEORAM  0x0400
#define TIMEPLT_WORKRAM   0x0800
#define TIMEPLT_SPRITES0  0x1000
#define TIMEPLT_SPRITES1  0x1100
#define TIMEPLT_SNDRAM    0x1200

#ifdef CPU_EMULATION

static unsigned char timeplt_nmi_enable = 0;
static unsigned char timeplt_snd_latch = 0;
static unsigned char timeplt_snd_irq = 0, timeplt_snd_irq_last = 0;
static unsigned char timeplt_ay_addr[2] = { 0, 0 };
static unsigned long timeplt_snd_icnt = 0;
static unsigned char timeplt_scanline = 0;
static unsigned char timeplt_mux_used = 0;

static inline unsigned char timeplt_RdZ80(unsigned short Addr) {
  if(current_cpu == 1) {
    if(Addr < 0x1000) return timeplt_rom_cpu2[Addr];
    switch(Addr & 0xf000) {
      case 0x3000: return memory[TIMEPLT_SNDRAM + (Addr & 0x3ff)];
      case 0x4000: {                         /* AY#0 data */
        /* LS90 timer (MAME timeplt_timer, total_cycles / 512) */
        static const unsigned char timer[10] = { 0x00,0x10,0x20,0x30,0x40,0x90,0xa0,0xb0,0xa0,0xd0 };
        unsigned char r = timeplt_ay_addr[0];
        if(r == 14) return timeplt_snd_latch;
        if(r == 15) return timer[(timeplt_snd_icnt / 45) % 10];
        return soundregs[r];
      }
      case 0x6000: return (timeplt_ay_addr[1] < 14) ? soundregs[16 + timeplt_ay_addr[1]] : 0xff;
    }
    return 0xff;
  }

  if(Addr < 0x6000) return timeplt_rom_cpu1[Addr];
  if((Addr & 0xf000) == 0xa000) return memory[Addr - 0xa000];   /* colour, video, work RAM */
  if((Addr & 0xfc00) == 0xb000) return memory[TIMEPLT_SPRITES0 + (Addr & 0xff)];
  if((Addr & 0xfc00) == 0xb400) return memory[TIMEPLT_SPRITES1 + (Addr & 0xff)];

  unsigned char keymask, retval;
  switch(Addr) {
    case 0xc000: timeplt_mux_used = 1; return timeplt_scanline;
    case 0xc200: return TIMEPLT_DSW1;
    case 0xc300:                             /* IN0: coin, start */
      keymask = buttons_get(); retval = 0xff;
      if(keymask & BUTTON_COIN)  retval &= ~0x01;
      if(keymask & BUTTON_START) retval &= ~0x08;
      return retval;
    case 0xc320:                             /* IN1: P1 stick + fire */
      keymask = buttons_get(); retval = 0xff;
      if(keymask & BUTTON_LEFT)  retval &= ~0x01;
      if(keymask & BUTTON_RIGHT) retval &= ~0x02;
      if(keymask & BUTTON_UP)    retval &= ~0x04;
      if(keymask & BUTTON_DOWN)  retval &= ~0x08;
      if(keymask & BUTTON_FIRE)  retval &= ~0x10;
      return retval;
    case 0xc340: return 0xff;                /* IN2: P2 */
    case 0xc360: return TIMEPLT_DSW0;
  }
  return 0x00;
}

static inline void timeplt_WrZ80(unsigned short Addr, unsigned char Value) {
  if(current_cpu == 1) {
    int ay;
    switch(Addr & 0xf000) {
      case 0x3000: memory[TIMEPLT_SNDRAM + (Addr & 0x3ff)] = Value; return;
      case 0x5000: timeplt_ay_addr[0] = Value & 0x0f; return;
      case 0x7000: timeplt_ay_addr[1] = Value & 0x0f; return;
      case 0x4000: ay = 0; break;
      case 0x6000: ay = 1; break;
      default: return;                       /* filters */
    }
    unsigned char r = timeplt_ay_addr[ay];
    if(r < 14) {
      soundregs[16 * ay + r] = Value;
      if(r == 13) ay_env_restart[ay] = 1;
    }
    return;
  }

  if((Addr & 0xf000) == 0xa000) {            /* colour, video, work RAM */
    if(!game_started && Addr >= 0xa400 && Addr < 0xa800) game_started = 1;
    memory[Addr - 0xa000] = Value;
    return;
  }
  if((Addr & 0xfc00) == 0xb000) { memory[TIMEPLT_SPRITES0 + (Addr & 0xff)] = Value; return; }
  if((Addr & 0xfc00) == 0xb400) { memory[TIMEPLT_SPRITES1 + (Addr & 0xff)] = Value; return; }
  if(Addr == 0xc000) { timeplt_snd_latch = Value; return; }
  if((Addr & 0xfff0) == 0xc300) {            /* LS259, output select A1-A3 */
    switch((Addr >> 1) & 7) {
      case 0: timeplt_nmi_enable = Value & 1; break;
      case 2:                                /* sound IRQ on the rising edge */
        if((Value & 1) && !timeplt_snd_irq_last) timeplt_snd_irq = 1;
        timeplt_snd_irq_last = Value & 1;
        break;
      case 4: timeplt_video_enable = Value & 1; break;
      /* 1 flip screen, 3 mute, 5/6 coin counters: ignored */
    }
  }
  /* 0xc200: watchdog */
}

static inline void timeplt_run_frame(void) {
  timeplt_mux_used = 0;
  unsigned char captured = 0;
  timeplt_scanline = 0;

  for(int i = 0; i < 1280; i++) {
    current_cpu = 0; StepZ80(&cpu[0]); StepZ80(&cpu[0]); StepZ80(&cpu[0]); StepZ80(&cpu[0]);
    current_cpu = 1; StepZ80(&cpu[1]); timeplt_snd_icnt++; StepZ80(&cpu[1]); timeplt_snd_icnt++;
    if((i % 5) == 0) timeplt_scanline++;

    /* latched until the sound CPU has interrupts enabled (as Frogger) */
    if(timeplt_snd_irq && Z80_IFF1(&cpu[1])) {
      IntZ80(&cpu[1], INT_RST38);
      timeplt_snd_irq = 0;
    }
    /* the game polls the scanline before rewriting sprite RAM: keep the
     * first set so both get drawn */
    if(timeplt_mux_used && !captured) {
      captured = 1;
      memcpy(timeplt_mux_bank0, memory + TIMEPLT_SPRITES0, 0x100);
      memcpy(timeplt_mux_bank1, memory + TIMEPLT_SPRITES1, 0x100);
    }
  }
  if(!timeplt_mux_used) {
    memset(timeplt_mux_bank0, 0, 0x100);
    memset(timeplt_mux_bank1, 0, 0x100);
  }

  if(timeplt_nmi_enable) {
    current_cpu = 0;
    IntZ80(&cpu[0], INT_NMI);
  }
}

#endif // CPU_EMULATION

#ifdef IO_EMULATION

static inline void timeplt_add_sprites(const unsigned char *bank0, const unsigned char *bank1) {
  for(int offs = 0x3e; offs >= 0x10 && active_sprites < 128; offs -= 2) {
    unsigned char sx_raw = bank0[offs];
    unsigned char sy_raw = bank1[offs + 1];
    unsigned char attr   = bank1[offs];
    int sy = 241 - sy_raw;
    if(sy <= -16 || sy >= 256) continue;

    struct sprite_S spr;
    spr.x = (int)sy_raw - 17;
    spr.y = (int)sx_raw + 16;
    spr.code  = bank0[offs + 1];
    spr.color = attr & 0x3f;
    /* bit 0: landscape flip Y (bit 7), bit 1: landscape flip X (bit 6, inverted) */
    spr.flags = ((attr >> 7) & 1) | (((~attr >> 6) & 1) << 1);
    if((spr.y > -16) && (spr.y < 304) && (spr.x > -16) && (spr.x < 224))
      sprite[active_sprites++] = spr;
  }
}

static inline void timeplt_prepare_frame(void) {
  active_sprites = 0;
  if(!timeplt_video_enable) return;
  timeplt_add_sprites(timeplt_mux_bank0, timeplt_mux_bank1);          /* first set */
  timeplt_add_sprites(memory + TIMEPLT_SPRITES0, memory + TIMEPLT_SPRITES1);
}

/* cat: 0 = tiles under the sprites, 1 = over them (colour RAM bit 4) */
static inline void timeplt_blit_tile(short row, char col, unsigned char cat) {
  unsigned short addr = tileaddr[row][col];
  unsigned char cram = memory[TIMEPLT_COLORRAM + addr];
  if(((cram >> 4) & 1) != cat) return;

  const unsigned short *tile = timeplt_tiles[memory[TIMEPLT_VIDEORAM + addr] + ((cram & 0x20) << 3)];
  const unsigned short *colors = timeplt_char_colors[cram & 0x1f];
  /* under the 90 deg rotation the axes swap: bit 6 flips across, bit 7 down */
  unsigned char flip_x = (cram >> 6) & 1;
  unsigned char flip_y = (cram >> 7) & 1;

  unsigned short *ptr = frame_buffer + 8 * col;
  for(char r = 0; r < 8; r++, ptr += 224) {
    unsigned short pix = tile[flip_y ? 7 - r : r];
    if(flip_x)
      for(char c = 0; c < 8; c++) ptr[c] = colors[(pix >> ((7 - c) * 2)) & 3];
    else
      for(char c = 0; c < 8; c++, pix >>= 2) ptr[c] = colors[pix & 3];
  }
}

/* Landscape sprite drawn transposed: sprite row r -> screen x (reversed),
 * sprite column c -> screen y. Pen 0 transparent. */
static inline void timeplt_blit_sprite(short row, unsigned char s) {
  const unsigned long *spr = timeplt_sprites[sprite[s].code];
  const unsigned short *colors = timeplt_sprite_colors[sprite[s].color];
  unsigned char fy = sprite[s].flags & 1, fx = sprite[s].flags & 2;

  int top = 8 * row;
  int y0 = sprite[s].y > top ? sprite[s].y : top;
  int y1 = (sprite[s].y + 15) < (top + 7) ? (sprite[s].y + 15) : (top + 7);
  if(y0 > y1) return;

  for(int r = 0; r < 16; r++) {
    int x = sprite[s].x + 15 - r;
    if(x < 0 || x >= 224) continue;
    unsigned long line = spr[fy ? 15 - r : r];
    for(int y = y0; y <= y1; y++) {
      int c = y - sprite[s].y;
      unsigned char px = (line >> ((fx ? 15 - c : c) * 2)) & 3;
      if(px) frame_buffer[(y - top) * 224 + x] = colors[px];
    }
  }
}

static inline void timeplt_render_row(short row) {
  if(row <= 1 || row >= 34 || !timeplt_video_enable) return;
  for(char col = 0; col < 28; col++) timeplt_blit_tile(row, col, 0);
  for(unsigned char s = 0; s < active_sprites; s++)
    if((sprite[s].y < 8 * (row + 1)) && ((sprite[s].y + 16) > 8 * row))
      timeplt_blit_sprite(row, s);
  for(char col = 0; col < 28; col++) timeplt_blit_tile(row, col, 1);   /* e.g. (c) KONAMI */
}

#endif // IO_EMULATION

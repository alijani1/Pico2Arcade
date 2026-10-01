/* xevious.h — Xevious (Namco 1982).
 *
 * Ported from GalaginoPlus (VirtualClaudioBoy, GPLv3), machines/xevious,
 * reshaped into upstream Galagino's per-game C layout. Galaga's board family:
 * three Z80s (main, motion, sound), Namco 06xx/50xx/51xx/54xx custom I/O
 * (answered directly, as Galagino does for Galaga) and the Namco WSG. Video
 * is its own: two 64x32 RAM tilemaps (1bpp text, 2bpp ground) scrolled on
 * both axes, the ground drawn by the CPU from the "planet map" ROMs read
 * through 0xF000, and 16x16 3bpp sprites in 1x1 / 2x1 / 1x2 / 2x2 groups.
 *
 * The 54xx noise chip is not emulated: its explosion commands trigger two
 * synthesized noise bursts (GalaginoPlus plays recorded samples, which
 * aren't in the MAME zip).
 *
 * Main CPU (CPU2 / CPU3 share everything above 0x6800)   memory[]
 *   0x0000-0x3FFF  ROM (CPU2: 0x0000-0x1FFF, CPU3: 0x0000-0x0FFF)
 *   0x6800-0x6807 r DIPs (bit 0 DSWB, bit 1 DSWA), w 0x6800-0x681F WSG
 *   0x6820-0x6827 w LS259: IRQ1, IRQ2, NMI3, !reset CPU2/3 + custom chips
 *   0x7000-0x70FF   06xx data, 0x7100 06xx control
 *   0x7800-0x7FFF   work RAM                   0x0000
 *   0x8000-0x87FF   RAM + sprite RAM 1 (pos)   0x0800
 *   0x9000-0x97FF   RAM + sprite RAM 2 (size)  0x1000
 *   0xA000-0xA7FF   RAM + sprite RAM 3 (code)  0x1800
 *   0xB000-0xB7FF   text colour RAM            0x2000
 *   0xB800-0xBFFF   ground colour RAM          0x2800
 *   0xC000-0xC7FF   text video RAM             0x3000
 *   0xC800-0xCFFF   ground video RAM           0x3800
 *   0xD000-0xD07F w scroll registers
 *   0xF000-0xFFFF   w planet-map latch, r planet-map lookup
 */

#ifdef __cplusplus
extern "C" {
#endif
extern unsigned char  *xevious_rom_cpu1, *xevious_rom_cpu2, *xevious_rom_cpu3;
extern unsigned char  *xevious_planetmap;            /* 2A @0, 2B @0x1000, 2C @0x3000 */
extern unsigned char  (*xevious_fgtiles)[8];          /* 512 text chars, 1bpp, rotated */
extern unsigned short (*xevious_bgtiles)[8];          /* 512 ground tiles, 2bpp, rotated */
extern unsigned long  (*xevious_sprites)[32];         /* 320 x 16 rows x 2 words, 4 bit/px */
extern unsigned short (*xevious_cmap_fg)[2];          /* 64 */
extern unsigned short (*xevious_cmap_bg)[4];          /* 128 */
extern unsigned short (*xevious_cmap_spr)[8];         /* 64, 0 = transparent */
extern signed char    (*xevious_wavetable)[32];
extern unsigned short xevious_scroll[4];              /* bg x, fg x, bg y, fg y (emulation.c) */
extern unsigned char  xevious_boom;                   /* 54xx: 1 = ground, 2 = ship explosion */
#ifdef __cplusplus
}
#endif

#define XEV_WORK     0x0000
#define XEV_SR1      0x0800
#define XEV_SR2      0x1000
#define XEV_SR3      0x1800
#define XEV_FGCOLOR  0x2000
#define XEV_BGCOLOR  0x2800
#define XEV_FGVIDEO  0x3000
#define XEV_BGVIDEO  0x3800

/* DIPs read through 0x6800-0x6807 (MAME bosco_dsw_r): 0xFF = factory
 * settings (1 coin 1 credit, 3 lives, normal difficulty, upright) */
#define XEVIOUS_DSWA  0xff
#define XEVIOUS_DSWB  0xff

#ifdef CPU_EMULATION

static char xev_sub_reset = 1;
static unsigned char xev_cs_ctrl = 0;
static int xev_namco_cnt = 0, xev_nmi_cnt = 0;
static unsigned char xev_credit = 0, xev_credit_mode = 0, xev_coin_args = 0;
static unsigned char xev_xfer_busy = 0, xev_prot_param = 0, xev_54_skip = 0;
static unsigned long xev_rng = 0xace1u;
static unsigned char xev_bs[2] = { 0, 0 };

/* planet-map lookup (schematic 9B), MAME xevious_bb_r */
static inline unsigned char xevious_bb_r(unsigned char offset) {
  const unsigned char *rom2a = xevious_planetmap;
  const unsigned char *rom2b = xevious_planetmap + 0x1000;
  const unsigned char *rom2c = xevious_planetmap + 0x3000;
  int adr_2b = ((xev_bs[1] & 0x7e) << 6) | ((xev_bs[0] & 0xfe) >> 1);
  int dat1 = (adr_2b & 1) ? (((rom2a[adr_2b >> 1] & 0xf0) << 4) | rom2b[adr_2b])
                          : (((rom2a[adr_2b >> 1] & 0x0f) << 8) | rom2b[adr_2b]);
  int adr_2c = ((dat1 & 0x1ff) << 2) | ((xev_bs[1] & 1) << 1) | (xev_bs[0] & 1);
  if(dat1 & 0x400) adr_2c ^= 1;
  if(dat1 & 0x200) adr_2c ^= 2;
  if(offset & 1) return rom2c[adr_2c | 0x800];
  unsigned char dat2 = rom2c[adr_2c];
  dat2 = (dat2 & 0x3f) | ((dat2 & 0x80) >> 1) | ((dat2 & 0x40) << 1);   /* swap bits 6/7 */
  if(dat1 & 0x400) dat2 ^= 0x40;
  if(dat1 & 0x200) dat2 ^= 0x80;
  return dat2;
}

/* 51xx in credit mode: credits (BCD), P1 direction code + fire, P2 idle */
static inline unsigned char xevious_51xx_r(void) {
  static unsigned char prev_mask = 0, prev_fire = 0, fire_pulse = 0;
  unsigned char keymask = buttons_get();
  if(!xev_credit_mode) {
    if(xev_namco_cnt > 2) return 0xff;
    xev_namco_cnt++;
    return 0xff;
  }
  /* the 51xx passes a direction code 0..8 (8 = centre), from the ROM's
   * movement table at 0x1633 (GalaginoPlus, derived from the disassembly) */
  unsigned char up = keymask & BUTTON_UP,   dn = keymask & BUTTON_DOWN;
  unsigned char lf = keymask & BUTTON_LEFT, rt = keymask & BUTTON_RIGHT;
  unsigned char dir = 8;
  if      (up && lf) dir = 7;
  else if (up && rt) dir = 1;
  else if (dn && rt) dir = 3;
  else if (dn && lf) dir = 5;
  else if (lf)       dir = 6;
  else if (up)       dir = 0;
  else if (rt)       dir = 2;
  else if (dn)       dir = 4;

  /* fire, active low: bit 5 = held (the game repeats the zapper itself),
   * bit 4 = a short pulse on the press (confirms a letter in name entry) */
  unsigned char fire = 0x30;
  unsigned char held = keymask & BUTTON_FIRE;
  if(held) fire &= ~0x20;
  if(held && !prev_fire) fire_pulse = 2;
  if(fire_pulse) { fire &= ~0x10; fire_pulse--; }
  prev_fire = held;

  unsigned char data[3] = {
    (unsigned char)(16 * (xev_credit / 10) + xev_credit % 10),
    (unsigned char)(dir | fire),
    0x38 };                                    /* P2: centre, buttons up */
  if((keymask & BUTTON_START) && !(prev_mask & BUTTON_START) && xev_credit) xev_credit--;
  if((keymask & BUTTON_COIN) && !(prev_mask & BUTTON_COIN) && xev_credit < 99) xev_credit++;
  prev_mask = keymask;
  if(xev_namco_cnt > 2) return 0xff;
  return data[xev_namco_cnt++];
}

static inline unsigned char xevious_RdZ80(unsigned short Addr) {
  if(current_cpu == 0 && Addr < 0x4000) return xevious_rom_cpu1[Addr];
  if(current_cpu == 1 && Addr < 0x2000) return xevious_rom_cpu2[Addr];
  if(current_cpu == 2 && Addr < 0x1000) return xevious_rom_cpu3[Addr];

  if(Addr >= 0x7800 && Addr < 0xd000) {
    if(Addr < 0x8800) return memory[Addr - 0x7800];       /* work RAM + SR1 */
    if(!(Addr & 0x800)) {                                 /* 0x9000/0xA000 windows */
      if(Addr >= 0x9000 && Addr < 0x9800) return memory[XEV_SR2 + (Addr & 0x7ff)];
      if(Addr >= 0xa000 && Addr < 0xa800) return memory[XEV_SR3 + (Addr & 0x7ff)];
    }
    if(Addr >= 0xb000) return memory[XEV_FGCOLOR + (Addr - 0xb000)];   /* B000-CFFF */
    return 0xff;
  }

  if((Addr & 0xfff8) == 0x6800) {           /* DIPs; bomb is DSWB bit 0 */
    unsigned char bit = Addr & 7;
    unsigned char b0 = (XEVIOUS_DSWB >> bit) & 1;
    unsigned char b1 = (XEVIOUS_DSWA >> bit) & 1;
    if(bit == 0 && (buttons_get() & BUTTON_FIRE2)) b0 = 0;
    return b0 | (b1 << 1);
  }

  if(Addr == 0x7100) return xev_xfer_busy ? 0xe0 : 0x10;

  if(Addr >= 0x7000 && Addr <= 0x70ff) {
    if(xev_cs_ctrl == 0x74) {                 /* 50xx protection: 4th byte checked */
      unsigned char seq = xev_namco_cnt < 4 ? xev_namco_cnt : 4;
      xev_namco_cnt++;
      if(seq == 3) return (xev_prot_param == 0x80 || xev_prot_param == 0x10) ? 0x05 : 0x95;
      return 0xff;
    }
    if(xev_cs_ctrl & 1) return xevious_51xx_r();
    if(xev_cs_ctrl & 4) {                     /* 50xx random numbers */
      xev_rng ^= xev_rng << 7;
      xev_rng ^= xev_rng >> 9;
      xev_rng ^= xev_rng << 8;
      return (unsigned char)xev_rng;
    }
    return 0xff;
  }

  if(Addr >= 0xf000) return xevious_bb_r(Addr & 1);
  return 0xff;
}

static inline void xevious_WrZ80(unsigned short Addr, unsigned char Value) {
  if(Addr < 0x4000) return;

  if(Addr >= 0x7800 && Addr < 0xd000) {
    if(Addr < 0x8800) { memory[Addr - 0x7800] = Value; return; }
    if(!(Addr & 0x800)) {
      if(Addr >= 0x9000 && Addr < 0x9800) { memory[XEV_SR2 + (Addr & 0x7ff)] = Value; return; }
      if(Addr >= 0xa000 && Addr < 0xa800) { memory[XEV_SR3 + (Addr & 0x7ff)] = Value; return; }
    }
    if(Addr >= 0xb000) memory[XEV_FGCOLOR + (Addr - 0xb000)] = Value;
    return;
  }

  if((Addr & 0xffe0) == 0x6800) { soundregs[Addr - 0x6800] = Value & 0x0f; return; }

  if((Addr & 0xfff8) == 0x6820) {             /* LS259 */
    unsigned char bit = Addr & 7;
    if(bit <= 2) irq_enable[bit] = Value & 1;
    else if(bit == 3) {
      xev_sub_reset = !(Value & 1);
      xev_credit_mode = 0;                    /* resets the custom chips too */
      xev_namco_cnt = 0;
      xev_xfer_busy = 0;
      xev_coin_args = 0;
      if(xev_sub_reset) {
        char saved = current_cpu;
        current_cpu = 1; ResetZ80(&cpu[1]);
        current_cpu = 2; ResetZ80(&cpu[2]);
        current_cpu = saved;
      }
    }
    return;
  }

  if(Addr == 0x7100) {                        /* 06xx control: select chip, start transfer */
    xev_namco_cnt = 0;
    xev_cs_ctrl = Value;
    xev_xfer_busy = (Value & 0xe0) ? 1 : 0;
    xev_nmi_cnt = 0;
    xev_54_skip = 0;
    return;
  }

  if(Addr >= 0x7000 && Addr <= 0x70ff) {
    if(xev_cs_ctrl == 0x64) { xev_prot_param = Value; xev_namco_cnt++; return; }   /* 50xx */
    if(xev_cs_ctrl == 0x68) {                 /* 54xx: 1x / 2x play A / B; 3x/4x/6x params */
      xev_namco_cnt++;
      if(xev_54_skip) { xev_54_skip--; return; }
      switch(Value >> 4) {
        case 1: xevious_boom = 2; break;      /* type A: ship / big explosion */
        case 2: if(xevious_boom != 2) xevious_boom = 1; break;   /* type B: ground target */
        case 3: case 4: xev_54_skip = 4; break;
        case 6: xev_54_skip = 5; break;
      }
      return;
    }
    if(xev_cs_ctrl & 1) {                     /* 51xx commands; Xevious' coinage has 6 args */
      xev_namco_cnt++;
      if(xev_coin_args) { xev_coin_args--; return; }
      switch(Value & 7) {
        case 1: xev_coin_args = 6; break;
        case 2: xev_credit_mode = 1; break;
        case 5: xev_credit_mode = 0; break;
      }
    }
    return;
  }

  if((Addr & 0xff80) == 0xd000) {             /* scroll: 9 bits, bit 8 from A0 */
    unsigned char reg = ((Addr & 0x7f) >> 4) & 0xf;
    if(reg < 4) xevious_scroll[reg] = Value | ((Addr & 1) << 8);
    return;
  }

  if(Addr >= 0xf000) xev_bs[Addr & 1] = Value;
}

/* Wait loops: the main and motion CPUs spend about half their time polling
 * a flag their interrupt handler sets:
 *   CPU1 0437: LD HL,(8000) / DEC L / JR NZ,0437   until (8000) == 1
 *   CPU2 018D: LD A,(HL) / DEC A / JR NZ,018D       until (8002) == 1 (HL = 8002)
 * While a CPU sits at the loop start and its flag isn't set, its next steps
 * only rewrite HL / A and the flags, which the loop reloads anyway: skipping
 * them is equivalent. An interrupt still lands (it pushes the loop address
 * and the handler runs on the following steps). Not while an EI is still
 * pending (iff_delay), so interrupt timing stays the same. CPU3 idles with
 * JR $, which StepZ80 already short-circuits. */
static inline unsigned char xevious_idle(int c) {
  if(cpu[c].iff_delay || cpu[c].halted) return 0;
  if(c == 0) return cpu[0].pc == 0x0437 && memory[XEV_SR1 + 0] != 1;
  return cpu[1].pc == 0x018d && cpu[1].h == 0x80 && cpu[1].l == 0x02 && memory[XEV_SR1 + 2] != 1;
}

static inline void xevious_run_frame(void) {
  game_started = 1;
  for(int i = 0; i < INST_PER_FRAME; i++) {
    if(!xevious_idle(0)) {
      current_cpu = 0; StepZ80(&cpu[0]); StepZ80(&cpu[0]); StepZ80(&cpu[0]); StepZ80(&cpu[0]);
    }
    if(!xev_sub_reset) {
      if(!xevious_idle(1)) {
        current_cpu = 1; StepZ80(&cpu[1]); StepZ80(&cpu[1]); StepZ80(&cpu[1]); StepZ80(&cpu[1]);
      }
      current_cpu = 2; StepZ80(&cpu[2]); StepZ80(&cpu[2]); StepZ80(&cpu[2]); StepZ80(&cpu[2]);
    }

    /* main CPU NMI = the 06xx transfer engine (one byte per NMI). During a
     * transfer the real chip fires them in a quick burst, so it completes
     * before the task that asked for it reads the result (the protection
     * check depends on that); otherwise the periodic rate from cs_ctrl. */
    if(xev_xfer_busy) {
      if(xev_nmi_cnt < 8) xev_nmi_cnt++;
      else { current_cpu = 0; IntZ80(&cpu[0], INT_NMI); xev_nmi_cnt = 0; }
    } else if(xev_cs_ctrl & 0xe0) {
      if(xev_nmi_cnt < (xev_cs_ctrl >> 5) * 64) xev_nmi_cnt++;
      else { current_cpu = 0; IntZ80(&cpu[0], INT_NMI); xev_nmi_cnt = 0; }
    }

    if(!xev_sub_reset && !irq_enable[2] &&
       ((i == INST_PER_FRAME / 4) || (i == 3 * INST_PER_FRAME / 4))) {
      current_cpu = 2;
      IntZ80(&cpu[2], INT_NMI);
    }
  }
  if(irq_enable[0]) { current_cpu = 0; IntZ80(&cpu[0], INT_RST38); }
  if(!xev_sub_reset && irq_enable[1]) { current_cpu = 1; IntZ80(&cpu[1], INT_RST38); }
}

#endif // CPU_EMULATION

#ifdef IO_EMULATION

/* sprite_S has an 8-bit code: Xevious' 9th code bit rides in flags bit 2
 * (bits 0-1 are the flips) */
static inline void xevious_push_sprite(unsigned short code, unsigned char color,
                                       unsigned char flips, short mx, short my) {
  if(active_sprites >= 120) return;
  short px = my, py = mx;
  if(px <= -16 || px >= 224 || py <= -16 || py >= 288) return;
  struct sprite_S s;
  s.x = px; s.y = py;
  s.code = code & 0xff;
  s.color = color;
  s.flags = flips | ((code >> 8) & 1) << 2;
  sprite[active_sprites++] = s;
}

static inline void xevious_prepare_frame(void) {
  active_sprites = 0;
  const unsigned char *sr1 = memory + XEV_SR1 + 0x780;
  const unsigned char *sr2 = memory + XEV_SR2 + 0x780;
  const unsigned char *sr3 = memory + XEV_SR3 + 0x780;

  for(int offs = 0; offs < 0x80; offs += 2) {
    if(sr3[offs + 1] & 0x40) continue;                     /* disabled */
    unsigned short code = (sr2[offs] & 0x80) ? ((sr3[offs] & 0x3f) + 0x100) : sr3[offs];
    unsigned char color = sr3[offs + 1] & 0x7f;
    unsigned char fx = (sr2[offs] & 4) != 0, fy = (sr2[offs] & 8) != 0;
    unsigned char flips = fx | (fy << 1);
    short mx = sr1[offs + 1] - 40 + 0x100 * (sr2[offs + 1] & 1);   /* MAME 288 axis */
    short my = sr1[offs] - 15;                                      /* MAME 224 axis */
    unsigned char size = sr2[offs];

    if(size & 2) {                           /* double height */
      unsigned short base = code;
      if(size & 1) {                         /* + double width: 2x2 */
        base &= ~3;
        xevious_push_sprite(base + 3, color, flips, fx ? mx : mx + 16, fy ? my + 16 : my);
        xevious_push_sprite(base + 1, color, flips, fx ? mx : mx + 16, fy ? my : my + 16);
      }
      base &= ~2;
      xevious_push_sprite(base + 2, color, flips, fx ? mx + 16 : mx, fy ? my + 16 : my);
      xevious_push_sprite(base,     color, flips, fx ? mx + 16 : mx, fy ? my : my + 16);
    } else if(size & 1) {                    /* double width */
      unsigned short base = code & ~1;
      xevious_push_sprite(base,     color, flips, fx ? mx + 16 : mx, fy ? my + 16 : my);
      xevious_push_sprite(base + 1, color, flips, fx ? mx : mx + 16, fy ? my + 16 : my);
    } else
      xevious_push_sprite(code, color, flips, mx, my);
  }
}

/* One copy of each sprite, 16 rows x 2 words, 4 bits per pixel. Flags bit 0
 * reverses the rows, bit 1 the columns (GalaginoPlus' four variants). */
static inline void xevious_blit_sprite(short row, unsigned char s) {
  unsigned short code = sprite[s].code | ((sprite[s].flags & 4) << 6);
  const unsigned long *spr = xevious_sprites[code];
  const unsigned short *colors = xevious_cmap_spr[sprite[s].color & 0x3f];
  unsigned char fr = sprite[s].flags & 1, fc = sprite[s].flags & 2;

  short top = 8 * row;
  short x = sprite[s].x;
  unsigned char c0 = (x < 0) ? -x : 0;
  unsigned char c1 = (x > 224 - 16) ? 224 - x : 16;
  for(short y = 0; y < 8; y++) {
    short r = top + y - sprite[s].y;
    if(r < 0 || r > 15) continue;
    const unsigned long *line = spr + 2 * (fr ? 15 - r : r);
    unsigned short *ptr = frame_buffer + 224 * y + x;
    for(unsigned char c = c0; c < c1; c++) {
      unsigned char cc = fc ? 15 - c : c;
      unsigned short col = colors[(line[cc >> 3] >> (4 * (cc & 7))) & 0x0f];
      if(col) ptr[c] = col;
    }
  }
}

/* Both tilemaps, scrolled on both axes (MAME scrolldx/dy: ground -20/-16,
 * text -32/-18). Tile bitmaps are pre-rotated: row = MAME x within the
 * tile, bit = MAME y. Drawn per scanline in runs of up to 8 pixels. */
static inline void xevious_blit_tilemap_row(short row, unsigned char bg) {
  unsigned short scrollx = bg ? xevious_scroll[0] : xevious_scroll[1];
  unsigned short scrolly = bg ? xevious_scroll[2] : xevious_scroll[3];
  short dx = bg ? -20 : -32;
  short dy = bg ? -16 : -18;
  const unsigned char *videoram = memory + (bg ? XEV_BGVIDEO : XEV_FGVIDEO);
  const unsigned char *colorram = memory + (bg ? XEV_BGCOLOR : XEV_FGCOLOR);

  for(unsigned char ry = 0; ry < 8; ry++) {
    unsigned short mame_x = (unsigned short)(row * 8 + ry + scrollx - dx) & 0x1ff;
    unsigned char tile_col = (mame_x >> 3) & 63;
    unsigned char x_fine = mame_x & 7;
    unsigned short *ptr = frame_buffer + ry * 224;

    unsigned short mame_y = (unsigned short)(scrolly - dy + 223) & 0xff;
    unsigned char tile_row = (mame_y >> 3) & 31;
    unsigned char y_fine = mame_y & 7;

    short col = 0;
    while(col < 224) {
      unsigned short ti = tile_row * 64 + tile_col;
      unsigned char code = videoram[ti];
      unsigned char attr = colorram[ti];
      unsigned char sx = (attr & 0x40) ? (7 - x_fine) : x_fine;
      unsigned char flip_y = attr & 0x80;
      unsigned char run = y_fine + 1;
      if(run > 224 - col) run = 224 - col;
      col += run;

      if(bg) {
        unsigned short packed = xevious_bgtiles[code | ((attr & 1) << 8)][sx];
        unsigned char color = ((attr & 0x3c) >> 2) | ((code & 0x80) >> 3) | ((attr & 3) << 5);
        const unsigned short *cmap = xevious_cmap_bg[color & 0x7f];
        if(!flip_y) {
          unsigned char sh = 2 * (7 - y_fine);
          for(unsigned char i = 0; i < run; i++, sh += 2) *ptr++ = cmap[(packed >> sh) & 3];
        } else {
          unsigned char sh = 2 * y_fine;
          for(unsigned char i = 0; i < run; i++, sh -= 2) *ptr++ = cmap[(packed >> sh) & 3];
        }
      } else {
        unsigned char bits = xevious_fgtiles[code][sx];
        unsigned short c1 = xevious_cmap_fg[(((attr & 3) << 4) | ((attr & 0x3c) >> 2)) & 0x3f][1];
        if(!bits || !c1) ptr += run;          /* pen 0 transparent */
        else if(!flip_y) {
          unsigned char sh = 7 - y_fine;
          for(unsigned char i = 0; i < run; i++, sh++, ptr++) if((bits >> sh) & 1) *ptr = c1;
        } else {
          unsigned char sh = y_fine;
          for(unsigned char i = 0; i < run; i++, sh--, ptr++) if((bits >> sh) & 1) *ptr = c1;
        }
      }
      tile_row = (tile_row - 1) & 31;         /* next cell, MAME y decreasing */
      y_fine = 7;
    }
  }
}

static inline void xevious_render_row(short row) {
  xevious_blit_tilemap_row(row, 1);           /* ground, opaque */
  for(unsigned char s = 0; s < active_sprites; s++)
    if((sprite[s].y < 8 * (row + 1)) && ((sprite[s].y + 16) > 8 * row))
      xevious_blit_sprite(row, s);
  xevious_blit_tilemap_row(row, 0);           /* text on top */
}

/* ---- 54xx explosions: noise bursts with a falling level and a falling
 * noise clock (rumble). Ground targets ~1.0 s, the ship ~1.8 s. ---- */
static unsigned long xev_noise_lfsr = 1;
static unsigned long xev_noise_clk = 0;
static int xev_noise_out = 0, xev_boom_env = 0, xev_boom_len = 0, xev_boom_pos = 0;
static int xev_boom_lp = 0;

/* +/- ~120, mixed into the WSG output */
static inline short xevious_boom_sample(void) {
  if(xevious_boom) {
    unsigned char ship = xevious_boom == 2;
    if(!xev_boom_len || ship) {               /* a ship explosion may cut a ground one */
      xev_boom_len = ship ? 43000 : 24000;
      xev_boom_pos = 0;
    }
    xevious_boom = 0;
  }
  if(!xev_boom_len) return 0;
  /* level: fast attack, then decay to 0 over the length */
  int left = xev_boom_len - xev_boom_pos;
  int env = (left * 120) / xev_boom_len;
  if(xev_boom_pos < 240) env = env * xev_boom_pos / 240;
  /* noise clock falls from ~6 kHz to ~1 kHz as the explosion dies */
  xev_noise_clk += 1000 + 5000 * left / xev_boom_len;
  while(xev_noise_clk >= 24000) {
    xev_noise_clk -= 24000;
    unsigned long bit = ((xev_noise_lfsr >> 0) ^ (xev_noise_lfsr >> 2)) & 1;
    xev_noise_lfsr = (xev_noise_lfsr >> 1) | (bit << 16);
    xev_noise_out = (xev_noise_lfsr & 1) ? env : -env;
  }
  xev_boom_lp += (xev_noise_out - xev_boom_lp) / 4;   /* soften the edges */
  if(++xev_boom_pos >= xev_boom_len) xev_boom_len = 0;
  return (short)xev_boom_lp;
}

#endif // IO_EMULATION

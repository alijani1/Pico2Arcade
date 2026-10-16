/*
 * Galagino.ino - Galaga arcade for ESP32 and Arduino IDE
 *
 * (c) 2023 Till Harbaum <till@harbaum.org>
 * 
 * Published under GPLv3
 *
 */

#include "pico_compat.h"    /* Pico2Arcade port: Arduino.h + ESP32→Pico shims */
#include "config.h"

#include "driver/i2s.h"     /* Pico2Arcade: local no-op stub, see src/driver/i2s.h */
#include "video.h"
#include "leds.h"

#include "emulation.h"

#include "tileaddr.h"

#ifdef PICO_CUSTOM_INPUT
extern "C" bool pico_sd_init(void);
#include "pico_romfs.h"
#include "pico_board.h"
#include "pico_drive.h"
#include "pico_hiscore.h"
extern "C" void pico_input_poll(void);
extern "C" void pico_config_load(void);
#endif

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 5)
// See https://github.com/espressif/arduino-esp32/issues/8467
#define WORKAROUND_I2S_APLL_PROBLEM
// as a workaround we run dkong audio also at 24khz (instead of 11765)
// and fill the buffer at half the rate resulting in 12khz effective
// sample rate
unsigned char dkong_obuf_toggle = 0;
#endif

#define IO_EMULATION

// the hardware supports 64 sprites
unsigned char active_sprites = 0;
struct sprite_S *sprite;

// buffer space for one row of 28 characters
unsigned short *frame_buffer;

#ifdef NUNCHUCK_INPUT
#include "Nunchuck.h"
#endif

// include converted rom data
#ifdef ENABLE_PACMAN
#include "pacman.h"
#endif

#ifdef ENABLE_GALAGA
#include "galaga.h"
#endif

#ifdef ENABLE_DKONG
#include "dkong.h"
#endif

#ifdef ENABLE_FROGGER
#include "frogger.h"
#endif

#ifdef ENABLE_DIGDUG
#include "digdug.h"
#endif

#ifdef ENABLE_1942
#include "1942.h"
#endif

#ifdef ENABLE_INVADERS
#include "invaders.h"
#endif

#ifdef ENABLE_SCOBRA
#include "scobra.h"
#endif

#ifdef ENABLE_POOYAN
#include "pooyan.h"
#endif

#ifdef ENABLE_TIMEPLT
#include "timeplt.h"
#endif

#ifdef ENABLE_XEVIOUS
#include "xevious.h"
#endif

#ifndef SINGLE_MACHINE
signed char machine = MCH_MENU;   // start with menu
#endif

// instance of main tft driver
Video tft = Video();

TaskHandle_t emulationtask;

#ifdef ENABLE_GALAGA
// the ship explosion sound is stored as a digi sample.
// All other sounds are generated on the fly via the
// original wave tables
unsigned short snd_boom_cnt = 0;
const signed char *snd_boom_ptr = NULL;
#endif

#ifdef ENABLE_DKONG
unsigned short dkong_sample_cnt[3] = { 0,0,0 };
const signed char *dkong_sample_ptr[3];
#endif

#if defined(ENABLE_FROGGER) || defined(ENABLE_1942) || defined(ENABLE_SCOBRA) || defined(ENABLE_POOYAN) || defined(ENABLE_TIMEPLT)
#define AY_EMULATION
int ay_period[2][4] = {{0,0,0,0}, {0,0,0,0}};
int ay_volume[2][3] = {{0,0,0}, {0,0,0}};
int ay_enable[2][3] = {{0,0,0}, {0,0,0}};
int audio_cnt[2][4], audio_toggle[2][4] = {{1,1,1,1},{1,1,1,1}};
unsigned long ay_noise_rng[2] = { 1, 1 };
extern unsigned char soundregs[];

// Envelope generator (used only where enabled per game below). A write to
// R13 restarts it: the game's OutZ80 sets ay_env_restart[ay].
extern "C" { unsigned char ay_env_restart[2] = { 0, 0 }; }
static bool ay_env_on = false;
static char ay_use_env[2][3];
static int ay_env_period[2], ay_env_cnt[2];
static unsigned char ay_env_shape[2], ay_env_step[2], ay_env_hold[2], ay_env_holdval[2];
static bool ay_env_attack[2];
static inline int ay_env_volume(int ay) {
  if(ay_env_hold[ay]) return ay_env_holdval[ay];
  return ay_env_attack[ay] ? ay_env_step[ay] : 15 - ay_env_step[ay];
}
// one envelope step every 16*EP AY clocks = 2*EP in the clock/8 units of ay_inc
static inline void ay_env_advance(int ay, int ay_inc) {
  if(ay_env_hold[ay]) return;
  ay_env_cnt[ay] += ay_inc;
  int step_len = 2 * (ay_env_period[ay] ? ay_env_period[ay] : 1);
  while(ay_env_cnt[ay] >= step_len && !ay_env_hold[ay]) {
    ay_env_cnt[ay] -= step_len;
    if(++ay_env_step[ay] < 16) continue;
    unsigned char s = ay_env_shape[ay];      // CONT ATT ALT HOLD
    if(!(s & 8))     { ay_env_hold[ay] = 1; ay_env_holdval[ay] = 0; }
    else if(s & 1)   { ay_env_hold[ay] = 1; ay_env_holdval[ay] = ((s >> 2) ^ (s >> 1)) & 1 ? 15 : 0; }
    else             { if(s & 2) ay_env_attack[ay] = !ay_env_attack[ay]; ay_env_step[ay] = 0; }
  }
}
#endif

// one method to return to the main menu is to reset
// the entire machine. The main disadvantage is a 
// short noise from the speaker during reset
extern "C" void hw_reset(void);
void hw_reset(void) {
  ESP.restart();
}

#ifndef SINGLE_MACHINE
// convert rgb565 big endian color to greyscale
unsigned short greyscale(unsigned short in) {
  unsigned short r = (in>>3) & 31;
  unsigned short g = ((in<<3) & 0x38) | ((in>>13)&0x07);
  unsigned short b = (in>>8)& 31;
  unsigned short avg = (2*r + g + 2*b)/4;
  
  return (((avg << 13) & 0xe000) |   // g2-g0
          ((avg <<  7) & 0x1f00) |   // b5-b0
          ((avg <<  2) & 0x00f8) |   // r5-r0
          ((avg >>  3) & 0x0007));   // g5-g3
}

#ifndef PICO_NO_UPSTREAM_MENU
// render one of three the menu logos. Only the active one is colorful
// render logo into current buffer starting with line "row" of the logo
void render_logo(short row, const unsigned short *logo, char active) {
  unsigned short marker = logo[0];
  const unsigned short *data = logo+1;

  // current pixel to be drawn
  unsigned short ipix = 0;
    
  // less than 8 rows in image left?
  unsigned short pix2draw = ((row <= 96-8)?(224*8):((96-row)*224));
  
  if(row >= 0) {
    // skip ahead to row
    unsigned short col = 0;
    unsigned short pix = 0;
    while(pix < 224*row) {
      if(data[0] != marker) {
        pix++;
        data++;
      } else {
        pix += data[1]+1;
        col = data[2];
        data += 3;
      }
    }
    
    // draw pixels remaining from previous run
    if(!active) col = greyscale(col);
    while(ipix < ((pix - 224*row < pix2draw)?(pix - 224*row):pix2draw))
      frame_buffer[ipix++] = col;
  } else
    // if row is negative, then skip target pixel
    ipix -= row * 224;
    
  while(ipix < pix2draw) {
    if(data[0] != marker)
      frame_buffer[ipix++] = active?*data++:greyscale(*data++);
    else {
      unsigned short color = data[2];
      if(!active) color = greyscale(color);
      for(unsigned short j=0;j<data[1]+1 && ipix < pix2draw;j++)
        frame_buffer[ipix++] = color;

      data += 3;
    }
  }  
}
#endif /* !PICO_NO_UPSTREAM_MENU */
#endif

#if !defined(SINGLE_MACHINE) && !defined(PICO_NO_UPSTREAM_MENU)
// menu for more than three machines
const unsigned short *logos[] = {
#ifdef ENABLE_PACMAN    
  pacman_logo,
#endif
#ifdef ENABLE_GALAGA
  galaga_logo,
#endif
#ifdef ENABLE_DKONG    
  dkong_logo,
#endif
#ifdef ENABLE_FROGGER    
  frogger_logo,
#endif
#ifdef ENABLE_DIGDUG    
  digdug_logo,
#endif
#ifdef ENABLE_1942
  _1942_logo,
#endif
};
#endif

// render one of 36 tile rows (8 x 224 pixel lines)
void render_line(short row) {
  // the upper screen half of frogger has a blue background
  // using 8 in fact adds a tiny fraction of red as well. But that does not hurt
  memset(frame_buffer, 
#ifdef ENABLE_FROGGER 
    (MACHINE_IS_FROGGER && row <= 17)?8:
#endif
    0, 2*224*8);

#ifndef SINGLE_MACHINE
  if(machine == MCH_MENU) {
#ifndef PICO_NO_UPSTREAM_MENU

    if(MACHINES <= 3) {    
      // non-scrolling menu for 2 or 3 machines
      for(char i=0;i<sizeof(logos)/sizeof(unsigned short*);i++) {
      	char offset = i*12;
	      if(sizeof(logos)/sizeof(unsigned short*) == 2) offset += 6;
	
	      if(row >= offset && row < offset+12)  
	        render_logo(8*(row-offset), logos[i], menu_sel == i+1);
      }
    } else {
      // scrolling menu for more than 3 machines
    
      // valid offset values range from 0 to MACHINE*96-1
      static int offset = 0;

      // check which logo would show up in this row. Actually
      // two may show up in the same character row when scrolling
      int logo_idx = ((row + offset/8) / 12)%MACHINES;
      if(logo_idx < 0) logo_idx += MACHINES;
      
      int logo_y = (row * 8 + offset)%96;  // logo line in this row
      
      // check if logo at logo_y shows up in current row
      render_logo(logo_y, logos[logo_idx], (menu_sel-1) == logo_idx);
      
      // check if a second logo may show up here
      if(logo_y > (96-8)) {
        logo_idx = (logo_idx + 1)%MACHINES;
        logo_y -= 96;
        render_logo(logo_y, logos[logo_idx], (menu_sel-1) == logo_idx);
      }
      
      if(row == 35) {
      	// finally offset is bound to game, something like 96*game:    
	      int new_offset = 96*((unsigned)(menu_sel-2)%MACHINES);
	      if(menu_sel == 1) new_offset = (MACHINES-1)*96;
	
	      // check if we need to scroll
	      if(new_offset != offset) {
	        int diff = (new_offset - offset) % (MACHINES*96);
	        if(diff < 0) diff += MACHINES*96;
	  
	        if(diff < MACHINES*96/2) offset = (offset+8)%(MACHINES*96);
	        else                     offset = (offset-8)%(MACHINES*96);
	        if(offset < 0) offset += MACHINES*96;
      	}
      }
    }
#endif /* !PICO_NO_UPSTREAM_MENU: menu area stays black */
  } else
#endif  

#ifdef ENABLE_PACMAN
PACMAN_BEGIN
  pacman_render_row(row);
PACMAN_END
#endif
  
#ifdef ENABLE_GALAGA
GALAGA_BEGIN
  galaga_render_row(row);
GALAGA_END
#endif

#ifdef ENABLE_DKONG
DKONG_BEGIN
  dkong_render_row(row);
DKONG_END
#endif

#ifdef ENABLE_FROGGER
FROGGER_BEGIN
  frogger_render_row(row);
FROGGER_END
#endif
  
#ifdef ENABLE_DIGDUG
DIGDUG_BEGIN
  digdug_render_row(row);
DIGDUG_END
#endif

#ifdef ENABLE_1942
_1942_BEGIN
  _1942_render_row(row);
_1942_END
#endif

#ifdef ENABLE_INVADERS
INVADERS_BEGIN
  invaders_render_row(row);
INVADERS_END
#endif

#ifdef ENABLE_SCOBRA
SCOBRA_BEGIN
  scobra_render_row(row);
SCOBRA_END
#endif

#ifdef ENABLE_POOYAN
POOYAN_BEGIN
  pooyan_render_row(row);
POOYAN_END
#endif

#ifdef ENABLE_TIMEPLT
TIMEPLT_BEGIN
  timeplt_render_row(row);
TIMEPLT_END
#endif

#ifdef ENABLE_XEVIOUS
XEVIOUS_BEGIN
  xevious_render_row(row);
XEVIOUS_END
#endif
}
  
#ifdef ENABLE_GALAGA
void galaga_trigger_sound_explosion(void) {
  if(game_started) {
    snd_boom_cnt = 2*sizeof(galaga_sample_boom);
    snd_boom_ptr = (const signed char*)galaga_sample_boom;
  }
}
#endif

#ifdef USE_NAMCO_WAVETABLE
static unsigned long snd_cnt[3] = { 0,0,0 };
static unsigned long snd_freq[3];
static const signed char *snd_wave[3];
static unsigned char snd_volume[3];
#endif

#ifdef SND_DIFF
static unsigned short snd_buffer[128]; // buffer space for two channels
#else
static unsigned short snd_buffer[64];  // buffer space for a single channel
#endif

void snd_render_buffer(void) {
#ifdef AY_EMULATION
  // AY-3-8910s of this machine: count, clock step per 24 kHz sample
  // (AY clock / 8 / 24000) and volume (count * 3 * 15 * AY_VOL <= ~500)
  int AY = 0, AY_INC = 9, AY_VOL = 11;
  ay_env_on = false;
#ifdef ENABLE_FROGGER
  if(MACHINE_IS_FROGGER) { AY = 1; AY_INC = 9; AY_VOL = 11; }   // 1.78 MHz
#endif
#ifdef ENABLE_1942
  if(MACHINE_IS_1942)    { AY = 2; AY_INC = 8; AY_VOL = 4; }    // 1.5 MHz
#endif
#ifdef ENABLE_SCOBRA
  if(MACHINE_IS_SCOBRA)  { AY = 2; AY_INC = 9; AY_VOL = 5; ay_env_on = true; }
#endif
#ifdef ENABLE_POOYAN
  if(MACHINE_IS_POOYAN)  { AY = 2; AY_INC = 9; AY_VOL = 5; ay_env_on = true; }   // 1.79 MHz
#endif
#ifdef ENABLE_TIMEPLT
  if(MACHINE_IS_TIMEPLT) { AY = 2; AY_INC = 9; AY_VOL = 5; ay_env_on = true; }   // 1.79 MHz
#endif

  if(AY) {
    // up to two AY's
    for(char ay=0;ay<AY;ay++) {
      int ay_off = 16*ay;

      // three tone channels
      for(char c=0;c<3;c++) {
	ay_period[ay][c] = soundregs[ay_off+2*c] + 256 * (soundregs[ay_off+2*c+1] & 15);
	ay_enable[ay][c] = (((soundregs[ay_off+7] >> c)&1) | ((soundregs[ay_off+7] >> (c+2))&2))^3;
	ay_volume[ay][c] = soundregs[ay_off+8+c] & 0x0f;
	ay_use_env[ay][c] = ay_env_on && (soundregs[ay_off+8+c] & 0x10);
      }
      if(ay_env_on) {
        ay_env_period[ay] = soundregs[ay_off+11] + 256 * soundregs[ay_off+12];
        if(ay_env_restart[ay]) {
          ay_env_restart[ay] = 0;
          ay_env_shape[ay] = soundregs[ay_off+13] & 15;
          ay_env_attack[ay] = (ay_env_shape[ay] & 4) != 0;
          ay_env_step[ay] = 0;
          ay_env_cnt[ay] = 0;
          ay_env_hold[ay] = 0;
        }
      }
      // noise channel
      ay_period[ay][3] = soundregs[ay_off+6] & 0x1f;
    }
  }
#endif

#ifdef ENABLE_INVADERS
  if(MACHINE_IS_INVADERS) invaders_sound_regs();
#endif

  // render first buffer contents
  for(int i=0;i<64;i++) {
    short v = 0;

#if defined(ENABLE_PACMAN) || defined(ENABLE_GALAGA)
  #ifndef SINGLE_MACHINE
    if(0
    #ifdef ENABLE_PACMAN
        || (machine == MCH_PACMAN)
    #endif
    #ifdef ENABLE_MSPACMAN
        || (machine == MCH_MSPACMAN)
    #endif
    #ifdef ENABLE_GALAGA
        || (machine == MCH_GALAGA)
    #endif
    #ifdef ENABLE_DIGDUG
        || (machine == MCH_DIGDUG)
    #endif
    #ifdef ENABLE_XEVIOUS
        || (machine == MCH_XEVIOUS)
    #endif
    )
  #endif
    {
      // add up to three wave signals
      if(snd_volume[0]) v += snd_volume[0] * snd_wave[0][(snd_cnt[0]>>13) & 0x1f];
      if(snd_volume[1]) v += snd_volume[1] * snd_wave[1][(snd_cnt[1]>>13) & 0x1f];
      if(snd_volume[2]) v += snd_volume[2] * snd_wave[2][(snd_cnt[2]>>13) & 0x1f];

  #ifdef ENABLE_GALAGA
      if(snd_boom_cnt) {
        v += *snd_boom_ptr;
        if(snd_boom_cnt & 1) snd_boom_ptr++;
        snd_boom_cnt--;
      }
  #endif
  #ifdef ENABLE_XEVIOUS
      if(MACHINE_IS_XEVIOUS) v += xevious_boom_sample();
  #endif
    }
#endif    

#ifdef ENABLE_DKONG
DKONG_BEGIN
    {
      v = 0;  // silence

      // no buffer available
      if(dkong_audio_rptr != dkong_audio_wptr)
        // copy data from dkong buffer into tx buffer
        // 8048 sounds gets 50% of the available volume range
#ifdef WORKAROUND_I2S_APLL_PROBLEM
        v = dkong_audio_transfer_buffer[dkong_audio_rptr][(dkong_obuf_toggle?32:0)+(i/2)];
#else
        v = dkong_audio_transfer_buffer[dkong_audio_rptr][i];
#endif
      // include sample sounds
      // walk is 6.25% volume, jump is at 12.5% volume and, stomp is at 25%
      for(char j=0;j<3;j++) {
        if(dkong_sample_cnt[j]) {
#ifdef WORKAROUND_I2S_APLL_PROBLEM
          v += *dkong_sample_ptr[j] >> (2-j); 
          if(i & 1) { // advance read pointer every second sample
            dkong_sample_ptr[j]++;
            dkong_sample_cnt[j]--;
          }
#else
          v += *dkong_sample_ptr[j]++ >> (2-j); 
          dkong_sample_cnt[j]--;
#endif
        }
      }
    }
DKONG_END
#endif

#ifdef AY_EMULATION
    if(AY) {
    v = 0;  // silence
    
    for(char ay=0;ay<AY;ay++) {
      if(ay_env_on) ay_env_advance(ay, AY_INC);

      // frogger can acually skip the noise generator as
      // it doesn't use it      
      if(ay_period[ay][3]) {
	      // process noise generator
	      audio_cnt[ay][3] += AY_INC; // for 24 khz
	      if(audio_cnt[ay][3] > ay_period[ay][3]) {
	        audio_cnt[ay][3] -=  ay_period[ay][3];
	        // progress rng
	        ay_noise_rng[ay] ^= (((ay_noise_rng[ay] & 1) ^ ((ay_noise_rng[ay] >> 3) & 1)) << 17);
	        ay_noise_rng[ay] >>= 1;
	      }
      }
	
      for(char c=0;c<3;c++) {
	      // a channel is on if period != 0, vol != 0 and tone bit == 0
	      int vol = ay_use_env[ay][c] ? ay_env_volume(ay) : ay_volume[ay][c];
	      if(ay_period[ay][c] && (vol || ay_use_env[ay][c]) && ay_enable[ay][c]) {
	        short bit = 1;
	        if(ay_enable[ay][c] & 1) bit &= (audio_toggle[ay][c]>0)?1:0;  // tone
	        if(ay_enable[ay][c] & 2) bit &= (ay_noise_rng[ay]&1)?1:0;     // noise
	  
	        if(bit == 0) bit = -1;
	        v += AY_VOL * bit * vol;
	  
	        audio_cnt[ay][c] += AY_INC; // for 24 khz
	        if(audio_cnt[ay][c] > ay_period[ay][c]) {
	          audio_cnt[ay][c] -= ay_period[ay][c];
	          audio_toggle[ay][c] = -audio_toggle[ay][c];
	        }
	      }
      }
    }
    }
#endif
#ifdef ENABLE_INVADERS
    if(MACHINE_IS_INVADERS) v = invaders_sound_sample();
#endif

    // v is now in the range of +/- 512, so expand to +/- 15 bit
    v = v*MASTER_VOLUME;

#ifdef SND_DIFF
    // generate differential output
    snd_buffer[2*i]   = 0x8000 + v;    // positive signal on GPIO26
    snd_buffer[2*i+1] = 0x8000 - v;    // negatve signal on GPIO25
#else
    // work-around weird byte order bug, see
    // https://github.com/espressif/arduino-esp32/issues/8467#issuecomment-1656616015
    snd_buffer[i^1]   = 0x8000 + v;
#endif
      
#if defined(ENABLE_PACMAN) || defined(ENABLE_GALAGA)
    snd_cnt[0] += snd_freq[0];
    snd_cnt[1] += snd_freq[1];
    snd_cnt[2] += snd_freq[2];
#endif
  }
  
#ifdef ENABLE_DKONG
  #ifndef SINGLE_MACHINE
  if(MACHINE_IS_DKONG)
  #endif
  {
#ifdef WORKAROUND_I2S_APLL_PROBLEM
    if(dkong_obuf_toggle)
#endif
      // advance write pointer. The buffer is a ring
      dkong_audio_rptr = (dkong_audio_rptr+1)&DKONG_AUDIO_QUEUE_MASK;
#ifdef WORKAROUND_I2S_APLL_PROBLEM
    dkong_obuf_toggle = !dkong_obuf_toggle;
#endif
  }
#endif
}

#ifdef USE_NAMCO_WAVETABLE
void audio_namco_waveregs_parse(void) {
#ifndef SINGLE_MACHINE
  if(
#ifdef ENABLE_PACMAN
    MACHINE_IS_PACMAN ||
#endif    
#ifdef ENABLE_GALAGA    
    MACHINE_IS_GALAGA ||
#endif
#ifdef ENABLE_DIGDUG    
    MACHINE_IS_DIGDUG ||
#endif
#ifdef ENABLE_XEVIOUS
    MACHINE_IS_XEVIOUS ||
#endif
  0) 
#endif  
  {
    // parse all three wsg channels
    for(char ch=0;ch<3;ch++) {  
      // channel volume
      snd_volume[ch] = soundregs[ch * 5 + 0x15];
    
      if(snd_volume[ch]) {
        // frequency
        snd_freq[ch] = (ch == 0) ? soundregs[0x10] : 0;
        snd_freq[ch] += soundregs[ch * 5 + 0x11] << 4;
        snd_freq[ch] += soundregs[ch * 5 + 0x12] << 8;
        snd_freq[ch] += soundregs[ch * 5 + 0x13] << 12;
        snd_freq[ch] += soundregs[ch * 5 + 0x14] << 16;
      
        // wavetable entry — explicit else-if chain. Upstream's #ifdef
        // stitching produced an unguarded digdug assignment that would
        // overwrite pacman/galaga when all three were compiled together
        // (the else binds to a single statement; the digdug line then
        // ran unconditionally after the chain).
#if defined(ENABLE_PACMAN)
        if(MACHINE_IS_PACMAN) {
          /* Namco WSG3 has 8 waves (single PROM). Mask 4-bit wave-select
           * register to 3 bits — Ms. Pac-Man writes 0x8-0xF for some
           * sounds but real hardware wraps those to 0x0-0x7. */
          snd_wave[ch] = pacman_wavetable[soundregs[ch * 5 + 0x05] & 0x07];
        }
  #if defined(ENABLE_GALAGA) || defined(ENABLE_DIGDUG)
        else
  #endif
#endif
#if defined(ENABLE_GALAGA)
        if(machine == MCH_GALAGA) {
          snd_wave[ch] = galaga_wavetable[soundregs[ch * 5 + 0x05] & 0x07];
        }
  #if defined(ENABLE_DIGDUG)
        else
  #endif
#endif
#if defined(ENABLE_DIGDUG)
        if(machine == MCH_DIGDUG) {
          snd_wave[ch] = digdug_wavetable[soundregs[ch * 5 + 0x05] & 0x0f];
        }
#endif
#if defined(ENABLE_XEVIOUS)
        if(machine == MCH_XEVIOUS) {
          snd_wave[ch] = xevious_wavetable[soundregs[ch * 5 + 0x05] & 0x07];
        }
#endif
      }
    }
  }
}
#endif  // MCH_PACMAN || MCH_GALAGA

#ifdef ENABLE_DKONG
void dkong_trigger_sound(char snd) {
  static const struct {
    const signed char *data;
    const unsigned short length; 
  } samples[] = {
    { (const signed char *)dkong_sample_walk0, sizeof(dkong_sample_walk0) },
    { (const signed char *)dkong_sample_walk1, sizeof(dkong_sample_walk1) },
    { (const signed char *)dkong_sample_walk2, sizeof(dkong_sample_walk2) },
    { (const signed char *)dkong_sample_jump,  sizeof(dkong_sample_jump)  },
    { (const signed char *)dkong_sample_stomp, sizeof(dkong_sample_stomp) }
  };

  // samples 0 = walk, 1 = jump, 2 = stomp

  if(!snd) {
    // walk0, walk1 and walk2 are variants
    char rnd = random() % 3;
    dkong_sample_cnt[0] = samples[rnd].length;
    dkong_sample_ptr[0] = samples[rnd].data;
  } else {
    dkong_sample_cnt[snd] = samples[snd+2].length;
    dkong_sample_ptr[snd] = samples[snd+2].data;
  }
}
#endif

void snd_transmit() {
  // (try to) transmit as much audio data as possible. Since we
  // write data in exact the size of the DMA buffers we can be sure
  // that either all or nothing is actually being written
  
  size_t bytesOut = 0;
  do {
    // copy data in i2s dma buffer if possible
    i2s_write(I2S_NUM_0, snd_buffer, sizeof(snd_buffer), &bytesOut, 0);

    // render the next audio chunk if data has actually been sent
    if(bytesOut) {
#if defined(ENABLE_PACMAN) || defined(ENABLE_GALAGA)
      audio_namco_waveregs_parse();
#endif
      snd_render_buffer();
    }
  } while(bytesOut);
}

void audio_dkong_bitrate(char is_dkong) {
  // The audio CPU of donkey kong runs at 6Mhz. A full bus
  // cycle needs 15 clocks which results in 400k cycles
  // per second. The sound CPU typically needs 34 instruction
  // cycles to write an updated audio value to the external
  // DAC connected to port 0.
  
  // The effective sample rate thus is 6M/15/34 = 11764.7 Hz
#ifndef WORKAROUND_I2S_APLL_PROBLEM
  i2s_set_sample_rates(I2S_NUM_0, is_dkong?11765:24000);
#endif
}

void audio_init(void) {  
  // init audio
#if defined(ENABLE_PACMAN) || defined(ENABLE_GALAGA)
  audio_namco_waveregs_parse();
#endif
  snd_render_buffer();

  // 24 kHz @ 16 bit = 48000 bytes/sec = 800 bytes per 60hz game frame =
  // 1600 bytes per 30hz screen update = ~177 bytes every four tile rows
  static const i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN),
    .sample_rate = 24000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
#ifdef SND_DIFF
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
#elif defined(SND_LEFT_CHANNEL) // For devices using the left channel (e.g. CYD)
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
#else
    .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
#endif
    .intr_alloc_flags = 0,
    .dma_buf_count = 4,
    .dma_buf_len = 64,   // 64 samples
#ifndef WORKAROUND_I2S_APLL_PROBLEM
    .use_apll = true
#else
    // APLL usage is broken in ESP-IDF 4.4.5
    .use_apll = false
#endif
  };

  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);

#if defined(SINGLE_MACHINE) && defined(ENABLE_DKONG)
  // only dkong installed? Then setup rate immediately
  audio_dkong_bitrate(true);
#endif

#ifdef SND_DIFF
  i2s_set_dac_mode(I2S_DAC_CHANNEL_BOTH_EN);
#elif defined(SND_LEFT_CHANNEL) // For devices using the left channel (e.g. CYD)
  i2s_set_dac_mode(I2S_DAC_CHANNEL_LEFT_EN); 
#else
  i2s_set_dac_mode(I2S_DAC_CHANNEL_RIGHT_EN);
#endif
}

void update_screen(void) {
  uint32_t t0 = micros();
  // Start of this two-frame video cycle (see the half-rate pacing below).
  static uint32_t frame_base = 0;
  if (frame_base == 0) frame_base = t0;

#ifdef ENABLE_PACMAN
PACMAN_BEGIN
  pacman_prepare_frame();    
PACMAN_END
#endif

#ifdef ENABLE_GALAGA
GALAGA_BEGIN
  galaga_prepare_frame();
GALAGA_END
#endif  
  
#ifdef ENABLE_DKONG
DKONG_BEGIN
  dkong_prepare_frame();
DKONG_END
#endif

#ifdef ENABLE_FROGGER
FROGGER_BEGIN
  frogger_prepare_frame();
FROGGER_END
#endif

#ifdef ENABLE_DIGDUG
DIGDUG_BEGIN
  digdug_prepare_frame();
DIGDUG_END
#endif

#ifdef ENABLE_1942
_1942_BEGIN
  _1942_prepare_frame();
_1942_END
#endif

#ifdef ENABLE_INVADERS
INVADERS_BEGIN
  invaders_prepare_frame();
INVADERS_END
#endif

#ifdef ENABLE_SCOBRA
SCOBRA_BEGIN
  scobra_prepare_frame();
SCOBRA_END
#endif

#ifdef ENABLE_POOYAN
POOYAN_BEGIN
  pooyan_prepare_frame();
POOYAN_END
#endif

#ifdef ENABLE_TIMEPLT
TIMEPLT_BEGIN
  timeplt_prepare_frame();
TIMEPLT_END
#endif

#ifdef ENABLE_XEVIOUS
XEVIOUS_BEGIN
  xevious_prepare_frame();
XEVIOUS_END
#endif

  // max possible video rate:
  // 8*224 pixels = 8*224*16 = 28672 bits
  // 2790 char rows per sec at 40Mhz = max 38 fps
#if TFT_SPICLK < 80000000
#define VIDEO_HALF_RATE
#endif

#ifdef VIDEO_HALF_RATE
  // render and transmit screen in two halfs as the display
  // running at 40Mhz can only update every second 60 hz game frame
  for(int half=0;half<2;half++) {

    for(int c=18*half;c<18*(half+1);c+=3) {
      render_line(c+0); tft.write(frame_buffer, 224*8);
      render_line(c+1); tft.write(frame_buffer, 224*8);
      render_line(c+2); tft.write(frame_buffer, 224*8);

      // audio is refilled 6 times per screen update. The screen is updated
      // every second frame. So audio is refilled 12 times per 30 Hz frame.
      // Audio registers are udated by CPU3 two times per 30hz frame.
      snd_transmit();
    } 
 
    // Pico2Arcade: pace each half against an absolute microsecond
    // deadline (frame_base + 16667 / + 33333). Upstream waited in whole
    // milliseconds from a truncated elapsed time, and the per-cycle
    // overhead accumulated, so games ran at ~56 fps (screen ~28) instead
    // of 60. Sleep in 1 ms steps while far from the deadline, then spin.
    uint32_t deadline = frame_base + (half ? 33333u : 16667u);
    while ((int32_t)(deadline - micros()) > 1500) delay(1);
    while ((int32_t)(deadline - micros()) > 0) tight_loop_contents();

    // physical refresh is 30Hz. So send vblank trigger twice a frame
    // to the emulation. This will make the game run with 60hz speed
    xTaskNotifyGive(emulationtask);
  }
  // Next cycle starts exactly 2 frames later. A small overrun is caught
  // up on the next cycle; after a long stall (e.g. a game load) resync
  // instead of racing to catch up.
  frame_base += 33333u;
  if ((int32_t)(micros() - frame_base) > 33333) frame_base = micros();
#else
  #warning FULL SPEED
  
  // render and transmit screen at once as the display
  // running at 80Mhz can update at full 60 hz game frame
  for(int c=0;c<36;c+=6) {
    render_line(c+0); tft.write(frame_buffer, 224*8);
    render_line(c+1); tft.write(frame_buffer, 224*8);
    render_line(c+2); tft.write(frame_buffer, 224*8);
    render_line(c+3); tft.write(frame_buffer, 224*8);
    render_line(c+4); tft.write(frame_buffer, 224*8);
    render_line(c+5); tft.write(frame_buffer, 224*8);

    // audio is updated 6 times per 60 Hz frame
    snd_transmit();
  } 
 
  // one screen at 60 Hz is 16.6ms
  unsigned long t1 = (micros()-t0)/1000;  // calculate time in milliseconds
  if(t1<16) vTaskDelay(16-t1);
  else      vTaskDelay(1);    // at least 1 ms delay to prevent watchdog timeout

  // physical refresh is 60Hz. So send vblank trigger once a frame
  xTaskNotifyGive(emulationtask);
#endif
   
#ifdef ENABLE_GALAGA
  /* the screen is only updated every second frame, scroll speed is thus doubled */
  static const signed char speeds[8] = { -1, -2, -3, 0, 3, 2, 1, 0 };
  stars_scroll_y += 2*speeds[starcontrol & 7];
#endif
}

void emulation_task(void *p) {
  prepare_emulation();

  while(1) {
    emulate_frame();
    pico_hiscore_frame();   /* restore / watch the high-score table (pico_hiscore.cpp) */
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("Galagino"); 

  Serial.print("ESP-IDF "); 
  Serial.println(ESP_IDF_VERSION, HEX); 

#ifdef WORKAROUND_I2S_APLL_PROBLEM
  Serial.println("I2S APLL workaround active"); 
#endif

  // this should not be needed as the CPU runs by default on 240Mhz nowadays
  setCpuFrequencyMhz(240);

  Serial.print("Free heap: "); Serial.println(ESP.getFreeHeap());
  Serial.print("Main core: "); Serial.println(xPortGetCoreID());
  Serial.print("Main priority: "); Serial.println(uxTaskPriorityGet(NULL));  

  // allocate memory for a single tile/character row
  frame_buffer = (unsigned short*)malloc(224*8*2);
  sprite = (struct sprite_S*)malloc(128 * sizeof(struct sprite_S));
  Serial.print("Free heap: "); Serial.println(ESP.getFreeHeap());

  // make button pins inputs
#ifdef PICO_CUSTOM_INPUT
  /* Direct-GPIO + PCA9539 init happens in pico_input.cpp. Sentinel
   * BTN_*_PIN values in config.h aren't real GPIOs.
   * (Forward decl in pico_compat.h.) */
#ifdef PICO_BOARD_TABLE
  /* Pin table first (standalone: ARCADE.INI on the ROM drive) — input and
   * audio are set up from it. */
  romfs_init();
  board_pins_load();
#endif
  pico_input_init();
  /* SD init deferred until after tft.begin() below — the CDC monitor
   * frequently doesn't attach in time to catch pre-video output, so
   * we move SD prints to the visible window. */
#else
  pinMode(BTN_START_PIN, INPUT_PULLUP);
#ifdef BTN_COIN_PIN
  pinMode(BTN_COIN_PIN, INPUT_PULLUP);
#endif

#ifdef NUNCHUCK_INPUT
  nunchuckSetup();
#else
  pinMode(BTN_LEFT_PIN, INPUT_PULLUP);
  pinMode(BTN_RIGHT_PIN, INPUT_PULLUP);
  pinMode(BTN_DOWN_PIN, INPUT_PULLUP);
  pinMode(BTN_UP_PIN, INPUT_PULLUP);
  pinMode(BTN_FIRE_PIN, INPUT_PULLUP);
#endif
#endif  /* !PICO_CUSTOM_INPUT */

  // initialize audio to default bitrate (24khz unless dkong is
  // the only game installed, then audio will directly be 
  // initialized to dkongs 11765hz)
  audio_init();

#ifdef LED_PIN
  leds_init();
#endif

  // let the cpu emulation run on the second core, so the main core
  // can completely focus on video
  xTaskCreatePinnedToCore(
      emulation_task, /* Function to implement the task */
      "emulation task", /* Name of the task */
      4096,  /* Stack size in words */
      NULL,  /* Task input parameter */
      2,  /* Priority of the task */
      &emulationtask,  /* Task handle. */
      0); /* Core where the task should run */

#ifdef PICO_CUSTOM_INPUT
  /* SD + config must be up BEFORE Video::begin so it can pick rotation
   * based on the runtime g_landscape_mode. */
#ifndef PICO_BOARD_TABLE
  romfs_init();          /* SD card, or the Quad's flash ROM drive (pico_romfs.cpp) */
#endif
  pico_config_load();
#endif

  tft.begin();

#ifdef PICO_USB_DRIVE
  /* USB ROM drive mode (menu row, or standalone: MENU held / first boot). */
  if (pico_drive_requested()) pico_drive_run();
#endif
#ifdef PICO_CUSTOM_INPUT
  pico_menu_pick_and_load();
#endif
}

#ifndef PICO_CUSTOM_INPUT
unsigned char buttons_get(void) {
  // galagino can be compiled without coin button. This will then
  // be implemented by the start button. Whenever the start button
  // is pressed, a virtual coin button will be sent first
  unsigned char input_states = 0;
#ifdef NUNCHUCK_INPUT
  input_states |= getNunchuckInput();
#endif;

#ifndef BTN_COIN_PIN
#ifdef BTN_COIN_PIN
  input_states |= (!digitalRead(BTN_COIN_PIN)) ? BUTTON_EXTRA : 0;
#else
  input_states |= (!digitalRead(BTN_START_PIN)) ? BUTTON_EXTRA : 0;
#endif
  static unsigned long virtual_coin_timer = 0;
  static int virtual_coin_state = 0;
  switch(virtual_coin_state)  {
    case 0:  // idle state
      if(input_states & BUTTON_EXTRA) {
        virtual_coin_state = 1;   // virtual coin pressed
        virtual_coin_timer = millis();
      }
      break;
    case 1:  // start was just pressed
      // check if 100 milliseconds have passed
      if(millis() - virtual_coin_timer > 100) {
        virtual_coin_state = 2;   // virtual coin released
        virtual_coin_timer = millis();        
      }
      break;
    case 2:  // virtual coin was released
      // check if 500 milliseconds have passed
      if(millis() - virtual_coin_timer > 500) {
        virtual_coin_state = 3;   // pause between virtual coin an start ended
        virtual_coin_timer = millis();        
      }
      break;
    case 3:  // pause ended
      // check if 100 milliseconds have passed
      if(millis() - virtual_coin_timer > 100) {
        virtual_coin_state = 4;   // virtual start ended
        virtual_coin_timer = millis();        
      }
      break;
    case 4:  // virtual start has ended
      // check if start button is actually still pressed
      if(! (input_states & BUTTON_EXTRA))
        virtual_coin_state = 0;   // button has been released, return to idle
      break;
  }
#endif

#ifndef SINGLE_MACHINE
  static unsigned long reset_timer = 0;
  
  // reset if coin (or start if no coin is configured) is held for
  // more than 1 second
  if(input_states & BUTTON_EXTRA) {
    if(machine != MCH_MENU) {

#ifdef MASTER_ATTRACT_GAME_TIMEOUT
       // if the game was started by the master attract mode then the user
       // pressing coin (or start) stops the timer, so the game keeps
       // running as long as the user wants
       master_attract_timeout = 0;
#endif

      if(!reset_timer)
        reset_timer = millis();

      if(millis() - reset_timer > 1000) {
        // disable backlight if pin is specified
#ifdef TFT_BL
        digitalWrite(TFT_BL, LOW);
#endif

        emulation_reset();
      }
    }    
  } else
    reset_timer = 0;
#endif
  
  unsigned char startAndCoinState =
#ifdef BTN_COIN_PIN
      // there is a coin pin -> coin and start work normal
      (digitalRead(BTN_START_PIN) ? 0 : BUTTON_START) |
      (digitalRead(BTN_COIN_PIN) ? 0 : BUTTON_COIN);
#else
      ((virtual_coin_state != 1) ? 0 : BUTTON_COIN) |
      (((virtual_coin_state != 3) && (virtual_coin_state != 4)) ? 0 : BUTTON_START); 
#endif

#ifdef NUNCHUCK_INPUT
      return startAndCoinState |
      input_states;
#else
      return startAndCoinState |
      (digitalRead(BTN_LEFT_PIN) ? 0 : BUTTON_LEFT) |
      (digitalRead(BTN_RIGHT_PIN) ? 0 : BUTTON_RIGHT) |
      (digitalRead(BTN_UP_PIN) ? 0 : BUTTON_UP) |
      (digitalRead(BTN_DOWN_PIN) ? 0 : BUTTON_DOWN) |
      (digitalRead(BTN_FIRE_PIN) ? 0 : BUTTON_FIRE);
#endif
}
#endif  /* !PICO_CUSTOM_INPUT — real impl lives in pico_input.cpp */

void loop(void) {
  // run video in main task. This will send signals
  // to the emulation task in the background to 
  // synchronize video
  update_screen(); 
  pico_hiscore_service();   /* high-score saves + save-then-exit (core 0 owns flash / SD) */
  pico_input_poll();        /* core-0-only inputs, e.g. the 5200 stick (pico_input.cpp) */

#ifdef LED_PIN
  leds_update();
#endif
}

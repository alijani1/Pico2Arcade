#ifndef _EMULATION_H_
#define _EMULATION_H_

#ifdef ENABLE_DKONG
#define DKONG_AUDIO_QUEUE_LEN   16
#define DKONG_AUDIO_QUEUE_MASK (DKONG_AUDIO_QUEUE_LEN-1)

extern unsigned char dkong_audio_transfer_buffer[DKONG_AUDIO_QUEUE_LEN][64];
extern unsigned char dkong_audio_rptr, dkong_audio_wptr;
#endif

// a total of 7 button is needed for
// most games
#define BUTTON_LEFT  0x01
#define BUTTON_RIGHT 0x02
#define BUTTON_UP    0x04
#define BUTTON_DOWN  0x08
#define BUTTON_FIRE  0x10
#define BUTTON_START 0x20
#define BUTTON_COIN  0x40
#define BUTTON_EXTRA 0x80
// second fire button (B), reported only while a two-button game runs
// (g_two_fire, pico_input.cpp) — e.g. Super Cobra / Scramble bomb
#define BUTTON_FIRE2 BUTTON_EXTRA

#ifndef SINGLE_MACHINE
enum {
      MCH_MENU = 0,
#ifdef ENABLE_PACMAN
      MCH_PACMAN,
#endif
#ifdef ENABLE_MSPACMAN
      MCH_MSPACMAN,
#endif
#ifdef ENABLE_GALAGA
      MCH_GALAGA,
#endif
#ifdef ENABLE_DKONG
      MCH_DKONG,
#endif
#ifdef ENABLE_FROGGER
      MCH_FROGGER,
#endif
#ifdef ENABLE_DIGDUG
      MCH_DIGDUG,
#endif
#ifdef ENABLE_1942
      MCH_1942,
#endif
#ifdef ENABLE_INVADERS
      MCH_INVADERS,
#endif
#ifdef ENABLE_POOYAN
      MCH_POOYAN,
#endif
#ifdef ENABLE_TIMEPLT
      MCH_TIMEPLT,
#endif
#ifdef ENABLE_XEVIOUS
      MCH_XEVIOUS,
#endif
#ifdef ENABLE_SCOBRA
      MCH_SCOBRA,
#endif
#ifdef ENABLE_SCRAMBLE
      MCH_SCRAMBLE,
#endif
#ifdef ENABLE_DKONGJR
      MCH_DKONGJR,
#endif
      MCH_LAST
};

#define MACHINES  (MCH_LAST-1)
#endif

// wrapper around machine specific code sections allowing to compile only
// code in that's needed for enabled machines

#ifdef ENABLE_PACMAN
  #ifdef SINGLE_MACHINE
    #define MACHINE_IS_PACMAN  1
    #define PACMAN_BEGIN
    #define PACMAN_END
  #else
    /* Ms. Pac-Man shares Pac-Man's render/frame path — treat it as a
     * Pac-Man variant for the render dispatch. F2 board bank switching
     * lives in pacman_RdZ80/OpZ80_INL, gated on the specific MCH value. */
    #ifdef ENABLE_MSPACMAN
      #define MACHINE_IS_PACMAN  (machine == MCH_PACMAN || machine == MCH_MSPACMAN)
      #define PACMAN_BEGIN  if(machine == MCH_PACMAN || machine == MCH_MSPACMAN) {
    #else
      #define MACHINE_IS_PACMAN  (machine == MCH_PACMAN)
      #define PACMAN_BEGIN  if(machine == MCH_PACMAN) {
    #endif
    #define PACMAN_END    }
  #endif
#endif

#ifdef ENABLE_GALAGA
  #ifdef SINGLE_MACHINE
    #define MACHINE_IS_GALAGA  1
    #define GALAGA_BEGIN
    #define GALAGA_END
  #else
    #define MACHINE_IS_GALAGA  (machine == MCH_GALAGA)
    // galaga is not the first if pacman is enabled
    #ifdef ENABLE_PACMAN
      // donkey kong, frogger, digdug or 1942 may come afterwards
      #if defined(ENABLE_DKONG) || defined(ENABLE_FROGGER) || defined(ENABLE_DIGDUG) || defined(ENABLE_1942)
        #define GALAGA_BEGIN  else if(machine == MCH_GALAGA) {
      #else
        #define GALAGA_BEGIN  else {
      #endif
    #else
      #define GALAGA_BEGIN  if(machine == MCH_GALAGA) {
    #endif
    #define GALAGA_END    }
  #endif
#endif

#ifdef ENABLE_DKONG
  #ifdef SINGLE_MACHINE
    #define MACHINE_IS_DKONG  1
    #define DKONG_BEGIN
    #define DKONG_END
  #else
    /* Donkey Kong Jr. runs on the DK code (dkong.h) */
    #ifdef ENABLE_DKONGJR
      #define MACHINE_IS_DKONG  (machine == MCH_DKONG || machine == MCH_DKONGJR)
    #else
      #define MACHINE_IS_DKONG  (machine == MCH_DKONG)
    #endif
    // dkong is not the first if pacman or galaga are enabled
    #if defined(ENABLE_PACMAN) || defined(ENABLE_GALAGA)
      #define DKONG_BEGIN  else if(MACHINE_IS_DKONG) {
    #else
      #define DKONG_BEGIN  if(MACHINE_IS_DKONG) {
    #endif
    #define DKONG_END    }
  #endif
#endif

#ifdef ENABLE_FROGGER
  #ifdef SINGLE_MACHINE
    #define MACHINE_IS_FROGGER  1
    #define FROGGER_BEGIN
    #define FROGGER_END
  #else
    #define MACHINE_IS_FROGGER  (machine == MCH_FROGGER)
    // frogger is not the first if pacman, galaga or dkong are enabled
    #if defined(ENABLE_PACMAN) || defined(ENABLE_GALAGA) || defined(ENABLE_DKONG)
      // digdug or 1942 may come afterwards
      #if defined(ENABLE_DIGDUG) || defined(ENABLE_1942)
        #define FROGGER_BEGIN  else if(machine == MCH_FROGGER) {
      #else
        #define FROGGER_BEGIN  else {
      #endif
    #else
      #define FROGGER_BEGIN  if(machine == MCH_FROGGER) {
    #endif
    #define FROGGER_END    } 
  #endif
#endif

#ifdef ENABLE_DIGDUG
  #ifdef SINGLE_MACHINE
    #define MACHINE_IS_DIGDUG  1
    #define DIGDUG_BEGIN
    #define DIGDUG_END
  #else
    #define MACHINE_IS_DIGDUG  (machine == MCH_DIGDUG)
    // digdug is not the first if pacman, galaga, dkong or frogger are enabled
    #if defined(ENABLE_PACMAN) || defined(ENABLE_GALAGA) || defined(ENABLE_DKONG) || defined(ENABLE_FROGGER)
      // 1942 and the GalaginoPlus ports may come afterwards
      #define DIGDUG_BEGIN  else if(machine == MCH_DIGDUG) {
    #else
      #define DIGDUG_BEGIN  if(machine == MCH_DIGDUG) {
    #endif
    #define DIGDUG_END    } 
  #endif
#endif

#ifdef ENABLE_1942
  #ifdef SINGLE_MACHINE
    #define MACHINE_IS_1942  1
    #define _1942_BEGIN
    #define _1942_END
  #else
    #define MACHINE_IS_1942  (machine == MCH_1942)
    // digdug is never first and always last
    #define _1942_BEGIN  else {
    #define _1942_END    }
  #endif
#endif

/* Games ported from GalaginoPlus: multi-machine only, never first in the
 * chain (Pac-Man always comes before), so a plain else-if. */
#ifdef ENABLE_INVADERS
  #define MACHINE_IS_INVADERS  (machine == MCH_INVADERS)
  #define INVADERS_BEGIN  else if(machine == MCH_INVADERS) {
  #define INVADERS_END    }
#endif
#ifdef ENABLE_XEVIOUS
  #define MACHINE_IS_XEVIOUS  (machine == MCH_XEVIOUS)
  #define XEVIOUS_BEGIN  else if(machine == MCH_XEVIOUS) {
  #define XEVIOUS_END    }
#endif
#ifdef ENABLE_TIMEPLT
  #define MACHINE_IS_TIMEPLT  (machine == MCH_TIMEPLT)
  #define TIMEPLT_BEGIN  else if(machine == MCH_TIMEPLT) {
  #define TIMEPLT_END    }
#endif
#ifdef ENABLE_POOYAN
  #define MACHINE_IS_POOYAN  (machine == MCH_POOYAN)
  #define POOYAN_BEGIN  else if(machine == MCH_POOYAN) {
  #define POOYAN_END    }
#endif
#ifdef ENABLE_SCOBRA
  /* Super Cobra and Scramble share the driver (scobra.h) */
  #ifdef ENABLE_SCRAMBLE
    #define MACHINE_IS_SCOBRA  (machine == MCH_SCOBRA || machine == MCH_SCRAMBLE)
  #else
    #define MACHINE_IS_SCOBRA  (machine == MCH_SCOBRA)
  #endif
  #define SCOBRA_BEGIN  else if(MACHINE_IS_SCOBRA) {
  #define SCOBRA_END    }
#endif

#if defined(__cplusplus)
extern "C"
{
#endif
void prepare_emulation(void);
void emulate_frame(void);
extern unsigned char *memory;
extern char game_started;
#ifdef ENABLE_GALAGA
extern unsigned char starcontrol;
#endif
#if defined(ENABLE_GALAGA) || defined(ENABLE_PACMAN) || defined(ENABLE_DIGDUG) || defined(ENABLE_SCOBRA) || defined(ENABLE_POOYAN) || defined(ENABLE_TIMEPLT) || defined(ENABLE_XEVIOUS)
extern unsigned char soundregs[32];
#endif
#ifndef SINGLE_MACHINE
extern signed char machine;
extern signed char menu_sel;
#endif
#ifdef ENABLE_DKONG
extern unsigned char colortable_select;
extern void audio_dkong_bitrate(char);
#endif

#ifndef SINGLE_MACHINE
extern void emulation_reset(void);

#ifdef MASTER_ATTRACT_MENU_TIMEOUT
extern unsigned long master_attract_timeout;
#endif
#endif

// external functions called by emulation
#ifdef ENABLE_GALAGA
extern void galaga_trigger_sound_explosion(void);
#endif
#ifdef ENABLE_DKONG
extern void dkong_trigger_sound(char);   // 0..15 = sfx, 16.. = mus
#endif
extern unsigned char buttons_get(void);
#if defined(__cplusplus)
}
#endif

/* -------- the following is used by the Z80 core's opcode fetch -------- */
extern char current_cpu;
#ifdef ENABLE_PACMAN
/* Runtime pointer — matches the extern "C" definition in pacman_runtime.cpp. */
#ifdef __cplusplus
extern "C" unsigned char *pacman_rom;
#else
extern unsigned char *pacman_rom;
#endif
#endif
#ifdef ENABLE_GALAGA
/* Runtime pointers — see galaga_runtime.cpp. extern "C" linkage to
 * match the definitions. */
#ifdef __cplusplus
extern "C" {
#endif
extern unsigned char *galaga_rom_cpu1;
extern unsigned char *galaga_rom_cpu2;
extern unsigned char *galaga_rom_cpu3;
#ifdef __cplusplus
}
#endif
#endif
#ifdef ENABLE_DKONG
#ifdef __cplusplus
extern "C" {
#endif
extern unsigned char *dkong_rom_cpu1;
#ifdef __cplusplus
}
#endif
#endif
#ifdef ENABLE_FROGGER
#ifdef __cplusplus
extern "C" {
#endif
extern unsigned char *frogger_rom_cpu1;
extern unsigned char *frogger_rom_cpu2;
#ifdef __cplusplus
}
#endif
#endif
#ifdef ENABLE_DIGDUG
/* Runtime pointers — see digdug_runtime.cpp. */
extern unsigned char *digdug_rom_cpu1;
extern unsigned char *digdug_rom_cpu2;
extern unsigned char *digdug_rom_cpu3;
#endif
#ifdef ENABLE_INVADERS
#ifdef __cplusplus
extern "C" unsigned char *invaders_rom;
#else
extern unsigned char *invaders_rom;
#endif
#endif
#ifdef ENABLE_XEVIOUS
#ifdef __cplusplus
extern "C" {
#endif
extern unsigned char *xevious_rom_cpu1, *xevious_rom_cpu2, *xevious_rom_cpu3;
#ifdef __cplusplus
}
#endif
#endif
#ifdef ENABLE_TIMEPLT
#ifdef __cplusplus
extern "C" {
#endif
extern unsigned char *timeplt_rom_cpu1;
extern unsigned char *timeplt_rom_cpu2;
#ifdef __cplusplus
}
#endif
#endif
#ifdef ENABLE_POOYAN
#ifdef __cplusplus
extern "C" {
#endif
extern unsigned char *pooyan_rom_cpu1;
extern unsigned char *pooyan_rom_cpu2;
#ifdef __cplusplus
}
#endif
#endif
#ifdef ENABLE_SCOBRA
#ifdef __cplusplus
extern "C" {
#endif
extern unsigned char *scobra_rom_cpu1;
extern unsigned char *scobra_rom_cpu2;
#ifdef __cplusplus
}
#endif
#endif
#ifdef ENABLE_1942
extern unsigned char _1942_bank;
extern const unsigned char _1942_rom_cpu1[];
extern const unsigned char _1942_rom_cpu2[];
extern const unsigned char _1942_rom_cpu1_b0[];
extern const unsigned char _1942_rom_cpu1_b1[];
extern const unsigned char _1942_rom_cpu1_b2[];
#endif

#define NONE  ((const unsigned char *)0l)

static inline byte OpZ80_INL(register word Addr) {
#ifdef ENABLE_1942
  static const unsigned char *_1942_bank_table[] = {
    _1942_rom_cpu1_b0, _1942_rom_cpu1_b1, _1942_rom_cpu1_b2 };
#endif

#if defined(SINGLE_MACHINE) && defined(ENABLE_PACMAN)
  /* Runtime pointer — pacman_rom populated by runtime_load_pacman().
     Bypass the static rom_table (which requires constant initializers).
     Mask Addr to 14 bits (16 KB Pac-Man ROM). */
  return pacman_rom[Addr & 0x3FFF];
#elif defined(SINGLE_MACHINE) && defined(ENABLE_GALAGA)
  /* Runtime pointer path for Galaga — 3 Z80 CPUs, cpu1 is 16 KB,
     cpu2/cpu3 are 4 KB each. Halt with a clear message if the ROMs
     weren't loaded (SD missing / ZIP not found). */
  unsigned char *rom = (current_cpu == 0) ? galaga_rom_cpu1
                     : (current_cpu == 1) ? galaga_rom_cpu2
                     :                      galaga_rom_cpu3;
  unsigned int mask = (current_cpu == 0) ? 0x3FFFu : 0x0FFFu;
  return rom[Addr & mask];
#else
#ifndef SINGLE_MACHINE
  /* Multi-machine mode. Every opcode and operand byte comes through here,
   * so the hot path is one test and one indexed load: the running machine's
   * per-CPU ROM pointer and address mask are looked up once, on the first
   * fetch (`machine` is fixed by then and never changes in a session — the
   * game list relaunches the firmware). Machines that don't fetch from a
   * plain ROM (Ms. Pac-Man's aux board, Space Invaders, 1942's banks) take
   * the slower path. */
  static unsigned char *fetch_rom[3];
  static unsigned short fetch_mask[3];
  static unsigned char fetch_mode = 0xff;        /* 0 = plain ROM, 1 = special, 0xff = not set up */
  if (fetch_mode == 0) return fetch_rom[(int)current_cpu][Addr & fetch_mask[(int)current_cpu]];
  if (fetch_mode == 0xff) {
    fetch_mode = 0;
    for (int c = 0; c < 3; c++) { fetch_rom[c] = 0; fetch_mask[c] = 0x3FFFu; }
    switch (machine) {
#ifdef ENABLE_PACMAN
      case MCH_PACMAN: fetch_rom[0] = pacman_rom; break;
#endif
#ifdef ENABLE_GALAGA
      case MCH_GALAGA:     /* cpu1 16K, cpu2 / cpu3 4K */
        fetch_rom[0] = galaga_rom_cpu1; fetch_rom[1] = galaga_rom_cpu2; fetch_rom[2] = galaga_rom_cpu3;
        fetch_mask[1] = fetch_mask[2] = 0x0FFFu;
        break;
#endif
#ifdef ENABLE_FROGGER
      case MCH_FROGGER:    /* cpu1 padded to 16K, cpu2 to 8K */
        fetch_rom[0] = frogger_rom_cpu1; fetch_rom[1] = frogger_rom_cpu2; fetch_mask[1] = 0x1FFFu;
        break;
#endif
#ifdef ENABLE_DKONG
      case MCH_DKONG: fetch_rom[0] = dkong_rom_cpu1; break;   /* cpu2 is the i8048 */
#endif
#ifdef ENABLE_DKONGJR
      case MCH_DKONGJR: fetch_rom[0] = dkong_rom_cpu1; fetch_mask[0] = 0x7FFFu; break;   /* 24K in 32K */
#endif
#ifdef ENABLE_DIGDUG
      case MCH_DIGDUG:     /* (digdug_StepZ80 fetches itself; kept for safety) */
        fetch_rom[0] = digdug_rom_cpu1; fetch_rom[1] = digdug_rom_cpu2; fetch_rom[2] = digdug_rom_cpu3;
        fetch_mask[1] = 0x1FFFu; fetch_mask[2] = 0x0FFFu;
        break;
#endif
#ifdef ENABLE_XEVIOUS
      case MCH_XEVIOUS:    /* 16K / 8K / 4K */
        fetch_rom[0] = xevious_rom_cpu1; fetch_rom[1] = xevious_rom_cpu2; fetch_rom[2] = xevious_rom_cpu3;
        fetch_mask[1] = 0x1FFFu; fetch_mask[2] = 0x0FFFu;
        break;
#endif
#ifdef ENABLE_TIMEPLT
      case MCH_TIMEPLT:    /* 24K in 32K, 4K */
        fetch_rom[0] = timeplt_rom_cpu1; fetch_rom[1] = timeplt_rom_cpu2;
        fetch_mask[0] = 0x7FFFu; fetch_mask[1] = 0x0FFFu;
        break;
#endif
#ifdef ENABLE_POOYAN
      case MCH_POOYAN:     /* 32K, 8K */
        fetch_rom[0] = pooyan_rom_cpu1; fetch_rom[1] = pooyan_rom_cpu2;
        fetch_mask[0] = 0x7FFFu; fetch_mask[1] = 0x1FFFu;
        break;
#endif
#ifdef ENABLE_SCOBRA
      case MCH_SCOBRA:     /* padded to 32K, 8K */
#ifdef ENABLE_SCRAMBLE
      case MCH_SCRAMBLE:
#endif
        fetch_rom[0] = scobra_rom_cpu1; fetch_rom[1] = scobra_rom_cpu2;
        fetch_mask[0] = 0x7FFFu; fetch_mask[1] = 0x1FFFu;
        break;
#endif
      default: fetch_mode = 1; break;   /* Ms. Pac-Man, Space Invaders, 1942 */
    }
    if (fetch_mode == 0) return fetch_rom[(int)current_cpu][Addr & fetch_mask[(int)current_cpu]];
  }
#ifdef ENABLE_MSPACMAN
  /* Ms. Pac-Man opcode fetch routes through the F2 aux-board decoder.
   * It handles ENABLE/DISABLE traps and picks between pacrom/auxrom/
   * pacman_rom based on the runtime decode flag. */
  extern unsigned char mspacman_fetch(unsigned short Addr);
  if(machine == MCH_MSPACMAN) return mspacman_fetch(Addr);
#endif
#ifdef ENABLE_INVADERS
  /* 8 KB ROM, then RAM (14 address bits, mirrored above 0x4000) */
  if(machine == MCH_INVADERS) {
    Addr &= 0x3FFF;
    return (Addr < 0x2000) ? invaders_rom[Addr] : memory[Addr - 0x2000];
  }
#endif
#ifdef ENABLE_1942
  if(machine == MCH_1942 && (Addr & 0x8000))
    return _1942_bank_table[_1942_bank][Addr-0x8000];
#endif
  return 0xff;
#else
  static unsigned char *rom_table[3] = { 0, 0, 0 };
  return rom_table[current_cpu][Addr];
#endif
#endif  /* !(SINGLE_MACHINE && ENABLE_PACMAN) */
}

#endif // _EMULATION_H_

/* z80trace harness — runs Galagino's Galaga machine on the PC with either
 * Z80 core and writes a per-instruction trace, so two cores can be diffed
 * to find the first instruction that behaves differently.
 *
 *   trace_<core>.exe rom_dir frames out.bin
 *
 * Each record (little-endian, 20 bytes): cpu, kind (0=instr 1=int 2=nmi),
 * pc_before, then state AFTER: AF (F & 0xD7, undocumented bits masked),
 * BC, DE, HL, SP, IX, IY, iff1. Interrupt records carry the vector in
 * pc_before. Input is always 0, so runs are deterministic. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include "z80_core.h"

#undef StepZ80
#undef IntZ80
extern void StepZ80(Z80 *R);
extern void IntZ80(Z80 *R, word Vector);
extern char current_cpu;
extern void prepare_emulation(void);
extern void emulate_frame(void);

unsigned char *galaga_rom_cpu1, *galaga_rom_cpu2, *galaga_rom_cpu3;

static FILE *trace_out;

#pragma pack(push, 1)
typedef struct {
  uint8_t cpu, kind;
  uint16_t pc_before, af, bc, de, hl, sp, ix, iy;
  uint8_t iff1, pad;
} Rec;
#pragma pack(pop)

static void snap(Z80 *R, Rec *r) {
#ifdef Z80_CORE_ZAZU
  uint8_t f = (R->sf << 7) | (R->zf << 6) | (R->hf << 4) | (R->pf << 2) |
              (R->nf << 1) | (R->cf);
  r->af = (uint16_t)((R->a << 8) | f);
  r->bc = (R->b << 8) | R->c; r->de = (R->d << 8) | R->e; r->hl = (R->h << 8) | R->l;
  r->sp = R->sp; r->ix = R->ix; r->iy = R->iy; r->iff1 = R->iff1;
#else
  r->af = R->AF.W & 0xFFD7;
  r->bc = R->BC.W; r->de = R->DE.W; r->hl = R->HL.W;
  r->sp = R->SP.W; r->ix = R->IX.W; r->iy = R->IY.W; r->iff1 = (R->IFF & IFF_1) ? 1 : 0;
#endif
}

static uint16_t pc_of(Z80 *R) { return Z80_PC(R); }

/* TRACE_NOFLAGS=1 in the environment drops the F register from every
 * record, so plain `cmp` finds the first real (non-flag) divergence. */
static int no_flags = -1;

static void emit(Rec *r) {
  if (no_flags < 0) no_flags = getenv("TRACE_NOFLAGS") != NULL;
  if (no_flags) r->af &= 0xFF00;
  fwrite(r, sizeof *r, 1, trace_out);
}

void trace_StepZ80(Z80 *R) {
  Rec r = {0};
  r.cpu = (uint8_t)current_cpu; r.kind = 0; r.pc_before = pc_of(R);
  StepZ80(R);
  snap(R, &r);
  emit(&r);
}

void trace_IntZ80(Z80 *R, word Vector) {
  Rec r = {0};
  r.cpu = (uint8_t)current_cpu;
  r.kind = (Vector == INT_NMI) ? 2 : 1;
  r.pc_before = Vector;
  IntZ80(R, Vector);
  snap(R, &r);
  emit(&r);
}

/* ---- platform stubs ---- */
unsigned long micros(void) { return (unsigned long)clock(); }
unsigned long millis(void) { return (unsigned long)clock(); }
unsigned char buttons_get(void) { return 0; }
uint32_t ulTaskNotifyTake(int a, unsigned long b) { (void)a; (void)b; return 1; }
void vTaskDelay(unsigned long t) { (void)t; }
uint32_t esp_random(void) { return 0; }
void galaga_trigger_sound_explosion(void) {}
word LoopZ80(Z80 *R) { (void)R; return INT_NONE; }   /* old core's RunZ80 only */

static unsigned char *load(const char *dir, const char *name, size_t sz) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", dir, name);
  FILE *f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
  unsigned char *b = malloc(sz);
  if (fread(b, 1, sz, f) != sz) { fprintf(stderr, "short read %s\n", path); exit(1); }
  fclose(f);
  return b;
}

int main(int argc, char **argv) {
  if (argc < 4) { fprintf(stderr, "usage: %s rom_dir frames out.bin\n", argv[0]); return 2; }
  galaga_rom_cpu1 = load(argv[1], "galaga_cpu1.bin", 16384);
  galaga_rom_cpu2 = load(argv[1], "galaga_cpu2.bin", 4096);
  galaga_rom_cpu3 = load(argv[1], "galaga_cpu3.bin", 4096);
  int frames = atoi(argv[2]);
  trace_out = fopen(argv[3], "wb");
  if (!trace_out) { perror("out"); return 1; }
  prepare_emulation();
  for (int i = 0; i < frames; i++) emulate_frame();
  fclose(trace_out);
  printf("%d frames traced\n", frames);
  return 0;
}

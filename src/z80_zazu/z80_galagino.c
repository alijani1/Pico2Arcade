/* z80_galagino.c — adapter between Galagino and the superzazu Z80 core.
 * See z80_galagino.h for the interrupt model. StepZ80/digdug_StepZ80 live
 * at the end of zazu_z80.c so they can call the opcode executor directly. */
#include "z80_galagino.h"

/* Same power-on state the previous core used (PC=0, SP=F000, all
 * registers/flags 0, IM0, interrupts off), so games boot identically. */
void ResetZ80(Z80 *R)
{
  z80_init(R);
  R->pc = 0x0000;
  R->sp = 0xF000;
  R->ix = R->iy = 0;
  R->mem_ptr = 0;
  R->a = R->b = R->c = R->d = R->e = R->h = R->l = 0;
  R->a_ = R->b_ = R->c_ = R->d_ = R->e_ = R->h_ = R->l_ = R->f_ = 0;
  R->sf = R->zf = R->yf = R->hf = R->xf = R->pf = R->nf = R->cf = 0;
  R->i = R->r = 0;
  R->interrupt_mode = 0;
  R->iff1 = R->iff2 = 0;
  R->iff_delay = 0;
  R->halted = 0;
}

void IntZ80(Z80 *R, word Vector)
{
  if (Vector == INT_NMI) { z80_take_nmi(R); return; }
  R->halted = 0;                         /* released even if dropped */
  z80_take_int(R, (uint8_t)(Vector & 0xFF));
}

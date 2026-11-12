/* bp.h --- the Book Port machine: an M16C/62 running bpfw._BB, plus the
   board around it.  The CPU core is GDB's m32c simulator, compiled under
   rename.h so it does not collide with the copy inside librc8650.  */

#ifndef BP_H
#define BP_H

#include <stdint.h>
#include <stddef.h>

/* Main crystal.  U1BRG = 7 must give the 115200 baud the RC8650's real-time
   PCM mode requires, and 14.7456 MHz / (16 * 8) is exactly that.  */
#define BP_XIN_HZ 14745600u

/* Keypad.  Twelve telephone keys plus A-F below them.  */
enum
{
  BP_K1, BP_K2, BP_K3,
  BP_K4, BP_K5, BP_K6,
  BP_K7, BP_K8, BP_K9,
  BP_KSTAR, BP_K0, BP_KHASH,
  BP_KA, BP_KB, BP_KC,
  BP_KD, BP_KE, BP_KF,
  BP_NKEYS
};

typedef struct
{
  /* A byte has finished shifting out of UART ch (0-2).  */
  void (*uart_tx) (void *ctx, int ch, uint8_t b);
  /* Level of the RC8650's CTS# as the Book Port sees it on P6_0.
     Return 0 when the chip can take another byte.  */
  int (*rc_cts) (void *ctx);
  /* A byte the CPU wrote over I2C to a device other than the clock:
     dev is the 7-bit address, idx counts bytes after it (0 = subaddress).  */
  void (*i2c_write) (void *ctx, int dev, int idx, uint8_t b);
  void *ctx;
} bp_hooks;

/* Load the firmware directory (bpfw._BB, and bpfwex._BB etc. if present).
   Returns 0 on success, -1 with a message on stderr otherwise.  */
int bp_load (const char *fwdir, const bp_hooks *hooks);
void bp_reset (void);

/* Run the machine for this many main-crystal cycles.  */
void bp_run (uint64_t xin_cycles);

/* A byte arriving on UART ch's RXD.  */
void bp_uart_rx (int ch, uint8_t b);

void bp_set_key (int key, int down);

/* The keypad as wired: a 4 x 5 matrix, position = row * 5 + column, rows
   on P7_4-P7_7 and columns on P10_3-P10_7.  bp_key_pos maps each key to
   its position.  */
#define BP_MATRIX 20
extern const int bp_key_pos[BP_NKEYS];
void bp_set_matrix (int pos, int down);

/* Main-crystal cycles since reset.  */
uint64_t bp_cycles (void);
uint32_t bp_pc (void);

/* Drive input pins: bits in mask of port p take the level in value.  */
void bp_set_pins (int p, uint8_t mask, uint8_t value);

/* Read memory as the CPU sees it, without side effects on SFRs.  */
uint8_t bp_peek (uint32_t addr);

/* Debug logging: bitmask of BP_LOG_*.  */
enum
{
  BP_LOG_SFR = 1,		/* first write of each SFR, and every port write */
  BP_LOG_EXT = 2,		/* accesses outside internal RAM/ROM */
  BP_LOG_INT = 4,		/* interrupts taken */
  BP_LOG_UART = 8,		/* UART traffic */
  BP_LOG_PC = 16,		/* periodic PC samples */
  BP_LOG_TRACE = 32,		/* every instruction's PC (huge) */
  BP_LOG_SFRALL = 64,		/* every SFR read and write (huge) */
};
extern int bp_log;

#endif

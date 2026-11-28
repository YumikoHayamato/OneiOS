// SPDX-License-Identifier: MIT
// What vsync_lock.h's seqlock becomes on arm64 (make check-order compiles this with the NDK to assembly and tests/check_order.sh
// reads where the barriers are): the x86 host tests cannot show a reordering, the instructions can.
#include <stdint.h>
#include "../../../kernel/patches/oneios-hosted/display/vsync_lock.h"

int probe_read(const volatile uint64_t *words, struct oneios_vsync_panel *out) { return oneios_vsync_read(words, out); }
void probe_write(volatile uint64_t *words, const struct oneios_vsync_panel *in) { oneios_vsync_write(words, in); }
// the guest's record, as OneiOSDisplay.cpp writes it
void probe_guest(volatile uint64_t *mine, uint64_t now, uint64_t sent)
{
  const uint64_t seq = oneios_rec_begin(&mine[ONEIOS_TICK_SEQ]);
  __atomic_store_n(&mine[ONEIOS_TICK_AT], now, __ATOMIC_RELAXED);
  __atomic_store_n(&mine[ONEIOS_TICK_SENT], sent, __ATOMIC_RELAXED);
  oneios_rec_end(&mine[ONEIOS_TICK_SEQ], seq);
}

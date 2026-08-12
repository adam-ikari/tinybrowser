#include "tb.h"
#include <time.h>

static uint64_t real_now_ms(const tb_clock *self) {
  (void)self;
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

const tb_clock tb_clock_real = { real_now_ms };

#pragma once
// Host stub: the decoder stamps packets with timer_hw->timerawl; the test ignores it.
#include <cstdint>
struct sim_timer_hw_t { volatile uint32_t timerawl; };
static sim_timer_hw_t sim_timer_hw_inst = { 0 };
#define timer_hw (&sim_timer_hw_inst)

// hal/cpu/timer.h
#ifndef TIMER_H
#define TIMER_H

typedef unsigned char uint8_t;
typedef unsigned int uint32_t;

void init_timer(uint32_t freq);
uint32_t get_tick_count(void);
void timer_wait(int ticks); // Added

// Global tick counter (incremented at timer frequency, e.g. 50Hz)
extern uint32_t timer_ticks;

// Alias for compatibility
#define timer_get_ticks get_tick_count

// Sleep for milliseconds
void timer_sleep(int ms);

#endif
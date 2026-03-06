// hal/cpu/timer.h
#ifndef TIMER_H
#define TIMER_H

typedef unsigned char uint8_t;
typedef unsigned int uint32_t;

void init_timer(uint32_t freq);
uint32_t get_tick_count(void);
void timer_wait(int ticks); // Added

// Alias for compatibility
#define timer_get_ticks get_tick_count

// Sleep for milliseconds
void timer_sleep(int ms);

#endif
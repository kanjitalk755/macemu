/*
 *  timer_interrupt.h - 60Hz timer interface (polling-based)
 */

#ifndef TIMER_INTERRUPT_H
#define TIMER_INTERRUPT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 *  Initialize timer system
 */
void setup_timer_interrupt(void);

/*
 *  Poll timer - call from CPU execution loop
 *  Returns number of timer expirations (0 if not ready, 1 if fired)
 */
uint64_t poll_timer_interrupt(void);

/*
 *  Stop timer
 */
void stop_timer_interrupt(void);

/*
 *  Get statistics
 */
uint64_t get_timer_interrupt_count(void);

#ifdef __cplusplus
}
#endif

#endif // TIMER_INTERRUPT_H

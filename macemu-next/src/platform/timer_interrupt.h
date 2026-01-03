/*
 *  timer_interrupt.h - 60Hz timer interrupt interface
 */

#ifndef TIMER_INTERRUPT_H
#define TIMER_INTERRUPT_H

#include <stdint.h>
#include <stdbool.h>

/*
 *  Setup periodic timer interrupt
 *
 *  interval_us: Interrupt interval in microseconds
 *               Use 16667 for 60Hz (Mac VBL rate)
 *
 *  Returns: true on success, false on failure
 */
bool setup_timer_interrupt(int interval_us);

/*
 *  Stop timer interrupt
 */
void stop_timer_interrupt(void);

/*
 *  Get number of timer interrupts fired
 *  (Useful for debugging/statistics)
 */
uint64_t get_timer_interrupt_count(void);

#endif // TIMER_INTERRUPT_H

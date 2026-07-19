#ifndef RED_ETS_INTEGRATION_H
#define RED_ETS_INTEGRATION_H

/*
 * Minimal integration contract
 * ============================
 *
 * 1. Add every red_extension/*.c file to the Red Keil target and add this
 *    directory to the include path.  The core is C99 and has no DriverLib
 *    include dependency.
 *
 * 2. In the existing Red BSP, implement red_ets_port_ops_t:
 *      now_ms                 existing monotonic millisecond tick
 *      period_capture_begin  comparator -> timer capture of N periods
 *      phase_capture_begin   timer compare -> ADC -> DMA for one phase
 *      capture_cancel        stop only ETS-owned requests/events
 *      restore_direct_mode   restore the original direct ADC/timer/DMA setup
 *      send_message          enqueue one existing Blue-Red protocol message
 *
 *    Do not wait in a callback.  Signal completion with the two public
 *    red_ets_*_capture_complete() hooks from the DMA/timer completion path.
 *
 * 3. Supply storage from existing mutually-exclusive Red work areas:
 *      period_ticks[64]      256 bytes
 *      phase_sum[512]        2048 bytes
 *      phase_count[512]      1024 bytes
 *      delay_ticks[512]      1024 bytes
 *      uniform_q15[512]      1024 bytes
 *      spectrum_ppm[32]      128 bytes
 *
 * 4. Call red_ets_poll() from the main super-loop.  Route only command 0x69
 *    to red_ets_handle_start().  Existing command 0x60 remains direct mode.
 *    Before starting direct mode call red_ets_abort_for_direct_measurement();
 *    poll until red_ets_is_busy() is false, then start the direct capture.
 *
 * 5. Capability HELLO bit6 must be set only when RED_ENABLE_ETS is 1.
 *    Commands 0x6A..0x6F are emitted by this module.  The existing transport
 *    remains responsible for UART framing queues, ACK timeout and retry.
 *
 * 6. Resource identifiers may be supplied without editing the extension:
 *
 *      RED_ETS_TIMER_RESOURCE
 *      RED_ETS_ADC_RESOURCE
 *      RED_ETS_DMA_RESOURCE
 *      RED_ETS_COMPARATOR_RESOURCE
 *      RED_ETS_EVENT_ROUTE_RESOURCE
 *
 *    Runtime values (clock, ADC rate, timeouts, bins, averaging and frequency
 *    range) live in red_ets_runtime_config_t and may be changed per board.
 *
 * 7. If the Red project already has an optimized Q15 real FFT, register a
 *    compatible adapter with red_ets_set_fft_backend().  Then compile with
 *    RED_ETS_ENABLE_REFERENCE_DFT=0 to remove the portable float DFT.
 */

#include "red_ets.h"

#endif

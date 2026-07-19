#ifndef RED_ETS_PORT_H
#define RED_ETS_PORT_H

#include <stddef.h>
#include <stdint.h>
#include "red_ets_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Platform operations are asynchronous.  A begin function only arms hardware;
 * completion is returned to the core with red_ets_period_capture_complete() or
 * red_ets_phase_capture_complete().  ISR code therefore remains in the BSP.
 */
typedef struct {
    uint32_t (*now_ms)(void *user);
    int (*frontend_settle_begin)(void *user);
    int (*period_capture_begin)(uint32_t *period_ticks,
                                size_t requested_count,
                                uint32_t timeout_ms,
                                void *user);
    int (*phase_capture_begin)(uint16_t delay_ticks,
                               uint16_t cycle_divider,
                               uint8_t averages,
                               uint16_t clip_low_code,
                               uint16_t clip_high_code,
                               uint32_t timeout_ms,
                               void *user);
    void (*capture_cancel)(void *user);
    void (*restore_direct_mode)(void *user);
    int (*send_message)(uint8_t command,
                        uint8_t flags,
                        uint16_t sequence,
                        const uint8_t *payload,
                        uint16_t payload_length,
                        void *user);
    int (*direct_measurement_busy)(void *user);
    void *user;
} red_ets_port_ops_t;

/* Optional resource aliases for a thin MSPM0 BSP adapter. */
#define RED_ETS_PORT_TIMER_RESOURCE       RED_ETS_TIMER_RESOURCE
#define RED_ETS_PORT_ADC_RESOURCE         RED_ETS_ADC_RESOURCE
#define RED_ETS_PORT_DMA_RESOURCE         RED_ETS_DMA_RESOURCE
#define RED_ETS_PORT_COMPARATOR_RESOURCE  RED_ETS_COMPARATOR_RESOURCE
#define RED_ETS_PORT_EVENT_ROUTE_RESOURCE RED_ETS_EVENT_ROUTE_RESOURCE

#ifdef __cplusplus
}
#endif
#endif

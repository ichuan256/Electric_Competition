#ifndef RED_ETS_H
#define RED_ETS_H

#include "red_ets_capture.h"
#include "red_ets_fft.h"
#include "red_ets_port.h"
#include "red_ets_protocol.h"
#include "red_ets_reconstruct.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    red_ets_runtime_config_t config;
    red_ets_workspace_t workspace;
    red_ets_port_ops_t port;
    red_ets_fft_backend_t fft_backend;
    void *fft_user;
    red_ets_request_t request;
    red_ets_capture_plan_t plan;
    red_ets_result_t result;
    red_ets_state_t state;
    red_ets_status_t last_status;
    uint32_t quality_flags;
    uint32_t start_ms;
    uint32_t state_enter_ms;
    uint32_t last_status_ms;
    uint32_t last_request_token;
    uint16_t sequence;
    uint16_t last_sequence;
    uint16_t phase_index;
    uint16_t publish_offset;
    uint16_t publish_chunks;
    uint16_t error_code;
    uint8_t publish_stage;
    uint8_t capture_armed;
    uint8_t last_request_valid;
    uint8_t status_sent;
    uint8_t failed_state;
    int32_t waveform_minimum_uv;
    int32_t waveform_maximum_uv;
} red_ets_context_t;

int red_ets_init(red_ets_context_t *context,
                 const red_ets_runtime_config_t *config,
                 const red_ets_workspace_t *workspace,
                 const red_ets_port_ops_t *port);

void red_ets_set_fft_backend(red_ets_context_t *context,
                             red_ets_fft_backend_t backend,
                             void *backend_user);

void red_ets_set_waveform_limits_uv(red_ets_context_t *context,
                                    int32_t minimum_uv,
                                    int32_t maximum_uv);

/* Accepts the exact 16-byte ETS_START payload and emits the common ACK. */
int red_ets_handle_start(red_ets_context_t *context,
                         uint16_t sequence,
                         const uint8_t *payload,
                         uint16_t payload_length);

/* Call frequently from the main loop; it never blocks waiting for hardware. */
void red_ets_poll(red_ets_context_t *context);

/* Completion hooks called by the Red BSP after DMA/timer operations finish. */
int red_ets_period_capture_complete(red_ets_context_t *context,
                                    size_t captured_periods);

int red_ets_phase_capture_complete(red_ets_context_t *context,
                                   uint32_t sample_sum,
                                   uint16_t valid_samples,
                                   int clipped);

void red_ets_capture_failed(red_ets_context_t *context,
                            red_ets_status_t reason);

/* Direct measurement has priority and may abort ETS without sharing results. */
void red_ets_abort_for_direct_measurement(red_ets_context_t *context);

/* Idempotent. Always restores the direct-mode ADC/timer/DMA configuration. */
void red_ets_release(red_ets_context_t *context);

int red_ets_is_busy(const red_ets_context_t *context);
red_ets_state_t red_ets_get_state(const red_ets_context_t *context);
const red_ets_result_t *red_ets_get_result(const red_ets_context_t *context);

#ifdef __cplusplus
}
#endif
#endif

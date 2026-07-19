#ifndef RED_ETS_TYPES_H
#define RED_ETS_TYPES_H

#include <stddef.h>
#include <stdint.h>
#include "red_ets_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RED_ETS_OK = 0,
    RED_ETS_E_ARGUMENT = -1,
    RED_ETS_E_DISABLED = -2,
    RED_ETS_E_BUSY = -3,
    RED_ETS_E_STATE = -4,
    RED_ETS_E_CAPACITY = -5,
    RED_ETS_E_PROTOCOL = -6,
    RED_ETS_E_CRC = -7,
    RED_ETS_E_TIMEOUT = -8,
    RED_ETS_E_TRIGGER = -9,
    RED_ETS_E_RANGE = -10,
    RED_ETS_E_CAPTURE = -11,
    RED_ETS_E_RECONSTRUCT = -12,
    RED_ETS_E_ANALYSIS = -13,
    RED_ETS_E_ABORTED = -14
} red_ets_status_t;

typedef enum {
    RED_ETS_STATE_IDLE = 0,
    RED_ETS_STATE_START_RECEIVED,
    RED_ETS_STATE_FRONTEND_SETTLE,
    RED_ETS_STATE_TRIGGER_ACQUIRE,
    RED_ETS_STATE_PERIOD_VALIDATE,
    RED_ETS_STATE_PLAN_PHASES,
    RED_ETS_STATE_PHASE_CAPTURE,
    RED_ETS_STATE_PHASE_VALIDATE,
    RED_ETS_STATE_RESAMPLE,
    RED_ETS_STATE_FFT,
    RED_ETS_STATE_RESULT_VALIDATE,
    RED_ETS_STATE_PUBLISH,
    RED_ETS_STATE_RELEASE,
    RED_ETS_STATE_DONE,
    RED_ETS_STATE_ERROR
} red_ets_state_t;

enum {
    RED_ETS_QUALITY_VALID                         = 1u << 0,
    RED_ETS_QUALITY_TRIGGER_MISSING               = 1u << 1,
    RED_ETS_QUALITY_TRIGGER_JITTER_HIGH           = 1u << 2,
    RED_ETS_QUALITY_PHASE_COVERAGE_INCOMPLETE     = 1u << 3,
    RED_ETS_QUALITY_SIGNAL_CHANGED                = 1u << 4,
    RED_ETS_QUALITY_ANALOG_BW_UNCALIBRATED        = 1u << 5,
    RED_ETS_QUALITY_ALIAS_OR_HARMONIC_AMBIGUOUS   = 1u << 6,
    RED_ETS_QUALITY_TIMER_QUANTIZATION_HIGH       = 1u << 7,
    RED_ETS_QUALITY_INPUT_CLIPPED                 = 1u << 8,
    RED_ETS_QUALITY_LOW_SNR                       = 1u << 9,
    RED_ETS_QUALITY_FUNDAMENTAL_OUT_OF_RANGE      = 1u << 10,
    RED_ETS_QUALITY_MISSING_PHASE_BINS            = 1u << 11,
    RED_ETS_QUALITY_FRONTEND_NOT_SETTLED          = 1u << 12
};

enum {
    RED_ETS_ERROR_DISABLED                    = 0x0201u,
    RED_ETS_ERROR_TRIGGER_TIMEOUT             = 0x0202u,
    RED_ETS_ERROR_TRIGGER_UNSTABLE            = 0x0203u,
    RED_ETS_ERROR_FREQUENCY_OUT_OF_RANGE      = 0x0204u,
    RED_ETS_ERROR_PHASE_CAPTURE_FAILED        = 0x0205u,
    RED_ETS_ERROR_PHASE_COVERAGE_FAILED       = 0x0206u,
    RED_ETS_ERROR_SIGNAL_NOT_PERIODIC         = 0x0207u,
    RED_ETS_ERROR_ANALOG_BANDWIDTH_INVALID    = 0x0208u,
    RED_ETS_ERROR_ABORTED_BY_DIRECT_MEASURE   = 0x0209u
};

enum {
    RED_ETS_FEATURE_WAVEFORM = 1u << 0,
    RED_ETS_FEATURE_SPECTRUM = 1u << 1,
    RED_ETS_FEATURE_THD_1_TO_5 = 1u << 2
};

typedef struct {
    uint32_t timer_clock_hz;
    uint32_t adc_max_rate_hz;
    uint32_t min_fundamental_hz;
    uint32_t max_fundamental_hz;
    uint32_t calibrated_analog_bandwidth_hz;
    uint32_t jitter_limit_ppm;
    uint16_t max_phase_bins;
    uint16_t period_samples;
    uint16_t period_discard;
    uint16_t frontend_settle_ms;
    uint16_t trigger_timeout_ms;
    uint16_t phase_timeout_ms;
    uint16_t default_deadline_ms;
    uint16_t adc_clip_low_code;
    uint16_t adc_clip_high_code;
    uint8_t default_averages;
    uint8_t default_harmonic_limit;
} red_ets_runtime_config_t;

typedef struct {
    uint32_t request_token;
    uint32_t max_fundamental_hz;
    uint16_t requested_phase_bins;
    uint16_t deadline_ms;
    uint8_t mode;
    uint8_t features;
    uint8_t averages_per_phase;
    uint8_t harmonic_limit;
} red_ets_request_t;

typedef struct {
    uint32_t *period_ticks;
    size_t period_capacity;
    uint32_t *phase_sum;
    uint16_t *phase_count;
    uint16_t *delay_ticks;
    size_t phase_capacity;
    int16_t *uniform_q15;
    size_t uniform_capacity;
    uint32_t *spectrum_ppm;
    size_t spectrum_capacity;
} red_ets_workspace_t;

typedef struct {
    uint32_t median_period_ticks;
    uint32_t fundamental_millihz;
    uint32_t jitter_ppm;
    uint32_t equivalent_sample_rate_hz;
    uint16_t phase_bins;
    uint16_t fft_bins;
    uint16_t cycle_divider;
    uint8_t averages_per_phase;
} red_ets_capture_plan_t;

typedef struct {
    uint32_t fundamental_millihz;
    uint32_t thd_1_to_5_ppm;
    uint32_t adc_max_rate_hz;
    uint32_t equivalent_sample_rate_hz;
    uint32_t timer_clock_hz;
    uint32_t calibrated_analog_bandwidth_hz;
    uint32_t quality_flags;
    uint16_t phase_bins;
    uint8_t averages_per_phase;
    uint8_t highest_valid_harmonic;
} red_ets_result_t;

#ifdef __cplusplus
}
#endif
#endif

#include "red_ets_capture.h"

#include <limits.h>
#include <string.h>

static uint32_t red_ets_isqrt_u64(uint64_t value)
{
    uint64_t bit = (uint64_t)1u << 62;
    uint64_t result = 0u;
    while (bit > value) {
        bit >>= 2;
    }
    while (bit != 0u) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)result;
}

static int red_ets_allowed_average(uint8_t value)
{
    return value == 1u || value == 2u || value == 4u ||
           value == 8u || value == 16u;
}

static uint16_t red_ets_next_fft_size(uint16_t phase_bins)
{
    uint16_t size = 64u;
    while (size < phase_bins && size < RED_ETS_MAX_PHASE_BINS) {
        size = (uint16_t)(size << 1);
    }
    return size;
}

void red_ets_runtime_config_defaults(red_ets_runtime_config_t *config)
{
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->timer_clock_hz = RED_ETS_TIMER_CLOCK_HZ_DEFAULT;
    config->adc_max_rate_hz = RED_ETS_ADC_MAX_RATE_HZ_DEFAULT;
    config->min_fundamental_hz = RED_ETS_MIN_FUNDAMENTAL_HZ_DEFAULT;
    config->max_fundamental_hz = RED_ETS_MAX_FUNDAMENTAL_HZ_DEFAULT;
    config->jitter_limit_ppm = RED_ETS_JITTER_LIMIT_PPM_DEFAULT;
    config->max_phase_bins = RED_ETS_MAX_PHASE_BINS;
    config->period_samples = RED_ETS_PERIOD_SAMPLES_DEFAULT;
    config->period_discard = RED_ETS_PERIOD_DISCARD_DEFAULT;
    config->frontend_settle_ms = RED_ETS_FRONTEND_SETTLE_MS_DEFAULT;
    config->trigger_timeout_ms = RED_ETS_TRIGGER_TIMEOUT_MS_DEFAULT;
    config->phase_timeout_ms = RED_ETS_PHASE_TIMEOUT_MS_DEFAULT;
    config->default_deadline_ms = RED_ETS_DEADLINE_MS_DEFAULT;
    config->adc_clip_low_code = RED_ETS_ADC_CLIP_LOW_CODE_DEFAULT;
    config->adc_clip_high_code = RED_ETS_ADC_CLIP_HIGH_CODE_DEFAULT;
    config->default_averages = RED_ETS_AVERAGES_DEFAULT;
    config->default_harmonic_limit = RED_ETS_HARMONIC_LIMIT_DEFAULT;
}

int red_ets_capture_validate_request(const red_ets_runtime_config_t *config,
                                     red_ets_request_t *request)
{
    if (config == NULL || request == NULL || config->timer_clock_hz == 0u ||
        config->adc_max_rate_hz == 0u || config->max_phase_bins == 0u) {
        return RED_ETS_E_ARGUMENT;
    }
    if (request->mode != 1u) {
        return RED_ETS_E_PROTOCOL;
    }
    if ((request->features & (uint8_t)~(RED_ETS_FEATURE_WAVEFORM |
                                        RED_ETS_FEATURE_SPECTRUM |
                                        RED_ETS_FEATURE_THD_1_TO_5)) != 0u) {
        return RED_ETS_E_PROTOCOL;
    }
    if (request->requested_phase_bins == 0u) {
        request->requested_phase_bins = config->max_phase_bins;
    }
    if (request->requested_phase_bins > config->max_phase_bins ||
        request->requested_phase_bins > RED_ETS_MAX_PHASE_BINS) {
        return RED_ETS_E_RANGE;
    }
    if (request->averages_per_phase == 0u) {
        request->averages_per_phase = config->default_averages;
    }
    if (!red_ets_allowed_average(request->averages_per_phase)) {
        return RED_ETS_E_RANGE;
    }
    if (request->harmonic_limit == 0u) {
        request->harmonic_limit = config->default_harmonic_limit;
    }
    if (request->harmonic_limit == 0u ||
        request->harmonic_limit >= RED_ETS_MAX_SPECTRUM_BINS) {
        return RED_ETS_E_RANGE;
    }
    if (request->deadline_ms == 0u) {
        request->deadline_ms = config->default_deadline_ms;
    }
    if (request->max_fundamental_hz == 0u ||
        request->max_fundamental_hz > config->max_fundamental_hz) {
        request->max_fundamental_hz = config->max_fundamental_hz;
    }
    return RED_ETS_OK;
}

int red_ets_capture_make_plan(const red_ets_runtime_config_t *config,
                              const red_ets_request_t *request,
                              uint32_t *period_ticks,
                              size_t period_count,
                              red_ets_capture_plan_t *plan,
                              uint32_t *quality_flags)
{
    size_t first;
    size_t usable;
    size_t i;
    size_t j;
    uint32_t median;
    uint64_t square_sum = 0u;
    uint32_t rms_ticks;
    uint64_t frequency_millihz;
    uint32_t frequency_hz;
    uint16_t phase_bins;

    if (config == NULL || request == NULL || period_ticks == NULL ||
        plan == NULL || quality_flags == NULL) {
        return RED_ETS_E_ARGUMENT;
    }
    first = config->period_discard;
    if (period_count <= first + 2u) {
        *quality_flags |= RED_ETS_QUALITY_TRIGGER_MISSING;
        return RED_ETS_E_TRIGGER;
    }
    usable = period_count - first;

    /* In-place insertion sort of the retained captures; no hidden scratch RAM. */
    for (i = first + 1u; i < period_count; ++i) {
        uint32_t key = period_ticks[i];
        j = i;
        while (j > first && period_ticks[j - 1u] > key) {
            period_ticks[j] = period_ticks[j - 1u];
            --j;
        }
        period_ticks[j] = key;
    }
    median = period_ticks[first + usable / 2u];
    if (median == 0u) {
        *quality_flags |= RED_ETS_QUALITY_TRIGGER_MISSING;
        return RED_ETS_E_TRIGGER;
    }

    for (i = first; i < period_count; ++i) {
        int64_t delta = (int64_t)period_ticks[i] - (int64_t)median;
        square_sum += (uint64_t)(delta * delta);
    }
    rms_ticks = red_ets_isqrt_u64(square_sum / usable);

    memset(plan, 0, sizeof(*plan));
    plan->median_period_ticks = median;
    plan->jitter_ppm = (uint32_t)(((uint64_t)rms_ticks * 1000000u + median / 2u) /
                                  median);
    if (plan->jitter_ppm > config->jitter_limit_ppm) {
        *quality_flags |= RED_ETS_QUALITY_TRIGGER_JITTER_HIGH;
        return RED_ETS_E_TRIGGER;
    }

    frequency_millihz = ((uint64_t)config->timer_clock_hz * 1000u + median / 2u) /
                         median;
    if (frequency_millihz > UINT32_MAX) {
        return RED_ETS_E_RANGE;
    }
    plan->fundamental_millihz = (uint32_t)frequency_millihz;
    frequency_hz = (uint32_t)((frequency_millihz + 500u) / 1000u);
    if (frequency_hz < config->min_fundamental_hz ||
        frequency_hz > request->max_fundamental_hz) {
        *quality_flags |= RED_ETS_QUALITY_FUNDAMENTAL_OUT_OF_RANGE;
        return RED_ETS_E_RANGE;
    }

    phase_bins = request->requested_phase_bins;
    if ((uint32_t)phase_bins > median) {
        phase_bins = (uint16_t)median;
    }
    if (phase_bins < 16u) {
        *quality_flags |= RED_ETS_QUALITY_TIMER_QUANTIZATION_HIGH;
        return RED_ETS_E_RANGE;
    }
    plan->phase_bins = phase_bins;
    plan->fft_bins = red_ets_next_fft_size(phase_bins);
    plan->averages_per_phase = request->averages_per_phase;
    plan->cycle_divider = (uint16_t)((frequency_hz + config->adc_max_rate_hz - 1u) /
                                     config->adc_max_rate_hz);
    if (plan->cycle_divider == 0u) {
        plan->cycle_divider = 1u;
    }
    plan->equivalent_sample_rate_hz = frequency_hz * phase_bins;
    return RED_ETS_OK;
}

int red_ets_capture_build_delays(const red_ets_capture_plan_t *plan,
                                 uint16_t *delay_ticks,
                                 size_t delay_capacity,
                                 uint32_t *quality_flags)
{
    uint16_t i;
    uint32_t previous = UINT32_MAX;
    if (plan == NULL || delay_ticks == NULL || quality_flags == NULL ||
        delay_capacity < plan->phase_bins || plan->phase_bins == 0u ||
        plan->median_period_ticks > UINT16_MAX) {
        return RED_ETS_E_ARGUMENT;
    }
    for (i = 0u; i < plan->phase_bins; ++i) {
        uint32_t delay = ((uint32_t)i * plan->median_period_ticks +
                          plan->phase_bins / 2u) / plan->phase_bins;
        if (delay >= plan->median_period_ticks) {
            delay = plan->median_period_ticks - 1u;
        }
        if (i != 0u && delay == previous) {
            *quality_flags |= RED_ETS_QUALITY_TIMER_QUANTIZATION_HIGH |
                              RED_ETS_QUALITY_MISSING_PHASE_BINS;
            return RED_ETS_E_RANGE;
        }
        delay_ticks[i] = (uint16_t)delay;
        previous = delay;
    }
    return RED_ETS_OK;
}

int red_ets_capture_store_phase(uint16_t phase_index,
                                uint32_t sample_sum,
                                uint16_t valid_samples,
                                int clipped,
                                uint32_t *phase_sum,
                                uint16_t *phase_count,
                                size_t phase_capacity,
                                uint32_t *quality_flags)
{
    if (phase_sum == NULL || phase_count == NULL || quality_flags == NULL ||
        phase_index >= phase_capacity || valid_samples == 0u) {
        if (quality_flags != NULL) {
            *quality_flags |= RED_ETS_QUALITY_MISSING_PHASE_BINS;
        }
        return RED_ETS_E_CAPTURE;
    }
    phase_sum[phase_index] = sample_sum;
    phase_count[phase_index] = valid_samples;
    if (clipped) {
        *quality_flags |= RED_ETS_QUALITY_INPUT_CLIPPED;
    }
    return RED_ETS_OK;
}

#include "red_ets_reconstruct.h"

#include <limits.h>

static int32_t red_ets_phase_average(const uint32_t *sum,
                                     const uint16_t *count,
                                     uint16_t index)
{
    return (int32_t)((sum[index] + count[index] / 2u) / count[index]);
}

static int32_t red_ets_interpolate_at(uint32_t target,
                                      const red_ets_capture_plan_t *plan,
                                      const uint16_t *delay,
                                      const uint32_t *sum,
                                      const uint16_t *count)
{
    uint16_t right;
    uint16_t left;
    uint32_t left_tick;
    uint32_t right_tick;
    int32_t left_value;
    int32_t right_value;
    int64_t numerator;

    for (right = 0u; right < plan->phase_bins; ++right) {
        if ((uint32_t)delay[right] >= target) {
            break;
        }
    }
    if (right == 0u) {
        left = (uint16_t)(plan->phase_bins - 1u);
        left_tick = delay[left];
        right_tick = (uint32_t)delay[0] + plan->median_period_ticks;
        if (target < left_tick) {
            target += plan->median_period_ticks;
        }
        left_value = red_ets_phase_average(sum, count, left);
        right_value = red_ets_phase_average(sum, count, 0u);
    } else if (right >= plan->phase_bins) {
        left = (uint16_t)(plan->phase_bins - 1u);
        left_tick = delay[left];
        right_tick = (uint32_t)delay[0] + plan->median_period_ticks;
        left_value = red_ets_phase_average(sum, count, left);
        right_value = red_ets_phase_average(sum, count, 0u);
    } else {
        left = (uint16_t)(right - 1u);
        left_tick = delay[left];
        right_tick = delay[right];
        left_value = red_ets_phase_average(sum, count, left);
        right_value = red_ets_phase_average(sum, count, right);
    }
    if (right_tick == left_tick) {
        return left_value;
    }
    numerator = (int64_t)(right_value - left_value) *
                (int64_t)(target - left_tick);
    return left_value + (int32_t)(numerator / (int64_t)(right_tick - left_tick));
}

int red_ets_reconstruct_uniform_q15(const red_ets_capture_plan_t *plan,
                                    const uint16_t *delay_ticks,
                                    const uint32_t *phase_sum,
                                    const uint16_t *phase_count,
                                    int16_t *uniform_q15,
                                    size_t uniform_capacity,
                                    uint32_t *quality_flags)
{
    uint16_t i;
    int64_t total = 0;
    int32_t mean;
    int32_t maximum_absolute = 0;

    if (plan == NULL || delay_ticks == NULL || phase_sum == NULL ||
        phase_count == NULL || uniform_q15 == NULL || quality_flags == NULL ||
        plan->phase_bins < 2u || uniform_capacity < plan->fft_bins) {
        return RED_ETS_E_ARGUMENT;
    }
    for (i = 0u; i < plan->phase_bins; ++i) {
        if (phase_count[i] == 0u ||
            (i != 0u && delay_ticks[i] <= delay_ticks[i - 1u])) {
            *quality_flags |= RED_ETS_QUALITY_PHASE_COVERAGE_INCOMPLETE |
                              RED_ETS_QUALITY_MISSING_PHASE_BINS;
            return RED_ETS_E_RECONSTRUCT;
        }
    }

    for (i = 0u; i < plan->fft_bins; ++i) {
        uint32_t target = ((uint32_t)i * plan->median_period_ticks +
                           plan->fft_bins / 2u) / plan->fft_bins;
        int32_t value = red_ets_interpolate_at(target, plan, delay_ticks,
                                               phase_sum, phase_count);
        if (value > INT16_MAX) {
            value = INT16_MAX;
        }
        uniform_q15[i] = (int16_t)value;
        total += value;
    }
    mean = (int32_t)(total / plan->fft_bins);
    for (i = 0u; i < plan->fft_bins; ++i) {
        int32_t centered = (int32_t)uniform_q15[i] - mean;
        int32_t absolute = centered < 0 ? -centered : centered;
        if (absolute > maximum_absolute) {
            maximum_absolute = absolute;
        }
    }
    if (maximum_absolute < 2) {
        *quality_flags |= RED_ETS_QUALITY_LOW_SNR;
        return RED_ETS_E_RECONSTRUCT;
    }
    for (i = 0u; i < plan->fft_bins; ++i) {
        int32_t centered = (int32_t)uniform_q15[i] - mean;
        int64_t scaled = (int64_t)centered * 32767;
        scaled /= maximum_absolute;
        if (scaled > 32767) {
            scaled = 32767;
        } else if (scaled < -32767) {
            scaled = -32767;
        }
        uniform_q15[i] = (int16_t)scaled;
    }
    return RED_ETS_OK;
}

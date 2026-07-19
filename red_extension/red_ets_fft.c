#include "red_ets_fft.h"

#include <math.h>
#include <string.h>

#ifndef RED_ETS_PI_F
#define RED_ETS_PI_F 3.14159265358979323846f
#endif

int red_ets_fft_reference_backend(const int16_t *samples,
                                  uint16_t sample_count,
                                  uint8_t harmonic_limit,
                                  uint32_t *magnitude_ppm,
                                  size_t magnitude_capacity,
                                  uint32_t *thd_ppm,
                                  void *user)
{
#if RED_ETS_ENABLE_REFERENCE_DFT
    float magnitude[RED_ETS_MAX_SPECTRUM_BINS];
    uint8_t harmonic;
    float fundamental;
    float thd_square = 0.0f;
    (void)user;
    if (samples == NULL || magnitude_ppm == NULL || thd_ppm == NULL ||
        sample_count < 8u || harmonic_limit == 0u ||
        harmonic_limit >= RED_ETS_MAX_SPECTRUM_BINS ||
        magnitude_capacity <= harmonic_limit) {
        return RED_ETS_E_ARGUMENT;
    }
    memset(magnitude, 0, sizeof(magnitude));
    memset(magnitude_ppm, 0, magnitude_capacity * sizeof(*magnitude_ppm));
    for (harmonic = 0u; harmonic <= harmonic_limit; ++harmonic) {
        float real = 0.0f;
        float imaginary = 0.0f;
        float angle_step = -2.0f * RED_ETS_PI_F * harmonic / sample_count;
        float cosine_step = cosf(angle_step);
        float sine_step = sinf(angle_step);
        float cosine = 1.0f;
        float sine = 0.0f;
        uint16_t i;
        for (i = 0u; i < sample_count; ++i) {
            float value = (float)samples[i];
            float next_cosine;
            real += value * cosine;
            imaginary += value * sine;
            next_cosine = cosine * cosine_step - sine * sine_step;
            sine = sine * cosine_step + cosine * sine_step;
            cosine = next_cosine;
        }
        magnitude[harmonic] = sqrtf(real * real + imaginary * imaginary);
    }
    fundamental = magnitude[1];
    if (!(fundamental > 1.0f)) {
        return RED_ETS_E_ANALYSIS;
    }
    magnitude_ppm[0] = 0u;
    for (harmonic = 1u; harmonic <= harmonic_limit; ++harmonic) {
        float relative = magnitude[harmonic] * 1000000.0f / fundamental;
        if (relative < 0.0f) {
            relative = 0.0f;
        } else if (relative > 4294967040.0f) {
            relative = 4294967040.0f;
        }
        magnitude_ppm[harmonic] = (uint32_t)(relative + 0.5f);
        if (harmonic >= 2u && harmonic <= 5u) {
            float ratio = magnitude[harmonic] / fundamental;
            thd_square += ratio * ratio;
        }
    }
    *thd_ppm = (uint32_t)(sqrtf(thd_square) * 1000000.0f + 0.5f);
    return RED_ETS_OK;
#else
    (void)samples;
    (void)sample_count;
    (void)harmonic_limit;
    (void)magnitude_ppm;
    (void)magnitude_capacity;
    (void)thd_ppm;
    (void)user;
    return RED_ETS_E_DISABLED;
#endif
}

uint8_t red_ets_fft_highest_valid_harmonic(
    uint32_t fundamental_millihz,
    uint32_t equivalent_sample_rate_hz,
    uint32_t calibrated_analog_bandwidth_hz,
    uint8_t requested_limit)
{
    uint8_t harmonic;
    uint8_t highest = 0u;
    if (fundamental_millihz == 0u || calibrated_analog_bandwidth_hz == 0u) {
        return 0u;
    }
    for (harmonic = 1u; harmonic <= requested_limit; ++harmonic) {
        uint64_t frequency_millihz = (uint64_t)fundamental_millihz * harmonic;
        if (frequency_millihz * 2u >=
                (uint64_t)equivalent_sample_rate_hz * 1000u ||
            frequency_millihz >=
                (uint64_t)calibrated_analog_bandwidth_hz * 1000u) {
            break;
        }
        highest = harmonic;
    }
    return highest;
}

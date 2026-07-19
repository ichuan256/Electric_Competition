#ifndef RED_ETS_FFT_H
#define RED_ETS_FFT_H

#include "red_ets_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*red_ets_fft_backend_t)(const int16_t *samples,
                                     uint16_t sample_count,
                                     uint8_t harmonic_limit,
                                     uint32_t *magnitude_ppm,
                                     size_t magnitude_capacity,
                                     uint32_t *thd_ppm,
                                     void *user);

/* Portable reference backend.  An existing optimized Q15 FFT may replace it. */
int red_ets_fft_reference_backend(const int16_t *samples,
                                  uint16_t sample_count,
                                  uint8_t harmonic_limit,
                                  uint32_t *magnitude_ppm,
                                  size_t magnitude_capacity,
                                  uint32_t *thd_ppm,
                                  void *user);

uint8_t red_ets_fft_highest_valid_harmonic(
    uint32_t fundamental_millihz,
    uint32_t equivalent_sample_rate_hz,
    uint32_t calibrated_analog_bandwidth_hz,
    uint8_t requested_limit);

#ifdef __cplusplus
}
#endif
#endif

#ifndef RED_ETS_RECONSTRUCT_H
#define RED_ETS_RECONSTRUCT_H

#include "red_ets_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reorders by actual delay and linearly resamples one circular period to Q15. */
int red_ets_reconstruct_uniform_q15(const red_ets_capture_plan_t *plan,
                                    const uint16_t *delay_ticks,
                                    const uint32_t *phase_sum,
                                    const uint16_t *phase_count,
                                    int16_t *uniform_q15,
                                    size_t uniform_capacity,
                                    uint32_t *quality_flags);

#ifdef __cplusplus
}
#endif
#endif

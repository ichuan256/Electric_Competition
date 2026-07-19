#ifndef RED_ETS_CAPTURE_H
#define RED_ETS_CAPTURE_H

#include "red_ets_types.h"

#ifdef __cplusplus
extern "C" {
#endif

void red_ets_runtime_config_defaults(red_ets_runtime_config_t *config);

int red_ets_capture_validate_request(const red_ets_runtime_config_t *config,
                                     red_ets_request_t *request);

/* period_ticks may be reordered while calculating the median. */
int red_ets_capture_make_plan(const red_ets_runtime_config_t *config,
                              const red_ets_request_t *request,
                              uint32_t *period_ticks,
                              size_t period_count,
                              red_ets_capture_plan_t *plan,
                              uint32_t *quality_flags);

int red_ets_capture_build_delays(const red_ets_capture_plan_t *plan,
                                 uint16_t *delay_ticks,
                                 size_t delay_capacity,
                                 uint32_t *quality_flags);

int red_ets_capture_store_phase(uint16_t phase_index,
                                uint32_t sample_sum,
                                uint16_t valid_samples,
                                int clipped,
                                uint32_t *phase_sum,
                                uint16_t *phase_count,
                                size_t phase_capacity,
                                uint32_t *quality_flags);

#ifdef __cplusplus
}
#endif
#endif

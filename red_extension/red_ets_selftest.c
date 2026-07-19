#include "red_ets_selftest.h"

#include "red_ets_capture.h"
#include "red_ets_fft.h"
#include "red_ets_protocol.h"
#include "red_ets_reconstruct.h"

#include <math.h>
#include <string.h>

int red_ets_selftest_run(void)
{
    red_ets_runtime_config_t config;
    red_ets_request_t request;
    red_ets_capture_plan_t plan;
    red_ets_frame_view_t view;
    uint32_t periods[64];
    uint32_t sums[512];
    uint16_t counts[512];
    uint16_t delays[512];
    int16_t wave[512];
    uint32_t spectrum[32];
    uint32_t quality = 0u;
    uint32_t thd_ppm = 0u;
    uint8_t start_payload[16] = {
        0x78u, 0x56u, 0x34u, 0x12u, 1u, 7u,
        0u, 2u, 8u, 5u, 0x10u, 0x27u,
        0x40u, 0x42u, 0x0Fu, 0u
    };
    uint8_t frame[RED_ETS_MAX_FRAME_SIZE];
    size_t frame_length;
    uint16_t i;

    red_ets_runtime_config_defaults(&config);
    memset(&request, 0, sizeof(request));
    if (red_ets_protocol_parse_start(start_payload, sizeof(start_payload),
                                     &request) != RED_ETS_OK ||
        request.request_token != 0x12345678u ||
        red_ets_capture_validate_request(&config, &request) != RED_ETS_OK) {
        return 1;
    }
    for (i = 0u; i < 64u; ++i) {
        periods[i] = 800u;
    }
    if (red_ets_capture_make_plan(&config, &request, periods, 64u,
                                  &plan, &quality) != RED_ETS_OK ||
        plan.fundamental_millihz != 100000000u || plan.phase_bins != 512u) {
        return 2;
    }
    if (red_ets_capture_build_delays(&plan, delays, 512u, &quality) != RED_ETS_OK) {
        return 3;
    }
    for (i = 0u; i < plan.phase_bins; ++i) {
        float angle = 2.0f * 3.14159265358979323846f * i / plan.phase_bins;
        int32_t code = 2048 + (int32_t)(1000.0f * sinf(angle));
        sums[i] = (uint32_t)code * 8u;
        counts[i] = 8u;
    }
    if (red_ets_reconstruct_uniform_q15(&plan, delays, sums, counts,
                                        wave, 512u, &quality) != RED_ETS_OK) {
        return 4;
    }
    if (red_ets_fft_reference_backend(wave, plan.fft_bins, 5u,
                                      spectrum, 32u, &thd_ppm, NULL) != RED_ETS_OK ||
        spectrum[1] < 990000u || spectrum[1] > 1010000u || thd_ppm > 2000u) {
        return 5;
    }
    if (red_ets_protocol_encode_frame(RED_ETS_NODE_RED, RED_ETS_NODE_BLUE,
                                      RED_ETS_CMD_START, RED_ETS_FLAG_ACK_REQ,
                                      0x1234u, start_payload,
                                      sizeof(start_payload), frame,
                                      sizeof(frame), &frame_length) != RED_ETS_OK ||
        red_ets_protocol_decode_frame(frame, frame_length, &view) != RED_ETS_OK ||
        view.sequence != 0x1234u || view.payload_length != 16u) {
        return 6;
    }
    return 0;
}

#ifdef RED_ETS_SELFTEST_MAIN
#include <stdio.h>
int main(void)
{
    int result = red_ets_selftest_run();
    if (result != 0) {
        (void)printf("red_ets_selftest failed at check %d\n", result);
        return result;
    }
    (void)printf("red_ets_selftest passed\n");
    return 0;
}
#endif

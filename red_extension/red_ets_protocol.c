#include "red_ets_protocol.h"

#include <string.h>

static uint16_t red_ets_get_u16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t red_ets_get_u32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void red_ets_put_u16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

static void red_ets_put_u32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

uint16_t red_ets_protocol_crc16(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFFu;
    size_t i;
    if (data == NULL && length != 0u) {
        return 0u;
    }
    for (i = 0u; i < length; ++i) {
        uint8_t bit;
        crc ^= (uint16_t)data[i] << 8;
        for (bit = 0u; bit < 8u; ++bit) {
            crc = (crc & 0x8000u) != 0u ?
                  (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

int red_ets_protocol_encode_frame(uint8_t destination,
                                  uint8_t source,
                                  uint8_t command,
                                  uint8_t flags,
                                  uint16_t sequence,
                                  const uint8_t *payload,
                                  uint16_t payload_length,
                                  uint8_t *frame,
                                  size_t frame_capacity,
                                  size_t *frame_length)
{
    size_t total = RED_ETS_FRAME_OVERHEAD + payload_length;
    uint16_t crc;
    if (frame == NULL || frame_length == NULL ||
        (payload == NULL && payload_length != 0u) ||
        payload_length > RED_ETS_PROTOCOL_MAX_PAYLOAD || frame_capacity < total) {
        return RED_ETS_E_ARGUMENT;
    }
    frame[0] = 0xD3u;
    frame[1] = 0x91u;
    frame[2] = RED_ETS_PROTOCOL_VERSION;
    frame[3] = destination;
    frame[4] = source;
    frame[5] = command;
    frame[6] = (uint8_t)(flags & 0x1Fu);
    red_ets_put_u16(&frame[7], sequence);
    red_ets_put_u16(&frame[9], payload_length);
    if (payload_length != 0u) {
        memcpy(&frame[11], payload, payload_length);
    }
    crc = red_ets_protocol_crc16(&frame[2], 9u + payload_length);
    red_ets_put_u16(&frame[11u + payload_length], crc);
    frame[13u + payload_length] = 0x91u;
    frame[14u + payload_length] = 0xD3u;
    *frame_length = total;
    return RED_ETS_OK;
}

int red_ets_protocol_decode_frame(const uint8_t *frame,
                                  size_t frame_length,
                                  red_ets_frame_view_t *view)
{
    uint16_t payload_length;
    uint16_t expected_crc;
    uint16_t received_crc;
    if (frame == NULL || view == NULL || frame_length < RED_ETS_FRAME_OVERHEAD ||
        frame[0] != 0xD3u || frame[1] != 0x91u) {
        return RED_ETS_E_PROTOCOL;
    }
    payload_length = red_ets_get_u16(&frame[9]);
    if (payload_length > RED_ETS_PROTOCOL_MAX_PAYLOAD ||
        frame_length != RED_ETS_FRAME_OVERHEAD + payload_length ||
        frame[13u + payload_length] != 0x91u ||
        frame[14u + payload_length] != 0xD3u) {
        return RED_ETS_E_PROTOCOL;
    }
    expected_crc = red_ets_protocol_crc16(&frame[2], 9u + payload_length);
    received_crc = red_ets_get_u16(&frame[11u + payload_length]);
    if (expected_crc != received_crc) {
        return RED_ETS_E_CRC;
    }
    view->version = frame[2];
    view->destination = frame[3];
    view->source = frame[4];
    view->command = frame[5];
    view->flags = frame[6];
    view->sequence = red_ets_get_u16(&frame[7]);
    view->payload_length = payload_length;
    view->payload = &frame[11];
    return RED_ETS_OK;
}

int red_ets_protocol_parse_start(const uint8_t *payload,
                                 uint16_t payload_length,
                                 red_ets_request_t *request)
{
    if (payload == NULL || request == NULL || payload_length != 16u) {
        return RED_ETS_E_PROTOCOL;
    }
    request->request_token = red_ets_get_u32(&payload[0]);
    request->mode = payload[4];
    request->features = payload[5];
    request->requested_phase_bins = red_ets_get_u16(&payload[6]);
    request->averages_per_phase = payload[8];
    request->harmonic_limit = payload[9];
    request->deadline_ms = red_ets_get_u16(&payload[10]);
    request->max_fundamental_hz = red_ets_get_u32(&payload[12]);
    return RED_ETS_OK;
}

size_t red_ets_protocol_build_status(uint8_t state,
                                     uint8_t progress_percent,
                                     uint16_t elapsed_ms,
                                     uint32_t captured_phase_points,
                                     uint32_t quality_flags,
                                     uint8_t payload[12])
{
    payload[0] = state;
    payload[1] = progress_percent > 100u ? 100u : progress_percent;
    red_ets_put_u16(&payload[2], elapsed_ms);
    red_ets_put_u32(&payload[4], captured_phase_points);
    red_ets_put_u32(&payload[8], quality_flags);
    return 12u;
}

size_t red_ets_protocol_build_result(const red_ets_result_t *result,
                                     uint8_t payload[32])
{
    red_ets_put_u32(&payload[0], result->fundamental_millihz);
    red_ets_put_u32(&payload[4], result->thd_1_to_5_ppm);
    red_ets_put_u32(&payload[8], result->adc_max_rate_hz);
    red_ets_put_u32(&payload[12], result->equivalent_sample_rate_hz);
    red_ets_put_u16(&payload[16], result->phase_bins);
    payload[18] = result->averages_per_phase;
    payload[19] = result->highest_valid_harmonic;
    red_ets_put_u32(&payload[20], result->timer_clock_hz);
    red_ets_put_u32(&payload[24], result->calibrated_analog_bandwidth_hz);
    red_ets_put_u32(&payload[28], result->quality_flags);
    return 32u;
}

size_t red_ets_protocol_build_wave_begin(uint16_t total_points,
                                         int32_t minimum_uv,
                                         int32_t maximum_uv,
                                         uint8_t payload[12])
{
    red_ets_put_u16(&payload[0], total_points);
    red_ets_put_u16(&payload[2], 1u);
    red_ets_put_u32(&payload[4], (uint32_t)minimum_uv);
    red_ets_put_u32(&payload[8], (uint32_t)maximum_uv);
    return 12u;
}

int red_ets_protocol_build_wave_chunk(uint16_t offset_points,
                                      const int16_t *samples,
                                      uint8_t point_count,
                                      uint8_t *payload,
                                      size_t payload_capacity,
                                      size_t *payload_length)
{
    uint8_t i;
    size_t needed = 4u + (size_t)point_count * 2u;
    if (samples == NULL || payload == NULL || payload_length == NULL ||
        point_count == 0u || point_count > 60u || payload_capacity < needed) {
        return RED_ETS_E_ARGUMENT;
    }
    red_ets_put_u16(&payload[0], offset_points);
    payload[2] = point_count;
    payload[3] = 0u;
    for (i = 0u; i < point_count; ++i) {
        red_ets_put_u16(&payload[4u + (size_t)i * 2u], (uint16_t)samples[i]);
    }
    *payload_length = needed;
    return RED_ETS_OK;
}

int red_ets_protocol_build_spectrum_chunk(uint16_t offset_bin,
                                          const uint32_t *magnitude_ppm,
                                          uint8_t bin_count,
                                          uint8_t *payload,
                                          size_t payload_capacity,
                                          size_t *payload_length)
{
    uint8_t i;
    size_t needed = 4u + (size_t)bin_count * 4u;
    if (magnitude_ppm == NULL || payload == NULL || payload_length == NULL ||
        bin_count == 0u || bin_count > 31u || payload_capacity < needed) {
        return RED_ETS_E_ARGUMENT;
    }
    red_ets_put_u16(&payload[0], offset_bin);
    payload[2] = bin_count;
    payload[3] = 1u;
    for (i = 0u; i < bin_count; ++i) {
        red_ets_put_u32(&payload[4u + (size_t)i * 4u], magnitude_ppm[i]);
    }
    *payload_length = needed;
    return RED_ETS_OK;
}

size_t red_ets_protocol_build_done(uint32_t request_token,
                                   uint16_t result_flags,
                                   uint16_t chunk_count,
                                   uint8_t payload[8])
{
    red_ets_put_u32(&payload[0], request_token);
    red_ets_put_u16(&payload[4], result_flags);
    red_ets_put_u16(&payload[6], chunk_count);
    return 8u;
}

size_t red_ets_protocol_build_ack(uint8_t acknowledged_command,
                                  uint8_t status,
                                  uint16_t detail,
                                  uint8_t payload[4])
{
    payload[0] = acknowledged_command;
    payload[1] = status;
    red_ets_put_u16(&payload[2], detail);
    return 4u;
}

size_t red_ets_protocol_build_error(uint16_t error_code,
                                    uint8_t failed_state,
                                    uint8_t failed_command,
                                    uint32_t detail,
                                    uint8_t payload[8])
{
    red_ets_put_u16(&payload[0], error_code);
    payload[2] = failed_state;
    payload[3] = failed_command;
    red_ets_put_u32(&payload[4], detail);
    return 8u;
}

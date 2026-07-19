#ifndef RED_ETS_PROTOCOL_H
#define RED_ETS_PROTOCOL_H

#include "red_ets_types.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RED_ETS_CMD_START = 0x69u,
    RED_ETS_CMD_STATUS = 0x6Au,
    RED_ETS_CMD_RESULT = 0x6Bu,
    RED_ETS_CMD_WAVE_BEGIN = 0x6Cu,
    RED_ETS_CMD_WAVE_CHUNK = 0x6Du,
    RED_ETS_CMD_SPECTRUM_CHUNK = 0x6Eu,
    RED_ETS_CMD_DONE = 0x6Fu,
    RED_ETS_CMD_ERROR = 0x7Eu,
    RED_ETS_CMD_ACK = 0x7Fu
};

enum {
    RED_ETS_FLAG_ACK_REQ = 1u << 0,
    RED_ETS_FLAG_RESPONSE = 1u << 1,
    RED_ETS_FLAG_EVENT = 1u << 2,
    RED_ETS_FLAG_ERROR = 1u << 3,
    RED_ETS_FLAG_RETRY = 1u << 4
};

enum {
    RED_ETS_ACK_OK = 0u,
    RED_ETS_ACK_BUSY = 1u,
    RED_ETS_ACK_REJECTED = 2u,
    RED_ETS_ACK_BAD_STATE = 3u,
    RED_ETS_ACK_BAD_PAYLOAD = 4u
};

#define RED_ETS_FRAME_OVERHEAD 15u
#define RED_ETS_MAX_FRAME_SIZE (RED_ETS_PROTOCOL_MAX_PAYLOAD + RED_ETS_FRAME_OVERHEAD)

typedef struct {
    uint8_t version;
    uint8_t destination;
    uint8_t source;
    uint8_t command;
    uint8_t flags;
    uint16_t sequence;
    uint16_t payload_length;
    const uint8_t *payload;
} red_ets_frame_view_t;

uint16_t red_ets_protocol_crc16(const uint8_t *data, size_t length);

int red_ets_protocol_encode_frame(uint8_t destination,
                                  uint8_t source,
                                  uint8_t command,
                                  uint8_t flags,
                                  uint16_t sequence,
                                  const uint8_t *payload,
                                  uint16_t payload_length,
                                  uint8_t *frame,
                                  size_t frame_capacity,
                                  size_t *frame_length);

int red_ets_protocol_decode_frame(const uint8_t *frame,
                                  size_t frame_length,
                                  red_ets_frame_view_t *view);

int red_ets_protocol_parse_start(const uint8_t *payload,
                                 uint16_t payload_length,
                                 red_ets_request_t *request);

size_t red_ets_protocol_build_status(uint8_t state,
                                     uint8_t progress_percent,
                                     uint16_t elapsed_ms,
                                     uint32_t captured_phase_points,
                                     uint32_t quality_flags,
                                     uint8_t payload[12]);

size_t red_ets_protocol_build_result(const red_ets_result_t *result,
                                     uint8_t payload[32]);

size_t red_ets_protocol_build_wave_begin(uint16_t total_points,
                                         int32_t minimum_uv,
                                         int32_t maximum_uv,
                                         uint8_t payload[12]);

int red_ets_protocol_build_wave_chunk(uint16_t offset_points,
                                      const int16_t *samples,
                                      uint8_t point_count,
                                      uint8_t *payload,
                                      size_t payload_capacity,
                                      size_t *payload_length);

int red_ets_protocol_build_spectrum_chunk(uint16_t offset_bin,
                                          const uint32_t *magnitude_ppm,
                                          uint8_t bin_count,
                                          uint8_t *payload,
                                          size_t payload_capacity,
                                          size_t *payload_length);

size_t red_ets_protocol_build_done(uint32_t request_token,
                                   uint16_t result_flags,
                                   uint16_t chunk_count,
                                   uint8_t payload[8]);

size_t red_ets_protocol_build_ack(uint8_t acknowledged_command,
                                  uint8_t status,
                                  uint16_t detail,
                                  uint8_t payload[4]);

size_t red_ets_protocol_build_error(uint16_t error_code,
                                    uint8_t failed_state,
                                    uint8_t failed_command,
                                    uint32_t detail,
                                    uint8_t payload[8]);

#ifdef __cplusplus
}
#endif
#endif

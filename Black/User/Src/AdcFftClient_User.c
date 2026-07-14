#include "AdcFftClient_User.h"

#include <string.h>
#include "BoardComm_User.h"

#define ADC_FFT_HEAD0                    0xF9U
#define ADC_FFT_HEAD1                    0x26U
#define ADC_FFT_TAIL0                    0x62U
#define ADC_FFT_TAIL1                    0x9FU
#define ADC_FFT_VERSION                  0x01U
#define ADC_FFT_FIXED_HEADER_LEN         9U
#define ADC_FFT_FRAME_OVERHEAD           13U
#define ADC_FFT_REQUEST_PAYLOAD_LEN      28U
#define ADC_FFT_ACCEPTED_PAYLOAD_LEN     18U
#define ADC_FFT_BUSY_PAYLOAD_LEN         8U
#define ADC_FFT_RESULT_PAYLOAD_LEN       36U
#define ADC_FFT_ERROR_PAYLOAD_LEN        12U
#define ADC_FFT_ACCEPT_TIMEOUT_MS        100UL
#define ADC_FFT_RESULT_TIMEOUT_MS        1000UL
#define ADC_FFT_MAX_RETRY                2U

AdcFftClientState adc_fft_client_state;

static uint8_t adc_fft_next_seq;
static uint16_t adc_fft_next_point_id;
static uint8_t adc_fft_retry_count;
static uint32_t adc_fft_phase_tick;

static void AdcFft_PutU16(uint8_t *data, uint16_t *pos, uint16_t value)
{
  data[(*pos)++] = (uint8_t)(value & 0xFFU);
  data[(*pos)++] = (uint8_t)((value >> 8) & 0xFFU);
}

static void AdcFft_PutU32(uint8_t *data, uint16_t *pos, uint32_t value)
{
  data[(*pos)++] = (uint8_t)(value & 0xFFUL);
  data[(*pos)++] = (uint8_t)((value >> 8) & 0xFFUL);
  data[(*pos)++] = (uint8_t)((value >> 16) & 0xFFUL);
  data[(*pos)++] = (uint8_t)((value >> 24) & 0xFFUL);
}

static uint16_t AdcFft_GetU16(const uint8_t *data)
{
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t AdcFft_GetU32(const uint8_t *data)
{
  return (uint32_t)data[0] |
         ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) |
         ((uint32_t)data[3] << 24);
}

static uint16_t AdcFft_Crc16(const uint8_t *data, uint16_t len)
{
  uint16_t crc = 0xFFFFU;

  while (len-- != 0U)
  {
    crc ^= (uint16_t)(*data++) << 8;
    for (uint8_t bit = 0U; bit < 8U; bit++)
    {
      crc = ((crc & 0x8000U) != 0U) ?
            (uint16_t)((crc << 1) ^ 0x1021U) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

static void AdcFft_RecordDebug(const uint8_t *data, uint16_t len)
{
  for (uint16_t i = 0U; i < len; i++)
  {
    adc_fft_client_state.rx_debug_buf[adc_fft_client_state.rx_debug_pos++] = data[i];
    if (adc_fft_client_state.rx_debug_pos >= ADC_FFT_CLIENT_RX_DEBUG_LEN)
    {
      adc_fft_client_state.rx_debug_pos = 0U;
    }
    adc_fft_client_state.rx_debug_count++;
  }
}

static uint16_t AdcFft_BuildRequestFrame(uint8_t *frame, uint16_t capacity)
{
  uint16_t pos = 0U;
  uint16_t crc;
  const AdcFftClientRequest *request = &adc_fft_client_state.active_request;

  if (capacity < (ADC_FFT_FRAME_OVERHEAD + ADC_FFT_REQUEST_PAYLOAD_LEN))
  {
    return 0U;
  }

  frame[pos++] = ADC_FFT_HEAD0;
  frame[pos++] = ADC_FFT_HEAD1;
  frame[pos++] = ADC_FFT_VERSION;
  frame[pos++] = ADC_FFT_CLIENT_TARGET;
  frame[pos++] = ADC_FFT_CLIENT_CMD_MEASURE_REQUEST;
  frame[pos++] = 0U;
  frame[pos++] = adc_fft_client_state.last_seq;
  AdcFft_PutU16(frame, &pos, ADC_FFT_REQUEST_PAYLOAD_LEN);
  AdcFft_PutU16(frame, &pos, request->sweep_id);
  AdcFft_PutU16(frame, &pos, request->point_id);
  AdcFft_PutU32(frame, &pos, request->reference_frequency_hz);
  AdcFft_PutU32(frame, &pos, request->dds_ftw);
  AdcFft_PutU32(frame, &pos, request->sample_rate_hz);
  AdcFft_PutU16(frame, &pos, request->fft_length);
  AdcFft_PutU16(frame, &pos, request->target_bin);
  AdcFft_PutU32(frame, &pos, request->settle_us);
  AdcFft_PutU16(frame, &pos, request->pre_capture_delay_us);
  frame[pos++] = request->window_mode;
  frame[pos++] = request->request_flags;

  crc = AdcFft_Crc16(&frame[2], (uint16_t)(pos - 2U));
  AdcFft_PutU16(frame, &pos, crc);
  frame[pos++] = ADC_FFT_TAIL0;
  frame[pos++] = ADC_FFT_TAIL1;
  return pos;
}

static uint8_t AdcFft_SendActiveRequest(void)
{
  uint8_t frame[ADC_FFT_FRAME_OVERHEAD + ADC_FFT_REQUEST_PAYLOAD_LEN];
  uint16_t frame_len = AdcFft_BuildRequestFrame(frame, (uint16_t)sizeof(frame));

  if ((frame_len == 0U) ||
      (BoardComm_SendRaw(frame, frame_len, BOARD_COMM_TIMEOUT_MS) != BOARD_COMM_OK))
  {
    adc_fft_client_state.error_count++;
    adc_fft_client_state.phase = ADC_FFT_CLIENT_ERROR;
    return 0U;
  }

  adc_fft_client_state.tx_count++;
  adc_fft_client_state.last_cmd = ADC_FFT_CLIENT_CMD_MEASURE_REQUEST;
  adc_fft_client_state.phase = ADC_FFT_CLIENT_WAIT_ACCEPTED;
  adc_fft_phase_tick = HAL_GetTick();
  return 1U;
}

static uint8_t AdcFft_FrameMatchesRequest(const uint8_t *payload, uint16_t payload_len)
{
  const AdcFftClientRequest *request = &adc_fft_client_state.active_request;

  if (payload_len < 8U)
  {
    return 0U;
  }
  return ((AdcFft_GetU16(&payload[0]) == request->sweep_id) &&
          (AdcFft_GetU16(&payload[2]) == request->point_id) &&
          (AdcFft_GetU32(&payload[4]) == request->reference_frequency_hz)) ? 1U : 0U;
}

static uint8_t AdcFft_ParseFrame(const uint8_t *frame, uint16_t frame_len)
{
  uint16_t payload_len;
  uint16_t expected_crc;
  uint16_t actual_crc;
  const uint8_t *payload;
  uint8_t cmd;

  if (frame_len < ADC_FFT_FRAME_OVERHEAD)
  {
    adc_fft_client_state.len_error_count++;
    adc_fft_client_state.error_count++;
    return 0U;
  }

  payload_len = AdcFft_GetU16(&frame[7]);
  if (((uint16_t)(ADC_FFT_FRAME_OVERHEAD + payload_len) != frame_len) ||
      (frame[frame_len - 2U] != ADC_FFT_TAIL0) ||
      (frame[frame_len - 1U] != ADC_FFT_TAIL1))
  {
    adc_fft_client_state.len_error_count++;
    adc_fft_client_state.error_count++;
    return 0U;
  }

  expected_crc = AdcFft_GetU16(&frame[ADC_FFT_FIXED_HEADER_LEN + payload_len]);
  actual_crc = AdcFft_Crc16(&frame[2], (uint16_t)(7U + payload_len));
  if (expected_crc != actual_crc)
  {
    adc_fft_client_state.crc_error_count++;
    adc_fft_client_state.error_count++;
    return 0U;
  }

  if ((frame[2] != ADC_FFT_VERSION) || (frame[3] != ADC_FFT_CLIENT_TARGET) ||
      (frame[6] != adc_fft_client_state.last_seq))
  {
    adc_fft_client_state.error_count++;
    return 0U;
  }

  cmd = frame[4];
  payload = &frame[ADC_FFT_FIXED_HEADER_LEN];
  adc_fft_client_state.last_cmd = cmd;

  if ((cmd == ADC_FFT_CLIENT_CMD_REQUEST_ACCEPTED) &&
      (payload_len == ADC_FFT_ACCEPTED_PAYLOAD_LEN) &&
      (AdcFft_FrameMatchesRequest(payload, payload_len) != 0U))
  {
    adc_fft_client_state.phase = ADC_FFT_CLIENT_WAIT_RESULT;
    adc_fft_phase_tick = HAL_GetTick();
  }
  else if ((cmd == ADC_FFT_CLIENT_CMD_BUSY) && (payload_len == ADC_FFT_BUSY_PAYLOAD_LEN) &&
           (AdcFft_GetU16(&payload[0]) == adc_fft_client_state.active_request.sweep_id) &&
           (AdcFft_GetU16(&payload[2]) == adc_fft_client_state.active_request.point_id))
  {
    adc_fft_client_state.busy_count++;
    adc_fft_client_state.retry_after_ms = AdcFft_GetU16(&payload[6]);
    adc_fft_client_state.phase = ADC_FFT_CLIENT_BUSY_DELAY;
    adc_fft_phase_tick = HAL_GetTick();
  }
  else if ((cmd == ADC_FFT_CLIENT_CMD_MEASURE_RESULT) &&
           (payload_len == ADC_FFT_RESULT_PAYLOAD_LEN) &&
           (AdcFft_FrameMatchesRequest(payload, payload_len) != 0U))
  {
    AdcFftClientResult *result = &adc_fft_client_state.last_result;
    result->sweep_id = AdcFft_GetU16(&payload[0]);
    result->point_id = AdcFft_GetU16(&payload[2]);
    result->reference_frequency_hz = AdcFft_GetU32(&payload[4]);
    result->main_frequency_hz = AdcFft_GetU32(&payload[8]);
    result->voltage_uv_rms = AdcFft_GetU32(&payload[12]);
    result->voltage_uv_peak = AdcFft_GetU32(&payload[16]);
    result->sample_rate_hz = AdcFft_GetU32(&payload[20]);
    result->fft_length = AdcFft_GetU16(&payload[24]);
    result->target_bin = AdcFft_GetU16(&payload[26]);
    result->main_bin = AdcFft_GetU16(&payload[28]);
    result->status = AdcFft_GetU16(&payload[30]);
    result->adc_min_code = AdcFft_GetU16(&payload[32]);
    result->adc_max_code = AdcFft_GetU16(&payload[34]);
    result->seq = frame[6];
    adc_fft_client_state.has_result = 1U;
    adc_fft_client_state.phase = ADC_FFT_CLIENT_RESULT_READY;
  }
  else if ((cmd == ADC_FFT_CLIENT_CMD_MEASURE_ERROR) &&
           (payload_len == ADC_FFT_ERROR_PAYLOAD_LEN))
  {
    adc_fft_client_state.last_error_code = (uint8_t)AdcFft_GetU16(&payload[4]);
    adc_fft_client_state.last_error_detail = AdcFft_GetU16(&payload[6]);
    adc_fft_client_state.error_count++;
    adc_fft_client_state.phase = ADC_FFT_CLIENT_ERROR;
  }
  else
  {
    adc_fft_client_state.error_count++;
    return 0U;
  }

  adc_fft_client_state.rx_count++;
  return 1U;
}

void AdcFftClient_Init(void)
{
  memset(&adc_fft_client_state, 0, sizeof(adc_fft_client_state));
  adc_fft_client_state.phase = ADC_FFT_CLIENT_IDLE;
  adc_fft_next_seq = 1U;
  adc_fft_next_point_id = 1U;
  adc_fft_retry_count = 0U;
  adc_fft_phase_tick = HAL_GetTick();
}

uint8_t AdcFftClient_RequestMeasurement(uint32_t reference_frequency_hz,
                                        uint32_t dds_ftw,
                                        uint32_t settle_us)
{
  AdcFftClientRequest *request = &adc_fft_client_state.active_request;

  if ((adc_fft_client_state.phase != ADC_FFT_CLIENT_IDLE) &&
      (adc_fft_client_state.phase != ADC_FFT_CLIENT_RESULT_READY) &&
      (adc_fft_client_state.phase != ADC_FFT_CLIENT_ERROR))
  {
    return 0U;
  }

  request->sweep_id = 1U;
  request->point_id = adc_fft_next_point_id++;
  request->reference_frequency_hz = reference_frequency_hz;
  request->dds_ftw = dds_ftw;
  request->sample_rate_hz = ADC_FFT_CLIENT_SAMPLE_RATE_HZ;
  request->fft_length = ADC_FFT_CLIENT_FFT_LENGTH;
  request->target_bin = ADC_FFT_CLIENT_TARGET_BIN_AUTO;
  request->settle_us = settle_us;
  request->pre_capture_delay_us = 0U;
  request->window_mode = 0U;
  request->request_flags = 0U;
  adc_fft_client_state.last_seq = adc_fft_next_seq++;
  adc_fft_client_state.has_result = 0U;
  adc_fft_client_state.last_error_code = 0U;
  adc_fft_client_state.last_error_detail = 0U;
  adc_fft_retry_count = 0U;
  adc_fft_client_state.phase = ADC_FFT_CLIENT_WAIT_DDS_SETTLE;
  adc_fft_phase_tick = HAL_GetTick();
  return 1U;
}

void AdcFftClient_Task(void)
{
  uint32_t now = HAL_GetTick();
  uint32_t elapsed = now - adc_fft_phase_tick;

  if (adc_fft_client_state.phase == ADC_FFT_CLIENT_WAIT_DDS_SETTLE)
  {
    uint32_t settle_ms = (adc_fft_client_state.active_request.settle_us + 999UL) / 1000UL;
    if (elapsed >= settle_ms)
    {
      (void)AdcFft_SendActiveRequest();
    }
  }
  else if ((adc_fft_client_state.phase == ADC_FFT_CLIENT_WAIT_ACCEPTED) &&
           (elapsed >= ADC_FFT_ACCEPT_TIMEOUT_MS))
  {
    adc_fft_client_state.timeout_count++;
    adc_fft_client_state.error_count++;
    adc_fft_client_state.phase = ADC_FFT_CLIENT_ERROR;
  }
  else if ((adc_fft_client_state.phase == ADC_FFT_CLIENT_WAIT_RESULT) &&
           (elapsed >= ADC_FFT_RESULT_TIMEOUT_MS))
  {
    adc_fft_client_state.timeout_count++;
    adc_fft_client_state.error_count++;
    adc_fft_client_state.phase = ADC_FFT_CLIENT_ERROR;
  }
  else if ((adc_fft_client_state.phase == ADC_FFT_CLIENT_BUSY_DELAY) &&
           (elapsed >= adc_fft_client_state.retry_after_ms))
  {
    if (adc_fft_retry_count < ADC_FFT_MAX_RETRY)
    {
      adc_fft_retry_count++;
      adc_fft_client_state.last_seq = adc_fft_next_seq++;
      (void)AdcFft_SendActiveRequest();
    }
    else
    {
      adc_fft_client_state.error_count++;
      adc_fft_client_state.phase = ADC_FFT_CLIENT_ERROR;
    }
  }
}

uint8_t AdcFftClient_IsFrameStart(const uint8_t *data, uint16_t size)
{
  return ((data != 0) && (size >= 2U) &&
          (data[0] == ADC_FFT_HEAD0) && (data[1] == ADC_FFT_HEAD1)) ? 1U : 0U;
}

uint8_t AdcFftClient_HandleRxBuffer(const uint8_t *data, uint16_t size)
{
  uint16_t offset = 0U;
  uint8_t parsed = 0U;

  if ((data == 0) || (size == 0U))
  {
    return 0U;
  }
  AdcFft_RecordDebug(data, size);

  while ((uint16_t)(size - offset) >= ADC_FFT_FRAME_OVERHEAD)
  {
    uint16_t payload_len;
    uint16_t frame_len;

    if ((data[offset] != ADC_FFT_HEAD0) || (data[offset + 1U] != ADC_FFT_HEAD1))
    {
      offset++;
      continue;
    }
    payload_len = AdcFft_GetU16(&data[offset + 7U]);
    frame_len = (uint16_t)(ADC_FFT_FRAME_OVERHEAD + payload_len);
    if (frame_len > (uint16_t)(size - offset))
    {
      adc_fft_client_state.len_error_count++;
      adc_fft_client_state.error_count++;
      break;
    }
    parsed |= AdcFft_ParseFrame(&data[offset], frame_len);
    offset = (uint16_t)(offset + frame_len);
  }
  return parsed;
}

uint8_t AdcFftClient_TakeResult(AdcFftClientResult *result)
{
  if ((result == 0) || (adc_fft_client_state.has_result == 0U))
  {
    return 0U;
  }
  *result = adc_fft_client_state.last_result;
  adc_fft_client_state.has_result = 0U;
  adc_fft_client_state.phase = ADC_FFT_CLIENT_IDLE;
  return 1U;
}

AdcFftClientState AdcFftClient_GetState(void)
{
  return adc_fft_client_state;
}

#include "AdcFftProtocol_User.h"

#include <string.h>
#include "BoardComm_User.h"

#define ADC_FFT_PROTO_HEAD0             0xF9U
#define ADC_FFT_PROTO_HEAD1             0x26U
#define ADC_FFT_PROTO_TAIL0             0x62U
#define ADC_FFT_PROTO_TAIL1             0x9FU
#define ADC_FFT_PROTO_VERSION           0x01U
#define ADC_FFT_PROTO_FIXED_HEADER_LEN  9U
#define ADC_FFT_PROTO_MAX_PAYLOAD_LEN   36U
#define ADC_FFT_PROTO_FRAME_MAX_LEN     (13U + ADC_FFT_PROTO_MAX_PAYLOAD_LEN)
#define ADC_FFT_REQUEST_PAYLOAD_LEN     28U
#define ADC_FFT_ACCEPT_PAYLOAD_LEN      18U
#define ADC_FFT_BUSY_PAYLOAD_LEN        8U
#define ADC_FFT_RESULT_PAYLOAD_LEN      36U
#define ADC_FFT_ERROR_PAYLOAD_LEN       12U
#define ADC_FFT_TX_TIMEOUT_MS           30U

AdcFftProtocolState adc_fft_protocol_state;

static uint8_t adc_fft_pending_frame[ADC_FFT_PROTO_FRAME_MAX_LEN];
static volatile uint16_t adc_fft_pending_len;
static uint8_t adc_fft_active_seq;
static uint8_t adc_fft_has_active_seq;

static uint16_t AdcFftProto_GetU16(const uint8_t *data)
{
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t AdcFftProto_GetU32(const uint8_t *data)
{
  return (uint32_t)data[0] |
         ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) |
         ((uint32_t)data[3] << 24);
}

static void AdcFftProto_PutU16(uint8_t *data, uint16_t *pos, uint16_t value)
{
  data[(*pos)++] = (uint8_t)(value & 0xFFU);
  data[(*pos)++] = (uint8_t)((value >> 8) & 0xFFU);
}

static void AdcFftProto_PutU32(uint8_t *data, uint16_t *pos, uint32_t value)
{
  data[(*pos)++] = (uint8_t)(value & 0xFFUL);
  data[(*pos)++] = (uint8_t)((value >> 8) & 0xFFUL);
  data[(*pos)++] = (uint8_t)((value >> 16) & 0xFFUL);
  data[(*pos)++] = (uint8_t)((value >> 24) & 0xFFUL);
}

static uint16_t AdcFftProto_Crc16(const uint8_t *data, uint16_t len)
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

static uint8_t AdcFftProto_Send(uint8_t cmd, uint8_t flags, uint8_t seq,
                                const uint8_t *payload, uint16_t payload_len)
{
  uint8_t frame[ADC_FFT_PROTO_FRAME_MAX_LEN];
  uint16_t pos = 0U;
  uint16_t crc;

  if ((payload_len > ADC_FFT_PROTO_MAX_PAYLOAD_LEN) ||
      ((payload_len != 0U) && (payload == 0)))
  {
    return 0U;
  }

  frame[pos++] = ADC_FFT_PROTO_HEAD0;
  frame[pos++] = ADC_FFT_PROTO_HEAD1;
  frame[pos++] = ADC_FFT_PROTO_VERSION;
  frame[pos++] = ADC_FFT_PROTO_TARGET;
  frame[pos++] = cmd;
  frame[pos++] = flags;
  frame[pos++] = seq;
  AdcFftProto_PutU16(frame, &pos, payload_len);
  for (uint16_t i = 0U; i < payload_len; i++)
  {
    frame[pos++] = payload[i];
  }
  crc = AdcFftProto_Crc16(&frame[2], (uint16_t)(7U + payload_len));
  AdcFftProto_PutU16(frame, &pos, crc);
  frame[pos++] = ADC_FFT_PROTO_TAIL0;
  frame[pos++] = ADC_FFT_PROTO_TAIL1;

  if (BoardComm_SendRaw(frame, pos, ADC_FFT_TX_TIMEOUT_MS) != BOARD_COMM_OK)
  {
    adc_fft_protocol_state.error_count++;
    return 0U;
  }
  adc_fft_protocol_state.tx_count++;
  adc_fft_protocol_state.last_cmd = cmd;
  adc_fft_protocol_state.last_seq = seq;
  return 1U;
}

static void AdcFftProto_SendError(uint8_t seq, uint16_t sweep_id, uint16_t point_id,
                                  uint16_t error_code, uint16_t detail,
                                  uint32_t reference_frequency_hz)
{
  uint8_t payload[ADC_FFT_ERROR_PAYLOAD_LEN];
  uint16_t pos = 0U;

  AdcFftProto_PutU16(payload, &pos, sweep_id);
  AdcFftProto_PutU16(payload, &pos, point_id);
  AdcFftProto_PutU16(payload, &pos, error_code);
  AdcFftProto_PutU16(payload, &pos, detail);
  AdcFftProto_PutU32(payload, &pos, reference_frequency_hz);
  adc_fft_protocol_state.last_error_code = (uint8_t)error_code;
  (void)AdcFftProto_Send(ADC_FFT_PROTO_CMD_MEASURE_ERROR, 0U, seq, payload, pos);
}

static void AdcFftProto_SendBusy(uint8_t seq, uint16_t sweep_id, uint16_t point_id)
{
  uint8_t payload[ADC_FFT_BUSY_PAYLOAD_LEN];
  uint16_t pos = 0U;
  uint16_t busy_state = 1U;
  AdcFftMeasureState state = AdcFftMeasure_GetState();

  if (state == ADC_FFT_PROCESSING) { busy_state = 2U; }
  else if (state == ADC_FFT_RESULT_READY) { busy_state = 3U; }
  AdcFftProto_PutU16(payload, &pos, sweep_id);
  AdcFftProto_PutU16(payload, &pos, point_id);
  AdcFftProto_PutU16(payload, &pos, busy_state);
  AdcFftProto_PutU16(payload, &pos, 20U);
  adc_fft_protocol_state.busy_count++;
  (void)AdcFftProto_Send(ADC_FFT_PROTO_CMD_BUSY, 0U, seq, payload, pos);
}

static void AdcFftProto_SendAccepted(uint8_t seq, const AdcFftMeasurementRequest *request)
{
  uint8_t payload[ADC_FFT_ACCEPT_PAYLOAD_LEN];
  uint16_t pos = 0U;

  AdcFftProto_PutU16(payload, &pos, request->sweep_id);
  AdcFftProto_PutU16(payload, &pos, request->point_id);
  AdcFftProto_PutU32(payload, &pos, request->reference_frequency_hz);
  AdcFftProto_PutU32(payload, &pos, request->sample_rate_hz);
  AdcFftProto_PutU16(payload, &pos, request->target_bin);
  AdcFftProto_PutU16(payload, &pos, request->fft_length);
  AdcFftProto_PutU16(payload, &pos, 0U);
  (void)AdcFftProto_Send(ADC_FFT_PROTO_CMD_REQUEST_ACCEPTED, 0U, seq, payload, pos);
}

static void AdcFftProto_SendResult(uint8_t seq, const AdcFftMeasurementResult *result)
{
  uint8_t payload[ADC_FFT_RESULT_PAYLOAD_LEN];
  uint16_t pos = 0U;

  AdcFftProto_PutU16(payload, &pos, result->sweep_id);
  AdcFftProto_PutU16(payload, &pos, result->point_id);
  AdcFftProto_PutU32(payload, &pos, result->reference_frequency_hz);
  AdcFftProto_PutU32(payload, &pos, result->main_frequency_hz);
  AdcFftProto_PutU32(payload, &pos, result->voltage_uv_rms);
  AdcFftProto_PutU32(payload, &pos, result->voltage_uv_peak);
  AdcFftProto_PutU32(payload, &pos, result->sample_rate_hz);
  AdcFftProto_PutU16(payload, &pos, ADC_FFT_SAMPLE_COUNT);
  AdcFftProto_PutU16(payload, &pos, result->target_bin);
  AdcFftProto_PutU16(payload, &pos, result->main_bin);
  AdcFftProto_PutU16(payload, &pos, result->status);
  AdcFftProto_PutU16(payload, &pos, result->adc_min_code);
  AdcFftProto_PutU16(payload, &pos, result->adc_max_code);
  (void)AdcFftProto_Send(ADC_FFT_PROTO_CMD_MEASURE_RESULT, 0U, seq, payload, pos);
}

static uint8_t AdcFftProto_ParseRequest(const uint8_t *payload,
                                        AdcFftMeasurementRequest *request)
{
  if ((payload == 0) || (request == 0))
  {
    return 0U;
  }
  request->sweep_id = AdcFftProto_GetU16(&payload[0]);
  request->point_id = AdcFftProto_GetU16(&payload[2]);
  request->reference_frequency_hz = AdcFftProto_GetU32(&payload[4]);
  request->dds_ftw = AdcFftProto_GetU32(&payload[8]);
  request->sample_rate_hz = AdcFftProto_GetU32(&payload[12]);
  request->fft_length = AdcFftProto_GetU16(&payload[16]);
  request->target_bin = AdcFftProto_GetU16(&payload[18]);
  request->settle_us = AdcFftProto_GetU32(&payload[20]);
  request->pre_capture_delay_us = AdcFftProto_GetU16(&payload[24]);
  request->window_mode = payload[26];
  request->flags = payload[27];
  if (request->target_bin == ADC_FFT_TARGET_BIN_AUTO)
  {
    request->target_bin = AdcFftMeasure_CalculateTargetBin(request->reference_frequency_hz,
                                                           request->sample_rate_hz);
  }
  return 1U;
}

static uint8_t AdcFftProto_ValidateRequest(const AdcFftMeasurementRequest *request,
                                           uint16_t *error_code)
{
  if ((request == 0) || (error_code == 0))
  {
    return 0U;
  }
  if (request->fft_length != ADC_FFT_SAMPLE_COUNT)
  {
    *error_code = ADC_FFT_PROTO_ERR_INVALID_LENGTH;
    return 0U;
  }
  if (AdcFftMeasure_IsSupportedSampleRate(request->sample_rate_hz) == 0U)
  {
    *error_code = ADC_FFT_PROTO_ERR_UNSUPPORTED_SAMPLE_RATE;
    return 0U;
  }
  if (request->reference_frequency_hz >= (request->sample_rate_hz / 2UL))
  {
    *error_code = ADC_FFT_PROTO_ERR_REFERENCE_OUT_OF_RANGE;
    return 0U;
  }
  if ((request->target_bin == 0U) ||
      (request->target_bin >= (ADC_FFT_SAMPLE_COUNT / 2U)))
  {
    *error_code = ADC_FFT_PROTO_ERR_INVALID_TARGET_BIN;
    return 0U;
  }
  *error_code = 0U;
  return 1U;
}

static void AdcFftProto_ProcessRequestFrame(const uint8_t *data, uint16_t size)
{
  uint16_t payload_len;
  uint16_t crc_expected;
  uint16_t crc_actual;
  uint8_t seq;
  AdcFftMeasurementRequest request;
  uint16_t error_code = 0U;

  if ((size < 13U) || (data[0] != ADC_FFT_PROTO_HEAD0) ||
      (data[1] != ADC_FFT_PROTO_HEAD1))
  {
    adc_fft_protocol_state.error_count++;
    return;
  }
  payload_len = AdcFftProto_GetU16(&data[7]);
  if ((payload_len > ADC_FFT_PROTO_MAX_PAYLOAD_LEN) ||
      ((uint16_t)(13U + payload_len) != size))
  {
    adc_fft_protocol_state.error_count++;
    adc_fft_protocol_state.len_error_count++;
    return;
  }
  if ((data[size - 2U] != ADC_FFT_PROTO_TAIL0) ||
      (data[size - 1U] != ADC_FFT_PROTO_TAIL1))
  {
    adc_fft_protocol_state.error_count++;
    return;
  }

  crc_expected = AdcFftProto_GetU16(&data[9U + payload_len]);
  crc_actual = AdcFftProto_Crc16(&data[2], (uint16_t)(7U + payload_len));
  if (crc_expected != crc_actual)
  {
    adc_fft_protocol_state.error_count++;
    adc_fft_protocol_state.crc_error_count++;
    return;
  }

  seq = data[6];
  adc_fft_protocol_state.rx_count++;
  adc_fft_protocol_state.last_cmd = data[4];
  adc_fft_protocol_state.last_seq = seq;
  if ((data[2] != ADC_FFT_PROTO_VERSION) ||
      (data[3] != ADC_FFT_PROTO_TARGET) ||
      (data[4] != ADC_FFT_PROTO_CMD_MEASURE_REQUEST) ||
      (payload_len != ADC_FFT_REQUEST_PAYLOAD_LEN))
  {
    AdcFftProto_SendError(seq, 0U, 0U, ADC_FFT_PROTO_ERR_INVALID_LENGTH,
                          payload_len, 0UL);
    return;
  }

  (void)AdcFftProto_ParseRequest(&data[ADC_FFT_PROTO_FIXED_HEADER_LEN], &request);
  adc_fft_protocol_state.last_request = request;
  if (AdcFftMeasure_GetState() != ADC_FFT_IDLE)
  {
    AdcFftProto_SendBusy(seq, request.sweep_id, request.point_id);
    return;
  }
  if (AdcFftProto_ValidateRequest(&request, &error_code) == 0U)
  {
    AdcFftProto_SendError(seq, request.sweep_id, request.point_id,
                          error_code, request.target_bin,
                          request.reference_frequency_hz);
    return;
  }
  if (AdcFftMeasure_Start(&request) == 0U)
  {
    AdcFftProto_SendBusy(seq, request.sweep_id, request.point_id);
    return;
  }

  adc_fft_active_seq = seq;
  adc_fft_has_active_seq = 1U;
  AdcFftProto_SendAccepted(seq, &request);
}

void AdcFftProtocol_Init(UART_HandleTypeDef *huart)
{
  (void)huart;
  memset(&adc_fft_protocol_state, 0, sizeof(adc_fft_protocol_state));
  adc_fft_pending_len = 0U;
  adc_fft_active_seq = 0U;
  adc_fft_has_active_seq = 0U;
}

uint8_t AdcFftProtocol_IsFrameStart(const uint8_t *data, uint16_t size)
{
  return ((data != 0) && (size >= 2U) &&
          (data[0] == ADC_FFT_PROTO_HEAD0) &&
          (data[1] == ADC_FFT_PROTO_HEAD1)) ? 1U : 0U;
}

uint8_t AdcFftProtocol_HandleRxBuffer(const uint8_t *data, uint16_t size)
{
  if ((data == 0) || (size > ADC_FFT_PROTO_FRAME_MAX_LEN) ||
      (AdcFftProtocol_IsFrameStart(data, size) == 0U))
  {
    adc_fft_protocol_state.error_count++;
    return 0U;
  }
  if (adc_fft_pending_len != 0U)
  {
    adc_fft_protocol_state.error_count++;
    return 0U;
  }

  for (uint16_t i = 0U; i < size; i++)
  {
    adc_fft_pending_frame[i] = data[i];
    adc_fft_protocol_state.rx_debug_buf[adc_fft_protocol_state.rx_debug_pos++] = data[i];
    if (adc_fft_protocol_state.rx_debug_pos >= sizeof(adc_fft_protocol_state.rx_debug_buf))
    {
      adc_fft_protocol_state.rx_debug_pos = 0U;
    }
    adc_fft_protocol_state.rx_debug_count++;
  }
  adc_fft_protocol_state.last_rx_size = size;
  adc_fft_protocol_state.pending_count++;
  __DMB();
  adc_fft_pending_len = size;
  return 1U;
}

void AdcFftProtocol_Task(void)
{
  uint8_t frame[ADC_FFT_PROTO_FRAME_MAX_LEN];
  uint16_t frame_len = 0U;
  AdcFftMeasurementResult result;

  if (adc_fft_pending_len != 0U)
  {
    __disable_irq();
    frame_len = adc_fft_pending_len;
    for (uint16_t i = 0U; i < frame_len; i++)
    {
      frame[i] = adc_fft_pending_frame[i];
    }
    adc_fft_pending_len = 0U;
    __enable_irq();
    AdcFftProto_ProcessRequestFrame(frame, frame_len);
  }

  AdcFftMeasure_Task();
  if ((adc_fft_has_active_seq != 0U) &&
      (AdcFftMeasure_TakeResult(&result) != 0U))
  {
    adc_fft_protocol_state.last_result = result;
    if ((result.status & ADC_FFT_STATUS_ADC_DMA_ERROR) != 0U)
    {
      AdcFftProto_SendError(adc_fft_active_seq, result.sweep_id, result.point_id,
                            ADC_FFT_PROTO_ERR_ADC_DMA_ERROR, result.status,
                            result.reference_frequency_hz);
    }
    else if ((result.status & ADC_FFT_STATUS_FRAME_TIMEOUT) != 0U)
    {
      AdcFftProto_SendError(adc_fft_active_seq, result.sweep_id, result.point_id,
                            ADC_FFT_PROTO_ERR_FRAME_TIMEOUT, result.status,
                            result.reference_frequency_hz);
    }
    else
    {
      AdcFftProto_SendResult(adc_fft_active_seq, &result);
    }
    adc_fft_has_active_seq = 0U;
  }
}

AdcFftProtocolState AdcFftProtocol_GetState(void)
{
  return adc_fft_protocol_state;
}

#include "FftUart_User.h"

#define FFT_UART_HEAD0                  0xF9U
#define FFT_UART_HEAD1                  0x26U
#define FFT_UART_TAIL0                  0x62U
#define FFT_UART_TAIL1                  0x9FU
#define FFT_UART_VERSION                0x01U
#define FFT_UART_MEASURE_FLAGS          0x01U
#define FFT_UART_FIXED_HEADER_LEN       9U
#define FFT_UART_CRC_AND_TAIL_LEN       4U
#define FFT_UART_CRC_OFFSET             9U

typedef enum {
  FFT_RX_WAIT_HEAD0 = 0,
  FFT_RX_WAIT_HEAD1,
  FFT_RX_READ_FIXED,
  FFT_RX_READ_REST
} FftUartRxState;

static FftUartState fft_uart_state;
static FftUartRxState fft_rx_state;
static uint8_t fft_rx_buf[FFT_UART_FRAME_MAX_LEN];
static uint16_t fft_rx_pos;
static uint16_t fft_rx_expected_len;

static void FftUart_ResetRx(void)
{
  fft_rx_state = FFT_RX_WAIT_HEAD0;
  fft_rx_pos = 0U;
  fft_rx_expected_len = 0U;
}

static void FftUart_UpdateRxSnapshot(void)
{
  fft_uart_state.rx_state_snapshot = (uint8_t)fft_rx_state;
  fft_uart_state.rx_pos_snapshot = fft_rx_pos;
  fft_uart_state.rx_expected_len_snapshot = fft_rx_expected_len;
  fft_uart_state.len_l_snapshot = fft_rx_buf[7];
  fft_uart_state.len_h_snapshot = fft_rx_buf[8];
}

static void FftUart_RecordRxDebugByte(uint8_t byte)
{
  fft_uart_state.last_byte = byte;
  fft_uart_state.rx_debug_buf[fft_uart_state.rx_debug_pos++] = byte;
  if (fft_uart_state.rx_debug_pos >= FFT_UART_RX_DEBUG_LEN)
  {
    fft_uart_state.rx_debug_pos = 0U;
  }
  fft_uart_state.rx_debug_count++;
}

static void FftUart_PutU16(uint8_t *frame, uint16_t *pos, uint16_t value)
{
  frame[(*pos)++] = (uint8_t)(value & 0xFFU);
  frame[(*pos)++] = (uint8_t)((value >> 8) & 0xFFU);
}

static void FftUart_PutU32(uint8_t *frame, uint16_t *pos, uint32_t value)
{
  frame[(*pos)++] = (uint8_t)(value & 0xFFUL);
  frame[(*pos)++] = (uint8_t)((value >> 8) & 0xFFUL);
  frame[(*pos)++] = (uint8_t)((value >> 16) & 0xFFUL);
  frame[(*pos)++] = (uint8_t)((value >> 24) & 0xFFUL);
}

static void FftUart_PutU64(uint8_t *frame, uint16_t *pos, uint64_t value)
{
  for (uint8_t i = 0U; i < 8U; i++)
  {
    frame[(*pos)++] = (uint8_t)((value >> (8U * i)) & 0xFFULL);
  }
}

static uint16_t FftUart_GetU16(const uint8_t *data)
{
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t FftUart_GetU32(const uint8_t *data)
{
  return (uint32_t)data[0] |
         ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) |
         ((uint32_t)data[3] << 24);
}

static int32_t FftUart_GetI32(const uint8_t *data)
{
  return (int32_t)FftUart_GetU32(data);
}

static uint64_t FftUart_GetU64(const uint8_t *data)
{
  uint64_t value = 0ULL;

  for (uint8_t i = 0U; i < 8U; i++)
  {
    value |= ((uint64_t)data[i]) << (8U * i);
  }

  return value;
}

static void FftUart_ParseResult(const uint8_t *frame)
{
  const uint8_t *payload = &frame[FFT_UART_FIXED_HEADER_LEN];
  FftUartResult result;

  result.sweep_id = FftUart_GetU16(&payload[0]);
  result.point_id = FftUart_GetU16(&payload[2]);
  result.frequency_mHz = FftUart_GetU64(&payload[4]);
  result.dds_ftw = FftUart_GetU32(&payload[12]);
  result.sample_rate_hz = FftUart_GetU32(&payload[16]);
  result.fft_length = FftUart_GetU16(&payload[20]);
  result.bin_index = FftUart_GetU16(&payload[22]);
  result.real = FftUart_GetI32(&payload[24]);
  result.imag = FftUart_GetI32(&payload[28]);
  result.block_exponent = payload[32];
  result.otr_count = FftUart_GetU16(&payload[33]);
  result.status_word = FftUart_GetU16(&payload[35]);
  result.seq = frame[6];

  fft_uart_state.last_cmd = frame[4];
  fft_uart_state.last_flags = frame[5];
  fft_uart_state.last_seq = frame[6];
  fft_uart_state.last_result = result;
  fft_uart_state.has_result = 1U;
  fft_uart_state.rx_count++;
}

static uint8_t FftUart_ValidateAndParse(void)
{
  uint16_t payload_len = FftUart_GetU16(&fft_rx_buf[7]);
  uint16_t crc_expected;
  uint16_t crc_actual;

  if ((fft_rx_buf[fft_rx_expected_len - 2U] != FFT_UART_TAIL0) ||
      (fft_rx_buf[fft_rx_expected_len - 1U] != FFT_UART_TAIL1))
  {
    fft_uart_state.error_count++;
    fft_uart_state.tail_error_count++;
    return 0U;
  }

  crc_expected = FftUart_GetU16(&fft_rx_buf[FFT_UART_CRC_OFFSET + payload_len]);
  crc_actual = FftUart_Crc16CcittFalse(&fft_rx_buf[2], (uint16_t)(7U + payload_len));
  if (crc_expected != crc_actual)
  {
    fft_uart_state.error_count++;
    fft_uart_state.crc_error_count++;
    return 0U;
  }

  if ((fft_rx_buf[2] != FFT_UART_VERSION) ||
      (fft_rx_buf[3] != FFT_UART_TARGET_AD9226) ||
      (fft_rx_buf[4] != FFT_UART_CMD_RESULT) ||
      (payload_len != FFT_UART_RESULT_PAYLOAD_LEN))
  {
    fft_uart_state.error_count++;
    return 0U;
  }

  FftUart_ParseResult(fft_rx_buf);
  return 1U;
}

void FftUart_Init(void)
{
  fft_uart_state.last_cmd = 0U;
  fft_uart_state.last_seq = 0U;
  fft_uart_state.last_flags = 0U;
  fft_uart_state.has_result = 0U;
  fft_uart_state.rx_count = 0UL;
  fft_uart_state.error_count = 0UL;
  fft_uart_state.crc_error_count = 0UL;
  fft_uart_state.len_error_count = 0UL;
  fft_uart_state.tail_error_count = 0UL;
  for (uint8_t i = 0U; i < FFT_UART_RX_DEBUG_LEN; i++)
  {
    fft_uart_state.rx_debug_buf[i] = 0U;
  }
  fft_uart_state.rx_debug_pos = 0U;
  fft_uart_state.rx_debug_count = 0UL;
  fft_uart_state.last_byte = 0U;
  fft_uart_state.rx_state_snapshot = 0U;
  fft_uart_state.rx_pos_snapshot = 0U;
  fft_uart_state.rx_expected_len_snapshot = 0U;
  fft_uart_state.len_l_snapshot = 0U;
  fft_uart_state.len_h_snapshot = 0U;
  FftUart_ResetRx();
  FftUart_UpdateRxSnapshot();
}

uint16_t FftUart_Crc16CcittFalse(const uint8_t *data, uint16_t len)
{
  uint16_t crc = 0xFFFFU;

  while (len-- != 0U)
  {
    crc ^= (uint16_t)(*data++) << 8;
    for (uint8_t i = 0U; i < 8U; i++)
    {
      if ((crc & 0x8000U) != 0U)
      {
        crc = (uint16_t)((crc << 1) ^ 0x1021U);
      }
      else
      {
        crc = (uint16_t)(crc << 1);
      }
    }
  }

  return crc;
}

uint16_t FftUart_BuildMeasureFrame(uint8_t *frame, uint16_t frame_capacity,
                                   const FftUartMeasureRequest *request,
                                   uint8_t seq)
{
  uint16_t pos = 0U;
  uint16_t crc;

  if ((frame == 0) || (request == 0) ||
      (frame_capacity < (13U + FFT_UART_MEASURE_PAYLOAD_LEN)))
  {
    return 0U;
  }

  frame[pos++] = FFT_UART_HEAD0;
  frame[pos++] = FFT_UART_HEAD1;
  frame[pos++] = FFT_UART_VERSION;
  frame[pos++] = FFT_UART_TARGET_AD9226;
  frame[pos++] = FFT_UART_CMD_MEASURE;
  frame[pos++] = FFT_UART_MEASURE_FLAGS;
  frame[pos++] = seq;
  FftUart_PutU16(frame, &pos, FFT_UART_MEASURE_PAYLOAD_LEN);
  FftUart_PutU16(frame, &pos, request->sweep_id);
  FftUart_PutU16(frame, &pos, request->point_id);
  FftUart_PutU64(frame, &pos, request->frequency_mHz);
  FftUart_PutU32(frame, &pos, request->dds_ftw);
  FftUart_PutU16(frame, &pos, request->target_bin);
  FftUart_PutU32(frame, &pos, request->settle_us);
  frame[pos++] = request->average_count;
  frame[pos++] = request->measure_flags;

  crc = FftUart_Crc16CcittFalse(&frame[2], (uint16_t)(pos - 2U));
  FftUart_PutU16(frame, &pos, crc);
  frame[pos++] = FFT_UART_TAIL0;
  frame[pos++] = FFT_UART_TAIL1;

  return pos;
}

uint8_t FftUart_PushRxByte(uint8_t byte)
{
  uint8_t status = 0U;

  FftUart_RecordRxDebugByte(byte);

  switch (fft_rx_state)
  {
  case FFT_RX_WAIT_HEAD0:
    if (byte == FFT_UART_HEAD0)
    {
      fft_rx_buf[0] = byte;
      fft_rx_pos = 1U;
      fft_rx_state = FFT_RX_WAIT_HEAD1;
      status |= FFT_UART_RX_CONSUMED;
    }
    break;

  case FFT_RX_WAIT_HEAD1:
    status |= FFT_UART_RX_CONSUMED;
    if (byte == FFT_UART_HEAD1)
    {
      fft_rx_buf[fft_rx_pos++] = byte;
      fft_rx_state = FFT_RX_READ_FIXED;
    }
    else if (byte == FFT_UART_HEAD0)
    {
      fft_rx_buf[0] = byte;
      fft_rx_pos = 1U;
    }
    else
    {
      FftUart_ResetRx();
    }
    break;

  case FFT_RX_READ_FIXED:
    status |= FFT_UART_RX_CONSUMED;
    fft_rx_buf[fft_rx_pos++] = byte;
    if (fft_rx_pos >= FFT_UART_FIXED_HEADER_LEN)
    {
      uint16_t payload_len = FftUart_GetU16(&fft_rx_buf[7]);
      if (payload_len > FFT_UART_MAX_PAYLOAD_LEN)
      {
        fft_uart_state.error_count++;
        fft_uart_state.len_error_count++;
        FftUart_ResetRx();
      }
      else
      {
        fft_rx_expected_len = (uint16_t)(13U + payload_len);
        fft_rx_state = FFT_RX_READ_REST;
      }
    }
    break;

  case FFT_RX_READ_REST:
    status |= FFT_UART_RX_CONSUMED;
    if (fft_rx_pos < FFT_UART_FRAME_MAX_LEN)
    {
      fft_rx_buf[fft_rx_pos++] = byte;
    }
    else
    {
      fft_uart_state.error_count++;
      fft_uart_state.len_error_count++;
      FftUart_ResetRx();
      break;
    }

    if (fft_rx_pos >= fft_rx_expected_len)
    {
      if (FftUart_ValidateAndParse() != 0U)
      {
        status |= FFT_UART_RX_RESULT_READY;
      }
      FftUart_ResetRx();
    }
    break;

  default:
    FftUart_ResetRx();
    break;
  }

  FftUart_UpdateRxSnapshot();
  return status;
}

uint8_t FftUart_TakeResult(FftUartResult *result)
{
  if ((result == 0) || (fft_uart_state.has_result == 0U))
  {
    return 0U;
  }

  *result = fft_uart_state.last_result;
  fft_uart_state.has_result = 0U;
  return 1U;
}

FftUartState FftUart_GetState(void)
{
  return fft_uart_state;
}

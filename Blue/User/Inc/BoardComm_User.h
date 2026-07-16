#ifndef _BOARD_COMM_USER_H_
#define _BOARD_COMM_USER_H_

#include "usart.h"

/*
 * Blue slave-side board UART communication.
 *
 * Wiring:
 *   MergeBlack PB10 / USART3_TX -> Blue PA10 / USART1_RX
 *   MergeBlack PB11 / USART3_RX <- Blue PA9  / USART1_TX
 *   GND                         <-> GND
 *
 * UART: 115200 bps, 8 data bits, 1 stop bit, no parity, no flow control.
 *
 * V2 frame:
 *   D3 91 VER DST SRC CMD FLAGS SEQ LEN PAYLOAD CRC16 91 D3
 * CRC16/CCITT-FALSE covers VER through the end of PAYLOAD.
 */

#define BOARD_COMM_HEAD1        0xD3U
#define BOARD_COMM_HEAD2        0x91U
#define BOARD_COMM_TAIL1        0x91U
#define BOARD_COMM_TAIL2        0xD3U
#define BOARD_COMM_VERSION      0x02U
#define BOARD_COMM_NODE_BLACK   0x01U
#define BOARD_COMM_NODE_BLUE    0x02U
#define BOARD_COMM_NODE_BROADCAST 0xFFU
#define BOARD_COMM_FLAG_ACK_REQ 0x01U
#define BOARD_COMM_FLAG_RESPONSE 0x02U
#define BOARD_COMM_FLAG_EVENT   0x04U
#define BOARD_COMM_FLAG_ERROR   0x08U
#define BOARD_COMM_MAX_PAYLOAD  128
#define BOARD_COMM_FRAME_OVERHEAD 15U
#define BOARD_COMM_TX_BUF_SIZE  (BOARD_COMM_MAX_PAYLOAD + BOARD_COMM_FRAME_OVERHEAD)
#define BOARD_COMM_RX_BUF_SIZE  (BOARD_COMM_TX_BUF_SIZE * 3U)
#define BOARD_COMM_TIMEOUT_MS   20

#define BOARD_COMM_CMD_PING     0x02U
#define BOARD_COMM_CMD_SOURCE_STAGE  0x10U
#define BOARD_COMM_CMD_SOURCE_COMMIT 0x11U
#define BOARD_COMM_CMD_SOURCE_GET_STATUS 0x12U
#define BOARD_COMM_CMD_SOURCE_STATUS 0x13U
#define BOARD_COMM_CMD_ACK      0x7FU
#define BOARD_COMM_CMD_KEYPAD   0x30U
#define BOARD_COMM_CMD_UI_STATE 0x31U
#define BOARD_COMM_CMD_SYS_STATUS BOARD_COMM_CMD_UI_STATE
#define BOARD_COMM_CMD_SWEEP_POINT   0x30U
#define BOARD_COMM_CMD_SWEEP_RESULT  0x32U
#define BOARD_COMM_CMD_ADC_SAMPLE_REQ   0x40U
#define BOARD_COMM_CMD_ADC_SAMPLE_RESP  0x41U
#define BOARD_COMM_CMD_LCR_EXCITATION_SET   0x48U
#define BOARD_COMM_CMD_LCR_EXCITATION_READY 0x49U
#define BOARD_COMM_CMD_ERROR    0xFF

typedef enum {
  BOARD_COMM_OK = 0,
  BOARD_COMM_ERROR,
  BOARD_COMM_TIMEOUT,
  BOARD_COMM_LENGTH_ERROR,
  BOARD_COMM_CHECKSUM_ERROR
} BoardComm_Status;

typedef struct {
  uint8_t last_cmd;
  uint8_t last_len;
  uint8_t last_src;
  uint8_t last_dst;
  uint8_t last_flags;
  uint16_t last_seq;
  uint8_t last_data[BOARD_COMM_MAX_PAYLOAD];
  BoardComm_Status last_status;
  uint16_t last_rx_size;
  uint32_t uart_error_code;
  uint32_t rx_count;
  uint32_t error_count;
} BoardComm_State;

void BoardComm_Init(void);
BoardComm_Status BoardComm_StartReceiveToIdleIT(void);
BoardComm_Status BoardComm_StopReceiveIT(void);
void BoardComm_HandleRxIdleEvent(UART_HandleTypeDef *huart, uint16_t size);
void BoardComm_ProcessTask(void);
BoardComm_Status BoardComm_Send(uint8_t cmd, const uint8_t *data, uint8_t len);
BoardComm_Status BoardComm_SendV2(uint8_t dst, uint8_t cmd, uint8_t flags,
                                  uint16_t seq, const uint8_t *data, uint8_t len);
BoardComm_Status BoardComm_SendRaw(const uint8_t *frame, uint16_t len, uint32_t timeout);
BoardComm_Status BoardComm_Ping(void);
BoardComm_State BoardComm_GetState(void);
uint8_t BoardComm_TakeKeypadKey(char *key);
const char *BoardComm_StatusText(BoardComm_Status status);

#endif

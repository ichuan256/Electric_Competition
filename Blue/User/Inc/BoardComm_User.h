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
 * Frame format:
 *   A5 5A CMD LEN DATA... CHECKSUM
 *
 * Checksum:
 *   CHECKSUM = CMD ^ LEN ^ DATA[0] ^ DATA[1] ^ ...
 */

#define BOARD_COMM_HEAD1        0xA5
#define BOARD_COMM_HEAD2        0x5A
#define BOARD_COMM_MAX_PAYLOAD  128
#define BOARD_COMM_TX_BUF_SIZE  (BOARD_COMM_MAX_PAYLOAD + 5U)
#define BOARD_COMM_RX_BUF_SIZE  128U
#define BOARD_COMM_TIMEOUT_MS   20

#define BOARD_COMM_CMD_PING     0x01
#define BOARD_COMM_CMD_PONG     0x81
#define BOARD_COMM_CMD_KEYPAD   0x10U
#define BOARD_COMM_CMD_SYS_STATUS    0x20U
#define BOARD_COMM_CMD_SWEEP_POINT   0x30U
#define BOARD_COMM_CMD_SWEEP_RESULT  0x32U
#define BOARD_COMM_CMD_ADC_SAMPLE_REQ   0x40U
#define BOARD_COMM_CMD_ADC_SAMPLE_RESP  0x41U
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
BoardComm_Status BoardComm_SendRaw(const uint8_t *frame, uint16_t len, uint32_t timeout);
BoardComm_Status BoardComm_Ping(void);
BoardComm_State BoardComm_GetState(void);
uint8_t BoardComm_TakeKeypadKey(char *key);
const char *BoardComm_StatusText(BoardComm_Status status);

#endif

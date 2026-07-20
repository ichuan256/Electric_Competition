#ifndef THD_BLUETOOTH_LINK_H
#define THD_BLUETOOTH_LINK_H

#include <stdint.h>

#define THD_BLUETOOTH_RX_CAPTURE_SIZE 32U

extern volatile uint16_t thd_bluetooth_rx_capture[THD_BLUETOOTH_RX_CAPTURE_SIZE];
extern volatile uint16_t thd_bluetooth_rx_capture_count;
extern volatile uint32_t thd_bluetooth_rx_byte_count;
extern volatile uint32_t thd_bluetooth_rx_overflow_count;

void ThdBluetoothLink_init(void);
void ThdBluetoothLink_task(void);
uint16_t ThdBluetoothLink_isVerified(void);

#endif

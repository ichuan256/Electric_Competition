#ifndef M0_BLUETOOTH_LINK_H
#define M0_BLUETOOTH_LINK_H
#include <stdint.h>
#define M0_BLUETOOTH_RX_CAPTURE_SIZE 32U
extern volatile uint8_t m0_bluetooth_rx_capture[M0_BLUETOOTH_RX_CAPTURE_SIZE];
extern volatile uint16_t m0_bluetooth_rx_capture_count;
extern volatile uint32_t m0_bluetooth_rx_byte_count;
extern volatile uint32_t m0_bluetooth_rx_overflow_count;
void M0BluetoothLink_init(void);
void M0BluetoothLink_task(void);
uint8_t M0BluetoothLink_isVerified(void);
#endif

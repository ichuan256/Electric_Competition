#ifndef BLUETOOTH_LINK_USER_H
#define BLUETOOTH_LINK_USER_H

#include "main.h"

typedef enum
{
  BLUETOOTH_LINK_DISCONNECTED = 0,
  BLUETOOTH_LINK_VERIFYING,
  BLUETOOTH_LINK_CONNECTED,
  BLUETOOTH_LINK_PROTOCOL_ERROR
} BluetoothLink_State;

void BluetoothLink_Init(void);
void BluetoothLink_Task(void);
BluetoothLink_State BluetoothLink_GetState(void);

#endif

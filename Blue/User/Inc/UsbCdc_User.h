#ifndef _USB_CDC_USER_H_
#define _USB_CDC_USER_H_

#include "stm32h7xx_hal.h"

uint8_t UsbCdc_Init(void);
void UsbCdc_Task(void);
uint8_t UsbCdc_IsConfigured(void);
uint8_t UsbCdc_ReadLine(char *line, uint16_t capacity);
uint8_t UsbCdc_Write(const char *text);
uint8_t UsbCdc_WriteLine(const char *text);

#endif

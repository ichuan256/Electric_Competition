#include "UsbCdc_User.h"

#include <string.h>
#include "usbd_cdc.h"
#include "usbd_core.h"
#include "usbd_desc.h"

#define USB_CDC_RX_PACKET_SIZE  64U
#define USB_CDC_RX_RING_SIZE    512U
#define USB_CDC_TX_RING_SIZE    4096U

USBD_HandleTypeDef USBD_Device;

static uint8_t usb_rx_packet[USB_CDC_RX_PACKET_SIZE];
static uint8_t usb_rx_ring[USB_CDC_RX_RING_SIZE];
static uint8_t usb_tx_ring[USB_CDC_TX_RING_SIZE];
static uint8_t usb_tx_packet[CDC_DATA_FS_MAX_PACKET_SIZE];
static volatile uint16_t usb_rx_head;
static volatile uint16_t usb_rx_tail;
static volatile uint16_t usb_tx_head;
static volatile uint16_t usb_tx_tail;

static int8_t UsbCdc_ItfInit(void);
static int8_t UsbCdc_ItfDeInit(void);
static int8_t UsbCdc_ItfControl(uint8_t cmd, uint8_t *buffer, uint16_t length);
static int8_t UsbCdc_ItfReceive(uint8_t *buffer, uint32_t *length);

static USBD_CDC_ItfTypeDef usb_cdc_fops = {
  UsbCdc_ItfInit,
  UsbCdc_ItfDeInit,
  UsbCdc_ItfControl,
  UsbCdc_ItfReceive
};

static int8_t UsbCdc_ItfInit(void)
{
  (void)USBD_CDC_SetRxBuffer(&USBD_Device, usb_rx_packet);
  return (int8_t)USBD_OK;
}

static int8_t UsbCdc_ItfDeInit(void)
{
  return (int8_t)USBD_OK;
}

static int8_t UsbCdc_ItfControl(uint8_t cmd, uint8_t *buffer, uint16_t length)
{
  (void)cmd;
  (void)buffer;
  (void)length;
  return (int8_t)USBD_OK;
}

static int8_t UsbCdc_ItfReceive(uint8_t *buffer, uint32_t *length)
{
  uint32_t count = (length != 0) ? *length : 0UL;
  for (uint32_t i = 0UL; i < count; i++)
  {
    uint16_t next = (uint16_t)((usb_rx_head + 1U) % USB_CDC_RX_RING_SIZE);
    if (next == usb_rx_tail)
    {
      break;
    }
    usb_rx_ring[usb_rx_head] = buffer[i];
    usb_rx_head = next;
  }
  (void)USBD_CDC_SetRxBuffer(&USBD_Device, usb_rx_packet);
  (void)USBD_CDC_ReceivePacket(&USBD_Device);
  return (int8_t)USBD_OK;
}

uint8_t UsbCdc_Init(void)
{
  RCC_OscInitTypeDef oscillator = {0};
  RCC_PeriphCLKInitTypeDef peripheral_clock = {0};

  usb_rx_head = usb_rx_tail = 0U;
  usb_tx_head = usb_tx_tail = 0U;
  oscillator.OscillatorType = RCC_OSCILLATORTYPE_HSI48;
  oscillator.HSI48State = RCC_HSI48_ON;
  if (HAL_RCC_OscConfig(&oscillator) != HAL_OK)
  {
    return 0U;
  }
  peripheral_clock.PeriphClockSelection = RCC_PERIPHCLK_USB;
  peripheral_clock.UsbClockSelection = RCC_USBCLKSOURCE_HSI48;
  if (HAL_RCCEx_PeriphCLKConfig(&peripheral_clock) != HAL_OK)
  {
    return 0U;
  }
  if ((USBD_Init(&USBD_Device, &VCP_Desc, 0U) != USBD_OK) ||
      (USBD_RegisterClass(&USBD_Device, USBD_CDC_CLASS) != USBD_OK) ||
      (USBD_CDC_RegisterInterface(&USBD_Device, &usb_cdc_fops) != USBD_OK) ||
      (USBD_Start(&USBD_Device) != USBD_OK))
  {
    return 0U;
  }
  return 1U;
}

uint8_t UsbCdc_IsConfigured(void)
{
  return (USBD_Device.dev_state == USBD_STATE_CONFIGURED) ? 1U : 0U;
}

uint8_t UsbCdc_Write(const char *text)
{
  uint16_t needed;
  uint16_t free_count;
  if (text == 0)
  {
    return 0U;
  }
  needed = (uint16_t)strlen(text);
  free_count = (usb_tx_tail > usb_tx_head) ?
               (uint16_t)(usb_tx_tail - usb_tx_head - 1U) :
               (uint16_t)(USB_CDC_TX_RING_SIZE - usb_tx_head + usb_tx_tail - 1U);
  if (needed > free_count)
  {
    return 0U;
  }
  while (*text != '\0')
  {
    usb_tx_ring[usb_tx_head] = (uint8_t)*text++;
    usb_tx_head = (uint16_t)((usb_tx_head + 1U) % USB_CDC_TX_RING_SIZE);
  }
  return 1U;
}

uint8_t UsbCdc_WriteLine(const char *text)
{
  return ((UsbCdc_Write(text) != 0U) && (UsbCdc_Write("\r\n") != 0U)) ? 1U : 0U;
}

uint8_t UsbCdc_ReadLine(char *line, uint16_t capacity)
{
  uint16_t cursor = usb_rx_tail;
  uint16_t length = 0U;
  uint8_t found = 0U;
  if ((line == 0) || (capacity < 2U))
  {
    return 0U;
  }
  while (cursor != usb_rx_head)
  {
    uint8_t ch = usb_rx_ring[cursor];
    cursor = (uint16_t)((cursor + 1U) % USB_CDC_RX_RING_SIZE);
    if ((ch == '\r') || (ch == '\n'))
    {
      found = 1U;
      break;
    }
    if (length < (uint16_t)(capacity - 1U))
    {
      line[length++] = (char)ch;
    }
  }
  if (found == 0U)
  {
    return 0U;
  }
  usb_rx_tail = cursor;
  while ((usb_rx_tail != usb_rx_head) &&
         ((usb_rx_ring[usb_rx_tail] == '\r') || (usb_rx_ring[usb_rx_tail] == '\n')))
  {
    usb_rx_tail = (uint16_t)((usb_rx_tail + 1U) % USB_CDC_RX_RING_SIZE);
  }
  line[length] = '\0';
  return 1U;
}

void UsbCdc_Task(void)
{
  USBD_CDC_HandleTypeDef *cdc;
  uint16_t count = 0U;
  if ((UsbCdc_IsConfigured() == 0U) || (usb_tx_tail == usb_tx_head))
  {
    return;
  }
  cdc = (USBD_CDC_HandleTypeDef *)USBD_Device.pClassData;
  if ((cdc == 0) || (cdc->TxState != 0U))
  {
    return;
  }
  while ((usb_tx_tail != usb_tx_head) && (count < CDC_DATA_FS_MAX_PACKET_SIZE))
  {
    usb_tx_packet[count++] = usb_tx_ring[usb_tx_tail];
    usb_tx_tail = (uint16_t)((usb_tx_tail + 1U) % USB_CDC_TX_RING_SIZE);
  }
  (void)USBD_CDC_SetTxBuffer(&USBD_Device, usb_tx_packet, count);
  (void)USBD_CDC_TransmitPacket(&USBD_Device);
}

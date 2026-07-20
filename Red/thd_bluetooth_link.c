#include "thd_bluetooth_link.h"

#include "driverlib.h"
#include "device.h"

#define LINK_BASE                 SCIB_BASE
#define LINK_BAUD                 115200UL
#define LINK_MAX_PAYLOAD          128U
#define LINK_OVERHEAD             15U
#define LINK_FRAME_CAPACITY       (LINK_MAX_PAYLOAD + LINK_OVERHEAD)
#define LINK_HEAD1                0xD3U
#define LINK_HEAD2                0x91U
#define LINK_TAIL1                0x91U
#define LINK_TAIL2                0xD3U
#define LINK_VERSION              0x01U
#define LINK_NODE_BLUE            0x02U
#define LINK_NODE_RED             0x03U
#define LINK_FLAG_RESPONSE        0x02U
#define LINK_CMD_HELLO            0x50U
#define LINK_CMD_ACK              0x7FU
#define LINK_VALID_MASK           0x001FU
#define LINK_RX_RING_SIZE         256U
#define LINK_RX_RING_MASK         (LINK_RX_RING_SIZE - 1U)

static uint16_t rx_frame[LINK_FRAME_CAPACITY];
static uint16_t rx_count;
static uint16_t rx_expected;
static uint16_t link_verified;
static volatile uint16_t rx_ring[LINK_RX_RING_SIZE];
static volatile uint16_t rx_ring_write;
static volatile uint16_t rx_ring_read;
volatile uint32_t thd_bluetooth_rx_byte_count;
volatile uint32_t thd_bluetooth_rx_overflow_count;
volatile uint16_t thd_bluetooth_rx_capture[THD_BLUETOOTH_RX_CAPTURE_SIZE];
volatile uint16_t thd_bluetooth_rx_capture_count;

static uint16_t crc16(const uint16_t *data, uint16_t length)
{
    uint16_t crc = 0xFFFFU;
    uint16_t i, bit;
    for(i = 0U; i < length; i++) {
        crc ^= (data[i] & 0x00FFU) << 8U;
        for(bit = 0U; bit < 8U; bit++) {
            crc = ((crc & 0x8000U) != 0U) ?
                  (uint16_t)((crc << 1U) ^ 0x1021U) : (uint16_t)(crc << 1U);
        }
    }
    return crc;
}

static void writeByte(uint16_t value)
{
    SCI_writeCharBlockingFIFO(LINK_BASE, value & 0x00FFU);
}

static void sendHelloAck(uint16_t sequence, uint16_t status, uint16_t validation)
{
    uint16_t fixed[9];
    uint16_t payload[4];
    uint16_t crc = 0xFFFFU;
    uint16_t i;

    fixed[0] = LINK_VERSION;
    fixed[1] = LINK_NODE_BLUE;
    fixed[2] = LINK_NODE_RED;
    fixed[3] = LINK_CMD_ACK;
    fixed[4] = LINK_FLAG_RESPONSE;
    fixed[5] = sequence & 0x00FFU;
    fixed[6] = sequence >> 8U;
    fixed[7] = 4U;
    fixed[8] = 0U;
    payload[0] = LINK_CMD_HELLO;
    payload[1] = status;
    payload[2] = validation & 0x00FFU;
    payload[3] = validation >> 8U;

    writeByte(LINK_HEAD1);
    writeByte(LINK_HEAD2);
    for(i = 0U; i < 9U; i++) {
        writeByte(fixed[i]);
    }
    /* Recalculate over contiguous logical fields without C28x byte packing. */
    crc = 0xFFFFU;
    for(i = 0U; i < 9U; i++) {
        uint16_t bit;
        crc ^= (fixed[i] & 0x00FFU) << 8U;
        for(bit = 0U; bit < 8U; bit++)
            crc = ((crc & 0x8000U) != 0U) ? (uint16_t)((crc << 1U) ^ 0x1021U) : (uint16_t)(crc << 1U);
    }
    for(i = 0U; i < 4U; i++) {
        uint16_t bit;
        writeByte(payload[i]);
        crc ^= (payload[i] & 0x00FFU) << 8U;
        for(bit = 0U; bit < 8U; bit++)
            crc = ((crc & 0x8000U) != 0U) ? (uint16_t)((crc << 1U) ^ 0x1021U) : (uint16_t)(crc << 1U);
    }
    writeByte(crc);
    writeByte(crc >> 8U);
    writeByte(LINK_TAIL1);
    writeByte(LINK_TAIL2);
}

static void processFrame(void)
{
    uint16_t length = rx_frame[9] | (rx_frame[10] << 8U);
    uint16_t sequence = rx_frame[7] | (rx_frame[8] << 8U);
    uint16_t received_crc = rx_frame[11U + length] | (rx_frame[12U + length] << 8U);
    uint16_t validation = 0U;
    uint16_t payload_ok;

    if((rx_frame[0] == LINK_HEAD1) && (rx_frame[1] == LINK_HEAD2) &&
       (rx_frame[13U + length] == LINK_TAIL1) && (rx_frame[14U + length] == LINK_TAIL2)) validation |= 0x0001U;
    if(rx_frame[2] == LINK_VERSION) validation |= 0x0002U;
    if((rx_frame[3] == LINK_NODE_RED) && (rx_frame[4] == LINK_NODE_BLUE)) validation |= 0x0004U;
    payload_ok = ((rx_frame[5] == LINK_CMD_HELLO) && (length == 8U) &&
                  (rx_frame[11] == LINK_NODE_BLUE) && (rx_frame[12] == LINK_VERSION) &&
                  (rx_frame[13] == 0x80U) && (rx_frame[14] == 0U)) ? 1U : 0U;
    if(payload_ok != 0U) validation |= 0x0008U;
    if(received_crc == crc16(&rx_frame[2], (uint16_t)(9U + length))) validation |= 0x0010U;

    /* A structurally complete HELLO addressed to Red always receives the
     * validation bitmap, including when version, payload, or CRC is wrong. */
    if(((validation & 0x0005U) == 0x0005U) && (rx_frame[5] == LINK_CMD_HELLO)) {
        link_verified = (validation == LINK_VALID_MASK) ? 1U : 0U;
        sendHelloAck(sequence, (link_verified != 0U) ? 0U : 4U, validation);
    }
}

static void parseByte(uint16_t value)
{
    value &= 0x00FFU;
    if(rx_count == 0U) {
        if(value == LINK_HEAD1) rx_frame[rx_count++] = value;
        return;
    }
    if(rx_count == 1U) {
        if(value == LINK_HEAD2) rx_frame[rx_count++] = value;
        else rx_count = (value == LINK_HEAD1) ? 1U : 0U;
        return;
    }
    if(rx_count >= LINK_FRAME_CAPACITY) {
        rx_count = rx_expected = 0U;
        return;
    }
    rx_frame[rx_count++] = value;
    if(rx_count == 11U) {
        uint16_t length = rx_frame[9] | (rx_frame[10] << 8U);
        if(length > LINK_MAX_PAYLOAD) {
            rx_count = rx_expected = 0U;
            return;
        }
        rx_expected = length + LINK_OVERHEAD;
    }
    if((rx_expected != 0U) && (rx_count == rx_expected)) {
        processFrame();
        rx_count = rx_expected = 0U;
    }
}

static __interrupt void scibRxISR(void)
{
    while(SCI_getRxFIFOStatus(LINK_BASE) != SCI_FIFO_RX0) {
        uint16_t value = SCI_readCharNonBlocking(LINK_BASE) & 0x00FFU;
        uint16_t next = (rx_ring_write + 1U) & LINK_RX_RING_MASK;

        thd_bluetooth_rx_byte_count++;
        if(thd_bluetooth_rx_capture_count < THD_BLUETOOTH_RX_CAPTURE_SIZE) {
            thd_bluetooth_rx_capture[thd_bluetooth_rx_capture_count++] = value;
        }
        if(next != rx_ring_read) {
            rx_ring[rx_ring_write] = value;
            rx_ring_write = next;
        }
        else {
            thd_bluetooth_rx_overflow_count++;
        }
    }

    if(SCI_getOverflowStatus(LINK_BASE)) {
        SCI_clearOverflowStatus(LINK_BASE);
        thd_bluetooth_rx_overflow_count++;
    }
    SCI_clearInterruptStatus(LINK_BASE, SCI_INT_RXFF);
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP9);
}

void ThdBluetoothLink_init(void)
{
    uint16_t capture_index;

    GPIO_setMasterCore(19U, GPIO_CORE_CPU1);
    GPIO_setPinConfig(GPIO_19_SCIRXDB);
    GPIO_setDirectionMode(19U, GPIO_DIR_MODE_IN);
    GPIO_setPadConfig(19U, GPIO_PIN_TYPE_PULLUP);
    GPIO_setQualificationMode(19U, GPIO_QUAL_ASYNC);
    GPIO_setMasterCore(18U, GPIO_CORE_CPU1);
    GPIO_setPinConfig(GPIO_18_SCITXDB);
    GPIO_setDirectionMode(18U, GPIO_DIR_MODE_OUT);
    GPIO_setPadConfig(18U, GPIO_PIN_TYPE_STD);

    SCI_performSoftwareReset(LINK_BASE);
    SCI_setConfig(LINK_BASE, DEVICE_LSPCLK_FREQ, LINK_BAUD,
                  SCI_CONFIG_WLEN_8 | SCI_CONFIG_STOP_ONE | SCI_CONFIG_PAR_NONE);
    SCI_resetChannels(LINK_BASE);
    SCI_resetRxFIFO(LINK_BASE);
    SCI_resetTxFIFO(LINK_BASE);
    SCI_enableFIFO(LINK_BASE);
    SCI_enableModule(LINK_BASE);
    SCI_performSoftwareReset(LINK_BASE);

    rx_ring_write = 0U;
    rx_ring_read = 0U;
    thd_bluetooth_rx_byte_count = 0UL;
    thd_bluetooth_rx_overflow_count = 0UL;
    thd_bluetooth_rx_capture_count = 0U;
    for(capture_index = 0U; capture_index < THD_BLUETOOTH_RX_CAPTURE_SIZE;
        capture_index++) {
        thd_bluetooth_rx_capture[capture_index] = 0U;
    }
    rx_count = rx_expected = link_verified = 0U;

    SCI_setFIFOInterruptLevel(LINK_BASE, SCI_FIFO_TX0, SCI_FIFO_RX1);
    SCI_clearInterruptStatus(LINK_BASE, SCI_INT_RXFF);
    Interrupt_register(INT_SCIB_RX, &scibRxISR);
    Interrupt_enable(INT_SCIB_RX);
    SCI_enableInterrupt(LINK_BASE, SCI_INT_RXFF);
}

void ThdBluetoothLink_task(void)
{
    while( rx_ring_read != rx_ring_write) {
        uint16_t value = rx_ring[rx_ring_read];
        rx_ring_read = (rx_ring_read + 1U) & LINK_RX_RING_MASK;
        parseByte(value);
    }
}

uint16_t ThdBluetoothLink_isVerified(void)
{
    return link_verified;
}

#include "m0_bluetooth_link.h"
#include "ti_msp_dl_config.h"

#define MAX_PAYLOAD 128U
#define OVERHEAD 15U
#define FRAME_SIZE (MAX_PAYLOAD + OVERHEAD)
#define H1 0xD3U
#define H2 0x91U
#define VERSION 0x01U
#define BLUE 0x02U
#define RED 0x03U
#define HELLO 0x50U
#define ACK 0x7FU
#define VALID_MASK 0x001FU
#define RING_SIZE 256U
#define RING_MASK 255U

static uint8_t frame[FRAME_SIZE];
static uint16_t frame_count, frame_expected;
static uint8_t verified;
static volatile uint8_t ring[RING_SIZE];
static volatile uint16_t ring_write, ring_read;
volatile uint8_t m0_bluetooth_rx_capture[M0_BLUETOOTH_RX_CAPTURE_SIZE];
volatile uint16_t m0_bluetooth_rx_capture_count;
volatile uint32_t m0_bluetooth_rx_byte_count;
volatile uint32_t m0_bluetooth_rx_overflow_count;

static uint16_t crc16(const uint8_t *data, uint16_t length)
{
    uint16_t crc = 0xFFFFU, i;
    uint8_t bit;
    for (i = 0U; i < length; i++) {
        crc ^= (uint16_t)data[i] << 8U;
        for (bit = 0U; bit < 8U; bit++)
            crc = (crc & 0x8000U) ? (uint16_t)((crc << 1U) ^ 0x1021U) : (uint16_t)(crc << 1U);
    }
    return crc;
}

static void sendAck(uint16_t sequence, uint8_t status, uint16_t validation)
{
    uint8_t tx[19], i;
    uint16_t crc;
    tx[0]=H1; tx[1]=H2; tx[2]=VERSION; tx[3]=BLUE; tx[4]=RED;
    tx[5]=ACK; tx[6]=0x02U; tx[7]=(uint8_t)sequence; tx[8]=(uint8_t)(sequence>>8U);
    tx[9]=4U; tx[10]=0U; tx[11]=HELLO; tx[12]=status;
    tx[13]=(uint8_t)validation; tx[14]=(uint8_t)(validation>>8U);
    crc=crc16(&tx[2],13U); tx[15]=(uint8_t)crc; tx[16]=(uint8_t)(crc>>8U);
    tx[17]=H2; tx[18]=H1;
    for(i=0U;i<sizeof(tx);i++) DL_UART_Main_transmitDataBlocking(UART_0_INST,tx[i]);
}

static void processFrame(void)
{
    uint16_t len=(uint16_t)frame[9]|((uint16_t)frame[10]<<8U);
    uint16_t seq=(uint16_t)frame[7]|((uint16_t)frame[8]<<8U);
    uint16_t got=(uint16_t)frame[11U+len]|((uint16_t)frame[12U+len]<<8U);
    uint16_t v=0U;
    if(frame[0]==H1 && frame[1]==H2 && frame[13U+len]==H2 && frame[14U+len]==H1) v|=1U;
    if(frame[2]==VERSION) v|=2U;
    if(frame[3]==RED && frame[4]==BLUE) v|=4U;
    if(frame[5]==HELLO && len==8U && frame[11]==BLUE && frame[12]==VERSION && frame[13]==0x80U && frame[14]==0U) v|=8U;
    if(got==crc16(&frame[2],(uint16_t)(9U+len))) v|=0x10U;
    if((v&5U)==5U && frame[5]==HELLO) {
        verified=(v==VALID_MASK)?1U:0U;
        sendAck(seq,verified?0U:4U,v);
    }
}

static void parse(uint8_t value)
{
    if(frame_count==0U){if(value==H1)frame[frame_count++]=value;return;}
    if(frame_count==1U){if(value==H2)frame[frame_count++]=value;else frame_count=(value==H1)?1U:0U;return;}
    if(frame_count>=FRAME_SIZE){frame_count=frame_expected=0U;return;}
    frame[frame_count++]=value;
    if(frame_count==11U){
        uint16_t len=(uint16_t)frame[9]|((uint16_t)frame[10]<<8U);
        if(len>MAX_PAYLOAD){frame_count=frame_expected=0U;return;}
        frame_expected=len+OVERHEAD;
    }
    if(frame_expected!=0U && frame_count==frame_expected){processFrame();frame_count=frame_expected=0U;}
}

void M0BluetoothLink_init(void)
{
    uint16_t i;
    ring_write=ring_read=frame_count=frame_expected=0U; verified=0U;
    m0_bluetooth_rx_byte_count=m0_bluetooth_rx_overflow_count=0U;
    m0_bluetooth_rx_capture_count=0U;
    for(i=0U;i<M0_BLUETOOTH_RX_CAPTURE_SIZE;i++)m0_bluetooth_rx_capture[i]=0U;
    NVIC_ClearPendingIRQ(UART_0_INST_INT_IRQN);
    NVIC_EnableIRQ(UART_0_INST_INT_IRQN);
}

void M0BluetoothLink_task(void)
{
    while(ring_read!=ring_write){uint8_t value=ring[ring_read];ring_read=(ring_read+1U)&RING_MASK;parse(value);}
}

uint8_t M0BluetoothLink_isVerified(void){return verified;}

void UART_0_INST_IRQHandler(void)
{
    if(DL_UART_Main_getPendingInterrupt(UART_0_INST)==DL_UART_MAIN_IIDX_RX){
        while(!DL_UART_Main_isRXFIFOEmpty(UART_0_INST)){
            uint8_t value=DL_UART_Main_receiveData(UART_0_INST);
            uint16_t next=(ring_write+1U)&RING_MASK;
            m0_bluetooth_rx_byte_count++;
            if(m0_bluetooth_rx_capture_count<M0_BLUETOOTH_RX_CAPTURE_SIZE)m0_bluetooth_rx_capture[m0_bluetooth_rx_capture_count++]=value;
            if(next!=ring_read){ring[ring_write]=value;ring_write=next;}else m0_bluetooth_rx_overflow_count++;
        }
    }
}

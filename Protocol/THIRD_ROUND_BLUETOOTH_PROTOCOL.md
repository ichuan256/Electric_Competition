# Third-round Blue-Red Bluetooth link

This note is subordinate to `Overview/THD-Overview.md`. The wire protocol is
THD protocol V1; the earlier temporary `A5 5A`/version-3 proposal is retired
and must not be used by either board.

## Hardware

- UART: 115200 bit/s, 8N1, no flow control, 3.3 V logic.
- Blue: UART4, PA0 TX and PA1 RX; PC13 reads Bluetooth STATE with pulldown.
- Blue UART4 RX uses DMA1 Stream3 in circular mode; application code consumes
  the DMA ring and does not poll RXNE/RDR directly.
- Red LAUNCHXL-F28379D: SCI-B, GPIO18 TX and GPIO19 RX.
- Cross TX/RX and connect signal ground.
- Blue USART1 remains owned by `BoardComm_User`; USART2 remains owned by
  `FpgaUart_User`. The Bluetooth link must not install callbacks or DMA on them.

## Frame

```text
D3 91 VER DST SRC CMD FLAGS SEQ_L SEQ_H LEN_L LEN_H PAYLOAD CRC_L CRC_H 91 D3
```

- Version: `01`; Blue node `02`; Red node `03`.
- CRC16/CCITT-FALSE covers VER through payload, polynomial `0x1021`, initial
  `0xFFFF`, no reflection/final XOR; CRC is sent low byte first.
- HELLO command `50`; ACK command `7F`; RESPONSE flag is bit 1.

## Connection verification

Temporary wired-test override: Blue currently sends HELLO every 500 ms without
using STATE as a transmit condition. The STATE input and debounce path remain
in the firmware. Set `BT_CONTINUOUS_TEST_MODE` to 0 to restore the formal
low-to-high edge behavior below.

Blue debounces STATE for 30 ms. Stable low shows red `NOT CONNECTED` and clears
the logical connection. Exactly once on a stable low-to-high edge, Blue sends:

```text
HELLO payload: 02 01 80 00 00 00 00 00
```

Red validates header/tail, protocol version, nodes, HELLO payload, and CRC. It
returns a four-byte ACK payload:

```text
50 STATUS DETAIL_L DETAIL_H
```

`DETAIL` bits are header/tail, version, nodes, HELLO payload, and CRC in bits
0..4 respectively. Success requires `STATUS=0` and `DETAIL=001F`. Blue also
validates the ACK frame, CRC, nodes, sequence, and acknowledged command. Only
then does it show green `CONNECTED`. The ACK timeout is 1000 ms; there is no
continuous retry while STATE remains high. A new attempt requires STATE to go
low and then high again.

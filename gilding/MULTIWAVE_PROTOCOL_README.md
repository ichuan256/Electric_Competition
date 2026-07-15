# FPGA 双 DAC 通信实现（V2）

本工程的 Blue→FPGA 链路已经切换到
[`Protocol/UNIFIED_DUAL_SOURCE_PROTOCOL_V2.md`](../Protocol/UNIFIED_DUAL_SOURCE_PROTOCOL_V2.md)
定义的 V2 帧。串口参数为 115200 baud、8N1、3.3 V TTL、共地；多字节字段均为
小端序，校验使用 CRC-16/CCITT-FALSE。

```text
D3 91 VER DST SRC CMD FLAGS SEQ_L SEQ_H LEN_L LEN_H PAYLOAD CRC_L CRC_H 91 D3
```

FPGA 节点为 `0x10`，Blue 节点为 `0x02`，版本固定为 `0x02`。CRC覆盖 `VER` 至
PAYLOAD 最后一个字节。当前 FPGA 命令负载上限为80字节，帧间超时为20 ms。

## 已实现命令

- `0x20 FPGA_CHANNEL_STAGE`：按协议暂存 DAC1 或 DAC2 的配置，不改变活动输出。
- `0x21 FPGA_COMMIT`：检查 transaction_id 后应用 channel_mask 指定的通道。
- `0x7F ACK/NACK`：响应负载固定8字节，与 Blue 的 `FpgaUart_User.c` 一致。

ACK负载：

```text
REQUEST_CMD STATUS DETAIL_L DETAIL_H TRANSACTION_L TRANSACTION_H MASK_L MASK_H
```

响应帧复用请求的SEQ。最近一个相同 `SRC+SEQ+CMD` 的重发请求只重发缓存响应，
不会再次提交或再次清零相位。

## 暂存与提交

每个通道拥有独立影子配置、transaction_id和有效标志。`CHANNEL_STAGE`成功只返回
暂存ACK；活动波形不变。`COMMIT`要求mask中的所有通道均已用同一transaction_id
暂存，否则返回 `NOT_STAGED(0x09)`。

当mask为 `0x03` 时，DAC1和DAC2的配置翻转握手在同一个系统时钟周期产生，并在
共享100 MHz采样时钟域同步捕获。两路实时混合器的相位累加器在同一采样边沿重启。

两路均支持：

- 0～4个分量；
- 实时DDS模式；
- 16～4096点周期缓存模式；
- 独立FTW、相位、幅度、占空比和14位补码偏置；
- 四槽宽位累加后饱和到 `-8192..8191`，不再补码回绕。

## 波形映射

整机仍由 Black 板外部DDS产生正弦，FPGA负责其他波形和偏置。因此V2波形编码在
FPGA内部映射如下：

| V2 waveform | 含义 | FPGA内部 |
|---:|---|---:|
| 0 | OFF | 0 |
| 1 | SINE | 0（交由外部DDS） |
| 2 | SQUARE | 1 |
| 3 | TRIANGLE | 2 |
| 4 | SAW | 3 |

如果一个事务同时包含正弦和其他波形，正弦槽在FPGA中输出零码，其他槽照常合成，
最后与外部DDS的模拟正弦相加，避免重复生成正弦。

## 校验和诊断

- CRC、帧尾或字段非法时不修改影子区之外的活动输出。
- CRC错误返回 `BAD_CRC(0x03)`。
- 版本、节点、长度、字段和未暂存提交分别返回明确NACK。
- LED1在收到任意UART字节后持续闪烁。
- PS_LED0在收到CRC和基础头字段正确的完整V2帧后持续闪烁。

当前顶层只实例化V2解析器，旧 `A5 5A ... XOR ... 5A A5` 帧不再控制DAC。

# FFT专用UART协议V1

本协议只服务于AD9226采样和FFT测量，不复用DAC波形协议。旧协议帧头为
`A5 5A`、帧尾为`5A A5`；FFT协议帧头为`F9 26`、帧尾为`62 9F`，两者可在
未来共用同一UART接收线时明确区分。

## 公共帧

```text
F9 26 VER TARGET CMD FLAGS SEQ LEN_L LEN_H PAYLOAD CRC_L CRC_H 62 9F
```

- `VER`固定为`01`。
- `TARGET`固定为`A2`，表示AD9226/FFT子系统。
- 多字节字段使用小端序。
- CRC使用CRC-16/CCITT-FALSE：初值`FFFF`、多项式`1021`、不反射、无最终异或。
- CRC覆盖`VER`到`PAYLOAD`最后一个字节，不包含帧头、CRC自身和帧尾。
- 波特率固定115200、8N1、3.3V UART并共地。

公共帧中各部分的字节偏移如下：

| 偏移 | 长度 | 内容 |
|---:|---:|---|
| 0 | 2 | 帧头`F9 26` |
| 2 | 1 | `VER` |
| 3 | 1 | `TARGET` |
| 4 | 1 | `CMD` |
| 5 | 1 | `FLAGS` |
| 6 | 1 | `SEQ` |
| 7 | 2 | `LEN`，小端序，仅表示PAYLOAD长度 |
| 9 | LEN | `PAYLOAD` |
| 9+LEN | 2 | CRC，小端序，即先发`CRC_L` |
| 11+LEN | 2 | 帧尾`62 9F` |

接收端必须检查`LEN`再等待完整帧，不允许依靠字节间隔判定帧结束。V1最大合法
PAYLOAD为37字节，建议单片机接收缓冲区至少保留64字节。CRC或帧尾错误时丢弃
当前候选帧，并继续搜索下一组`F9 26`。

## 单点测量命令 0x11

单片机先设置DDS并等待DDS输出稳定，然后发送本命令。PAYLOAD固定24字节：

| 偏移 | 类型 | 字段 |
|---:|---|---|
| 0 | uint16 | `sweep_id` |
| 2 | uint16 | `point_id` |
| 4 | uint64 | `frequency_mHz`，DDS实际输出频率 |
| 12 | uint32 | `dds_ftw`，DDS实际使用的频率控制字 |
| 16 | uint16 | `target_bin`，范围0~2048 |
| 18 | uint32 | `settle_us`，FPGA额外等待时间 |
| 22 | uint8 | `average_count`，V1必须为1 |
| 23 | uint8 | `measure_flags`，V1填0 |

公共头建议：

```text
VER=01 TARGET=A2 CMD=11 FLAGS=01 SEQ=递增值 LEN=18 00
```

## 单点结果 0x91

FPGA完成4096点采样和FFT后只回传一次结果。PAYLOAD固定37字节：

| 偏移 | 类型 | 字段 |
|---:|---|---|
| 0 | uint16 | `sweep_id` |
| 2 | uint16 | `point_id` |
| 4 | uint64 | `frequency_mHz`原样回显 |
| 12 | uint32 | `dds_ftw`原样回显 |
| 16 | uint32 | `sample_rate_hz`，固定50000000 |
| 20 | uint16 | `fft_length`，固定4096 |
| 22 | uint16 | `bin_index` |
| 24 | int32 | 目标频点实部，16位FFT结果符号扩展 |
| 28 | int32 | 目标频点虚部，16位FFT结果符号扩展 |
| 32 | uint8 | `block_exponent` |
| 33 | uint16 | 采样期间OTR计数 |
| 35 | uint16 | 状态字 |

状态字：bit0结果有效，bit1未找到目标频点，bit2发生OTR，bit3 FFT帧错误。

## 扫频频率统一规则

ADC采样率固定为50MHz，FFT长度为4096，因此频点间隔为：

```text
delta_f = 50_000_000 / 4096 = 12_207.03125 Hz
```

为了避免矩形窗下的频谱泄漏，单片机优先选择整数FFT频点：

```text
target_bin = k
DDS目标频率 = k * 50_000_000 / 4096
```

单片机必须把DDS最终实际使用的`frequency_mHz`和`dds_ftw`都写入测量命令。
FPGA不会自行修改DDS频率，只将它们与本次采样结果绑定并原样回传。单片机收到
结果后必须同时核对`sweep_id`、`point_id`、`frequency_mHz`和`dds_ftw`，然后才
能切换到下一个扫频点。

V1采用严格的一发一收：收到当前点的`0x91`结果之前，不发送下一条`0x11`命令。

## CRC参考实现

```c
uint16_t fft_crc16_ccitt_false(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    while (len--) {
        crc ^= (uint16_t)(*data++) << 8;
        for (unsigned i = 0; i < 8; ++i) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}
```

CRC输入从偏移2的`VER`开始，到PAYLOAD最后一个字节结束。线上顺序为低字节在前。

## 黄金测试帧

以下命令用于验证单片机的序列化和CRC实现：

- `sweep_id=1`
- `point_id=0`
- `frequency_mHz=1000976563`，对应约1.000976563MHz
- `dds_ftw=0x12345678`，这里只作为通信测试值
- `target_bin=82`
- `settle_us=1000`
- `average_count=1`
- `SEQ=1`

完整帧：

```text
F9 26 01 A2 11 01 01 18 00
01 00 00 00 B3 B0 A9 3B 00 00 00 00 78 56 34 12
52 00 E8 03 00 00 01 00
A3 C4 62 9F
```

CRC数值为`0xC4A3`，因此线上字节顺序是`A3 C4`。实际测量时必须把测试用的
`0x12345678`换成DDS真正使用的FTW。

## 错误处理规则

- FPGA只对完整、帧尾正确且CRC正确的`0x11`命令启动测量。
- `VER`、`TARGET`、`CMD`或`LEN`不合法时不启动FFT，也不产生结果帧。
- V1不支持命令排队；单片机必须等待当前`SEQ`对应的`0x91`。
- 单片机必须同时核对`SEQ`、`sweep_id`、`point_id`、`frequency_mHz`和`dds_ftw`。
- 状态字bit0为0时，本频点不得作为有效扫频结果。
- 状态字bit2或OTR计数非0时，应标记输入过量程并由单片机调整前端。

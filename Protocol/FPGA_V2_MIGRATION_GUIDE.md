# FPGA V2 通信改动与联调方案

本文用于指导 FPGA 工程把 Blue→FPGA 通信从旧 `A5 5A ... XOR` 帧迁移到
`UNIFIED_DUAL_SOURCE_PROTOCOL_V2.md` 第 8 章定义的 V2 数字波形协议。

## 1. MCU 侧已经采用的接口

- UART：默认 115200 baud、8N1、3.3 V TTL，共地。
- Blue 节点：`SRC=0x02`。
- FPGA 节点：`DST=0x10`。
- 帧格式：

```text
D3 91 02 DST SRC CMD FLAGS SEQ_L SEQ_H LEN_L LEN_H
PAYLOAD...
CRC_L CRC_H 91 D3
```

- CRC：CRC-16/CCITT-FALSE，`poly=0x1021`、`init=0xFFFF`、不反射、
  `xorout=0x0000`；覆盖 `VER` 到 PAYLOAD 最后一个字节。
- Blue 会先发送 `FPGA_CHANNEL_STAGE(0x20)`，收到成功 ACK 后才发送
  `FPGA_COMMIT(0x21)`。
- Blue 对当前帧最多重发 2 次，每次 ACK 超时 100 ms。
- ACK 必须回显请求帧的 `SEQ`；否则 Blue 会把它当作无关或过期响应。
- 旧 `A5 5A` 数据帧和旧 `5A C0 00 9A` ACK 不再被 Blue 识别。

## 2. 建议的 RTL 模块划分

```text
uart_rx
  -> v2_stream_parser
  -> command_dispatch
       -> channel_shadow[0]
       -> channel_shadow[1]
       -> commit_controller
       -> status_registers
  -> response_fifo
  -> uart_tx

channel_shadow[0/1]
  -> CDC request/ack
  -> waveform_engine[0/1]
       -> realtime DDS / cache player
       -> wide signed mixer
       -> saturator + clipping counter
       -> DAC1 / DAC2
```

UART 时钟域只负责收帧、校验和暂存。100 MHz 采样域负责波形状态切换。
多位配置总线不得直接跨时钟域，应使用稳定保持寄存器加 toggle/握手，或异步 FIFO。

## 3. V2 流式解析器

建议状态机：

```text
SEARCH_D3 -> SEARCH_91 -> READ_FIXED_HEADER -> READ_PAYLOAD
          -> READ_CRC -> READ_TAIL -> VALIDATE -> DISPATCH
```

实现要求：

1. 仅在搜索状态识别 `D3 91`；进入帧内后严格按 `LEN` 计数。
2. FPGA 接收负载上限设为 80 字节，`LEN_H` 当前必须为 0。
3. 帧间超过 20 ms 未收完整帧，立即清空当前解析状态。
4. 按顺序校验版本、目标节点、保留标志位、长度、CRC、帧尾。
5. 校验失败不得修改任一通道影子配置或活动配置。
6. CRC 或格式错误可更新错误计数；若固定头已足以取得 `SRC/SEQ/CMD`，可返回 NACK。

## 4. FPGA_CHANNEL_STAGE（CMD 0x20）

固定负载 10 字节，之后每个分量 16 字节。最多 4 个分量，因此最大负载 74 字节。

```text
u16 transaction_id
u8  channel_id         // 0=DAC1，1=DAC2
u8  generation_mode    // 1=REALTIME，2=CACHE
u8  component_count    // 0..4
u8  channel_flags      // bit0 enable，bit1 saturate
i16 offset_code        // -8192..8191
u16 period_points      // REALTIME=0，CACHE=16..4096

repeat component_count times:
  u8  slot_id
  u8  waveform         // 0=OFF,1=SINE,2=SQUARE,3=TRIANGLE,4=SAW
  u16 component_flags  // bit0 enable
  u32 ftw
  u32 phase_word
  u16 amplitude_code   // 0..8191，8191=单分量峰值3500mV
  u16 duty_code        // 0..65535
```

必须检查：

- `channel_id`、模式、分量数、slot 唯一性及所有保留位；
- `offset_code` 和 `amplitude_code` 范围；
- REALTIME 模式的 `period_points` 必须为 0；
- CACHE 模式的 `period_points` 必须为 16～4096；
- SINE/方波/三角波/锯齿波编码必须与上表一致。

校验成功后只写对应通道的 shadow bank，并记录 `transaction_id` 和 `shadow_valid`，
不能改变 DAC 当前输出。随后返回 ACK，`applied_mask=0`。

## 5. FPGA_COMMIT（CMD 0x21）

```text
u16 transaction_id
u8  channel_mask       // bit0=DAC1，bit1=DAC2
u8  commit_flags       // bit0 清相位，bit1 双通道原子切换，bit2 等待建表，bit3 失败保持旧输出
```

提交控制器建议状态：

```text
IDLE
 -> VALIDATE_SHADOWS
 -> BUILD_CACHE_CH0 / BUILD_CACHE_CH1
 -> WAIT_ALL_READY
 -> ISSUE_CDC_COMMIT
 -> WAIT_SAMPLE_DOMAIN_ACK
 -> SEND_COMMIT_ACK
```

关键语义：

1. `channel_mask` 中每个通道必须存在相同 `transaction_id` 的有效 shadow。
2. 任一通道校验或 CACHE 建表失败，所有活动通道保持旧输出。
3. 双通道提交必须在同一个 100 MHz 采样边沿切换 active bank。
4. `commit_flags.bit0=1` 时，在该切换边沿同步清零目标通道相位累加器，再加载初相。
5. 只有采样域确认切换完成后才能返回成功 ACK。
6. 成功 ACK 的 `applied_mask` 必须等于请求 `channel_mask`。

不要在“已收到 COMMIT”或“开始 CACHE 建表”时提前返回成功。若实现选择返回
`BUSY`，Blue 会把它视为本次命令未完成；更推荐在 FPGA 内部等待完成后一次性返回 ACK。

## 6. ACK/NACK（CMD 0x7F）

响应帧：`DST=0x02`、`SRC=0x10`、设置 `RESPONSE`，并回显请求 `SEQ`。
失败时同时设置 `ERROR`。

```text
u8  request_cmd
u8  status
u16 detail
u16 transaction_id
u16 applied_mask
```

至少实现以下状态：

| 状态 | 值 | 使用场景 |
|---|---:|---|
| OK | 0x00 | 暂存成功或提交真正完成 |
| BAD_VERSION | 0x01 | VER 不是 2 |
| BAD_LENGTH | 0x02 | 长度与命令不符 |
| BAD_CRC | 0x03 | CRC 错误 |
| BAD_TARGET | 0x04 | 节点或通道不存在 |
| BAD_COMMAND | 0x05 | 未实现命令 |
| BAD_FIELD | 0x06 | 字段、保留位或 slot 非法 |
| OUT_OF_RANGE | 0x07 | 数字码、点数超范围 |
| BUSY | 0x08 | 上一提交仍在进行 |
| NOT_STAGED | 0x09 | 缺少同事务号 shadow |
| CLIPPING | 0x0B | 严格模式下预计削顶 |
| TIMEOUT | 0x0C | CACHE 或 CDC 超时 |
| HW_FAULT | 0x0D | 时钟、BRAM 或 DAC 故障 |

## 7. 波形引擎建议

- 当 AD9910 的 1 GHz 系统时钟与 FPGA 的 100 MHz 采样时钟来自同一外部时钟时，必须用 `ad9910_ftw = round(f * 2^32 / 1000000000)`，再令 `fpga_ftw = ad9910_ftw * 10`。不得在 FPGA 端独立按 100 MHz 重新取整，否则会产生持续相位漂移。
- FPGA 仍使用 32 位相位累加器自然回绕。
- `phase_word` 是初相对应的 32 位整周相位字。
- SINE 建议使用 ROM/CORDIC；方波由相位与 `duty_code` 比较；三角波和锯齿波由相位字映射。
- 每个分量先生成有符号幅值，再乘 `amplitude_code`。
- CH1、CH2 量程均为 7 Vpp；`amplitude_code=8191` 对应单分量峰值 3500 mV。
- 四路叠加和偏置应在至少 18～20 位有符号位宽完成。
- `channel_flags.bit1=1` 时饱和到 `-8192..8191`；禁止直接截低 14 位造成回绕。
- 每通道维护 `clipping` 标志和饱和计数。
- DAC1、DAC2 必须使用同一采样时钟，才能保证跨通道相位差稳定。

## 8. CACHE 模式建议

- 每通道使用 active/build 两个 BRAM bank。
- STAGE 只更新 shadow；COMMIT 后在 build bank 生成 `period_points` 个采样点。
- 建表期间 active bank 继续输出，不能出现半周期或部分新数据。
- 两个目标通道均 ready 后，同一采样边沿交换 bank。
- 建表或等待超时返回错误，active bank 不变。

## 9. 去重、复位与安全状态

- 缓存最近 1～4 个 `{SRC,SEQ,CMD}` 及完整响应。
- 收到完全相同的重发请求时，直接重发缓存响应，不得再次提交。
- 上电、PLL 失锁、看门狗超时或协议状态机复位时，两路 DAC 输出安全零码并进入 sleep。
- shadow 无效不能影响 active；错误计数和最后错误码可保留供状态查询。

## 10. 推荐联调顺序

1. 只验证 PING/ACK 帧、CRC、SEQ 回显和错误 CRC NACK。
2. DAC1 REALTIME：单正弦、不同频率、幅值、初相和偏置。
3. DAC1 四分量叠加，验证饱和而非回绕。
4. DAC1 CACHE：16、4096 点边界及建表期间旧输出连续。
5. DAC2 重复步骤 2～4，确认不是 DAC1 的镜像或空 ACK。
6. 同一事务分别 STAGE DAC1/DAC2，再用 `channel_mask=0x03` COMMIT，示波器验证同步边沿。
7. 重发同一 COMMIT，确认只返回同一 ACK，不发生第二次相位清零。
8. 测试 BAD_CRC、错误事务号、缺少通道、非法点数、UART 半帧超时。
9. 连续运行并读取状态，检查 CRC 错误计数、削顶计数和时钟锁定状态。

### CRC 联调样例

以下 STAGE 表示事务 `0x1234`、DAC1、REALTIME、0 个分量、开启饱和，
`SEQ=1`：

```text
D3 91 02 10 02 20 01 01 00 0A 00
34 12 00 01 00 02 00 00 00 00
5B 2B 91 D3
```

对应成功 ACK（`applied_mask=0`）：

```text
D3 91 02 02 10 7F 02 01 00 08 00
20 00 00 00 34 12 00 00
AA 70 91 D3
```

CRC 以小端发送，因此第一帧 CRC 数值为 `0x2B5B`，第二帧为 `0x70AA`。

## 11. Blue 端验收现象

- STAGE 成功后，Blue 屏幕 FPGA ACK 命令显示 `20`。
- COMMIT 完成后显示 `21/0`，队列索引到末尾，dirty mask 清零。
- FPGA 返回非零状态时，Blue 将同一错误码上报 Black。
- FPGA 不响应时，Blue 重试后上报 `TIMEOUT(0x0C)`。

# 双路信号源与 LCR 测量统一通信协议 V2

> 状态：设计基线，供 Black、Blue 与 FPGA 后续重构使用。  
> 适用题目：2024 年东南大学大学生电子设计竞赛第二轮 B 题《双路信号源及 LCR 测量》。  
> 本文替代新功能中的旧 `A5 5A CMD LEN ... XOR` 板间帧和 AD9744 单目标帧；迁移期允许旧协议与 V2 并存。

## 1. 设计目标

V2 必须完整覆盖以下功能：

- CH1 与 CH2 两个独立电压输出通道；
- 两通道独立启停、配置、查询和同步切换；
- 正弦波、方波、三角波、锯齿波；
- 频率、幅值、相位、相位差、占空比和直流偏置；
- 每通道最多 4 个波形分量叠加；
- 实时 DDS 与周期缓存两种 FPGA 生成模式；
- CH1、CH2 均为 0～7 Vpp（峰值 0～3500 mV）的物理量表达；
- 0.01 Hz 频率步进、5°或更细相位步进、0.1%～99.9% 占空比；
- L、C、R 单点测量、扫频、结果和错误回传；
- 参数合法性检查、明确 ACK/NACK、超时重试和重复帧幂等处理；
- FPGA 端解析简单、无浮点、配置原子提交、双通道相位同步。

题目指标与协议字段的对应关系：

| 题目功能 | V2 表达方式 |
|---|---|
| 双路电压输出 | `channel_id=0/1`，提交时使用 `channel_mask` |
| 频率 | `frequency_cHz`，单位 0.01 Hz |
| 幅值 | 界面使用峰值 mV；`amplitude_uunit_pp` 使用 µVpp |
| 相位及相位差 | 每个分量独立 `phase_mdeg`，单位 0.001° |
| 直流偏置 | 每个物理通道独立 `dc_offset_uunit` |
| 波形 | `waveform` 枚举 SINE/SQUARE/TRIANGLE/SAW |
| 方波占空比 | `duty_ppm`，1000～999000 对应 0.1%～99.9% |
| 任意叠加 | 每通道 0～4 个 `SOURCE_COMPONENT` |
| 稳定显示 | `SOURCE_STATUS`、`MEASURE_STATUS`、`UI_STATE` |
| L/C/R 测量 | `MEASURE_START/ACCEPTED/RESULT/ERROR` |

## 2. 系统分层与节点

```text
键盘与业务状态机                         显示、标定、ADC测量
Black MCU  <--------- UART ---------->  Blue MCU  <--------- UART ---------->  FPGA
 node 0x01          V2物理量协议          node 0x02       V2数字码协议          node 0x10
                                                            |
                                                            +--> DAC1：CH1 电压输出
                                                            +--> DAC2：CH2 电压输出
```

节点编号：

| 节点 | ID | 职责 |
|---|---:|---|
| Black MCU | `0x01` | 键盘、业务状态、信号源和测量调度 |
| Blue MCU | `0x02` | 屏幕、物理量标定、ADC、FPGA 网关 |
| FPGA | `0x10` | 双 DAC 数字波形生成与同步提交 |
| 广播 | `0xFF` | 仅用于无副作用的发现和紧急关闭 |

物理通道固定映射，`CH` 不再表示“把同一份配置临时路由到某个未知目标”：

| `channel_id` | `channel_mask` | 物理意义 | FPGA 输出 |
|---:|---:|---|---|
| `0` | bit0 | CH1 电压输出 | DAC1 |
| `1` | bit1 | CH2 电压输出 | DAC2 |

两个通道共享 100 MHz DAC 采样时钟。只有共享采样时钟并使用同一次 `COMMIT`，跨通道相位差才有严格定义。

当 AD9910 的 1 GHz 系统时钟与 FPGA 的 100 MHz 采样时钟由同一外部时钟派生时，Blue 必须先按 `ad9910_ftw = round(f * 2^32 / 1 GHz)` 计算 AD9910 基准频率字，再发送 `fpga_ftw = ad9910_ftw * 10`。两边独立取整会造成持续相位漂移。

Black 必须分别保存 CH1、CH2 的通道配置。每套配置独立包含 4 个波形分量、分量数、
当前分量、直流偏置和 REALTIME/CACHE 模式。切换 CH 只改变当前编辑与提交目标，
不得把原通道配置复制或穿透到另一个 DAC，也不得清除未选通道的活动输出。

## 3. UART 与字节序

- Black ↔ Blue：115200 baud、8N1、3.3 V TTL、共地；后续可提升到 460800 baud。
- Blue ↔ FPGA：默认 115200 baud、8N1、3.3 V TTL、共地；推荐硬件验证后提升到 921600 baud。
- 所有多字节整数均为小端序。
- 有符号整数使用二进制补码。
- 接收器必须设置帧间超时：115200 baud 下建议 20 ms，超时立即回到找帧头状态。

## 4. V2 通用帧

```text
D3 91 VER DST SRC CMD FLAGS SEQ_L SEQ_H LEN_L LEN_H PAYLOAD CRC_L CRC_H 91 D3
```

CRC 覆盖 `VER` 至 `PAYLOAD` 最后一个字节，不包含帧头、CRC 自身和帧尾。

| 字段 | 长度 | 说明 |
|---|---:|---|
| SOF | 2 | 固定 `D3 91`，与现有 `A5 5A`、`F9 26` 帧区分 |
| VER | 1 | 当前固定 `0x02` |
| DST | 1 | 目标节点 ID |
| SRC | 1 | 源节点 ID |
| CMD | 1 | 命令字 |
| FLAGS | 1 | 通用控制标志 |
| SEQ | 2 | 请求序号，发送端逐帧递增 |
| LEN | 2 | PAYLOAD 长度，0～192；FPGA 命令限制为 0～80 |
| PAYLOAD | LEN | 命令负载 |
| CRC | 2 | CRC-16/CCITT-FALSE，小端发送 |
| EOF | 2 | 固定 `91 D3` |

CRC 参数：

```text
poly   = 0x1021
init   = 0xFFFF
refin  = false
refout = false
xorout = 0x0000
```

`FLAGS`：

| 位 | 名称 | 说明 |
|---:|---|---|
| bit0 | `ACK_REQ` | 请求处理后必须应答 |
| bit1 | `RESPONSE` | 本帧为响应 |
| bit2 | `EVENT` | 主动事件，无需请求 |
| bit3 | `ERROR` | 响应表示失败 |
| bit4 | `MORE` | 后续还有分片 |
| bit5 | `URGENT` | 紧急命令，仅允许安全关闭类命令使用 |
| bit6..7 | 保留 | 必须发送 0，接收端非 0 时拒绝 |

解析规则：

1. 只在空闲状态搜索 `D3 91`。
2. 找到帧头后严格按 `LEN` 收取，不在 PAYLOAD 内重新搜索帧头。
3. 依次校验版本、目标、长度、CRC 和帧尾，再交给命令分发器。
4. 解析失败不得改变活动输出。
5. 同一 `SRC + SEQ + CMD` 的重复请求必须返回之前的响应，不得重复提交输出。

## 5. 通用响应

### 5.1 ACK/NACK - `CMD=0x7F`

响应帧设置 `RESPONSE`；失败时同时设置 `ERROR`。

| 偏移 | 类型 | 字段 | 说明 |
|---:|---|---|---|
| 0 | u8 | `request_cmd` | 被响应的命令 |
| 1 | u8 | `status` | 状态码 |
| 2 | u16 | `detail` | 字段序号、边界值或硬件细节 |
| 4 | u16 | `transaction_id` | 配置事务号，无事务时为 0 |
| 6 | u16 | `applied_mask` | 已应用对象位图 |

状态码：

| 值 | 名称 | 含义 |
|---:|---|---|
| `0x00` | OK | 已接收或已完成 |
| `0x01` | BAD_VERSION | 不支持的协议版本 |
| `0x02` | BAD_LENGTH | 负载长度不正确 |
| `0x03` | BAD_CRC | CRC 错误 |
| `0x04` | BAD_TARGET | 节点或通道不存在 |
| `0x05` | BAD_COMMAND | 不支持的命令 |
| `0x06` | BAD_FIELD | 字段编码非法 |
| `0x07` | OUT_OF_RANGE | 参数超出能力范围 |
| `0x08` | BUSY | 当前事务未完成 |
| `0x09` | NOT_STAGED | COMMIT 对应配置尚未暂存 |
| `0x0A` | UNSUPPORTED | 硬件未实现该能力 |
| `0x0B` | CLIPPING | 数字叠加或模拟量程会削顶 |
| `0x0C` | TIMEOUT | 下游设备或测量超时 |
| `0x0D` | HW_FAULT | DAC、ADC、DMA 或时钟故障 |

## 6. 能力发现与兼容

### 6.1 GET_CAPABILITIES - `CMD=0x01`

PAYLOAD 为空。响应负载：

| 偏移 | 类型 | 字段 |
|---:|---|---|
| 0 | u32 | `feature_bits` |
| 4 | u32 | `sample_clock_hz` |
| 8 | u8 | `physical_channel_count` |
| 9 | u8 | `max_components_per_channel` |
| 10 | u16 | `max_payload` |
| 12 | u32 | `firmware_version` |
| 16 | u32 | `hardware_version` |

`feature_bits`：bit0 双通道、bit1 正弦、bit2 方波、bit3 三角波、bit4 锯齿波、bit5 叠加、bit6 实时 DDS、bit7 周期缓存、bit8 原子双通道提交、bit9 LCR 单点、bit10 LCR 扫频。

发送配置前必须先读取能力。若 FPGA 尚未实现 DAC2 协议控制，它必须清除 bit0，并对电流通道配置返回 `UNSUPPORTED`，不能静默超时。

### 6.2 PING - `CMD=0x02`

请求携带 u32 cookie，响应原样回显，用于线路和往返时延测试。

### 6.3 EMERGENCY_OFF - `CMD=0x03`

允许广播，必须设置 `URGENT|ACK_REQ`。PAYLOAD 为 u8 `channel_mask`。FPGA 在下一个采样边沿将对应通道置零并进入 sleep；该命令不等待普通配置事务。

## 7. Black ↔ Blue 物理量信号源协议

Black 负责用户意图；Blue 负责标定、量程选择以及把物理量换算成 FPGA 数字码。不得让 FPGA 解析浮点数或执行 µV/µA 到 DAC 码的标定换算。

### 7.1 SOURCE_STAGE - `CMD=0x10`

每帧只暂存一个物理通道，暂存不改变当前输出。

固定头部 18 字节：

| 偏移 | 类型 | 字段 | 说明 |
|---:|---|---|---|
| 0 | u16 | `transaction_id` | 本轮配置事务号 |
| 2 | u8 | `channel_id` | 0=CH1，1=CH2 |
| 3 | u8 | `output_enable` | 0=关闭，1=开启 |
| 4 | u8 | `generation_mode` | 0=AUTO，1=REALTIME，2=CACHE |
| 5 | u8 | `component_count` | 0～4 |
| 6 | u8 | `range_mode` | 0=AUTO，其余为标定量程编号 |
| 7 | u8 | `stage_flags` | bit0 允许限幅，bit1 要求严格相位同步 |
| 8 | i32 | `dc_offset_uunit` | CH1、CH2 均使用 µV |
| 12 | u16 | `cache_points` | 0=Blue 自动计算，或 16～4096 |
| 14 | u16 | `settle_us` | 提交后建议稳定等待时间 |
| 16 | u16 | `calibration_id` | 0=使用当前有效标定 |

随后紧跟 `component_count` 个 20 字节分量：

| 分量偏移 | 类型 | 字段 | 说明 |
|---:|---|---|---|
| 0 | u8 | `slot_id` | 0～3，不得重复 |
| 1 | u8 | `waveform` | 波形类型 |
| 2 | u16 | `component_flags` | bit0 enable，其余保留 |
| 4 | u32 | `frequency_cHz` | 0.01 Hz；最大约 42.94967295 MHz |
| 8 | i32 | `phase_mdeg` | -180000～+180000，Blue 归一化到一周 |
| 12 | u32 | `amplitude_uunit_pp` | CH1、CH2 均使用 µVpp，范围 0～7000000 |
| 16 | u32 | `duty_ppm` | 1000～999000，仅方波使用 |

波形枚举：

| 值 | 波形 |
|---:|---|
| 0 | OFF |
| 1 | SINE |
| 2 | SQUARE |
| 3 | TRIANGLE |
| 4 | SAW |

幅值界面与数字码换算：

- 屏幕 `AMP(mV)` 输入的是波形峰值，范围 0～3500 mV；对应单分量最大 7 Vpp。
- Black 按 `amplitude_code=floor(AMP_mV*8191/3500)` 生成发给 FPGA 的幅值码。
- Black→Blue 仍保持 `amplitude_uunit_pp` 的 µVpp 语义，按
  `round(amplitude_code*7000000/8191)` 传输；Blue 必须直接使用 µVpp 反算，
  不得先截断到整数 mV。
- Blue→FPGA 的 `amplitude_code` 保持 0～8191，8191 对应单分量峰值 3500 mV。

参数规则：

- CH1、CH2 所有交流分量叠加后的理论峰值与偏置必须位于±3500 mV 量程内；
  超过时 FPGA 必须饱和并返回 `clipping` 状态。
- 正弦波允许 0.01 Hz～20 MHz 以上，实际范围由能力响应和模拟带宽共同限制。
- 方波、三角波、锯齿波允许 0.01 Hz～4 MHz 以上，实际范围由能力响应返回。
- `AUTO` 模式下，无公共整数周期或高频配置选择 REALTIME；存在可靠公共周期且点数为 16～4096 时可选择 CACHE。
- 对双通道相位差有要求时，两通道必须使用相同 `transaction_id`，并由一次 `SOURCE_COMMIT` 同步应用。

### 7.2 SOURCE_COMMIT - `CMD=0x11`

| 偏移 | 类型 | 字段 |
|---:|---|---|
| 0 | u16 | `transaction_id` |
| 2 | u8 | `channel_mask`，bit0 电压、bit1 电流 |
| 3 | u8 | `commit_flags` |
| 4 | u32 | `apply_delay_us` |

`commit_flags`：bit0 同步清零相位累加器；bit1 两通道原子切换；bit2 等待缓存构建完成；bit3 提交失败时保持所有旧输出。

Blue 必须完成以下顺序：

```text
校验两通道物理量
  -> 选择模拟量程和标定参数
  -> 换算 FPGA FTW/PHASE/AMP/OFFSET
  -> 向 FPGA 暂存所需通道
  -> 确认 FPGA 两通道均 ACK
  -> 向 FPGA 发送一次 FPGA_COMMIT
  -> FPGA 同一采样边沿应用
  -> Blue 返回 SOURCE_COMMIT ACK
```

任一步失败都保持旧输出，并明确返回错误，不能只更新其中一个通道。

### 7.3 SOURCE_GET_STATUS - `CMD=0x12`

请求负载为 u8 `channel_mask`。响应对每个通道返回：活动事务号、enable、生成模式、分量数、量程、裁剪状态、实际输出频率/幅值/偏置、FPGA commit 号和最后错误码。

显示端应显示“实际应用值”，而不是仅显示用户请求值。频率量化误差、幅值量化误差和自动量程结果均由该状态返回。

### 7.4 SOURCE_STATUS - `CMD=0x13`

负载与 `SOURCE_GET_STATUS` 响应相同，但设置 `EVENT` 标志。Blue 在提交完成、输出启停、量程变化、裁剪或硬件故障时主动发送；Black 不应依赖固定周期重复发送完整 UI 快照。

## 8. Blue ↔ FPGA 数字波形协议

### 8.1 FPGA_CHANNEL_STAGE - `CMD=0x20`

每帧暂存一个 DAC 通道。固定头部 10 字节：

| 偏移 | 类型 | 字段 |
|---:|---|---|
| 0 | u16 | `transaction_id` |
| 2 | u8 | `channel_id`，0=DAC1，1=DAC2 |
| 3 | u8 | `generation_mode`，1=REALTIME，2=CACHE |
| 4 | u8 | `component_count`，0～4 |
| 5 | u8 | `channel_flags`，bit0 enable，bit1 saturate |
| 6 | i16 | `offset_code`，14 位有效补码 -8192～8191 |
| 8 | u16 | `period_points`，REALTIME 为 0，CACHE 为 16～4096 |

随后是 `component_count` 个 16 字节数字分量：

| 分量偏移 | 类型 | 字段 |
|---:|---|---|
| 0 | u8 | `slot_id`，0～3 |
| 1 | u8 | `waveform`，与第 7.1 节一致 |
| 2 | u16 | `component_flags`，bit0 enable |
| 4 | u32 | `ftw`，`round(f * 2^32 / Fs)` |
| 8 | u32 | `phase_word`，`round(phase * 2^32 / 360°)` |
| 12 | u16 | `amplitude_code`，0～8191；8191 对应单分量峰值 3500 mV |
| 14 | u16 | `duty_code`，0～65535 |

与现有 FPGA 实现相比必须完成的改动：

- `channel_id=1` 必须真正连接 DAC2，不能继续仅返回超时；
- 波形编码扩展为 3 位，新增 SINE；
- DAC1、DAC2 各自拥有影子寄存器、实时 DDS 状态和缓存状态；
- 数字叠加在更宽位宽中完成，默认饱和到 -8192～8191，并置 `clipping` 状态；不得静默 14 位回绕；
- 校验失败只修改错误状态，不修改活动配置。

### 8.2 FPGA_COMMIT - `CMD=0x21`

| 偏移 | 类型 | 字段 |
|---:|---|---|
| 0 | u16 | `transaction_id` |
| 2 | u8 | `channel_mask` |
| 3 | u8 | `commit_flags` |

FPGA 校验指定通道均已使用同一 `transaction_id` 暂存。校验通过后：

1. CACHE 通道先在备用 BRAM 构建完整周期，活动输出保持不变。
2. 所有目标通道准备完成后，在同一个 100 MHz 采样时钟边沿切换。
3. `commit_flags.bit0=1` 时，两通道所有相位累加器同步清零，再加入各自 `phase_word`。
4. 返回 ACK；响应中的 `applied_mask` 必须与请求一致。

### 8.3 FPGA_GET_STATUS - `CMD=0x22`

返回：时钟锁定、活动通道、各通道模式、活动事务号、缓存构建状态、缓存点数、裁剪计数、UART CRC 错误计数、最近错误码。

## 9. FPGA 推荐结构

```text
UART RX
  -> V2 frame parser（长度 + 流式 CRC16）
  -> command dispatcher
  -> channel0 shadow config ----+
  -> channel1 shadow config ----+--> atomic commit controller
                                      |
                         shared 100 MHz sample clock
                           |                        |
                     DAC1 waveform engine     DAC2 waveform engine
                     realtime/cache           realtime/cache
                           |                        |
                     saturating mixer         saturating mixer
                           |                        |
                          DAC1                     DAC2

status/error -> response FIFO -> UART TX
```

实现约束：

- UART 域到采样域采用影子寄存器 + toggle/握手 CDC，禁止多位总线裸跨时钟域。
- 双通道缓存模式若要求后台无缝更新，每通道需要 active/build 两个 BRAM bank。
- ACK 只表示协议动作真实完成；暂存 ACK 与提交 ACK 必须区分。
- FPGA 保存最近 1～4 个 `SRC+SEQ+CMD` 响应，用于处理 MCU 超时重发。
- 看门狗超时、时钟失锁或复位时，两路进入安全零码并拉高 sleep。

## 10. UI 与键盘协议

### 10.1 KEY_EVENT - `CMD=0x30`

| 偏移 | 类型 | 字段 |
|---:|---|---|
| 0 | u8 | `key_code`，ASCII |
| 1 | u8 | `key_action`，0=按下，1=长按，2=释放 |
| 2 | u16 | `duration_ms` |

### 10.2 UI_STATE - `CMD=0x31`

用于保持现有“Black 管业务、Blue 管屏幕”的结构：

| 偏移 | 类型 | 字段 |
|---:|---|---|
| 0 | u8 | `page_id` |
| 1 | u8 | `selected_channel`，0=电压，1=电流 |
| 2 | u8 | `selected_component`，0～3 |
| 3 | u8 | `focus_field` |
| 4 | u8 | `editing` |
| 5 | u8 | `edit_length` |
| 6 | u8[] | UTF-8/ASCII 编辑缓冲，最大 16 字节 |

信号源的实际参数不重复塞入 UI_STATE；屏幕通过 `SOURCE_GET_STATUS` 或 `SOURCE_STATUS` 事件更新。这样界面字段增减不会破坏信号源协议。

## 11. LCR 测量协议

### 11.0 本机 AUTO 测量控制分工

本机测量时由 Blue 作为 LCR 主控：Blue 控制扫频状态机、同步 ADC 采集、复数阻抗计算、自动判型、精测平均和 LCD 显示；Black 只执行 AD9910 激励设置并转发键盘事件。两路 ADC 原始数组不得通过板间 UART 传输。

Blue 本地采样固定映射为 `P1-9/PC0/ADC1_IN10=Vin`、`P1-8/PC1/ADC2_IN11=Vr`，由 TIM6 TRGO 同时触发并通过 DMA 保存4096组打包数据。PC0 不再承担对数检波输入；旧 `F9 26` ADC帧和 `0x40/0x41` 对数检波请求/响应实现均退出当前 Black↔Blue 运行解析器。

`CMD=0x40～0x47` 保留给上位机测量控制、状态和结果扩展。本机 LCD 测量不依赖 `MEASURE_RESULT 0x42`，而使用下面的板间激励握手。

#### LCR_EXCITATION_SET - `CMD=0x48`，Blue→Black

| 偏移 | 类型 | 字段 | 说明 |
|---:|---|---|---|
| 0 | u16 | `request_id` | 激励请求号 |
| 2 | u32 | `frequency_hz` | 目标频率，当前允许 1 kHz～1 MHz |
| 6 | u16 | `amplitude_mVpp` | AD9910 模块输出峰峰值 |
| 8 | i32 | `phase_mdeg` | 相位，0.001° |
| 12 | u8 | `enable` | 0=关闭，1=输出正弦 |
| 13 | u8 | `reserved` | 固定 0 |
| 14 | u16 | `settle_us` | Blue 在收到 READY 后等待的稳定时间 |

#### LCR_EXCITATION_READY - `CMD=0x49`，Black→Blue

| 偏移 | 类型 | 字段 | 说明 |
|---:|---|---|---|
| 0 | u16 | `request_id` | 回显请求号 |
| 2 | u8 | `result` | 0=成功，1=长度错误，2=参数越界 |
| 3 | u8 | `reserved` | 固定 0 |
| 4 | u32 | `actual_ftw` | 实际写入 AD9910 的 FTW |
| 8 | u32 | `actual_frequency_cHz` | 由 FTW 反算的实际频率，0.01 Hz |

Black 必须在主循环中执行 AD9910 配置，UART 中断回调只复制请求，不得在中断中操作 DDS 或阻塞发送。Blue 必须使用 `actual_frequency_cHz` 进行相量拟合和 L/C 换算。

### 11.1 MEASURE_START - `CMD=0x40`

| 偏移 | 类型 | 字段 | 说明 |
|---:|---|---|---|
| 0 | u16 | `measurement_id` | 测量事务号 |
| 2 | u8 | `measure_type` | 0=AUTO，1=R，2=L，3=C，4=RAW_Z |
| 3 | u8 | `flags` | bit0 自动量程，bit1 扫频点 |
| 4 | u32 | `excitation_frequency_cHz` | 激励频率 |
| 8 | u32 | `excitation_uVpp` | 电压激励幅值 |
| 12 | i32 | `dc_bias_uV` | 激励偏置 |
| 16 | u16 | `settle_us` | DDS 稳定时间 |
| 18 | u16 | `average_count` | 平均次数 |
| 20 | u32 | `sample_rate_hz` | 当前推荐 2500000 |
| 24 | u16 | `sample_count` | 当前推荐 4096 |
| 26 | u16 | `calibration_id` | 测量标定编号 |

测量流程必须是：暂存激励配置 → 提交并等待稳定 → 回复 ACCEPTED → ADC 定时器触发 DMA → 计算电压和电流复数响应 → 返回结果。测量过程中普通输出配置可暂存，但不得覆盖当前激励。

### 11.2 MEASURE_ACCEPTED - `CMD=0x41`

回显 `measurement_id`、实际激励频率、实际采样率、量程和预计完成时间。

### 11.3 MEASURE_RESULT - `CMD=0x42`

| 偏移 | 类型 | 字段 | 单位 |
|---:|---|---|---|
| 0 | u16 | `measurement_id` | - |
| 2 | u16 | `result_flags` | 有效、过量程、低 SNR、已标定等 |
| 4 | u32 | `frequency_cHz` | 0.01 Hz |
| 8 | u32 | `voltage_rms_uV` | µV |
| 12 | u32 | `current_rms_uA` | µA |
| 16 | i32 | `phase_mdeg` | V 相对 I 的相位 |
| 20 | u32 | `impedance_mohm` | mΩ |
| 24 | i32 | `resistance_mohm` | mΩ |
| 28 | i32 | `reactance_mohm` | mΩ |
| 32 | u64 | `inductance_nH` | nH，无效时 0 |
| 40 | u64 | `capacitance_fF` | fF，无效时 0 |
| 48 | u32 | `quality_factor_x1000` | Q×1000 |
| 52 | u32 | `error_ppm` | 估计误差 |

计算关系：

```text
Z = V_complex / I_complex
R = real(Z)
X = imag(Z)
L = X / (2*pi*f)       when X > 0
C = -1 / (2*pi*f*X)    when X < 0
```

### 11.4 MEASURE_ERROR - `CMD=0x43`

回显 `measurement_id`、错误码、失败阶段、ADC min/max、超时信息。错误不得被伪装成数值 0 的有效测量结果。

### 11.5 SWEEP_START / POINT / COMPLETE - `CMD=0x44/0x45/0x46`

扫频请求包含起始频率、终止频率、点数、线性/对数模式、每点稳定时间和平均次数。每个点使用 `measurement_id + point_index` 绑定，允许边测边回传，最终 COMPLETE 给出成功点数、失败点数和总耗时。

### 11.6 MEASURE_STATUS - `CMD=0x47`

设置 `EVENT` 标志，回传 `measurement_id`、IDLE/SETTLING/CAPTURING/PROCESSING/DONE/ERROR 状态、当前扫频点、总点数和预计剩余时间，供屏幕稳定显示测量进度。

## 12. 模式选择规则

Black 可让用户选择 AUTO、DDS、CACHE；Blue 最终决策并在状态中回报实际模式。

```text
用户选 DDS   -> 强制 REALTIME
用户选 CACHE -> 校验公共周期和 POINTS；不合法则 NACK，不静默降级
用户选 AUTO  -> 可形成 16..4096 点公共周期时选 CACHE，否则选 REALTIME
```

公共周期校验至少满足：

```text
points = round(100000000 / base_frequency_hz)
16 <= points <= 4096
每个启用分量的频率 / base_frequency 接近整数
每个分量 FTW * points 接近 2^32 的整数倍
```

## 13. 超时与重试

| 链路/操作 | 建议超时 | 重试 |
|---|---:|---:|
| PING、能力查询 | 50 ms | 2 |
| 普通暂存 | 100 ms | 2 |
| 实时 DDS COMMIT | 100 ms | 2 |
| 4096 点缓存构建并提交 | 5 ms FPGA 内部，MCU 总超时 100 ms | 2 |
| ADC 单点测量 | `settle + capture + compute + 50 ms` | 由业务决定 |

重试必须复用原 `SEQ` 和 `transaction_id`。新的业务请求才分配新 `SEQ`。接收端对重复请求返回缓存响应，避免重复切换相位或重复启动测量。

## 14. 编码示例

以下 CRC 均按本文参数计算，可直接作为解析器单元测试向量。

### 14.1 Black 向 Blue 发送 PING

`SEQ=1`，cookie=`0x12345678`：

```text
D3 91 02 02 01 02 01 01 00 04 00 78 56 34 12 48 D8 91 D3
```

### 14.2 Blue 暂存 DAC1 的 1 MHz 正弦

事务号 `0x1234`，实时 DDS，单分量，FTW=`0x028F5C26`，直接对应同源 1 MHz，相位 0，幅度码 8191：

```text
D3 91 02 10 02 20 01 02 00 1A 00
34 12 00 01 01 01 00 00 00 00
00 01 01 00 26 5C 8F 02 00 00 00 00 FF 1F 00 80
46 FD 91 D3
```

### 14.3 原子提交 DAC1 与 DAC2

事务号 `0x1234`，`channel_mask=0x03`，同步清相位、原子切换、失败保持旧输出：

```text
D3 91 02 10 02 21 01 03 00 04 00 34 12 03 0B B5 69 91 D3
```

## 15. 安全与边界检查

- 电压通道请求值不得超过当前硬件标定允许的 Vpp、偏置和负载能力。
- 电流通道请求值不得超过当前硬件标定允许的 Ipp、偏置和负载能力。
- MCU 在发送 FPGA 配置前计算最坏情况数字峰值；若超限且未允许限幅，返回 `CLIPPING`。
- FPGA 仍需执行最终饱和保护，不能依赖 MCU 永远正确。
- 方波占空比必须在 0.1%～99.9%；非方波忽略该字段但仍要求合法编码。
- 输出 enable 前必须确保 PLL lock、DAC 时钟正常且对应模拟量程已经切换完成。
- 通信失联看门狗是否自动关闭输出由系统配置决定；电流通道建议默认启用失联关闭。

## 16. 迁移方案

### 阶段 1：并行解码

- Black↔Blue 保留现有 `A5 5A` 和 `F9 26` 解码器，新增 `D3 91` V2 解码器。
- Blue↔FPGA 保留当前 DAC1 帧用于回归，新增 V2 parser；能力响应中只声明已实现功能。

### 阶段 2：FPGA 双通道

- 将 DAC2 从按键测试模块迁移到第二套可配置波形引擎。
- 实现两个 `CHANNEL_STAGE` 影子区和一次 `FPGA_COMMIT`。
- 加入 SINE、CRC16、SEQ、NACK、状态查询和饱和混频。

### 阶段 3：MCU 物理量网关

- Blue 实现 µV/µA 到量程、DAC code、FTW、phase word 的标定换算。
- Black UI 从单一 `CH` 目标改成明确的 VOLTAGE/CURRENT 通道页。
- MODE 扩展为 AUTO/DDS/CACHE，CACHE 非法时在屏幕显示原因。

### 阶段 4：LCR 闭环

- 保留现有 2.5 MHz、4096 点 TIM+ADC+DMA 测量链路。
- 增加同步电压/电流采样或经标定的双路检测链路。
- 输出完整复阻抗及 R/L/C 结果，并完成 1% 精度校准和验收。

## 17. 验收用例

1. 分别配置电压和电流通道，确认 TARGET 不会串路。
2. 一次事务暂存两个通道并 COMMIT，示波器确认相位差稳定且重复提交不产生额外跳变。
3. 测试 SINE/SQUARE/TRIANGLE/SAW、0.01 Hz 边界、最高频率和非法频率 NACK。
4. 测试幅值、偏置、占空比上下边界以及叠加削顶检测。
5. DDS/CACHE/AUTO 三种选择与实际模式状态一致。
6. CRC 错、截断帧、错误长度、错误通道均不改变活动输出。
7. ACK 丢失后重发同一 SEQ，输出只提交一次。
8. 双通道 CACHE 构建期间旧输出连续，完成后同一采样边沿切换。
9. L/C/R 标准件单点和扫频结果满足题目 1% 目标。
10. 通信失联、PLL 失锁、复位和紧急关闭均进入定义好的安全状态。

## 18. 当前工程与 V2 的关键差距

| 当前工程 | V2 要求 |
|---|---|
| FPGA 新帧只接收 `TARGET=0` | DAC1/DAC2 均可暂存和提交 |
| DAC2 由 KEY1 测试逻辑控制 | DAC2 由 V2 `channel_id=1` 控制 |
| XOR、无版本、无序号 | CRC16、VER、SEQ、明确 ACK/NACK |
| 配置接收即提交单通道 | 双通道影子配置 + 原子 COMMIT |
| 多波形编码无正弦 | 统一 3 位波形编码，包含 SINE |
| 数字溢出发生补码回绕 | 宽位累加、饱和和 clipping 状态 |
| Black→Blue 发送 UI 内部快照 | 物理量 SOURCE 协议与 UI_STATE 分离 |
| LCR 只返回参考频率幅值 | 返回 V/I 复响应、Z、R、X、L、C 和误差状态 |

该差距表也是后续实现任务的拆分依据。

# On-board ADC Sine Measurement Design

本文定义由测量板使用板上 ADC 采样，并在 MCU 端通过正弦最小二乘拟合计算幅度的方案。Black 板负责驱动 DDS，测量板负责在 DDS 稳定后采样、计算参考频率对应的幅度，并把结果包装成：

```text
reference_frequency_hz -> main_frequency_hz, voltage_uv_rms
```

上层业务默认只消费“主频对应的电压值”，不直接依赖原始 ADC 波形或完整频谱。

## 1. 目标与边界

- 单帧采样点数固定 `N = 4096`。
- 采样率固定为硬件可保证的 `Fs = 2,500,000 Hz`。
- 采样由定时器硬件触发，DMA 搬运整帧数据，软件只在 DMA 完成中断后处理。
- Black 板必须先完成 DDS 设置并等待输出稳定，再向 FFT 测量板发起采样请求。
- FFT 测量板不主动改变 DDS，也不根据 UI 频率猜测当前测量点。
- 本轮重构只新增 FFT 测量和通信协议模块，不改动显示、DDS、AGC、旧串口协议等已有模块。

4096 点采样耗时为 `1.6384 ms`。TIM6 的 PSC/ARR 配置完成后，软件必须从寄存器反算真实采样率；反算值与请求值不一致时拒绝启动采样。

## 2. 采样架构

不采用“每个采样点进一次定时器中断 + 软件读取 ADC”的方式。2.5 MHz下逐点中断会让CPU负载和采样抖动不可控。推荐链路：

```text
TIMx update/TRGO at Fs
        |
        v
ADC external trigger regular conversion
        |
        v
DMA writes adc_raw[4096]
        |
        v
DMA transfer-complete interrupt sets frame_ready
        |
        v
main/task runs preprocess + FFT + result packaging
```

定时器只提供严格采样节拍；中断只在整帧 DMA 完成、错误或超时时触发。

## 3. Black 与 FFT 板握手流程

DDS 稳定由 Black 板负责保证。FFT 测量板只有收到“已稳定，可以采样”的请求后才启动 ADC。

```text
Black:
  SET_DDS(reference_frequency_hz)
  WAIT_DDS_SETTLE(settle_us)
  SEND MEASURE_REQUEST
  WAIT REQUEST_ACCEPTED or BUSY/ERROR
  WAIT MEASURE_RESULT
  verify sweep_id, point_id, reference_frequency_hz, seq
  store voltage result
  go next frequency

FFT board:
  IDLE
    -> receive MEASURE_REQUEST
    -> validate frequency/rate/length
    -> reply REQUEST_ACCEPTED
    -> ARM_CAPTURE
    -> CAPTURING
    -> FRAME_READY
    -> FFT_PROCESSING
    -> send MEASURE_RESULT
    -> IDLE
```

严格规则：

- Black 在收到当前点结果前，不发送下一点请求。
- FFT 板处于 `CAPTURING` 或 `FFT_PROCESSING` 时，收到新请求必须回 `BUSY`，不能覆盖当前测量。
- Black 发请求前必须已经等待 DDS 稳定；FFT 板不再额外等待 DDS，只可根据请求中的 `pre_capture_delay_us` 做短延时保护。
- 所有响应必须回显 `sweep_id`、`point_id`、`reference_frequency_hz` 和 `seq`。

## 4. UART 协议帧

沿用 FFT 专用帧格式，和旧 DAC 协议区分：

```text
F9 26 VER TARGET CMD FLAGS SEQ LEN_L LEN_H PAYLOAD CRC_L CRC_H 62 9F
```

固定项：

```text
VER    = 0x01
TARGET = 0xA3    // MCU ADC FFT measurement
CRC    = CRC-16/CCITT-FALSE, little-endian on wire
UART   = 115200 8N1, 3.3V, common ground
```

当前工程复用原有板间串口，不新增或更换引脚：

```text
Black USART3_TX PB10  -> Blue USART1_RX PA10
Black USART3_RX PB11  <- Blue USART1_TX PA9
Black GND             --- Blue GND
```

现有 LCR 测试模式保持 `1 MHz` DDS 激励不变，采样请求固定使用 `Fs = 2.5 MHz`，避免 `Fs/2` 奈奎斯特边界。

命令分配：

| CMD | 方向 | 名称 | 说明 |
|---:|---|---|---|
| `0x21` | Black -> FFT | `MEASURE_REQUEST` | DDS 已稳定，请求采一帧并计算 |
| `0x61` | FFT -> Black | `REQUEST_ACCEPTED` | 请求合法，已经准备开始采样 |
| `0x62` | FFT -> Black | `BUSY` | FFT 板忙，本次请求未执行 |
| `0xA1` | FFT -> Black | `MEASURE_RESULT` | 主频电压结果 |
| `0xE1` | FFT -> Black | `MEASURE_ERROR` | 请求非法、采样失败或计算失败 |

### 4.1 MEASURE_REQUEST 0x21

Black 必须在 DDS 设置完成并等待稳定后发送。PAYLOAD 固定 28 字节：

| 偏移 | 类型 | 字段 |
|---:|---|---|
| 0 | uint16 | `sweep_id` |
| 2 | uint16 | `point_id` |
| 4 | uint32 | `reference_frequency_hz`，DDS 实际输出频率 |
| 8 | uint32 | `dds_ftw`，没有则填 0 |
| 12 | uint32 | `sample_rate_hz`，当前固定 2500000 |
| 16 | uint16 | `fft_length`，固定 4096 |
| 18 | uint16 | `target_bin`，Black 可填 0xFFFF 表示由 FFT 板计算 |
| 20 | uint32 | `settle_us`，Black 已经等待过的 DDS 稳定时间 |
| 24 | uint16 | `pre_capture_delay_us`，FFT 板启动采样前额外等待，建议 0..200 |
| 26 | uint8 | `window_mode`，0=auto, 1=rectangular, 2=Hann |
| 27 | uint8 | `request_flags`，bit0=coherent_hint |

FFT 板校验规则：

- `fft_length` 必须为 `4096`。
- `sample_rate_hz` 只能取支持档位。
- `reference_frequency_hz` 必须小于 `sample_rate_hz / 2`。
- 若 `target_bin != 0xFFFF`，必须满足 `1 <= target_bin <= 2047`。
- 当前非空闲状态时，不启动采样，返回 `BUSY`。

### 4.2 REQUEST_ACCEPTED 0x61

FFT 板确认请求合法且会执行后立即返回。PAYLOAD 固定 18 字节：

| 偏移 | 类型 | 字段 |
|---:|---|---|
| 0 | uint16 | `sweep_id` |
| 2 | uint16 | `point_id` |
| 4 | uint32 | `reference_frequency_hz` |
| 8 | uint32 | `sample_rate_hz` |
| 12 | uint16 | `target_bin`，FFT 板最终采用值 |
| 14 | uint16 | `fft_length`，固定 4096 |
| 16 | uint16 | `accepted_flags` |

收到 `REQUEST_ACCEPTED` 后，Black 不需要再发送任何确认，只等待 `MEASURE_RESULT` 或 `MEASURE_ERROR`。

### 4.3 BUSY 0x62

PAYLOAD 固定 8 字节：

| 偏移 | 类型 | 字段 |
|---:|---|---|
| 0 | uint16 | `sweep_id` |
| 2 | uint16 | `point_id` |
| 4 | uint16 | `busy_state`，1=capturing, 2=fft_processing, 3=sending_result |
| 6 | uint16 | `retry_after_ms` |

Black 收到 `BUSY` 后可以延时重发同一个 `point_id`，但必须使用新的 `SEQ`。

### 4.4 MEASURE_RESULT 0xA1

PAYLOAD 固定 36 字节：

| 偏移 | 类型 | 字段 |
|---:|---|---|
| 0 | uint16 | `sweep_id` |
| 2 | uint16 | `point_id` |
| 4 | uint32 | `reference_frequency_hz` |
| 8 | uint32 | `main_frequency_hz` |
| 12 | uint32 | `voltage_uv_rms` |
| 16 | uint32 | `voltage_uv_peak` |
| 20 | uint32 | `sample_rate_hz` |
| 24 | uint16 | `fft_length` |
| 26 | uint16 | `target_bin` |
| 28 | uint16 | `main_bin` |
| 30 | uint16 | `status` |
| 32 | uint16 | `adc_min_code` |
| 34 | uint16 | `adc_max_code` |

使用 `uv` 而不是 `mv` 是为了保留小信号分辨率。业务层显示时再换算成 mV。

状态位：

```text
bit0 result_valid
bit1 reference_out_of_range
bit2 adc_overrange
bit3 fft_busy
bit4 frame_timeout
bit5 peak_not_near_reference
bit6 amplitude_saturated
bit7 low_snr
bit8 used_hann_window
bit9 used_rectangular_window
bit10 timer_triggered_dma_capture
bit11 adc_dma_error
bit12 used_least_squares
```

Blue 根据芯片修订版本配置 ADC 时钟：Rev.Y 为 36 MHz，Rev.V 的有效 ADC 时钟为 32 MHz。ADC1 保持16位、2.5周期采样时间。
TIM6 由240 MHz APB1定时器时钟分频，生成精确的2.5 MHz TRGO；DMA1 Stream1
以 normal 模式一次搬运 4096 个 halfword。原始缓冲由链接器放在 AXI SRAM，并按
32 字节对齐；DMA 完成后先停止 TIM6，再做 D-Cache invalidate，随后才进入主频幅度计算。

### 4.5 MEASURE_ERROR 0xE1

PAYLOAD 固定 12 字节：

| 偏移 | 类型 | 字段 |
|---:|---|---|
| 0 | uint16 | `sweep_id` |
| 2 | uint16 | `point_id` |
| 4 | uint16 | `error_code` |
| 6 | uint16 | `detail` |
| 8 | uint32 | `reference_frequency_hz` |

错误码建议：

```text
1 invalid_length
2 unsupported_sample_rate
3 reference_out_of_range
4 invalid_target_bin
5 adc_dma_error
6 frame_timeout
7 fft_error
8 protocol_crc_error
```

## 5. 采样率一致性

采样频率由 TIM6 TRGO 决定：

```text
timer_clock_hz = 240000000
PSC = 0
ARR = 95
actual_sample_rate_hz = timer_clock_hz / ((PSC + 1) * (ARR + 1))
                      = 2500000 Hz
```

协议请求、TIM6配置、最小二乘时间轴、回传结果和屏幕显示都必须使用该值。Blue 在定时器初始化完成后读取实际 PSC/ARR 反算；结果不等于请求值时，本次采样启动失败。

`target_bin` 和 `main_bin` 字段为兼容现有协议保留，按真实采样率计算近似频点；最小二乘算法本身不依赖整数频点。

## 6. ADC 数据检查

每帧保留原始最小值和最大值：

```text
adc_min_code
adc_max_code
adc_overrange = adc_min_code <= low_limit || adc_max_code >= high_limit
```

直流偏置不再预先减均值，而是作为最小二乘模型中的常数项 `c` 一起求解。

## 7. 正弦最小二乘模型

已知 DDS 实际参考频率 `f` 和真实采样率 `Fs`，对4096个样本拟合：

```text
y[n] = a*sin(2*pi*f*n/Fs) + b*cos(2*pi*f*n/Fs) + c
```

构造三列设计矩阵 `[sin, cos, 1]`，累加3x3正规方程并使用带主元选择的高斯消元求出 `a`、`b`、`c`。因此即使采样窗口不是整数周期，也不需要 Hann 窗或 FFT 频点搜索。

全局调试状态保留：

```text
fit_sin_uv     = a
fit_cos_uv     = b
fit_offset_uv  = c
```

## 8. 幅值计算

拟合系数先由ADC码值换算为微伏，随后计算：

```text
voltage_uv_peak = sqrt(a*a + b*b)
voltage_uv_rms  = voltage_uv_peak / sqrt(2)
voltage_uv_min  = c - voltage_uv_peak
voltage_uv_max  = c + voltage_uv_peak
voltage_uv_pp   = 2 * voltage_uv_peak
main_frequency_hz = reference_frequency_hz
```

若正规方程不可解或拟合幅值过低，置位 `low_snr`，结果可以回传，但 `result_valid` 不置位。

## 9. 重构模块边界

只新增 FFT 测量相关模块，不改其他模块内部职责。

建议新增：

```text
User/Inc/AdcFftMeasure_User.h
User/Src/AdcFftMeasure_User.c
User/Inc/AdcFftProtocol_User.h
User/Src/AdcFftProtocol_User.c
```

`AdcFftProtocol_User`：

- 解析 `MEASURE_REQUEST`。
- 校验请求字段。
- 管理 `SEQ/sweep_id/point_id` 绑定。
- 发送 `REQUEST_ACCEPTED/BUSY/MEASURE_RESULT/MEASURE_ERROR`。

`AdcFftMeasure_User`：

- 配置固定2.5 MHz采样率，并从定时器寄存器核对真实值。
- 启动 TIM/ADC/DMA 单帧采样。
- 处理 DMA 完成和错误状态。
- 使用 `a*sin()+b*cos()+c` 最小二乘拟合并完成电压换算。
- 输出 `AdcFftMeasurementResult`。

对外接口建议：

```c
typedef struct {
    uint16_t sweep_id;
    uint16_t point_id;
    uint32_t reference_frequency_hz;
    uint32_t dds_ftw;
    uint32_t sample_rate_hz;
    uint16_t target_bin;
    uint16_t pre_capture_delay_us;
    uint8_t window_mode;
    uint8_t flags;
} AdcFftMeasurementRequest;

typedef struct {
    uint16_t sweep_id;
    uint16_t point_id;
    uint32_t reference_frequency_hz;
    uint32_t main_frequency_hz;
    uint32_t voltage_uv_rms;
    uint32_t voltage_uv_peak;
    uint32_t sample_rate_hz;
    uint16_t target_bin;
    uint16_t main_bin;
    uint16_t status;
    uint16_t adc_min_code;
    uint16_t adc_max_code;
} AdcFftMeasurementResult;
```

状态机：

```text
ADC_FFT_IDLE
ADC_FFT_ACCEPTED
ADC_FFT_PRE_DELAY
ADC_FFT_ARM_DMA
ADC_FFT_CAPTURING
ADC_FFT_FRAME_READY
ADC_FFT_PROCESSING
ADC_FFT_RESULT_READY
ADC_FFT_ERROR
```

## 10. 落地顺序

1. 先实现协议解析和回包，不接 ADC，使用假数据回 `MEASURE_RESULT`。
2. Black 侧完成 DDS 设置、等待稳定、发送 `MEASURE_REQUEST`、接收并绑定结果。
3. 测量板接入 TIM/ADC/DMA，使用2.5 MHz验证采样完整性。
4. 使用固定测试正弦验证最小二乘系数 `a/b/c`。
5. 加入电压标定系数，验证 `voltage_uv_rms`。
6. 最后把业务层结果来源替换为 `AdcFftMeasurementResult`。

## 11. 关键验收项

- Black 在 DDS 稳定等待完成后才发送 `MEASURE_REQUEST`。
- FFT 板收到合法请求后先回 `REQUEST_ACCEPTED`，再启动采样。
- 当前测量未结束时，新请求返回 `BUSY`，不会覆盖当前帧。
- 请求、TIM6寄存器反算、计算时间轴和结果回传的采样率均为 `2.5 MHz`。
- `sample_rate_hz=2.5 MHz` 时，4096点采样耗时约 `1.6384 ms`。
- DMA 完成中断每帧只触发一次，不存在每采样点中断。
- 输入非整数周期正弦时，最小二乘幅值仍与示波器测量值接近。
- 改变输入相位时，`a/b`发生变化，但 `sqrt(a*a+b*b)`保持稳定。
- 改变直流偏置时，`c`跟随变化，交流幅值保持稳定。
- ADC 过量程时 `adc_overrange` 置位，`result_valid` 不置位。
- Black 用 `SEQ + sweep_id + point_id + reference_frequency_hz` 绑定结果，不能把旧结果挂到新 DDS 点上。

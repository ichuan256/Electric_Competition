# On-board ADC FFT Measurement Design

本文定义由 FFT 测量板使用板上 ADC 采样并在 MCU 端完成 FFT 的重构方案。Black 板负责驱动 DDS，FFT 测量板负责在 DDS 稳定后采样、计算主频幅度，并把结果包装成：

```text
reference_frequency_hz -> main_frequency_hz, voltage_mv_rms
```

上层业务默认只消费“主频对应的电压值”，不直接依赖原始 ADC 波形或完整频谱。

## 1. 目标与边界

- 单帧采样点数固定 `N = 4096`。
- 采样率可配置，首版默认使用稳定档 `Fs = 2,000,000 Hz`。
- 若 ADC、DMA、模拟前端和 FFT 处理均稳定，可切换到高速档 `Fs = 4,000,000 Hz`。
- 采样由定时器硬件触发，DMA 搬运整帧数据，软件只在 DMA 完成中断后处理。
- Black 板必须先完成 DDS 设置并等待输出稳定，再向 FFT 测量板发起采样请求。
- FFT 测量板不主动改变 DDS，也不根据 UI 频率猜测当前测量点。
- 本轮重构只新增 FFT 测量和通信协议模块，不改动显示、DDS、AGC、旧串口协议等已有模块。

采样率档位建议：

| 档位 | `Fs` | 4096点采样时间 | 频率分辨率 `df = Fs/N` | 用途 |
|---|---:|---:|---:|---|
| stable | 2,000,000 Hz | 2.048 ms | 488.28125 Hz | 默认稳定档 |
| fast | 4,000,000 Hz | 1.024 ms | 976.5625 Hz | 硬件验证后启用 |
| safe | 1,000,000 Hz | 4.096 ms | 244.140625 Hz | 调试/前端带宽较低时 |

首版建议固定 `2 MHz`，把串口握手、DMA 时序、FFT 幅度标定跑稳后再开放 `4 MHz`。

## 2. 采样架构

不采用“每个采样点进一次定时器中断 + 软件读取 ADC”的方式。2 MHz 或 4 MHz 下逐点中断都会让 CPU 负载和采样抖动不可控。推荐链路：

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

普通测量默认优先使用 `Fs = 2 MHz`。现有 LCR 测试模式保持 `1 MHz` DDS 激励不变，
为了避开 `Fs/2` 奈奎斯特边界，该测试请求固定使用 `Fs = 4 MHz`。

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
| 12 | uint32 | `sample_rate_hz`，建议 2000000，允许 1000000/4000000 |
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
```

Blue 当前实现使用独立 PLL3R 为 ADC 提供 48 MHz 内核时钟，ADC1 保持 16 位、2.5 周期采样时间。
TIM6 由 240 MHz APB1 定时器时钟分频，生成精确的 1/2/4 MHz TRGO；DMA1 Stream1
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

## 5. 频率与频点计算

FFT 板按请求中的 `sample_rate_hz` 计算目标 bin：

```text
target_bin = round(reference_frequency_hz * 4096 / sample_rate_hz)
bin_frequency_hz = target_bin * sample_rate_hz / 4096
```

如果 Black 可以让 DDS 落在整数 FFT 点，优先使用：

```text
reference_frequency_hz = k * sample_rate_hz / 4096
```

2 MHz 档位下：

```text
df = 2,000,000 / 4096 = 488.28125 Hz
```

4 MHz 档位下：

```text
df = 4,000,000 / 4096 = 976.5625 Hz
```

若 DDS 无法精确落在整数 bin，FFT 板使用 Hann 窗，并在 `target_bin +/- 2` 内找最大峰作为主频。

## 6. ADC 数据预处理

ADC 原始码先去 DC，再换算成电压序列：

```text
dc_code = mean(adc_raw[0..4095])
x_code[n] = adc_raw[n] - dc_code
adc_lsb_uv = vref_uv / adc_full_scale_code
x_uv[n] = x_code[n] * adc_lsb_uv / analog_gain
```

每帧记录：

```text
adc_min_code
adc_max_code
adc_overrange = adc_min_code <= low_limit || adc_max_code >= high_limit
```

如果前端有固定偏置，可保留慢速校准值 `adc_mid`，但每帧仍建议减均值，避免 DC bin 污染低频测量。

## 7. 窗函数策略

默认策略：

```text
if request_flags.bit0 coherent_hint && abs(reference_frequency_hz - target_bin*Fs/N) 很小:
    rectangular window
else:
    Hann window
```

Hann 窗 coherent gain：

```text
CG = sum(w[n]) / N ~= 0.5
```

rectangular 窗：

```text
CG = 1.0
```

FFT 幅度换算必须除以 `CG`。

## 8. 主频幅度计算

FFT 输入为 `x_uv[n]`。RFFT 输出频谱 `X[k]`，只使用 `1..2047`。

主频搜索：

```text
search_start = max(1, target_bin - 2)
search_end   = min(2047, target_bin + 2)
main_bin     = argmax(real[k]^2 + imag[k]^2)
main_frequency_hz = main_bin * sample_rate_hz / 4096
```

幅度：

```text
mag = sqrt(real[main_bin]^2 + imag[main_bin]^2)
voltage_uv_peak = 2 * mag / (4096 * CG)
voltage_uv_rms  = voltage_uv_peak / sqrt(2)
```

若 `main_bin` 不在参考频率附近或峰值过低，置位 `peak_not_near_reference` 或 `low_snr`，结果可以回传，但 `result_valid` 不应置位。

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

- 配置采样率档位。
- 启动 TIM/ADC/DMA 单帧采样。
- 处理 DMA 完成和错误状态。
- 做去 DC、窗函数、RFFT、主频搜索和电压换算。
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
3. FFT 板接入 TIM/ADC/DMA，先用 `1 MHz` 或 `2 MHz` 验证采样完整性。
4. 接入 CMSIS-DSP RFFT，使用固定测试正弦验证 `main_bin`。
5. 加入电压标定系数，验证 `voltage_uv_rms`。
6. 稳定后再打开 `4 MHz` 档位。
7. 最后把业务层结果来源替换为 `AdcFftMeasurementResult`。

## 11. 关键验收项

- Black 在 DDS 稳定等待完成后才发送 `MEASURE_REQUEST`。
- FFT 板收到合法请求后先回 `REQUEST_ACCEPTED`，再启动采样。
- 当前测量未结束时，新请求返回 `BUSY`，不会覆盖当前帧。
- `sample_rate_hz=2 MHz` 时，4096 点采样耗时约 `2.048 ms`。
- `sample_rate_hz=4 MHz` 时，4096 点采样耗时约 `1.024 ms`。
- DMA 完成中断每帧只触发一次，不存在每采样点中断。
- 输入整数 bin 正弦时，`main_bin == target_bin`。
- 输入非整数 bin 正弦时，`main_bin` 在 `target_bin +/- 2` 内。
- Hann/rectangular 模式经过 `CG` 修正后，同幅度输入的电压结果接近。
- ADC 过量程时 `adc_overrange` 置位，`result_valid` 不置位。
- Black 用 `SEQ + sweep_id + point_id + reference_frequency_hz` 绑定结果，不能把旧结果挂到新 DDS 点上。

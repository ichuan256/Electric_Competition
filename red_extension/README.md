# Red 顺序等效采样扩展集成说明

## 1. 模块用途

`red_extension` 是 Red 测量板的顺序等效采样（ETS，Equivalent-Time Sampling）扩展。
它用于稳定周期信号的 `100 kHz～1 MHz` 实验性测量，不替换题目范围内原有的
`1 kHz～100 kHz` 直接采样流程。

扩展实现以下功能：

1. 通过比较器和定时器捕获输入周期；
2. 根据实测周期规划最多512个相位采样点；
3. 每个相位跨多个输入周期重复采样并平均；
4. 按实际定时器延迟重排、插值为均匀周期；
5. 计算基波、谐波相对幅值和1至5次谐波THD；
6. 使用现有Blue-Red帧格式发送`0x69～0x6F`消息；
7. 结束、取消或失败时统一恢复直接采样的ADC、Timer和DMA配置。

核心代码不包含MSPM0 DriverLib头文件，不直接访问寄存器，也不固定任何引脚、定时器、
ADC、DMA或事件路由。所有硬件访问都由原Red工程提供的异步回调完成。

## 2. 文件说明

| 文件 | 作用 |
| --- | --- |
| `red_ets.h/.c` | 顶层非阻塞状态机和公开API |
| `red_ets_config.h` | 编译开关、资源宏和默认参数 |
| `red_ets_types.h` | 公共类型、状态、质量位和错误码 |
| `red_ets_port.h` | Red工程必须实现的硬件适配回调 |
| `red_ets_capture.h/.c` | 周期校验、抖动计算和相位延迟规划 |
| `red_ets_reconstruct.h/.c` | 实际延迟重排、插值和Q15归一化 |
| `red_ets_fft.h/.c` | 可替换频谱后端和便携参考DFT |
| `red_ets_protocol.h/.c` | CRC16、帧编解码和ETS载荷编解码 |
| `red_ets_integration.h` | 精简集成检查清单 |
| `red_ets_selftest.h/.c` | 与硬件无关的自测入口 |

## 3. 集成原则

### 3.1 与直接采样完全互斥

原有命令`MEASURE_START 0x60`必须始终进入直接采样模式。只有新命令
`ETS_START 0x69`才能启动本模块。

```text
0x60 -> 原直接测量状态机
0x69 -> red_ets_handle_start()
```

开始直接测量前，如果ETS正在运行：

```c
red_ets_abort_for_direct_measurement(&g_red_ets);

while (red_ets_is_busy(&g_red_ets)) {
    red_ets_poll(&g_red_ets);
}

/* 此时restore_direct_mode()已经执行，可以启动原直接采样。 */
direct_measurement_start();
```

不要让直接模式和ETS同时占用ADC、DMA或触发定时器。

### 3.2 必须保留统一恢复入口

所有成功、超时、取消和硬件错误最终都会进入：

```c
red_ets_release(&g_red_ets);
```

`restore_direct_mode()`回调必须重新配置原直接采样所需的：

- ADC通道和采样时间；
- ADC触发源；
- DMA源地址、目标地址和长度；
- 定时器时钟、分频和事件路由；
- 比较器及ETS专用事件；
- 中断标志和DMA完成标志。

禁止只停止ETS定时器后直接跳回原状态机。

## 4. 加入Keil工程

将`red_extension`目录加入Keil的Include Paths，并把以下源文件加入Target：

```text
red_ets.c
red_ets_capture.c
red_ets_reconstruct.c
red_ets_fft.c
red_ets_protocol.c
```

`red_ets_selftest.c`只在开发和回归测试时加入。

工程必须使用C99或更新语言模式。默认参考DFT调用：

```c
sinf()
cosf()
sqrtf()
```

因此使用参考DFT时需要链接数学库。正式工程若复用已有Q15 FFT，应设置：

```c
#define RED_ETS_ENABLE_REFERENCE_DFT 0
```

并调用`red_ets_set_fft_backend()`注册现有FFT适配函数。

## 5. 编译配置

### 5.1 功能开关

```c
#define RED_ENABLE_ETS 1
```

设为0后，`ETS_START`会返回禁用状态；扩展不会启动采样。HELLO能力位bit6也只能在该宏为1时设置。

```c
#if RED_ENABLE_ETS
capability_bits |= RED_ETS_HELLO_CAPABILITY_BIT;
#endif
```

### 5.2 硬件资源宏

这些宏只用于Red BSP适配层，核心不会解释其具体类型：

```c
#define RED_ETS_TIMER_RESOURCE       MY_PHASE_TIMER
#define RED_ETS_ADC_RESOURCE         MY_ADC_INSTANCE
#define RED_ETS_DMA_RESOURCE         MY_ADC_DMA_CHANNEL
#define RED_ETS_COMPARATOR_RESOURCE  MY_TRIGGER_COMPARATOR
#define RED_ETS_EVENT_ROUTE_RESOURCE MY_EVENT_CHANNEL
```

建议在Keil的全局预包含头文件或工程宏中定义，不要修改扩展源码。

## 6. 缓冲区配置

扩展不在内部创建大数组，所有缓存由原工程提供。推荐与直接采样工作区互斥复用：

```c
static uint32_t g_ets_period_ticks[64];
static uint32_t g_ets_phase_sum[512];
static uint16_t g_ets_phase_count[512];
static uint16_t g_ets_delay_ticks[512];
static int16_t  g_ets_uniform_q15[512];
static uint32_t g_ets_spectrum_ppm[32];

static const red_ets_workspace_t g_ets_workspace = {
    .period_ticks      = g_ets_period_ticks,
    .period_capacity   = 64,
    .phase_sum         = g_ets_phase_sum,
    .phase_count       = g_ets_phase_count,
    .delay_ticks       = g_ets_delay_ticks,
    .phase_capacity    = 512,
    .uniform_q15       = g_ets_uniform_q15,
    .uniform_capacity  = 512,
    .spectrum_ppm      = g_ets_spectrum_ppm,
    .spectrum_capacity = 32
};
```

占用约5.5 KiB。如果与直接模式复用内存，必须保证两种状态机不会同时访问该区域。

## 7. 运行参数

先载入默认值，再按实际板级资源修改：

```c
red_ets_runtime_config_t ets_config;

red_ets_runtime_config_defaults(&ets_config);

ets_config.timer_clock_hz = 80000000u;
ets_config.adc_max_rate_hz = 4000000u;
ets_config.min_fundamental_hz = 100000u;
ets_config.max_fundamental_hz = 1000000u;
ets_config.max_phase_bins = 512u;
ets_config.period_samples = 64u;
ets_config.period_discard = 8u;
ets_config.default_averages = 8u;
ets_config.frontend_settle_ms = 300u;
ets_config.trigger_timeout_ms = 100u;
ets_config.phase_timeout_ms = 100u;
ets_config.default_deadline_ms = 10000u;

/* 扩展模拟通带未标定时必须填0。 */
ets_config.calibrated_analog_bandwidth_hz = 0u;
```

这些参数是运行时变量，可以按板卡、时钟配置或测试阶段替换，无需修改算法源文件。

## 8. 硬件回调

### 8.1 获取系统时间

```c
static uint32_t ets_now_ms(void *user)
{
    (void)user;
    return System_GetTickMs();
}
```

必须是单调递增的32位毫秒计数，允许自然回绕。

### 8.2 开始前端稳定等待

```c
static int ets_frontend_settle_begin(void *user)
{
    (void)user;
    AGC_SetTargetVpp(2000u);
    return 0;
}
```

该函数只发起设置，不能在函数内部阻塞300 ms。等待由`red_ets_poll()`根据
`frontend_settle_ms`完成。

### 8.3 启动周期捕获

```c
static int ets_period_capture_begin(uint32_t *period_ticks,
                                    size_t requested_count,
                                    uint32_t timeout_ms,
                                    void *user)
{
    (void)timeout_ms;
    (void)user;

    /* 配置比较器上升沿 -> Timer Capture。 */
    return BSP_ETS_TimerCaptureStart(period_ticks, requested_count);
}
```

捕获内容是相邻有效上升沿之间的周期计数，而不是绝对时间戳。默认捕获64个周期。

完成后从Timer/DMA完成路径调用：

```c
red_ets_period_capture_complete(&g_red_ets, captured_period_count);
```

### 8.4 启动一个相位点采样

```c
static int ets_phase_capture_begin(uint16_t delay_ticks,
                                   uint16_t cycle_divider,
                                   uint8_t averages,
                                   uint16_t clip_low_code,
                                   uint16_t clip_high_code,
                                   uint32_t timeout_ms,
                                   void *user)
{
    (void)timeout_ms;
    (void)user;

    /*
     * 每次比较器上升沿复位相位Timer；延迟delay_ticks后触发ADC。
     * 每cycle_divider个输入周期最多触发一次，共取得averages个有效样点。
     */
    return BSP_ETS_PhaseAdcStart(delay_ticks,
                                 cycle_divider,
                                 averages,
                                 clip_low_code,
                                 clip_high_code);
}
```

DMA完成后计算有效样点之和、有效数量和削顶状态，然后调用：

```c
red_ets_phase_capture_complete(&g_red_ets,
                               adc_sample_sum,
                               valid_sample_count,
                               clipped);
```

不要每取得一个ADC样点就调用该函数。它表示当前整个相位点的平均采样已经完成。

### 8.5 取消和恢复

```c
static void ets_capture_cancel(void *user)
{
    (void)user;
    BSP_ETS_StopDma();
    BSP_ETS_StopAdcTrigger();
    BSP_ETS_StopTimerCapture();
}

static void ets_restore_direct_mode(void *user)
{
    (void)user;
    BSP_DirectMeasurement_ReinitializeAdcTimerDma();
}
```

这两个函数应设计为幂等函数，多次调用不会产生副作用。

### 8.6 发送协议消息

```c
static int ets_send_message(uint8_t command,
                            uint8_t flags,
                            uint16_t sequence,
                            const uint8_t *payload,
                            uint16_t payload_length,
                            void *user)
{
    (void)user;

    return BlueLink_QueueFrame(RED_ETS_NODE_BLUE,
                               RED_ETS_NODE_RED,
                               command,
                               flags,
                               sequence,
                               payload,
                               payload_length);
}
```

返回0表示已经成功进入发送队列；非0表示队列忙，状态机会在后续`poll`中重试。
ACK等待、200 ms超时、RETRY标志和最多3次重发仍由现有BlueLink传输层管理。

如果现有Red工程还没有统一组帧函数，也可以调用：

```c
red_ets_protocol_encode_frame(...);
```

它会生成完整的：

```text
D3 91 VER DST SRC CMD FLAGS SEQ LEN PAYLOAD CRC16 91 D3
```

## 9. 初始化示例

```c
#include "red_ets.h"

static red_ets_context_t g_red_ets;

static int direct_measurement_busy(void *user)
{
    (void)user;
    return Measurement_IsBusy();
}

void RedEts_Init(void)
{
    red_ets_runtime_config_t config;
    red_ets_port_ops_t port;

    red_ets_runtime_config_defaults(&config);
    config.timer_clock_hz = BSP_ETS_TIMER_ACTUAL_HZ;
    config.adc_max_rate_hz = BSP_ETS_ADC_MAX_RATE_HZ;
    config.calibrated_analog_bandwidth_hz = 0u;

    memset(&port, 0, sizeof(port));
    port.now_ms = ets_now_ms;
    port.frontend_settle_begin = ets_frontend_settle_begin;
    port.period_capture_begin = ets_period_capture_begin;
    port.phase_capture_begin = ets_phase_capture_begin;
    port.capture_cancel = ets_capture_cancel;
    port.restore_direct_mode = ets_restore_direct_mode;
    port.send_message = ets_send_message;
    port.direct_measurement_busy = direct_measurement_busy;
    port.user = NULL;

    if (red_ets_init(&g_red_ets, &config, &g_ets_workspace, &port) != RED_ETS_OK) {
        Error_Handler();
    }
}
```

如果工程未通过其他头文件包含`string.h`，初始化文件需要自行包含：

```c
#include <string.h>
```

## 10. 主循环调用

`red_ets_poll()`是非阻塞函数，应放入现有主循环或周期任务：

```c
int main(void)
{
    Board_Init();
    RedEts_Init();

    for (;;) {
        BlueLink_Poll();
        Measurement_Poll();
        red_ets_poll(&g_red_ets);
    }
}
```

不要在中断中调用`red_ets_poll()`、频谱计算或协议分片发送。

## 11. UART接收路由

现有Blue-Red协议解析器完成CRC和地址检查后，只需增加一个命令分支：

```c
switch (frame.command) {
case 0x60u:
    /* 原直接采样入口，保持不变。 */
    red_ets_abort_for_direct_measurement(&g_red_ets);
    DirectMeasurement_HandleStart(&frame);
    break;

case RED_ETS_CMD_START:
    (void)red_ets_handle_start(&g_red_ets,
                               frame.sequence,
                               frame.payload,
                               frame.payload_length);
    break;

default:
    ExistingProtocol_Dispatch(&frame);
    break;
}
```

如果直接测量不能等待ETS释放，应先设置“直接测量待启动”标志，在
`red_ets_is_busy()==0`后再真正初始化ADC和DMA。

## 12. ETS_START载荷

`ETS_START 0x69`固定16字节，小端序：

| 偏移 | 类型 | 字段 |
| ---: | --- | --- |
| 0 | `u32` | `request_token` |
| 4 | `u8` | `mode`，固定1 |
| 5 | `u8` | bit0波形、bit1频谱、bit2五谐波THD |
| 6 | `u16` | 相位点数，0为自动，最大512 |
| 8 | `u8` | 平均次数，0为默认，可选1/2/4/8/16 |
| 9 | `u8` | 最高谐波，默认5 |
| 10 | `u16` | 截止时间ms，0为默认10000 |
| 12 | `u32` | 允许的最高基波Hz，首版不超过1000000 |

相同`SEQ + request_token`的重复命令只重新ACK，不会重新启动采样。

## 13. 输出命令

| CMD | 输出内容 |
| ---: | --- |
| `0x6A` | 状态、进度、耗时、相位点数量、质量位 |
| `0x6B` | 基频、THD、ADC速率、等效采样率和有效谐波数 |
| `0x6C` | 波形点数和格式声明 |
| `0x6D` | Q15周期波形分片，每片最多60点 |
| `0x6E` | 相对幅值ppm频谱，每片最多31个bin |
| `0x6F` | 事务完成和分片完整标志 |

通用`ACK 0x7F`和`ERROR 0x7E`继续沿用现有协议。

## 14. 替换为现有Q15 FFT

正式版本推荐复用Red原工程的Q15实数FFT，而不是长期使用便携DFT。适配函数原型：

```c
static int red_q15_fft_adapter(const int16_t *samples,
                               uint16_t sample_count,
                               uint8_t harmonic_limit,
                               uint32_t *magnitude_ppm,
                               size_t magnitude_capacity,
                               uint32_t *thd_ppm,
                               void *user)
{
    /*
     * 1. 在原FFT工作区原地计算；
     * 2. magnitude_ppm[1]固定归一化为1000000；
     * 3. 填充0到harmonic_limit；
     * 4. thd_ppm = sqrt(U2^2+...+U5^2)/U1 * 1000000。
     */
    return RED_ETS_OK;
}
```

初始化后注册：

```c
red_ets_set_fft_backend(&g_red_ets, red_q15_fft_adapter, fft_workspace);
```

## 15. 质量位注意事项

没有完成扩展模拟通带标定时：

```c
config.calibrated_analog_bandwidth_hz = 0u;
```

模块仍可返回原始重建波形和频谱，但会设置：

```text
ETS_ANALOG_BW_UNCALIBRATED
ETS_ALIAS_OR_HARMONIC_AMBIGUOUS
```

并且不会设置`ETS_VALID`。不得为了让界面显示“有效”而虚构带宽数值。

只有同时满足以下条件才可能置有效位：

- 触发周期稳定；
- 相位点完整且无重复延迟；
- 输入未削顶；
- 基波位于允许范围；
- 等效奈奎斯特频率覆盖目标谐波；
- 已标定模拟带宽覆盖目标谐波。

## 16. 自测

`red_ets_selftest_run()`不访问硬件，覆盖：

- `ETS_START`载荷解析；
- 100 kHz周期规划；
- 512相位延迟表；
- 周期插值和Q15归一化；
- 基波频谱与THD；
- 完整帧编码、CRC和解码。

```c
int check = red_ets_selftest_run();
```

返回0表示通过，其他值表示失败检查编号。该函数建议只在PC测试或开发固件中使用，验收固件可不编译。

## 17. 最小集成检查表

- [ ] `RED_ENABLE_ETS=1`；
- [ ] HELLO能力位bit6已设置；
- [ ] `0x60`仍只进入直接测量；
- [ ] `0x69`路由到`red_ets_handle_start()`；
- [ ] 主循环持续调用`red_ets_poll()`；
- [ ] Timer捕获完成时调用`red_ets_period_capture_complete()`；
- [ ] 每个相位DMA完成时调用`red_ets_phase_capture_complete()`；
- [ ] 所有Port回调均为非阻塞；
- [ ] `restore_direct_mode()`能完整恢复原ADC/Timer/DMA；
- [ ] UART发送回调使用现有ACK和重发队列；
- [ ] 未标定带宽保持为0；
- [ ] 执行“ETS失败→直接测量→ETS成功→直接测量”回归测试。


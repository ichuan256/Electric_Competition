# 单片机端FFT扫频开发交接说明

本文供负责单片机程序的Codex或开发者直接接手。FPGA端已经实现独立的AD9226
50MHz采样、4096点FFT和FFT专用UART解析器。单片机负责DDS、扫频流程、结果
配对和上层显示，FPGA不会主动改变DDS频率。

协议的唯一规范文件是`FFT_UART_PROTOCOL.md`。如果本文与协议文件不一致，以协议
文件为准。

## 开发目标

单片机程序需要完成以下模块，建议保持相互独立：

1. `dds_driver`：设置DDS频率/FTW，并返回硬件真正采用的FTW和实际频率。
2. `fft_protocol`：FFT帧序列化、CRC、流式解码和字段读写。
3. `sweep_controller`：逐点设置DDS、等待稳定、请求FFT、等待结果和超时重试。
4. `result_store`：按`sweep_id + point_id`保存频率、复数FFT值和状态。
5. 原DAC波形协议模块：继续保留，但不得把FFT帧交给旧协议解析器。

禁止把DDS设置、UART中断和FFT帧解析全部写在一个阻塞函数中。UART接收建议采用
中断或DMA写环形缓冲区，主循环/任务负责解析完整帧。

## 频率统一

FPGA参数固定：

```text
ADC采样率 Fs = 50,000,000 Hz
FFT长度    N  = 4096
频点间隔   Δf = Fs/N = 12,207.03125 Hz
```

相干扫频推荐用整数频点：

```text
target_bin = k, 0 <= k <= 2048
requested_frequency = k * 50,000,000 / 4096
```

DDS通常不能精确产生请求频率，因此命令中不得发送用户界面的理想值。应先计算并
写入DDS FTW，再根据实际系统时钟和FTW反算`frequency_mHz`：

```text
frequency_mHz = round(dds_ftw * DDS_CLOCK_HZ * 1000 / 2^FTW_BITS)
target_bin = round(frequency_mHz * 4096 / (50,000,000 * 1000))
```

上式必须使用64位或更宽的中间变量，避免乘法溢出。若DDS芯片的频率公式不同，
只替换`dds_driver`中的换算，UART仍发送硬件实际FTW和实际频率。

## 推荐状态机

```text
IDLE
  -> SET_DDS
  -> WAIT_DDS_SETTLE
  -> SEND_FFT_REQUEST
  -> WAIT_FFT_RESULT
       -> 验证SEQ和扫频元数据
       -> 保存Re/Im、指数、OTR和状态
       -> NEXT_POINT
  -> FINISHED
```

任意时刻只允许一个未完成的FFT请求。`SEQ`每发送一个新请求递增，溢出后自然回到
0。重试同一个频点时建议继续使用原`point_id`，但分配新的`SEQ`，这样可以识别
延迟到达的旧结果。

推荐超时：

```text
timeout_ms = ceil(settle_us / 1000) + 100
```

4096点采样本身约81.92us；115200波特率发送一个约50字节结果帧约4.34ms。
100ms余量用于FFT处理、任务调度和串口缓冲。首次超时可重发一次，连续失败应停止
扫频并报告通信错误，不应无上限重试。

## 接收解析器

推荐解析顺序：

1. 在环形缓冲区搜索连续字节`F9 26`。
2. 至少收到9字节后读取`LEN_L/LEN_H`。
3. 拒绝`LEN > 37`，丢弃当前第一个`F9`后重新同步。
4. 等待总长度`13 + LEN`字节。
5. 检查帧尾`62 9F`。
6. 对`VER`至PAYLOAD末尾计算CRC。
7. 只把`VER=01、TARGET=A2、CMD=91、LEN=37`交给扫频控制器。

不要使用“UART空闲1ms代表一帧结束”之类的规则。帧可以一次发送，也可以分成多次
UART发送；只要字节顺序连续，解析结果必须相同。

多字节字段统一小端序。建议实现`put_u16_le/put_u32_le/put_u64_le`和对应的
`get_*_le`函数，不要直接发送带有编译器填充的C结构体。

## FFT结果的数值处理

FPGA返回的是目标频点复数结果：

```text
Re = result.real
Im = result.imag
幅度平方 = Re*Re + Im*Im
相位 = atan2(Im, Re)
```

FFT使用块浮点缩放，跨频点比较绝对幅度时必须结合`block_exponent`统一尺度。第一版
调试可先保存原始`Re/Im/block_exponent`，不要过早转换成浮点。计算`Re*Re`和
`Im*Im`时至少使用64位有符号中间变量。

如果状态bit0为0、bit1为1、bit3为1，当前结果无效。bit2为1或`otr_count != 0`
表示ADC输入曾超量程，单片机应降低DDS幅度或调整模拟前端后重新测量。

## 与旧DAC协议共存

同一UART可以同时承载两类协议：

```text
DAC协议：A5 5A ... 5A A5
FFT协议：F9 26 ... 62 9F
```

接收分发器看到`A5 5A`时送旧DAC解析器，看到`F9 26`时送FFT解析器。不要让两个
解析器同时消费同一个环形缓冲区；应由最外层分发器确定帧类型后再移交。

## 最低验收项目

开发完成后至少验证：

- CRC黄金帧得到`0xC4A3`，线上字节为`A3 C4`。
- 将一帧拆成任意多次UART发送仍能正确解析。
- 在帧前插入随机字节后能够重新找到`F9 26`。
- CRC错误、错误帧尾、错误长度不会启动下一扫频点。
- 结果`SEQ`不匹配时会丢弃，不会误绑定到当前DDS频率。
- `frequency_mHz`与`dds_ftw`回显不匹配时会报错。
- 连续扫频严格一发一收，不会在上一点完成前切换DDS。
- OTR状态能够传递到上层并触发重新测量或错误提示。
- 原DAC协议仍能独立使用，FFT帧不会被识别为DAC命令。

## 给接手Codex的实现提示

开始开发前先读取：

- `FFT_UART_PROTOCOL.md`
- 单片机现有UART驱动和旧DAC协议实现
- DDS芯片型号、DDS参考时钟、FTW位数和当前调频函数

需要优先确认的硬件参数只有三个：DDS型号、DDS参考时钟、FTW位数。确认后先实现
协议编解码单元测试，再接UART，最后接入DDS扫频状态机。不要修改FPGA协议常量，
除非FPGA和单片机两端在同一次版本升级中同步修改。

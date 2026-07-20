# 第三轮信号失真度测量系统总览

## 1. 目标

在第二轮 `Black + Blue + gilding` 系统基础上新增 TI 开发板 `Red`，组成第三轮系统。
Red 使用 TI MCU 片内 ADC 自动测量输入周期信号；Black 复用按键和系统交互；Blue 复用 LCD，
并作为无线显示端显示 THD、一个周期波形及基波到 5 次谐波的归一化幅值；gilding FPGA
接收 Red 的谐波测量结果，通过 AD9744 DAC 重建波形并输出到示波器。

## 2. 功能需求

### 2.1 必须实现

1. 输入峰峰值 300 mV 至 600 mV，基频 1 kHz，THD 5% 至 50%。
2. 仅统计 2 至 5 次谐波：
   `THD = sqrt(U2^2 + U3^2 + U4^2 + U5^2) / U1 * 100%`。
3. 基本要求的 THD 绝对误差不超过 5 个百分点。
4. 从 Black 按键启动，经 Blue 蓝牙转发到 Red，再到 Blue 显示测量 THD，不超过 10 秒。
5. 主控制器和数据采集器使用 TI MCU 及其片内 ADC，不使用片外 ADC 或成品采集模块。

### 2.2 发挥部分

1. 输入峰峰值扩展到 30 mV 至 600 mV。
2. 基频扩展到 1 kHz 至 100 kHz。
3. THD 绝对误差收紧到不超过 3 个百分点。
4. 显示一个周期波形。
5. 显示 `U1/U1` 至 `U5/U1` 的归一化幅值。
6. Red 测量装置与 Blue 显示模块之间使用双向蓝牙通信。
7. FPGA 根据 Red 测得的基频、1 至 5 次谐波幅值和相位重建周期波形，通过 DAC 输出到示波器。

## 3. 非功能需求

- 一键自动完成量程选择、基频估计、采样、运算和显示，测量期间不需要人工操作。
- 从 Black 启动到 Red 完成测量目标不超过 2 秒，到 Blue 无线显示目标不超过 3 秒。
- 算法层不依赖 MCU 寄存器，可在 PC 上用合成波形和示波器导出数据回归测试。
- 无效输入、削顶、信号过小、频率越界和无线超时必须显示明确状态，不输出伪测量值。

## 4. 约束与假设

1. Red 已冻结为 `LAUNCHXL-F28379D`，主控为 `TMS320F28379D`，当前使用 CPU1 Driverlib 工程。
2. Red 的 ADC、定时器、DMA 和 SCI 驱动放在 BSP/用户模块层，DSP API 与蓝牙协议不直接依赖寄存器对象。
3. Red 开发环境采用 Code Composer Studio + C2000Ware，工程名固定为 `Red`。
4. 本阶段先完成 Blue-Red 蓝牙链路校验；第二轮测量代码不得直接覆盖或默认复用到第三轮。
5. 第三轮保留并继续修改根目录现有 `Black/`、`Blue/`、`gilding/`，只新增 `Red/` 放置 TI 工程。
6. 输入为单端周期电压信号；除该输入外，不给测量装置引入外部参考时钟、同步或触发信号。
7. Red 是测量主控和数据采集器；Black/Blue 不得代替 Red 执行 ADC 采样或最终 THD 计算。
8. 开发板丝印可见板载OPA2365高速ADC缓冲电路，但是否接入正式输入以及对应ADC引脚，
   必须对照原理图和跳线状态确认，不能只凭照片假定连接关系。
9. 第三轮所有 MCU/FPGA 工程的代码优化等级统一为 `O0`（关闭优化）；每次构建均需核对实际编译参数，未经明确确认不得改为其他优化等级。Blue 的工程设置以人工已修改版本为准，后续自动修改不得覆盖。
10. UART接收采用“硬件接收搬运层 + 软件环形缓冲区 + 主循环协议解析”的统一新结构：硬件支持UART DMA时优先使用DMA循环接收；硬件不提供对应DMA触发源时，使用UART/SCI RX FIFO中断立即搬运到软件环形缓冲区。禁止新增主循环直接轮询RXNE、RX FIFO或接收寄存器的方案。Black与Blue之间第二轮遗留的老通信结构暂不迁移，其他新增UART均遵循本条；Blue-Red链路中Blue UART4使用DMA1 Stream3循环接收，Red因F28379D DMA无SCI触发源而使用SCIB RX FIFO中断加环形缓冲区。

## 5. 总体方案

```text
Black（按键） --现有有线 BoardComm--> Blue（LCD/蓝牙主站）
                                          |
                                   双向蓝牙串口链路
                                          |
输入信号 --> 保护/偏置 --> Red TI MCU 片内 ADC --> DMA --> DSP/THD/谐波相位
                                          |
                                          +--蓝牙--> Blue --现有UART--> gilding FPGA
                                                                           |
                                                                  AD9744 DAC --> 示波器
```

- 单个片内 ADC 由硬件定时器等间隔触发，DMA 连续保存一整帧样点。
- 正式帧的采样通道和采样间隔全程不变，不进行多通道扫描或交替采样。
- 从正式帧估计基频，并计算 1 至 5 次谐波相量。
- THD 只由 2 至 5 次谐波计算；直流、6 次及以上频谱能量不进入题目结果。
- FPGA 只按 Red 的测量结果重建波形，不参与 ADC 采样、基频估计或 THD 计算。

## 6. 模块边界

| 模块 | 职责 | 第二轮资产处理 |
| --- | --- | --- |
| `BSP/adc_capture` | 单ADC、定时触发、DMA、等间隔采样帧 | 重写 TI 驱动 |
| `DSP/frequency` | 基频粗估计与精估计 | 复用算法思想 |
| `DSP/harmonics` | 去直流、窗函数、1 至 5 次相量 | 从 FFT/拟合代码提炼 |
| `DSP/thd` | 归一化幅值、THD、质量标志 | 新增纯算法模块 |
| `App/measurement` | 一键测量状态机与超时 | 复用状态机模式 |
| `Blue/BluetoothLink` | 转发 Black 启动事件，接收测量结果 | 在 Blue 工程新增 |
| `Red/Protocol/blue_link` | 蓝牙控制、状态、结果、波形帧 | 复用 CRC16 帧思想 |
| `Blue` 显示页 | 完整测量显示 | 在原 Blue 工程内派生页面 |
| `Blue/FpgaReconstruction` | 把 Red 谐波结果换算并转发给 FPGA | 在 Blue 工程新增 |
| `gilding/thd_reconstruction` | 1 至 5 次正弦合成和 AD9744 输出 | 在 FPGA 工程新增 |

## 7. 实施阶段

1. 从 C2000Ware 的 F28379D CPU1 Empty Driverlib 示例派生 `Red` 工程，先跑通 SCI-B 蓝牙链路，
   再逐项接入定时器触发单 ADC 和 DMA 4096 点整帧采集。
2. 在 PC 测试中实现基频估计、五次谐波相量和 THD，覆盖非相干采样、噪声和直流偏置。
3. 固定 1 kHz / 300 至 600 mVpp，完成基本要求闭环。
4. 完成单路低失真前端和正式采样前的增益选择，扩展至 30 mVpp；采样过程中增益保持不变。
5. 加入自适应采样率和 1 至 100 kHz 全范围测试。
6. 打通 Black 到 Blue 的启动事件、Blue-Red 蓝牙协议、一个周期波形和 Blue 显示页面。
7. 建立幅频响应校准表、误差预算与整机验收数据。
8. 增加 Blue 到 FPGA 的重建命令和 FPGA 五谐波合成模块，验证示波器重建波形。

## 8. 验收门槛

- 1 kHz、300 至 600 mVpp、THD 5% 至 50%：绝对误差不超过 5 个百分点。
- 1 kHz 至 100 kHz、30 至 600 mVpp：目标绝对误差不超过 3 个百分点。
- 各测试点连续测量 10 次，THD 极差目标不超过 0.5 个百分点。
- Black 启动至 Red 测量完成小于 2 秒，至 Blue 无线显示小于 3 秒，任何情况下不超过 10 秒。
- 波形显示恰好覆盖一个稳定周期，归一化幅值显示到 5 次谐波。
- 示波器上的 DAC 重建波形基频与 Red 测量值一致，1 至 5 次谐波幅值比例满足标定误差要求。

## 9. 顶层设计

### 9.1 方案决策

第三轮保留第二轮的Black、Blue和gilding，在此基础上增加LAUNCHXL-F28379D Red板，不替换
原有两块STM32H750。Red使用F28379D的一个片内ADC，以定时器固定间隔触发并由DMA
保存4096点帧，再完成基频、五次谐波和THD分析。板载调试器用于下载和调试。

### 9.2 三板职责

| 板卡 | 第三轮职责 | 是否负责 THD 采集/计算 |
| --- | --- | --- |
| Black | 复用键盘和系统交互；通过现有有线 BoardComm 向 Blue 发送启动事件 | 否 |
| Red | TI 测量主控、片内 ADC、基频估计、U1-U5 与 THD、波形抽取 | 是，且必须由它完成 |
| Blue | 复用 LCD；通过蓝牙控制 Red，接收并显示完整测量结果 | 否 |
| gilding | 接收 Red 测得的五谐波参数，数字合成后通过 AD9744 输出到示波器 | 否 |

```text
Black --现有有线 BoardComm--> Blue <--双向蓝牙--> Red <-- 唯一外部被测输入
                              |
                         现有 FPGA UART
                              |
                         gilding FPGA --> AD9744 DAC --> 示波器
```

Black 仍只与 Blue 连接。Blue 收到 Black 的启动按键事件后，通过蓝牙向 Red 发送一次
`MEASURE_START`。Red 不接受外部基频、采样率或谐波参数，独立完成自动检测和测量。

### 9.3 模拟输入

建议信号链：

```text
BNC --> 限流/ESD --> AD603 AGC --> ADA4817 V4.1高速加法器叠加1.65 V偏置 --> TI 片内 ADC
```

- 通带至少覆盖 1 kHz 至 500 kHz，因为最高需测量 100 kHz 基波的 5 次谐波。
- 题目明确将标称THD限定为只处理到5次谐波，并由参赛队用发生器的“谐波发生”功能设置输入。设计假设6次以上谐波和500 kHz以上干扰远小于1至5次分量，不增加会明显损伤500 kHz幅相的高阶窄过渡带抗混叠滤波器。
- 加法器到ADC之间仅保留简单的串联隔离电阻和小电容RC，用于抑制射频干扰和ADC采样回踢；截止频率应明显高于500 kHz，具体值与ADC采样保持时间联合仿真和实测，并在1至500 kHz做幅相标定。
- AGC把送入ADC的整体交流波形稳定在约2.0 Vpp；加法器叠加1.65 V直流偏置，标称ADC波形范围为0.65至2.65 V。
- 加法器使用`双路加法器模块V4.1`（ADA4817-1）的同相相加配置；模块使用±5 V线性电源，标称-3 dB带宽110 MHz，两路输入阻抗约1 kΩ，输出串联50Ω。
- 加法器A输入接AD603模块的2.0 Vpp交流输出，B输入接低噪声1.65 V直流基准。ADC为高阻负载，不在ADC端并联50Ω终端，否则会造成幅值分压。
- AGC使用`V2.0-宽带AGC-AD603`模块：两级AD603、AD8307检波、NE5532环路放大及OPA695输出缓冲。模块需稳定的±5 V线性电源，输入和输出均按50 Ω链路连接。
- 该模块是闭环AGC；外部DAC设定的是目标输出幅度参考，不是直接设定AD603增益。V2.0参考程序使用 `Vdac_mV = 223*ln(Vpp_target_mV)+502.3` 作为初始拟合，因此2.0 Vpp初值约对应2.20 V DAC输出，最终必须按实际模块标定。
- Red 的 F28379D 片内 DAC 专用于 AD603 模块的幅度目标设定，输出在两次采样和分析期间保持不变；具体 DAC 通道和引脚后续冻结。
- ADA4817加法器的1.65 V偏置由Red测量端独立的5 V/3.5 W精密电源产生，已知电源纹波小于1 mV；不占用Red DAC，也不从Blue板跨板传输模拟电压。
- 偏置链路暂定为`5 V -> 20.3 kΩ/10.0 kΩ 0.1%精密分压 -> 10 µF || 100 nF滤波 -> 运放电压跟随器 -> 加法器B输入`。跟随器必须能稳定驱动约1 kΩ负载；不得把高阻分压点直接接入加法器。
- 精密电源、跟随器、AD603、加法器和Red必须在测量端单点共地。1.65 V的少量直流误差由Red去直流处理消除，但仍需实测带载电压、频谱噪声和上电稳定性。
- 模块标称增益范围约-40至40 dB，可覆盖30 mVpp放大至2.0 Vpp所需的36.5 dB；标称通带远高于500 kHz。
- 按3.3 V ADC参考设计，前级必须把所有瞬时样点控制在0.2至3.1 V内；该范围包含AGC过冲、噪声和分量叠加裕量。
- 该模块原理图没有增益保持开关，正式采样时AGC环路保持闭环。设定DAC后先等待环路稳定；资料提示某些工况的稳定时间可达200 ms，设计等待值暂取300 ms。
- 测量操作前提为“先接入并保持被测信号，再按键启动测量”。Red收到开始命令后仍固定等待0.3 s，然后进行输入范围检查和第一帧采样。
- 基波真实绝对幅值的反推和AGC增益标定暂缓；当前Red优先输出THD、五个相对幅值和五个相位。
- 运放、偏置源、保护二极管与无源器件必须使用低失真器件，前端自身 THD 目标低于 0.2%。
- 将样点换算电压低于0.2 V或高于3.1 V记为越界点；任意一帧中连续2个越界点，或累计4个越界点，即判为削顶/前级控制失败并不输出有效THD；单个孤立毛刺只记录质量标志。

### 9.4 数字核心

- `TIMER_EVENT -> ADC -> DMA`，采样期间 CPU 不逐点处理中断。
- ADC使用12位模式，正参考选择板上`VDD`（标称约3.3 V），负参考为`VSS`。固件保留可配置的实测参考电压标定值，不假定恒等于3.300 V；该标定主要用于日后的基波绝对幅值换算，THD和归一化幅值为比值，对参考电压绝对误差不敏感。
- ADC原始缓存使用16位样点，固定4096点、占8 KiB；两次采样先后覆盖同一个ADC缓存，任何时刻都不同时保存两帧。
- 另设一个4096×16位、8 KiB的FFT工作区。每帧DMA完成后把ADC数据去直流、缩放为Q15并复制到FFT工作区，Q15实数FFT只在工作区内原地覆盖；正式帧的ADC时域数据保留并随后原地转换为Q15，供11参数联合最小二乘使用。
- 两次4096点FFT复用同一个8 KiB FFT工作区；Q15蝶形乘法使用32位中间值并明确定义逐级缩放和饱和规则。FFT的统一幅值缩放不影响峰值搜索和三点插值。
- Red 仍采用定长静态内存：8 KiB ADC 缓存、8 KiB FFT 工作区、拟合矩阵、协议缓存、栈和状态机均需通过链接映射核算；不建立无上限或重复的大尺寸浮点数组。
- 运算时复用工作区，不长期保存完整浮点数组；显示波形压缩为 256 点以内。
- 定时器周期在一帧采样期间保持恒定，实际采样率必须由时钟和分频参数反算并交给算法层。
- Black 按键是测量启动入口；无线端不能注入采样时钟或替代 Red 的自动测量流程。

### 9.5 显示与无线

Blue 是第三轮蓝牙显示和数据桥接模块，在原有 LCD 工程中增加 THD 页面，显示 THD、一个
周期波形、1 至 5 次归一化幅值和错误状态；同时把 Red 的谐波重建参数转发给 FPGA。
Red 可保留串口日志，但不假设 Red 自带显示屏。

Blue 和 Red 各连接一个 3.3 V UART 蓝牙透传模块，两模块一主一从并设置为上电自动配对重连。
主机装在 Blue 或 Red 均可；透明链路建立后 UART 数据双向传输，应用层不依赖蓝牙角色和具体型号。

### 9.6 测量状态机

```text
IDLE
  -> START_RECEIVED    接收 Blue 经蓝牙发送的开始命令
  -> AGC_SETTLE        设定2.0 Vpp目标并等待闭环稳定
  -> INPUT_CHECK       短帧检查幅值与削顶
  -> COARSE_CAPTURE    1.25 MSPS采集第一帧4096点
  -> COARSE_FFT        第一次FFT粗估基频
  -> FORMAL_CAPTURE    按粗估基频调整采样率并釆集第二帧4096点
  -> FORMAL_FFT        第二次FFT精化基频
  -> ANALYZE           去直流并求 U1..U5
  -> VALIDATE          检查基波、削顶、噪声和频率边界
  -> PUBLISH_BLUE      蓝牙发送结果、谐波和压缩波形
  -> WAIT_RECON_ACK    等待 Blue 回报 FPGA 重建提交结果
  -> DONE / ERROR
```

每个状态都有独立超时。无线发送失败只产生通信告警，不应清除 Red 已得到的测量结果。

#### 9.6.1 十秒时延预算

系统上电后Blue和Red应预先完成蓝牙绑定和连接；测量命令不等待现场配对。若链路未就绪，
Blue直接显示通信错误，不进入10 s测量流程。从Black按键生效到Blue显示完整结果的目标预算为：

| 阶段 | 设计上限 |
| --- | ---: |
| Black→Blue→Red命令传递 | 50 ms |
| AD603闭环稳定 | 300 ms |
| 两帧4096点ADC实际采集 | 25 ms |
| 第一次4096点Q15 FFT | 300 ms |
| 第二次4096点Q15 FFT | 300 ms |
| 频率精化与11参数联合最小二乘 | 700 ms |
| 校验、结果组帧 | 100 ms |
| Red→Blue结果、谐波和压缩波形传输 | 500 ms |
| Blue显示并向FPGA提交重建参数 | 300 ms |
| 一次通信重发和调度裕量 | 1000 ms |
| 预算合计 | 3.575 s |

工程内部目标为5 s内显示，题目硬上限为10 s。FFT和最小二乘时间是开发预算，Red首版
必须用定时器实测；若算法总时间超过1.5 s，优先优化Q15 FFT、三角函数和矩阵累加，不削减采样和校验步骤。

低置信度自动重测是可删除的非必需功能，不与首次测量主流程耦合。实现时使用独立编译期开关
`RED_ENABLE_AUTO_REMEASURE`，置0时不编译重测分支，不保留隐式延时、额外采样或等待状态。
开启时仅对低信噪比、两次频率不一致或拟合残差过大自动重测一次；削顶直接报错。
只有从本次启动起已用时间小于6 s时才允许进入重测，Red测量内部在9 s强制截止，
保留至少1 s给结果通信、Blue显示和FPGA提交。若首版整机实测的最坏时间不能稳定满足
10 s，验收版直接关闭该开关，首次结果不合格时报告无效而不重测。

### 9.7 采样策略

正式测量使用一个ADC通道等间隔连续采样，每一帧固定4096点。为兼顾1 kHz至100 kHz、
五次谐波和32 KB SRAM，使用两个先后执行的4096点阶段：

#### 阶段A：基频粗测

```text
Fs_coarse = 1.25 MSPS
N_coarse  = 4096
T_coarse  = 3.2768 ms
```

该采样率的奈奎斯特频率为625 kHz，高于100 kHz基波的五次谐波500 kHz，因此题目定义的
1至5次谐波在粗测帧中不会混叠回基频。流程为去直流、Hann窗、4096点实数Q15 FFT，
在1 kHz至100 kHz范围内寻找能够匹配 `f0、2f0...5f0` 结构的最低候选频率，再对峰值格点
`k-1、k、k+1`的对数幅值做三点抛物线插值。不得简单把最大谱峰无条件当作基频。在1 kHz下粗测帧覆盖
约3.28个周期，只用于得到第二阶段的采样率和频率搜索窗，不输出最终幅相结果。

#### 阶段B：正式测量

根据粗测基频设置正式采样率：

```text
Fs_formal = clamp(40 * f0, 200 kSPS, 4 MSPS)
N_formal  = 4096
```

这样100 kHz基波使用4 MSPS，五次谐波每周期仍有8点；1 kHz基波使用200 kSPS，正式帧
覆盖约20.5个周期。DMA完成前不改变ADC通道、增益、定时器周期或ADC配置。

正式帧算法采用“第二次4096点FFT精化f0 + 五谐波联合最小二乘”：

1. 对正式帧去直流、加Hann窗并做第二次4096点Q15 FFT，在粗测 `f0` 附近找到峰值格点 `k`，再用 `k-1、k、k+1` 的对数幅值做三点抛物线插值。
2. 建立包含DC和1至5次谐波正余弦项的11参数模型。
3. 正余弦基函数与ADC数据使用Q15，逐点构建正规方程时用Q15乘法和`int64_t`累加器；累加完成后才将11×11矩阵和11维向量缩放转换为`float32`。
4. 仅在小型方程求解阶段使用软件`float32`，对11×11对称正定矩阵做Cholesky分解，通过一次前向代入和一次回代联合求解 `DC、a1、b1...a5、b5`。不显式计算矩阵逆；对角根号项非正、过小或非有限时立即标记拟合失败。
5. 将正式帧第一个ADC样点定义为`t=0`，按 `x_k(t)=Uk*cos(2*pi*k*f0*t+phi_k)` 余弦约定计算 `Uk=sqrt(ak^2+bk^2)` 和 `phi_k=atan2(-bk,ak)`。
6. 用 `U2..U5` 计算THD，用五个 `phi_k` 填充Blue-Red协议的FFT相位字段。

两次FFT统一使用以下小数格点偏移：

```text
yL = ln(|X[k-1]|)
y0 = ln(|X[k]|)
yR = ln(|X[k+1]|)
delta = 0.5 * (yL - yR) / (yL - 2*y0 + yR)
f_est = (k + delta) * Fs / N
```

`delta`正常限制在`[-0.5, 0.5]`；分母过小、三点不构成局部峰值或插值越界时，标记估频无效而不输出伪结果。

第二次FFT插值后再做一次最小残差频率微调。令 `df = Fs_formal/N_formal`，初始搜索半径
`step = df/8`，分别在 `f_est-step、f_est、f_est+step` 构建11参数联合拟合并计算残差平方和
`SSE-、SSE0、SSE+`。对三个“频率-残差”点做抛物线极小值插值，将频率修正量限制在
`[-step, step]`，再在修正后频率上做一次最终Cholesky联合拟合。若三点不构成开口向上的局部极小值，则退回三者中SSE最小的频率，不继续外推。

输入基频通常不与FFT栅格严格对齐，因此不得直接使用峰值栅格的幅值和相位作为最终结果；
特别是相位必须来自精化 `f0` 后的复数拟合，否则频率偏差会转化为明显相位误差。

两个阶段复用同一4096点ADC缓存（8 KiB），并共用另一块4096点FFT工作区（8 KiB）：
第一帧处理完并保存少量候选参数后，第二帧覆盖ADC缓存，任何时刻都不同时保存两帧。
FFT只覆盖FFT工作区，因此正式帧时域样点能继续用于联合最小二乘。阶段A只
负责找到基频，不输出最终幅值和相位；最终THD、五个相对幅值和五个相位全部来自阶段B。

对去直流后的样点 `x[n]`，在每个目标频率 `k*f0` 计算复相量：

```text
Ak = sum(x[n] * cos(2*pi*k*f0*n/Fs))
Bk = sum(x[n] * sin(2*pi*k*f0*n/Fs))
Uk = scale * sqrt(Ak^2 + Bk^2)
```

非相干采样使用 Hann 窗并补偿相干增益；相干采样优先使用矩形窗：

```text
normalized[k] = Uk / U1, k = 1..5
THD = sqrt(U2^2 + U3^2 + U4^2 + U5^2) / U1 * 100%
```

所有谐波使用同一套幅值标定和窗增益补偿，不把噪声底或 6 次以上分量计入题目 THD。

最终联合拟合后计算归一化拟合残差：

```text
fit_residual_ratio = RMS(adc_ac - fitted_1_to_5_harmonics) / RMS(adc_ac)
```

- `fit_residual_ratio < 3%`：测量有效。
- `3% <= fit_residual_ratio < 5%`：测量结果可发布，同时设置拟合质量警告。
- `fit_residual_ratio >= 5%`：认为噪声、高次分量或估频异常，不发布有效THD。

该残差是模型拟合质量指标，不等于与标准仪器比较得到的THD测量误差。

第一次FFT还需计算基波峰相对频谱噪声底的SNR。噪声底使用排除DC、基波主瓣和2至5次谐波主瓣后剩余频点的稳健中值估计：

- `SNR >= 20 dB`：正常。
- `12 dB <= SNR < 20 dB`：继续测量，设置低SNR警告。
- `SNR < 12 dB`：认为无有效输入，不输出THD。

第一次粗测频率与第二次正式估计频率必须满足：

```text
abs(f_formal - f_coarse) <= max(1.25e6/4096, 0.05*f_formal)
```

即容差下限为粗测FFT的一个频点间隔约305.2 Hz，高频时允许5%相对偏差。超限表示粗测可能认错基频或输入在两帧之间发生变化；按两次FFT固定方案直接返回估频不一致错误，不自动增加第三次FFT。

### 9.8 一个周期波形

从正式帧中选择远离帧边界的上升过零点，以精估计周期做分数索引线性插值，重采样为
固定 200 点。若不足一个完整周期或相邻周期相关性过低，则不发布波形并返回质量标志。

### 9.9 板间与算法接口

- `Black <-> Blue`：保留第二轮现有有线 BoardComm，Black 只发送按键事件。
- `Blue <-> Red`：新增双向蓝牙串口链路，传启动、状态、THD、基频、五个相对幅值、
  FFT得到的五个分量相位、预留基波绝对幅值、质量标志和LCD波形。

顶层数据口径固定为：

```text
相对幅值 = [1, U2/U1, U3/U1, U4/U1, U5/U1]
基波绝对幅值 = 独立预留字段 fundamental_uvrms
```

相对幅值是当前必须传输的数据；基波绝对幅值在未完成标定时允许无效，但字段位置永久保留，
后续启用绝对幅值时不升级帧结构。FPGA重建使用五个相对幅值；Blue根据五个FFT相位
换算出与采样帧起点无关的重建相位。

Red 收到 Blue 的启动命令后自主测量，不接受 Blue 提供的基频、采样率或谐波幅值。

算法层输入：

```c
typedef struct {
    const uint16_t *samples;
    uint32_t sample_count;
    uint32_t sample_rate_hz;
    float adc_lsb_volts;
    float analog_gain;
} RedSampleFrame;
```

算法层输出：

```c
typedef struct {
    float fundamental_hz;
    float amplitude_vrms[5];
    float fft_phase_rad[5];
    float normalized[5];
    float thd_percent;
    uint32_t quality_flags;
} RedMeasurementResult;
```

接口不包含 TI 驱动对象，确保同一算法可在 PC 测试和最终选定的 TI MCU 上运行。

### 9.10 误差预算

| 来源 | THD 误差目标 |
| --- | ---: |
| 模拟前端自身失真 | 0.2 个百分点 |
| ADC 非线性与时钟抖动 | 0.4 个百分点 |
| 基频估计与谱泄漏 | 0.7 个百分点 |
| 增益频响与窗函数补偿 | 0.5 个百分点 |
| 噪声、量化与重复性 | 0.4 个百分点 |
| 设计 RSS 目标 | 小于 1.1 个百分点 |

该预算为设计目标，最终以 1 至 100 kHz、30 至 600 mVpp 的标定数据收敛。

### 9.11 校准与测试

- 直流零点：输入短接时记录单ADC测量链路的偏置和噪声。
- 幅频响应：用低失真正弦源标定 1、2、5、10、20、50、100、200、500 kHz。
- 增益交叉验证：若前端有两档增益，在重叠幅值区分别采完整帧并比较基波与 THD；同一帧不切换。
- 算法回归：合成随机相位的 1 至 5 次谐波，加入直流、噪声、频偏和采样时钟误差。
- 整机验收：频率、幅值、THD 三维组合抽点，记录误差、耗时和 10 次重复性。

### 9.12 当前待冻结事项

1. 结合 LAUNCHXL-F28379D 原理图确认正式 ADC 输入引脚、模拟前端路径和外部接线。
2. 蓝牙 UART 已冻结为 Blue UART4 PA0/PA1、Red SCI-B GPIO18/GPIO19；Blue 的 STATE 输入冻结为 PC13。
3. 最终蓝牙透传模块型号、波特率上限和主从配置命令。
4. 模拟前端运放、电源与增益精确值，需结合现有器件库存和仿真确认。

## 10. Blue-Red 蓝牙通信协议 V1

### 10.1 物理层

Blue 和 Red 各使用一个带 UART 透传功能的蓝牙模块：

| 项目 | 约定 |
| --- | --- |
| MCU 接口 | 3.3 V TTL UART |
| 串口参数 | 115200 bps，8N1，无硬件流控 |
| 蓝牙角色 | 两模块一主一从；主机位于 Blue 或 Red 均可 |
| 配对方式 | 固定地址绑定，上电自动重连 |
| Blue UART | UART4：PA0 TX、PA1 RX |
| Red UART | SCI-B：GPIO18 TX、GPIO19 RX |
| Blue STATE | PC13，下拉输入；高电平=物理连接，低电平=未连接 |
| Blue 接收方式 | UART4 RX 使用 DMA1 Stream3 循环接收；主循环只消费 DMA 缓冲区，不直接轮询 UART RDR |
| Red 接收方式 | F28379D DMA不提供SCI RX触发源；SCIB使用RX FIFO中断（阈值1字节）搬运到256项软件环形缓冲区，主循环只消费环形缓冲区 |
| 推荐附加引脚 | `EN/KEY` 配置模式输出，当前阶段不由软件控制 |

> **LAUNCHXL-F28379D 接线注意：** TI 开发板正反两面的排针丝印可能采用不同的观察方向或编号顺序，容易把同一排针的位置认反。接线不得只按目测的左右位置判断，必须同时核对连接器编号、针脚号和 GPIO 功能。本项目 Red 端固定为 `J1-4 / GPIO18 / SCIB_TX` 与 `J1-3 / GPIO19 / SCIB_RX`；上电前应使用官方引脚表或万用表再次确认。此次调试曾因正反面丝印理解不一致导致 UART 接错，出现发送端波形正常但 Red 接收 FIFO 始终为空的现象。

若最终模块只支持较低波特率，可降为 57600 bps；应用层帧格式不变。蓝牙断开期间 Red
仍保留最近一次完整结果，重连并完成 `HELLO` 后可按 Blue 请求重新发送。

### 10.2 节点与字节序

```text
NODE_BLACK = 0x01
NODE_BLUE  = 0x02
NODE_RED   = 0x03
BROADCAST  = 0xFF
```

所有 16/32 位整数使用小端序。线上不传 C 语言 `float`，统一传整数物理量，避免 STM32 与
TI 编译器的浮点格式、对齐和结构体填充差异。

### 10.3 帧格式

沿用第二轮 BoardComm V2 的帧结构和 CRC 算法，Blue 可以复用现有解析状态机：

```text
D3 91 VER DST SRC CMD FLAGS SEQ_L SEQ_H LEN_L LEN_H PAYLOAD CRC_L CRC_H 91 D3
```

| 字段 | 长度 | 说明 |
| --- | ---: | --- |
| `D3 91` | 2 | 帧头 |
| `VER` | 1 | 协议版本，固定 `0x01` |
| `DST/SRC` | 2 | 目标和源节点 |
| `CMD` | 1 | 命令字 |
| `FLAGS` | 1 | 应答、响应、事件和错误标志 |
| `SEQ` | 2 | 测量事务号，Blue 每次启动递增 |
| `LEN` | 2 | Payload 长度，0 至 128 |
| `PAYLOAD` | N | 命令载荷 |
| `CRC16` | 2 | CRC-16/CCITT-FALSE，小端发送 |
| `91 D3` | 2 | 帧尾 |

CRC 覆盖从 `VER` 到 Payload 最后一个字节：初值 `0xFFFF`，多项式 `0x1021`，不反射，
结果不异或。帧接收器按长度收帧，帧头字节出现在 Payload 中不需要转义。

标志位：

```text
bit0 ACK_REQ   需要对端 ACK
bit1 RESPONSE  当前帧是请求的响应
bit2 EVENT     主动状态/数据事件
bit3 ERROR     错误响应
bit4 RETRY     当前帧是重发
bit5..7        保留，发送时为 0
```

### 10.4 命令表

| CMD | 名称 | 方向 | 是否应答 | 用途 |
| ---: | --- | --- | --- | --- |
| `0x02` | `PING` | 双向 | 是 | 链路检测 |
| `0x50` | `HELLO` | 双向 | 是 | 版本和能力握手 |
| `0x60` | `MEASURE_START` | Blue→Red | 是 | 启动一次全自动测量 |
| `0x61` | `MEASURE_STATUS` | Red→Blue | 否 | 上报测量阶段和进度 |
| `0x62` | `MEASURE_RESULT` | Red→Blue | 是 | THD、基频和总体结果 |
| `0x63` | `HARMONIC_RESULT` | Red→Blue | 是 | U1 至 U5 相对幅值、FFT相位及预留基波幅值 |
| `0x64` | `WAVE_BEGIN` | Red→Blue | 是 | 声明波形点数与格式 |
| `0x65` | `WAVE_CHUNK` | Red→Blue | 是 | 分片发送一个周期波形 |
| `0x66` | `MEASURE_DONE` | Red→Blue | 是 | 当前事务完整结束 |
| `0x67` | `RESULT_REQUEST` | Blue→Red | 是 | 重发最近一次完整结果 |
| `0x68` | `RECON_STATUS` | Blue→Red | 否 | 回报 FPGA 波形重建提交结果 |
| `0x7E` | `ERROR` | 双向 | 否 | 协议或测量错误 |
| `0x7F` | `ACK` | 双向 | 否 | 通用应答 |

### 10.5 握手和启动载荷

`HELLO 0x50` Payload，8 字节：

| 偏移 | 类型 | 字段 |
| ---: | --- | --- |
| 0 | `u8` | `role`：2=Blue，3=Red |
| 1 | `u8` | `protocol_version`：1 |
| 2 | `u16` | `max_payload`：128 |
| 4 | `u32` | `capability_bits` |

能力位：bit0=THD，bit1=谐波幅值，bit2=波形，bit3=结果重发，bit4=状态事件，
bit5=FPGA DAC重建状态。

#### 10.5.1 STATE 边沿触发的双端校验

> 正式 STATE 触发模式已恢复（2026-07-20）：`BT_CONTINUOUS_TEST_MODE=0`，Blue 不再每
> 500 ms 连续发送 `HELLO`，检验帧只由 PC13/STATE 的稳定低到高转换触发。

1. Blue 对 PC13 做 30 ms 去抖。稳定低电平时，链路状态立即清零，LCD 以红色显示 `NOT CONNECTED`。
2. 只有稳定的低到高转换触发一次握手：完成 30 ms 去抖后先等待 200 ms 透明链路稳定时间，再次确认 STATE 仍为高，然后发送一次 `HELLO`；STATE 持续为高时不得周期发送或重复触发，等待期间变低则取消本次发送。
3. Blue 发送协议 V1 的 `HELLO`，序号递增，载荷固定为 `02 01 80 00 00 00 00 00`。
4. Red 校验帧头帧尾、版本、源/目标节点、HELLO 载荷和 CRC，并返回同序号 `ACK`。
5. 此处 `ACK.detail` 定义为校验位图：bit0=帧头尾，bit1=版本，bit2=节点，bit3=HELLO 载荷，bit4=CRC；完全正确为 `0x001F`。
6. Red 仅在五项均正确时置本端链路校验成功，并令 `ACK.status=0`；载荷不符时返回 `BAD_PAYLOAD(4)` 和实际位图。
7. Blue 仅在 ACK 本身帧格式、CRC、源/目标、序号均正确，且 `acked_cmd=HELLO`、`status=0`、`detail=0x001F` 时接受连接，LCD 以绿色显示 `CONNECTED`。
8. 等待应答超时为 1000 ms。超时或任一校验失败均保持红色未连接；只有 STATE 再次先回到低电平、随后升高，才发起下一轮校验。

`MEASURE_START 0x60` Payload，8 字节：

| 偏移 | 类型 | 字段 |
| ---: | --- | --- |
| 0 | `u32` | `request_token`，由 Blue 生成，用于去重 |
| 4 | `u16` | `requested_features`：bit0 THD、bit1 谐波、bit2 波形 |
| 6 | `u16` | `deadline_ms`，固定 10000 |

Blue 不发送基频、采样率、量程或谐波幅值。Red 收到重复的 `SEQ + request_token` 时不得启动
第二次测量，只重发 ACK 或当前状态。

`RECON_STATUS 0x68` Payload，8字节：

| 偏移 | 类型 | 字段 |
| ---: | --- | --- |
| 0 | `u16` | `transaction_id`，等于测量SEQ |
| 2 | `u8` | `status`：0=成功，1=FPGA忙，2=配置拒绝，3=ACK超时，4=链路错误 |
| 3 | `u8` | `output_channel` |
| 4 | `u32` | `detail`，成功时回报实际基波FTW |

### 10.6 状态与结果载荷

`MEASURE_STATUS 0x61` Payload，8 字节：

| 偏移 | 类型 | 字段 |
| ---: | --- | --- |
| 0 | `u8` | `state`：1 探测、2 估频、3 采集、4 分析、5 校验、6 发送 |
| 1 | `u8` | `progress_percent`：0 至 100 |
| 2 | `u16` | `elapsed_ms` |
| 4 | `u32` | `quality_flags`，测量完成前可为 0 |

`MEASURE_RESULT 0x62` Payload，24 字节：

| 偏移 | 类型 | 单位 | 字段 |
| ---: | --- | --- | --- |
| 0 | `u32` | mHz | `fundamental_millihz` |
| 4 | `u32` | ppm | `thd_ppm`，1%=10000 ppm |
| 8 | `u32` | uV | `input_vpp_uv` |
| 12 | `u32` | Hz | `sample_rate_hz` |
| 16 | `u16` | 点 | `sample_count` |
| 18 | `u8` | - | `gain_code`：前端固定/可选增益编号 |
| 19 | `u8` | - | `result_version`：1 |
| 20 | `u32` | - | `quality_flags` |

`HARMONIC_RESULT 0x63` Payload，48 字节：

```text
fundamental_uvrms       u32      基波绝对幅值预留通道，单位uVrms
relative_amplitude_ppm  u32[5]   Uk/U1，U1固定为1000000 ppm
fft_phase_mdeg          i32[5]   FFT得到的基波至5次谐波相位
harmonic_flags          u32      有效性和版本标志
```

字段偏移：

| 偏移 | 类型 | 字段 |
| ---: | --- | --- |
| 0 | `u32` | `fundamental_uvrms` |
| 4 | `u32[5]` | `relative_amplitude_ppm[0..4]`，依次对应基波至5次谐波 |
| 24 | `i32[5]` | `fft_phase_mdeg[0..4]`，依次对应基波至5次谐波，范围 `-180000..180000` |
| 44 | `u32` | `harmonic_flags` |

相对幅值定义：

```text
relative_amplitude_ppm[0] = 1000000
relative_amplitude_ppm[1] = U2/U1 * 1000000
relative_amplitude_ppm[2] = U3/U1 * 1000000
relative_amplitude_ppm[3] = U4/U1 * 1000000
relative_amplitude_ppm[4] = U5/U1 * 1000000
```

`fundamental_uvrms` 是独立预留通道：完成绝对幅值标定后发送真实基波有效值；未标定时填0，
并清除 `harmonic_flags.bit0`。Blue 和 FPGA不得因为该预留字段无效而拒绝相对幅值或THD结果。

```text
harmonic_flags.bit0  FUNDAMENTAL_AMPLITUDE_VALID
harmonic_flags.bit1  RELATIVE_AMPLITUDES_VALID
harmonic_flags.bit2  FFT_PHASES_VALID
harmonic_flags.bit3  FPGA_RECONSTRUCTION_READY
harmonic_flags.bit8..15  payload_version，首版为1
```

Red 直接发送以正式采样帧首点为`t=0`的五个原始拟合相位，不在Red-Blue协议中预先归零。Blue向FPGA
生成重建参数时，再消除采样帧起点带来的公共时间平移：

```text
reconstruction_phase_mdeg[0] = 0
reconstruction_phase_mdeg[k-1]
    = wrap(fft_phase_mdeg[k-1] - k*fft_phase_mdeg[0]), k = 2..5
```

质量标志：

```text
bit0 VALID
bit1 INPUT_TOO_SMALL
bit2 CLIPPED
bit3 FUNDAMENTAL_OUT_OF_RANGE
bit4 LOW_SNR
bit5 FREQUENCY_UNSTABLE
bit6 FRONTEND_UNCALIBRATED
bit7 WAVEFORM_INVALID
bit8 BLUETOOTH_RECONNECTED
bit9 RESULT_PARTIAL
```

### 10.7 波形分片

一个周期固定重采样为 200 点，每点为有符号 `Q15` 归一化值，`-32767..32767` 对应本周期
最小值到最大值。波形只用于显示，不参与 Blue 端重新计算 THD。

`WAVE_BEGIN 0x64` Payload，12 字节：

| 偏移 | 类型 | 字段 |
| ---: | --- | --- |
| 0 | `u16` | `total_points`：200 |
| 2 | `u16` | `sample_format`：1=有符号 Q15 |
| 4 | `i32` | `minimum_uv` |
| 8 | `i32` | `maximum_uv` |

`WAVE_CHUNK 0x65` Payload：

```text
offset_points  u16
point_count    u8       最大 60
reserved       u8       固定 0
samples        i16[point_count]
```

最大分片载荷为 124 字节，符合 128 字节上限。200 点通常拆成 60+60+60+20 四帧。Blue 按
`SEQ + offset_points` 去重和拼装；缺片时不显示旧波形，并用 `RESULT_REQUEST` 请求整组重发。

### 10.8 ACK、超时与重发

`ACK 0x7F` Payload，4 字节：

| 偏移 | 类型 | 字段 |
| ---: | --- | --- |
| 0 | `u8` | `acked_cmd` |
| 1 | `u8` | `status`：0=OK，1=BUSY，2=REJECTED，3=BAD_STATE，4=BAD_PAYLOAD |
| 2 | `u16` | `detail`，无附加信息时为 0 |

- 所有带 `ACK_REQ` 的帧必须在 200 ms 内收到相同 `SEQ` 的 ACK。
- 未收到 ACK 时重发，设置 `RETRY`；最多重发 3 次，之后进入链路错误状态。
- Blue 发送 `MEASURE_START` 后，Red 每 250 ms 至 500 ms 发送一次状态事件。
- Red 必须在 8 秒内给出结果或 `ERROR`，为 Blue 渲染和协议重试预留 2 秒。
- Blue 只有收到 RESULT、HARMONIC、全部 WAVE_CHUNK 和 DONE 后才把事务标为完整。
- 相同 `SEQ + CMD + 分片偏移` 的重复数据只 ACK，不重复提交给界面。

### 10.9 错误帧

`ERROR 0x7E` Payload，8 字节：

| 偏移 | 类型 | 字段 |
| ---: | --- | --- |
| 0 | `u16` | `error_code` |
| 2 | `u8` | `failed_state` |
| 3 | `u8` | `failed_cmd` |
| 4 | `u32` | `detail` |

首版错误码：

```text
0x0001 NO_SIGNAL
0x0002 INPUT_TOO_SMALL
0x0003 INPUT_CLIPPED
0x0004 FREQUENCY_OUT_OF_RANGE
0x0005 ADC_CAPTURE_FAILED
0x0006 ANALYSIS_FAILED
0x0007 BUSY
0x0101 PROTOCOL_VERSION_MISMATCH
0x0102 CRC_ERROR_LIMIT
0x0103 BLUETOOTH_DISCONNECTED
0x0104 RESULT_CACHE_EMPTY
```

### 10.10 完整时序

```text
Black              Blue                    Red
  | KEY_START         |                       |
  |------------------>|                       |
  |                    | MEASURE_START         |
  |                    |---------------------->|
  |                    | ACK                   |
  |                    |<----------------------|
  |                    | MEASURE_STATUS ...    |
  |                    |<----------------------|
  |                    | MEASURE_RESULT        |
  |                    |<----------------------|
  |                    | ACK ----------------->|
  |                    | HARMONIC_RESULT       |
  |                    |<----------------------|
  |                    | WAVE_BEGIN/CHUNK ...  |
  |                    |<----------------------|
  |                    | MEASURE_DONE          |
  |                    |<----------------------|
  |                    | ACK ----------------->|
  |                    | 更新 LCD              |
```

Blue 在收到 Black 按键后立即显示“正在测量”；Red 返回状态时刷新进度；完整事务结束后一次性
切换到最终结果页面，避免半帧数据造成 THD、谐波和波形来自不同测量事务。

## 11. Red测量结果到FPGA的DAC波形重建

### 11.1 功能边界

Red 负责测量，FPGA 负责重建：

```text
Red：ADC样点 -> f0、U1..U5、五个FFT相位 -> 蓝牙
Blue：接收结果 -> 显示 -> 相位归一化/FPGA参数换算 -> 现有UART
FPGA：五谐波DDS合成 -> 14位DAC码 -> AD9744 -> 示波器
```

FPGA 不接收原始 ADC 帧，也不重新计算 THD。Blue 只进行单位和 DAC 码换算，不修改 Red
测得的频率、谐波比例或FFT相位信息。重建使用题目定义的 1 至 5 次谐波近似波形：

```text
y(t) = A1*cos(w*t)
     + A2*cos(2*w*t + phase2)
     + A3*cos(3*w*t + phase3)
     + A4*cos(4*w*t + phase4)
     + A5*cos(5*w*t + phase5)
```

### 11.2 为什么使用谐波参数重建

- 题目只要求分析到 5 次谐波，五谐波模型与 THD 计算对象完全一致。
- 不需要通过 115200 UART 发送 4096 点原始 ADC 数据。
- FPGA 用相位累加器连续输出，低频时不会受有限波形点数限制。
- 示波器显示的是 Red 测得谐波成分的重建结果，而不是 FPGA 重新测量的结果。

Blue 上的 200 点波形只用于 LCD 显示；FPGA 重建不使用这 200 点 Q15 数据。

### 11.3 Blue到FPGA五谐波重建专用协议

物理层继续使用现有 Blue-FPGA UART：115200 bps、8N1、3.3 V TTL、共地。帧继续使用
FPGA V2 格式：

```text
D3 91 VER DST SRC CMD FLAGS SEQ LEN PAYLOAD CRC16 91 D3
VER=0x02, SRC=BLUE(0x02), DST=FPGA(0x10)
```

第三轮新增两个专用命令，不使用第二轮通用多波形命令的“最多4个分量”数据结构。
第二轮 `FPGA_CHANNEL_STAGE 0x20` 和 `FPGA_COMMIT 0x21` 仍保留给原有双路信号源功能，
五谐波重建模式使用独立的影子寄存器和输出MUX：

| CMD | 名称 | 说明 |
| ---: | --- | --- |
| `0x30` | `THD_RECON_STAGE` | 暂存一次五谐波重建配置，不改变当前DAC输出 |
| `0x31` | `THD_RECON_COMMIT` | 原子提交配置并从统一零相位启动输出 |

`THD_RECON_STAGE 0x30` Payload 固定40字节，所有多字节整数均为小端序：

| 偏移 | 类型 | 字段 |
| ---: | --- | --- |
| 0 | `u16` | `transaction_id`，等于当前测量SEQ |
| 2 | `u32` | `fundamental_ftw`，基波32位频率控制字 |
| 6 | `i16` | `dc_offset_code`，首版固定0 |
| 8 | `u8` | `harmonic_count`，固定5 |
| 9 | `u8` | `output_channel`，首版固定0=DAC1 |
| 10 | `u16` | `amplitude_code[0]`，基波峰值码 |
| 12 | `u32` | `phase_word[0]`，基波相位固定为0 |
| 16 | `u16` | `amplitude_code[1]`，二次谐波峰值码 |
| 18 | `u32` | `phase_word[1]`，二次谐波相对相位 |
| 22 | `u16` | `amplitude_code[2]`，三次谐波峰值码 |
| 24 | `u32` | `phase_word[2]`，三次谐波相对相位 |
| 28 | `u16` | `amplitude_code[3]`，四次谐波峰值码 |
| 30 | `u32` | `phase_word[3]`，四次谐波相对相位 |
| 34 | `u16` | `amplitude_code[4]`，五次谐波峰值码 |
| 36 | `u32` | `phase_word[4]`，五次谐波相对相位 |

每个谐波条目6字节；条目不再传输波形类型、单独频率或占空比。FPGA将五个条目
固定解释为正弦基波、二次、三次、四次和五次谐波，各分量频率由同一个
`fundamental_ftw` 内部乘以1至5得到。第1次谐波的 `phase_word` 固定0。Blue 按100 MHz
DAC采样时钟换算：

```text
fundamental_ftw = round(fundamental_millihz * 2^32 / (100000000 * 1000))
reconstruction_phase_mdeg[0] = 0
reconstruction_phase_mdeg[k-1]
    = wrap(fft_phase_mdeg[k-1] - k*fft_phase_mdeg[0]), k = 2..5
phase_word[k-1] = round(reconstruction_phase_mdeg[k-1] * 2^32 / 360000), k = 1..5
```

`THD_RECON_COMMIT 0x31` Payload 固定4字节：

| 偏移 | 类型 | 字段 |
| ---: | --- | --- |
| 0 | `u16` | `transaction_id` |
| 2 | `u16` | `commit_flags`：bit0=清相位，bit1=启用DAC输出 |

FPGA 对两个命令均返回现有 `ACK 0x7F`，ACK必须回显请求SEQ、事务号和状态。只有STAGE完整、
CRC正确且所有字段合法时，COMMIT才允许切换输出；失败时保持上一条重建波形不变。

字段合法性检查固定为：

- Payload长度必须等于40，`harmonic_count` 必须等于5，`output_channel` 首版必须等于0。
- `fundamental_ftw` 必须非0；`amplitude_code[0]` 必须非0，五个幅度码均不得大于8191。
- `phase_word[0]` 必须等于0；其余4个32位相位字允许取任意值，按模2^32解释。
- `sum(amplitude_code[0..4]) + abs(dc_offset_code)` 必须不大于6552，否则返回越界NACK。

Blue只在收到STAGE成功ACK后发送COMMIT。FPGA只接受与最近一次有效STAGE相同
`transaction_id` 的COMMIT。重复收到同一 `SEQ + transaction_id` 时返回之前的ACK，
不重复清零相位或触发短暂断波。

### 11.4 幅值缩放与防饱和

Red 提供 `relative_amplitude_ppm[k] = Uk/U1`。Blue 根据五个比例自动选择安全的基波DAC幅值：

```text
ratio_sum = sum(relative_amplitude_ppm[k]) / 1000000
A1_code   = floor(0.8 * 8191 / ratio_sum)
Ak_code   = round(A1_code * relative_amplitude_ppm[k] / 1000000)
```

这样五个分量最坏情况同相叠加时仍只使用约80%数字满量程。Blue 发送前再次检查：

```text
sum(abs(Ak_code)) + abs(dc_offset_code) <= 6552
```

不满足时整体同比缩小，不允许逐谐波单独限幅，因为单独限幅会改变THD和波形形状。

### 11.5 FPGA重建数据通路

新增独立的 `thd_harmonic_reconstruction` 模块：

```text
100 MHz DAC时钟
      |
32位基波相位累加器
      |
      +--> 1*phase + phase1 --> sine LUT --> × A1 --+
      +--> 2*phase + phase2 --> sine LUT --> × A2 --+
      +--> 3*phase + phase3 --> sine LUT --> × A3 --+--> 宽位求和
      +--> 4*phase + phase4 --> sine LUT --> × A4 --+       |
      +--> 5*phase + phase5 --> sine LUT --> × A5 --+       v
                                                        饱和到14位
                                                            |
                                                         AD9744
```

- 五个分量共享一个基波相位累加器，保证严格整数倍频和长期相位关系稳定。
- 正弦表使用至少4096点、14位有符号查找表，五个谐波共享同一ROM；内部乘法和累加保留保护位。
- 幅值乘法使用至少28位乘积，右移缩放前加半LSB做四舍五入，不允许简单截断。
- 五路先用宽位有符号累加器求和，只在最终DAC出口做一次14位饱和，不逐谐波限幅。
- STAGE写影子寄存器；COMMIT在同一DAC时钟边沿原子切换全部参数并清零流水线。
- 重建模式与第二轮实时DDS/周期缓存模式互斥，通过顶层输出MUX选择，不删除旧功能。
- 最终结果在进入AD9744前做14位饱和保护；正常配置不应触发饱和。

### 11.6 完整时序

```text
Red                 Blue                         FPGA/DAC
 | RESULT+HARMONIC    |                              |
 |------------------->|                              |
 |                     | LCD显示                      |
 |                     | THD_RECON_STAGE             |
 |                     |----------------------------->|
 |                     |<------------------------- ACK|
 |                     | THD_RECON_COMMIT            |
 |                     |----------------------------->|
 |                     |<------------------------- ACK|
 |                     |                              | 原子启动五谐波输出
 |                     | RECON_STATUS                |--------> 示波器
 |<--------------------|                              |
```

Blue 只有在同一测量SEQ的 `MEASURE_RESULT` 和 `HARMONIC_RESULT` 都校验成功后才配置FPGA。
FPGA COMMIT成功后，Blue向Red发送 `RECON_STATUS`，并在LCD上显示“DAC重建已启动”。
FPGA重建失败只产生重建告警，不得把已经有效的Red THD测量结果改为无效。

### 11.7 验收标准

1. DAC输出基频相对Red测量频率误差由32位FTW量化决定，目标小于0.01%。
2. 示波器FFT测得的2至5次谐波相对基波比例与Red结果一致，首版目标误差不超过2%。
3. 示波器按题目公式计算的重建THD与Red测量THD目标相差不超过1个百分点。
4. 新结果COMMIT时允许波形重新起相，但不得出现超过DAC安全范围的异常尖峰。
5. UART帧、CRC或事务错误时继续输出上一条有效波形，不得输出半更新参数。

### 11.8 数字精度预算

Blue-Red通信不会产生连续的模拟精度损失。所有量采用整数定标，CRC错误的帧会被丢弃并重传；
只要一帧通过CRC，其数值就与Red发送值逐位相同。各级量化如下：

| 环节 | 数值格式 | 分辨率/影响 |
| --- | --- | --- |
| 五个相对幅值 | `u32 ppm` | `1e-6`，即相对基波0.0001% |
| 基波绝对幅值 | `u32 uVrms` | 1 uV；实际精度由ADC和标定决定 |
| 五个FFT相位 | `i32 mdeg` | 0.001° |
| FPGA频率控制字 | 32位FTW、100 MHz时钟 | 约0.0233 Hz/LSB |
| FPGA相位控制字 | 32位相位字 | 约`8.38e-8°/LSB` |
| FPGA正弦ROM | 4096点 | 地址步进0.0879°，无插值最坏瞬时幅值误差约0.153%满幅 |
| AD9744数字输入 | 有符号14位 | 满量程约0.0122%/LSB |

在80%数字满量程下，基波通常约有数千个DAC码。若某次谐波为基波的5%，其幅值约为数百码，
四舍五入带来的该谐波相对误差约0.15%，折算到总THD远小于题目3个百分点限值。

Blue换算FTW、相位字和幅值码时必须使用64位中间变量和四舍五入除法，禁止先转换为低位整数：

```text
Ak_code = round_u64(A1_code * relative_amplitude_ppm[k], 1000000)
FTW     = round_u64(fundamental_millihz * 2^32, DAC_CLK_HZ * 1000)
```

数字链路的目标附加THD误差不超过0.2个百分点。最终重建误差主要来自AD9744的INL/DNL、
输出运放失真、重建滤波器的幅频/相频响应和电源噪声，因此必须对DAC模拟链路在1 kHz至
500 kHz范围内做增益与相位标定。通信定标精度不是整机的主要瓶颈。

## 12. 当前软件实现状态（2026-07-20）

- Black 默认用 `C` 键发起失真度测量，专用板间命令为 `0x33`；原键盘事件仍保留。
- 本轮只完成第一项：Blue-Red 蓝牙物理连接与协议互验，不声明第二轮测量、结果显示或 FPGA 重建功能已迁移。
- Blue 已移除第二轮旧屏幕任务的运行入口；旧源文件暂时保留但不再执行，避免直接删除导致后续迁移信息丢失。
- Blue 已实现 UART4（PA0 TX、PA1 RX，115200 8N1）、DMA1 Stream3 循环接收和 PC13 STATE 接口。当前已恢复正式模式：PC13/STATE 稳定低到高时单次发送 HELLO，只有正确接收并校验 Red ACK 后才显示绿色 `CONNECTED`；STATE 持续为高时不重复发送。
- Red 已在 LAUNCHXL-F28379D CPU1 Driverlib 工程实现 SCI-B（GPIO18 TX、GPIO19 RX，115200 8N1）RX FIFO中断加256项环形缓冲区接收、HELLO 校验和带校验位图的 ACK 回复。
- Blue 原 USART1 仍归 `BoardComm_User` 使用，USART2 仍归 `FpgaUart_User` 使用；新模块不注册它们的中断、DMA 或回调，未覆盖旧串口所有权。

# LCR 校准流程 README

适用项目：全国大学生电子设计竞赛“信号源及 LCR 测量”  
适用固件：`DRIVER FIXED` 驱动板模式  
当前参考参数：

- `RREF = 470 Ω`
- `RBIAS = 1 kΩ`（永久并联在 DUT 两端，固件自动去嵌）
- `LOAD = 470 Ω`
- `ADC_RIN = 1 MΩ`
- 校准频点：100 Hz、200 Hz、500 Hz、1 kHz、2 kHz、5 kHz、10 kHz、20 kHz
- 校准顺序：`ZERO → SHORT → LOAD → OPEN → VERIFY → EXPORT`

---

## 1. 校准前准备

### 1.1 硬件连接

正式 LCR 测量链路应保持为：

```text
驱动板输出
   │
   ├── V1 采样点
   │
  DUT ∥ RBIAS 1 kΩ
   │
   ├── V2 采样点
   │
 RREF = 470 Ω
   │
  GND
```

注意：

1. `V1` 应位于驱动板及输出保护/隔离网络之后，靠近 DUT 输入端。
2. `V2` 位于 DUT 与 `RREF` 之间。
3. `RREF` 在 SHORT、LOAD、OPEN、VERIFY 过程中始终保留，不允许拆除。
4. `RBIAS = 1 kΩ` 永久焊在 `V1`—`V2` 之间，所有标准件和 DUT 都与它并联，标准和测量时都不拆。
5. 固件按 `Y_DUT = Y_测得 - 1/1000` 统一去嵌；更改 RBIAS 阻值后必须同步修改固件并重新校准。
6. DUT 短路是指 `V1` 与 `V2` 短接，不是将驱动板输出直接短路到地。
7. 驱动板、DAC、STM32、FPGA、AD7606 和电源必须共地。

### 1.2 上电检查

校准前确认：

- 驱动板正负电源正常；
- 输出没有明显直流偏置；
- LCR 激励为零中心交流信号；
- V1、V2 未接反；
- 示波器输入使用 1 MΩ，不要使用 50 Ω终端；
- RREF 和 LOAD 的实际阻值已确认；
- 进入校准页面后，中途不要退出，必须连续做到 VERIFY。

---

## 2. 校准页面状态说明

页面主要状态：

```text
Last=上一次操作结果
STAGE=当前暂存校准有效位
ACTIVE seq=已提交正式校准序号
```

常见结果：

| 显示 | 含义 |
|---|---|
| `OK` | 当前步骤成功 |
| `ARG` | 参数或前置步骤不满足 |
| `ADC` | ADC 采集失败 |
| `SIGNAL` | 激励输出失败 |
| `MATH` | 计算失败 |
| `VERIFY` | 验证误差超限 |
| `QUALITY` | 信号质量、残差或稳定性不合格 |

`STAGE` 典型变化：

| 已完成步骤 | STAGE |
|---|---:|
| 无 | `0x00` |
| ZERO | `0x03` |
| ZERO + SHORT | `0x07` |
| ZERO + SHORT + LOAD | `0x0F` |
| ZERO + SHORT + LOAD + OPEN | `0x1F` |
| VERIFY 成功并提交 | `0x3F` |

如果直接点击 OPEN，而页面显示：

```text
Last=ARG
STAGE=0x00
TRY=0
DIV=0
V1=0
V2=0
```

说明 OPEN 没有真正开始采样，而是前置步骤未完成。

---

## 3. 完整校准流程

## 3.1 ZERO：零点校准

### 接线

将两路 ADC 输入都接模拟地：

```text
V1 → GND
V2 → GND
```

不要接 DUT，也不要保留交流激励到 ADC 输入。

### 操作

点击：

```text
1 ZERO
```

### 目的

测量：

- ±10 V 量程下 V1、V2 的 ADC 零点；
- ±5 V 量程下 V1、V2 的 ADC 零点；
- ADC 零点噪声水平。

### 成功标志

```text
Last=OK
STAGE=0x03
```

如果 ZERO 失败，优先检查：

- V1/V2 是否真正接地；
- ADC 是否仍有交流输入；
- FPGA ADC 是否忙或未恢复；
- SPI/CRC 是否报错；
- 接地是否可靠。

---

## 3.2 SHORT：短路校准

### 接线

恢复正式测量链路，然后短接 DUT 位置：

```text
V1 ───────── V2
               │
             470 Ω RREF
               │
              GND
```

### 操作

点击：

```text
2 SHORT
```

### 目的

测量并保存：

- 夹具短路残余电阻；
- 导线电阻；
- 接触电阻；
- 短路状态下的残余电感和相位误差。

### 成功标志

```text
Last=OK
STAGE=0x07
```

### 注意

- 不要把 RREF 拆掉。
- 不要把驱动板输出直接短路到地。
- 短接线应尽可能短，并与正式夹具位置一致。

---

## 3.3 LOAD：标准负载校准

### 接线

拆除短路线，在 DUT 位置接入标准电阻：

```text
V1
 │
 470 Ω 标准电阻
 │
V2
 │
470 Ω RREF
 │
GND
```

### 操作

确认页面顶部：

```text
LOAD = 标准电阻实测值
```

然后点击：

```text
3 LOAD
```

### 目的

测量整条链路的复数比例误差，包括：

- 驱动板增益和相位；
- V1/V2 通道增益差；
- ADC 通道相位差；
- 夹具及连接线误差；
- RREF 误差。

### 成功标志

```text
Last=OK
STAGE=0x0F
```

### 注意

- 建议输入标准电阻的实测值，不要只使用标称值。
- 标准件应尽量接近 470 Ω。
- 校准后最终验收不能只用这一个 LOAD 标准件。

---

## 3.4 OPEN：开路校准

### 接线

拆除外接 DUT 和 LOAD，不安装其他标准件；永久焊接的 1 kΩ RBIAS 仍保留在 V1 与 V2 之间：

```text
V1
 │
1 kΩ RBIAS（永久保留）
 │
V2
 │
470 Ω RREF
 │
GND
```

### 操作

点击：

```text
4 OPEN
```

### 目的

测量：

- 夹具开路电容；
- 走线、连接器和线缆寄生；
- 高阻状态下的导纳误差；
- V2 低信号噪声水平。

### 运行时正常现象

OPEN 过程中通常会看到：

```text
TRY 逐步增加
GOOD 逐步增加
DIV 非 0
FS 非 0
V1 有明显交流幅度
V2 很小，但不一定严格为 0
```

OPEN 使用多个有效导纳结果，并在 Y 平面保留最接近中心的结果进行平均。

### 成功标志

```text
Last=OK
STAGE=0x1F
```

### 如果显示 ARG

若出现：

```text
Last=ARG
STAGE=0x00 或未达到 0x0F
TRY=0
```

说明前面的 LOAD 尚未成功，OPEN 没有真正开始。

---

## 3.5 VERIFY：验证与提交

### 接线

重新接入 470 Ω 标准电阻：

```text
V1
 │
470 Ω 标准电阻
 │
V2
 │
470 Ω RREF
 │
GND
```

### 操作

点击：

```text
5 VERIFY
```

### 目的

用当前暂存校准参数重新测量标准件，并检查：

- 阻值误差；
- 相位误差；
- 拟合残差；
- 结果稳定性；
- 校准硬件模式是否为 DRIVER。

### 成功行为

VERIFY 成功后：

1. 暂存校准提交为 ACTIVE；
2. `STAGE` 变为 `0x3F`；
3. `seq` 增加；
4. 自动通过串口导出校准数据；
5. 页面显示校准已提交。

典型显示：

```text
VERIFY PASS
STAGE=0x3F
ACTIVE seq=...
```

### 失败处理

若 VERIFY 失败：

- 不会提交当前暂存数据；
- 先检查 LOAD 标准值是否输入正确；
- 检查标准件连接是否与 LOAD 步骤一致；
- 检查 V1/V2 是否接反；
- 检查驱动板是否削顶或振荡；
- 必要时从 ZERO 重新开始。

---

## 3.6 EXPORT：手动导出

VERIFY 成功后通常会自动导出，也可点击：

```text
EXPORT
```

串口输出应包含：

```text
LCR_CAL_BEGIN
...
CRC32
...
LCR_CAL_END
```

MATLAB 工具读取导出内容后，可生成：

```text
lcr_calibration_generated.c
lcr_calibration_generated.h
```

生成文件后需：

1. 替换工程内旧文件；
2. 重新编译；
3. 重新烧录；
4. 用独立标准件交叉验证。

---

## 4. 重要限制

### 4.1 中途退出会丢失未提交步骤

ZERO、SHORT、LOAD、OPEN 在 VERIFY 之前只保存在暂存区。

以下操作可能导致暂存内容丢失：

- 点击 BACK；
- 退出校准页面；
- 系统复位；
- 断电；
- 重新进入校准页面。

因此必须连续完成：

```text
ZERO → SHORT → LOAD → OPEN → VERIFY
```

### 4.2 DRIVER 与旧校准不兼容

正式固件使用：

```text
DRIVER FIXED
```

旧的 `DIRECT_DAC` 校准文件不能用于驱动板模式。

如果显示：

```text
CAL PROFILE MISMATCH
DRIVER CAL REQUIRED
```

需要重新执行完整 DRIVER 校准。

### 4.3 校准参数不等于永久保存

当前校准在运行过程中可保存在 RAM，并可通过串口导出。

要实现掉电后自动加载，必须：

- 将 MATLAB 生成的校准文件重新编译进固件；或
- 后续增加 Flash/EEPROM 持久化。

---

## 5. 校准后验收

校准完成后，至少测试：

### 电阻

- 短路；
- 100 Ω；
- 470 Ω附近标准电阻；
- 1 kΩ；
- 10 kΩ；
- 开路。

### 电容

建议覆盖：

- 10 nF；
- 100 nF；
- 1 μF；
- 10 μF。

### 电感

建议覆盖：

- 1 mH；
- 10 mH；
- 100 mH；
- 1 H。

### 频率

至少覆盖：

```text
100 Hz
1 kHz
10 kHz
20 kHz
```

不要只使用 LOAD 校准时的同一个 470 Ω电阻验证精度。

---

## 6. 故障快速判断

| 现象 | 优先判断 |
|---|---|
| `Last=ARG`，`TRY=0` | 前置步骤未完成 |
| `STAGE=0x00` | 当前暂存校准为空 |
| ADC错误 | 检查 FPGA ADC、SPI、CRC、BUSY |
| V1/V2 都为0 | 激励未启动或 ADC 未采样 |
| V1正常、V2极低 | 高阻或 OPEN 状态，可能正常 |
| V1残差很大 | 驱动板失真、削顶或振荡 |
| VERIFY阻值正确但相位异常 | V1/V2极性、通道相位或接线问题 |
| OPEN不稳定 | 夹具晃动、环境耦合、V2噪声过大 |
| 重启后校准消失 | 尚未将导出文件重新编译进固件 |

---

## 7. 最简操作清单

```text
1. 进入 GUIDED LCR CALIBRATION
2. 确认 DRIVER FIXED
3. 输入 RREF、LOAD、ADC_RIN
4. V1/V2 接地，执行 ZERO
5. DUT位置短接，执行 SHORT
6. DUT位置接470 Ω标准件，执行 LOAD
7. DUT位置开路，执行 OPEN
8. 再接470 Ω标准件，执行 VERIFY
9. 确认 VERIFY PASS、STAGE=0x3F
10. 保存串口导出数据
11. MATLAB生成校准文件
12. 替换文件、重新编译烧录
13. 使用独立 R/L/C 标准件交叉验证
```

---

## 8. 当前工程的 USB 校准方式

当前 Blue 固件已将板载 Type-C 配置为 USB CDC 虚拟串口，电脑识别名称为：

```text
Blue LCR Calibration CDC
```

该实现固定使用本文规定的拓扑：

```text
V1 — DUT — V2 — RREF — GND
```

基础公式为：

```text
Z_DUT = RREF × (V1 - V2) / V2
```

USB 虚拟串口支持以下 ASCII 命令，每条命令以回车或换行结束：

```text
HELP
STATUS
SET RREF 470.000
SET LOAD 470.000
ZERO
SHORT
LOAD
OPEN
VERIFY
EXPORT
```

校准频点严格使用本文的 8 点：100 Hz、200 Hz、500 Hz、1 kHz、2 kHz、5 kHz、10 kHz、20 kHz。OPEN 每个频点采集 7 次，在导纳平面剔除距离中心最远的 2 次后平均。VERIFY 对全部 8 点检查标准负载的复数误差，通过后提交为 ACTIVE 并自动输出 `LCR_CAL_BEGIN` 到 `LCR_CAL_END`。

可直接运行电脑端引导工具：

```powershell
pip install pyserial
python Tools/lcr_usb_calibration.py --port COM7 --rref 470.02 --load 469.98
```

其中 `--rref` 和 `--load` 应填写万用表测得的实际阻值。工具会逐步提示更换 ZERO、SHORT、LOAD、OPEN、VERIFY 接线，并把最终数据保存为 `lcr_calibration_export.txt`。

当前 Blue 使用片内双 ADC：`PC0/ADC1` 采 V1，`PC1/ADC2` 采 V2，输入必须限制在 0～3.3 V。本文原先针对 ±10 V/±5 V ADC 量程的 ZERO 描述，在本工程中对应为单一 0～3.3 V 量程的两通道直流零点和噪声检查。

校准应用范围为 100 Hz～20 kHz。AUTO 扫频中超过 20 kHz 的点不会外推使用低频校准参数，而继续使用未校准的复数测量值。

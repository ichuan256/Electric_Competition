# THS3491 Multisim 仿真说明

## 1. 模型文件

THS3491 模型目录：

`C:\Users\CaoTX\Downloads\sbomai5a`

里面关键文件：

- `THS3491DDA.cir`：PSpice/TINA 子电路模型，Multisim 应导入这个文件。
- `THS3491DDA.TSM`：TINA-TI 符号/宏模型相关文件，Multisim 通常不用它。
- `THS3491DDA.tld`：TINA-TI 库描述文件。

模型头部为：

```spice
.SUBCKT THS3491DDA INP INN OUT REF PD_not VCC VEE GND
```

Multisim 建元件时的引脚顺序必须按这个来。

## 2. Multisim 导入步骤

1. 打开 `Tools -> Component Wizard`。
2. 选择 `Create a component from a SPICE model`。
3. 选择模型文件：

   `C:\Users\CaoTX\Downloads\sbomai5a\THS3491DDA.cir`

4. 子电路选择 `THS3491DDA`。
5. 引脚映射：

| 模型引脚 | 原理图符号引脚 | 连接 |
|---|---|---|
| `INP` | `VIN+` | 正输入 |
| `INN` | `VIN-` | 负输入 |
| `OUT` | `VOUT` | 输出 |
| `REF` | `REF` | 图中接 GND |
| `PD_not` | `PD#` | 接 VCC，使能 |
| `VCC` | `+VS` | 正电源 |
| `VEE` | `-VS` | 负电源 |
| `GND` | `EP/GND` | 地 |

原理图里的 `NC` 不需要接入模型。

## 3. 建议先仿真四路 THS3491 输出级

先不要把 OPA820、二极管限幅和输入 LC 网络全部接上。建议先验证右半边四路 THS3491 并联输出能收敛：

- 供电：`VCC = +15V`，`VSS = -15V`
- `REF` 接地
- `PD#` 接 `VCC`
- 每路输出串 `2.2R`
- 四路输出汇合后接负载
- 负载先用 `50R`，确认正常后再换成图里的 `0.2R`

TI 模型备注说它主要按 `+15V/-15V` 对齐数据手册；低于这个供电时结果可能不准。

## 4. 图中四路 THS3491 增益关系

每路基本是同相放大：

- 输入串联电阻：`49.9R`
- 到 `VIN+` 前还有 `2k`
- 反馈：`2k`

单路近似闭环增益：

```text
Av ~= 1 + 2k / 2k = 2 V/V
```

四路并联不是电压增益变 4 倍，而是输出电流能力增大。每路输出串 `2.2R` 用来均流和隔离。

## 5. 分析设置

AC 仿真：

- Start：`1 kHz`
- Stop：`100 MHz`，需要看射频可拉到 `500 MHz`
- Points/decade：`100`
- 输入 AC 幅度：`1 V`

瞬态仿真：

- 先用 `100 mVpp`
- 频率先从 `100 kHz` 或 `1 MHz` 开始
- 最大时间步长小于周期的 `1/100`
- 例如 `10 MHz`，最大步长 `1 ns`

收敛建议：

- 先用 `50R` 负载跑通
- 再改 `5R`
- 最后再试 `0.2R`
- 每个电源脚加 `100nF + 10uF` 去耦


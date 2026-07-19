# Stage3 FPGA：THD 五谐波波形重建

本工程仅保留 DAC1、Blue-FPGA UART V2 和两颗调试 LED。DAC2、旧多波形协议、周期缓存、
AD9910 参考时钟以及 FPGA FFT 均不进入本顶层。

## 数据通路

- 输入时钟：板载 50 MHz，经 `clk_wiz_0` 产生 100 MHz DAC 采样时钟。
- 重建模型：基波到 5 次谐波共用一个 32 位基波相位累加器。
- 相位：各支路使用 `k*phase + phase_word[k]`；协议零相位按余弦定义。
- 查表：4096 点、14 位有符号正弦表；五个读口使用同一初始化数据。
- 幅值：29 位有符号乘积，Q13 对称四舍五入，五路宽位相加后只做一次 14 位饱和。
- DAC 编码：AD9744 的 14 位二进制补码。
- 上电默认：DAC 睡眠且输出零码；只有合法 STAGE 后的匹配 COMMIT 才改变输出。

## UART

物理层为 115200 bps、8N1、3.3 V TTL、共地。实现 `THD_RECON_STAGE(0x30)`、
`THD_RECON_COMMIT(0x31)` 和现有 8 字节载荷的 `ACK(0x7F)`。详细字段以仓库根目录
`THD-Overview.md` 第 11.3 节以及 `G:/fpga/THD-Overview-顺序采样扩展.md` 为准。

STAGE 首版固定 `dc_offset_code=0`、`harmonic_count=5`、`output_channel=0`；
基波幅度必须非零，五个幅度码分别不超过 8191 且总和不超过 6552，基波相位字固定为 0。
重复请求只有在 `SRC + CMD + SEQ + transaction_id` 全部一致时才重发缓存 ACK。

- LED1：收到任意一个完整 UART 字节后持续约 2 Hz 闪烁。
- PS_LED0：收到 CRC、版本和地址均正确的完整 V2 帧后持续约 1 Hz 闪烁。
- CRC、字段或事务错误不会修改当前 DAC 输出。

## 创建工程

先生成查找表（仓库中已提交生成结果），然后运行 Vivado Tcl：

```text
python scripts/generate_sine_lut.py
source G:/fpga/stage2/-/Gilding/stage3/rebuild_stage3.tcl
```

器件固定为 `xc7z020clg400-2`，顶层固定为 `thd_recon_top`。

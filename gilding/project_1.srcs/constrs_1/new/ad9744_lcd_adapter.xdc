# 领航者 ZYNQ 与 AD9744 DDS 工程约束。
#
# 重要硬件警告：
#   AD9744 模块 P1 排针与领航者 RGB LCD 40 针接口的电源、地位置不兼容。
#   禁止把 AD9744 模块直接插到 LCD 接口，必须使用短转接线或转接板。
#
# 已确认的转接映射：
#   FPGA 管脚按照下列 AD9744 信号顺序分配：
#   D2, D1, D4, D3, ..., D14, D13, PD/SLEEP, CLK.
#   +5V 和 GND 仅由硬件连接，不出现在 Verilog/XDC 端口中。

set_property -dict {PACKAGE_PIN U18 IOSTANDARD LVCMOS33} [get_ports sys_clk]
set_property -dict {PACKAGE_PIN N16 IOSTANDARD LVCMOS33} [get_ports sys_rst_n]

# 板载调试 LED0。
set_property -dict {PACKAGE_PIN H15 IOSTANDARD LVCMOS33} [get_ports led0]

# 单片机 UART：MCU_TX -> K14（FPGA RX），MCU_RX <- M15（FPGA TX），3.3 V 电平并共地。
set_property -dict {PACKAGE_PIN K14 IOSTANDARD LVCMOS33} [get_ports mcu_uart_rxd]
set_property -dict {PACKAGE_PIN M15 IOSTANDARD LVCMOS33} [get_ports mcu_uart_txd]
set_property PULLUP true [get_ports mcu_uart_rxd]

# AD9744 控制信号
set_property -dict {PACKAGE_PIN Y17 IOSTANDARD LVCMOS33} [get_ports dac_sleep] ;# AD9744 PD/SLEEP
set_property -dict {PACKAGE_PIN Y16 IOSTANDARD LVCMOS33} [get_ports dac_clk]   ;# AD9744 CLK

# AD9744 数据总线。模块丝印为 D14..D1，Verilog 端口为 dac_data[13:0]。
set_property -dict {PACKAGE_PIN T16 IOSTANDARD LVCMOS33} [get_ports {dac_data[13]}] ;# D14
set_property -dict {PACKAGE_PIN U17 IOSTANDARD LVCMOS33} [get_ports {dac_data[12]}] ;# D13
set_property -dict {PACKAGE_PIN V17 IOSTANDARD LVCMOS33} [get_ports {dac_data[11]}] ;# D12
set_property -dict {PACKAGE_PIN V18 IOSTANDARD LVCMOS33} [get_ports {dac_data[10]}] ;# D11
set_property -dict {PACKAGE_PIN T17 IOSTANDARD LVCMOS33} [get_ports {dac_data[9]}]  ;# D10
set_property -dict {PACKAGE_PIN R18 IOSTANDARD LVCMOS33} [get_ports {dac_data[8]}]  ;# D9
set_property -dict {PACKAGE_PIN Y18 IOSTANDARD LVCMOS33} [get_ports {dac_data[7]}]  ;# D8
set_property -dict {PACKAGE_PIN Y19 IOSTANDARD LVCMOS33} [get_ports {dac_data[6]}]  ;# D7
set_property -dict {PACKAGE_PIN P15 IOSTANDARD LVCMOS33} [get_ports {dac_data[5]}]  ;# D6
set_property -dict {PACKAGE_PIN P16 IOSTANDARD LVCMOS33} [get_ports {dac_data[4]}]  ;# D5
set_property -dict {PACKAGE_PIN W18 IOSTANDARD LVCMOS33} [get_ports {dac_data[3]}]  ;# D4
set_property -dict {PACKAGE_PIN W19 IOSTANDARD LVCMOS33} [get_ports {dac_data[2]}]  ;# D3
set_property -dict {PACKAGE_PIN R16 IOSTANDARD LVCMOS33} [get_ports {dac_data[1]}]  ;# D2
set_property -dict {PACKAGE_PIN R17 IOSTANDARD LVCMOS33} [get_ports {dac_data[0]}]  ;# D1

set_property DRIVE 8 [get_ports {dac_data[*]}]
set_property DRIVE 8 [get_ports dac_clk]
set_property SLEW FAST [get_ports {dac_data[*]}]
set_property SLEW FAST [get_ports dac_clk]

# AD9744 要求建立时间 tS >= 2.0 ns、保持时间 tH >= 1.5 ns。
# 第一路数据以180 MHz/0度更新；DAC转发时钟使用180 MHz/18度，
# 经ODDR后采样上升沿约位于数据更新后的3.06 ns，扩大建立/保持裕量。
# 从实际 ODDR 时钟输入推导外部 DAC 采样时钟，保留 Clocking Wizard 与
# 转发时钟的共同路径，避免错误的跨时钟域分析。
create_generated_clock -name dac_sample_clk \
    -source [get_pins u_dac_clk_oddr/C] -edges {2 3 4} [get_ports dac_clk]
set_output_delay -clock dac_sample_clk -max 2.0 [get_ports {dac_data[*]}]
set_output_delay -clock dac_sample_clk -min -1.5 [get_ports {dac_data[*]}]
set_false_path -from [get_ports mcu_uart_rxd]

# UART配置采用“稳定多位数据总线 + 翻转握手”跨时钟域协议。
# 只切断第二路配置接收寄存器D端的CDC路径，不依赖Clocking Wizard生成时钟名称。
set_false_path -to [get_pins -hierarchical -regexp \
    {.*u_dac2/(ftw|phase_offset|amplitude|dc_offset|duty|wave_sel|output_enable)_reg.*\/D}]

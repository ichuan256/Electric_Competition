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

if {[llength [get_clocks -quiet sys_clk]] == 0} {
    create_clock -period 20.000 -name sys_clk [get_ports sys_clk]
}

set_property -dict {PACKAGE_PIN U18 IOSTANDARD LVCMOS33} [get_ports sys_clk]
set_property -dict {PACKAGE_PIN N16 IOSTANDARD LVCMOS33} [get_ports sys_rst_n]

# 板载PS_LED0：完整UART帧成功提交后闪烁。
set_property -dict {PACKAGE_PIN H15 IOSTANDARD LVCMOS33} [get_ports led0]
# 板载LED1：收到任意UART字节后闪烁。
set_property -dict {PACKAGE_PIN L15 IOSTANDARD LVCMOS33} [get_ports led1]

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
# 数据在内部 100 MHz 时钟上升沿更新，转发的 dac_clk 在 5 ns 后产生上升沿。
# 从实际 ODDR 时钟输入推导外部 DAC 采样时钟，保留 Clocking Wizard 与
# 转发时钟的共同路径，避免错误的跨时钟域分析。
create_generated_clock -name dac_sample_clk \
    -source [get_pins u_dac_clk_oddr/C] -invert [get_ports dac_clk]
set_output_delay -clock dac_sample_clk -max 2.0 [get_ports {dac_data[*]}]
set_output_delay -clock dac_sample_clk -min -1.5 [get_ports {dac_data[*]}]
set_false_path -from [get_ports mcu_uart_rxd]

# 多波形配置采用稳定数据总线加翻转握手跨时钟域。只切断活动配置
# 寄存器D端路径；mix_toggle_sync仍由ASYNC_REG同步链处理。
set_false_path -to [get_pins -hierarchical -regexp \
    {.*(mix_type|mix_ftw|mix_phase|mix_amp|mix_duty|mix_offset)_reg.*\/D}]

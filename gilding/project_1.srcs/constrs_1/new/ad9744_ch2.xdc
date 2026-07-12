# 第二路 AD9744 独立引脚约束。映射已经按用户确认修正：U7 为 D6，V7 为 D5。
set_property -dict {PACKAGE_PIN Y6  IOSTANDARD LVCMOS33} [get_ports dac2_sleep] ;# PD/SLEEP
set_property -dict {PACKAGE_PIN Y7  IOSTANDARD LVCMOS33} [get_ports dac2_clk]   ;# CLK
set_property -dict {PACKAGE_PIN V6  IOSTANDARD LVCMOS33} [get_ports {dac2_data[13]}] ;# D14
set_property -dict {PACKAGE_PIN W6  IOSTANDARD LVCMOS33} [get_ports {dac2_data[12]}] ;# D13
set_property -dict {PACKAGE_PIN T9  IOSTANDARD LVCMOS33} [get_ports {dac2_data[11]}] ;# D12
set_property -dict {PACKAGE_PIN U10 IOSTANDARD LVCMOS33} [get_ports {dac2_data[10]}] ;# D11
set_property -dict {PACKAGE_PIN U8  IOSTANDARD LVCMOS33} [get_ports {dac2_data[9]}]  ;# D10
set_property -dict {PACKAGE_PIN U9  IOSTANDARD LVCMOS33} [get_ports {dac2_data[8]}]  ;# D9
set_property -dict {PACKAGE_PIN V8  IOSTANDARD LVCMOS33} [get_ports {dac2_data[7]}]  ;# D8
set_property -dict {PACKAGE_PIN W8  IOSTANDARD LVCMOS33} [get_ports {dac2_data[6]}]  ;# D7
set_property -dict {PACKAGE_PIN U7  IOSTANDARD LVCMOS33} [get_ports {dac2_data[5]}]  ;# D6
set_property -dict {PACKAGE_PIN V7  IOSTANDARD LVCMOS33} [get_ports {dac2_data[4]}]  ;# D5
set_property -dict {PACKAGE_PIN T5  IOSTANDARD LVCMOS33} [get_ports {dac2_data[3]}]  ;# D4
set_property -dict {PACKAGE_PIN U5  IOSTANDARD LVCMOS33} [get_ports {dac2_data[2]}]  ;# D3
set_property -dict {PACKAGE_PIN T11 IOSTANDARD LVCMOS33} [get_ports {dac2_data[1]}]  ;# D2
set_property -dict {PACKAGE_PIN V5  IOSTANDARD LVCMOS33} [get_ports {dac2_data[0]}]  ;# D1

set_property DRIVE 8 [get_ports {dac2_data[*]}]
set_property DRIVE 8 [get_ports dac2_clk]
set_property SLEW FAST [get_ports {dac2_data[*]}]
set_property SLEW FAST [get_ports dac2_clk]

# 第二路同样从本通道 ODDR 的真实 C 输入推导，不能从 sys_clk 另建一棵时钟。
create_generated_clock -name dac2_sample_clk \
    -source [get_pins u_dac2/u_dac2_clk_oddr/C] -divide_by 1 -invert [get_ports dac2_clk]
set_output_delay -clock dac2_sample_clk -max 2.0 [get_ports {dac2_data[*]}]
set_output_delay -clock dac2_sample_clk -min -1.5 [get_ports {dac2_data[*]}]

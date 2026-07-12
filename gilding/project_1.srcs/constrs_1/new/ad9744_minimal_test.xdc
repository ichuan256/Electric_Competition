# AD9744 第一路最小测试顶层专用约束。
# 使用该文件时，请在 Constraints 中禁用 ad9744_lcd_adapter.xdc 和
# ad9744_ch2.xdc，避免同一物理管脚或不存在端口被重复约束。

set_property -dict {PACKAGE_PIN U18 IOSTANDARD LVCMOS33} [get_ports sys_clk]
set_property -dict {PACKAGE_PIN N16 IOSTANDARD LVCMOS33} [get_ports sys_rst_n]
set_property -dict {PACKAGE_PIN H15 IOSTANDARD LVCMOS33} [get_ports led0]

set_property -dict {PACKAGE_PIN Y17 IOSTANDARD LVCMOS33} [get_ports dac_sleep]
set_property -dict {PACKAGE_PIN Y16 IOSTANDARD LVCMOS33} [get_ports dac_clk]

set_property -dict {PACKAGE_PIN T16 IOSTANDARD LVCMOS33} [get_ports {dac_data[13]}]
set_property -dict {PACKAGE_PIN U17 IOSTANDARD LVCMOS33} [get_ports {dac_data[12]}]
set_property -dict {PACKAGE_PIN V17 IOSTANDARD LVCMOS33} [get_ports {dac_data[11]}]
set_property -dict {PACKAGE_PIN V18 IOSTANDARD LVCMOS33} [get_ports {dac_data[10]}]
set_property -dict {PACKAGE_PIN T17 IOSTANDARD LVCMOS33} [get_ports {dac_data[9]}]
set_property -dict {PACKAGE_PIN R18 IOSTANDARD LVCMOS33} [get_ports {dac_data[8]}]
set_property -dict {PACKAGE_PIN Y18 IOSTANDARD LVCMOS33} [get_ports {dac_data[7]}]
set_property -dict {PACKAGE_PIN Y19 IOSTANDARD LVCMOS33} [get_ports {dac_data[6]}]
set_property -dict {PACKAGE_PIN P15 IOSTANDARD LVCMOS33} [get_ports {dac_data[5]}]
set_property -dict {PACKAGE_PIN P16 IOSTANDARD LVCMOS33} [get_ports {dac_data[4]}]
set_property -dict {PACKAGE_PIN W18 IOSTANDARD LVCMOS33} [get_ports {dac_data[3]}]
set_property -dict {PACKAGE_PIN W19 IOSTANDARD LVCMOS33} [get_ports {dac_data[2]}]
set_property -dict {PACKAGE_PIN R16 IOSTANDARD LVCMOS33} [get_ports {dac_data[1]}]
set_property -dict {PACKAGE_PIN R17 IOSTANDARD LVCMOS33} [get_ports {dac_data[0]}]

set_property DRIVE 8 [get_ports {dac_data[*]}]
set_property DRIVE 8 [get_ports dac_clk]
set_property SLEW FAST [get_ports {dac_data[*]}]
set_property SLEW FAST [get_ports dac_clk]

# AD9744 tS=2.0 ns、tH=1.5 ns。时钟从实际 ODDR 输入推导。
create_generated_clock -name dac_sample_clk \
    -source [get_pins u_dac_clk_oddr/C] -edges {2 3 4} [get_ports dac_clk]
set_output_delay -clock dac_sample_clk -max 2.0 [get_ports {dac_data[*]}]
set_output_delay -clock dac_sample_clk -min -1.5 [get_ports {dac_data[*]}]

# AD9744 DDS demo constraints for ALIENTEK Navigator ZYNQ.
#
# Important hardware warning:
#   The AD9744 module P1 header is not pin-compatible with the Navigator RGB LCD
#   40-pin socket power/GND positions. Do NOT bare-plug the AD9744 module into
#   the LCD socket. Use a short adapter cable/board.
#
# Adapter mapping used here, confirmed by the user:
#   FPGA pins are assigned in the AD9744 physical order:
#   D2, D1, D4, D3, ..., D14, D13, PD/SLEEP, CLK.
#   +5V and GND are hardware-only supplies and do not appear in Verilog/XDC.

create_clock -period 20.000 -name sys_clk [get_ports sys_clk]

set_property -dict {PACKAGE_PIN U18 IOSTANDARD LVCMOS33} [get_ports sys_clk]
set_property -dict {PACKAGE_PIN N16 IOSTANDARD LVCMOS33} [get_ports sys_rst_n]

# AD9744 control
set_property -dict {PACKAGE_PIN Y17 IOSTANDARD LVCMOS33} [get_ports dac_sleep] ;# AD9744 PD/SLEEP
set_property -dict {PACKAGE_PIN Y16 IOSTANDARD LVCMOS33} [get_ports dac_clk]   ;# AD9744 CLK

# AD9744 data bus. AD9744 silkscreen uses D14..D1; Verilog uses dac_data[13:0].
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

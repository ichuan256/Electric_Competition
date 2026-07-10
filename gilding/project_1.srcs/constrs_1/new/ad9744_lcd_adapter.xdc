# AD9744 DDS demo constraints for ALIENTEK Navigator ZYNQ.
#
# Important hardware warning:
#   The AD9744 module P1 header is not pin-compatible with the Navigator RGB LCD
#   40-pin socket power/GND positions. Do NOT bare-plug the AD9744 module into
#   the LCD socket. Use a short adapter cable/board.
#
# Adapter mapping used here:
#   AD9744 P1 signal -> Navigator RGB LCD signal area
#   PD/SLEEP is driven low by FPGA logic; CLK and D14..D1 are mapped to nearby
#   LCD pins to keep the bus physically grouped.

create_clock -period 20.000 -name sys_clk [get_ports sys_clk]

set_property -dict {PACKAGE_PIN U18 IOSTANDARD LVCMOS33} [get_ports sys_clk]
set_property -dict {PACKAGE_PIN N16 IOSTANDARD LVCMOS33} [get_ports sys_rst_n]

# AD9744 control
set_property -dict {PACKAGE_PIN W18 IOSTANDARD LVCMOS33} [get_ports dac_sleep] ;# AD9744 PD/SLEEP, LCD_R0
set_property -dict {PACKAGE_PIN P19 IOSTANDARD LVCMOS33} [get_ports dac_clk]   ;# AD9744 CLK, LCD_CLK

# AD9744 data bus. AD9744 silkscreen uses D14..D1; Verilog uses dac_data[13:0].
set_property -dict {PACKAGE_PIN W19 IOSTANDARD LVCMOS33} [get_ports {dac_data[13]}] ;# D14, LCD_R1
set_property -dict {PACKAGE_PIN R16 IOSTANDARD LVCMOS33} [get_ports {dac_data[12]}] ;# D13, LCD_R2
set_property -dict {PACKAGE_PIN R17 IOSTANDARD LVCMOS33} [get_ports {dac_data[11]}] ;# D12, LCD_R3
set_property -dict {PACKAGE_PIN W20 IOSTANDARD LVCMOS33} [get_ports {dac_data[10]}] ;# D11, LCD_R4
set_property -dict {PACKAGE_PIN V20 IOSTANDARD LVCMOS33} [get_ports {dac_data[9]}]  ;# D10, LCD_R5
set_property -dict {PACKAGE_PIN P18 IOSTANDARD LVCMOS33} [get_ports {dac_data[8]}]  ;# D9,  LCD_R6
set_property -dict {PACKAGE_PIN N17 IOSTANDARD LVCMOS33} [get_ports {dac_data[7]}]  ;# D8,  LCD_R7
set_property -dict {PACKAGE_PIN V17 IOSTANDARD LVCMOS33} [get_ports {dac_data[6]}]  ;# D7,  LCD_G0
set_property -dict {PACKAGE_PIN V18 IOSTANDARD LVCMOS33} [get_ports {dac_data[5]}]  ;# D6,  LCD_G1
set_property -dict {PACKAGE_PIN T17 IOSTANDARD LVCMOS33} [get_ports {dac_data[4]}]  ;# D5,  LCD_G2
set_property -dict {PACKAGE_PIN R18 IOSTANDARD LVCMOS33} [get_ports {dac_data[3]}]  ;# D4,  LCD_G3
set_property -dict {PACKAGE_PIN Y18 IOSTANDARD LVCMOS33} [get_ports {dac_data[2]}]  ;# D3,  LCD_G4
set_property -dict {PACKAGE_PIN Y19 IOSTANDARD LVCMOS33} [get_ports {dac_data[1]}]  ;# D2,  LCD_G5
set_property -dict {PACKAGE_PIN P15 IOSTANDARD LVCMOS33} [get_ports {dac_data[0]}]  ;# D1,  LCD_G6

set_property DRIVE 8 [get_ports {dac_data[*]}]
set_property DRIVE 8 [get_ports dac_clk]
set_property SLEW FAST [get_ports {dac_data[*]}]
set_property SLEW FAST [get_ports dac_clk]

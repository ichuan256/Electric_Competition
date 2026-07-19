# Stage3: ZYNQ-7020 + DAC1 + Blue UART only.
set_property -dict {PACKAGE_PIN U18 IOSTANDARD LVCMOS33} [get_ports sys_clk]
create_clock -name sys_clk -period 20.000 [get_ports sys_clk]
set_property -dict {PACKAGE_PIN N16 IOSTANDARD LVCMOS33} [get_ports sys_rst_n]

# Active-low board LEDs: PS_LED0=valid V2 frame, LED1=any UART byte.
set_property -dict {PACKAGE_PIN H15 IOSTANDARD LVCMOS33} [get_ports led0]
set_property -dict {PACKAGE_PIN L15 IOSTANDARD LVCMOS33} [get_ports led1]

# Blue UART: Blue_TX -> FPGA K14; Blue_RX <- FPGA M15.
set_property -dict {PACKAGE_PIN K14 IOSTANDARD LVCMOS33} [get_ports mcu_uart_rxd]
set_property -dict {PACKAGE_PIN M15 IOSTANDARD LVCMOS33} [get_ports mcu_uart_txd]
set_property PULLUP true [get_ports mcu_uart_rxd]

# DAC1 control signals.
set_property -dict {PACKAGE_PIN Y17 IOSTANDARD LVCMOS33} [get_ports dac_sleep]
set_property -dict {PACKAGE_PIN Y16 IOSTANDARD LVCMOS33} [get_ports dac_clk]

# DAC1 14-bit two's-complement bus: Verilog bit 13..0 -> AD9744 D14..D1.
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

# Data is updated on sample_clk rising edges. ODDR makes the external DAC clock
# rise on sample_clk falling edges, giving a nominal 5 ns setup interval.
create_generated_clock -name dac_sample_clk \
    -source [get_pins u_dac_clk_oddr/C] -divide_by 1 -invert [get_ports dac_clk]
set_output_delay -clock dac_sample_clk -max 2.0 [get_ports {dac_data[*]}]
set_output_delay -clock dac_sample_clk -min -1.5 [get_ports {dac_data[*]}]

# UART RX is asynchronous and enters a two-flop synchronizer.
set_false_path -from [get_ports mcu_uart_rxd]

# Multi-bit settings remain stable around the synchronized commit toggle.
set_false_path -to [get_pins commit_sync_reg[0]/D]
set_false_path -to [get_pins -hierarchical -regexp {.*active_.*_reg.*\/D}]

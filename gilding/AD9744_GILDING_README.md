# AD9744 on Gilding

This Vivado project contains a first-pass FPGA-only AD9744 DDS demo.

## Files

- `project_1.srcs/sources_1/new/ad9744_dds_top.v`
  - Top module: `ad9744_dds_top`
  - Generates a fixed 1 kHz sine wave.
  - Drives `dac_clk`, `dac_data[13:0]`, and `dac_sleep`.
  - Uses no manually-created Vivado IP.

- `project_1.srcs/constrs_1/new/ad9744_lcd_adapter.xdc`
  - Maps the AD9744 digital bus to the user-confirmed FPGA pins.
  - Keeps the data lines in the physical AD9744 order: `D2,D1,D4,D3,...D14,D13,PD,CLK`.

## Hardware warning

The AD9744 module P1 pinout is:

| AD9744 row | Left | Right |
| --- | --- | --- |
| 1 | PD/SLEEP | CLK |
| 2 | D14 | D13 |
| 3 | D12 | D11 |
| 4 | D10 | D9 |
| 5 | D8 | D7 |
| 6 | D6 | D5 |
| 7 | D4 | D3 |
| 8 | D2 | D1 |
| 9 | GND | GND |
| 10 | +5V | +5V |

The Navigator ZYNQ baseboard RGB LCD 40-pin connector does not match these
power and ground positions. Do not bare-plug the AD9744 module into the LCD
connector unless your adapter board explicitly remaps the power and ground
positions. Use a short 2x10 adapter cable/board and wire the signals according
to `ad9744_lcd_adapter.xdc`. Supply AD9744 `+5V` and `GND` from the board power
pins or a stable external 5 V supply, with common ground to the FPGA board.

## Adapter signal table

| AD9744 P1 | Navigator signal | FPGA pin |
| --- | --- | --- |
| D2 | user adapter pin 1 | R16 |
| D1 | user adapter pin 2 | R17 |
| D4 | user adapter pin 3 | W18 |
| D3 | user adapter pin 4 | W19 |
| D6 | user adapter pin 5 | P15 |
| D5 | user adapter pin 6 | P16 |
| D8 | user adapter pin 7 | Y18 |
| D7 | user adapter pin 8 | Y19 |
| D10 | user adapter pin 9 | T17 |
| D9 | user adapter pin 10 | R18 |
| D12 | user adapter pin 11 | V17 |
| D11 | user adapter pin 12 | V18 |
| D14 | user adapter pin 13 | T16 |
| D13 | user adapter pin 14 | U17 |
| PD/SLEEP | user adapter pin 15 | Y17 |
| CLK | user adapter pin 16 | Y16 |
| spare | user adapter pin 17 | T14 |
| spare | user adapter pin 18 | T15 |
| spare | user adapter pin 19 | V16 |
| spare | user adapter pin 20 | W16 |
| GND | GND | board GND |
| +5V | +5V | board +5V |

## Vivado operation

1. Open `project_1.xpr`.
2. Confirm `ad9744_dds_top` is the top module.
3. Run Synthesis.
4. Run Implementation.
5. Generate Bitstream.
6. Program the FPGA.

## IP cores

No IP cores are required for this first version.

For a higher-quality or adjustable design later:

- Use `Clocking Wizard` if you want a DAC sample clock above the 50 MHz board clock.
- Use `Block Memory Generator` if you want a larger sine table loaded from `.coe`.
- Use UART control logic to update frequency, amplitude, and waveform selection.

## Software

No ARM/PS software is required for this version. The PL fabric starts generating
the waveform immediately after configuration and reset release.

# AD9744 on Gilding

This Vivado project contains a first-pass FPGA-only AD9744 DDS demo.

## Files

- `project_1.srcs/sources_1/new/ad9744_dds_top.v`
  - Top module: `ad9744_dds_top`
  - Generates a fixed 1 kHz sine wave.
  - Drives `dac_clk`, `dac_data[13:0]`, and `dac_sleep`.
  - Uses no manually-created Vivado IP.

- `project_1.srcs/constrs_1/new/ad9744_lcd_adapter.xdc`
  - Maps the AD9744 digital bus to the Navigator ZYNQ RGB LCD connector signal area.
  - Keeps the data lines grouped on LCD RGB pins.

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
connector. Use a short 2x10 adapter cable/board and wire the signals according
to `ad9744_lcd_adapter.xdc`. Supply AD9744 `+5V` and `GND` from the board power
pins or a stable external 5 V supply, with common ground to the FPGA board.

## Adapter signal table

| AD9744 P1 | Navigator signal | FPGA pin |
| --- | --- | --- |
| PD/SLEEP | LCD_R0 | W18 |
| CLK | LCD_CLK | P19 |
| D14 | LCD_R1 | W19 |
| D13 | LCD_R2 | R16 |
| D12 | LCD_R3 | R17 |
| D11 | LCD_R4 | W20 |
| D10 | LCD_R5 | V20 |
| D9 | LCD_R6 | P18 |
| D8 | LCD_R7 | N17 |
| D7 | LCD_G0 | V17 |
| D6 | LCD_G1 | V18 |
| D5 | LCD_G2 | T17 |
| D4 | LCD_G3 | R18 |
| D3 | LCD_G4 | Y18 |
| D2 | LCD_G5 | Y19 |
| D1 | LCD_G6 | P15 |
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

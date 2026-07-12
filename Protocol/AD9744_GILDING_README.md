# Gilding AD9744 UART Protocol

## 1. Scope

This document defines the UART protocol between the STM32 controller and the FPGA AD9744 signal-source logic.

The protocol keeps the original single-waveform commands and adds a transaction mode for waveform superposition. In the new mode, the MCU sends a begin frame, then sends one or more waveform parameter groups, then sends a commit frame and an end frame. The FPGA stores all received waveform entries during the transaction and starts summing them only after the end frame is accepted.

Target use cases:

- Single sine, square, triangle, and sawtooth output.
- Two independent DAC channels.
- Multi-waveform superposition on one channel.
- Later extension for arbitrary waveform recipes without changing the physical UART frame.

## 2. UART Parameters

```text
Baud rate : 115200 bit/s
Data bits : 8
Parity    : none
Stop bits : 1
Level     : 3.3 V TTL/CMOS
Order     : LSB first inside each UART byte
```

MCU TX connects to FPGA RX. FPGA TX connects to MCU RX. Both boards must share ground.

## 3. Common Frame Format

The MCU always sends a 7-byte command frame:

```text
A5 CMD DATA0 DATA1 DATA2 DATA3 XOR
```

`DATA0` is the least significant byte of the 32-bit data word.

```text
DATA = DATA0 + (DATA1 << 8) + (DATA2 << 16) + (DATA3 << 24)
XOR  = A5 ^ CMD ^ DATA0 ^ DATA1 ^ DATA2 ^ DATA3
```

After a command is parsed, the FPGA returns a 4-byte ACK frame:

```text
5A CMD STATUS XOR
```

```text
XOR = 5A ^ CMD ^ STATUS
```

`STATUS` values:

| STATUS | Meaning |
| --- | --- |
| `00` | OK, command accepted |
| `01` | Bad checksum |
| `02` | Unknown command |
| `03` | Invalid data range |
| `04` | Command is not valid in current state |
| `05` | Waveform slot overflow |
| `06` | Transaction is incomplete |
| `07` | Output would exceed allowed DAC range |
| `08` | Busy, retry later |

The MCU should wait for ACK before sending the next command. If ACK times out, the MCU may retry the same frame. FPGA command handling should be idempotent where possible.

## 4. Numeric Encoding

All multi-byte numeric fields are little-endian inside `DATA`.

| Quantity | Encoding |
| --- | --- |
| Frequency | `FTW = round(f_out_hz * 2^32 / 100000000)` |
| Phase | `PHASE = round((phase_deg mod 360) * 2^32 / 360)` |
| Amplitude | unsigned 14-bit code, `0..8191`, where `8191` is full scale |
| DC offset | signed 14-bit DAC code, `-8192..8191`, sent as signed 16-bit in low word |
| Duty | unsigned 16-bit code, `0..65535` corresponding to `0..100%` |
| Waveform | `0=sine`, `1=square`, `2=triangle`, `3=sawtooth` |
| Output enable | bit0, `1=output enabled`, `0=zero output` |
| Final output bias target | signed millivolts, stored by the MCU UI as `output_bias_mv` |

Recommended square-wave duty range for the contest requirement is `0.1%..99.9%`, corresponding roughly to `66..65469`.

To avoid clipping, each waveform should satisfy:

```text
abs(offset_code) + amplitude_code <= 8191
```

For waveform superposition, the FPGA should either saturate the final summed value to signed 14-bit range or reject unsafe recipes. The preferred first implementation is to reject a recipe when the worst-case sum may exceed the DAC range:

```text
sum(abs(offset_i) + amplitude_i) <= 8191
```

The final output bias target is currently only recorded in the STM32 UI/status state. The conversion from final output bias target to the Blue board DAC output value is intentionally left as a calibration item:

```text
board_dac_value = f(output_bias_mv)
```

After the hardware conversion relationship is measured or confirmed from the schematic, this function should be added on the Blue side before writing the board DAC.

## 5. Original Single-Waveform Commands

These commands directly update the active output registers. They remain valid outside transaction mode.

Channel 1 uses `01..07`.

| CMD | Parameter | DATA meaning |
| --- | --- | --- |
| `01` | Frequency | unsigned 32-bit FTW |
| `02` | Phase | unsigned 32-bit phase word |
| `03` | Amplitude | low 16 bits, `0..8191` |
| `04` | DC offset | low 16 bits as signed 14-bit code |
| `05` | Duty | low 16 bits, `0..65535` |
| `06` | Waveform | low 8 bits, `0..3` |
| `07` | Output enable | bit0 |

Channel 2 uses the same layout with the command high bit set.

| Channel 2 CMD | Equivalent channel 1 CMD |
| --- | --- |
| `81` | `01` frequency |
| `82` | `02` phase |
| `83` | `03` amplitude |
| `84` | `04` DC offset |
| `85` | `05` duty |
| `86` | `06` waveform |
| `87` | `07` output enable |

## 6. Multi-Waveform Superposition Transaction

### 6.1 Design Goal

The MCU enters a dedicated configuration mode and sends a complete waveform list. The FPGA stores the list in a staging buffer. The previous output should continue until the transaction is committed and ended. After the end frame, the FPGA atomically switches to the new summed waveform output.

This avoids partially updated output during configuration.

### 6.2 State Machine

```text
IDLE
  | CMD 20 BEGIN
  v
RECEIVING
  | CMD 21 WAVE_BEGIN
  | CMD 22..28 WAVE_PARAM frames
  | repeat for each waveform
  | CMD 2E COMMIT
  v
COMMITTED
  | CMD 2F END
  v
ACTIVE_SUM_OUTPUT
```

Abort path:

```text
RECEIVING or COMMITTED -- CMD 2D ABORT --> IDLE, staging buffer discarded
```

If any command is received in the wrong state, FPGA returns `STATUS=04`.

### 6.3 Transaction Commands

| CMD | Name | Valid state | DATA meaning |
| --- | --- | --- | --- |
| `20` | SUM_BEGIN | IDLE or ACTIVE_SUM_OUTPUT | start a new staging transaction |
| `21` | SUM_WAVE_BEGIN | RECEIVING | select/create one waveform slot |
| `22` | SUM_FREQ | RECEIVING | frequency FTW for current slot |
| `23` | SUM_PHASE | RECEIVING | phase word for current slot |
| `24` | SUM_AMPLITUDE | RECEIVING | amplitude code for current slot |
| `25` | SUM_OFFSET | RECEIVING | signed offset code for current slot |
| `26` | SUM_DUTY | RECEIVING | duty code for current slot |
| `27` | SUM_WAVEFORM | RECEIVING | waveform type for current slot |
| `28` | SUM_ENABLE | RECEIVING | enable bit for current slot |
| `2D` | SUM_ABORT | RECEIVING or COMMITTED | discard staging buffer |
| `2E` | SUM_COMMIT | RECEIVING | validate staging buffer |
| `2F` | SUM_END | COMMITTED | atomically switch output to summed buffer |

### 6.4 SUM_BEGIN Data Layout

```text
DATA[7:0]    channel_id      0=channel1, 1=channel2
DATA[15:8]   wave_count      expected waveform count, 1..MAX_SUM_WAVES
DATA[23:16]  flags
DATA[31:24]  protocol_ver    currently 1
```

`flags`:

| Bit | Meaning |
| --- | --- |
| 0 | `1`: clear previous sum output before applying new recipe |
| 1 | `1`: reject recipe if worst-case sum exceeds DAC range |
| 2 | `1`: allow FPGA saturation instead of rejecting overflow |
| 3..7 | reserved, send as 0 |

Recommended first-use value:

```text
DATA = (1 << 24) | (0x03 << 16) | (wave_count << 8) | channel_id
```

This means protocol version 1, clear previous sum, reject unsafe overflow.

### 6.5 SUM_WAVE_BEGIN Data Layout

```text
DATA[7:0]    wave_index      0..MAX_SUM_WAVES-1
DATA[15:8]   channel_id      0=channel1, 1=channel2
DATA[23:16]  flags
DATA[31:24]  reserved        send as 0
```

`flags`:

| Bit | Meaning |
| --- | --- |
| 0 | waveform enabled by default |
| 1 | last waveform hint, optional |
| 2..7 | reserved, send as 0 |

After `SUM_WAVE_BEGIN`, commands `22..28` write into that selected slot.

### 6.6 Wave Parameter Frames

Each waveform slot uses the same parameter encoding as the original single-waveform commands.

| CMD | Field | DATA |
| --- | --- | --- |
| `22` | frequency | unsigned 32-bit FTW |
| `23` | phase | unsigned 32-bit phase word |
| `24` | amplitude | low 16 bits, `0..8191` |
| `25` | offset | low 16 bits, signed value `-8192..8191` |
| `26` | duty | low 16 bits, `0..65535` |
| `27` | waveform | low 8 bits, `0..3` |
| `28` | enable | bit0 |

A waveform slot is complete only after all required fields have been received:

```text
frequency, phase, amplitude, offset, duty, waveform, enable
```

FPGA should track a per-slot valid mask. `SUM_COMMIT` fails with `STATUS=06` if any enabled slot is incomplete.

### 6.7 SUM_COMMIT

`CMD=2E` validates all staged waveform entries.

```text
DATA[7:0]    channel_id
DATA[15:8]   expected_wave_count
DATA[31:16]  reserved, send as 0
```

On `SUM_COMMIT`, FPGA checks:

- transaction state is RECEIVING;
- expected count matches the number declared by `SUM_BEGIN`;
- all enabled waveform slots are complete;
- every field is in range;
- optional worst-case sum does not exceed DAC signed 14-bit range.

If validation succeeds, FPGA returns `STATUS=00` and enters COMMITTED state. Output is still not switched yet.

### 6.8 SUM_END

`CMD=2F` tells FPGA to apply the staged recipe.

```text
DATA[7:0]    channel_id
DATA[31:8]   reserved, send as 0
```

On `SUM_END`, FPGA atomically copies the staged buffer into the active sum buffer and starts outputting:

```text
sample[n] = saturate14(sum(wave_i[n]) + global_offset_optional)
```

For the first implementation, no global offset is required. Each waveform uses its own offset. If `SUM_END` is received before successful `SUM_COMMIT`, FPGA returns `STATUS=04`.

### 6.9 SUM_ABORT

`CMD=2D` discards the staging buffer and returns to IDLE or the previous active output.

```text
DATA = 0
```

The MCU should send `SUM_ABORT` after repeated ACK timeout or if the user exits the configuration screen.

## 7. Example Transaction

Example: configure channel 1 to output a sum of two waveforms.

```text
20  channel=0, wave_count=2, flags=clear+reject_overflow, version=1
21  wave_index=0, channel=0, enabled=1
22  wave0 FTW
23  wave0 PHASE
24  wave0 AMPLITUDE
25  wave0 OFFSET
26  wave0 DUTY
27  wave0 WAVEFORM
28  wave0 ENABLE
21  wave_index=1, channel=0, enabled=1
22  wave1 FTW
23  wave1 PHASE
24  wave1 AMPLITUDE
25  wave1 OFFSET
26  wave1 DUTY
27  wave1 WAVEFORM
28  wave1 ENABLE
2E  commit channel=0, expected_wave_count=2
2F  end channel=0
```

The MCU should check `STATUS=00` after every frame. If a nonzero status is returned, show an error and send `SUM_ABORT`.

## 8. FPGA Implementation Notes

Recommended constants:

```text
MAX_SUM_WAVES = 8 for first implementation
DDS sample clock = 100 MHz
Internal phase accumulator width = 32 bits
DAC output = signed 14-bit two's-complement-compatible code
Summing accumulator width >= 20 bits
```

Each staged waveform slot should contain:

```text
valid_mask
channel_id
ftw
phase_word
amplitude_code
offset_code
duty_code
waveform_type
enable
```

Recommended summing datapath:

1. Each enabled waveform generates a signed 14-bit sample before offset.
2. Scale by `amplitude_code`.
3. Add `offset_code`.
4. Sum all enabled waveform samples in a wider signed accumulator.
5. Saturate to `-8192..8191`.
6. Convert to AD9744 output coding as required by the DAC interface.

For predictable output, update active recipe only on `SUM_END`, preferably synchronized to a DDS sample boundary.

## 9. MCU UI Notes

The MCU should expose one configuration UI that selects the protocol by waveform count.

When `wave_count == 1`, the MCU must use the original single-waveform commands only:

```text
01/81 frequency
02/82 phase
03/83 amplitude
04/84 offset
05/85 duty
06/86 waveform
07/87 output enable
```

No `SUM_BEGIN`, `SUM_WAVE_BEGIN`, `SUM_COMMIT`, or `SUM_END` frame is sent for a single waveform.
For backward compatibility with simple FPGA receivers, the MCU may send these seven legacy frames as a timed command group without blocking on ACK between frames. If ACK frames are returned, the MCU still records and displays them for status/debugging.

When `wave_count > 1`, the MCU uses the multi-waveform transaction flow:

1. User enters multi-waveform mode.
2. MCU sends `SUM_BEGIN` with channel and wave count.
3. For each waveform row in the UI, MCU sends `SUM_WAVE_BEGIN` followed by `SUM_FREQ..SUM_ENABLE`.
4. MCU sends `SUM_COMMIT`.
5. If ACK is OK, MCU sends `SUM_END`.
6. UI shows success and returns to move state.

If the user cancels editing, MCU sends `SUM_ABORT`.

Multi-waveform transaction mode should use only `20..2F` during a transaction. The FPGA should start staging multiple waveforms only after the `SUM_BEGIN` frame and should atomically apply the staged sum only after `SUM_END`.

## 10. Backward Compatibility

- Existing single-waveform commands `01..07` and `81..87` are unchanged.
- Existing ACK format is unchanged.
- If FPGA firmware does not support `20..2F`, it returns `STATUS=02` unknown command.
- MCU should detect `STATUS=02` and disable the multi-waveform UI for that session.

## 11. Recommended Test Order

1. Verify old single-waveform commands still work.
2. Set UI `wave_count=1` and confirm the MCU sends only `01..07` or `81..87`, with no `20..2F` frames.
3. Set UI `wave_count=2` and confirm the MCU sends `SUM_BEGIN`, both waveform slots, `SUM_COMMIT`, then `SUM_END`.
4. Send two low-amplitude sine waves at different frequencies; verify output shows both components.
5. Send incomplete waveform and confirm `SUM_COMMIT` returns `STATUS=06`.
6. Send an unsafe amplitude recipe and confirm either `STATUS=07` or saturated output, depending on flags.
7. Send `SUM_ABORT` halfway through a transaction and confirm previous output remains unchanged.

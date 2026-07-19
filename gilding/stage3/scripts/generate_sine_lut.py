import math
from pathlib import Path

out = Path(__file__).resolve().parents[1] / "rtl" / "sine_lut_4096.mem"
lines = []
for i in range(4096):
    value = round(8191 * math.sin(2 * math.pi * i / 4096))
    if value < 0:
        value = (1 << 14) + value
    lines.append(f"{value & 0x3FFF:04X}")
out.write_text("\n".join(lines) + "\n", encoding="ascii")
print(out)

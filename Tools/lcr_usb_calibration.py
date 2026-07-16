#!/usr/bin/env python3
"""通过 Blue 板 USB CDC 完成 LCR 引导校准。"""

import argparse
import pathlib
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("缺少 pyserial，请先执行: pip install pyserial", file=sys.stderr)
    raise SystemExit(2)


def choose_port(explicit: str | None) -> str:
    if explicit:
        return explicit
    ports = list(list_ports.comports())
    matching = [p for p in ports if "Blue LCR Calibration" in (p.description or "")]
    candidates = matching or ports
    if len(candidates) == 1:
        return candidates[0].device
    if not candidates:
        raise RuntimeError("未发现串口，请确认 Blue 板 Type-C 数据线已连接")
    print("可用串口：")
    for index, port in enumerate(candidates, 1):
        print(f"  {index}. {port.device}  {port.description}")
    return candidates[int(input("选择序号: ")) - 1].device


def send(port: serial.Serial, command: str) -> None:
    port.write((command + "\r\n").encode("ascii"))
    port.flush()


def wait_step(port: serial.Serial, step: str, timeout: float = 90.0) -> list[str]:
    lines: list[str] = []
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        raw = port.readline()
        if not raw:
            continue
        line = raw.decode("ascii", errors="replace").strip()
        if not line:
            continue
        print(line)
        lines.append(line)
        if line.startswith("ERR "):
            raise RuntimeError(line)
        if line.startswith(f"OK {step} STAGE="):
            return lines
    raise TimeoutError(f"等待 {step} 完成超时")


def run_step(port: serial.Serial, step: str, prompt: str) -> list[str]:
    input(f"\n{prompt}\n接好后按回车开始 {step}...")
    port.reset_input_buffer()
    send(port, step)
    return wait_step(port, step)


def main() -> int:
    parser = argparse.ArgumentParser(description="Blue USB LCR 引导校准")
    parser.add_argument("--port", help="例如 COM7；不填则自动选择")
    parser.add_argument("--rref", type=float, default=470.0, help="RREF 实测阻值/Ω")
    parser.add_argument("--load", type=float, default=470.0, help="LOAD 实测阻值/Ω")
    parser.add_argument("--output", default="lcr_calibration_export.txt")
    args = parser.parse_args()

    port_name = choose_port(args.port)
    print(f"连接 {port_name}")
    with serial.Serial(port_name, 115200, timeout=0.5, write_timeout=2.0) as port:
        time.sleep(0.8)
        send(port, f"SET RREF {args.rref:.6f}")
        send(port, f"SET LOAD {args.load:.6f}")
        run_step(port, "ZERO", "将 V1、V2 都接模拟地；不要保留交流激励。")
        run_step(port, "SHORT", "恢复 RREF，将 DUT 位置 V1 与 V2 短接。")
        run_step(port, "LOAD", f"DUT 位置接入实测 {args.load:.6f} Ω 标准电阻。")
        run_step(port, "OPEN", "拆除 DUT/LOAD，使 V1 与 V2 之间开路，RREF 保留。")
        lines = run_step(port, "VERIFY", f"重新接入 {args.load:.6f} Ω 标准电阻。")

        export: list[str] = []
        deadline = time.monotonic() + 15.0
        while time.monotonic() < deadline:
            raw = port.readline()
            if not raw:
                continue
            line = raw.decode("ascii", errors="replace").strip()
            print(line)
            if line == "LCR_CAL_BEGIN":
                export = [line]
            elif export:
                export.append(line)
                if line == "LCR_CAL_END":
                    break
        if not export:
            send(port, "EXPORT")
            while True:
                line = port.readline().decode("ascii", errors="replace").strip()
                if line == "LCR_CAL_BEGIN":
                    export = [line]
                elif export:
                    export.append(line)
                    if line == "LCR_CAL_END":
                        break
        output = pathlib.Path(args.output)
        output.write_text("\n".join(export) + "\n", encoding="utf-8")
        print(f"\n校准通过，导出已保存到 {output.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

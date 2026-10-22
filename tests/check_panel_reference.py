"""Compare the ported LCD initialization against the supplied vendor sequence.

Run: python tests/check_panel_reference.py [path-to-vendor-Gamma2.2.txt]
"""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[1]
reference = Path(sys.argv[1]) if len(sys.argv) > 1 else (
    Path.home() / "Desktop/射频管家/屏幕相关/IC&Code/"
    "HSD0.96IPS(HSD010HPW2-A00)（80RGB 160）+ST7735S---Gamma2.2.txt"
)
source = (root / "components/bsp/bsp_display.c").read_text(encoding="utf-8")
board = (root / "components/bsp/include/bsp_board.h").read_text(encoding="utf-8")
pins = re.findall(r"#define\s+(BSP_\w+)\s+GPIO_NUM_(\d+)", board)
assert len(pins) == 19, f"Unexpected pin count: {len(pins)}"
assert len({pin for _, pin in pins}) == len(pins), "GPIO collision"

expected = []
for kind, value in re.findall(
    r"Write\(\s*(Command|Parameter)\s*,\s*(0x[0-9A-Fa-f]+)\s*\)",
    reference.read_text(encoding="utf-8", errors="replace"),
):
    if kind == "Command":
        expected.append((int(value, 16), []))
    else:
        expected[-1][1].append(int(value, 16))

madctl = int(re.search(r"#define\s+BSP_LCD_MADCTL\s+(0x\w+)", board)[1], 16)
# Sleep-out/reset delays are separate in the port, and addressing is per flush.
expected = [(cmd, [madctl] if cmd == 0x36 else data)
            for cmd, data in expected if cmd not in (0x11, 0x2A, 0x2B, 0x2C)]
table = source.split("init_commands[] = {", 1)[1].split("\n};", 1)[0]
actual = []
for cmd, length, values in re.findall(
    r"\{(0x[0-9A-Fa-f]+),\s*(\d+),\s*\{([^}]+)\}\}", table
):
    values = values.replace("BSP_LCD_MADCTL", hex(madctl))
    data = [int(v.strip(), 0) for v in values.split(",") if v.strip()]
    actual.append((int(cmd, 16), data[:int(length)]))
assert actual == expected, "LCD parameters differ from the vendor sequence"
assert madctl in (0x68, 0xA8), "Expected vendor landscape orientation"
for name, expected_value in {"WIDTH": 160, "HEIGHT": 80, "X_OFFSET": 1, "Y_OFFSET": 26}.items():
    value = int(re.search(rf"#define\s+BSP_LCD_{name}\s+(\d+)", board)[1])
    assert value == expected_value, f"Unexpected LCD {name}"
print("PASS: unique board pins; vendor LCD parameters, gamma, landscape and offsets")

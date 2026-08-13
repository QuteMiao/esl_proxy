#!/usr/bin/env python3
"""
swimlane_records.json -> PNG 速览图

Perfetto 才是正经看图的地方（能缩放、能点、能看 tooltip）。这个脚本只解决一件事:
不打开浏览器也能一眼看到整体形状 —— 贴进文档、发给别人、CI 里存档都方便。

只用标准库 (zlib + struct), 不依赖 matplotlib/PIL, 免得为了看张图去装一堆东西。

用法:
    python3 tools/swimlane_png.py report/swimlane_records.json
    python3 tools/swimlane_png.py records.json -o out.png --width 1800
"""

import argparse
import json
import os
import struct
import sys
import zlib
from collections import defaultdict

# ── 配色 (R, G, B) ──────────────────────────────────────────────────────────
BG = (250, 250, 252)
LANE_BG = (238, 240, 245)
LANE_BG_ALT = (231, 234, 241)
AXIS = (150, 156, 170)
TEXT = (60, 64, 78)

C_RUN_CUBE = (58, 124, 214)     # 蓝: CUBE 执行
C_RUN_VEC = (232, 138, 42)      # 橙: VECTOR 执行
C_GATED = (150, 150, 158)       # 灰: ED 占槽等门铃
C_WAIT = (198, 210, 232)        # 浅蓝: 可执行但核还忙
C_FIN = (206, 196, 176)         # 米: 完成到被看见

# 5x7 点阵字模，只覆盖标注需要的字符
GLYPHS = {
    "0": ["01110", "10001", "10011", "10101", "11001", "10001", "01110"],
    "1": ["00100", "01100", "00100", "00100", "00100", "00100", "01110"],
    "2": ["01110", "10001", "00001", "00010", "00100", "01000", "11111"],
    "3": ["11111", "00010", "00100", "00010", "00001", "10001", "01110"],
    "4": ["00010", "00110", "01010", "10010", "11111", "00010", "00010"],
    "5": ["11111", "10000", "11110", "00001", "00001", "10001", "01110"],
    "6": ["00110", "01000", "10000", "11110", "10001", "10001", "01110"],
    "7": ["11111", "00001", "00010", "00100", "01000", "01000", "01000"],
    "8": ["01110", "10001", "10001", "01110", "10001", "10001", "01110"],
    "9": ["01110", "10001", "10001", "01111", "00001", "00010", "01100"],
    "C": ["01110", "10001", "10000", "10000", "10000", "10001", "01110"],
    "U": ["10001", "10001", "10001", "10001", "10001", "10001", "01110"],
    "B": ["11110", "10001", "10001", "11110", "10001", "10001", "11110"],
    "E": ["11111", "10000", "10000", "11110", "10000", "10000", "11111"],
    "V": ["10001", "10001", "10001", "10001", "10001", "01010", "00100"],
    "S": ["01111", "10000", "10000", "01110", "00001", "00001", "11110"],
    "R": ["11110", "10001", "10001", "11110", "10100", "10010", "10001"],
    "N": ["10001", "11001", "10101", "10011", "10001", "10001", "10001"],
    "I": ["01110", "00100", "00100", "00100", "00100", "00100", "01110"],
    "G": ["01110", "10001", "10000", "10111", "10001", "10001", "01111"],
    "A": ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
    "T": ["11111", "00100", "00100", "00100", "00100", "00100", "00100"],
    "D": ["11110", "10001", "10001", "10001", "10001", "10001", "11110"],
    "W": ["10001", "10001", "10001", "10101", "10101", "11011", "10001"],
    "F": ["11111", "10000", "10000", "11110", "10000", "10000", "10000"],
    "M": ["10001", "11011", "10101", "10101", "10001", "10001", "10001"],
    "O": ["01110", "10001", "10001", "10001", "10001", "10001", "01110"],
    "P": ["11110", "10001", "10001", "11110", "10000", "10000", "10000"],
    "L": ["10000", "10000", "10000", "10000", "10000", "10000", "11111"],
    "H": ["10001", "10001", "10001", "11111", "10001", "10001", "10001"],
    "J": ["00111", "00010", "00010", "00010", "00010", "10010", "01100"],
    "K": ["10001", "10010", "10100", "11000", "10100", "10010", "10001"],
    "Q": ["01110", "10001", "10001", "10001", "10101", "10010", "01101"],
    "X": ["10001", "10001", "01010", "00100", "01010", "10001", "10001"],
    "Y": ["10001", "10001", "01010", "00100", "00100", "00100", "00100"],
    "Z": ["11111", "00001", "00010", "00100", "01000", "10000", "11111"],
    ":": ["00000", "01100", "01100", "00000", "01100", "01100", "00000"],
    "+": ["00000", "00100", "00100", "11111", "00100", "00100", "00000"],
    "_": ["00000", "00000", "00000", "00000", "00000", "00000", "11111"],
    "-": ["00000", "00000", "00000", "11111", "00000", "00000", "00000"],
    ".": ["00000", "00000", "00000", "00000", "00000", "01100", "01100"],
    " ": ["00000"] * 7,
    "u": ["00000", "00000", "10001", "10001", "10001", "10011", "01101"],
    "s": ["00000", "00000", "01111", "10000", "01110", "00001", "11110"],
    "%": ["11001", "11010", "00010", "00100", "01000", "01011", "10011"],
    "(": ["00010", "00100", "01000", "01000", "01000", "00100", "00010"],
    ")": ["01000", "00100", "00010", "00010", "00010", "00100", "01000"],
    "=": ["00000", "00000", "11111", "00000", "11111", "00000", "00000"],
    "/": ["00001", "00010", "00010", "00100", "01000", "01000", "10000"],
    ",": ["00000", "00000", "00000", "00000", "01100", "00100", "01000"],
}

GLYPH_W, GLYPH_H = 5, 7


class Canvas:
    def __init__(self, width, height, bg=BG):
        self.w = width
        self.h = height
        self.buf = bytearray(bg * (width * height))

    def rect(self, x0, y0, x1, y1, color):
        """填充矩形；坐标自动裁剪到画布内，宽度不足 1px 时保底画 1px。"""
        x0 = max(0, int(x0))
        y0 = max(0, int(y0))
        x1 = min(self.w, max(int(x1), x0 + 1))
        y1 = min(self.h, max(int(y1), y0 + 1))
        if x0 >= x1 or y0 >= y1:
            return
        row = bytes(color) * (x1 - x0)
        for y in range(y0, y1):
            off = (y * self.w + x0) * 3
            self.buf[off:off + len(row)] = row

    def text(self, x, y, s, color=TEXT, scale=1):
        cx = x
        for ch in s:
            glyph = GLYPHS.get(ch)
            if glyph is None:
                # 缺字画个实心块而不是留空：留空会让标题静默少字母，
                # 上一版就因此把 QWEN3_DYNAMIC 显示成 WEN3_D NAMIC
                self.rect(cx, y + scale, cx + GLYPH_W * scale,
                          y + (GLYPH_H - 1) * scale, color)
                cx += (GLYPH_W + 1) * scale
                continue
            for gy, line in enumerate(glyph):
                for gx, bit in enumerate(line):
                    if bit == "1":
                        self.rect(cx + gx * scale, y + gy * scale,
                                  cx + (gx + 1) * scale, y + (gy + 1) * scale, color)
            cx += (GLYPH_W + 1) * scale
        return cx

    def write_png(self, path):
        raw = bytearray()
        stride = self.w * 3
        for y in range(self.h):
            raw.append(0)  # 每行的 filter type，0 = None
            raw += self.buf[y * stride:(y + 1) * stride]

        def chunk(tag, data):
            return (struct.pack(">I", len(data)) + tag + data +
                    struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

        header = struct.pack(">IIBBBBB", self.w, self.h, 8, 2, 0, 0, 0)
        png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
               chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))
        with open(path, "wb") as f:
            f.write(png)


def render(data, width=1600, lane_h=7, gap=1, out="swimlane.png",
           used_only=False, from_us=None, to_us=None):
    tasks = data["tasks"]
    meta = data["meta"]
    if not tasks:
        print("no task records", file=sys.stderr)
        return None

    # 泳道按 (type, core) 排；每个核只画执行段，天然不重叠
    lanes = defaultdict(list)
    for rec in tasks:
        (_tid, core, typ, _slot, path, _blk,
         stage_ns, pub_ns, run_ns, end_ns, fin_ns) = rec
        lanes[(typ, core)].append((stage_ns, pub_ns, run_ns, end_ns, fin_ns, path))

    if used_only:
        keys = sorted(lanes)
    else:
        # 默认把整片硬件都画出来: 哪些核从头到尾没派到活, 是这张图第一眼要回答的问题
        keys = [(t, c) for t in range(meta["exe_type_cnt"])
                for c in range(meta["aic_cnt"])]

    # 原始 ns 是相对 swim_init 的, 头上还挂着建图和起线程的那一段。横轴和
    # --from-us/--to-us 都以"第一个任务开跑"为 0 点, 免得用户去猜那段偏移。
    base = min(r[8] for r in tasks)
    t_min = base if from_us is None else base + int(from_us * 1000)
    t_max = (max(max(r[9], r[10]) for r in tasks) if to_us is None
             else base + int(to_us * 1000))
    span = max(t_max - t_min, 1)

    pad_l, pad_r, pad_t, pad_b = 74, 16, 54, 34
    plot_w = width - pad_l - pad_r
    plot_h = len(keys) * (lane_h + gap)
    height = plot_h + pad_t + pad_b

    cv = Canvas(width, height)

    def x_of(ns):
        return pad_l + (ns - t_min) / span * plot_w

    def bar(x0, y0, x1, y1, color):
        """
        画一段泳道内容，横向裁到绘图区。
        缩放时窗口外的任务会算出负坐标，只靠画布裁剪会让它糊到左边的标签栏上。
        整段都在窗口外就直接不画，避免退化成贴边的一像素假条。
        """
        x0 = max(x0, pad_l)
        x1 = min(x1, pad_l + plot_w)
        if x1 <= pad_l or x0 >= pad_l + plot_w:
            return
        cv.rect(x0, y0, x1, y1, color)

    # 标题
    title = f"{meta['case']}  ED={meta['ed_enable']}  SCALE={meta['exec_duration_scale']}"
    cv.text(pad_l, 12, title.upper(), TEXT, 2)
    cv.text(pad_l, 32, f"{len(tasks)} TASKS   {span / 1000.0:.0f} us SPAN   "
                       f"{len(lanes)}/{len(keys)} CORES USED", (110, 116, 130), 1)

    # 时间刻度
    for i in range(6):
        ns = t_min + span * i / 5
        gx = x_of(ns)
        cv.rect(gx, pad_t - 4, gx + 1, pad_t + plot_h, (222, 225, 233))
        cv.text(gx + 2, height - pad_b + 8, f"{(ns - base) / 1000.0:.0f}us",
                (120, 126, 140), 1)

    # 泳道
    for i, key in enumerate(keys):
        typ, core = key
        y0 = pad_t + i * (lane_h + gap)
        y1 = y0 + lane_h
        cv.rect(pad_l, y0, pad_l + plot_w, y1,
                LANE_BG if i % 2 == 0 else LANE_BG_ALT)
        label = f"{'CUBE' if typ == 0 else 'VEC'}_{core:02d}"
        cv.text(4, y0 + max((lane_h - GLYPH_H) // 2, 0), label, (96, 102, 118), 1)

        run_color = C_RUN_CUBE if typ == 0 else C_RUN_VEC
        for stage_ns, pub_ns, run_ns, end_ns, fin_ns, _path in lanes[key]:
            # 细条画在泳道上下边缘，粗条(执行)占满，这样重叠也看得出层次
            if stage_ns:
                bar(x_of(stage_ns), y0, x_of(pub_ns), y0 + 2, C_GATED)
            if run_ns > pub_ns:
                bar(x_of(pub_ns), y0, x_of(run_ns), y0 + 2, C_WAIT)
            if fin_ns > end_ns:
                bar(x_of(end_ns), y1 - 2, x_of(fin_ns), y1, C_FIN)
            bar(x_of(run_ns), y0 + 2, x_of(end_ns), y1 - 2, run_color)

    # 图例
    ly = height - pad_b + 20
    lx = pad_l
    for color, name in ((C_RUN_CUBE, "RUN CUBE"), (C_RUN_VEC, "RUN VEC"),
                        (C_GATED, "ED GATED"), (C_WAIT, "WAIT CORE"),
                        (C_FIN, "FIN")):
        cv.rect(lx, ly, lx + 16, ly + 7, color)
        lx = cv.text(lx + 20, ly, name, (110, 116, 130), 1) + 14

    cv.write_png(out)
    return out, width, height


def main():
    ap = argparse.ArgumentParser(description="swimlane_records.json -> PNG 速览图")
    ap.add_argument("input")
    ap.add_argument("-o", "--output")
    ap.add_argument("--width", type=int, default=1600)
    ap.add_argument("--lane-height", type=int, default=7)
    ap.add_argument("--used-only", action="store_true",
                    help="只画派到过任务的核，默认把整片硬件都画出来")
    ap.add_argument("--from-us", type=float, help="时间窗起点 (us, 相对 t0)")
    ap.add_argument("--to-us", type=float, help="时间窗终点 (us, 相对 t0)")
    args = ap.parse_args()

    with open(args.input, "r", encoding="utf-8") as f:
        data = json.load(f)

    out = args.output or os.path.join(os.path.dirname(args.input) or ".",
                                      "swimlane.png")
    result = render(data, width=args.width, lane_h=args.lane_height, out=out,
                    used_only=args.used_only, from_us=args.from_us,
                    to_us=args.to_us)
    if result is None:
        return 1
    path, w, h = result
    print(f"{path}  ({w}x{h})")
    return 0


if __name__ == "__main__":
    sys.exit(main())

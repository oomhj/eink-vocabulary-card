#!/usr/bin/env python3
"""抓取串口日志：逐行带时间戳写 logs/serial_*.txt（t0=脚本启动时刻）。
用法: python3 tools/serial_log.py [时长秒]   # 默认 300s，0=一直跑直到 Ctrl-C
"""
import os, sys, time, serial
from datetime import datetime

PORT = "/dev/cu.wchusbserial11110"  # CH340（端口名随拔插变化，必要时改这里）
BAUD = 74880
DURATION = int(sys.argv[1]) if len(sys.argv) > 1 else 300

os.makedirs("logs", exist_ok=True)
stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
txt_path = f"logs/serial_{stamp}.txt"

ser = serial.Serial(PORT, BAUD, timeout=0.5)
t0 = time.time()
buf = bytearray()
with open(txt_path, "ab") as f:
    f.write(f"=== start {datetime.now().isoformat()} port={PORT} baud={BAUD} ===\n".encode())
    f.flush()
    try:
        while DURATION <= 0 or time.time() - t0 < DURATION:
            data = ser.read(4096)
            if not data:
                continue
            buf += data
            if b"\n" in buf:
                head, buf = buf.rsplit(b"\n", 1)
                try:
                    line = head.decode("utf-8", "replace")
                except Exception:
                    line = repr(head)
                rel = time.time() - t0
                f.write(f"[{rel:8.2f}] {line}\n".encode("utf-8", "replace"))
                f.flush()          # 立即落盘（txt 不再缓冲）
                print(f"[{rel:8.2f}] {line}", flush=True)
    except KeyboardInterrupt:
        pass
    ser.close()
    if buf:
        f.write(f"[{time.time()-t0:8.2f}] {buf.decode('utf-8','replace')}\n".encode())
    f.write(f"=== end {datetime.now().isoformat()} ===\n".encode())
print(f"saved: {txt_path}")

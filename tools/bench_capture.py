"""bench_capture.py - log a Z80 core benchmark run (bench_* envs) to a file.

The board reboots between games, which drops the USB serial port; this
reconnects automatically (RP2350 USB VID 0x2E8A), keeps every [BENCH]
line, and exits after "[BENCH] DONE".

    python tools/bench_capture.py bench_marat.log
"""
import sys
import time

import serial
import serial.tools.list_ports

VID = 0x2E8A


def find_port():
    for p in serial.tools.list_ports.comports():
        if p.vid == VID:
            return p.device
    return None


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "bench.log"
    print(f"logging [BENCH] lines to {out_path}; waiting for the board...")
    with open(out_path, "a", encoding="utf-8") as out:
        while True:
            port = find_port()
            if not port:
                time.sleep(0.5)
                continue
            try:
                with serial.Serial(port, 115200, timeout=1) as ser:
                    print(f"connected {port}")
                    while True:
                        line = ser.readline().decode("utf-8", "replace").strip()
                        if "[BENCH]" not in line:
                            continue
                        print(line)
                        out.write(line + "\n")
                        out.flush()
                        if "[BENCH] DONE" in line:
                            return
            except (serial.SerialException, OSError):
                print("disconnected (reboot), reconnecting...")
                time.sleep(0.5)


if __name__ == "__main__":
    main()

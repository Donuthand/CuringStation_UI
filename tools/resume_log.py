"""Reconnect to a station that is already running, WITHOUT resetting it, and keep logging.

    python resume_log.py COM3 run_part2.csv

Same output format as `temp_log.py record`, so `temp_log.py plot` reads the result. The difference:
DTR and RTS are set low BEFORE the port opens, so the ESP32's auto-reset circuit is not pulsed and
a cure in progress keeps running. Written 2026-10-07 for the T26 run (the first copy lived in a
session scratchpad and was lost). Serial Monitor must be closed.
"""

import sys
import time

import serial  # pyserial


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    port, out_path = sys.argv[1], sys.argv[2]
    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = port, 115200, 1
    ser.dtr = False
    ser.rts = False
    ser.open()
    print(f"Resumed {port} to {out_path} (DTR/RTS off) - Ctrl+C to stop.")
    with ser, open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"# resumed {time.strftime('%Y-%m-%d %H:%M:%S')} from {port} (DTR/RTS off, no reset intended)\n")
        f.write("time_s,control_C,ssr,setpoint_C,free_heap_B,min_free_heap_B\n")
        try:
            while True:
                raw = ser.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                print(line)
                f.write(line + "\n")
                f.flush()  # every line on disk at once
        except KeyboardInterrupt:
            print("\nStopped.")


if __name__ == "__main__":
    main()

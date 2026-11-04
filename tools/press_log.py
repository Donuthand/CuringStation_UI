"""Log the station's serial output with a laptop timestamp on every line (remote-button tests T18/T19).
Rebuilt 2026-10-26 (the 10-16 presswatch.py lived only in a session scratchpad and was lost).

    python press_log.py COM12 ..\..\..\Paper\Data\2026-10-26_T18_station_presses.log

Opens the port with DTR/RTS held off, so the ESP32 is NOT reset and running cures keep going.
Every `[RemoteStop] heard packet #N` line is also counted on screen. Ctrl+C to stop.
Needs pyserial; close the Serial Monitor first (one program per port).
"""
import sys
import time

import serial  # pyserial


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: python press_log.py COMx out.log")
    port, out_path = sys.argv[1], sys.argv[2]
    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = port, 115200, 1
    ser.dtr = False
    ser.rts = False
    ser.open()
    heard = 0
    print(f"Logging {port} to {out_path} - Ctrl+C to stop.")
    with ser, open(out_path, "a", encoding="utf-8", newline="\n") as f:
        f.write(f"# recorded {time.strftime('%Y-%m-%d %H:%M:%S')} from {port}\n")
        try:
            while True:
                raw = ser.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").rstrip()
                stamp = time.strftime("%H:%M:%S") + f".{int(time.time() * 1000) % 1000:03d}"
                f.write(f"{stamp} {line}\n")
                f.flush()
                if "heard packet" in line:
                    heard += 1
                    print(f"{stamp}  HEARD #{heard}")
                elif "STOP_ALARM" in line or "[RemoteStop]" in line:
                    print(f"{stamp}  {line}")
        except KeyboardInterrupt:
            print(f"\nStopped. {heard} packets heard.")


if __name__ == "__main__":
    main()

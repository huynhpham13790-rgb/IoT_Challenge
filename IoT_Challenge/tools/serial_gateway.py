#!/usr/bin/env python3
"""
Fallback gateway: read telemetry from the board over USB-serial and push it
straight to the HIS Server.

The system's NORMAL path is:
    board -> Zigbee -> NCP -> zigbee2mqtt -> MQTT -> gateway_test -> TCP -> server
with everything in the middle running on a Raspberry Pi.

When the Pi is unavailable, this script replaces that whole middle section with
a much shorter route:
    board -> USB-serial (VCOM) -> this script -> TCP -> HIS Server

The firmware prints one `[JSON]{...}` line per AI cycle (1 second) on VCOM; this
script only adds bedId/room and forwards it. The field names already match the
zigbee2mqtt payload, so nothing on the server needs changing.

Known limitation: this path BYPASSES Zigbee entirely, so it exercises nothing of
the Zigbee/NCP/pairing stack. It is meant for viewing the dashboard and checking
the AI + server logic - it does not replace a real end-to-end test with the Pi.

Usage:
    python3 tools/serial_gateway.py --port /dev/ttyACM1 --server 127.0.0.1 --tcp-port 5000
"""

import argparse
import glob
import json
import os
import socket
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is missing. Install it with: pip install pyserial")

PREFIX = "[JSON]"

# Serial number printed on the sensor board's J-Link OB. Used to find the port
# by identity rather than by number.
DEFAULT_BOARD_SERIAL = "440364712"


def resolve_port(explicit_port, serial_number):
    """Find the sensor board's tty, preferring identity over the port number.

    /dev/ttyACM* numbering is assigned in enumeration order, so it is NOT stable:
    unplugging the boards, replugging them in a different order, or one of them
    re-enumerating is enough to swap ttyACM0 and ttyACM1. That has already
    happened twice here, and the failure is quiet and confusing - the gateway
    happily reads the NCP's port, sees no [JSON] lines, and the dashboard simply
    stops updating with no error anywhere.

    /dev/serial/by-id/ contains a symlink per device named after its USB serial
    number, which never changes. Prefer that; fall back to the explicit port only
    when the serial number cannot be matched.
    """
    if serial_number:
        matches = glob.glob(f"/dev/serial/by-id/*{serial_number}*")
        if matches:
            resolved = os.path.realpath(matches[0])
            print(f"[GW] Board serial {serial_number} -> {resolved}", flush=True)
            return resolved
        print(f"[GW] WARNING: no device with serial {serial_number} under "
              f"/dev/serial/by-id - falling back to {explicit_port}", flush=True)
    return explicit_port


class ServerLink:
    """TCP link to the HIS Server that reconnects itself when dropped.

    The server just reads newline-terminated JSON lines
    (BedTcpIngestionService), so there is no handshake to perform here.
    """

    def __init__(self, host, port):
        self.host = host
        self.port = port
        self.sock = None

    def _connect(self):
        s = socket.create_connection((self.host, self.port), timeout=5)
        s.settimeout(5)
        self.sock = s
        print(f"[GW] Connected to HIS Server {self.host}:{self.port}", flush=True)

    def send_line(self, line: str) -> bool:
        for attempt in (1, 2):
            try:
                if self.sock is None:
                    self._connect()
                self.sock.sendall((line + "\n").encode())
                return True
            except (OSError, socket.timeout) as e:
                # First attempt: reconnect and retry. If the second attempt
                # also fails, drop this packet but do NOT exit - losing the
                # network is routine, and later packets will get through once
                # the server is back.
                print(f"[GW] Send failed ({e}) - reconnecting", flush=True)
                try:
                    if self.sock:
                        self.sock.close()
                except OSError:
                    pass
                self.sock = None
                if attempt == 2:
                    return False
                time.sleep(1.0)
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0",
                    help="fallback serial port, only used if --serial finds nothing")
    ap.add_argument("--serial", default=DEFAULT_BOARD_SERIAL,
                    help="USB serial number of the SENSOR BOARD (not the NCP); "
                         "resolved via /dev/serial/by-id. Pass an empty string to "
                         "use --port directly.")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--server", default="127.0.0.1")
    ap.add_argument("--tcp-port", type=int, default=5000)
    ap.add_argument("--bed-id", default="BED-101")
    ap.add_argument("--room", default="ICU-1")
    ap.add_argument("--quiet", action="store_true",
                    help="print every 10th packet instead of every one")
    args = ap.parse_args()

    port = resolve_port(args.port, args.serial)
    print(f"[GW] Reading {port} @ {args.baud} -> "
          f"{args.server}:{args.tcp_port} (bed={args.bed_id}, room={args.room})",
          flush=True)

    link = ServerLink(args.server, args.tcp_port)

    # pyserial must NOT assert DTR/RTS when opening the port: on Silabs boards
    # (and most boards with a J-Link OB) DTR is wired to the reset pin, so every
    # open RESETS THE CHIP. The effect is very easy to misdiagnose: starting the
    # gateway wipes the forecaster's 64-second window, so the dashboard keeps
    # reporting "filling the window" forever while the raw serial log clearly
    # shows the model running perfectly well.
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = args.baud
    ser.timeout = 2
    ser.dtr = False
    ser.rts = False
    ser.open()

    sent = failed = bad = 0
    while True:
        try:
            raw = ser.readline()
        except serial.SerialException as e:
            print(f"[GW] Lost the serial port ({e}) - board unplugged? Exiting.", flush=True)
            return 1

        if not raw:
            continue

        text = raw.decode(errors="replace").strip()
        if PREFIX not in text:
            continue

        payload = text[text.index(PREFIX) + len(PREFIX):]
        try:
            data = json.loads(payload)
        except json.JSONDecodeError:
            # Line truncated by an interleaved printf -> skip it; the next
            # packet will be fine.
            bad += 1
            continue

        data["bedId"] = args.bed_id
        data["room"] = args.room

        if link.send_line(json.dumps(data, separators=(",", ":"))):
            sent += 1
        else:
            failed += 1

        if not args.quiet or sent % 10 == 0:
            hr = data.get("heart_rate")
            sp = data.get("spo2")
            print(f"[GW] #{sent} HR={hr if hr is not None else '--'} "
                  f"SpO2={sp if sp is not None else '--'} "
                  f"drop={data.get('drop_rate')}% dpm={data.get('drops_per_min')} "
                  f"w={data.get('weight_g')}g alarm={data.get('alarm')} "
                  f"| failed={failed} bad_json={bad}", flush=True)


if __name__ == "__main__":
    sys.exit(main() or 0)

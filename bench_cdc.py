#!/usr/bin/env python3
"""
Interactive / Streaming Throughput Benchmark on USB CDC Port.
Useful for testing throughput against a Smartphone, Tablet, or Web Bluetooth.

Modes:
  1. RX Mode: Listens on /dev/ttyACM* and measures incoming BLE->USB throughput in KB/s.
  2. TX Mode: Streams high-speed data from USB->BLE at maximum pace.
  3. Echo Mode: Bounces back all received bytes (measure round-trip speed).
"""

import sys
import os
import time
import glob
import argparse
import serial

def find_port():
    for p in sorted(glob.glob("/dev/ttyACM*")):
        try:
            sys_path = os.path.realpath(f"/sys/class/tty/{os.path.basename(p)}/device")
            uevent = os.path.join(sys_path, "uevent")
            if os.path.exists(uevent):
                with open(uevent, "r") as f:
                    if "1a86" in f.read():
                        return p
        except Exception:
            pass
    if os.path.exists("/dev/ttyACM2"):
        return "/dev/ttyACM2"
    devs = glob.glob("/dev/ttyACM*")
    return devs[0] if devs else None

def run_rx_benchmark(ser):
    print("\n--- [RX BENCHMARK: BLE Client -> CH592 -> USB CDC] ---")
    print("Start sending text or files from your phone/BLE client now.")
    print("Press Ctrl+C to stop.\n")

    total_bytes = 0
    start_time = None
    last_report_time = time.time()
    last_report_bytes = 0

    try:
        while True:
            n = ser.in_waiting
            if n > 0:
                data = ser.read(n)
                now = time.time()
                if start_time is None:
                    start_time = now
                    last_report_time = now
                total_bytes += len(data)

                # Report stats every 1.0 second
                if now - last_report_time >= 1.0:
                    dt = now - last_report_time
                    inst_kb = ((total_bytes - last_report_bytes) / 1024.0) / dt
                    inst_kbps = inst_kb * 8.0
                    avg_dt = now - start_time
                    avg_kb = (total_bytes / 1024.0) / avg_dt if avg_dt > 0 else 0
                    print(f"  [Speed] Current: {inst_kb:6.2f} KB/s ({inst_kbps:6.1f} kbps) | "
                          f"Average: {avg_kb:6.2f} KB/s | Total: {total_bytes / 1024.0:6.1f} KB")
                    last_report_time = now
                    last_report_bytes = total_bytes
            else:
                time.sleep(0.005)
    except KeyboardInterrupt:
        print("\n\nBenchmark stopped.")
        if start_time and total_bytes > 0:
            elapsed = time.time() - start_time
            rate_kb = (total_bytes / 1024.0) / elapsed
            print(f"Total Bytes : {total_bytes} bytes ({total_bytes / 1024.0:.2f} KB)")
            print(f"Duration    : {elapsed:.2f} s")
            print(f"Avg Speed   : {rate_kb:.2f} KB/s ({rate_kb * 8.0:.1f} kbps)")

def run_tx_benchmark(ser, total_kb=64, chunk_size=244):
    print(f"\n--- [TX BENCHMARK: USB CDC -> CH592 -> BLE Client] ---")
    print(f"Sending {total_kb} KB in {chunk_size}-byte chunks over USB CDC...")
    print("Ensure Notifications are ENABLED in your BLE app!\n")

    payload = bytes([i % 256 for i in range(chunk_size)])
    total_bytes = total_kb * 1024
    chunks = total_bytes // chunk_size
    sent_bytes = 0

    t0 = time.perf_counter()
    for c in range(chunks):
        ser.write(payload)
        sent_bytes += len(payload)

    ser.flush()
    t1 = time.perf_counter()
    elapsed = t1 - t0
    rate_kb = (sent_bytes / 1024.0) / elapsed if elapsed > 0 else 0

    print(f"Finished sending!")
    print(f"Sent Bytes  : {sent_bytes} bytes ({sent_bytes / 1024.0:.2f} KB)")
    print(f"Duration    : {elapsed:.2f} s")
    print(f"USB->BLE Rate: {rate_kb:.2f} KB/s ({rate_kb * 8.0:.1f} kbps)")

def main():
    parser = argparse.ArgumentParser(description="CH592 USB CDC Throughput Tool")
    parser.add_argument("--port", default=None, help="Serial port (e.g. /dev/ttyACM2)")
    parser.add_argument("--mode", choices=["rx", "tx", "reboot"], default="rx",
                        help="Benchmark mode: 'rx' (receive from BLE), 'tx' (send to BLE), 'reboot' (1200 baud reset)")
    parser.add_argument("--size", type=int, default=64, help="Total KB to send in TX mode")
    args = parser.parse_args()

    port = args.port or find_port()
    if not port:
        print("[ERROR] No /dev/ttyACM* port found!")
        sys.exit(1)

    if args.mode == "reboot":
        print(f"Triggering 1200 bps touch-to-reset on {port}...")
        ser = serial.Serial(port, baudrate=1200)
        ser.close()
        print("Reset triggered!")
        return

    print(f"Opening {port} at 115200 bps...")
    ser = serial.Serial(port, baudrate=115200, timeout=0.1)
    ser.reset_input_buffer()
    ser.reset_output_buffer()

    if args.mode == "rx":
        run_rx_benchmark(ser)
    elif args.mode == "tx":
        run_tx_benchmark(ser, total_kb=args.size)

    ser.close()

if __name__ == "__main__":
    main()

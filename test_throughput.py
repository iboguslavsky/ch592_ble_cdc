#!/usr/bin/env python3
"""
Automated Throughput & Latency Benchmark for CH592 BLE to USB CDC Bridge.
Connects to CH592 over BLE (via Bleak) and over USB CDC (via pyserial)
simultaneously, then benchmarks throughput in both directions.
"""

import os
import sys
# Force /usr/bin before /usr/local/bin to use system BlueZ (5.64) instead of legacy 5.9
os.environ["PATH"] = "/usr/bin:" + os.environ.get("PATH", "")

import time
import glob
import asyncio
import subprocess
import serial
from bleak import BleakScanner, BleakClient

NUS_SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9F"
NUS_RX_CHAR_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9F"  # Central -> Peripheral (Write)
NUS_TX_CHAR_UUID = "6E400003-B5A3-F393-E0A9-E50E24DCCA9F"  # Peripheral -> Central (Notify)

def find_ch592_serial():
    """Auto-detect the CH592 USB CDC ACM device path."""
    for port in sorted(glob.glob("/dev/ttyACM*")):
        try:
            sys_path = os.path.realpath(f"/sys/class/tty/{os.path.basename(port)}/device")
            udev_uevent = os.path.join(sys_path, "uevent")
            if os.path.exists(udev_uevent):
                with open(udev_uevent, "r") as f:
                    content = f.read()
                    if "1a86" in content or "WCH" in content:
                        return port
        except Exception:
            pass
    if os.path.exists("/dev/ttyACM2"):
        return "/dev/ttyACM2"
    candidates = glob.glob("/dev/ttyACM*")
    return candidates[0] if candidates else None

def check_serial_conflict(port):
    """Check if another process (like cu/picocom/minicom) has the serial port open."""
    try:
        res = subprocess.run(["fuser", port], capture_output=True, text=True)
        pids = res.stdout.strip().split()
        if pids:
            return pids
    except Exception:
        pass
    return []

async def run_benchmark():
    port = find_ch592_serial()
    if not port:
        print("[ERROR] No /dev/ttyACM* serial port found!")
        print("        Ensure CH592 is plugged in and recognized.")
        sys.exit(1)

    conflicting_pids = check_serial_conflict(port)
    if conflicting_pids:
        print(f"[WARNING] Serial port {port} is currently opened by process(es): {' '.join(conflicting_pids)}")
        print("          If you have 'cu', 'picocom', or 'minicom' open in another terminal,")
        print(f"          please close it so this benchmark has exclusive access to {port}.")
        print("          Continuing in 2 seconds...")
        time.sleep(2.0)

    print(f"[INFO] Using CH592 USB serial port: {port}")
    print("[INFO] Scanning for BLE device 'CH592_BLE_CDC'...")

    device = None
    try:
        devices = await BleakScanner.discover(timeout=5.0)
        for d in devices:
            if d.name and "CH592_BLE_CDC" in d.name:
                device = d
                break
    except Exception as e:
        print(f"[ERROR] BLE scan failed: {e}")
        sys.exit(1)

    if not device:
        print("[ERROR] Could not find 'CH592_BLE_CDC' over BLE.")
        print("        NOTE: The CH592 peripheral only accepts ONE active BLE connection.")
        print("        If your phone or tablet is currently connected, please DISCONNECT it")
        print("        in Serial Bluetooth Terminal / nRF Connect so Linux can connect.")
        sys.exit(1)

    print(f"[INFO] Found {device.name} [{device.address}]")
    print("[INFO] Connecting over BLE...")

    try:
        client = BleakClient(device, timeout=10.0)
        await client.connect()
    except Exception as e:
        print(f"[ERROR] BLE connection failed: {e}")
        print("        Ensure no phone is connected to CH592_BLE_CDC and try again.")
        sys.exit(1)

    try:
        print(f"[INFO] Connected to BLE peripheral! Negotiated MTU size: {client.mtu_size}")

        try:
            ser = serial.Serial(port, baudrate=115200, timeout=0.1)
        except serial.SerialException as e:
            print(f"[ERROR] Could not open {port}: {e}")
            print("        Please close any terminal program (cu/picocom) using this port.")
            await client.disconnect()
            sys.exit(1)

        ser.reset_input_buffer()
        ser.reset_output_buffer()
        time.sleep(0.1)

        # ---------------------------------------------------------
        # TEST 1: Round-Trip Latency (Ping-Pong)
        # ---------------------------------------------------------
        print("\n" + "="*55)
        print(" TEST 1: Round-Trip Latency (Ping-Pong)")
        print("="*55)

        noti_queue = asyncio.Queue()
        def on_notify(sender, data):
            noti_queue.put_nowait(data)

        await client.start_notify(NUS_TX_CHAR_UUID, on_notify)
        await asyncio.sleep(0.1)

        rtt_samples = []
        for i in range(10):
            ser.reset_input_buffer()
            while not noti_queue.empty():
                noti_queue.get_nowait()

            ping_data = f"PING_{i:04d}\n".encode()
            t0 = time.perf_counter()

            # Write to BLE RX characteristic (Central -> CH592)
            await client.write_gatt_char(NUS_RX_CHAR_UUID, ping_data, response=False)

            # Read from USB CDC serial port
            deadline = time.time() + 1.0
            rx_buf = b""
            while time.time() < deadline:
                chunk = ser.read(ser.in_waiting or 1)
                if chunk:
                    rx_buf += chunk
                    if ping_data in rx_buf:
                        break
                await asyncio.sleep(0.001)

            t1 = time.perf_counter()
            if ping_data in rx_buf:
                rtt_ms = (t1 - t0) * 1000.0
                rtt_samples.append(rtt_ms)
                print(f"  Ping #{i+1:02d}: RTT = {rtt_ms:6.2f} ms")
            else:
                print(f"  Ping #{i+1:02d}: TIMEOUT (rx={rx_buf})")

        if rtt_samples:
            avg_rtt = sum(rtt_samples) / len(rtt_samples)
            print(f"\n  Latency -> Min: {min(rtt_samples):.2f} ms | Avg: {avg_rtt:.2f} ms | Max: {max(rtt_samples):.2f} ms")

        # ---------------------------------------------------------
        # TEST 2: BLE -> USB CDC Throughput
        # ---------------------------------------------------------
        print("\n" + "="*55)
        print(" TEST 2: Throughput [BLE Central -> USB CDC]")
        print("="*55)
        ser.reset_input_buffer()

        payload_size = max(20, min(client.mtu_size - 3, 240))
        total_test_bytes = 32 * 1024  # 32 KB
        test_payload = bytes([i % 256 for i in range(payload_size)])
        num_chunks = total_test_bytes // payload_size
        actual_to_send = num_chunks * payload_size

        print(f"  Streaming {actual_to_send / 1024:.1f} KB in {num_chunks} packets ({payload_size} bytes/packet)...")

        received_bytes = 0
        stop_reader = False

        def usb_reader():
            nonlocal received_bytes
            while not stop_reader:
                waiting = ser.in_waiting
                if waiting > 0:
                    data = ser.read(waiting)
                    received_bytes += len(data)
                else:
                    time.sleep(0.001)

        import threading
        reader_thread = threading.Thread(target=usb_reader, daemon=True)
        reader_thread.start()

        t_start = time.perf_counter()
        for c in range(num_chunks):
            await client.write_gatt_char(NUS_RX_CHAR_UUID, test_payload, response=False)
            # Pace to avoid flooding BlueZ DBus socket queue
            if (c % 6) == 0:
                await asyncio.sleep(0.005)

        deadline = time.time() + 4.0
        while received_bytes < actual_to_send and time.time() < deadline:
            await asyncio.sleep(0.02)

        t_end = time.perf_counter()
        stop_reader = True
        reader_thread.join(timeout=0.5)

        elapsed = t_end - t_start
        rate_kbs = (received_bytes / 1024.0) / elapsed if elapsed > 0 else 0
        rate_kbps = rate_kbs * 8.0

        print(f"  Sent     : {actual_to_send} bytes")
        print(f"  Received : {received_bytes} bytes ({(received_bytes/actual_to_send)*100:.1f}%)")
        print(f"  Duration : {elapsed:.2f} s")
        print(f"  Speed    : {rate_kbs:.2f} KB/s ({rate_kbps:.1f} kbps)")

        # ---------------------------------------------------------
        # TEST 3: USB CDC -> BLE Throughput
        # ---------------------------------------------------------
        print("\n" + "="*55)
        print(" TEST 3: Throughput [USB CDC -> BLE Central]")
        print("="*55)

        ble_rx_bytes = 0
        while not noti_queue.empty():
            noti_queue.get_nowait()

        def on_test_notify(sender, data):
            nonlocal ble_rx_bytes
            ble_rx_bytes += len(data)

        await client.stop_notify(NUS_TX_CHAR_UUID)
        await client.start_notify(NUS_TX_CHAR_UUID, on_test_notify)
        await asyncio.sleep(0.1)

        usb_payload = bytes([i % 256 for i in range(payload_size)])
        total_usb_bytes = 32 * 1024
        usb_chunks = total_usb_bytes // payload_size
        actual_usb_send = usb_chunks * payload_size

        print(f"  Streaming {actual_usb_send / 1024:.1f} KB in {usb_chunks} packets ({payload_size} bytes/packet)...")

        t_start = time.perf_counter()
        for c in range(usb_chunks):
            ser.write(usb_payload)
            # Pacing to align with BLE radio connection intervals
            if (c % 6) == 0:
                ser.flush()
                await asyncio.sleep(0.005)

        ser.flush()
        deadline = time.time() + 4.0
        while ble_rx_bytes < actual_usb_send and time.time() < deadline:
            await asyncio.sleep(0.02)

        t_end = time.perf_counter()
        elapsed = t_end - t_start
        rate_kbs = (ble_rx_bytes / 1024.0) / elapsed if elapsed > 0 else 0
        rate_kbps = rate_kbs * 8.0

        print(f"  Sent     : {actual_usb_send} bytes")
        print(f"  Received : {ble_rx_bytes} bytes ({(ble_rx_bytes/actual_usb_send)*100:.1f}%)")
        print(f"  Duration : {elapsed:.2f} s")
        print(f"  Speed    : {rate_kbs:.2f} KB/s ({rate_kbps:.1f} kbps)")

        ser.close()
        await client.disconnect()

        print("\n" + "="*55)
        print(" BENCHMARK COMPLETED SUCCESSFULLY")
        print("="*55)

    except Exception as e:
        print(f"[ERROR] Benchmark failed: {e}")
        try:
            await client.disconnect()
        except Exception:
            pass

if __name__ == "__main__":
    asyncio.run(run_benchmark())

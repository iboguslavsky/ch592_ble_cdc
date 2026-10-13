# CH592 BLE to USB CDC UART Bridge

A transparent, high-throughput bidirectional bridge between **Bluetooth Low Energy (BLE)** and **USB CDC ACM (Virtual Serial COM Port)** for the **WCH CH592** microcontroller.

Designed specifically for minimalist CH592 boards (such as the [biemster/ch592-nano](https://oshwlab.com/biemster/ch592-nano) board) that **do not expose hardware UART pins** and only have USB D+/D- and BOOT (PB22) available.

```
+------------------+          USB (Bulk EP)          +--------------------+
|  Host Computer   | <=============================> |   WCH CH592 MCU    |
| (/dev/ttyACM*)   |                                 | (CDC ACM + Nordic) |
+------------------+                                 +--------------------+
                                                               ^
                                                               | BLE 2.4GHz
                                                               | (Nordic NUS)
                                                               v
                                                     +--------------------+
                                                     | Smartphone / PC /  |
                                                     | BLE Central Client |
                                                     +--------------------+
```

---

## Features

- **Standard Nordic UART Service (NUS)**:
  - Universally recognized by mobile apps (nRF Connect, Serial Bluetooth Terminal) and Web Bluetooth without custom client drivers.
  - Supports both `WRITE` (with acknowledgment) and `WRITE_NO_RSP` (maximum streaming throughput) on the RX characteristic.
  - TX characteristic uses GATT Notifications with dynamic payload sizing up to negotiated MTU (23 to 247 bytes).
- **Native USB CDC ACM**:
  - Full-Speed USB (12 Mbps PHY) running zero-wait-state from SRAM (`.highcode`).
  - Native driver support on Linux (`/dev/ttyACM*`), macOS (`/dev/cu.usbmodem*`), and Windows (`usbser.sys`).
- **100% Transparent Data Stream**:
  - Zero protocol encapsulation, zero escaping, zero overhead.
  - Bytes sent from the USB serial terminal are immediately transmitted over BLE notifications.
  - Bytes received over BLE are immediately written to USB CDC endpoints.
- **On-Demand Diagnostics via BOOT Button (PB22)**:
  - Momentarily grounding Pad 2 (PB22) prints a non-intrusive status report to the USB serial terminal: connection state, negotiated MTU, MAC address, and bridged byte counters.
- **Auto-Advertising & Reconnect**:
  - Device advertises automatically as `CH592_BLE_CDC` on boot and immediately restarts advertising upon disconnection.

---

## Nordic UART Service (NUS) Specifications

The bridge exposes the industry-standard Nordic UART Service 128-bit UUIDs:

| Attribute | UUID | Properties | Description |
|---|---|---|---|
| **Service** | `6E400001-B5A3-F393-E0A9-E50E24DCCA9F` | Primary Service | Nordic UART Service |
| **RX Characteristic** | `6E400002-B5A3-F393-E0A9-E50E24DCCA9F` | `WRITE`, `WRITE_WITHOUT_RESPONSE` | Central -> CH592 (Data sent to USB CDC) |
| **TX Characteristic** | `6E400003-B5A3-F393-E0A9-E50E24DCCA9F` | `NOTIFY` | CH592 -> Central (Data received from USB CDC) |
| **TX CCCD** | `00002902-0000-1000-8000-00805F9B34FB` | `READ`, `WRITE` | Client Characteristic Configuration Descriptor |

---

## Physical Pad Pinout (CH592-Nano Board)

| Position (Front View, Left-to-Right) | Net / Silkscreen | CH592 Pin | USB Cable Wire / Connection |
|:---:|:---:|:---:|---|
| **Pad 1 (Far Left)** | `GND` | Pin 29 (EPAD) | **USB GND** (Black) & Power Supply GND |
| **Pad 2** | `BOOT` | Pin 17 (PB22) | **Bridge to GND while plugging in to flash**; tap to print link status |
| **Pad 3 (Center)** | `USBD-` | Pin 13 (PB10) | **USB D-** (White) |
| **Pad 4** | `USBD+` | Pin 12 (PB11) | **USB D+** (Green) |
| **Pad 5 (Far Right)** | `3v3` | Pin 5 (VIO33/VDD33) | **External +3.3V Power** *(Do NOT connect 5V USB VBUS!)* |

> [!CAUTION]
> The CH592 operates on **3.3V**. Never connect 5V USB VBUS directly to Pad 5! USB carries D+, D-, and GND only.

---

## Quick Start

### 1. Build

```bash
cd ch592_ble_cdc
make
```

Outputs in `build/`:
- `build/ch592_ble_cdc.bin`
- `build/ch592_ble_cdc.hex`
- `build/ch592_ble_cdc.elf`

### 2. Flash to CH592

1. Bridge **Pad 2 (BOOT)** to **Pad 1 (GND)**.
2. Connect USB to PC (or power on).
3. Execute the flash script:
   ```bash
   ./flash.sh
   ```
   *(or `wchisp flash -V build/ch592_ble_cdc.bin`)*
4. Remove the BOOT bridge. The device boots immediately.

---

## Connecting & Testing

### 1. Host USB Connection
Open your favorite serial terminal program at any baud rate (e.g. 115200):

```bash
picocom -b 115200 /dev/ttyACM0
```
*(or `tio /dev/ttyACM0`, `minicom -D /dev/ttyACM0`)*

### 2. BLE Central Connection

#### Option A: Android / iOS (Serial Bluetooth Terminal)
1. Install **Serial Bluetooth Terminal** by Kai Morich from Google Play.
2. In the app menu, open **Devices** -> select **Bluetooth LE**.
3. Tap **Scan** and choose **CH592_BLE_CDC**.
4. Tap **Connect**. The app automatically detects the Nordic NUS service.
5. Anything typed in the app appears on your host terminal `/dev/ttyACM0`, and anything typed into `picocom` appears on the phone screen!

#### Option B: nRF Connect (Android / iOS / Desktop)
1. Scan for devices and connect to **CH592_BLE_CDC**.
2. Expand the `Nordic UART Service` (`6E400001-...`).
3. Tap the triple-arrow icon on characteristic `6E400003-...` to **Enable Notifications**.
4. Tap the upload arrow on characteristic `6E400002-...` to send text or hex data to USB.

#### Option C: Python (`bleak`)
```python
import asyncio
from bleak import BleakClient, BleakScanner

UART_SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9F"
UART_RX_CHAR_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9F"
UART_TX_CHAR_UUID = "6E400003-B5A3-F393-E0A9-E50E24DCCA9F"

def notification_handler(sender, data):
    print("From USB:", data.decode(errors="replace"))

async def main():
    device = await BleakScanner.find_device_by_name("CH592_BLE_CDC")
    if not device:
        print("Device not found")
        return
    async with BleakClient(device) as client:
        print("Connected!")
        await client.start_notify(UART_TX_CHAR_UUID, notification_handler)
        await client.write_gatt_char(UART_RX_CHAR_UUID, b"Hello from Python over BLE!\r\n")
        await asyncio.sleep(5)

asyncio.run(main())
```

---

## Diagnostics Button (PB22)

Because there are no other buttons or LEDs on the nano board, **Pad 2 (PB22)** functions as an on-demand status probe during runtime:

Momentarily short Pad 2 to Pad 1 (GND) while connected to `/dev/ttyACM0`. The bridge outputs a formatted diagnostics block:

```text
--- [CH592 BLE-CDC Bridge Status] ---
 BLE Link : CONNECTED (Handle 0x0000, MTU 247, Noti ON)
 USB CDC  : Terminal Connected
 Device   : CH592_BLE_CDC (84:C2:E4:01:23:45)
 Bridged  : USB->BLE: 1048 B | BLE->USB: 892 B (Clicks: 1)
-------------------------------------
```

When disconnected from BLE:
```text
--- [CH592 BLE-CDC Bridge Status] ---
 BLE Link : ADVERTISING (Nordic NUS: 6E400001-...)
 USB CDC  : Terminal Connected
 Device   : CH592_BLE_CDC (84:C2:E4:01:23:45)
 Bridged  : USB->BLE: 0 B | BLE->USB: 0 B (Clicks: 2)
-------------------------------------
```

---

---

## Performance & Throughput Benchmarks

The bridge supports **LE 2M PHY (2 Mbps)** with full iOS and Android compatibility.

### Real-World Test Results

| Parameter | LE 1M PHY (Default) | LE 2M PHY (Negotiated) | Improvement |
|:---|:---:|:---:|:---:|
| **Physical Bitrate** | 1.0 Mbps | 2.0 Mbps | 2.0× |
| **Negotiated MTU** | 247 bytes (244 B payload) | 247 bytes (244 B payload) | Identical |
| **Connection Interval** | 15.0 ms (Apple minimum) | 15.0 ms (Apple minimum) | Identical |
| **Packets per Event** | Up to 5 | Up to 10 | 2.0× |
| **Measured Application Throughput** | **83.54 KB/s** (668.4 kbps) | **145.24 KB/s** (**1161.9 kbps**) | **+73.8%** |
| **Theoretical Link Limit (15ms CI)** | 97.6 KB/s | 162.6 KB/s | — |
| **Bus Efficiency** | 85.6% | **89.3%** | High |

### Live Benchmark Run (`./bench_cdc.py --mode tx --size 256`)
```text
$ ./bench_cdc.py --mode tx --size 256
Opening /dev/ttyACM2 at 115200 bps...

--- [TX BENCHMARK: USB CDC -> CH592 -> BLE Client] ---
Sending 256 KB in 244-byte chunks over USB CDC...
Ensure Notifications are ENABLED in your BLE app!

Finished sending!
Sent Bytes  : 262056 bytes (255.91 KB)
Duration    : 1.76 s
USB->BLE Rate: 145.24 KB/s (1161.9 kbps)
```

### Live Diagnostics Report (Pad 2 / PB22 Pressed during Test)
```text
--- [CH592 BLE-CDC Bridge Status] ---
 BLE Link : CONNECTED (Handle 0x0001, MTU 247, Noti ON, PHY 2 Mbps)
 USB CDC  : Terminal Connected
 Device   : CH592_BLE_CDC (0C:3D:5E:5D:AD:75)
 Bridged  : USB->BLE: 262056 B | BLE->USB: 24 B (Clicks: 2)
-------------------------------------
```

### Running the Live Benchmark Tool (`bench_cdc.py`):
```bash
# Send 256 KB from USB CDC over BLE notifications to your phone/client:
./bench_cdc.py --mode tx --size 256

# Receive data streamed from BLE client to USB CDC:
./bench_cdc.py --mode rx

# Trigger software reboot into ISP Bootloader (via 1200-baud touch):
./bench_cdc.py --mode reboot
```

> [!NOTE]
> Make sure notifications are **ENABLED** on the TX characteristic (`6E400003-...`) in your BLE client app before running TX benchmark. Tap Pad 2 (PB22) to verify `Noti ON` and `PHY 2 Mbps`.

---

## Automated Loopback Benchmark (`test_throughput.py`)

You can also test bidirectional speed and latency directly from your Linux machine using the included [`test_throughput.py`](file:///home/igor/ch59x/ch592_ble_cdc/test_throughput.py) script.

The script connects simultaneously to the CH592 over **Bluetooth LE** (via `bleak`) and over **USB CDC** (via `/dev/ttyACM*`), and measures:
1. **Round-Trip Ping Latency** (BLE -> USB CDC -> Host, min / avg / max ms).
2. **BLE -> USB CDC Throughput** (streaming packets over GATT write-without-response).
3. **USB CDC -> BLE Throughput** (streaming packets over GATT notifications).

### How to Run:

1. **Disconnect your phone** from `CH592_BLE_CDC` (the CH592 only allows one active BLE link).
2. **Close your serial terminal** (`cu`, `picocom`, or `minicom`) so the script can access the serial port.
3. Run the benchmark:
   ```bash
   ./test_throughput.py
   ```

---

## Architecture & Source Code Layout

```
ch592_ble_cdc/
├── APP/
│   ├── bridge.c            # Bridge logic, BLE advertising, Nordic NUS callbacks & event loop
│   ├── bridge_main.c       # main() entry point & high-frequency circulation loop
│   ├── usb_cdc.c           # High-speed native USB CDC ACM device driver
│   └── include/
│       ├── bridge.h        # Bridge configuration & function prototypes
│       └── usb_cdc.h       # USB CDC driver interface
├── Profile/
│   ├── ble_uart_service.c  # Nordic UART Service GATT attribute table & handlers
│   └── include/
│       └── ble_uart_service.h
├── HAL/                    # Hardware Abstraction Layer (MCU clock, RTC, Sleep)
├── LIB/                    # WCH BLE Controller and Host stack library (LIBCH59xBLE.a)
├── StdPeriphDriver/        # CH59x standard peripheral drivers (GPIO, Flash, SysTick, etc.)
├── Makefile                # Complete build script targeting riscv-none-elf-gcc
├── flash.sh                # Automated ISP flash script using wchisp
└── test_throughput.py      # Automated loopback throughput and latency benchmark
```

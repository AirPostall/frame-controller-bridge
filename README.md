# FrameControllerBridge · V4

Use the left and right Steam Frame controllers as one wired USB gamepad. An ESP32-S3 receives each hand over Bluetooth Low Energy (BLE) and sends gamepad input through its native USB port. A browser on the board’s temporary Wi-Fi network lets you learn buttons, calibrate touch sensitivity and save a profile for use with the browser closed.

The default USB layout targets the **wired 8BitDo Pro 3**. It provides two analog triggers and four extra button outputs, PL, PR, L4 and R4. Apple lists connected gamepads through its Game Controller framework. Recognition of the bridge on a physical Mac or iPad still needs to be verified after flashing. [Check host recognition](#check-host-recognition)

## Pair the firmware with your controllers

You need an ESP32-S3 board with native USB OTG, at least 2 MB of flash, a USB data cable, and the advertised Bluetooth address of each controller. The published firmware starts **unpaired**, and a fresh profile has **no touch calibrations**.

1. Download the project using **Code → Download ZIP** on GitHub and extract it, or clone the repository. Open the bundled [Set-up-controllers.html](Set-up-controllers.html) locally in a current Safari or Chrome browser.
2. Enter the two distinct BLE addresses in the **Left controller** and **Right controller** fields. Use six pairs of hexadecimal digits separated by colons. If you need to identify the addresses, wake one hand at a time and use a BLE scanner that displays advertised device addresses.
3. Select **8BitDo Pro 3 · wired compatibility test**. The other available layouts are **Standard · generic HID** and **Extended · original 16-bit sticks**.
4. Choose **Download paired firmware**. The page creates `frame_bridge-paired.bin` locally. Keep this file with your own setup. It contains the addresses you entered. To switch controller pairs or USB layouts, generate and flash a new application.

## Flash the ESP32-S3

Use a terminal on macOS, Windows or Linux with Python 3.10+ and esptool 5. If you need to install esptool, open a terminal in your unpacked release folder and create a virtual environment using the commands for your system. [Espressif installation guide](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/installation.html)

**macOS or Linux**

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install "esptool>=5,<6"
```

**Windows Command Prompt**

```bat
py -3 -m venv .venv
.venv\Scripts\activate.bat
python -m pip install "esptool>=5,<6"
```

Connect the board using a data cable. On a board with **BOOT** and **RESET** (sometimes labeled **EN**), hold BOOT, tap RESET and release BOOT to enter the serial downloader. Run the following in the same terminal. If several ports appear, compare the list with the board unplugged and again in downloader mode.

```sh
python -m serial.tools.list_ports -v
cd firmware
```

Throughout the commands below, replace `PORT` with the port you identified. Typical names start with `/dev/cu.usbmodem` or `/dev/cu.usbserial` on macOS, `COM` on Windows, and `/dev/ttyACM` or `/dev/ttyUSB` on Linux. Choose **one** of the following flash procedures. [Espressif flash commands](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/esptool/basic-commands.html)

### Fresh board

Copy your downloaded `frame_bridge-paired.bin` into `firmware`. Keep its filename. From that folder, flash the supplied bootloader and partition table together with your paired application.

```sh
python -m esptool --chip esp32s3 --port PORT write-flash --flash-mode dio --flash-freq 80m --flash-size 2MB 0x0 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin 0x10000 frame_bridge-paired.bin
```

### Upgrade a board running Frame Bridge V3

To make a backup first, open V3 setup and choose **Bindings → Export JSON**. Copy your downloaded `frame_bridge-paired.bin` into the `firmware` folder, keeping its filename. From that folder, write just the application at the V3 factory-app offset. This preserves the bootloader, partition table and stored profile.

```sh
python -m esptool --chip esp32s3 --port PORT write-flash 0x10000 frame_bridge-paired.bin
```

V4 loads a valid V3 profile, including its existing touch calibrations. Its two optional analog trigger sources start off. Saving the profile in V4 writes a separate settings entry. V4 can also import a V3 JSON backup.

After either procedure, wait for esptool to finish verifying the image. Tap RESET if the board stays in downloader mode. Connect the board’s **native USB/OTG data port** to the host for gameplay. On boards with separate USB and UART connectors, the UART connector is for programming and serial access.

## Connect the hands and open setup

1. Wake both controllers and disconnect them from any headset or other BLE host that is using them. To start pairing on a hand, hold **Steam** and the thin **View/Menu** button together until it beeps. Each hand connects and reconnects independently.
2. With the firmware running, **tap BOOT once**. On a phone or computer, join the board’s `FrameBridge-XXXX` Wi-Fi network with password `frame-setup`. Stay connected if the device reports that the network has no internet.
3. Open the [board setup page](http://192.168.4.1/). In **Live inputs**, check that each hand’s packet counter advances and its controls respond. A second device can display this page while the USB host runs a game.
4. When you finish, tap BOOT again to close setup Wi-Fi. It also closes automatically after 30 minutes. Saved mappings run on the board while Wi-Fi is off.

## Bind clicks, grips and sticks

A fresh profile maps the right face buttons to A/B/X/Y, the left D-pad to four directions, and each bumper and stick click to its matching side. Left View sends Select, left Steam sends Start, and right Menu sends Home. Trigger and grab presses start unbound. The Pro 3 layout exposes these additional choices in **Bindings**.

| Binding in setup | Gamepad output |
| --- | --- |
| L trigger / R trigger | LT / RT |
| L grab / R grab | PL / PR |
| L grab full click / R grab full click | L4 / R4 |
| Right Steam | Home, shared with the Home binding |

Choose **Learn** beside an action, then press and hold just the physical input you want to use. For a full grab click, hold the first stage before choosing Learn, then squeeze through the second stage. If a trigger or grab produces travel without a distinct click, choose Learn at rest (or with the first stage held), pull to the desired position, choose **Capture held travel**, release, then choose **Confirm release**. Choose **Save to board** to apply your changes.

Stick axes are experimental and start disabled. **Live inputs** shows the raw X and Y readings from each controller. Enable the sticks you want under **Bindings**. From firmware 4.0.1, all three USB layouts invert each stick’s Y reading when sending it to the game.

## Calibrate touch and sensitivity

Every touch surface starts **uncalibrated** on a fresh profile. Learn each surface you want to use on your own controllers. The left D-pad offers one shared touch contact and four separately bindable directional clicks.

1. Under **Touch**, select the hand and control, then choose **Learn touch**. Keep the controller steady and nearby fingers still. Touch lightly without clicking. For a grip sensor, lift and rest just the selected finger.
2. Follow the five **Capture** prompts in order. Alternate between lifted, touching, lifted, touching and lifted. If several candidate signals move, preview one and move the target finger and adjacent fingers independently. Choose **Use this signal** once it follows the intended finger.
3. Adjust **Engage at** and **Release at** for a continuous signal. A signal that rises on contact takes a higher engage level than release level. For a falling signal, reverse that order. Select an optional **Gamepad output** to hold a button or D-pad direction while touched. Choose **Save to board**, then confirm the result under **Live touches**.
4. For an analog output, open **Bindings → Analog trigger sources** and select a calibrated surface for LT or RT. Its release level gives zero and its engage level gives full output (255). Widen the gap for a gentler response. Binary sensors give only the two endpoints. A learned digital trigger press still gives full output.

For example, learn the right A surface and assign its contact to **R grab (PR)**. A light touch should activate PR. Pressing through the click should add A. Watch both outputs in **Live inputs**, then try the gesture on the host.

To back up or transfer your setup, choose **Bindings → Export JSON**. The file captures the settings shown, including unsaved edits. **Import JSON** stages a saved profile for review. Choose **Save to board** to apply it. Controller addresses and the USB layout are set separately in the firmware pairing page.

## Check host recognition

**Host configured the USB gamepad** on the setup page means USB enumeration succeeded. To verify that games can receive input, look for a native controller entry and a press/release from that controller. 8BitDo lists wired Pro 3 support for macOS 26 and USB-C iPads running iPadOS 26. The bridge implements gamepad inputs only. [8BitDo Apple compatibility](https://www.8bitdo.com/apple/) · [Apple controller discovery](https://developer.apple.com/documentation/gamecontroller/discovering-game-controllers)

| Host | Test |
| --- | --- |
| macOS | Check **System Settings → Game Controllers**. With the Swift toolchain installed, run `swift Check-Pro3.swift 60` from the release root. Press and release A on a Frame controller. Look for USB ID `2dc8:6009`, a native controller entry and the input events under that entry. Disconnect other gamepads if names are ambiguous. |
| iPadOS | Connect the native USB port to an unlocked USB-C iPad with a data cable. In Settings, search for **Game Controller**. Check for an entry, then press and release A in a controller-enabled game. Extra button support varies by game. |
| Windows | Open `joy.cpl`, select the gamepad and inspect **Properties** while pressing A. The bridge presents a USB HID gamepad. For a Steam title requiring Xbox-style input, enable Steam Input for that game. |

If **Live inputs** reports BLE button changes but the Mac detects zero native controllers, note the checker’s USB line and the layout shown beside **USB host check**. If the board shows **Standard** or **Extended**, repeat pairing and flashing with the **8BitDo Pro 3** layout selected.

## Build from source

Use ESP-IDF 6.1.1. Keep the sibling directories `source/FrameBridge`, `source/main` and `source/release` together. The project’s dependency manifest pins the TinyUSB components. From an activated ESP-IDF terminal, start in `source/FrameBridge`. [ESP-IDF setup for ESP32-S3](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/)

```sh
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
```

For a personal build, enter both addresses under **Steam Frame controllers** in menuconfig and save before building. Run `idf.py -p PORT flash` from the project directory to flash your configured build.

To generate a portable unpaired setup page, leave both address defaults at zero, build, and run the following command in the same ESP-IDF terminal. Open the resulting HTML, enter your controller addresses, and use the paired application in the flashing procedure above. For a fresh board, copy the paired image into the matching `build` directory and run the fresh-board command there.

```sh
python ../release/make_setup.py build/frame_bridge.bin ../../Set-up-controllers.html
```

The [source tests](source/FrameBridge/tests) cover report decoding and encoding, touch learning, and profile backups. On macOS, run `python ../release/verify_pro3.py build/frame_bridge.elf build/frame_bridge.bin` from the ESP-IDF project directory to compare the compiled Pro 3 layout with the installed Apple mapping. Use the host test above to verify physical recognition and input.

# FrameControllerBridge · V5

Use the left and right Steam Frame controllers as one USB or Bluetooth gamepad. An ESP32-S3 receives both hands over Bluetooth Low Energy (BLE) and starts each session with USB gamepad output selected. Hold **BOOT for at least 3 seconds** to send input exclusively to a Bluetooth host such as an iPad. Hold it again for at least 3 seconds to return to USB. The USB cable can stay connected throughout. A browser on the board’s temporary Wi-Fi network lets you learn buttons, calibrate touch sensitivity and save a profile for use with the browser closed.

The default USB layout targets the **wired 8BitDo Pro 3**. It provides two analog triggers and four extra button outputs, PL, PR, L4 and R4. Apple lists connected gamepads through its Game Controller framework. Recognition of the bridge on a physical Mac or iPad still needs to be verified after flashing. [Check host recognition](#check-host-recognition)

## Pair the firmware with your controllers

You need an ESP32-S3 board with native USB OTG, at least 2 MB of flash, a USB data cable, and the advertised Bluetooth address of each controller. The published firmware starts **unpaired**, and a fresh profile has **no touch calibrations**.

1. Download the project using **Code → Download ZIP** on GitHub and extract it, or clone the repository. Open the bundled [Set-up-controllers.html](Set-up-controllers.html) locally in a current Safari or Chrome browser.
2. Enter the two distinct BLE addresses in the **Left controller** and **Right controller** fields. Use six pairs of hexadecimal digits separated by colons. If you need to identify the addresses, wake one hand at a time and use a BLE scanner that displays advertised device addresses.
3. Choose the **USB layout** that works with your wired host. The default is **8BitDo Pro 3**. The Bluetooth output uses the Pro 3 control mapping with all three USB layout choices.
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

### Upgrade a board running Frame Bridge V3, V4, or V5

To make a backup first, open the current setup page and choose **Bindings → Export JSON**. Copy your downloaded `frame_bridge-paired.bin` into the `firmware` folder, keeping its filename. From that folder, write just the application at the factory-app offset. This preserves the bootloader, partition table and stored profile.

```sh
python -m esptool --chip esp32s3 --port PORT write-flash 0x10000 frame_bridge-paired.bin
```

Firmware 5.3.2 updates saved profiles and V3–V7 JSON backups to use each hand’s `0x14` byte 7 for trigger travel. It clears output bindings from both trigger touch contacts and keeps their calibrations available on the **Touch** tab.

After either procedure, wait for esptool to finish verifying the image. Tap RESET if the board stays in downloader mode. For wired gameplay connect the board’s **native USB/OTG data port** to the host. For wireless gameplay either USB connector can supply power on boards that support it. On boards with separate USB and UART connectors, the UART connector is for programming and serial access.

## Connect the hands and open setup

1. Wake both controllers and disconnect them from any headset or other BLE host that is using them. To start pairing on a hand, hold **Steam** and the thin **View/Menu** button together until it beeps. Each hand connects and reconnects independently.
2. With the firmware running, **tap BOOT once**. On a phone or computer, join the board’s `FrameBridge-XXXX` Wi-Fi network with password `frame-setup`. Stay connected if the device reports that the network has no internet.
3. Open the [board setup page](http://192.168.4.1/). In **Live inputs**, the bound controls are listed by hand, including released buttons. Press, touch or squeeze a control to check its saved assignment. The packet counters and raw stick readings are available on the same tab. A second device can display this page while the USB host runs a game.
4. When you finish, tap BOOT again to close setup Wi-Fi. It also closes automatically after 30 minutes. Saved mappings run on the board while Wi-Fi is off.

## Pair an iPad over Bluetooth

1. Power the board and let the firmware start. Wake the two Steam Frame controllers as usual.
2. Hold the board’s **BOOT** button for **at least 3 seconds**, then release it. The bridge switches from USB to Bluetooth output and opens a **2-minute** pairing window. A short tap toggles setup Wi-Fi.
3. On the iPad, open **Settings → Bluetooth**, select **FrameBridge Gamepad**, and accept the pairing request if shown.
4. Open a controller-enabled game and press and release **A**. For diagnostics, open **Live inputs** on the board setup page to see the wireless host status. A subscribed host receives both sticks, analog triggers, the D-pad and mapped buttons, including rear buttons in games that support them.

**To return to USB**, hold BOOT again for at least 3 seconds. You can do this while pairing, while connected, or after a timeout. If the wireless host disconnects or the pairing window expires, the bridge stays in Bluetooth mode. To open another pairing window, switch to USB, release BOOT, then hold it again to select Bluetooth.

The board remembers the last paired wireless host across power cycles. After a restart, select Bluetooth mode to reconnect. If the iPad still has an old pairing after the board’s flash was erased, use **Forget This Device** on the iPad, then repeat pairing. A battery percentage shown for the Bluetooth gamepad does not measure either hand’s charge.

## Bind clicks, grips and sticks

A fresh profile maps the right face buttons to A/B/X/Y, the left D-pad to four directions, and each bumper and stick click to its matching side. Left View sends Select and left Steam sends Start. With the Pro 3 layout, both right Menu and right Steam send Home. Left and right trigger travel drive L2 and R2 immediately. Grab presses start unbound. The **Bindings** table shows each output’s assigned inputs and their report bits or travel bytes.

| Binding in setup | Gamepad output |
| --- | --- |
| Left trigger / right trigger | L2 / R2 |
| L grab / R grab | PL / PR |
| L grab full click / R grab full click | L4 / R4 |
| Right Steam | Home, shared with the Home binding |

Choose **Learn** beside an output, then press a physical button to add that input. For example, learn right A under both A and B to make one press send both outputs. Learn left bumper under A as well and either physical button can hold A. The game receives the release when the last input holding A is released.

To edit from the other direction, open **Edit by Steam Frame button**, select a physical source, and check or uncheck its outputs. Unchecking one output removes that connection. **Clear** on an output row removes its button and travel connections. Rename a physical source or a gamepad output to show your own label in setup and Live inputs. Choose **Save to board** to apply mappings and names.

For L2 and R2, open **Bindings → Trigger activation and release** and slowly squeeze one trigger. Its live reading shows byte 7 of that hand’s `0x14` report. Wired Pro 3 and Bluetooth send the full 0–255 travel range. Standard and Extended USB use the digital trigger buttons. To adjust their thresholds, set **Activate at** above **Release at** if the byte rises during a squeeze, or below it if it falls. Choose **Apply values**, then **Save to board**. A falling calibration reverses the analog direction so a full squeeze still gives 255.

To learn a trigger or grab press threshold, start at rest, squeeze to the desired position, choose **Capture held travel**, release, and choose **Confirm release**. For a full grab click, start with the first stage already held.

## Customize the joysticks

Open **Joysticks** on the board setup page. Enable the left or right stick and move it to compare the raw reading with the preview of your settings. Edits take effect in the game when you choose **Save to board**. The settings apply to both USB and Bluetooth.

- **Deadzone (0–50%)** ignores small movements near the center of each axis. If a stick drifts at rest, raise the deadzone just enough for the preview to stay centered.
- **Sensitivity (25–200%)** scales movement outside the deadzone. Below 100%, a full physical push produces less than full game input. Above 100%, the game reaches full input earlier.
- **Invert X** reverses horizontal movement. **Invert Y** reverses vertical movement. Each direction can be set independently for each stick.

Fresh profiles start with both sticks disabled, a 0% deadzone, 100% sensitivity and only Y inverted. An upgrade keeps each stick’s previous enable setting and direction.

## Calibrate touch and sensitivity

Every touch surface starts **uncalibrated** on a fresh profile. Learn each surface you want to use on your own controllers. The left D-pad offers one shared touch contact and four separately bindable directional clicks. On the right hand, report `0x19` byte 5 carries the Steam signal and report `0x14` bit 8 carries its click. The right-hand card shows the raw Steam level. Select **Touch → Right hand → Steam** to calibrate its touch thresholds and assign an output.

1. Under **Touch**, both hands’ surfaces are listed with their calibration and output. Select a surface from either hand to open its editor, then choose **Learn touch**. Keep the controller steady and nearby fingers still. Touch lightly without clicking. For a grip sensor, lift and rest just the selected finger.
2. Follow the five **Capture** prompts in order. Alternate between lifted, touching, lifted, touching and lifted. If several candidate signals move, preview one and move the target finger and adjacent fingers independently. Choose **Use this signal** once it follows the intended finger.
3. Adjust **Engage at** and **Release at** for a continuous signal. A signal that rises on contact takes a higher engage level than release level. For a falling signal, reverse that order. Select an optional **Gamepad output** to hold a button or D-pad direction while touched. Choose **Save to board**, then confirm the result under **Live touches**.

The **Trigger** touch surface on each hand is available for calibration and monitoring. Its gamepad output stays unbound. Resting a finger on the trigger leaves L2 or R2 at its current travel value.

For example, learn the right A surface and assign its contact to the **PR** output in the Pro 3 layout. A light touch should activate PR. Pressing through the click should add A. Watch both outputs in **Live inputs**, then try the gesture on the host.

To back up or transfer your setup, choose **Bindings → Export JSON**. The file captures the settings shown, including unsaved edits. **Import JSON** stages a saved profile for review. Choose **Save to board** to apply it. Controller addresses and the USB layout are set separately in the firmware pairing page.

## Check host recognition

**Host configured the USB gamepad** on the setup page means USB enumeration succeeded. To verify that games can receive input, look for a native controller entry and a press/release from that controller. 8BitDo lists Bluetooth Pro 3 support on iPadOS 26 and macOS 26, with wired support on USB-C iPads and Macs. The bridge implements a BLE HID gamepad using the Bluetooth identity and input mapping in Apple’s Pro 3 controller database. Physical iPad pairing and gameplay require an on-device test. The bridge implements gamepad inputs only. [8BitDo Apple compatibility](https://www.8bitdo.com/apple/) · [Apple controller discovery](https://developer.apple.com/documentation/gamecontroller/discovering-game-controllers)

| Host | Test |
| --- | --- |
| macOS | Check **System Settings → Game Controllers**. With the Swift toolchain installed, run `swift Check-Pro3.swift 60` from the release root. Press and release A on a Frame controller. Look for a native controller entry and press/release events under that entry. A wired Pro 3 also shows USB ID `2dc8:6009`. Disconnect other gamepads if names are ambiguous. |
| iPadOS | Pair **FrameBridge Gamepad** over Bluetooth, or connect the native USB port to an unlocked USB-C iPad with a data cable. In Settings, search for **Game Controller**. Check for an entry, then press and release A in a controller-enabled game. Extra button support varies by game. |
| Windows | Open `joy.cpl`, select the gamepad and inspect **Properties** while pressing A. The bridge presents a USB HID gamepad. For a Steam title requiring Xbox-style input, enable Steam Input for that game. |

If **Live inputs** reports BLE button changes but the Mac detects zero native controllers, note the checker’s USB line and the layout shown beside **USB host check**. If the board shows **Standard** or **Extended**, repeat pairing and flashing with the **8BitDo Pro 3** layout selected.

## Build from source

Use ESP-IDF 6.1.1. Keep the sibling directories `source/FrameBridge`, `source/main` and `source/release` together. The project’s dependency manifest pins the TinyUSB components. From an activated ESP-IDF terminal, start in `source/FrameBridge`. [ESP-IDF setup for ESP32-S3](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/)

```sh
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
```

If reusing an existing SDK configuration from V5.0 or earlier, enable NimBLE bond persistence and set its maximum simultaneous connections to at least three. Fresh builds use these defaults. For a personal build, enter both addresses under **Steam Frame controllers** in menuconfig and save before building. Run `idf.py -p PORT flash` from the project directory to flash your configured build.

To generate a portable unpaired setup page, leave both address defaults at zero, build, and run the following command in the same ESP-IDF terminal. Open the resulting HTML, enter your controller addresses, and use the paired application in the flashing procedure above. For a fresh board, copy the paired image into the matching `build` directory and run the fresh-board command there.

```sh
python ../release/make_setup.py build/frame_bridge.bin ../../Set-up-controllers.html
```

The [source tests](source/FrameBridge/tests) cover report decoding and encoding, touch learning, and profile backups. On macOS, run `python ../release/verify_pro3.py --bluetooth build/frame_bridge.elf build/frame_bridge.bin` from the ESP-IDF project directory to compare both compiled Pro 3 identities and input reports with the installed Apple mappings. Use the host test above to verify physical recognition and input.

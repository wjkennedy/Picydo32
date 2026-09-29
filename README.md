# Picydo32

Picydo32 is an alpha-quality Pico-8-compatible runtime for the original ESP32, built for CYD boards with 2.4-inch and 2.8-inch ILI9341 displays.

The current target is the ESP32-D0WD-V3 revision 3, with cartridges loaded as `.p8` files from an SD card. The project also includes a desktop SDL build for emulator testing.

## Current status

- CYD ILI9341 display bring-up is working.
- Pico-8-style Lua cartridges can run from SD card.
- The ESP32 startup browser recursively scans SD-card directories for `.p8` files.
- Generic Bluetooth Classic HID gamepad support is enabled in the reference firmware.
- Audio is deliberately disabled for the current ESP32 bring-up.
- The web configuration server is currently deprecated to preserve RAM.
- Wi-Fi and Bluetooth are build-time alternatives on the original ESP32 because simultaneous use exceeds the available internal RAM budget.

This is development firmware, not a finished Pico-8 replacement. Pico-8 API coverage and controller compatibility remain incomplete.

## Hardware

### CYD display

| Signal | GPIO |
|---|---:|
| ILI9341 MOSI | 13 |
| ILI9341 SCLK | 14 |
| ILI9341 CS | 15 |
| ILI9341 DC | 2 |
| ILI9341 reset | -1 |
| Backlight | 27 |
| Display size | 240 × 320 |

The game viewport is rendered as a doubled Pico-8-sized image, with the remaining display area reserved for runtime status information.

### SD card

The SD card uses a separate VSPI bus:

| Signal | GPIO |
|---|---:|
| MOSI | 23 |
| MISO | 19 |
| SCLK | 18 |
| CS | 5 |

The browser searches recursively below `/sd`, for example:

```text
/sd/Celeste/celeste.p8
/sd/Splore/star_splore.p8
/sd/Other Game/game.p8
```

Use a FAT-formatted card. The browser currently lists up to 24 cartridges and truncates long display names.

### Bluetooth controller

The reference firmware uses the original ESP32 Bluetooth Classic HID host. On boot it scans for HID devices and attempts to connect to the first discovered device.

Pairing behavior:

- SSP confirmation is accepted automatically.
- Legacy PIN pairing uses `1234`.
- The device is configured as connectable and discoverable.

Cheap generic controllers vary considerably. The current report decoder supports common button/hat/X/Y layouts and prints the first raw HID report to the serial console for troubleshooting.

## Controls

In the SD browser:

- D-pad up/down: select a cartridge
- A: launch the selected cartridge
- B: exit the browser

The firmware also retains configured GPIO inputs and serial keyboard input for bring-up. USB keyboard support depends on the board’s USB hardware and is not yet a dedicated input path.

## Building for desktop

Requirements are CMake, a C/C++ compiler, and the bundled SDL sources.

```bash
./build.sh
```

This builds `build-pc/pc_pico` and launches it on a desktop system.

## Building ESP32 firmware

Use an ESP-IDF checkout and the Xtensa ESP32 toolchain:

```bash
IDF_PATH=/path/to/esp-idf ./build.sh firmware
```

The merged image is written to:

```text
build-esp/picopico-esp32-merged.bin
```

The checked-in reference `sdkconfig` enables the Bluetooth gamepad build. To build the Wi-Fi configuration instead, disable `CONFIG_BLUETOOTH_GAMEPAD` with ESP-IDF configuration tools and rebuild. Wi-Fi is retained for future Splore/network work but is not part of the RAM-constrained Bluetooth firmware.

## Flashing

```bash
IDF_PATH=/path/to/esp-idf \
./build.sh flash /dev/cu.usbserial-0001
```

Linux serial devices usually look like `/dev/ttyUSB0` or `/dev/ttyACM0`.

The command flashes the bootloader, partition table, and application image for the project’s 4 MB flash layout.

## Serial diagnostics

Useful startup messages include:

```text
found N .p8 cartridges
BT HID host ready; put the controller in pairing mode
BT device discovered: ...
BT authentication complete
Parsing cart ...
```

Connect to the board’s serial port while flashing or rebooting to inspect startup failures.

## Project layout

```text
src/                    Pico-8 runtime, Lua API, renderer, and browser UI
esp/                    ESP32 backend, CYD display, SD loader, Bluetooth HID host
carts/                  Example source cartridges for desktop testing
lua/                    Lua runtime used by the project
SDL/                    Bundled desktop SDL dependency
SDL_mixer/              Bundled desktop mixer dependency
build.sh                Desktop, firmware, image merge, and flash entry point
partitions.csv          ESP32 4 MB flash partition layout
```

## Known limitations

- The ESP32 and Wi-Fi/Bluetooth builds are memory constrained.
- Bluetooth is Classic HID only; BLE gamepads are not supported.
- Generic HID report layouts are not all interchangeable.
- Audio and music are disabled in the ESP32 reference build.
- The web configuration server is not included.
- Cartridge writes, persistent `cartdata`, save storage, and OTA updates are not implemented.
- Pico-8 API compatibility is incomplete; consult the source when porting a cartridge.

## Development priorities

1. Validate Bluetooth pairing and input across target generic controllers.
2. Improve the SD browser presentation and cartridge metadata handling.
3. Continue CYD 2.4/2.8 display validation.
4. Reduce runtime RAM use so Wi-Fi and additional services can return safely.
5. Expand Pico-8 API compatibility and automated cartridge tests.

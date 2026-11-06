# key-agent

A phone-configurable USB keyboard sequencer for ESP32-S3. Connect the board to a computer, join its Wi-Fi hotspot from your phone, and build repeating keyboard macros in an offline web interface.

## Features

- Four independently timed sequences, each with up to 32 actions.
- Key presses, modifier combinations, and standalone waits.
- Letters, numbers, arrows, common keys, F1–F24, and symbols.
- Infinite repetition or 1–100,000 cycles.
- Per-sequence enable, save, start, and stop; start-all and stop-all controls.
- Settings persist across power cycles in NVS flash storage.
- BOOT button for quick start/stop. All sequences remain paused after power-on, USB reconnection, or host sleep.
- English interface and firmware messages, with no external scripts or internet dependency.

## Hardware

The existing hardware setup uses an ESP32-S3 N16R8 board with 16 MB flash and two USB-C ports. With the antenna pointing up and the ports pointing down, the left USB/OTG port connects to the target computer; the right USB-to-UART port is used for flashing and logs. Verify the labels on your board: connector positions vary by manufacturer. BOOT is connected to GPIO 0.

## Quick start

1. Flash the firmware using the instructions below.
2. Connect the native USB/OTG port to the target computer with a data-capable cable.
3. On your phone, join Wi-Fi **key-agent**, password **71581234**.
4. Open **http://192.168.4.1**. Stay connected if the phone reports no internet access.
5. Select a sequence, edit its actions, enable it, and click **Save and apply**.
6. Click **Start sequence** or **Start all**. Open a text editor on the computer to observe the output.

Press BOOT while any sequence is running to stop all sequences. When all are paused, BOOT starts every enabled sequence using the saved settings. Do not hold BOOT while powering on or resetting the board: this enters download mode.

The default sequence types `1`, `2`, and `3`, holding each key for 40 ms and waiting 500 ms after release. All four sequences are enabled by default but remain paused until started.

## Timing and behavior

| Setting | Range |
| --- | --- |
| Key hold | 20–10,000 ms |
| Wait after release | 0–86,400,000 ms |
| Standalone wait | 1–86,400,000 ms |
| Repetitions | 0 for unlimited; otherwise 1–100,000 |

1 second = 1,000 ms; 1 hour = 3,600,000 ms. Each sequence has its own timer, but USB key presses are serialized. Ready sequences are served in priority order: 1, 2, 3, then 4. A held key is released before another sequence takes ownership. Wait values are minimum delays, not guaranteed periods; higher-priority activity can delay lower-priority sequences.

Saving stops only the selected sequence, releases its key, and stores its configuration. Other sequences continue. Starting a sequence begins from its first action. Start all leaves already-running sequences in progress. Disabled sequences retain their actions. If none are enabled, nothing starts.

Switching tabs preserves unsaved drafts; reloading the page discards them. Start all is disabled while any draft is unsaved. BOOT always uses saved settings. Closing the page or disconnecting your phone does not stop playback.

Key labels use the US keyboard layout. Actual characters depend on the host keyboard layout and input method. Use Cmd/Win for Command on macOS.

## Build from source

The project uses ESP-IDF 6.1.0, esp_tinyusb 2.4.0, and the component versions recorded in `dependencies.lock`. Install ESP-IDF and activate its environment using its `export.sh` script (or the equivalent environment activation command on your platform).

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p YOUR_SERIAL_PORT -b 115200 flash
idf.py -p YOUR_SERIAL_PORT monitor
```

Exit the serial monitor with Ctrl+]. Use the USB-to-UART port for flashing. The build output is `build/hid_keyboard_123.bin`.

## Flash a release

Download and extract the English firmware ZIP from the repository Releases page. Install the official `esptool` Python package in a virtual environment, then run from the extracted directory:

```sh
python -m esptool --chip esp32s3 --port YOUR_SERIAL_PORT --baud 115200 write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 key-agent.bin
```

Release packages include SHA-256 checksums. The separate images leave the NVS partition at `0x9000` untouched. Do not run `erase-flash` if you want to retain saved settings. Existing v4 settings remain compatible; older single-sequence settings are loaded into Sequence 1. NVS initialization may erase storage if it is incompatible or cannot be initialized due to exhausted pages.

## Development and validation

After the initial ESP-IDF configure/build downloads managed components:

```sh
./tests/run.sh
python3 tests/preview_server.py
```

The host tests require Clang and run with AddressSanitizer and UndefinedBehaviorSanitizer. They cover sequencing, timing, modifiers, finite/infinite repetition, release retries, reconnection, four-sequence arbitration, enabled flags, and JSON validation. The preview server listens at `http://127.0.0.1:8766` and only simulates the API; it does not operate hardware.

The English release is a localization of the existing v4 implementation. Previous hardware checks do not constitute hardware validation of this newly built binary. Verify typing, stopping, and persistence on your own board after flashing.

## Source layout

| File | Purpose |
| --- | --- |
| `main/main.c` | USB HID, Wi-Fi AP, HTTP API, NVS, and BOOT button |
| `main/sequence.c` | Sequence state machine and shared keyboard scheduler |
| `main/config_json.c` | JSON validation and serialization |
| `main/index.html` | Embedded offline English control page |
| `partitions.csv` | NVS, PHY, and 3 MiB factory application layout |
| `tests/` | Host tests and mock UI preview server |

## Network access

The hotspot uses a fixed default password defined in `main/main.c`. Anyone connected to the hotspot can control the keyboard through the device API. Change `AP_PASSWORD` before building if you need a different password. Configuration is served over local HTTP; the device does not join an external Wi-Fi network.

## Attribution

USB descriptor setup is based on Espressif's CC0/Unlicense `tusb_hid` example. ESP-IDF and managed components retain their respective licenses; the component manager retrieves them during configuration. This repository does not bundle third-party drivers or flashing executables.

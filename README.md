# OpenAnnealer

> **Work in progress, not yet fully bench-verified.** This is a conversion of [OpenTrickler](https://github.com/eamars/OpenTrickler) — RP2040/RP2350 firmware originally built for an automated powder-charging scale trickler — into a controller for a **cartridge case annealer**. The core conversion (anneal cycle, manual/commissioning mode, dwell-time calibration, LCD menu, web portal, and removal of the scale subsystem) is code-complete, but most of it has not yet been run on real hardware end-to-end. Treat `main` as pre-release.

This repo is not affiliated with the upstream OpenTrickler project or its community — please don't bring OpenAnnealer questions to their Discord.

**Hardware target: Pico 2 W (RP2350) only.** OpenTrickler supports both the original Pico W (RP2040) and Pico 2 W (RP2350); this fork standardizes on RP2350, since OTA firmware updates rely on RP2350's native partition-table/"Try Before You Buy" boot mechanism, which doesn't exist on RP2040. Pico W build instructions have been removed accordingly — if you're on a Pico W, this project isn't for you.

## What this is

The target hardware is a stepper-driven case feeder pushing cartridge cases into a vertical holder inside a horizontally-mounted induction coil. A servo positions the case in the coil for the heat dwell, then swings clear so the case drops. Most of the electrical hardware carries over from OpenTrickler (RP2350, TMC2209 steppers, PWM servo, mini 12864 display/encoder/Neopixel, on-board EEPROM, WiFi/REST/web UI) — the control logic, menu, and web UI have been reworked for annealing instead of powder charging.

An anneal cycle runs: **position the holder → feed a case in → let it settle → heat for the configured dwell → drop the case → repeat.** (The holder is positioned *before* feeding, not after — feeding into a holder that isn't ready risks the case missing it or binding.)

### Done so far

- **Induction heater trigger**: on/off GPIO control with a hard FreeRTOS safety timer that force-disables the coil after a configurable max dwell time, independent of anything else in the system. The trigger pin is runtime-configurable (EEPROM + REST + web UI).
- **Case holder servo**: simplified from OpenTrickler's dual-shutter (2-channel) design to a single PWM channel, since only one physical servo is used.
- **Anneal cycle**: the core state machine (position → feed → settle → heat → drop → cooldown, repeating for a configured case count or indefinitely). Bench-verified for the original feed/hold ordering; the corrected position-before-feed sequencing has not yet been re-verified on the bench.
- **Manual/commissioning mode**: jog the feeder motor, toggle the case holder between hold/drop, and pulse the induction coil for bench testing — useful for checking the mechanism before running full cycles.
- **Profiles as anneal recipes**: profiles (previously per-powder PID tuning) now hold a per-case-type recipe — feed time/speed, pre-heat settle, dwell time, post-heat delay, and holder hold ratio.
- **Dwell-time calibration mode**: a guided workflow using temperature-indicating (Tempilaq-style) paint — run the coil continuously, press the knob the instant the paint changes color, and save the measured time directly into the selected profile's dwell time.
- **LCD menu**: reworked for annealing — profile select → cycle count → start; Manual mode entry; Settings gained Induction Heater and Calibrate Dwell Time, lost Scale; Case Holder Hold/Drop terminology fixed (was stale Open/Close).
- **Scale subsystem removed** entirely — weight measurement has no role in annealing. Frees GPIO 0/1 for a future sensor.
- **Web portal rebuilt**: the main tab is now a real anneal-cycle control panel (start/stop, live state, cases completed, dwell time) plus a manual-mode panel and case holder control; Settings gained the reworked Profile/Anneal Mode/Feeder Motor/Spare Motor pages and lost Scale.
- **OTA firmware updates (in progress)**: the RP2350 bootrom's "Try Before You Buy" A/B partition rollback mechanism has been validated on real hardware, and the update-receive/confirm/rollback-reporting code is implemented (`src/ota_update.c`) — but a full live upload-to-confirm-or-rollback cycle against the real board has not yet succeeded; see that module's comments for the current state.

### Not yet done

- A live, end-to-end OTA update test (see above).
- Bench verification of Milestones 4 through 9 (manual mode, profiles-as-recipes, calibration mode, LCD menu, scale removal, web portal) — all build cleanly and have been checked for internal consistency, but none have been run against the real annealer yet.
- Optional IR temperature sensor for target-temperature annealing (speculative; exact sensor hardware not yet chosen).

### Supported Hardware (current fork)

- Raspberry Pi Pico 2 W (RP2350) — see hardware target note above
- Mini 12864 Display Module (with rotary encoder, 3x Neopixel LED)
- Dedicated Neopixel LED (up to 16 chains)
- 1x Miniature Servo Motor (TowerPro SG/MG90S, or similar) — case holder
- 2x TMC2209 (STEP/DIR with 1-line UART) — one drives the case feeder, the second is unused for now
- On-board EEPROM (up to 256 kbits)
- Induction heater module, triggered via a configurable GPIO

### Remote Connectivity

* WiFi (2.4 GHz only, AP or Station mode)
* Web Interface
* RESTful Interface
* mDNS Lookup

## Building from source

Reference: https://datasheets.raspberrypi.com/pico/getting-started-with-pico.pdf

### Prerequisites

[Git](https://gitforwindows.org/) and [VSCode](https://code.visualstudio.com/) are required to build the firmware. To install build dependencies, use the [VSCode Raspberry Pi Pico extension](https://marketplace.visualstudio.com/items?itemName=raspberry-pi.raspberry-pi-pico) and create a pico-example project (any project will trigger the download of pico-sdk, a collection of tools required to build the firmware locally). When creating the example project, select a Pico SDK version of v2.1.1 in the version dropdown — this repo's `library/pico-sdk` submodule is pinned to that exact release, and `configure_env.ps1` expects the matching toolchain/CMake/Ninja/picotool versions that the extension downloads alongside it.

Then you can verify the installation of pico-sdk by inspecting the path from `C:\Users\<user name>\.pico-sdk`.
![pico_sdk_path](resources/pico_sdk_path.png)

### Downloading Source Code

From PowerShell, execute below command to fetch the source code:

    git clone https://github.com/cody0303/OpenAnnealer

Next change to the cloned directory

    cd OpenAnnealer

Next use git to initialise the required submodules

    git submodule init

Now using git clone all submodules. It may take up to 5 minutes to clone all required submodules.

    git submodule update --init --recursive

### Configure CMake

Open the PowerShell, run the below script to load required environment variables:

    .\configure_env.ps1

To build firmware for Pico 2 W (the only supported target), from the same PowerShell session, run below command:

    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPICO_BOARD=pico2_w

This produces a plain (non-OTA) image that boots directly from a BOOTSEL flash — the normal day-to-day development workflow. Building an OTA-capable image (one that can be pushed via `/ota_upload` or flashed into an A/B partition) additionally requires `-DOTA_TBYB_BUILD=ON`; see `CMakeLists.txt` for why that's opt-in rather than the default.

### Build Firmware

From the same workspace root directory, run the below command to build the firmware from source code into the `build` directory:

    cmake --build build --config Debug

On success, you can find app.uf2 in the `<workspace_root>/build/` directory. To flash it, put the Pico into bootloader mode (hold BOOTSEL while plugging in the USB cable) and copy app.uf2 onto the drive that appears.

### Use VSCode

You need to call VSCode from script to pre-configure environment variables. You can simply call

    .\run_vscode.ps1

The VSCode cmake plugin is pre-configured to build for Pico 2 W.

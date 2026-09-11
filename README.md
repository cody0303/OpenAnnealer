# OpenAnnealer

> **Work in progress.** This is an active conversion of [OpenTrickler](https://github.com/eamars/OpenTrickler) — RP2040/RP2350 firmware originally built for an automated powder-charging scale trickler — into a controller for a **cartridge case annealer**. Large parts of the codebase, menu, web UI, and this README still describe the original trickler behavior and haven't been converted yet. Expect breaking changes on `main`, and treat anything below as a snapshot of where the fork currently stands, not a finished product.

This repo is not affiliated with the upstream OpenTrickler project or its community — please don't bring OpenAnnealer questions to their Discord.

**Hardware target: Pico 2 W (RP2350) only.** OpenTrickler supports both the original Pico W (RP2040) and Pico 2 W (RP2350); this fork is standardizing on RP2350, since planned OTA firmware updates rely on RP2350's native partition-table/"Try Before You Buy" boot mechanism, which doesn't exist on RP2040. Pico W build instructions have been removed accordingly — if you're on a Pico W, this project isn't for you.

## What's changing, and why

The target hardware is a stepper-driven case feeder pushing cartridge cases into a vertical holder inside a horizontally-mounted induction coil, with a servo holding the case up in the coil during the heat dwell and then swinging clear so it drops. Most of the electrical hardware carries over from OpenTrickler (RP2350, TMC2209 steppers, PWM servo, mini 12864 display/encoder/Neopixel, on-board EEPROM, WiFi/REST/web UI) — what's changing is the control logic and some pin assignments.

### Done so far

- Induction heater trigger: on/off GPIO control with a hard FreeRTOS safety timer that force-disables the coil after a configurable max dwell time, independent of anything else in the system. The trigger pin is runtime-configurable (EEPROM + REST + web UI), not hardcoded, since the right pin depends on how each build is wired.
- Case holder servo simplified from the original dual-shutter (2-channel) design down to a single PWM channel, since only one physical servo is used.
- The core anneal cycle: `charge_mode` has been replaced with an anneal-cycle state machine (feed a case in → hold it in the coil → heat for a configured dwell → drop it → repeat), bench-verified including multi-cycle runs and an immediate coil-off abort.

### Still using the original OpenTrickler logic (not yet converted)

- The scale subsystem and Cleanup Mode are still present and still describe powder trickling, not case annealing.
- The LCD menu, REST API, and web portal mostly still reflect the trickler workflow (the anneal cycle above is reachable, but menu labels/web UI haven't been reworked for it yet).
- OTA firmware updates (in progress) and a dwell-time calibration mode don't exist yet.

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

### Build Firmware

From the same workspace root directory, run the below command to build the firmware from source code into the `build` directory:

    cmake --build build --config Debug

On success, you can find app.uf2 in the `<workspace_root>/build/` directory. To flash it, put the Pico into bootloader mode (hold BOOTSEL while plugging in the USB cable) and copy app.uf2 onto the drive that appears.

### Use VSCode

You need to call VSCode from script to pre-configure environment variables. You can simply call

    .\run_vscode.ps1

The VSCode cmake plugin is pre-configured to build for Pico 2 W.

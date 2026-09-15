/*
 * LED blink with FreeRTOS
 */

#include <FreeRTOS.h>
#include <task.h>
#include <stdio.h>
#include <stdint.h>
#include "pico/stdlib.h"
#include "hardware/watchdog.h"

#include "FreeRTOSConfig.h"
#include "configuration.h"
#include "u8g2.h"

// modules
#include "app.h"
#include "motors.h"
#include "eeprom.h"
#include "display.h"
#include "anneal_mode.h"
#include "rest_endpoints.h"
#include "wireless.h"
#include "neopixel_led.h"
#include "mini_12864_module.h"
#include "menu.h"
#include "profile.h"
#include "servo_gate.h"
#include "induction_heater.h"
#include "ota_update.h"
#include "ir_temp_sensor.h"


// Boot-order tracing for the OTA_DEBUG_SERIAL build (see CMakeLists.txt) - traces
// which init step is in progress, so if boot stalls/fails silently (as seen
// debugging the OTA/partition-table boot issue), the last printed line pinpoints
// where. No-ops entirely in a normal (non-debug) build.
#if OTA_DEBUG_SERIAL
#define BOOT_TRACE(msg) do { printf("=== BOOT: " msg " ===\n"); } while (0)
#else
#define BOOT_TRACE(msg) do {} while (0)
#endif

int main()
{
#if OTA_DEBUG_SERIAL
    stdio_init_all();
    sleep_ms(1500);
#endif
    BOOT_TRACE("main() start");

    // Initialize EEPROM first
    eeprom_init();
    BOOT_TRACE("eeprom_init() done");

    // Initialize Neopixel RGB on the mini 12864 board
    neopixel_led_init();
    BOOT_TRACE("neopixel_led_init() done");

    // Configure other functions from mini 12864 display
    mini_12864_module_init();
    BOOT_TRACE("mini_12864_module_init() done");

    // Check for a pending OTA update: confirm it (after a stability delay) if we're
    // the freshly-flashed candidate, or surface a rollback fault if we're not.
    // Needs EEPROM (already up) and the LCD/neopixel (just initialized) for fault
    // display, and must run before wireless_init() so a rollback fault is visible
    // before any network activity starts.
    ota_update_init();
    BOOT_TRACE("ota_update_init() done");

    // Initialize wireless settings
    wireless_init();
    BOOT_TRACE("wireless_init() done");

    // Load config for motors
    motor_init_err_t motor_init_err = motors_init();
    if (motor_init_err != MOTOR_INIT_OK) {
        handle_motor_init_error(motor_init_err);
    }

    // Initialize the induction heater trigger
    induction_heater_init();

    // Optional IR temp sensor (Milestone 11) - on its own I2C bus, fully non-fatal if
    // not populated (see ir_temp_sensor_init()'s own probe/degrade logic).
    ir_temp_sensor_init();

    // Initialize anneal mode settings
    anneal_mode_config_init();

    // Initialize profile data
    profile_data_init();

    // Initialize the servo
    servo_gate_init();

    // Start menu task
    xTaskCreate(menu_task, "Menu Task", 1024, NULL, 6, NULL);

    // Start RTOS
    vTaskStartScheduler();

    while (true)
        ;
}

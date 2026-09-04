#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "hardware/gpio.h"
#include "configuration.h"
#include "eeprom.h"
#include "common.h"
#include "induction_heater.h"

induction_heater_t induction_heater;

const eeprom_induction_heater_data_t default_induction_heater_data = {
    .induction_heater_config_rev = 0,
    .active_high = true,
    .max_dwell_ms = 5000,   // Safety cap; must comfortably cover the longest expected dwell/calibration run
    .gpio_pin = INDUCTION_TRIGGER_PIN_DEFAULT,
};


// GPIOs 23/24/25/29 are used internally by the cyw43 wireless chip on Pico W/2W and
// must never be repurposed, or WiFi silently breaks.
static bool _is_gpio_reserved_for_wifi(uint8_t pin) {
    return pin == 23 || pin == 24 || pin == 25 || pin == 29;
}


static void _induction_heater_set_gpio(bool active) {
    bool level = induction_heater.eeprom_induction_heater_data.active_high ? active : !active;
    gpio_put(induction_heater.eeprom_induction_heater_data.gpio_pin, level);
}


static void _induction_heater_safety_timeout(TimerHandle_t timer) {
    (void) timer;

    // Force the coil off regardless of what the caller thinks the state is.
    _induction_heater_set_gpio(false);
    induction_heater.is_active = false;
    induction_heater.safety_cutoff_fault = true;
}


// Switches which GPIO drives the coil trigger. Always cuts power first: on the old
// pin if one was already claimed, then leaves it as a safe floating input before
// claiming the new pin as an output driven inactive.
static void _induction_heater_apply_pin(uint8_t new_pin) {
    induction_heater_enable(false);

    uint8_t old_pin = induction_heater.eeprom_induction_heater_data.gpio_pin;
    if (induction_heater.pin_claimed && old_pin != new_pin) {
        gpio_set_dir(old_pin, GPIO_IN);
    }

    induction_heater.eeprom_induction_heater_data.gpio_pin = new_pin;

    gpio_init(new_pin);
    gpio_set_dir(new_pin, GPIO_OUT);
    induction_heater.pin_claimed = true;
    _induction_heater_set_gpio(false);
}


bool induction_heater_config_save(void) {
    bool is_ok = save_config(EEPROM_INDUCTION_HEATER_CONFIG_BASE_ADDR, &induction_heater.eeprom_induction_heater_data, sizeof(induction_heater.eeprom_induction_heater_data));
    return is_ok;
}


bool induction_heater_init(void) {
    bool is_ok = true;

    memset(&induction_heater, 0x0, sizeof(induction_heater));

    is_ok = load_config(EEPROM_INDUCTION_HEATER_CONFIG_BASE_ADDR,
                         &induction_heater.eeprom_induction_heater_data,
                         &default_induction_heater_data,
                         sizeof(induction_heater.eeprom_induction_heater_data),
                         EEPROM_INDUCTION_HEATER_CONFIG_REV);
    if (!is_ok) {
        printf("Unable to read induction heater configuration\n");
        return false;
    }

    if (_is_gpio_reserved_for_wifi(induction_heater.eeprom_induction_heater_data.gpio_pin)) {
        printf("Induction heater pin conflicts with WiFi, falling back to default\n");
        induction_heater.eeprom_induction_heater_data.gpio_pin = INDUCTION_TRIGGER_PIN_DEFAULT;
    }

    // Register to eeprom save all
    eeprom_register_handler(induction_heater_config_save);

    // Initialize GPIO, forced inactive immediately
    gpio_init(induction_heater.eeprom_induction_heater_data.gpio_pin);
    gpio_set_dir(induction_heater.eeprom_induction_heater_data.gpio_pin, GPIO_OUT);
    induction_heater.pin_claimed = true;
    _induction_heater_set_gpio(false);

    // One-shot safety timer: created stopped, (re)started every time the coil is enabled
    induction_heater.safety_timer = xTimerCreate(
        "InductionSafety",
        pdMS_TO_TICKS(induction_heater.eeprom_induction_heater_data.max_dwell_ms),
        pdFALSE,    // one-shot
        NULL,
        _induction_heater_safety_timeout
    );

    return is_ok && (induction_heater.safety_timer != NULL);
}


void induction_heater_enable(bool enable) {
    if (enable) {
        induction_heater.safety_cutoff_fault = false;

        // (Re)arm the safety timer for the currently configured max dwell time before energising the coil
        xTimerChangePeriod(induction_heater.safety_timer, pdMS_TO_TICKS(induction_heater.eeprom_induction_heater_data.max_dwell_ms), portMAX_DELAY);
        xTimerReset(induction_heater.safety_timer, portMAX_DELAY);

        _induction_heater_set_gpio(true);
        induction_heater.is_active = true;
    }
    else {
        xTimerStop(induction_heater.safety_timer, portMAX_DELAY);

        _induction_heater_set_gpio(false);
        induction_heater.is_active = false;
    }
}


bool induction_heater_is_active(void) {
    return induction_heater.is_active;
}


bool http_rest_induction_heater_config(struct fs_file *file, int num_params, char *params[], char *values[]) {
    // Mappings:
    // i0 (bool): active_high
    // i1 (int): max_dwell_ms
    // i2 (int): gpio_pin
    // ee (bool): save to eeprom

    static char json_buffer[224];
    bool save_to_eeprom = false;
    bool pin_rejected = false;

    for (int idx = 0; idx < num_params; idx += 1) {
        if (strcmp(params[idx], "i0") == 0) {
            induction_heater.eeprom_induction_heater_data.active_high = string_to_boolean(values[idx]);
        }
        else if (strcmp(params[idx], "i1") == 0) {
            induction_heater.eeprom_induction_heater_data.max_dwell_ms = strtoul(values[idx], NULL, 10);
        }
        else if (strcmp(params[idx], "i2") == 0) {
            long requested_pin = strtol(values[idx], NULL, 10);

            if (requested_pin < 0 || requested_pin > 28 || _is_gpio_reserved_for_wifi((uint8_t) requested_pin)) {
                pin_rejected = true;
            }
            else if ((uint8_t) requested_pin != induction_heater.eeprom_induction_heater_data.gpio_pin) {
                _induction_heater_apply_pin((uint8_t) requested_pin);
            }
        }
        else if (strcmp(params[idx], "ee") == 0) {
            save_to_eeprom = string_to_boolean(values[idx]);
        }
    }

    if (save_to_eeprom) {
        induction_heater_config_save();
    }

    snprintf(json_buffer,
             sizeof(json_buffer),
             "%s"
             "{\"i0\":%s,\"i1\":%lu,\"i2\":%d,\"active\":%s,\"fault\":%s,\"pin_rejected\":%s}",
             http_json_header,
             boolean_to_string(induction_heater.eeprom_induction_heater_data.active_high),
             induction_heater.eeprom_induction_heater_data.max_dwell_ms,
             induction_heater.eeprom_induction_heater_data.gpio_pin,
             boolean_to_string(induction_heater.is_active),
             boolean_to_string(induction_heater.safety_cutoff_fault),
             boolean_to_string(pin_rejected));

    size_t data_length = strlen(json_buffer);
    file->data = json_buffer;
    file->len = data_length;
    file->index = data_length;
    file->flags = FS_FILE_FLAGS_HEADER_INCLUDED;

    return true;
}

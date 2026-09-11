#include <stdbool.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "configuration.h"
#include "eeprom.h"
#include "common.h"
#include "servo_gate.h"

// Attributes
servo_gate_t servo_gate;

// Const settings
const float _servo_pwm_freq = 50.0;
const uint16_t _pwm_full_scale_level = 65535;


const eeprom_servo_gate_config_t default_eeprom_servo_gate_config = {
    .servo_gate_config_rev = 0,
    .servo_gate_enable = false,
    .close_duty_cycle = 0.05f,
    .open_duty_cycle = 0.09f,
    .open_speed_pct_s = 5.0f,
    .close_speed_pct_s = 3.0f,
};


const char * _gate_state_string[] = {
    "Disabled",
    "Hold",
    "Drop"
};
const char * gate_state_to_string(gate_state_t state) {
    return _gate_state_string[state];
}

static inline float clamp01(float x) {
    if (x < 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return x;
}

static void inline _set_duty_cycle(uint16_t duty_cycle) {
    // SERVO_PWM_PIN (27) is odd -> PWM channel B -> the CC register's upper 16 bits
    // (CC_A/channel A, the even-GPIO half, is bits [15:0] and isn't routed to any
    // pin now, so it's left untouched).
    hw_write_masked(
        &pwm_hw->slice[SERVO_PWM_SLICE_NUM].cc,
        ((uint32_t) duty_cycle) << 16,
        0xffff0000u
    );
}


static void _servo_gate_set_current_state(float open_ratio) {
    float range = servo_gate.eeprom_servo_gate_config.close_duty_cycle - servo_gate.eeprom_servo_gate_config.open_duty_cycle;
    uint16_t duty_cycle = _pwm_full_scale_level * (servo_gate.eeprom_servo_gate_config.open_duty_cycle + range * open_ratio);

    _set_duty_cycle(duty_cycle);
}



void servo_gate_set_ratio(gate_ratio_t ratio, bool block_wait) {
    float r = (ratio == HOLDER_RATIO_DISABLED) ? HOLDER_RATIO_DISABLED : clamp01(ratio);
    
    xSemaphoreTake(servo_gate.move_ready_semphore, 0); 
    
    xQueueOverwrite(servo_gate.control_queue, &r);

    if (block_wait) {
        xSemaphoreTake(servo_gate.move_ready_semphore, portMAX_DELAY);
    }
}

void servo_gate_control_task(void *p) {
    (void)p;

    const float UNKNOWN_RATIO = -2.0f;
    float prev_open_ratio = UNKNOWN_RATIO;

    while (true) {
        gate_ratio_t new_ratio;
        xQueueReceive(servo_gate.control_queue, &new_ratio, portMAX_DELAY);

        // --- DISABLE ---
        if (new_ratio == HOLDER_RATIO_DISABLED) {
            servo_gate.gate_state = HOLDER_DISABLED;

            // Do NOT modify prev_open_ratio
            xSemaphoreGive(servo_gate.move_ready_semphore);
            continue;
        }

        // Clamp to valid range
        float new_open_ratio = clamp01((float)new_ratio);

        // First valid move: set immediately
        if (prev_open_ratio == UNKNOWN_RATIO) {
            _servo_gate_set_current_state(new_open_ratio);
        } else {
            // Skip ramp if no change
            if (fabsf(new_open_ratio - prev_open_ratio) > 0.0001f) {

                float delta = new_open_ratio - prev_open_ratio;

                // 0 = open, 1 = closed
                float speed = (delta < 0.0f)
                    ? servo_gate.eeprom_servo_gate_config.open_speed_pct_s
                    : servo_gate.eeprom_servo_gate_config.close_speed_pct_s;

                if (speed < 0.0001f) speed = 0.0001f;

                uint32_t ramp_time_us = (uint32_t)(fabsf(delta / speed) * 1e6f);

                if (ramp_time_us < 1000) {
                    _servo_gate_set_current_state(new_open_ratio);
                } else {
                    uint32_t start_time = time_us_32();
                    uint32_t stop_time  = start_time + ramp_time_us;

                    while (true) {
                        uint32_t current_time = time_us_32();
                        if (current_time > stop_time) break;

                        float percentage = (current_time - start_time) / (float)ramp_time_us;
                        float current_ratio = prev_open_ratio + delta * percentage;

                        _servo_gate_set_current_state(current_ratio);
                    }

                    _servo_gate_set_current_state(new_open_ratio);
                }
            }
        }

        // Update discrete state for reporting/UI
        if (new_open_ratio <= 0.0001f) {
            servo_gate.gate_state = HOLDER_DROP;
        } else if (new_open_ratio >= 0.9999f) {
            servo_gate.gate_state = HOLDER_HOLD;
        }

        // Save last ratio
        prev_open_ratio = new_open_ratio;
        servo_gate.gate_ratio = (gate_ratio_t)new_open_ratio;

        // Signal completion
        xSemaphoreGive(servo_gate.move_ready_semphore);
    }
}


bool servo_gate_config_save(void) {
    bool is_ok = save_config(EEPROM_SERVO_GATE_CONFIG_BASE_ADDR, &servo_gate.eeprom_servo_gate_config, sizeof(servo_gate.eeprom_servo_gate_config));
    return is_ok;
}

bool servo_gate_config_init() {
    bool is_ok = true;

    // Read charge mode config from EEPROM
    memset(&servo_gate, 0x0, sizeof(servo_gate));
    is_ok = load_config(EEPROM_SERVO_GATE_CONFIG_BASE_ADDR, &servo_gate.eeprom_servo_gate_config, &default_eeprom_servo_gate_config, sizeof(servo_gate.eeprom_servo_gate_config), EEPROM_SERVO_GATE_CONFIG_REV);
    if (!is_ok) {
        printf("Unable to read servo gate configuration\n");
        return false;
    }

    // Register to eeprom save all
    eeprom_register_handler(servo_gate_config_save);

    // Initialize settings
    if (servo_gate.eeprom_servo_gate_config.servo_gate_enable) {
        servo_gate.gate_state = HOLDER_DROP;
    }
    else {
        servo_gate.gate_state = HOLDER_DISABLED;
    }

    return is_ok;
}


bool servo_gate_init() {
    bool is_ok = true;

    is_ok = servo_gate_config_init();

    // Initialize pin (only one physical servo is used)
    gpio_set_function(SERVO_PWM_PIN, GPIO_FUNC_PWM);

    pwm_config cfg = pwm_get_default_config();

    // Set to 50hz frequency
    uint32_t sys_freq = clock_get_hz(clk_sys);
    float divider = ceil(sys_freq / (4096 * _servo_pwm_freq)) / 16.0f;
    uint16_t wrap = sys_freq / divider / _servo_pwm_freq - 1;

    pwm_config_set_clkdiv(&cfg, divider);
    pwm_config_set_wrap(&cfg, wrap);

    pwm_init(pwm_gpio_to_slice_num(SERVO_PWM_PIN), &cfg, true);

    // Start the RTOS task and queue
    servo_gate.control_queue = xQueueCreate(1, sizeof(gate_ratio_t));
    servo_gate.move_ready_semphore = xSemaphoreCreateBinary();

    xTaskCreate(
        servo_gate_control_task,
        "servo_gate_controller",
        configMINIMAL_STACK_SIZE,
        NULL,
        8,
        &servo_gate.control_task_handler
    );

    // No, we don't set the servo gate state

    return is_ok;
}


bool http_rest_servo_gate_state(struct fs_file *file, int num_params, char *params[], char *values[]) {
    static char servo_gate_json_buffer[96];

    for (int idx = 0; idx < num_params; idx += 1) {
        if (strcmp(params[idx], "g0") == 0) {
            gate_state_t state = (gate_state_t)atoi(values[idx]);

            float ratio = HOLDER_RATIO_DISABLED;
            switch (state) {
                case HOLDER_DROP:     ratio = HOLDER_RATIO_DROP; break;
                case HOLDER_HOLD:     ratio = HOLDER_RATIO_HOLD; break;
                case HOLDER_DISABLED:
                default:              ratio = HOLDER_RATIO_DISABLED; break;
            }

            servo_gate_set_ratio(ratio, false);
        }
        else if (strcmp(params[idx], "r0") == 0) {
            float ratio = strtof(values[idx], NULL);
            servo_gate_set_ratio(ratio, false);
        }
    }

    snprintf(servo_gate_json_buffer,
             sizeof(servo_gate_json_buffer),
             "%s"
             "{\"g0\":%d}",
             http_json_header,
             (int)servo_gate.gate_state);

    size_t data_length = strlen(servo_gate_json_buffer);
    file->data = servo_gate_json_buffer;
    file->len = data_length;
    file->index = data_length;
    file->flags = FS_FILE_FLAGS_HEADER_INCLUDED;

    return true;
}


bool http_rest_servo_gate_config(struct fs_file *file, int num_params, char *params[], char *values[]) {
    // Mappings
    // c0 (bool): servo_gate_enable
    // c1 (float): close_duty_cycle
    // c2 (float): open_duty_cycle
    // c3 (float): close_speed_pct_s
    // c4 (float): open_speed_pct_s
    // ee (bool): save_to_eeprom

    static char servo_gate_json_buffer[256];
    bool save_to_eeprom = false;


    // Control
    for (int idx = 0; idx < num_params; idx += 1) {
        if (strcmp(params[idx], "c0") == 0) {
            bool enable = string_to_boolean(values[idx]);
            servo_gate.eeprom_servo_gate_config.servo_gate_enable = enable;
        }
        else if (strcmp(params[idx], "c1") == 0) {
            servo_gate.eeprom_servo_gate_config.close_duty_cycle = strtof(values[idx], NULL);
        }
        else if (strcmp(params[idx], "c2") == 0) {
            servo_gate.eeprom_servo_gate_config.open_duty_cycle = strtof(values[idx], NULL);
        }
        else if (strcmp(params[idx], "c3") == 0) {
            servo_gate.eeprom_servo_gate_config.close_speed_pct_s = strtof(values[idx], NULL);
        }
        else if (strcmp(params[idx], "c4") == 0) {
            servo_gate.eeprom_servo_gate_config.open_speed_pct_s = strtof(values[idx], NULL);
        }
        else if (strcmp(params[idx], "ee") == 0) {
            save_to_eeprom = string_to_boolean(values[idx]);
        }
    }

    // Perform action
    if (save_to_eeprom) {
        servo_gate_config_save();
    }

    // Response
    snprintf(servo_gate_json_buffer,
             sizeof(servo_gate_json_buffer),
             "%s"
             "{\"c0\":%s,\"c1\":%0.3f,\"c2\":%0.3f,\"c3\":%0.3f,\"c4\":%0.3f}",
             http_json_header,
             boolean_to_string(servo_gate.eeprom_servo_gate_config.servo_gate_enable),
             servo_gate.eeprom_servo_gate_config.close_duty_cycle,
             servo_gate.eeprom_servo_gate_config.open_duty_cycle,
             servo_gate.eeprom_servo_gate_config.close_speed_pct_s,
             servo_gate.eeprom_servo_gate_config.open_speed_pct_s);

    size_t data_length = strlen(servo_gate_json_buffer);
    file->data = servo_gate_json_buffer;
    file->len = data_length;
    file->index = data_length;
    file->flags = FS_FILE_FLAGS_HEADER_INCLUDED;

    return true;
}

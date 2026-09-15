#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <FreeRTOS.h>
#include <queue.h>
#include <task.h>
#include <semphr.h>
#include <u8g2.h>

#include "app.h"
#include "mini_12864_module.h"
#include "display.h"
#include "motors.h"
#include "servo_gate.h"
#include "induction_heater.h"
#include "anneal_mode.h"
#include "eeprom.h"
#include "neopixel_led.h"
#include "common.h"
#include "profile.h"
#include "ir_temp_sensor.h"


uint8_t anneal_cycle_count_digits[] = {0, 0, 0, 0};  // 4 digits (max 9999) - LCD entry only, REST/web have no such limit

anneal_mode_config_t anneal_mode_config;

extern servo_gate_t servo_gate;


const eeprom_anneal_mode_data_t default_anneal_mode_data = {
    .anneal_mode_data_rev = 0,

    .inter_cycle_delay_ms = 500,
    .cycle_count = 1,
    .use_temperature_mode = false,

    .neopixel_ready_colour = RGB_COLOUR_GREEN,
    .neopixel_heating_colour = RGB_COLOUR_RED,
    .neopixel_fault_colour = RGB_COLOUR_YELLOW,
};

// Menu system
extern AppState_t exit_state;
extern QueueHandle_t encoder_event_queue;
extern neopixel_led_config_t neopixel_led_config;

TaskHandle_t anneal_status_render_task_handler = NULL;
static char title_string[30];

static TickType_t heat_start_tick = 0;
static float last_heat_elapsed_seconds = 0.0f;

// Set when the induction heater's own hardware safety timer force-cuts the coil
// before a heat step finishes on its own terms - very likely to repeat identically on
// every remaining case in the run (nothing about the profile changes between cases),
// so this stops the whole batch after the current case finishes dropping normally
// rather than silently feeding more cases into the same problem. Consumed (and reset)
// by anneal_mode_cooldown(); also reset at the start of a fresh run in case a prior
// run ended some other way without going through cooldown.
static bool safety_limit_hit_last_case = false;

typedef enum {
    ANNEAL_MODE_EVENT_NO_EVENT = (1 << 0),
    ANNEAL_MODE_EVENT_INDUCTION_FAULT = (1 << 1),          // Coil safety cutoff fired in time-based
                                                            // mode - dwell_time_ms is very likely
                                                            // configured longer than the heater's
                                                            // own max_dwell_ms
    ANNEAL_MODE_EVENT_TEMP_SENSOR_FAULT = (1 << 2),        // Sensor went unhealthy mid-heat; that
                                                            // case's heat fell back to time-based dwell
    ANNEAL_MODE_EVENT_TEMP_TARGET_NOT_REACHED = (1 << 3),  // Coil safety cutoff fired in temperature
                                                            // mode - target_temp_c was never reached
} AnnealModeEventBit_t;


static const char * anneal_mode_state_to_string(anneal_mode_state_t state) {
    switch (state) {
        case ANNEAL_MODE_FEED:     return "Feeding";
        case ANNEAL_MODE_HOLD:     return "Positioning";
        case ANNEAL_MODE_HEAT:     return "Heating";
        case ANNEAL_MODE_DROP:     return "Dropping";
        case ANNEAL_MODE_COOLDOWN: return "Cooldown";
        case ANNEAL_MODE_EXIT:
        default:                  return "Idle";
    }
}


void anneal_status_render_task(void *p) {
    char progress_string[24];

    u8g2_t *display_handler = get_display_handler();

    while (true) {
        TickType_t last_render_tick = xTaskGetTickCount();

        u8g2_ClearBuffer(display_handler);

        u8g2_SetFont(display_handler, u8g2_font_helvB08_tr);
        u8g2_DrawStr(display_handler, 5, 10, title_string);

        u8g2_DrawHLine(display_handler, 0, 13, u8g2_GetDisplayWidth(display_handler));

        // Case progress: completed / target (or "completed / -" for continuous runs)
        memset(progress_string, 0x0, sizeof(progress_string));
        if (anneal_mode_config.eeprom_anneal_mode_data.cycle_count == 0) {
            snprintf(progress_string, sizeof(progress_string), "Case: %lu", anneal_mode_config.cases_completed);
        }
        else {
            snprintf(progress_string, sizeof(progress_string), "Case: %lu / %lu",
                     anneal_mode_config.cases_completed,
                     anneal_mode_config.eeprom_anneal_mode_data.cycle_count);
        }
        u8g2_SetFont(display_handler, u8g2_font_profont11_tf);
        u8g2_DrawStr(display_handler, 5, 30, progress_string);

        // Live dwell countdown while heating
        char dwell_string[24];
        memset(dwell_string, 0x0, sizeof(dwell_string));
        if (anneal_mode_config.anneal_mode_state == ANNEAL_MODE_HEAT) {
            float elapsed_seconds = (float)((xTaskGetTickCount() - heat_start_tick) * portTICK_PERIOD_MS) / 1000.0f;
            snprintf(dwell_string, sizeof(dwell_string), "Dwell: %.1f s", elapsed_seconds);
        }
        else {
            snprintf(dwell_string, sizeof(dwell_string), "Last dwell: %.1f s", last_heat_elapsed_seconds);
        }
        u8g2_DrawStr(display_handler, 5, 45, dwell_string);

        // Bottom line: coil status plus a live temperature readout whenever Milestone
        // 11's sensor is physically present - shown regardless of whether the cycle is
        // currently in time-based or temperature-based mode, combined into one line
        // since the display has no room to spare for a dedicated row.
        char status_string[24];
        memset(status_string, 0x0, sizeof(status_string));
        if (ir_temp_sensor_is_present()) {
            if (ir_temp_sensor_is_initializing()) {
                snprintf(status_string, sizeof(status_string), "Temp: init...");
            }
            else if (!ir_temp_sensor_is_healthy()) {
                snprintf(status_string, sizeof(status_string), "Temp: FAULT");
            }
            else {
                snprintf(status_string, sizeof(status_string), "%.1fC%s",
                         ir_temp_sensor_get_object_temp_c(),
                         induction_heater_is_active() ? " COIL ON" : "");
            }
        }
        else if (induction_heater_is_active()) {
            snprintf(status_string, sizeof(status_string), "COIL ON");
        }
        if (strlen(status_string)) {
            u8g2_DrawStr(display_handler, 5, 60, status_string);
        }

        u8g2_SendBuffer(display_handler);

        vTaskDelayUntil(&last_render_tick, pdMS_TO_TICKS(100));
    }
}


static void anneal_mode_hold(void) {
    neopixel_led_set_colour(
        neopixel_led_config.eeprom_neopixel_led_metadata.default_led_colours.mini12864_backlight_colour,
        anneal_mode_config.eeprom_anneal_mode_data.neopixel_ready_colour,
        anneal_mode_config.eeprom_anneal_mode_data.neopixel_ready_colour,
        true
    );

    snprintf(title_string, sizeof(title_string), "Positioning");

    ButtonEncoderEvent_t button_encoder_event = button_wait_for_input(false);
    if (button_encoder_event == BUTTON_RST_PRESSED) {
        anneal_mode_config.anneal_mode_state = ANNEAL_MODE_EXIT;
        return;
    }

    // The holder must be in position BEFORE the case is fed, not after - feeding
    // into a holder that's still in its dropped/clear position (left over from the
    // previous case) risks the incoming case missing the holder or binding against
    // it mid-move. Block until the move completes.
    //
    // Always the same fixed position, not per-profile: the holder is a swing arm with
    // a manual adjustment nut for case length, so there's only ever one "in" endpoint
    // for the servo to reach, regardless of which case type is loaded.
    servo_gate_set_ratio(HOLDER_RATIO_HOLD, true);

    anneal_mode_config.anneal_mode_state = ANNEAL_MODE_FEED;
}


static void anneal_mode_feed(void) {
    snprintf(title_string, sizeof(title_string), "Feeding Case");

    ButtonEncoderEvent_t button_encoder_event = button_wait_for_input(false);
    if (button_encoder_event == BUTTON_RST_PRESSED) {
        anneal_mode_config.anneal_mode_state = ANNEAL_MODE_EXIT;
        return;
    }

    profile_t * profile = profile_get_selected();
    motor_set_speed(SELECT_FEEDER_MOTOR, profile->feed_speed_rps);
    vTaskDelay(pdMS_TO_TICKS(profile->feed_run_time_ms));
    motor_set_speed(SELECT_FEEDER_MOTOR, 0);

    // Let the case finish settling into the already-positioned holder before
    // enabling the coil.
    vTaskDelay(pdMS_TO_TICKS(profile->pre_heat_settle_ms));

    anneal_mode_config.anneal_mode_state = ANNEAL_MODE_HEAT;
}


static void anneal_mode_heat(void) {
    heat_start_tick = xTaskGetTickCount();

    neopixel_led_set_colour(
        neopixel_led_config.eeprom_neopixel_led_metadata.default_led_colours.mini12864_backlight_colour,
        anneal_mode_config.eeprom_anneal_mode_data.neopixel_heating_colour,
        anneal_mode_config.eeprom_anneal_mode_data.neopixel_heating_colour,
        true
    );

    snprintf(title_string, sizeof(title_string), "Heating");

    induction_heater_enable(true);

    profile_t * profile = profile_get_selected();
    bool use_temp_target = anneal_mode_config.eeprom_anneal_mode_data.use_temperature_mode &&
        ir_temp_sensor_is_present() && profile->target_temp_c > 0.0f;
    const bool temp_mode_requested = use_temp_target;  // Immutable - see the safety-limit branch below

    // dwell_time_ms is a *time-mode* calibrated value (what Milestone 6's paint-based
    // calibration measures) - reusing it as a hard cap in temperature mode too would
    // arbitrarily cut a temp-mode cycle short at a duration that has nothing to do with
    // reaching the target. In temperature mode, the real ceiling is the induction
    // heater's own hardware max_dwell_ms safety timer instead (enforced independently
    // by induction_heater.c - this loop just notices via induction_heater_is_active()
    // going false below, same as the existing fault path). Confirmed on the bench:
    // with temp mode on and no case actually reaching target, this used to stop at
    // dwell_time_ms instead of running to the real safety limit.
    bool has_time_limit = !use_temp_target;
    TickType_t stop_tick = xTaskGetTickCount() + pdMS_TO_TICKS(profile->dwell_time_ms);
    bool aborted = false;

    while (!has_time_limit || xTaskGetTickCount() < stop_tick) {
        ButtonEncoderEvent_t button_encoder_event = button_wait_for_input(false);
        if (button_encoder_event == BUTTON_RST_PRESSED) {
            aborted = true;
            break;
        }

        // The induction heater's own hardware safety timer force-cut the coil before
        // this heat step finished on its own terms - in time-based mode that almost
        // always means dwell_time_ms is configured longer than the heater's own
        // max_dwell_ms; in temperature mode it means target_temp_c was never reached
        // in time. Either way this is very likely to repeat identically on every
        // remaining case in this run, so stop the whole batch (after this case still
        // drops normally below) rather than silently feeding more cases into the same
        // problem.
        if (!induction_heater_is_active()) {
            if (temp_mode_requested) {
                anneal_mode_config.anneal_mode_event |= ANNEAL_MODE_EVENT_TEMP_TARGET_NOT_REACHED;
            }
            else {
                anneal_mode_config.anneal_mode_event |= ANNEAL_MODE_EVENT_INDUCTION_FAULT;
            }
            safety_limit_hit_last_case = true;
            break;
        }

        if (use_temp_target) {
            if (ir_temp_sensor_is_healthy()) {
                if (ir_temp_sensor_get_object_temp_c() >= profile->target_temp_c) {
                    break;
                }
            }
            else {
                // Sensor faulted mid-heat (or is still in its brief initializing
                // window) - don't guess off a reading we can't currently trust. Flag
                // it and fall back to the fixed dwell_time_ms for this case, measured
                // from when heating started (stop_tick was already computed above).
                // If dwell_time_ms has already elapsed by this point (temp mode was
                // running unbounded until now), this ends the heat immediately rather
                // than let a now-untrusted cycle continue any further.
                anneal_mode_config.anneal_mode_event |= ANNEAL_MODE_EVENT_TEMP_SENSOR_FAULT;
                use_temp_target = false;
                has_time_limit = true;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }

    // Always disable explicitly, regardless of how the loop above ended - never rely
    // on the safety timer alone as the only thing turning the coil off on this path.
    induction_heater_enable(false);

    TickType_t now = xTaskGetTickCount();
    last_heat_elapsed_seconds = (float)((now - heat_start_tick) * portTICK_PERIOD_MS) / 1000.0f;

    if (aborted) {
        anneal_mode_config.anneal_mode_state = ANNEAL_MODE_EXIT;
        return;
    }

    anneal_mode_config.anneal_mode_state = ANNEAL_MODE_DROP;
}


static void anneal_mode_drop(void) {
    snprintf(title_string, sizeof(title_string), "Dropping Case");

    // Unlike OpenTrickler's optional accessory gate, the case holder servo is not
    // optional hardware for this application - always drop, regardless of
    // servo_gate_enable (which defaults to false and would otherwise silently skip
    // this move every time).
    servo_gate_set_ratio(HOLDER_RATIO_DROP, true);

    vTaskDelay(pdMS_TO_TICKS(profile_get_selected()->post_heat_delay_ms));

    anneal_mode_config.cases_completed += 1;

    anneal_mode_config.anneal_mode_state = ANNEAL_MODE_COOLDOWN;
}


static void anneal_mode_cooldown(void) {
    if (safety_limit_hit_last_case) {
        // The case that just heated has already dropped normally (see
        // anneal_mode_drop()) - this only stops the batch from continuing to feed and
        // heat another one the same likely-to-fail way. See the comment where this
        // flag is set in anneal_mode_heat().
        safety_limit_hit_last_case = false;
        anneal_mode_config.anneal_mode_state = ANNEAL_MODE_EXIT;
        return;
    }

    uint32_t target = anneal_mode_config.eeprom_anneal_mode_data.cycle_count;
    if (target != 0 && anneal_mode_config.cases_completed >= target) {
        anneal_mode_config.anneal_mode_state = ANNEAL_MODE_EXIT;
        return;
    }

    snprintf(title_string, sizeof(title_string), "Next Case In...");

    TickType_t stop_tick = xTaskGetTickCount() + pdMS_TO_TICKS(anneal_mode_config.eeprom_anneal_mode_data.inter_cycle_delay_ms);
    while (xTaskGetTickCount() < stop_tick) {
        ButtonEncoderEvent_t button_encoder_event = button_wait_for_input(false);
        if (button_encoder_event == BUTTON_RST_PRESSED) {
            anneal_mode_config.anneal_mode_state = ANNEAL_MODE_EXIT;
            return;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }

    anneal_mode_config.anneal_mode_state = ANNEAL_MODE_HOLD;
}


uint8_t anneal_mode_menu(bool anneal_mode_skip_user_input) {
    if (!anneal_mode_skip_user_input) {
        anneal_mode_config.eeprom_anneal_mode_data.cycle_count = anneal_cycle_count_digits[3] * 1000 +
                                                                   anneal_cycle_count_digits[2] * 100 +
                                                                   anneal_cycle_count_digits[1] * 10 +
                                                                   anneal_cycle_count_digits[0];
    }

    if (anneal_status_render_task_handler == NULL) {
        UBaseType_t current_task_priority = uxTaskPriorityGet(xTaskGetCurrentTaskHandle());
        xTaskCreate(anneal_status_render_task, "Anneal Status Render Task", configMINIMAL_STACK_SIZE, NULL, current_task_priority - 1, &anneal_status_render_task_handler);
    }
    else {
        vTaskResume(anneal_status_render_task_handler);
    }

    motor_enable(SELECT_FEEDER_MOTOR, true);

    anneal_mode_config.cases_completed = 0;
    anneal_mode_config.anneal_mode_state = ANNEAL_MODE_HOLD;
    safety_limit_hit_last_case = false;  // Belt-and-suspenders: a prior run that ended via
                                          // RST (never reaching cooldown, which normally
                                          // consumes this) shouldn't bleed into a new run.

    bool quit = false;
    while (!quit) {
        switch (anneal_mode_config.anneal_mode_state) {
            case ANNEAL_MODE_FEED:
                anneal_mode_feed();
                break;
            case ANNEAL_MODE_HOLD:
                anneal_mode_hold();
                break;
            case ANNEAL_MODE_HEAT:
                anneal_mode_heat();
                break;
            case ANNEAL_MODE_DROP:
                anneal_mode_drop();
                break;
            case ANNEAL_MODE_COOLDOWN:
                anneal_mode_cooldown();
                break;
            case ANNEAL_MODE_EXIT:
            default:
                quit = true;
                break;
        }
    }

    // Safety: make certain the coil and feeder are off no matter which state we exited from
    induction_heater_enable(false);
    motor_enable(SELECT_FEEDER_MOTOR, false);

    neopixel_led_set_colour(neopixel_led_config.eeprom_neopixel_led_metadata.default_led_colours.mini12864_backlight_colour,
                            neopixel_led_config.eeprom_neopixel_led_metadata.default_led_colours.led1_colour,
                            neopixel_led_config.eeprom_neopixel_led_metadata.default_led_colours.led2_colour,
                            true);

    vTaskSuspend(anneal_status_render_task_handler);

    return 1;  // Return back to main menu
}


bool anneal_mode_config_init(void) {
    bool is_ok = load_config(EEPROM_ANNEAL_MODE_BASE_ADDR, &anneal_mode_config.eeprom_anneal_mode_data, &default_anneal_mode_data, sizeof(anneal_mode_config.eeprom_anneal_mode_data), EEPROM_ANNEAL_MODE_DATA_REV);
    if (!is_ok) {
        printf("Unable to read anneal mode configuration\n");
        return is_ok;
    }

    eeprom_register_handler(anneal_mode_config_save);

    return true;
}


bool anneal_mode_config_save(void) {
    bool is_ok = save_config(EEPROM_ANNEAL_MODE_BASE_ADDR, &anneal_mode_config.eeprom_anneal_mode_data, sizeof(eeprom_anneal_mode_data_t));
    return is_ok;
}


bool http_rest_anneal_mode_config(struct fs_file *file, int num_params, char *params[], char *values[]) {
    // Mappings:
    // (feed/holder/dwell timing moved to /rest/profile_config - see profile.c)
    // c0 (int): inter_cycle_delay_ms
    // c1 (int): cycle_count
    // c2 (str): neopixel_ready_colour
    // c3 (str): neopixel_heating_colour
    // c4 (str): neopixel_fault_colour
    // c5 (bool): use_temperature_mode - only applied while no cycle is running (silently
    //            ignored mid-cycle, matching the web UI disabling the control while running)
    // ee (bool): save to eeprom

    static char json_buffer[256];
    bool save_to_eeprom = false;

    for (int idx = 0; idx < num_params; idx += 1) {
        if (strcmp(params[idx], "c0") == 0) {
            anneal_mode_config.eeprom_anneal_mode_data.inter_cycle_delay_ms = strtoul(values[idx], NULL, 10);
        }
        else if (strcmp(params[idx], "c1") == 0) {
            anneal_mode_config.eeprom_anneal_mode_data.cycle_count = strtoul(values[idx], NULL, 10);
        }
        else if (strcmp(params[idx], "c2") == 0) {
            anneal_mode_config.eeprom_anneal_mode_data.neopixel_ready_colour._raw_colour = hex_string_to_decimal(values[idx]);
        }
        else if (strcmp(params[idx], "c3") == 0) {
            anneal_mode_config.eeprom_anneal_mode_data.neopixel_heating_colour._raw_colour = hex_string_to_decimal(values[idx]);
        }
        else if (strcmp(params[idx], "c4") == 0) {
            anneal_mode_config.eeprom_anneal_mode_data.neopixel_fault_colour._raw_colour = hex_string_to_decimal(values[idx]);
        }
        else if (strcmp(params[idx], "c5") == 0) {
            if (anneal_mode_config.anneal_mode_state == ANNEAL_MODE_EXIT) {
                anneal_mode_config.eeprom_anneal_mode_data.use_temperature_mode = string_to_boolean(values[idx]);
            }
        }
        else if (strcmp(params[idx], "ee") == 0) {
            save_to_eeprom = string_to_boolean(values[idx]);
        }
    }

    if (save_to_eeprom) {
        anneal_mode_config_save();
    }

    snprintf(json_buffer,
             sizeof(json_buffer),
             "%s"
             "{\"c0\":%lu,\"c1\":%lu,\"c2\":\"#%06lx\",\"c3\":\"#%06lx\",\"c4\":\"#%06lx\",\"c5\":%s}",
             http_json_header,
             anneal_mode_config.eeprom_anneal_mode_data.inter_cycle_delay_ms,
             anneal_mode_config.eeprom_anneal_mode_data.cycle_count,
             anneal_mode_config.eeprom_anneal_mode_data.neopixel_ready_colour._raw_colour,
             anneal_mode_config.eeprom_anneal_mode_data.neopixel_heating_colour._raw_colour,
             anneal_mode_config.eeprom_anneal_mode_data.neopixel_fault_colour._raw_colour,
             boolean_to_string(anneal_mode_config.eeprom_anneal_mode_data.use_temperature_mode));

    size_t data_length = strlen(json_buffer);
    file->data = json_buffer;
    file->len = data_length;
    file->index = data_length;
    file->flags = FS_FILE_FLAGS_HEADER_INCLUDED;

    return true;
}


bool http_rest_anneal_mode_state(struct fs_file *file, int num_params, char *params[], char *values[]) {
    // Mappings:
    // s0 (int): cycle count set point (overrides eeprom_anneal_mode_data.cycle_count for this run)
    // s1 (int): cases completed (read-only)
    // s2 (anneal_mode_state_t | int): mode state
    // s3 (uint32_t): anneal mode event bitmask (cleared after read)
    // s4 (string): human-readable current phase
    // s5 (string): elapsed dwell time in seconds (live while heating, last value otherwise)

    static char json_buffer[192];
    char elapsed_time_buffer[16] = {0};

    for (int idx = 0; idx < num_params; idx += 1) {
        if (strcmp(params[idx], "s0") == 0) {
            anneal_mode_config.eeprom_anneal_mode_data.cycle_count = strtoul(values[idx], NULL, 10);
        }
        else if (strcmp(params[idx], "s2") == 0) {
            anneal_mode_state_t new_state = (anneal_mode_state_t) atoi(values[idx]);

            if (new_state == ANNEAL_MODE_EXIT && anneal_mode_config.anneal_mode_state != ANNEAL_MODE_EXIT) {
                ButtonEncoderEvent_t button_event = BUTTON_RST_PRESSED;
                xQueueSend(encoder_event_queue, &button_event, portMAX_DELAY);
            }
            else if (new_state == ANNEAL_MODE_HOLD && anneal_mode_config.anneal_mode_state == ANNEAL_MODE_EXIT) {
                exit_state = APP_STATE_ENTER_ANNEAL_MODE_FROM_REST;

                ButtonEncoderEvent_t button_event = OVERRIDE_FROM_REST;
                xQueueSend(encoder_event_queue, &button_event, portMAX_DELAY);
            }

            anneal_mode_config.anneal_mode_state = new_state;
        }
    }

    if (anneal_mode_config.anneal_mode_state == ANNEAL_MODE_HEAT) {
        float elapsed_seconds = (float)((xTaskGetTickCount() - heat_start_tick) * portTICK_PERIOD_MS) / 1000.0f;
        snprintf(elapsed_time_buffer, sizeof(elapsed_time_buffer), "%.2f", elapsed_seconds);
    }
    else {
        snprintf(elapsed_time_buffer, sizeof(elapsed_time_buffer), "%.2f", last_heat_elapsed_seconds);
    }

    snprintf(json_buffer,
             sizeof(json_buffer),
             "%s"
             "{\"s0\":%lu,\"s1\":%lu,\"s2\":%d,\"s3\":%lu,\"s4\":\"%s\",\"s5\":\"%s\"}",
             http_json_header,
             anneal_mode_config.eeprom_anneal_mode_data.cycle_count,
             anneal_mode_config.cases_completed,
             (int) anneal_mode_config.anneal_mode_state,
             anneal_mode_config.anneal_mode_event,
             anneal_mode_state_to_string(anneal_mode_config.anneal_mode_state),
             elapsed_time_buffer);

    anneal_mode_config.anneal_mode_event = 0;

    size_t data_length = strlen(json_buffer);
    file->data = json_buffer;
    file->len = data_length;
    file->index = data_length;
    file->flags = FS_FILE_FLAGS_HEADER_INCLUDED;

    return true;
}

#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <string.h>
#include <stdio.h>
#include <u8g2.h>

#include "app.h"
#include "display.h"
#include "mini_12864_module.h"
#include "motors.h"
#include "servo_gate.h"
#include "induction_heater.h"
#include "profile.h"
#include "anneal_calibration_mode.h"
#include "common.h"
#include "ir_temp_sensor.h"

extern QueueHandle_t encoder_event_queue;
extern AppState_t exit_state;

static char title_string[32] = "";
static char line1[32] = "";
static char line2[32] = "";
static bool show_next_key = false;

static anneal_calibration_state_t anneal_calibration_state = ANNEAL_CALIBRATION_EXIT;
static float last_measured_dwell_seconds = 0.0f;

TaskHandle_t anneal_calibration_render_task_handler = NULL;


void anneal_calibration_render_task(void *p) {
    while (true) {
        TickType_t last_render_tick = xTaskGetTickCount();
        u8g2_t * display_handler = get_display_handler();

        u8g2_ClearBuffer(display_handler);

        if (strlen(title_string)) {
            u8g2_SetFont(display_handler, u8g2_font_helvB08_tr);
            u8g2_DrawStr(display_handler, 5, 10, title_string);
        }

        u8g2_DrawHLine(display_handler, 0, 13, u8g2_GetDisplayWidth(display_handler));

        u8g2_SetFont(display_handler, u8g2_font_helvR08_tr);
        if (strlen(line1)) {
            u8g2_DrawStr(display_handler, 5, 25, line1);
        }
        if (strlen(line2)) {
            u8g2_DrawStr(display_handler, 5, 37, line2);
        }

        if (show_next_key) {
            u8g2_DrawButtonUTF8(display_handler, 64, 59, U8G2_BTN_HCENTER | U8G2_BTN_INV | U8G2_BTN_BW1, 0, 1, 1, "Next");
        }

        u8g2_SendBuffer(display_handler);

        vTaskDelayUntil(&last_render_tick, pdMS_TO_TICKS(50));
    }
}


uint8_t anneal_calibration_mode_menu() {
    if (anneal_calibration_render_task_handler == NULL) {
        UBaseType_t current_task_priority = uxTaskPriorityGet(xTaskGetCurrentTaskHandle());
        xTaskCreate(anneal_calibration_render_task, "Anneal Calibration Render Task", configMINIMAL_STACK_SIZE, NULL, current_task_priority - 1, &anneal_calibration_render_task_handler);
    }
    else {
        vTaskResume(anneal_calibration_render_task_handler);
    }

    anneal_calibration_state = ANNEAL_CALIBRATION_ENTER;

    profile_t * profile = profile_get_selected();

    // Position the holder BEFORE feeding a case in - matches anneal_mode's own
    // ordering (see anneal_mode_hold()'s comment): feeding into a holder that's still
    // dropped/clear risks the case missing the holder or binding against it mid-move.
    // Always the same fixed "hold" endpoint, not per-profile - the holder is a swing
    // arm with a manual adjustment nut for case length.
    strcpy(title_string, "Calibrating");
    strcpy(line1, "Positioning...");
    memset(line2, 0x0, sizeof(line2));
    show_next_key = false;

    servo_gate_set_ratio(HOLDER_RATIO_HOLD, true);

    // Feed a case in, same as anneal_mode's own feed step.
    strcpy(line1, "Feeding case...");
    motor_enable(SELECT_FEEDER_MOTOR, true);
    motor_set_speed(SELECT_FEEDER_MOTOR, profile->feed_speed_rps);
    vTaskDelay(pdMS_TO_TICKS(profile->feed_run_time_ms));
    motor_set_speed(SELECT_FEEDER_MOTOR, 0);
    motor_enable(SELECT_FEEDER_MOTOR, false);

    // Let the case finish settling into the holder the same way a real cycle would.
    vTaskDelay(pdMS_TO_TICKS(profile->pre_heat_settle_ms));

    strcpy(title_string, "Watch the paint");
    strcpy(line1, "Press knob when");
    strcpy(line2, "colour changes");
    vTaskDelay(pdMS_TO_TICKS(1500));  // Give the operator a moment to read the prompt

    TickType_t heat_start_tick = xTaskGetTickCount();
    induction_heater_enable(true);

    bool aborted = false;
    bool timed_out = false;

    while (true) {
        ButtonEncoderEvent_t button_encoder_event = button_wait_for_input(false);

        if (button_encoder_event == BUTTON_RST_PRESSED) {
            aborted = true;
            break;
        }
        if (button_encoder_event == BUTTON_ENCODER_PRESSED) {
            break;
        }

        // The induction heater's own max_dwell_ms safety timer force-cut the coil -
        // this run didn't get a real reading, not a legitimate calibration result.
        if (!induction_heater_is_active()) {
            timed_out = true;
            break;
        }

        float elapsed_seconds = (float)((xTaskGetTickCount() - heat_start_tick) * portTICK_PERIOD_MS) / 1000.0f;
        snprintf(line1, sizeof(line1), "Time: %.1f s", elapsed_seconds);

        // Optional live temperature alongside the stopwatch (Milestone 11) - purely
        // informational here, the paint colour change is still what actually ends
        // this run; the operator can use this to sanity-check against the paint.
        memset(line2, 0x0, sizeof(line2));
        if (ir_temp_sensor_is_enabled()) {
            if (ir_temp_sensor_is_healthy()) {
                snprintf(line2, sizeof(line2), "Temp: %.1f C", ir_temp_sensor_get_object_temp_c());
            }
            else {
                strcpy(line2, "Temp: --");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }

    // Always disable explicitly, regardless of how the loop above ended.
    induction_heater_enable(false);

    TickType_t heat_end_tick = xTaskGetTickCount();
    last_measured_dwell_seconds = (float)((heat_end_tick - heat_start_tick) * portTICK_PERIOD_MS) / 1000.0f;

    // Drop the case now that heating is done, whether or not the run was valid.
    servo_gate_set_ratio(HOLDER_RATIO_DROP, true);

    if (aborted) {
        strcpy(title_string, "Calibration aborted");
        memset(line1, 0x0, sizeof(line1));
        memset(line2, 0x0, sizeof(line2));
        vTaskDelay(pdMS_TO_TICKS(1500));

        anneal_calibration_state = ANNEAL_CALIBRATION_EXIT;
        vTaskSuspend(anneal_calibration_render_task_handler);
        return 1;
    }

    if (timed_out) {
        strcpy(title_string, "Calibration FAILED");
        strcpy(line1, "Coil safety timeout -");
        strcpy(line2, "not saved. Check max");
        vTaskDelay(pdMS_TO_TICKS(3000));

        anneal_calibration_state = ANNEAL_CALIBRATION_EXIT;
        vTaskSuspend(anneal_calibration_render_task_handler);
        return 1;
    }

    // Result screen: offer to save into the selected profile's dwell_time_ms.
    strcpy(title_string, "Result");
    snprintf(line1, sizeof(line1), "Dwell: %.2f s", last_measured_dwell_seconds);
    strcpy(line2, "Press: Save  RST: Discard");
    show_next_key = false;

    while (true) {
        ButtonEncoderEvent_t ev = button_wait_for_input(true);
        if (ev == BUTTON_RST_PRESSED) {
            break;
        }
        if (ev == BUTTON_ENCODER_PRESSED) {
            profile->dwell_time_ms = (uint32_t)(last_measured_dwell_seconds * 1000.0f);
            profile_data_save();

            strcpy(title_string, "Saved");
            memset(line1, 0x0, sizeof(line1));
            memset(line2, 0x0, sizeof(line2));
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;
        }
    }

    anneal_calibration_state = ANNEAL_CALIBRATION_EXIT;
    vTaskSuspend(anneal_calibration_render_task_handler);

    return 1;  // Return back to main menu
}


bool http_rest_anneal_calibration_state(struct fs_file *file, int num_params, char *params[], char *values[]) {
    // Mappings:
    // s0 (anneal_calibration_state_t | int): enter/exit calibration mode
    // s1 (float, read-only): last measured dwell time in seconds

    static char json_buffer[128];

    for (int idx = 0; idx < num_params; idx += 1) {
        if (strcmp(params[idx], "s0") == 0) {
            anneal_calibration_state_t new_state = (anneal_calibration_state_t) atoi(values[idx]);

            if (new_state == ANNEAL_CALIBRATION_ENTER && anneal_calibration_state != ANNEAL_CALIBRATION_ENTER) {
                exit_state = APP_STATE_ENTER_CASE_CALIBRATION_MODE;

                ButtonEncoderEvent_t button_event = OVERRIDE_FROM_REST;
                xQueueSend(encoder_event_queue, &button_event, portMAX_DELAY);
            }
            else if (new_state == ANNEAL_CALIBRATION_EXIT && anneal_calibration_state != ANNEAL_CALIBRATION_EXIT) {
                // Same abort path as a local RST press - cuts power first regardless
                // of which state (heating, result screen) it's currently in.
                ButtonEncoderEvent_t button_event = BUTTON_RST_PRESSED;
                xQueueSend(encoder_event_queue, &button_event, portMAX_DELAY);
            }
        }
    }

    snprintf(json_buffer,
             sizeof(json_buffer),
             "%s"
             "{\"s0\":%d,\"s1\":%0.2f}",
             http_json_header,
             (int) anneal_calibration_state,
             last_measured_dwell_seconds);

    size_t data_length = strlen(json_buffer);
    file->data = json_buffer;
    file->len = data_length;
    file->index = data_length;
    file->flags = FS_FILE_FLAGS_HEADER_INCLUDED;

    return true;
}

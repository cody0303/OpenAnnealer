#include <FreeRTOS.h>
#include <queue.h>
#include <task.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "app.h"
#include "u8g2.h"
#include "mini_12864_module.h"
#include "motors.h"
#include "display.h"
#include "common.h"
#include "anneal_manual_mode.h"
#include "servo_gate.h"
#include "induction_heater.h"

// Memory from other modules
extern QueueHandle_t encoder_event_queue;
extern servo_gate_t servo_gate;
extern induction_heater_t induction_heater;
extern AppState_t exit_state;

// Internal
anneal_manual_mode_config_t anneal_manual_mode_config;

#define FEED_SPEED_STEP_RPS 0.5f

// The encoder only gives us three physical gestures (rotate CW/CCW, press) plus RST -
// not enough to give feed-jog, holder-toggle, AND coil-pulse each their own gesture on
// one screen without ambiguity. Split into a small menu (rotate to pick, press to
// enter) plus one screen per action (RST always backs out to the menu), same
// navigation idiom used by every other menu in this app - this is also what actually
// makes the coil reachable from the LCD at all; it was REST-only before.
typedef enum {
    MANUAL_SCREEN_MENU = 0,
    MANUAL_SCREEN_JOG,
    MANUAL_SCREEN_HOLDER,
    MANUAL_SCREEN_COIL,
} manual_screen_t;

#define MANUAL_MENU_ITEM_COUNT 3
static const char * const manual_menu_items[MANUAL_MENU_ITEM_COUNT] = {
    "Feed Jog",
    "Case Holder",
    "Pulse Coil",
};

static manual_screen_t manual_screen = MANUAL_SCREEN_MENU;
static uint8_t manual_menu_cursor = 0;

static char title_string[30];
TaskHandle_t anneal_manual_render_task_handler = NULL;


void anneal_manual_render_task(void *p) {
    char buf[32];

    u8g2_t * display_handler = get_display_handler();

    while (true) {
        TickType_t last_render_tick = xTaskGetTickCount();

        u8g2_ClearBuffer(display_handler);

        // Draw title
        if (strlen(title_string)) {
            u8g2_SetFont(display_handler, u8g2_font_helvB08_tr);
            u8g2_DrawStr(display_handler, 5, 10, title_string);
        }

        // Draw line
        u8g2_DrawHLine(display_handler, 0, 13, u8g2_GetDisplayWidth(display_handler));

        u8g2_SetFont(display_handler, u8g2_font_profont11_tf);

        switch (manual_screen) {
            case MANUAL_SCREEN_MENU:
                for (int idx = 0; idx < MANUAL_MENU_ITEM_COUNT; idx += 1) {
                    memset(buf, 0x0, sizeof(buf));
                    snprintf(buf, sizeof(buf), "%s%s", (idx == manual_menu_cursor) ? "> " : "  ", manual_menu_items[idx]);
                    u8g2_DrawStr(display_handler, 5, 25 + idx * 12, buf);
                }
                u8g2_DrawStr(display_handler, 5, 61, "Press: select  RST: exit");
                break;

            case MANUAL_SCREEN_JOG:
                memset(buf, 0x0, sizeof(buf));
                sprintf(buf, "Feed speed: %0.2f", anneal_manual_mode_config.feed_speed_rps);
                u8g2_DrawStr(display_handler, 5, 25, buf);
                u8g2_DrawStr(display_handler, 5, 61, "RST: back");
                break;

            case MANUAL_SCREEN_HOLDER:
                memset(buf, 0x0, sizeof(buf));
                sprintf(buf, "Holder: %s", gate_state_to_string(servo_gate.gate_state));
                u8g2_DrawStr(display_handler, 5, 25, buf);
                u8g2_DrawStr(display_handler, 5, 49, "Press: hold/drop");
                u8g2_DrawStr(display_handler, 5, 61, "RST: back");
                break;

            case MANUAL_SCREEN_COIL:
                memset(buf, 0x0, sizeof(buf));
                sprintf(buf, "Coil: %s%s", induction_heater_is_active() ? "ON" : "off",
                        induction_heater.safety_cutoff_fault ? " (fault)" : "");
                u8g2_DrawStr(display_handler, 5, 25, buf);
                u8g2_DrawStr(display_handler, 5, 49, "Press: pulse coil");
                u8g2_DrawStr(display_handler, 5, 61, "RST: back");
                break;
        }

        u8g2_SendBuffer(display_handler);

        vTaskDelayUntil(&last_render_tick, pdMS_TO_TICKS(50));
    }
}


static void anneal_manual_mode_set_holder(bool held) {
    if (servo_gate.gate_state == HOLDER_DISABLED) {
        return;
    }
    anneal_manual_mode_config.holder_held = held;
    servo_gate_set_ratio(held ? HOLDER_RATIO_HOLD : HOLDER_RATIO_DROP, true);
}


uint8_t anneal_manual_mode_menu() {
    // If the display task is never created then we shall create one, otherwise we shall resume the task
    if (anneal_manual_render_task_handler == NULL) {
        // The render task shall have lower priority than the current one
        UBaseType_t current_task_priority = uxTaskPriorityGet(xTaskGetCurrentTaskHandle());
        xTaskCreate(anneal_manual_render_task, "Manual Render Task", configMINIMAL_STACK_SIZE, NULL, current_task_priority - 1, &anneal_manual_render_task_handler);
    }
    else {
        vTaskResume(anneal_manual_render_task_handler);
    }

    // Initialize the manual mode config
    memset(&anneal_manual_mode_config, 0x0, sizeof(anneal_manual_mode_config));

    // Enter manual mode
    anneal_manual_mode_config.manual_mode_state = ANNEAL_MANUAL_MODE_ENTER;

    // Only the feeder motor is driven in this application; the spare motor is unused.
    motor_enable(SELECT_FEEDER_MOTOR, true);

    // Start with the holder clear (if enabled)
    anneal_manual_mode_set_holder(false);

    // Always start at the top-level menu, not wherever a previous session left off.
    manual_screen = MANUAL_SCREEN_MENU;
    manual_menu_cursor = 0;

    // Update current status
    snprintf(title_string, sizeof(title_string), "Manual / Commissioning");

    bool quit = false;
    while (!quit) {
        // Wait if button is pressed
        ButtonEncoderEvent_t button_encoder_event;
        xQueueReceive(encoder_event_queue, &button_encoder_event, portMAX_DELAY);

        switch (manual_screen) {
            case MANUAL_SCREEN_MENU:
                switch (button_encoder_event) {
                    case BUTTON_RST_PRESSED:
                        quit = true;
                        break;
                    case BUTTON_ENCODER_ROTATE_CW:
                        manual_menu_cursor = (manual_menu_cursor + 1) % MANUAL_MENU_ITEM_COUNT;
                        break;
                    case BUTTON_ENCODER_ROTATE_CCW:
                        manual_menu_cursor = (manual_menu_cursor + MANUAL_MENU_ITEM_COUNT - 1) % MANUAL_MENU_ITEM_COUNT;
                        break;
                    case BUTTON_ENCODER_PRESSED:
                        // MANUAL_SCREEN_JOG/HOLDER/COIL are 1/2/3, in the same order
                        // as manual_menu_items - see the enum's own comment.
                        manual_screen = (manual_screen_t) (MANUAL_SCREEN_JOG + manual_menu_cursor);
                        break;
                    default:
                        break;
                }
                break;

            case MANUAL_SCREEN_JOG:
                switch (button_encoder_event) {
                    case BUTTON_RST_PRESSED:
                        anneal_manual_mode_config.feed_speed_rps = 0;
                        motor_set_speed(SELECT_FEEDER_MOTOR, 0);
                        manual_screen = MANUAL_SCREEN_MENU;
                        break;
                    case BUTTON_ENCODER_ROTATE_CW:
                        anneal_manual_mode_config.feed_speed_rps += FEED_SPEED_STEP_RPS;
                        motor_set_speed(SELECT_FEEDER_MOTOR, anneal_manual_mode_config.feed_speed_rps);
                        break;
                    case BUTTON_ENCODER_ROTATE_CCW:
                        anneal_manual_mode_config.feed_speed_rps -= FEED_SPEED_STEP_RPS;
                        motor_set_speed(SELECT_FEEDER_MOTOR, anneal_manual_mode_config.feed_speed_rps);
                        break;
                    default:
                        break;
                }
                break;

            case MANUAL_SCREEN_HOLDER:
                switch (button_encoder_event) {
                    case BUTTON_RST_PRESSED:
                        manual_screen = MANUAL_SCREEN_MENU;
                        break;
                    case BUTTON_ENCODER_PRESSED:
                        // Toggle the case holder servo between hold/drop - useful for
                        // checking the mechanism seats/clears cases correctly before
                        // running full cycles.
                        anneal_manual_mode_set_holder(!anneal_manual_mode_config.holder_held);
                        break;
                    default:
                        break;
                }
                break;

            case MANUAL_SCREEN_COIL:
                switch (button_encoder_event) {
                    case BUTTON_RST_PRESSED:
                        // Always leave the coil off when backing out of this screen -
                        // same reasoning as forcing it off on full exit below.
                        induction_heater_enable(false);
                        manual_screen = MANUAL_SCREEN_MENU;
                        break;
                    case BUTTON_ENCODER_PRESSED:
                        // Bounded entirely by the induction heater's own max_dwell_ms
                        // safety timer, same as every other trigger path (REST h0,
                        // the Settings > Induction Heater LCD form, the web UI).
                        induction_heater_enable(true);
                        break;
                    default:
                        break;
                }
                break;
        }
    }

    motor_enable(SELECT_FEEDER_MOTOR, false);

    // Always leave the coil off on exit - this mode is exactly where someone might
    // have pulsed it via REST for bench testing and forgotten about it.
    induction_heater_enable(false);

    anneal_manual_mode_config.manual_mode_state = ANNEAL_MANUAL_MODE_EXIT;

    vTaskSuspend(anneal_manual_render_task_handler);
    return 1;  // Return backs to the main menu view
}


bool http_rest_manual_mode_state(struct fs_file *file, int num_params, char *params[], char *values[]) {
    // Mappings
    // s0 (anneal_manual_mode_state_t | int): Manual mode state (enter/exit)
    // s1 (float): Feed speed (rps)
    // g0 (bool): Holder held (true) / dropped (false)
    // h0 (bool): Pulse the induction coil on/off for bench testing - bounded by the
    //            induction heater's own max_dwell_ms safety timer regardless.

    static char manual_mode_json_buffer[192];

    // Control
    for (int idx = 0; idx < num_params; idx += 1) {
        if (strcmp(params[idx], "s0") == 0) {
            anneal_manual_mode_state_t new_state = (anneal_manual_mode_state_t) atoi(values[idx]);

            // Exit
            if (new_state == ANNEAL_MANUAL_MODE_EXIT && anneal_manual_mode_config.manual_mode_state != ANNEAL_MANUAL_MODE_EXIT) {
                ButtonEncoderEvent_t button_event = BUTTON_RST_PRESSED;
                xQueueSend(encoder_event_queue, &button_event, portMAX_DELAY);
            }

            // Enter
            else if (new_state == ANNEAL_MANUAL_MODE_ENTER && anneal_manual_mode_config.manual_mode_state != ANNEAL_MANUAL_MODE_ENTER) {
                // Set exit_status for the menu
                exit_state = APP_STATE_ENTER_MANUAL_MODE;

                // Then signal the menu to stop
                ButtonEncoderEvent_t button_event = OVERRIDE_FROM_REST;
                xQueueSend(encoder_event_queue, &button_event, portMAX_DELAY);
            }

            anneal_manual_mode_config.manual_mode_state = new_state;
        }
        else if (strcmp(params[idx], "s1") == 0) {
            anneal_manual_mode_config.feed_speed_rps = strtof(values[idx], NULL);
            motor_set_speed(SELECT_FEEDER_MOTOR, anneal_manual_mode_config.feed_speed_rps);
        }
        else if (strcmp(params[idx], "g0") == 0) {
            anneal_manual_mode_set_holder(string_to_boolean(values[idx]));
        }
        else if (strcmp(params[idx], "h0") == 0) {
            induction_heater_enable(string_to_boolean(values[idx]));
        }
    }

    // Response
    snprintf(manual_mode_json_buffer,
             sizeof(manual_mode_json_buffer),
             "%s"
             "{\"s0\":%d,\"s1\":%0.3f,\"g0\":%s,\"h0\":%s,\"h0_fault\":%s}",
             http_json_header,
             (int) anneal_manual_mode_config.manual_mode_state,
             anneal_manual_mode_config.feed_speed_rps,
             boolean_to_string(anneal_manual_mode_config.holder_held),
             boolean_to_string(induction_heater_is_active()),
             boolean_to_string(induction_heater.safety_cutoff_fault));


    size_t data_length = strlen(manual_mode_json_buffer);
    file->data = manual_mode_json_buffer;
    file->len = data_length;
    file->index = data_length;
    file->flags = FS_FILE_FLAGS_HEADER_INCLUDED;


    return true;
}

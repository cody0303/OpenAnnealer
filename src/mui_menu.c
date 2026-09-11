/*
  This file is created to be compiled in C instead of C++ mode. 
*/
#include <stdio.h>
#include "u8g2.h"
#include "mui.h"
#include "mui_u8g2.h"
#include "app.h"
#include "pico/stdlib.h"

#include "anneal_mode.h"
#include "version.h"
#include "common.h"
#include "profile.h"
#include "servo_gate.h"
#include "induction_heater.h"


// External modules/varaibles
extern uint8_t anneal_cycle_count_digits[];
extern AppState_t exit_state;
extern servo_gate_t servo_gate;
extern eeprom_profile_data_t profile_data;


const char * get_selected_profile_name(void * data, uint16_t idx) {
    return profile_data.profiles[idx].name;
}

uint16_t get_profile_count() {
    return MAX_PROFILE_CNT;
}


uint8_t mui_hrule(mui_t *ui, uint8_t msg)
{
    u8g2_t *u8g2 = mui_get_U8g2(ui);
    switch(msg)
    {
        case MUIF_MSG_DRAW:
            u8g2_DrawHLine(u8g2, 0, mui_get_y(ui), u8g2_GetDisplayWidth(u8g2));
            break;
    }
    return 0;
}


uint8_t render_version_page(mui_t * ui, uint8_t msg) {
    switch (msg) {
        case MUIF_MSG_DRAW:
        {
            u8g2_uint_t x = mui_get_x(ui);
            u8g2_uint_t y = mui_get_y(ui);
            u8g2_t *u8g2 = mui_get_U8g2(ui);

            char buf[32];

            u8g2_SetFont(u8g2, u8g2_font_profont11_tf);

            snprintf(buf, sizeof(buf), "Ver: %s", version_string);
            u8g2_DrawStr(u8g2, x, y, buf);

            snprintf(buf, sizeof(buf), "VCS: %s", vcs_hash);
            u8g2_DrawStr(u8g2, x, y + 10, buf);

            snprintf(buf, sizeof(buf), "Build: %s", build_type);
            u8g2_DrawStr(u8g2, x, y + 20, buf);

            break;
        }
        default:
            break;
    }
    return 0;
}


uint8_t render_profile_ver_info(mui_t *ui, uint8_t msg) {
    switch (msg) {
        case MUIF_MSG_DRAW: 
        {
            u8g2_uint_t x = mui_get_x(ui);
            u8g2_uint_t y = mui_get_y(ui);
            u8g2_t *u8g2 = mui_get_U8g2(ui);

            u8g2_SetFont(u8g2, u8g2_font_profont11_tf);

            profile_t * current_profile = profile_get_selected();

            char buf[32];
            snprintf(buf, sizeof(buf), 
                     "Rev:%lx,Comp:%lx", current_profile->rev, current_profile->compatibility);

            u8g2_DrawStr(u8g2, x, y, buf);
        }
    }

    return 0;
}


uint8_t render_profile_feed_details(mui_t *ui, uint8_t msg) {
    switch(msg)
    {
        case MUIF_MSG_DRAW:
        {
            char buf[30];
            u8g2_t *u8g2 = mui_get_U8g2(ui);
            profile_t * current_profile = profile_get_selected();

            u8g2_SetFont(u8g2, u8g2_font_profont11_tf);

            memset(buf, 0x0, sizeof(buf));
            snprintf(buf, sizeof(buf), "Feed time:%lums", current_profile->feed_run_time_ms);
            u8g2_DrawStr(u8g2, 5, 25, buf);

            memset(buf, 0x0, sizeof(buf));
            snprintf(buf, sizeof(buf), "Feed speed:%0.2f", current_profile->feed_speed_rps);
            u8g2_DrawStr(u8g2, 5, 35, buf);

            memset(buf, 0x0, sizeof(buf));
            snprintf(buf, sizeof(buf), "Settle:%lums", current_profile->pre_heat_settle_ms);
            u8g2_DrawStr(u8g2, 5, 45, buf);
            break;
        }
    }
    return 0;
}


uint8_t render_anneal_mode_next_button(mui_t * ui, uint8_t msg) {
    switch (msg) {
        case MUIF_MSG_CURSOR_SELECT:
        case MUIF_MSG_VALUE_INCREMENT:
        case MUIF_MSG_VALUE_DECREMENT:
            mui_SaveForm(ui);
            ui->arg = 11;  // goto form 11 (cycle count entry)
            return mui_GotoFormAutoCursorPosition(ui, ui->arg);
        default:
            mui_u8g2_btn_goto_wm_fi(ui, msg);
            break;
    }

    return 0;
}


uint8_t render_profile_heat_details(mui_t *ui, uint8_t msg) {
    switch(msg)
    {
        case MUIF_MSG_DRAW:
        {
            char buf[30];
            u8g2_t *u8g2 = mui_get_U8g2(ui);
            profile_t * current_profile = profile_get_selected();

            u8g2_SetFont(u8g2, u8g2_font_profont11_tf);

            memset(buf, 0x0, sizeof(buf));
            snprintf(buf, sizeof(buf), "Dwell:%lums", current_profile->dwell_time_ms);
            u8g2_DrawStr(u8g2, 5, 25, buf);

            memset(buf, 0x0, sizeof(buf));
            snprintf(buf, sizeof(buf), "Post-heat:%lums", current_profile->post_heat_delay_ms);
            u8g2_DrawStr(u8g2, 5, 35, buf);

            memset(buf, 0x0, sizeof(buf));
            snprintf(buf, sizeof(buf), "Hold ratio:%0.2f", current_profile->holder_hold_ratio);
            u8g2_DrawStr(u8g2, 5, 45, buf);

            break;
        }
    }
    return 0;
}


uint8_t render_servo_gate_state_with_action(mui_t *ui, uint8_t msg) {
    uint8_t return_value = mui_u8g2_u8_radio_wm_pi(ui, msg);

    switch (msg) {
        case MUIF_MSG_CURSOR_SELECT:
        {
            uint8_t *value = (uint8_t *)muif_get_data(ui->uif);
            gate_state_t state = (gate_state_t)(*value);

            float ratio = HOLDER_RATIO_DISABLED;

            switch (state) {
                case HOLDER_DROP:
                    ratio = HOLDER_RATIO_DROP;
                    break;

                case HOLDER_HOLD:
                    ratio = HOLDER_RATIO_HOLD;
                    break;

                case HOLDER_DISABLED:
                default:
                    ratio = HOLDER_RATIO_DISABLED;
                    break;
            }

            servo_gate_set_ratio(ratio, false);
            break;
        }
    }

    return return_value;
}



// Momentary "pulse the coil" button for bench testing from the LCD, mirroring
// render_anneal_mode_next_button's idiom: delegate everything to the standard
// button handler (draw + select-driven navigation to its own arg, i.e. stay on the
// same form), just with the extra side effect on select. Bounded entirely by the
// induction heater's own max_dwell_ms safety timer, same as every other trigger path.
uint8_t render_induction_heater_pulse_button(mui_t * ui, uint8_t msg) {
    if (msg == MUIF_MSG_CURSOR_SELECT) {
        induction_heater_enable(true);
    }
    return mui_u8g2_btn_goto_wm_fi(ui, msg);
}


muif_t muif_list[] = {
        /* normal text style */
        MUIF_U8G2_FONT_STYLE(0, u8g2_font_helvR08_tr),

        /* bold text style */
        MUIF_U8G2_FONT_STYLE(1, u8g2_font_helvB08_tr),

        /* monospaced font */
        MUIF_U8G2_FONT_STYLE(2, u8g2_font_profont12_tr),

        // Large mono space font
        MUIF_U8G2_FONT_STYLE(3, u8g2_font_profont22_tf),

        MUIF_U8G2_LABEL(),                                                    /* allow MUI_LABEL command */

        // Horizontal line
        MUIF_RO("HL", mui_hrule),

        /* main menu */
        MUIF_RO("MU",mui_u8g2_goto_data),
        MUIF_BUTTON("GC", mui_u8g2_goto_form_w1_pi),

        /* Goto Form Button where the width is equal to the size of the text, spaces can be used to extend the size */
        MUIF_BUTTON("BN", mui_u8g2_btn_goto_wm_fi),

        MUIF_BUTTON("B1", render_anneal_mode_next_button),

        // Leave
        MUIF_VARIABLE("LV", &exit_state, mui_u8g2_btn_exit_wm_fi),

        // Render version
        MUIF_RO("VE", render_version_page),

        // Render servo gate state
        MUIF_VARIABLE("RB",&servo_gate.gate_state, render_servo_gate_state_with_action),

        // Induction heater bench-test pulse button
        MUIF_BUTTON("IH", render_induction_heater_pulse_button),

        // input for a number between 0 to 9 //
        MUIF_U8G2_U8_MIN_MAX("N4", &anneal_cycle_count_digits[4], 0, 9, mui_u8g2_u8_min_max_wm_mud_pi),
        MUIF_U8G2_U8_MIN_MAX("N3", &anneal_cycle_count_digits[3], 0, 9, mui_u8g2_u8_min_max_wm_mud_pi),
        MUIF_U8G2_U8_MIN_MAX("N2", &anneal_cycle_count_digits[2], 0, 9, mui_u8g2_u8_min_max_wm_mud_pi),
        MUIF_U8G2_U8_MIN_MAX("N1", &anneal_cycle_count_digits[1], 0, 9, mui_u8g2_u8_min_max_wm_mud_pi),
        MUIF_U8G2_U8_MIN_MAX("N0", &anneal_cycle_count_digits[0], 0, 9, mui_u8g2_u8_min_max_wm_mud_pi),

        MUIF_U8G2_U16_LIST("P0", (uint16_t *) &profile_data.current_profile_idx, NULL, get_selected_profile_name, get_profile_count, mui_u8g2_u16_list_parent_wm_pi),
        MUIF_U8G2_U16_LIST("P1", (uint16_t *) &profile_data.current_profile_idx, NULL, get_selected_profile_name, get_profile_count, mui_u8g2_u16_list_child_w1_pi),

        // Render profile details
        MUIF_RO("P2", render_profile_ver_info),
        MUIF_RO("P3", render_profile_feed_details),
        MUIF_RO("P4", render_profile_heat_details)
    };

const size_t muif_cnt = sizeof(muif_list) / sizeof(muif_t);

fds_t fds_data[] = {
    // Main menu
    MUI_FORM(1)
    MUI_STYLE(1)
    MUI_LABEL(5,10, "OpenAnnealer")
    MUI_XY("HL", 0,13)

    MUI_STYLE(0)
    MUI_DATA("MU", 
        MUI_10 "Start|"
        MUI_20 "Manual|"
        MUI_40 "Wireless|"
        MUI_30 "Settings"
        )
    MUI_XYA("GC", 5, 25, 0) 
    MUI_XYA("GC", 5, 37, 1) 
    MUI_XYA("GC", 5, 49, 2) 
    MUI_XYA("GC", 5, 61, 3)

    // Menu 10: Select profile
    MUI_FORM(10)
    MUI_STYLE(1)
    MUI_LABEL(5,10, "Select Profile")
    MUI_XY("HL", 0,13)

    MUI_STYLE(0)
    MUI_XYT("B1",115, 59, "Next")  // Jump to form 11 or 12
    MUI_XYAT("BN",14, 59, 1, "Back")  // Jump to form 1
    MUI_XYA("P0", 5, 25, 33)  // Jump to form 33 (profile selection)

    // Menu 11: Cycle Count
    MUI_FORM(11)
    MUI_STYLE(1)
    MUI_LABEL(5,10, "Select Cycle Count")
    MUI_XY("HL", 0,13)

    MUI_STYLE(3)
    MUI_XY("N4",20, 35)
    MUI_XY("N3",36, 35)
    MUI_XY("N2",52, 35)
    MUI_XY("N1",76, 35)
    MUI_XY("N0",92, 35)

    MUI_STYLE(0)
    MUI_XYAT("BN",115, 59, 13, "Next")
    MUI_XYAT("BN",14, 59, 10, "Back")

    // Menu 13: Warning page
    MUI_FORM(13)
    MUI_STYLE(1)
    MUI_LABEL(5, 10, "Warning")
    MUI_XY("HL", 0,13)

    MUI_STYLE(0)
    MUI_LABEL(5, 25, "Load cases and press")
    MUI_LABEL(5, 37, "Next to anneal")

    MUI_STYLE(0)
    // Put "Next" first so it is focused by default
    MUI_XYAT("LV", 115, 59, 1, "Next")  // APP_STATE_ENTER_ANNEAL_MODE
    MUI_XYAT("BN",14, 59, 10, "Back")

    // Menu 20: Manual / commissioning mode
    MUI_FORM(20)
    MUI_STYLE(1)
    MUI_LABEL(5,10, "Warning")
    MUI_XY("HL", 0,13)
    MUI_STYLE(0)
    MUI_LABEL(5, 25, "Manual jog/test mode -")
    MUI_LABEL(5, 37, "press Next to enter")
    MUI_XYAT("BN",14, 59, 1, "Back")
    MUI_XYAT("LV", 115, 59, 5, "Next")  // APP_STATE_ENTER_MANUAL_MODE

    // Menu 30: Configurations
    MUI_FORM(30)
    MUI_STYLE(1)
    MUI_LABEL(5,10, "Settings")
    MUI_XY("HL", 0,13)

    MUI_STYLE(0)
    MUI_DATA("MU",
        MUI_32 "Profile Manager|"
        MUI_42 "Induction Heater|"
        MUI_43 "Calibrate Dwell|"
        MUI_39 "Case Holder|"
        MUI_37 "EEPROM|"
        MUI_35 "Reboot|"
        MUI_36 "Version|"
        MUI_1 "<-Return"  // Back to main menu
        )
    MUI_XYA("GC", 5, 25, 0) 
    MUI_XYA("GC", 5, 37, 1) 
    MUI_XYA("GC", 5, 49, 2) 
    MUI_XYA("GC", 5, 61, 3)

    // Menu 32: Select Profile Page
    MUI_FORM(32)
    MUI_STYLE(1)
    MUI_LABEL(5,10, "Select Profile")
    MUI_XY("HL", 0,13)

    MUI_STYLE(0)
    MUI_XYA("P0", 5, 25, 33)  // Jump to form 33
    MUI_XYAT("BN",115, 59, 34, "Next")  // Jump to form 34
    MUI_XYAT("BN",14, 59, 30, "Back")  // Jump to form 30

    // Render details
    MUI_XY("P2", 5, 37)

    // Child List for profile selection
    MUI_FORM(33)
    MUI_STYLE(1)
    MUI_LABEL(5,10, "Select Profile")
    MUI_XY("HL", 0,13)

    MUI_STYLE(0)
    MUI_XYA("P1", 5, 25, 0) 
    MUI_XYA("P1", 5, 37, 1) 
    MUI_XYA("P1", 5, 49, 2) 
    MUI_XYA("P1", 5, 61, 3)

    // Menu 34: profile details (PID)
    MUI_FORM(34)
    MUI_STYLE(1)
    MUI_LABEL(5,10, "Profile Details (1/2)")
    MUI_XY("HL", 0,13)

    // Draw details
    MUI_AUX("P3")

    MUI_STYLE(0)
    MUI_XYAT("BN",115, 59, 38, "Next")  // Jump next to page 38
    MUI_XYAT("BN",14, 59, 32, "Back")  // Jump back to form 30

    // Menu 38: profile details (others)
    MUI_FORM(38)
    MUI_STYLE(1)
    MUI_LABEL(5,10, "Profile Details (2/2)")
    MUI_XY("HL", 0,13)

    // Draw details
    MUI_AUX("P4")

    MUI_STYLE(0)
    MUI_XYAT("BN",115, 59, 30, "Exit")  // Jump back to form 30
    MUI_XYAT("BN",14, 59, 34, "Back")  // Jump next to page 30


    // Menu 35: Reboot
    MUI_FORM(35)
    MUI_STYLE(1)
    MUI_LABEL(5,10, "Reboot")
    MUI_XY("HL", 0,13)
    MUI_STYLE(0)
    MUI_LABEL(5, 25, "Press Next to perform")
    MUI_LABEL(5, 37, "software reboot")
    MUI_XYAT("BN",14, 59, 30, "Back")
    MUI_XYAT("LV", 115, 59, 9, "Next")  // APP_STATE_ENTER_REBOOT

    // Menu 36 Version
    MUI_FORM(36)
    MUI_STYLE(1)
    MUI_LABEL(5,10, "Version Info")
    MUI_XY("HL", 0,13)

    MUI_XY("VE", 5, 25)  // Version text

    MUI_STYLE(0)
    MUI_XYAT("BN", 64, 59, 30, " OK ")

    // EEPROM submenu
    MUI_FORM(37)
    MUI_STYLE(1)
    MUI_LABEL(5,10, "EEPROM")
    MUI_XY("HL", 0,13)

    MUI_STYLE(0)
    MUI_DATA("MU",
        MUI_60 "Save to EEPROM|"
        MUI_61 "Erase EEPROM|"
        MUI_30 "<-Return"  // back to view 30
    )
    MUI_XYA("GC", 5, 25, 0) 
    MUI_XYA("GC", 5, 37, 1) 
    MUI_XYA("GC", 5, 49, 2) 
    MUI_XYA("GC", 5, 61, 3)

    // Case Holder submenu
    MUI_FORM(39)
    MUI_STYLE(1)
    MUI_LABEL(5,10, "Case Holder Control")
    MUI_XY("HL", 0,13)

    MUI_XYAT("RB", 5, 25, 0, "Disable")
    MUI_XYAT("RB", 5, 37, 1, "Hold")
    MUI_XYAT("RB", 5, 49, 2, "Drop")
    MUI_XYAT("BN", 64, 59, 30, " OK ")  // Jump to form 30

    // Wirelss submenu
    MUI_FORM(40)
    MUI_STYLE(1)
    MUI_LABEL(5,10, "Wireless")
    MUI_XY("HL", 0,13)

    MUI_STYLE(0)
    MUI_DATA("MU",
        MUI_41 "Wifi Info|"
        MUI_1 "<-Return"  // back to view 1
    )
    MUI_XYA("GC", 5, 25, 0) 
    MUI_XYA("GC", 5, 37, 1) 
    MUI_XYA("GC", 5, 49, 2) 
    MUI_XYA("GC", 5, 61, 3)

    // Wifi info
    MUI_FORM(41)
    MUI_LABEL(5,10, "Wifi Info")
    MUI_XY("HL", 0,13)

    MUI_STYLE(0)
    MUI_LABEL(3,27, "Press OK to view Wifi")
    MUI_LABEL(3,37, "information")

    MUI_STYLE(0)
    MUI_XYAT("LV",64, 59, 10, " OK ")  // APP_STATE_ENTER_WIFI_INFO

    // Induction heater bench-test
    MUI_FORM(42)
    MUI_STYLE(1)
    MUI_LABEL(5,10, "Induction Heater")
    MUI_XY("HL", 0,13)

    MUI_STYLE(0)
    MUI_LABEL(5, 25, "For bench testing only.")
    MUI_LABEL(5, 37, "Cutoff is automatic.")
    MUI_XYAT("IH", 5, 49, 42, "Pulse Coil")
    MUI_XYAT("BN", 64, 59, 30, " OK ")  // Jump to form 30

    // Calibrate dwell time
    MUI_FORM(43)
    MUI_STYLE(1)
    MUI_LABEL(5, 10, "Warning")
    MUI_XY("HL", 0,13)

    MUI_STYLE(0)
    MUI_LABEL(5, 25, "Place painted case,")
    MUI_LABEL(5, 37, "press Next to start")

    MUI_STYLE(0)
    MUI_XYAT("BN",14, 59, 30, "Back")
    MUI_XYAT("LV", 115, 59, 11, "Next")  // APP_STATE_ENTER_CASE_CALIBRATION_MODE


    // Save to EEPROM
    MUI_FORM(60)
    MUI_STYLE(1)
    MUI_LABEL(5,10, "Save to EEPROM")
    MUI_XY("HL", 0,13)
    MUI_STYLE(0)
    MUI_LABEL(5, 25, "Press Next to save")
    MUI_LABEL(5, 37, "changes to EEPROM")
    MUI_XYAT("BN",14, 59, 37, "Back")
    MUI_XYAT("LV", 115, 59, 7, "Next")  // APP_STATE_ENTER_EEPROM_SAVE

    // Erase entire EEPROM
    MUI_FORM(61)
    MUI_STYLE(1)
    MUI_LABEL(5,10, "Erase EEPROM")
    MUI_XY("HL", 0,13)
    MUI_STYLE(0)
    MUI_LABEL(5, 25, "Press Next to erase")
    MUI_LABEL(5, 37, "the EEPROM")
    MUI_XYAT("BN",14, 59, 37, "Back")
    MUI_XYAT("LV", 115, 59, 8, "Next")  // APP_STATE_ENTER_EEPROM_ERASE
};
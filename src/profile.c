#include <string.h>

#include "profile.h"
#include "eeprom.h"
#include "common.h"


eeprom_profile_data_t profile_data;

// Placeholder recipes - actual dwell_time_ms per case type is meant to be discovered
// via Milestone 6's calibration mode and saved back here, not guessed up front.
// feed_run_time_ms/feed_speed_rps/pre_heat_settle_ms/post_heat_delay_ms carry over the
// same starting values anneal_mode used as its old global defaults, since those aren't
// case-length dependent. Holder position isn't part of the recipe at all - see
// profile.h's comment on profile_t.
#define DEFAULT_FEED_RUN_TIME_MS   1000
#define DEFAULT_FEED_SPEED_RPS     1.0f
#define DEFAULT_PRE_HEAT_SETTLE_MS 300
// 1s: deliberately low/safe rather than a guessed "real" dwell time - actual dwell
// time is case/alloy/coil dependent and must come from Milestone 6's calibration
// mode, not this default.
#define DEFAULT_DWELL_TIME_MS      1000
#define DEFAULT_POST_HEAT_DELAY_MS 500

const eeprom_profile_data_t default_profile_data = {
    .profile_data_rev = 0,
    .profiles[0] = {
        .name = "LC 5.56",
        .feed_run_time_ms = DEFAULT_FEED_RUN_TIME_MS,
        .feed_speed_rps = DEFAULT_FEED_SPEED_RPS,
        .pre_heat_settle_ms = DEFAULT_PRE_HEAT_SETTLE_MS,
        .dwell_time_ms = DEFAULT_DWELL_TIME_MS,
        .post_heat_delay_ms = DEFAULT_POST_HEAT_DELAY_MS,
    },
    .profiles[1] = {
        .name = "Profile1",
    },
    .profiles[2] = {
        .name = "Profile2",
    },
    .profiles[3] = {
        .name = "Profile3",
    },
    .profiles[4] = {
        .name = "Profile4",
    },
    .profiles[5] = {
        .name = "Profile5",
    },
    .profiles[6] = {
        .name = "Profile6",
    },
    .profiles[7] = {
        .name = "Profile7",
    },
};


bool profile_data_save() {
    bool is_ok = save_config(EEPROM_PROFILE_DATA_BASE_ADDR, &profile_data, sizeof(profile_data));
    return is_ok;
}


bool profile_data_init() {
    bool is_ok = true;

    // Read profile index table
    memset(&profile_data, 0x0, sizeof(eeprom_profile_data_t));
    is_ok = load_config(EEPROM_PROFILE_DATA_BASE_ADDR, &profile_data, &default_profile_data, sizeof(profile_data), EEPROM_PROFILE_DATA_REV);

    if (!is_ok) {
        printf("Unable to read profile data\n");
        return false;
    }

    // Register to eeprom save all
    eeprom_register_handler(profile_data_save);

    return true;
}


uint16_t profile_get_selected_idx() {
    return profile_data.current_profile_idx;
}


profile_t * profile_get_selected() {
    return &profile_data.profiles[profile_get_selected_idx()];
}


profile_t * profile_select(uint8_t idx) {
    profile_data.current_profile_idx = idx;

    return profile_get_selected(idx);
}


bool http_rest_profile_config(struct fs_file *file, int num_params, char *params[], char *values[]) {
    // Mappings:
    // pf (int): profile index
    // p2 (str): name
    // p3 (int): feed_run_time_ms
    // p4 (float): feed_speed_rps
    // p5 (int): pre_heat_settle_ms
    // p6 (int): dwell_time_ms
    // p7 (int): post_heat_delay_ms
    // p9 (float): target_temp_c (Milestone 11; p8 was holder_hold_ratio, retired)
    // ee (bool): save to eeprom
    static char buf[256];

    // Read the current loaded profile index
    uint8_t profile_idx = profile_get_selected_idx();

    // Overwrite the profile index (if applicable)
    for (int idx = 0; idx < num_params; idx += 1) {
        if (strcmp(params[idx], "pf") == 0) {
            profile_idx = (uint16_t) atoi(values[0]);
        }
    }

    if (profile_idx >= MAX_PROFILE_CNT) {
        strcpy(buf, "{\"error\":\"InvalidProfileIndex\"}");
    }

    else {
        profile_t * current_profile = profile_select(profile_idx);
        bool save_to_eeprom = false;

        // Control
        for (int idx = 0; idx < num_params; idx += 1) {
            if (strcmp(params[idx], "p2") == 0) {
                strncpy(current_profile->name, values[idx], sizeof(current_profile->name));
            }
            else if (strcmp(params[idx], "p3") == 0) {
                current_profile->feed_run_time_ms = strtoul(values[idx], NULL, 10);
            }
            else if (strcmp(params[idx], "p4") == 0) {
                current_profile->feed_speed_rps = strtof(values[idx], NULL);
            }
            else if (strcmp(params[idx], "p5") == 0) {
                current_profile->pre_heat_settle_ms = strtoul(values[idx], NULL, 10);
            }
            else if (strcmp(params[idx], "p6") == 0) {
                current_profile->dwell_time_ms = strtoul(values[idx], NULL, 10);
            }
            else if (strcmp(params[idx], "p7") == 0) {
                current_profile->post_heat_delay_ms = strtoul(values[idx], NULL, 10);
            }
            else if (strcmp(params[idx], "p9") == 0) {
                current_profile->target_temp_c = strtof(values[idx], NULL);
            }
            else if (strcmp(params[idx], "ee") == 0) {
                save_to_eeprom = string_to_boolean(values[idx]);
            }
        }

        // Perform action
        if (save_to_eeprom) {
            profile_data_save();
        }

        // Response
        snprintf(buf, sizeof(buf),
                 "%s"
                 "{\"pf\":%d,\"p2\":\"%s\",\"p3\":%lu,\"p4\":%0.3f,\"p5\":%lu,\"p6\":%lu,\"p7\":%lu,\"p9\":%0.2f}",
                 http_json_header,
                 profile_idx,
                 current_profile->name,
                 current_profile->feed_run_time_ms,
                 current_profile->feed_speed_rps,
                 current_profile->pre_heat_settle_ms,
                 current_profile->dwell_time_ms,
                 current_profile->post_heat_delay_ms,
                 current_profile->target_temp_c);
    }

    size_t response_len = strlen(buf);
    file->data = buf;
    file->len = response_len;
    file->index = response_len;
    file->flags = FS_FILE_FLAGS_HEADER_INCLUDED;

    return true;
}


bool http_rest_profile_summary(struct fs_file *file, int num_params, char *params[], char *values[])
{
    // It does not take argument
    assert(MAX_PROFILE_CNT <= 8);  // Ensures 256 byte buffer us sufficient
    static char buf[256];

    // Response
    // s0 (dict): A dictionary of all profiles in {idx: name} format. 
    // s1 (int): The current loaded profile index
    memset(buf, 0x0, sizeof(buf));
    const char * item_template = "\"%d\":\"%s\",";

    // Create header
    snprintf(buf, sizeof(buf), 
             "%s{\"s0\":{",
             http_json_header);

    size_t char_idx = strlen(buf);

    // Write profile information
    for (uint8_t p_idx=0; p_idx < MAX_PROFILE_CNT; p_idx+=1) {
        snprintf(&buf[char_idx], sizeof(buf) - char_idx, 
                 item_template,
                 p_idx, &profile_data.profiles[p_idx].name);
        char_idx += strnlen((const char *) &buf[char_idx], sizeof(buf));
    }

    // Append close bracket (replace the last comma)
    buf[char_idx - 1] = '}';

    // Append s1
    snprintf(&buf[char_idx], sizeof(buf) - char_idx,
             ",\"s1\":%d}", 
             profile_data.current_profile_idx);

    size_t response_len = strlen(buf);
    file->data = buf;
    file->len = response_len;
    file->index = response_len;
    file->flags = FS_FILE_FLAGS_HEADER_INCLUDED;

    return true;
}
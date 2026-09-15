#ifndef PROFILE_H_
#define PROFILE_H_

#include <stdint.h>
#include <stdbool.h>
#include "http_rest.h"


#define PROFILE_NAME_MAX_LEN    16
#define MAX_PROFILE_CNT         8

#define EEPROM_PROFILE_DATA_REV             4           // 16 bit - bumped: added target_temp_c (Milestone 11)

typedef struct
{
    uint32_t rev;
    uint32_t compatibility;

    char name[PROFILE_NAME_MAX_LEN];

    // Anneal recipe: how this case type should be fed/held/heated. dwell_time_ms is
    // the one value Milestone 6's calibration mode is meant to help discover and save
    // per profile. Holder position is NOT part of the recipe: the physical holder is a
    // swing arm with a manual adjustment nut for case length, so the servo always
    // swings to the same two fixed endpoints (HOLDER_RATIO_HOLD/HOLDER_RATIO_DROP in
    // servo_gate.h) regardless of which profile is selected.
    uint32_t feed_run_time_ms;   // How long to run the feeder at feed_speed_rps to advance one case
    float feed_speed_rps;
    uint32_t pre_heat_settle_ms; // Let the holder finish moving before enabling the coil
    uint32_t dwell_time_ms;      // Heat time; must be <= the induction heater's max_dwell_ms.
                                 // This is the operative heat duration in pure time-based
                                 // mode. When target_temp_c (below) is in effect instead, this
                                 // is NOT reused as a cap - it's a time-mode-calibrated value
                                 // with no bearing on how long reaching a temperature target
                                 // should take. The real ceiling in temperature mode is the
                                 // induction heater's own hardware max_dwell_ms safety timer.
    uint32_t post_heat_delay_ms; // Let the coil fully de-energize before dropping
    float target_temp_c;        // Optional target object temperature (Milestone 11). Only
                                 // consulted when the IR temp sensor's global toggle is on;
                                 // 0 or the sensor being disabled/unhealthy means pure
                                 // time-based dwell for this profile.
} profile_t;


typedef struct {
    uint16_t profile_data_rev;
    uint16_t current_profile_idx;

    profile_t profiles[MAX_PROFILE_CNT];
} eeprom_profile_data_t;


#ifdef __cplusplus
extern "C" {
#endif



// Interface
bool profile_data_init(void);
bool profile_data_save();

profile_t * profile_select(uint8_t idx);
profile_t * profile_get_selected();

// REST interface
bool http_rest_profile_config(struct fs_file *file, int num_params, char *params[], char *values[]);
bool http_rest_profile_summary(struct fs_file *file, int num_params, char *params[], char *values[]);


#ifdef __cplusplus
}
#endif

#endif  // PROFILE_H_
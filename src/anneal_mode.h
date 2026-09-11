#ifndef ANNEAL_MODE_H_
#define ANNEAL_MODE_H_

#include <stdint.h>
#include "http_rest.h"
#include "common.h"
#include "neopixel_led.h"


#define EEPROM_ANNEAL_MODE_DATA_REV                     1              // 16 byte

typedef enum {
    ANNEAL_MODE_EXIT = 0,
    ANNEAL_MODE_FEED = 1,
    ANNEAL_MODE_HOLD = 2,
    ANNEAL_MODE_HEAT = 3,
    ANNEAL_MODE_DROP = 4,
    ANNEAL_MODE_COOLDOWN = 5,
} anneal_mode_state_t;

typedef struct {
    uint16_t anneal_mode_data_rev;

    // Feed: how long to run the feeder motor at feed_speed_rps to advance one case
    uint32_t feed_run_time_ms;
    float feed_speed_rps;

    // Holder / heat timing.
    // holder_hold_ratio and dwell_time_ms are global for now; a future pass may move
    // them to per-profile fields once case-length-specific holder positions and
    // per-recipe dwell times (from the calibration mode) are needed.
    uint32_t pre_heat_settle_ms;    // Let the holder finish moving before enabling the coil
    uint32_t dwell_time_ms;         // Heat time; must be <= the induction heater's max_dwell_ms
    uint32_t post_heat_delay_ms;    // Let the coil fully de-energize before dropping
    float holder_hold_ratio;        // Servo ratio while holding the case in the coil

    // Cycle
    uint32_t inter_cycle_delay_ms;  // Pause between cases
    uint32_t cycle_count;           // 0 = run until stopped

    // LED related settings
    rgbw_u32_t neopixel_ready_colour;
    rgbw_u32_t neopixel_heating_colour;
    rgbw_u32_t neopixel_fault_colour;
} eeprom_anneal_mode_data_t;

typedef struct {
    eeprom_anneal_mode_data_t eeprom_anneal_mode_data;
    anneal_mode_state_t anneal_mode_state;
    uint32_t anneal_mode_event;
    uint32_t cases_completed;
} anneal_mode_config_t;


// C Functions
#ifdef __cplusplus
extern "C" {
#endif


bool anneal_mode_config_init(void);
uint8_t anneal_mode_menu(bool anneal_mode_skip_user_input);
bool anneal_mode_config_save(void);

// REST interface
bool http_rest_anneal_mode_config(struct fs_file *file, int num_params, char *params[], char *values[]);
bool http_rest_anneal_mode_state(struct fs_file *file, int num_params, char *params[], char *values[]);


#ifdef __cplusplus
}  // __cplusplus
#endif


#endif  // ANNEAL_MODE_H_

#ifndef ANNEAL_MODE_H_
#define ANNEAL_MODE_H_

#include <stdint.h>
#include "http_rest.h"
#include "common.h"
#include "neopixel_led.h"


#define EEPROM_ANNEAL_MODE_DATA_REV                     3              // 16 byte - bumped: added use_temperature_mode (Milestone 11)

// Values match chronological execution order (HOLD first: the holder must be in
// position before a case is fed, not after - see anneal_mode_hold() for why). The
// web UI's step-progress widget relies on this ordering to highlight steps
// correctly; keep them in sync if this sequence ever changes again.
typedef enum {
    ANNEAL_MODE_EXIT = 0,
    ANNEAL_MODE_HOLD = 1,
    ANNEAL_MODE_FEED = 2,
    ANNEAL_MODE_HEAT = 3,
    ANNEAL_MODE_DROP = 4,
    ANNEAL_MODE_COOLDOWN = 5,
} anneal_mode_state_t;

typedef struct {
    uint16_t anneal_mode_data_rev;

    // Feed/holder/heat timing is per-profile now (see profile.h) - a case's feed
    // time, dwell time, and holder hold ratio all depend on its length/alloy, which
    // is exactly what a profile represents. Only settings that apply regardless of
    // which case type is loaded stay here.

    // Cycle
    uint32_t inter_cycle_delay_ms;  // Pause between cases
    uint32_t cycle_count;           // 0 = run until stopped

    // Milestone 11: global time-vs-temperature mode switch for the anneal cycle (front
    // page). Deliberately NOT a property of the IR temp sensor module itself - the
    // sensor always reads/reports its own real health regardless of this switch, so a
    // "not currently in temperature mode" state never looks like a sensor fault. Only
    // changeable while no cycle is running - see http_rest_anneal_mode_config().
    bool use_temperature_mode;

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

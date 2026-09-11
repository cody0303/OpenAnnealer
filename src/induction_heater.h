#ifndef INDUCTION_HEATER_H_
#define INDUCTION_HEATER_H_

#include <stdint.h>
#include <stdbool.h>
#include <FreeRTOS.h>
#include <timers.h>
#include "http_rest.h"

#define EEPROM_INDUCTION_HEATER_CONFIG_REV     2              // 16 byte

typedef struct {
    uint16_t induction_heater_config_rev;
    bool active_high;          // GPIO level that energises the coil
    uint32_t max_dwell_ms;     // Hard safety cap: coil is force-disabled after this long regardless of caller
    uint8_t gpio_pin;          // Runtime-configurable trigger pin; wiring varies per build
} eeprom_induction_heater_data_t;


typedef struct {
    eeprom_induction_heater_data_t eeprom_induction_heater_data;
    bool is_active;
    bool safety_cutoff_fault;      // Set when the safety timer had to force-disable the coil
    bool pin_claimed;              // True once a GPIO has actually been init'd as the trigger output
    TimerHandle_t safety_timer;
} induction_heater_t;


#ifdef __cplusplus
extern "C" {
#endif

bool induction_heater_init(void);
bool induction_heater_config_save(void);

/**
 * @brief Enable/disable the induction coil trigger.
 * Enabling (re)starts a hard safety timer (max_dwell_ms) that force-disables the
 * coil no matter what the caller does afterwards. Disabling cancels that timer.
 */
void induction_heater_enable(bool enable);
bool induction_heater_is_active(void);

bool http_rest_induction_heater_config(struct fs_file *file, int num_params, char *params[], char *values[]);
bool http_rest_induction_heater_state(struct fs_file *file, int num_params, char *params[], char *values[]);

#ifdef __cplusplus
}
#endif

#endif  // INDUCTION_HEATER_H_

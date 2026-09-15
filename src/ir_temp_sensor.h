#ifndef IR_TEMP_SENSOR_H_
#define IR_TEMP_SENSOR_H_

#include <stdint.h>
#include <stdbool.h>
#include "http_rest.h"

#define EEPROM_IR_TEMP_SENSOR_DATA_REV      1              // 16 byte - bumped: removed `enabled` (the
                                                            // time-vs-temperature mode switch now lives
                                                            // in anneal_mode's own config instead - see
                                                            // eeprom_anneal_mode_data_t.use_temperature_mode)

// Non-contact IR thermometer (MLX90614) support for Milestone 11's optional
// target-temperature annealing. Fully optional hardware: if the sensor doesn't ACK at
// init, the feature disables itself in software (same degrade-gracefully pattern as
// motors_init()) - nothing else in the firmware requires this to be populated.
//
// Deliberately has no "enabled" concept of its own: this module always probes, reads,
// and reports its own real health whenever hardware is present, regardless of whether
// the anneal cycle is currently in time-based or temperature-based mode. That mode
// switch lives in anneal_mode's config instead - keeping it out of here means a
// "not currently using temperature mode" state never gets mistaken for a sensor fault.
typedef struct {
    uint16_t ir_temp_sensor_data_rev;
    uint8_t sda_pin;
    uint8_t scl_pin;
    float emissivity;       // 0.0-1.0; written to the sensor's own onboard EEPROM only
                             // when it differs from this value (see ir_temp_sensor.cpp -
                             // that EEPROM has a limited write-cycle life)
    float offset_c;         // Linear correction added to the raw object-temperature reading
    uint8_t filter_window;  // Rolling-average sample count. Deliberately small by default -
                             // anneal dwells can be a couple of seconds, so a long filter
                             // would lag well behind the real temperature ramp.
} eeprom_ir_temp_sensor_data_t;


#ifdef __cplusplus
extern "C" {
#endif

bool ir_temp_sensor_init(void);
bool ir_temp_sensor_config_save(void);

// True once the sensor has ACK'd at init - the only thing this module tracks about
// its own applicability. Callers that care about time-vs-temperature mode (anneal_mode)
// combine this with their own mode switch instead of this module deciding it for them.
bool ir_temp_sensor_is_present(void);

// True when present and recent reads have been succeeding, AND the rolling filter has
// collected at least one full window of samples. Always reflects real hardware health,
// independent of whether anneal_mode is currently using temperature mode. False here
// means "don't trust the current reading" - see anneal_mode.cpp for how it's used.
// This is also false during the brief (filter_window * poll interval, well under a
// second by default) startup window right after the sensor comes up - see
// ir_temp_sensor_is_initializing() to tell that apart from a real fault.
bool ir_temp_sensor_is_healthy(void);

// True only during the brief window after the sensor is first detected, before the
// rolling filter has collected its first full set of samples - distinct from a real
// fault so the UI can show "Initializing..." rather than "Sensor Fault".
bool ir_temp_sensor_is_initializing(void);

float ir_temp_sensor_get_object_temp_c(void);
float ir_temp_sensor_get_ambient_temp_c(void);

bool http_rest_ir_temp_sensor_config(struct fs_file *file, int num_params, char *params[], char *values[]);
bool http_rest_ir_temp_sensor_state(struct fs_file *file, int num_params, char *params[], char *values[]);

#ifdef __cplusplus
}
#endif

#endif  // IR_TEMP_SENSOR_H_

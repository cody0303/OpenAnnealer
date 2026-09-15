#ifndef IR_TEMP_SENSOR_H_
#define IR_TEMP_SENSOR_H_

#include <stdint.h>
#include <stdbool.h>
#include "http_rest.h"

#define EEPROM_IR_TEMP_SENSOR_DATA_REV      0              // 16 byte

// Non-contact IR thermometer (MLX90614) support for Milestone 11's optional
// target-temperature annealing. Fully optional hardware: if the sensor doesn't ACK at
// init, the feature disables itself in software (same degrade-gracefully pattern as
// motors_init()) - nothing else in the firmware requires this to be populated.
typedef struct {
    uint16_t ir_temp_sensor_data_rev;
    bool enabled;           // Global "use temperature" toggle (front page); per-profile
                             // target_temp_c in profile.h is only consulted when this is on
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

// True only when the global toggle is on AND the sensor actually probed present at
// init - callers (anneal_mode's heat state) should treat this as "use temperature
// mode for this cycle", falling back to pure time-based dwell otherwise.
bool ir_temp_sensor_is_enabled(void);

// True when enabled and recent reads have been succeeding, AND the rolling filter has
// collected at least one full window of samples. False here (while enabled) means
// "trust the timer, not the temperature" for this cycle - see anneal_mode.cpp. This is
// also false during the brief (filter_window * poll interval, well under a second by
// default) startup window right after the sensor comes up - see
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

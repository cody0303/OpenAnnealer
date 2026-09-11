#ifndef ANNEAL_CALIBRATION_MODE_H_
#define ANNEAL_CALIBRATION_MODE_H_

#include <stdint.h>
#include <stdbool.h>
#include "http_rest.h"

typedef enum {
    ANNEAL_CALIBRATION_EXIT = 0,
    ANNEAL_CALIBRATION_ENTER = 1,
} anneal_calibration_state_t;


// C Functions
#ifdef __cplusplus
extern "C" {
#endif

// Guided dwell-time calibration: feed a case marked with temperature-indicating
// paint, run the coil continuously, and let the operator press the knob the instant
// it changes colour - capturing exactly how long the coil ran so it can be saved as
// the selected profile's dwell_time_ms. Correct dwell time depends on the coil, case
// alloy/thickness, and induction heater power, so it can't be computed up front.
uint8_t anneal_calibration_mode_menu();

bool http_rest_anneal_calibration_state(struct fs_file *file, int num_params, char *params[], char *values[]);

#ifdef __cplusplus
}  // __cplusplus
#endif

#endif  // ANNEAL_CALIBRATION_MODE_H_

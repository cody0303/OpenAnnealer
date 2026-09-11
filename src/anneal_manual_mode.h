#ifndef ANNEAL_MANUAL_MODE_H_
#define ANNEAL_MANUAL_MODE_H_

#include <stdint.h>
#include "http_rest.h"
#include "motors.h"

typedef enum {
    ANNEAL_MANUAL_MODE_EXIT = 0,
    ANNEAL_MANUAL_MODE_ENTER = 1,
} anneal_manual_mode_state_t;


typedef struct {
    float feed_speed_rps;
    bool holder_held;
    anneal_manual_mode_state_t manual_mode_state;
} anneal_manual_mode_config_t;


// C Functions
#ifdef __cplusplus
extern "C" {
#endif

// Manual/commissioning mode: jog the feeder motor and toggle the case holder servo
// with the encoder, useful for bench-testing the mechanism before running full
// anneal cycles or calibrating dwell time. Replaces the old scale-oriented
// cleanup_mode.
uint8_t anneal_manual_mode_menu();

bool http_rest_manual_mode_state(struct fs_file *file, int num_params, char *params[], char *values[]);

#ifdef __cplusplus
}  // __cplusplus
#endif

#endif  // ANNEAL_MANUAL_MODE_H_

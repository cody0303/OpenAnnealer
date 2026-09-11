#include "scale.h"
#include "FreeRTOS.h"
#include "task.h"
#include <math.h>
#include <stdlib.h>

extern scale_config_t scale_config;

static float simulated_scale_weight = 0.0f;

void simulated_scale_force_zero(void) {
    simulated_scale_weight = 0.0f;
}

// The original powder-trickler charge-curve simulation was tied to charge_mode's
// target weight and the coarse/fine motor speeds, neither of which exist in this
// application. The scale subsystem isn't used by the annealer at all and is slated
// for full removal; this stub only exists so scale.c keeps linking until then.
void simulated_scale_read_loop_task(void *p) {
    const TickType_t interval = pdMS_TO_TICKS(100);

    while (true) {
        if (scale_config.persistent_config.scale_driver == SCALE_DRIVER_SIMULATED) {
            float noise = ((float)(rand() % 101) - 50.0f) / 1000.0f;
            simulated_scale_weight += noise;

            scale_config.current_scale_measurement = simulated_scale_weight;
            if (scale_config.scale_measurement_ready) {
                xSemaphoreGive(scale_config.scale_measurement_ready);
            }
        }

        vTaskDelay(interval);
    }
}

scale_handle_t simulated_scale_handle = {
    .read_loop_task = simulated_scale_read_loop_task,
    .force_zero = simulated_scale_force_zero,
};

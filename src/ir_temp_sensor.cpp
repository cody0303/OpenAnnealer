#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include <FreeRTOS.h>
#include <task.h>

#include "hardware/i2c.h"
#include "configuration.h"

#include "ir_temp_sensor.h"
#include "eeprom.h"
#include "common.h"
#include "FloatRingBuffer.h"

#define MLX90614_I2C_ADDR           0x5A
#define MLX90614_RAM_TA             0x06    // Ambient temperature
#define MLX90614_RAM_TOBJ1          0x07    // Object 1 temperature
#define MLX90614_EEPROM_EMISSIVITY  0x24    // 0x20 | 0x04 - EEPROM read/write command

// Poll close to the sensor's own native measurement rate - deliberately NOT a long,
// slow interval, since anneal dwells can be as short as a couple of seconds and a
// sluggish poll rate would make target-temperature detection lag reality.
#define IR_TEMP_SENSOR_POLL_INTERVAL_MS    150

// Bounds on the configurable rolling-average window. Kept small on purpose - a long
// filter would smooth right through a fast heat ramp. See eeprom_ir_temp_sensor_data_t's
// filter_window comment.
#define IR_TEMP_SENSOR_FILTER_MIN_WINDOW   1
#define IR_TEMP_SENSOR_FILTER_MAX_WINDOW   10

// Sensor is considered unhealthy (fall back to time-only dwell) after this many
// consecutive read failures - the induction coil's switching field is close enough to
// plausibly corrupt an occasional I2C transaction.
#define IR_TEMP_SENSOR_MAX_CONSECUTIVE_FAILURES    3

typedef struct {
    eeprom_ir_temp_sensor_data_t eeprom_ir_temp_sensor_data;

    bool sensor_present;               // Probed successfully at init
    uint32_t consecutive_read_failures;

    float ambient_temp_c;              // Raw - ambient changes slowly, no filtering needed
    float object_temp_c;               // Filtered (rolling average) + offset applied

    FloatRingBuffer * object_temp_filter;
    TaskHandle_t poll_task_handler;
} ir_temp_sensor_t;

static ir_temp_sensor_t ir_temp_sensor;

const eeprom_ir_temp_sensor_data_t default_ir_temp_sensor_data = {
    .ir_temp_sensor_data_rev = 0,
    .enabled = false,
    .sda_pin = IR_TEMP_SENSOR_SDA_PIN_DEFAULT,
    .scl_pin = IR_TEMP_SENSOR_SCL_PIN_DEFAULT,
    .emissivity = 1.0f,     // Matches the MLX90614's own factory-default emissivity, so
                             // a brand new sensor needs zero EEPROM writes out of the box
    .offset_c = 0.0f,
    .filter_window = 3,
};


static uint8_t _clamp_filter_window(uint8_t window) {
    if (window < IR_TEMP_SENSOR_FILTER_MIN_WINDOW) return IR_TEMP_SENSOR_FILTER_MIN_WINDOW;
    if (window > IR_TEMP_SENSOR_FILTER_MAX_WINDOW) return IR_TEMP_SENSOR_FILTER_MAX_WINDOW;
    return window;
}


static void _recreate_temp_filter(uint8_t window) {
    if (ir_temp_sensor.object_temp_filter != NULL) {
        delete ir_temp_sensor.object_temp_filter;
    }
    ir_temp_sensor.object_temp_filter = new FloatRingBuffer(_clamp_filter_window(window));
}


// Standard SMBus Packet Error Code (CRC-8, polynomial x^8+x^2+x+1, no reflection) -
// the MLX90614 requires a valid PEC on every EEPROM write or it silently ignores it.
static uint8_t _mlx90614_pec(const uint8_t *data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i += 1) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit += 1) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}


static bool _mlx90614_read_word(uint8_t command, uint16_t *out_raw) {
    uint8_t rx[3];  // data low, data high, PEC (PEC not checked on read - see header)

    int rc = i2c_write_blocking(IR_TEMP_SENSOR_I2C, MLX90614_I2C_ADDR, &command, 1, true);
    if (rc != 1) {
        return false;
    }

    rc = i2c_read_blocking(IR_TEMP_SENSOR_I2C, MLX90614_I2C_ADDR, rx, sizeof(rx), false);
    if (rc != (int) sizeof(rx)) {
        return false;
    }

    *out_raw = (uint16_t) rx[0] | ((uint16_t) rx[1] << 8);
    return true;
}


static bool _mlx90614_write_word(uint8_t command, uint16_t data) {
    uint8_t addr_write = (uint8_t)(MLX90614_I2C_ADDR << 1);
    uint8_t pec_input[4] = { addr_write, command, (uint8_t)(data & 0xFF), (uint8_t)(data >> 8) };
    uint8_t tx[4] = { command, pec_input[2], pec_input[3], _mlx90614_pec(pec_input, sizeof(pec_input)) };

    int rc = i2c_write_blocking(IR_TEMP_SENSOR_I2C, MLX90614_I2C_ADDR, tx, sizeof(tx), false);
    return rc == (int) sizeof(tx);
}


// The MLX90614's EEPROM cells must be erased (written to 0x0000) before a new value
// can be written - writing directly over a non-zero cell is not reliable per the
// datasheet. Both steps need the ~5ms EEPROM write time to complete before the next
// I2C transaction.
static bool _mlx90614_write_eeprom_word(uint8_t command, uint16_t data) {
    if (!_mlx90614_write_word(command, 0x0000)) {
        return false;
    }
    sleep_ms(10);

    if (!_mlx90614_write_word(command, data)) {
        return false;
    }
    sleep_ms(10);

    return true;
}


static float _raw_to_celsius(uint16_t raw) {
    // Datasheet: 0.02 K per LSB
    return ((float) raw * 0.02f) - 273.15f;
}


// Reads the sensor's currently-stored emissivity and writes the configured value only
// if it actually differs - the sensor's EEPROM has a limited write-cycle life, and the
// stored value is itself the source of truth for "has this been set", so there's no
// need to track that separately. A brand new sensor reads back 0xFFFF (1.0) here, which
// matches this project's own default, so a fresh sensor needs no write at all.
static void _sync_emissivity_if_needed(void) {
    uint16_t current_raw;
    if (!_mlx90614_read_word(MLX90614_EEPROM_EMISSIVITY, &current_raw)) {
        printf("IR temp sensor: unable to read current emissivity, leaving it alone\n");
        return;
    }

    float current_emissivity = (float) current_raw / 65535.0f;
    float desired_emissivity = ir_temp_sensor.eeprom_ir_temp_sensor_data.emissivity;
    uint16_t desired_raw = (uint16_t) (desired_emissivity * 65535.0f + 0.5f);

    // Compare on the raw 16-bit value actually stored (rather than the float
    // conversion) so a value already matching doesn't trigger a needless write due to
    // float rounding.
    if (current_raw == desired_raw) {
        return;
    }

    printf("IR temp sensor: emissivity mismatch (sensor=%.4f, configured=%.4f) - writing sensor EEPROM\n",
           current_emissivity, desired_emissivity);

    if (!_mlx90614_write_eeprom_word(MLX90614_EEPROM_EMISSIVITY, desired_raw)) {
        printf("IR temp sensor: emissivity write failed\n");
        return;
    }

    uint16_t verify_raw;
    if (_mlx90614_read_word(MLX90614_EEPROM_EMISSIVITY, &verify_raw) && verify_raw == desired_raw) {
        printf("IR temp sensor: emissivity updated and verified\n");
    } else {
        printf("IR temp sensor: emissivity write could not be verified\n");
    }
}


static void ir_temp_sensor_poll_task(void *p) {
    while (true) {
        TickType_t last_poll_tick = xTaskGetTickCount();

        uint16_t ta_raw, tobj_raw;
        bool ta_ok = _mlx90614_read_word(MLX90614_RAM_TA, &ta_raw);
        bool tobj_ok = _mlx90614_read_word(MLX90614_RAM_TOBJ1, &tobj_raw);

        if (ta_ok && tobj_ok) {
            ir_temp_sensor.consecutive_read_failures = 0;
            ir_temp_sensor.ambient_temp_c = _raw_to_celsius(ta_raw);

            float object_reading = _raw_to_celsius(tobj_raw) + ir_temp_sensor.eeprom_ir_temp_sensor_data.offset_c;
            ir_temp_sensor.object_temp_filter->enqueue(object_reading);
            ir_temp_sensor.object_temp_c = ir_temp_sensor.object_temp_filter->getMean();
        }
        else {
            ir_temp_sensor.consecutive_read_failures += 1;
        }

        vTaskDelayUntil(&last_poll_tick, pdMS_TO_TICKS(IR_TEMP_SENSOR_POLL_INTERVAL_MS));
    }
}


bool ir_temp_sensor_init(void) {
    memset(&ir_temp_sensor, 0x0, sizeof(ir_temp_sensor));

    bool is_ok = load_config(EEPROM_IR_TEMP_SENSOR_BASE_ADDR,
                              &ir_temp_sensor.eeprom_ir_temp_sensor_data,
                              &default_ir_temp_sensor_data,
                              sizeof(ir_temp_sensor.eeprom_ir_temp_sensor_data),
                              EEPROM_IR_TEMP_SENSOR_DATA_REV);
    if (!is_ok) {
        printf("IR temp sensor: unable to read config, feature disabled this boot\n");
        return false;
    }

    eeprom_register_handler(ir_temp_sensor_config_save);

    _recreate_temp_filter(ir_temp_sensor.eeprom_ir_temp_sensor_data.filter_window);

    // Bring up the bus and probe for the sensor. Fully optional hardware - if it
    // doesn't ACK, disable gracefully rather than failing boot (same pattern as
    // motors_init()'s non-fatal degrade path).
    i2c_init(IR_TEMP_SENSOR_I2C, 100 * 1000);
    gpio_set_function(ir_temp_sensor.eeprom_ir_temp_sensor_data.sda_pin, GPIO_FUNC_I2C);
    gpio_set_function(ir_temp_sensor.eeprom_ir_temp_sensor_data.scl_pin, GPIO_FUNC_I2C);
    gpio_pull_up(ir_temp_sensor.eeprom_ir_temp_sensor_data.sda_pin);
    gpio_pull_up(ir_temp_sensor.eeprom_ir_temp_sensor_data.scl_pin);

    uint16_t probe_raw;
    ir_temp_sensor.sensor_present = _mlx90614_read_word(MLX90614_RAM_TA, &probe_raw);

    if (!ir_temp_sensor.sensor_present) {
        printf("IR temp sensor: no ACK at probe, feature disabled this boot\n");
        return true;
    }

    printf("IR temp sensor: MLX90614 present\n");
    _sync_emissivity_if_needed();

    xTaskCreate(ir_temp_sensor_poll_task, "IR Temp Sensor Poll Task", configMINIMAL_STACK_SIZE, NULL, 2, &ir_temp_sensor.poll_task_handler);

    return true;
}


bool ir_temp_sensor_config_save(void) {
    return save_config(EEPROM_IR_TEMP_SENSOR_BASE_ADDR, &ir_temp_sensor.eeprom_ir_temp_sensor_data, sizeof(ir_temp_sensor.eeprom_ir_temp_sensor_data));
}


bool ir_temp_sensor_is_enabled(void) {
    return ir_temp_sensor.eeprom_ir_temp_sensor_data.enabled && ir_temp_sensor.sensor_present;
}


bool ir_temp_sensor_is_initializing(void) {
    return ir_temp_sensor_is_enabled() &&
        (ir_temp_sensor.object_temp_filter->getCounter() < ir_temp_sensor.eeprom_ir_temp_sensor_data.filter_window);
}


bool ir_temp_sensor_is_healthy(void) {
    return ir_temp_sensor_is_enabled() &&
        !ir_temp_sensor_is_initializing() &&
        (ir_temp_sensor.consecutive_read_failures < IR_TEMP_SENSOR_MAX_CONSECUTIVE_FAILURES);
}


float ir_temp_sensor_get_object_temp_c(void) {
    return ir_temp_sensor.object_temp_c;
}


float ir_temp_sensor_get_ambient_temp_c(void) {
    return ir_temp_sensor.ambient_temp_c;
}


bool http_rest_ir_temp_sensor_config(struct fs_file *file, int num_params, char *params[], char *values[]) {
    // Mappings:
    // t0 (bool): enabled (global temperature-mode toggle)
    // t1 (int): sda_pin
    // t2 (int): scl_pin
    // t3 (float): emissivity (0.0-1.0)
    // t4 (float): offset_c
    // t5 (int): filter_window
    // ee (bool): save to eeprom

    static char json_buffer[256];
    bool save_to_eeprom = false;
    bool filter_window_changed = false;

    for (int idx = 0; idx < num_params; idx += 1) {
        if (strcmp(params[idx], "t0") == 0) {
            ir_temp_sensor.eeprom_ir_temp_sensor_data.enabled = string_to_boolean(values[idx]);
        }
        else if (strcmp(params[idx], "t1") == 0) {
            ir_temp_sensor.eeprom_ir_temp_sensor_data.sda_pin = (uint8_t) atoi(values[idx]);
        }
        else if (strcmp(params[idx], "t2") == 0) {
            ir_temp_sensor.eeprom_ir_temp_sensor_data.scl_pin = (uint8_t) atoi(values[idx]);
        }
        else if (strcmp(params[idx], "t3") == 0) {
            ir_temp_sensor.eeprom_ir_temp_sensor_data.emissivity = strtof(values[idx], NULL);
        }
        else if (strcmp(params[idx], "t4") == 0) {
            ir_temp_sensor.eeprom_ir_temp_sensor_data.offset_c = strtof(values[idx], NULL);
        }
        else if (strcmp(params[idx], "t5") == 0) {
            ir_temp_sensor.eeprom_ir_temp_sensor_data.filter_window = _clamp_filter_window((uint8_t) atoi(values[idx]));
            filter_window_changed = true;
        }
        else if (strcmp(params[idx], "ee") == 0) {
            save_to_eeprom = string_to_boolean(values[idx]);
        }
    }

    if (filter_window_changed) {
        _recreate_temp_filter(ir_temp_sensor.eeprom_ir_temp_sensor_data.filter_window);
    }

    if (save_to_eeprom) {
        ir_temp_sensor_config_save();
    }

    snprintf(json_buffer,
             sizeof(json_buffer),
             "%s"
             "{\"t0\":%s,\"t1\":%d,\"t2\":%d,\"t3\":%0.4f,\"t4\":%0.2f,\"t5\":%d}",
             http_json_header,
             boolean_to_string(ir_temp_sensor.eeprom_ir_temp_sensor_data.enabled),
             ir_temp_sensor.eeprom_ir_temp_sensor_data.sda_pin,
             ir_temp_sensor.eeprom_ir_temp_sensor_data.scl_pin,
             ir_temp_sensor.eeprom_ir_temp_sensor_data.emissivity,
             ir_temp_sensor.eeprom_ir_temp_sensor_data.offset_c,
             ir_temp_sensor.eeprom_ir_temp_sensor_data.filter_window);

    size_t data_length = strlen(json_buffer);
    file->data = json_buffer;
    file->len = data_length;
    file->index = data_length;
    file->flags = FS_FILE_FLAGS_HEADER_INCLUDED;

    return true;
}


bool http_rest_ir_temp_sensor_state(struct fs_file *file, int num_params, char *params[], char *values[]) {
    // Read-only:
    // s0 (bool): sensor physically present (probed at boot)
    // s1 (bool): healthy (present, enabled, filter warmed up, and reads currently succeeding)
    // s2 (float): live object temperature, Celsius (filtered + offset applied)
    // s3 (float): live ambient temperature, Celsius
    // s4 (bool): initializing (filter still collecting its first window of samples -
    //            distinct from a real fault, see ir_temp_sensor_is_initializing())

    static char json_buffer[192];

    snprintf(json_buffer,
             sizeof(json_buffer),
             "%s"
             "{\"s0\":%s,\"s1\":%s,\"s2\":%0.2f,\"s3\":%0.2f,\"s4\":%s}",
             http_json_header,
             boolean_to_string(ir_temp_sensor.sensor_present),
             boolean_to_string(ir_temp_sensor_is_healthy()),
             ir_temp_sensor.object_temp_c,
             ir_temp_sensor.ambient_temp_c,
             boolean_to_string(ir_temp_sensor_is_initializing()));

    size_t data_length = strlen(json_buffer);
    file->data = json_buffer;
    file->len = data_length;
    file->index = data_length;
    file->flags = FS_FILE_FLAGS_HEADER_INCLUDED;

    return true;
}

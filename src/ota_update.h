#ifndef OTA_UPDATE_H_
#define OTA_UPDATE_H_

#include <stdint.h>
#include <stdbool.h>
#include "http_rest.h"

// Fixed 2-partition A/B layout, must match targets/partition_table.json exactly.
// Hardcoded rather than resolved at runtime via rom_get_uf2_target_partition(): this
// firmware only ever deals with its own fixed pair, so there's no need for the more
// general (and, as of this writing, not yet hardware-validated) partition-lookup path.
#define OTA_PARTITION_A_FLASH_OFFSET    0x00002000u
#define OTA_PARTITION_B_FLASH_OFFSET    0x00182000u
#define OTA_PARTITION_SIZE_BYTES        (1536u * 1024u)

#define EEPROM_OTA_DATA_REV             1

typedef enum {
    OTA_RESULT_NONE = 0,        // no update has been attempted since this field was last valid
    OTA_RESULT_CONFIRMED = 1,   // the most recent update booted, stayed up, and was bought
    OTA_RESULT_ROLLED_BACK = 2, // the most recent update never confirmed; bootrom fell back
} ota_result_t;

// Persisted across reboots/rollbacks so the OLD image can recognise "I came back after
// an update that didn't stick" - flash partition state doesn't help here, since after a
// rollback we're running the old image again, not the failed one.
typedef struct {
    uint16_t ota_data_rev;
    bool     update_pending;     // true: a candidate image was written+triggered, not yet confirmed
    uint8_t  target_partition;   // which partition (0/1) that candidate was written to
    uint8_t  last_result;        // ota_result_t, persisted so REST can report it after a plain reboot
} __attribute__((packed)) eeprom_ota_data_t;

#ifdef __cplusplus
extern "C" {
#endif

// Loads persisted OTA state, determines the currently running partition, and - if the
// last known attempt targeted a DIFFERENT partition than the one we're running on now -
// recognises that as a failed/rolled-back update and surfaces it (LCD, neopixel, REST)
// until a new update attempt starts. If instead we're running on the partition that was
// the pending candidate, starts a delayed self-confirmation timer rather than confirming
// immediately - see ota_update.c for why immediate confirmation would defeat the point.
// Must run after eeprom_init() and after the LCD/neopixel are initialised.
void ota_update_init(void);

// REST handler for /rest/ota_update. GET-only status/control, matching every other
// handler in this codebase - firmware upload itself goes through the separate
// httpd_post_* callbacks in ota_update.c (POST body streamed straight to the inactive
// partition), not through this query-param style handler.
bool http_rest_ota_update(struct fs_file *file, int num_params, char *params[], char *values[]);

// Small read-only accessors for system_control.c's /rest/system_control to report
// alongside VCS hash/build type, without system_control.c needing to know OTA internals.
char ota_update_get_running_partition_letter(void);
const char * ota_update_get_last_result_string(void);

#ifdef __cplusplus
}
#endif

#endif  // OTA_UPDATE_H_

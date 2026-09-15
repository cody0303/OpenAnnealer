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
#define EEPROM_OTA_SETTINGS_REV         0

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

// Deliberately a SEPARATE EEPROM slot/struct from eeprom_ota_data_t above, not just another
// field on it. eeprom_ota_data_t's update_pending/target_partition are written by the OLD
// firmware immediately before it triggers a reboot into a new candidate, specifically so
// the NEW firmware can read them back and recognise "I'm the fresh candidate, don't confirm
// until proven stable" (see ota_update_init()). A rev bump on that struct makes load_config()
// discard that just-written state as a schema mismatch, so the new candidate never arms its
// health-check/confirm logic and never calls rom_explicit_buy() - it sits as a permanently
// tentative TBYB image, and the next reboot for ANY reason rolls it back. Learned this the
// hard way: adding debug_mode directly to eeprom_ota_data_t (and bumping its rev) caused
// exactly this - a real, silent rollback on real hardware. debug_mode has nothing to do with
// that transition's bookkeeping, so it lives in its own slot that can be freely
// versioned/extended without ever touching the update_pending continuity requirement.
typedef struct {
    uint16_t ota_settings_rev;
    bool     debug_mode;         // must be true for httpd_post_begin() to accept an /ota_upload -
                                  // off by default so a non-technical end user can't stumble into a
                                  // confusing/risky firmware upload; persists across reboots so a
                                  // maintainer doesn't have to re-enable it after every single update
} __attribute__((packed)) eeprom_ota_settings_t;

#ifdef __cplusplus
extern "C" {
#endif

// Loads persisted OTA state, determines the currently running partition, and - if the
// last known attempt targeted a DIFFERENT partition than the one we're running on now -
// recognises that as a failed/rolled-back update and surfaces it (LCD, neopixel, REST)
// until a new update attempt starts. If instead we're running on the partition that was
// the pending candidate, starts a periodic network-health-check timer that confirms once
// connectivity is proven stable (or a fallback timeout elapses) rather than confirming
// immediately or on a flat timer alone - see ota_update.c for details.
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

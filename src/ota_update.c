#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "pico/bootrom.h"
#include "pico/flash.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "lwip/pbuf.h"

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"

#include "ota_update.h"
#include "eeprom.h"
#include "common.h"
#include "display.h"
#include "neopixel_led.h"
#include "wireless.h"

// A freshly-booted candidate image is only allowed to confirm itself (rom_explicit_buy())
// once the network has actually proven itself reachable, not just after a flat delay -
// deliberately not "immediately on boot" either, since that would defeat the entire point
// of TBYB (an image that hangs/crashes a few seconds in would already be permanent).
//
// wireless_is_network_ready() is polled on this interval; the network must read ready for
// a continuous OTA_HEALTH_MIN_STABLE_MS before it counts (guards against confirming on a
// flaky first join that immediately drops again). OTA_HEALTH_MAX_WAIT_MS is a fallback
// only - if the network never comes up at all (e.g. no WiFi configured, hardware fault),
// this still confirms eventually rather than leaving the image permanently tentative
// forever, matching the old flat-timer behaviour as a worst case rather than the norm.
#define OTA_HEALTH_POLL_INTERVAL_MS 1000
#define OTA_HEALTH_MIN_STABLE_MS    5000
#define OTA_HEALTH_MAX_WAIT_MS      60000

// Delay between finishing a successful upload (so the HTTP response has time to reach
// the client) and actually triggering the flash-update reboot.
#define OTA_REBOOT_DELAY_MS        750

#define OTA_UPLOAD_URI             "/ota_upload"
#define OTA_STATUS_URI             "/rest/ota_update"

typedef struct {
    eeprom_ota_data_t eeprom_ota_data;
    eeprom_ota_settings_t eeprom_ota_settings;

    int8_t running_partition;      // from rom_get_boot_info(), -1 if unknown
    bool rollback_fault;           // true after boot-time rollback detection, until acknowledged
    TimerHandle_t confirm_timer;
    TickType_t health_check_start_tick;
    TickType_t network_ready_since_tick;   // only meaningful while network_currently_ready
    bool network_currently_ready;

    // In-progress upload state (not persisted - purely transient).
    bool download_active;
    bool download_failed;
    uint32_t target_offset;        // absolute flash offset of the inactive partition
    uint8_t target_partition;      // 0 or 1
    uint32_t expected_len;
    uint32_t bytes_received;
    uint8_t staging[FLASH_SECTOR_SIZE];
    uint32_t staging_fill;
    uint32_t next_write_offset;    // offset within target_offset..+size already erased/programmed
    bool last_upload_failed;       // sticky until the next upload attempt begins
} ota_update_t;

static ota_update_t ota;

const eeprom_ota_data_t default_ota_data = {
    .ota_data_rev = 0,
    .update_pending = false,
    .target_partition = 0,
    .last_result = OTA_RESULT_NONE,
};

const eeprom_ota_settings_t default_ota_settings = {
    .ota_settings_rev = 0,
    .debug_mode = false,
};


static uint32_t _ota_other_partition_offset(int8_t running_partition, uint8_t *out_partition) {
    if (running_partition == 0) {
        if (out_partition) *out_partition = 1;
        return OTA_PARTITION_B_FLASH_OFFSET;
    } else {
        if (out_partition) *out_partition = 0;
        return OTA_PARTITION_A_FLASH_OFFSET;
    }
}


bool ota_update_config_save(void) {
    return save_config(EEPROM_OTA_DATA_BASE_ADDR, &ota.eeprom_ota_data, sizeof(ota.eeprom_ota_data));
}


bool ota_update_settings_save(void) {
    return save_config(EEPROM_OTA_SETTINGS_BASE_ADDR, &ota.eeprom_ota_settings, sizeof(ota.eeprom_ota_settings));
}


// Mirrors handle_motor_init_error()'s raw-draw idiom (motors.c) - this runs outside
// the normal mui menu navigation, so it draws directly rather than going through a
// MUI_FORM. Bounded loop so boot always continues afterward.
static void _ota_show_rollback_fault(void) {
    u8g2_t * display_handler = get_display_handler();
    const int REPEAT_COUNT = 3;

    for (int repeat = 0; repeat < REPEAT_COUNT; repeat += 1) {
        BaseType_t scheduler_state = xTaskGetSchedulerState();

        _neopixel_led_set_colour(0xFF0000, 0xFF0000, 0xffffff);
        delay_ms(200, scheduler_state);
        _neopixel_led_set_colour(0x000000, 0x000000, 0xffffff);
        delay_ms(200, scheduler_state);

        u8g2_ClearBuffer(display_handler);
        u8g2_SetFont(display_handler, u8g2_font_helvB08_tr);
        u8g2_DrawStr(display_handler, 5, 10, "OTA Update Failed");
        u8g2_DrawHLine(display_handler, 0, 13, u8g2_GetDisplayWidth(display_handler));
        u8g2_SetFont(display_handler, u8g2_font_profont11_tf);
        u8g2_DrawStr(display_handler, 5, 25, "Rolled back to");
        u8g2_DrawStr(display_handler, 5, 37, "previous firmware");
        u8g2_SendBuffer(display_handler);

        delay_ms(2000, scheduler_state);
    }
}


static void _ota_confirm_now(void) {
    static uint8_t buy_scratch[4096] __attribute__((aligned(4)));
#if OTA_DEBUG_SERIAL
    printf("OTA: calling rom_explicit_buy()...\n");
#endif
    int rc = rom_explicit_buy(buy_scratch, sizeof(buy_scratch));
#if OTA_DEBUG_SERIAL
    printf("OTA: rom_explicit_buy() returned rc=%d\n", rc);
#endif

    if (rc >= 0) {
        printf("OTA: image confirmed (rom_explicit_buy rc=%d)\n", rc);
        ota.eeprom_ota_data.update_pending = false;
        ota.eeprom_ota_data.last_result = OTA_RESULT_CONFIRMED;
        ota_update_config_save();
    } else {
        printf("OTA: rom_explicit_buy() FAILED rc=%d - leaving update_pending set\n", rc);
    }
}


// Runs every OTA_HEALTH_POLL_INTERVAL_MS while a candidate image awaits confirmation.
// Confirms once the network has been continuously ready for OTA_HEALTH_MIN_STABLE_MS,
// or unconditionally once OTA_HEALTH_MAX_WAIT_MS has elapsed as a fallback - see the
// macro comments above for why the fallback exists.
static void _ota_health_check_timer_callback(TimerHandle_t timer) {
    TickType_t now = xTaskGetTickCount();
    bool ready = wireless_is_network_ready();

    if (ready) {
        if (!ota.network_currently_ready) {
            ota.network_ready_since_tick = now;
            ota.network_currently_ready = true;
        }
    } else {
        ota.network_currently_ready = false;
    }

    uint32_t elapsed_ms = (now - ota.health_check_start_tick) * portTICK_PERIOD_MS;
    bool stable_long_enough = ota.network_currently_ready &&
        ((now - ota.network_ready_since_tick) * portTICK_PERIOD_MS >= OTA_HEALTH_MIN_STABLE_MS);
    bool timed_out = elapsed_ms >= OTA_HEALTH_MAX_WAIT_MS;

#if OTA_DEBUG_SERIAL
    printf("OTA: health check tick, elapsed=%lu ms, ready=%d, stable_long_enough=%d, raw_link_status=%d\n",
           elapsed_ms, ready, stable_long_enough, wireless_get_raw_link_status_for_debug());
#endif

    if (!stable_long_enough && !timed_out) {
        return;
    }

    xTimerStop(timer, 0);

    if (timed_out && !stable_long_enough) {
        printf("OTA: network health check timed out after %lu ms, confirming anyway (fallback)\n", elapsed_ms);
    } else {
        printf("OTA: network ready and stable, confirming update\n");
    }

    _ota_confirm_now();
}


void ota_update_init(void) {
    memset(&ota, 0x0, sizeof(ota));
    ota.running_partition = -1;

    boot_info_t boot_info;
    memset(&boot_info, 0, sizeof(boot_info));
    if (rom_get_boot_info(&boot_info)) {
        ota.running_partition = boot_info.partition;
    } else {
        printf("OTA: rom_get_boot_info() failed, cannot determine running partition\n");
    }

    bool is_ok = load_config(EEPROM_OTA_DATA_BASE_ADDR,
                              &ota.eeprom_ota_data,
                              &default_ota_data,
                              sizeof(ota.eeprom_ota_data),
                              EEPROM_OTA_DATA_REV);

    // Separate slot from eeprom_ota_data above - see eeprom_ota_settings_t's comment in
    // ota_update.h for why debug_mode must never share a struct/rev with update_pending.
    load_config(EEPROM_OTA_SETTINGS_BASE_ADDR,
                &ota.eeprom_ota_settings,
                &default_ota_settings,
                sizeof(ota.eeprom_ota_settings),
                EEPROM_OTA_SETTINGS_REV);

    eeprom_register_handler(ota_update_config_save);
    eeprom_register_handler(ota_update_settings_save);

    if (!is_ok) {
        // Raw EEPROM read failure (not a CRC/rev mismatch - load_config() already
        // defaults those). Deliberately don't guess here: don't auto-confirm (could
        // wrongly make a bad update permanent) and don't raise a fault (could be a
        // false alarm unrelated to any real update). Just leave things as they are;
        // the next successful boot will re-evaluate from a fresh EEPROM read.
        printf("OTA: unable to read OTA state from EEPROM, skipping this boot's check\n");
        return;
    }

    if (!ota.eeprom_ota_data.update_pending) {
        return;
    }

    if (ota.running_partition >= 0 && ota.eeprom_ota_data.target_partition == (uint8_t) ota.running_partition) {
        // We are the freshly-flashed candidate. Don't confirm immediately - poll for
        // real network health first (see _ota_health_check_timer_callback()).
        printf("OTA: running as pending candidate on partition %d, waiting for network health before self-confirming\n",
               ota.running_partition);
        ota.health_check_start_tick = xTaskGetTickCount();
        ota.network_currently_ready = false;
        ota.confirm_timer = xTimerCreate("OtaHealthCheck", pdMS_TO_TICKS(OTA_HEALTH_POLL_INTERVAL_MS), pdTRUE, NULL, _ota_health_check_timer_callback);
        if (ota.confirm_timer != NULL) {
            xTimerStart(ota.confirm_timer, portMAX_DELAY);
        }
    } else {
        // We attempted an update targeting a different partition than the one we're
        // running on now - the bootrom fell back to us, so that update never stuck.
        printf("OTA: rollback detected (attempted partition %d, running on %d)\n",
               ota.eeprom_ota_data.target_partition, ota.running_partition);
        ota.rollback_fault = true;
        ota.eeprom_ota_data.update_pending = false;
        ota.eeprom_ota_data.last_result = OTA_RESULT_ROLLED_BACK;
        ota_update_config_save();
        _ota_show_rollback_fault();
    }
}


static void _do_flash_erase_and_program(void *param) {
    uint32_t offset = *(uint32_t *) param;
    flash_range_erase(offset, FLASH_SECTOR_SIZE);
    flash_range_program(offset, ota.staging, FLASH_SECTOR_SIZE);
}


// Writes exactly one FLASH_SECTOR_SIZE-sized staging buffer to flash at
// target_offset + next_write_offset, multicore-safely. Blocks the calling task
// (the lwIP/tcpip task, when called from httpd_post_receive_data()) for the
// duration of the erase+program - a bounded, one-off cost during a deliberate
// firmware update, not routine traffic.
static bool _ota_flush_staging_sector(void) {
    uint32_t offset = ota.target_offset + ota.next_write_offset;
    int rc = flash_safe_execute(_do_flash_erase_and_program, &offset, 2000);
    if (rc != PICO_OK) {
        printf("OTA: flash_safe_execute() failed rc=%d at offset 0x%08lx\n", rc, offset);
        return false;
    }
    ota.next_write_offset += FLASH_SECTOR_SIZE;
    ota.staging_fill = 0;
    return true;
}


static void _ota_reboot_timer_callback(TimerHandle_t timer) {
    (void) timer;
    uint32_t target_runtime_addr = 0x10000000u + ota.target_offset;
    printf("OTA: triggering flash-update boot into partition %d at 0x%08lx\n",
           ota.target_partition, target_runtime_addr);
    rom_reboot(BOOT_TYPE_FLASH_UPDATE, 10, target_runtime_addr, 0);
    // Does not return on success.
    printf("OTA: rom_reboot() returned - did not reboot\n");
}


err_t httpd_post_begin(void *connection, const char *uri, const char *http_request,
                        u16_t http_request_len, int content_len, char *response_uri,
                        u16_t response_uri_len, u8_t *post_auto_wnd) {
    (void) connection;
    (void) http_request;
    (void) http_request_len;

    *post_auto_wnd = 1;
    snprintf(response_uri, response_uri_len, "%s", OTA_STATUS_URI);

    // Prefix match (not exact equality) so a trailing query string still matches,
    // but require the prefix to end the path segment (end of string or '?') so
    // "/ota_uploadxyz" doesn't false-positive.
    size_t prefix_len = strlen(OTA_UPLOAD_URI);
    if (strncmp(uri, OTA_UPLOAD_URI, prefix_len) != 0 ||
        (uri[prefix_len] != '\0' && uri[prefix_len] != '?')) {
        return ERR_VAL;
    }

    if (!ota.eeprom_ota_settings.debug_mode) {
        printf("OTA: rejecting upload, debug mode is off\n");
        return ERR_VAL;
    }

    if (ota.download_active) {
        printf("OTA: rejecting upload, one is already in progress\n");
        return ERR_INPROGRESS;
    }

    if (content_len <= 0 || (uint32_t) content_len > OTA_PARTITION_SIZE_BYTES) {
        printf("OTA: rejecting upload, bad content length %d\n", content_len);
        return ERR_VAL;
    }

    // _ota_other_partition_offset() only knows about our fixed 2-partition layout
    // (0 or 1); anything else must be rejected outright rather than guessed at - a
    // wrong guess here means writing into the partition we're actually running from.
    if (ota.running_partition != 0 && ota.running_partition != 1) {
        printf("OTA: rejecting upload, running partition %d is not a recognised A/B slot\n", ota.running_partition);
        return ERR_VAL;
    }

    ota.target_offset = _ota_other_partition_offset((int8_t) ota.running_partition, &ota.target_partition);
    ota.expected_len = (uint32_t) content_len;
    ota.bytes_received = 0;
    ota.staging_fill = 0;
    ota.next_write_offset = 0;
    ota.download_failed = false;
    ota.last_upload_failed = false;
    ota.download_active = true;

    printf("OTA: accepting upload of %lu bytes into partition %d (offset 0x%08lx)\n",
           ota.expected_len, ota.target_partition, ota.target_offset);

    return ERR_OK;
}


static void _ota_ingest_bytes(const uint8_t *data, uint16_t len) {
    while (len > 0 && !ota.download_failed) {
        uint32_t space = sizeof(ota.staging) - ota.staging_fill;
        uint32_t take = (len < space) ? len : space;

        memcpy(ota.staging + ota.staging_fill, data, take);
        ota.staging_fill += take;
        ota.bytes_received += take;
        data += take;
        len -= take;

        if (ota.staging_fill == sizeof(ota.staging)) {
            if (!_ota_flush_staging_sector()) {
                ota.download_failed = true;
            }
        }
    }
}


err_t httpd_post_receive_data(void *connection, struct pbuf *p) {
    (void) connection;

    if (!ota.download_active) {
        pbuf_free(p);
        return ERR_VAL;
    }

    if (!ota.download_failed) {
        if (ota.bytes_received + p->tot_len > ota.expected_len) {
            printf("OTA: received more data than declared Content-Length, aborting\n");
            ota.download_failed = true;
        } else {
            for (struct pbuf *q = p; q != NULL; q = q->next) {
                _ota_ingest_bytes((const uint8_t *) q->payload, q->len);
            }
        }
    }

    pbuf_free(p);
    return ota.download_failed ? ERR_VAL : ERR_OK;
}


void httpd_post_finished(void *connection, char *response_uri, u16_t response_uri_len) {
    (void) connection;

    snprintf(response_uri, response_uri_len, "%s", OTA_STATUS_URI);

    if (!ota.download_active) {
        return;
    }

    ota.download_active = false;

    if (ota.download_failed || ota.bytes_received != ota.expected_len) {
        printf("OTA: upload failed (received %lu of %lu bytes)\n", ota.bytes_received, ota.expected_len);
        ota.last_upload_failed = true;
        return;
    }

    // Flush any partial trailing sector. flash_range_program() requires a
    // page-aligned (256 byte) length; pad the unused tail with 0xFF, which is what
    // erased flash already reads as, so it's indistinguishable from unwritten space.
    if (ota.staging_fill > 0) {
        // _do_flash_erase_and_program() always writes a full FLASH_SECTOR_SIZE from
        // ota.staging, so pad the unused tail with 0xFF (what erased flash already
        // reads as) rather than tracking a separate partial-page length.
        memset(ota.staging + ota.staging_fill, 0xFF, sizeof(ota.staging) - ota.staging_fill);

        uint32_t offset = ota.target_offset + ota.next_write_offset;
        int rc = flash_safe_execute(_do_flash_erase_and_program, &offset, 2000);
        if (rc != PICO_OK) {
            printf("OTA: final sector flash write failed rc=%d\n", rc);
            ota.last_upload_failed = true;
            return;
        }
    }

    printf("OTA: upload complete (%lu bytes), scheduling flash-update reboot into partition %d\n",
           ota.bytes_received, ota.target_partition);

    ota.eeprom_ota_data.update_pending = true;
    ota.eeprom_ota_data.target_partition = ota.target_partition;
    ota_update_config_save();

    TimerHandle_t reboot_timer = xTimerCreate("OtaReboot", pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS), pdFALSE, NULL, _ota_reboot_timer_callback);
    if (reboot_timer != NULL) {
        xTimerStart(reboot_timer, portMAX_DELAY);
    }
}


char ota_update_get_running_partition_letter(void) {
    if (ota.running_partition == 0) return 'A';
    if (ota.running_partition == 1) return 'B';
    return '?';
}


const char * ota_update_get_last_result_string(void) {
    switch (ota.eeprom_ota_data.last_result) {
        case OTA_RESULT_CONFIRMED: return "confirmed";
        case OTA_RESULT_ROLLED_BACK: return "rolled_back";
        default: return "none";
    }
}


bool http_rest_ota_update(struct fs_file *file, int num_params, char *params[], char *values[]) {
    // Mappings:
    // ack (bool): acknowledge/clear a rollback fault
    // dbg (bool): enable/disable debug mode (required for /ota_upload to accept anything -
    //             see httpd_post_begin()). Persisted immediately, not gated behind a
    //             separate "ee" save flag - this is the only field this endpoint writes.

    static char json_buffer[256];

    for (int idx = 0; idx < num_params; idx += 1) {
        if (strcmp(params[idx], "ack") == 0 && string_to_boolean(values[idx])) {
            ota.rollback_fault = false;
        }
        else if (strcmp(params[idx], "dbg") == 0) {
            ota.eeprom_ota_settings.debug_mode = string_to_boolean(values[idx]);
            ota_update_settings_save();
        }
    }

    char partition_letter = ota_update_get_running_partition_letter();

    snprintf(json_buffer,
             sizeof(json_buffer),
             "%s"
             "{\"partition\":\"%c\",\"pending\":%s,\"last_result\":\"%s\",\"fault\":%s,"
             "\"downloading\":%s,\"bytes_received\":%lu,\"expected_len\":%lu,\"upload_failed\":%s,"
             "\"debug_mode\":%s}",
             http_json_header,
             partition_letter,
             boolean_to_string(ota.eeprom_ota_data.update_pending),
             ota_update_get_last_result_string(),
             boolean_to_string(ota.rollback_fault),
             boolean_to_string(ota.download_active),
             ota.bytes_received,
             ota.expected_len,
             boolean_to_string(ota.last_upload_failed),
             boolean_to_string(ota.eeprom_ota_settings.debug_mode));

    size_t data_length = strlen(json_buffer);
    file->data = json_buffer;
    file->len = data_length;
    file->index = data_length;
    file->flags = FS_FILE_FLAGS_HEADER_INCLUDED;

    return true;
}

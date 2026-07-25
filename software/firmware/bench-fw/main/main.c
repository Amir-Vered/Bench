#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"

#include "driver/gpio.h"

#define WIFI_SSID CONFIG_BENCH_WIFI_SSID
#define WIFI_PASS CONFIG_BENCH_WIFI_PASSWORD

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define LOG_LINE_COUNT 16
#define LOG_LINE_LEN   128

#define CDC_RX_BUF_SIZE 1024

#define UPLOAD_BODY_MAX_BYTES 160000
#define UNO_FLASH_SIZE        32768
#define UNO_PAGE_SIZE         128

#define STK_OK        0x10
#define STK_INSYNC    0x14
#define CRC_EOP       0x20
#define STK_GET_SYNC  0x30
#define STK_LOAD_ADDR 0x55
#define STK_PROG_PAGE 0x64
#define STK_READ_PAGE 0x74
#define STK_LEAVE     0x51

#define GPIO_LED_WIFI     GPIO_NUM_13
#define GPIO_LED_TARGET   GPIO_NUM_14
#define GPIO_LED_UPLOAD   GPIO_NUM_15
#define GPIO_LED_FAULT    GPIO_NUM_16

#define GPIO_TPS_FAULT    GPIO_NUM_47
#define GPIO_TPS_EN       GPIO_NUM_48

#define LED_ON            1
#define LED_OFF           0

#define TPS_EN_ON         1
#define TPS_EN_OFF        0

#define TPS_FAULT_ACTIVE  0

static const char *TAG = "bench-fw";

static EventGroupHandle_t wifi_event_group;
static int wifi_retry_count = 0;
static const int WIFI_MAX_RETRIES = 5;

static bool target_present = false;
static bool target_power_enabled = true;
static bool target_power_fault = false;

static char log_lines[LOG_LINE_COUNT][LOG_LINE_LEN];
static int log_line_index = 0;

static TaskHandle_t usb_host_task_handle = NULL;
static bool usb_host_ready = false;

static SemaphoreHandle_t cdc_rx_mutex = NULL;
static SemaphoreHandle_t cdc_rx_sem = NULL;
static uint8_t cdc_rx_buf[CDC_RX_BUF_SIZE];
static size_t cdc_rx_head = 0;
static size_t cdc_rx_tail = 0;

static SemaphoreHandle_t upload_mutex = NULL;
static bool upload_running = false;
static int upload_job_counter = 0;
static int upload_progress = 0;
static char upload_job_id[32] = "";
static char upload_state[32] = "idle";
static char upload_message[96] = "idle";
static char upload_error[40] = "";

typedef struct {
    char *hex_text;
    size_t hex_len;
    bool reset;
    bool verify;
} upload_task_args_t;

typedef struct {
    uint8_t image[UNO_FLASH_SIZE];
    uint32_t highest_addr;
} flash_image_t;

static void bench_log(const char *fmt, ...);
static void bench_gpio_update_status_leds(void);

static void bench_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    vsnprintf(log_lines[log_line_index], LOG_LINE_LEN, fmt, args);

    va_end(args);

    ESP_LOGI(TAG, "%s", log_lines[log_line_index]);

    log_line_index = (log_line_index + 1) % LOG_LINE_COUNT;
}

static esp_err_t send_json(httpd_req_t *req, const char *json) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_json_status(httpd_req_t *req, const char *status, const char *json) {
    httpd_resp_set_status(req, status);
    return send_json(req, json);
}

static char *find_bytes(const char *haystack, size_t haystack_len, const char *needle, size_t needle_len) {
    if (needle_len == 0 || haystack_len < needle_len) {
        return NULL;
    }

    for (size_t i = 0; i <= haystack_len - needle_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            return (char *) haystack + i;
        }
    }

    return NULL;
}

static bool header_has_name(const char *headers, size_t headers_len, const char *name) {
    char pattern[64];

    snprintf(pattern, sizeof(pattern), "name=\"%s\"", name);

    return find_bytes(headers, headers_len, pattern, strlen(pattern)) != NULL;
}

static bool field_is_false(const char *data, size_t len) {
    while (len > 0 && (*data == ' ' || *data == '\r' || *data == '\n' || *data == '\t')) {
        data++;
        len--;
    }

    while (len > 0 &&
           (data[len - 1] == ' ' ||
            data[len - 1] == '\r' ||
            data[len - 1] == '\n' ||
            data[len - 1] == '\t')) {
        len--;
    }

    return len == 5 && memcmp(data, "false", 5) == 0;
}

static esp_err_t copy_part(char **out, size_t *out_len, const char *data, size_t len) {
    char *copy = malloc(len + 1);

    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memcpy(copy, data, len);
    copy[len] = '\0';

    *out = copy;
    *out_len = len;

    return ESP_OK;
}

static esp_err_t extract_upload_parts(
    char *body,
    size_t body_len,
    const char *content_type,
    upload_task_args_t *args
) {
    args->hex_text = NULL;
    args->hex_len = 0;
    args->reset = true;
    args->verify = true;

    if (content_type == NULL || strstr(content_type, "multipart/form-data") == NULL) {
        return copy_part(&args->hex_text, &args->hex_len, body, body_len);
    }

    const char *boundary_key = "boundary=";
    const char *boundary_start = strstr(content_type, boundary_key);

    if (boundary_start == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    boundary_start += strlen(boundary_key);

    char boundary[96] = {0};
    size_t boundary_len = 0;

    if (*boundary_start == '"') {
        boundary_start++;

        while (*boundary_start != '\0' &&
               *boundary_start != '"' &&
               boundary_len < sizeof(boundary) - 1) {
            boundary[boundary_len++] = *boundary_start++;
        }
    } else {
        while (*boundary_start != '\0' &&
               *boundary_start != ';' &&
               boundary_len < sizeof(boundary) - 1) {
            boundary[boundary_len++] = *boundary_start++;
        }
    }

    if (boundary_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char delimiter[128];

    snprintf(delimiter, sizeof(delimiter), "--%s", boundary);

    size_t delimiter_len = strlen(delimiter);
    char *cursor = body;
    char *body_end = body + body_len;

    while (cursor < body_end) {
        char *part = find_bytes(cursor, body_end - cursor, delimiter, delimiter_len);

        if (part == NULL) {
            break;
        }

        part += delimiter_len;

        if (part + 2 <= body_end && part[0] == '-' && part[1] == '-') {
            break;
        }

        if (part + 2 <= body_end && part[0] == '\r' && part[1] == '\n') {
            part += 2;
        }

        char *headers_end = find_bytes(part, body_end - part, "\r\n\r\n", 4);

        if (headers_end == NULL) {
            break;
        }

        char *content_start = headers_end + 4;
        char *next = find_bytes(content_start, body_end - content_start, delimiter, delimiter_len);

        if (next == NULL) {
            break;
        }

        char *content_end = next;

        if (content_end - content_start >= 2 &&
            content_end[-2] == '\r' &&
            content_end[-1] == '\n') {
            content_end -= 2;
        }

        size_t headers_len = headers_end - part;
        size_t content_len = content_end - content_start;

        if (header_has_name(part, headers_len, "file")) {
            esp_err_t result = copy_part(&args->hex_text, &args->hex_len, content_start, content_len);

            if (result != ESP_OK) {
                return result;
            }
        } else if (header_has_name(part, headers_len, "reset")) {
            if (field_is_false(content_start, content_len)) {
                args->reset = false;
            }
        } else if (header_has_name(part, headers_len, "verify")) {
            if (field_is_false(content_start, content_len)) {
                args->verify = false;
            }
        }

        cursor = next;
    }

    if (args->hex_text == NULL || args->hex_len == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_OK;
}

static void upload_set_state(const char *state, int progress, const char *message, const char *error) {
    if (upload_mutex != NULL) {
        xSemaphoreTake(upload_mutex, portMAX_DELAY);
    }

    strncpy(upload_state, state, sizeof(upload_state) - 1);
    upload_state[sizeof(upload_state) - 1] = '\0';

    upload_progress = progress;

    strncpy(upload_message, message, sizeof(upload_message) - 1);
    upload_message[sizeof(upload_message) - 1] = '\0';

    if (error != NULL) {
        strncpy(upload_error, error, sizeof(upload_error) - 1);
        upload_error[sizeof(upload_error) - 1] = '\0';
    } else {
        upload_error[0] = '\0';
    }

    if (upload_mutex != NULL) {
        xSemaphoreGive(upload_mutex);
    }

    bench_gpio_update_status_leds();
}

static void bench_gpio_set_leds(bool wifi, bool target, bool upload, bool fault) {
    gpio_set_level(GPIO_LED_WIFI, wifi ? LED_ON : LED_OFF);
    gpio_set_level(GPIO_LED_TARGET, target ? LED_ON : LED_OFF);
    gpio_set_level(GPIO_LED_UPLOAD, upload ? LED_ON : LED_OFF);
    gpio_set_level(GPIO_LED_FAULT, fault ? LED_ON : LED_OFF);
}

static void bench_gpio_set_target_power(bool enabled) {
    gpio_set_level(GPIO_TPS_EN, enabled ? TPS_EN_ON : TPS_EN_OFF);
    target_power_enabled = enabled;
}

static bool bench_gpio_read_power_fault(void) {
    target_power_fault = gpio_get_level(GPIO_TPS_FAULT) == TPS_FAULT_ACTIVE;
    return target_power_fault;
}

static void bench_gpio_update_status_leds(void) {
    bool upload_active = false;

    if (upload_mutex != NULL) {
        xSemaphoreTake(upload_mutex, portMAX_DELAY);
        upload_active = upload_running;
        xSemaphoreGive(upload_mutex);
    }

    bench_gpio_set_leds(
        true,
        target_present,
        upload_active,
        target_power_fault
    );
}

static esp_err_t bench_gpio_init(void) {
    gpio_config_t output_config = {
        .pin_bit_mask =
            (1ULL << GPIO_LED_WIFI) |
            (1ULL << GPIO_LED_TARGET) |
            (1ULL << GPIO_LED_UPLOAD) |
            (1ULL << GPIO_LED_FAULT) |
            (1ULL << GPIO_TPS_EN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&output_config));

    gpio_config_t input_config = {
        .pin_bit_mask = (1ULL << GPIO_TPS_FAULT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&input_config));

    bench_gpio_set_target_power(true);
    bench_gpio_read_power_fault();
    bench_gpio_update_status_leds();

    bench_log("GPIO initialized");

    return ESP_OK;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }

    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }

    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }

    return -1;
}

static esp_err_t parse_hex_byte(const char *text, uint8_t *value) {
    int high = hex_nibble(text[0]);
    int low = hex_nibble(text[1]);

    if (high < 0 || low < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *value = (uint8_t) ((high << 4) | low);

    return ESP_OK;
}

static esp_err_t parse_intel_hex(const char *text, size_t len, flash_image_t *flash) {
    memset(flash->image, 0xFF, sizeof(flash->image));
    flash->highest_addr = 0;

    uint32_t base_addr = 0;
    bool saw_eof = false;

    const char *cursor = text;
    const char *end = text + len;

    while (cursor < end) {
        while (cursor < end && (*cursor == '\r' || *cursor == '\n' || *cursor == ' ' || *cursor == '\t')) {
            cursor++;
        }

        if (cursor >= end) {
            break;
        }

        const char *line_start = cursor;

        while (cursor < end && *cursor != '\r' && *cursor != '\n') {
            cursor++;
        }

        size_t line_len = cursor - line_start;

        if (line_len == 0) {
            continue;
        }

        if (line_start[0] != ':') {
            return ESP_ERR_INVALID_ARG;
        }

        if (((line_len - 1) % 2) != 0 || line_len < 11) {
            return ESP_ERR_INVALID_SIZE;
        }

        size_t record_len = (line_len - 1) / 2;
        uint8_t record[280];

        if (record_len > sizeof(record)) {
            return ESP_ERR_INVALID_SIZE;
        }

        for (size_t i = 0; i < record_len; i++) {
            esp_err_t result = parse_hex_byte(line_start + 1 + (i * 2), &record[i]);

            if (result != ESP_OK) {
                return result;
            }
        }

        uint8_t checksum = 0;

        for (size_t i = 0; i < record_len; i++) {
            checksum = (uint8_t) (checksum + record[i]);
        }

        if (checksum != 0) {
            return ESP_ERR_INVALID_CRC;
        }

        uint8_t byte_count = record[0];
        uint16_t offset = ((uint16_t) record[1] << 8) | record[2];
        uint8_t record_type = record[3];

        if (record_len != (size_t) byte_count + 5) {
            return ESP_ERR_INVALID_SIZE;
        }

        if (record_type == 0x00) {
            uint32_t absolute_addr = base_addr + offset;

            if (absolute_addr + byte_count > UNO_FLASH_SIZE) {
                return ESP_ERR_INVALID_SIZE;
            }

            memcpy(&flash->image[absolute_addr], &record[4], byte_count);

            if (absolute_addr + byte_count > flash->highest_addr) {
                flash->highest_addr = absolute_addr + byte_count;
            }
        } else if (record_type == 0x01) {
            saw_eof = true;
            break;
        } else if (record_type == 0x02) {
            if (byte_count != 2) {
                return ESP_ERR_INVALID_SIZE;
            }

            base_addr = (((uint32_t) record[4] << 8) | record[5]) << 4;
        } else if (record_type == 0x04) {
            if (byte_count != 2) {
                return ESP_ERR_INVALID_SIZE;
            }

            base_addr = (((uint32_t) record[4] << 8) | record[5]) << 16;
        }
    }

    if (!saw_eof) {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static void cdc_rx_clear(void) {
    if (cdc_rx_mutex != NULL) {
        xSemaphoreTake(cdc_rx_mutex, portMAX_DELAY);
    }

    cdc_rx_head = 0;
    cdc_rx_tail = 0;

    if (cdc_rx_mutex != NULL) {
        xSemaphoreGive(cdc_rx_mutex);
    }

    while (cdc_rx_sem != NULL && xSemaphoreTake(cdc_rx_sem, 0) == pdTRUE) {
    }
}

static bool cdc_rx_push(const uint8_t *data, size_t len) {
    if (cdc_rx_mutex == NULL || cdc_rx_sem == NULL) {
        return false;
    }

    xSemaphoreTake(cdc_rx_mutex, portMAX_DELAY);

    for (size_t i = 0; i < len; i++) {
        size_t next_head = (cdc_rx_head + 1) % CDC_RX_BUF_SIZE;

        if (next_head == cdc_rx_tail) {
            cdc_rx_tail = (cdc_rx_tail + 1) % CDC_RX_BUF_SIZE;
        }

        cdc_rx_buf[cdc_rx_head] = data[i];
        cdc_rx_head = next_head;
        xSemaphoreGive(cdc_rx_sem);
    }

    xSemaphoreGive(cdc_rx_mutex);

    return true;
}

static bool cdc_rx_pop(uint8_t *value) {
    bool has_data = false;

    xSemaphoreTake(cdc_rx_mutex, portMAX_DELAY);

    if (cdc_rx_tail != cdc_rx_head) {
        *value = cdc_rx_buf[cdc_rx_tail];
        cdc_rx_tail = (cdc_rx_tail + 1) % CDC_RX_BUF_SIZE;
        has_data = true;
    }

    xSemaphoreGive(cdc_rx_mutex);

    return has_data;
}

static esp_err_t cdc_read_exact(uint8_t *data, size_t len, uint32_t timeout_ms) {
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    for (size_t i = 0; i < len; i++) {
        while (!cdc_rx_pop(&data[i])) {
            TickType_t now = xTaskGetTickCount();

            if ((now - start) >= timeout_ticks) {
                return ESP_ERR_TIMEOUT;
            }

            TickType_t remaining = timeout_ticks - (now - start);

            if (xSemaphoreTake(cdc_rx_sem, remaining) != pdTRUE) {
                return ESP_ERR_TIMEOUT;
            }
        }
    }

    return ESP_OK;
}

static esp_err_t stk_expect_ok(uint32_t timeout_ms) {
    uint8_t response[2];

    esp_err_t result = cdc_read_exact(response, sizeof(response), timeout_ms);

    if (result != ESP_OK) {
        return result;
    }

    if (response[0] != STK_INSYNC || response[1] != STK_OK) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t stk_tx_expect_ok(cdc_acm_dev_hdl_t cdc_handle, const uint8_t *data, size_t len, uint32_t timeout_ms) {
    cdc_rx_clear();

    esp_err_t result = cdc_acm_host_data_tx_blocking(cdc_handle, data, len, timeout_ms);

    if (result != ESP_OK) {
        return result;
    }

    return stk_expect_ok(timeout_ms);
}

static esp_err_t stk_sync(cdc_acm_dev_hdl_t cdc_handle) {
    const uint8_t sync_cmd[] = {
        STK_GET_SYNC,
        CRC_EOP,
    };

    for (int attempt = 0; attempt < 25; attempt++) {
        esp_err_t result = stk_tx_expect_ok(cdc_handle, sync_cmd, sizeof(sync_cmd), 150);

        if (result == ESP_OK) {
            return ESP_OK;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    return ESP_ERR_TIMEOUT;
}

static esp_err_t stk_load_address(cdc_acm_dev_hdl_t cdc_handle, uint32_t byte_addr) {
    uint16_t word_addr = (uint16_t) (byte_addr / 2);

    uint8_t cmd[] = {
        STK_LOAD_ADDR,
        (uint8_t) (word_addr & 0xFF),
        (uint8_t) ((word_addr >> 8) & 0xFF),
        CRC_EOP,
    };

    return stk_tx_expect_ok(cdc_handle, cmd, sizeof(cmd), 500);
}

static esp_err_t stk_program_page(cdc_acm_dev_hdl_t cdc_handle, const uint8_t *page, size_t page_len) {
    uint8_t cmd[UNO_PAGE_SIZE + 5];

    if (page_len > UNO_PAGE_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    cmd[0] = STK_PROG_PAGE;
    cmd[1] = (uint8_t) ((page_len >> 8) & 0xFF);
    cmd[2] = (uint8_t) (page_len & 0xFF);
    cmd[3] = 'F';

    memcpy(&cmd[4], page, page_len);

    cmd[4 + page_len] = CRC_EOP;

    return stk_tx_expect_ok(cdc_handle, cmd, page_len + 5, 1000);
}

static esp_err_t stk_read_page(cdc_acm_dev_hdl_t cdc_handle, uint8_t *page, size_t page_len) {
    uint8_t cmd[] = {
        STK_READ_PAGE,
        (uint8_t) ((page_len >> 8) & 0xFF),
        (uint8_t) (page_len & 0xFF),
        'F',
        CRC_EOP,
    };

    cdc_rx_clear();

    esp_err_t result = cdc_acm_host_data_tx_blocking(cdc_handle, cmd, sizeof(cmd), 500);

    if (result != ESP_OK) {
        return result;
    }

    uint8_t insync = 0;
    result = cdc_read_exact(&insync, 1, 1000);

    if (result != ESP_OK) {
        return result;
    }

    if (insync != STK_INSYNC) {
        return ESP_FAIL;
    }

    result = cdc_read_exact(page, page_len, 1000);

    if (result != ESP_OK) {
        return result;
    }

    uint8_t ok = 0;
    result = cdc_read_exact(&ok, 1, 1000);

    if (result != ESP_OK) {
        return result;
    }

    if (ok != STK_OK) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void stk_leave_programming(cdc_acm_dev_hdl_t cdc_handle) {
    const uint8_t leave_cmd[] = {
        STK_LEAVE,
        CRC_EOP,
    };

    (void) stk_tx_expect_ok(cdc_handle, leave_cmd, sizeof(leave_cmd), 500);
}

static esp_err_t optiboot_upload(cdc_acm_dev_hdl_t cdc_handle, const flash_image_t *flash, bool verify) {
    esp_err_t result = stk_sync(cdc_handle);

    if (result != ESP_OK) {
        return result;
    }

    if (flash->highest_addr == 0) {
        stk_leave_programming(cdc_handle);
        return ESP_OK;
    }

    uint32_t total = flash->highest_addr;

    for (uint32_t addr = 0; addr < total; addr += UNO_PAGE_SIZE) {
        size_t page_len = UNO_PAGE_SIZE;

        if (addr + page_len > total) {
            page_len = total - addr;
        }

        result = stk_load_address(cdc_handle, addr);

        if (result != ESP_OK) {
            return result;
        }

        result = stk_program_page(cdc_handle, &flash->image[addr], page_len);

        if (result != ESP_OK) {
            return result;
        }

        int progress = 15 + (int) ((addr + page_len) * 70 / total);

        if (progress > 85) {
            progress = 85;
        }

        upload_set_state("uploading", progress, "writing flash", NULL);
    }

    if (verify) {
        uint8_t verify_page[UNO_PAGE_SIZE];

        for (uint32_t addr = 0; addr < total; addr += UNO_PAGE_SIZE) {
            size_t page_len = UNO_PAGE_SIZE;

            if (addr + page_len > total) {
                page_len = total - addr;
            }

            result = stk_load_address(cdc_handle, addr);

            if (result != ESP_OK) {
                return result;
            }

            result = stk_read_page(cdc_handle, verify_page, page_len);

            if (result != ESP_OK) {
                return result;
            }

            if (memcmp(verify_page, &flash->image[addr], page_len) != 0) {
                return ESP_ERR_INVALID_CRC;
            }

            int progress = 86 + (int) ((addr + page_len) * 13 / total);

            if (progress > 99) {
                progress = 99;
            }

            upload_set_state("verifying", progress, "verifying flash", NULL);
        }
    }

    stk_leave_programming(cdc_handle);

    return ESP_OK;
}

static void usb_host_lib_task(void *arg) {
    (void) arg;

    while (1) {
        uint32_t event_flags = 0;

        esp_err_t result = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

        if (result != ESP_OK) {
            ESP_LOGE(TAG, "USB host event handling failed: %s", esp_err_to_name(result));
            continue;
        }

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static void usb_cdc_event_callback(const cdc_acm_host_dev_event_data_t *event, void *user_ctx) {
    (void) user_ctx;

    if (event == NULL) {
        return;
    }

    if (event->type == CDC_ACM_HOST_DEVICE_DISCONNECTED) {
        target_present = false;
        bench_log("USB CDC device disconnected");
    } else if (event->type == CDC_ACM_HOST_ERROR) {
        bench_log("USB CDC device error: %d", event->data.error);
    }
}

static bool usb_cdc_rx_callback(const uint8_t *data, size_t data_len, void *user_arg) {
    (void) user_arg;

    return cdc_rx_push(data, data_len);
}

static esp_err_t bench_usb_init(void) {
    usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    esp_err_t result = usb_host_install(&host_config);

    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "USB host install failed: %s", esp_err_to_name(result));
        return result;
    }

    if (usb_host_task_handle == NULL) {
        BaseType_t task_result = xTaskCreate(
            usb_host_lib_task,
            "usb_host",
            4096,
            NULL,
            10,
            &usb_host_task_handle
        );

        if (task_result != pdPASS) {
            ESP_LOGE(TAG, "Failed to create USB host task");
            return ESP_ERR_NO_MEM;
        }
    }

    cdc_acm_host_driver_config_t cdc_driver_config = {
        .driver_task_stack_size = 4096,
        .driver_task_priority = 8,
        .xCoreID = tskNO_AFFINITY,
        .new_dev_cb = NULL,
    };

    result = cdc_acm_host_install(&cdc_driver_config);

    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "CDC ACM host install failed: %s", esp_err_to_name(result));
        return result;
    }

    usb_host_ready = true;
    bench_log("USB host CDC service initialized");

    return ESP_OK;
}

static esp_err_t bench_usb_open_uno(cdc_acm_dev_hdl_t *cdc_handle, uint32_t timeout_ms) {
    if (!usb_host_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    cdc_acm_host_device_config_t dev_config = {
        .connection_timeout_ms = timeout_ms,
        .out_buffer_size = 256,
        .in_buffer_size = 256,
        .event_cb = usb_cdc_event_callback,
        .data_cb = usb_cdc_rx_callback,
        .user_arg = NULL,
    };

    return cdc_acm_host_open(
        CDC_HOST_ANY_VID,
        CDC_HOST_ANY_PID,
        CDC_HOST_ANY_DEV_ADDR,
        &dev_config,
        cdc_handle
    );
}

static bool bench_usb_probe_target(void) {
    cdc_acm_dev_hdl_t cdc_handle = NULL;

    esp_err_t result = bench_usb_open_uno(&cdc_handle, 100);

    if (result == ESP_OK) {
        target_present = true;
        cdc_acm_host_close(cdc_handle);
        return true;
    }

    target_present = false;
    return false;
}

static esp_err_t bench_usb_prepare_uno(cdc_acm_dev_hdl_t *cdc_handle, bool reset) {
    esp_err_t result = bench_usb_open_uno(cdc_handle, 3000);

    if (result != ESP_OK) {
        target_present = false;
        return result;
    }

    target_present = true;

    cdc_acm_line_coding_t line_coding = {
        .dwDTERate = 115200,
        .bCharFormat = 0,
        .bParityType = 0,
        .bDataBits = 8,
    };

    result = cdc_acm_host_line_coding_set(*cdc_handle, &line_coding);

    if (result != ESP_OK) {
        cdc_acm_host_close(*cdc_handle);
        *cdc_handle = NULL;
        return result;
    }

    cdc_rx_clear();

    if (reset) {
        cdc_acm_host_set_control_line_state(*cdc_handle, false, false);
        vTaskDelay(pdMS_TO_TICKS(100));
        cdc_acm_host_set_control_line_state(*cdc_handle, true, true);
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    return ESP_OK;
}

static void discard_request_body(httpd_req_t *req) {
    char buffer[128];
    int remaining = req->content_len;

    while (remaining > 0) {
        int to_read = remaining < (int) sizeof(buffer) ? remaining : (int) sizeof(buffer);
        int received = httpd_req_recv(req, buffer, to_read);

        if (received <= 0) {
            return;
        }

        remaining -= received;
    }
}

static esp_err_t status_handler(httpd_req_t *req) {
    bench_gpio_read_power_fault();
    
    if (!upload_running) {
        if (target_power_enabled && !target_power_fault) {
            bench_usb_probe_target();
        } else {
            target_present = false;
        }
    }

    bench_gpio_update_status_leds();

    char state[32];
    char message[96];
    char job_id[32];
    int progress = 0;
    bool running = false;

    xSemaphoreTake(upload_mutex, portMAX_DELAY);

    running = upload_running;
    strncpy(state, upload_state, sizeof(state) - 1);
    state[sizeof(state) - 1] = '\0';
    strncpy(message, upload_message, sizeof(message) - 1);
    message[sizeof(message) - 1] = '\0';
    strncpy(job_id, upload_job_id, sizeof(job_id) - 1);
    job_id[sizeof(job_id) - 1] = '\0';
    progress = upload_progress;

    xSemaphoreGive(upload_mutex);

    char json[720];

    snprintf(
        json,
        sizeof(json),
        "{"
            "\"device\":\"bench\","
            "\"version\":\"0.1.0\","
            "\"mode\":\"firmware\","
            "\"state\":\"%s\","
            "\"target\":{"
                "\"present\":%s,"
                "\"name\":%s"
            "},"
            "\"power\":{"
                "\"enabled\":%s,"
                "\"fault\":%s"
            "},"
            "\"upload\":{"
                "\"active\":%s,"
                "\"job_id\":%s,"
                "\"progress\":%d,"
                "\"message\":\"%s\""
            "}"
        "}",
        running ? state : "idle",
        target_present ? "true" : "false",
        target_present ? "\"Arduino Uno R3\"" : "null",
        target_power_enabled ? "true" : "false",
        target_power_fault ? "true" : "false",
        running ? "true" : "false",
        running ? job_id : "null",
        running ? progress : 0,
        running ? message : "idle"
    );

    return send_json(req, json);
}

static esp_err_t target_power_handler(httpd_req_t *req) {
    char body[128] = {0};

    int received = httpd_req_recv(req, body, sizeof(body) - 1);

    if (received <= 0) {
        return send_json_status(
            req,
            "400 Bad Request",
            "{\"error\":\"BAD_REQUEST\",\"message\":\"missing request body\"}"
        );
    }

    body[received] = '\0';

    if (strstr(body, "\"enabled\":true") != NULL ||
        strstr(body, "\"enabled\": true") != NULL) {
        bench_gpio_set_target_power(true);
        bench_log("target power enabled");
    } else if (strstr(body, "\"enabled\":false") != NULL ||
               strstr(body, "\"enabled\": false") != NULL) {
        bench_gpio_set_target_power(false);
        bench_log("target power disabled");
    } else {
        return send_json_status(
            req,
            "400 Bad Request",
            "{\"error\":\"BAD_REQUEST\",\"message\":\"expected enabled boolean\"}"
        );
    }

    bench_gpio_read_power_fault();
    bench_gpio_update_status_leds();

    char json[128];

    snprintf(
        json,
        sizeof(json),
        "{"
            "\"enabled\":%s,"
            "\"fault\":%s"
        "}",
        target_power_enabled ? "true" : "false",
        target_power_fault ? "true" : "false"
    );

    return send_json(req, json);
}

static esp_err_t target_reset_handler(httpd_req_t *req) {
    discard_request_body(req);

    if (!target_power_enabled) {
        return send_json_status(req, "400 Bad Request", "{\"error\":\"POWER_DISABLED\"}");
    }

    if (target_power_fault) {
        return send_json_status(req, "400 Bad Request", "{\"error\":\"POWER_FAULT\"}");
    }

    cdc_acm_dev_hdl_t cdc_handle = NULL;

    esp_err_t result = bench_usb_prepare_uno(&cdc_handle, true);

    if (result != ESP_OK) {
        bench_log("target reset failed: %s", esp_err_to_name(result));
        return send_json_status(req, "400 Bad Request", "{\"error\":\"NO_TARGET\"}");
    }

    cdc_acm_host_close(cdc_handle);

    bench_log("target reset requested through USB CDC control lines");

    return send_json(req, "{\"ok\":true,\"message\":\"target reset requested\"}");
}

static void upload_task(void *arg) {
    upload_task_args_t *task_args = (upload_task_args_t *) arg;
    flash_image_t *flash = malloc(sizeof(flash_image_t));

    if (flash == NULL) {
        upload_set_state("failed", 0, "out of memory", "UPLOAD_FAILED");
        goto done;
    }

    upload_set_state("receiving", 0, "parsing Intel HEX", NULL);

    esp_err_t result = parse_intel_hex(task_args->hex_text, task_args->hex_len, flash);

    if (result != ESP_OK) {
        bench_log("invalid HEX file: %s", esp_err_to_name(result));
        upload_set_state("failed", 0, "invalid HEX file", "INVALID_HEX");
        goto done;
    }

    if (!target_power_enabled) {
        upload_set_state("failed", 0, "target power is disabled", "POWER_DISABLED");
        goto done;
    }

    if (target_power_fault) {
        upload_set_state("fault", 0, "target power fault", "POWER_FAULT");
        goto done;
    }

    upload_set_state("opening_target", 5, "opening USB serial target", NULL);

    cdc_acm_dev_hdl_t cdc_handle = NULL;

    result = bench_usb_prepare_uno(&cdc_handle, task_args->reset);

    if (result != ESP_OK) {
        bench_log("USB open failed: %s", esp_err_to_name(result));
        upload_set_state("failed", 5, "USB target open failed", "USB_OPEN_FAILED");
        goto done;
    }

    upload_set_state("bootloader_sync", 10, "syncing with Optiboot", NULL);

    result = optiboot_upload(cdc_handle, flash, task_args->verify);

    cdc_acm_host_close(cdc_handle);

    if (result == ESP_ERR_TIMEOUT) {
        bench_log("bootloader sync/upload timeout");
        upload_set_state("failed", upload_progress, "bootloader timeout", "BOOTLOADER_TIMEOUT");
        goto done;
    }

    if (result == ESP_ERR_INVALID_CRC) {
        bench_log("verify failed");
        upload_set_state("failed", upload_progress, "verify failed", "VERIFY_FAILED");
        goto done;
    }

    if (result != ESP_OK) {
        bench_log("upload failed: %s", esp_err_to_name(result));
        upload_set_state("failed", upload_progress, "upload failed", "UPLOAD_FAILED");
        goto done;
    }

    upload_set_state("success", 100, "upload complete", NULL);
    bench_log("%s: upload complete", upload_job_id);

done:
    if (flash != NULL) {
        free(flash);
    }

    if (task_args != NULL) {
        if (task_args->hex_text != NULL) {
            free(task_args->hex_text);
        }

        free(task_args);
    }

    xSemaphoreTake(upload_mutex, portMAX_DELAY);
    upload_running = false;
    xSemaphoreGive(upload_mutex);

    vTaskDelete(NULL);
}

static esp_err_t upload_handler(httpd_req_t *req) {
    xSemaphoreTake(upload_mutex, portMAX_DELAY);

    if (upload_running) {
        xSemaphoreGive(upload_mutex);

        discard_request_body(req);

        return send_json_status(
            req,
            "409 Conflict",
            "{\"error\":\"UPLOAD_ACTIVE\",\"message\":\"another upload is already running\"}"
        );
    }

    xSemaphoreGive(upload_mutex);

    if (req->content_len <= 0 || req->content_len > UPLOAD_BODY_MAX_BYTES) {
        discard_request_body(req);

        return send_json_status(
            req,
            "400 Bad Request",
            "{\"error\":\"BAD_REQUEST\",\"message\":\"invalid upload size\"}"
        );
    }

    char *body = malloc(req->content_len + 1);

    if (body == NULL) {
        discard_request_body(req);

        return send_json_status(
            req,
            "500 Internal Server Error",
            "{\"error\":\"UPLOAD_FAILED\",\"message\":\"out of memory\"}"
        );
    }

    int total_received = 0;

    while (total_received < req->content_len) {
        int remaining = req->content_len - total_received;
        int received = httpd_req_recv(req, body + total_received, remaining);

        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }

        if (received <= 0) {
            free(body);

            return send_json_status(
                req,
                "400 Bad Request",
                "{\"error\":\"BAD_REQUEST\",\"message\":\"failed to receive upload body\"}"
            );
        }

        total_received += received;
    }

    body[total_received] = '\0';

    char content_type[160] = {0};
    size_t content_type_len = httpd_req_get_hdr_value_len(req, "Content-Type");

    if (content_type_len > 0 && content_type_len < sizeof(content_type)) {
        httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type));
    }

    upload_task_args_t *task_args = calloc(1, sizeof(upload_task_args_t));

    if (task_args == NULL) {
        free(body);

        return send_json_status(
            req,
            "500 Internal Server Error",
            "{\"error\":\"UPLOAD_FAILED\",\"message\":\"out of memory\"}"
        );
    }

    esp_err_t result = extract_upload_parts(body, total_received, content_type, task_args);

    free(body);

    if (result != ESP_OK) {
        free(task_args);

        return send_json_status(
            req,
            "400 Bad Request",
            "{\"error\":\"BAD_REQUEST\",\"message\":\"could not find HEX file in upload\"}"
        );
    }

    xSemaphoreTake(upload_mutex, portMAX_DELAY);

    upload_job_counter++;

    snprintf(
        upload_job_id,
        sizeof(upload_job_id),
        "upload-fw-%04d",
        upload_job_counter
    );

    upload_running = true;
    upload_progress = 0;
    strncpy(upload_state, "receiving", sizeof(upload_state) - 1);
    strncpy(upload_message, "upload accepted", sizeof(upload_message) - 1);
    upload_error[0] = '\0';

    xSemaphoreGive(upload_mutex);

    BaseType_t task_result = xTaskCreate(
        upload_task,
        "bench_upload",
        8192,
        task_args,
        6,
        NULL
    );

    if (task_result != pdPASS) {
        xSemaphoreTake(upload_mutex, portMAX_DELAY);
        upload_running = false;
        xSemaphoreGive(upload_mutex);

        if (task_args->hex_text != NULL) {
            free(task_args->hex_text);
        }

        free(task_args);

        return send_json_status(
            req,
            "500 Internal Server Error",
            "{\"error\":\"UPLOAD_FAILED\",\"message\":\"failed to start upload task\"}"
        );
    }

    bench_log("%s: accepted upload body (%d bytes)", upload_job_id, total_received);

    char json[160];

    snprintf(
        json,
        sizeof(json),
        "{"
            "\"job_id\":\"%s\","
            "\"state\":\"receiving\","
            "\"message\":\"upload accepted\""
        "}",
        upload_job_id
    );

    return send_json(req, json);
}

static esp_err_t upload_status_handler(httpd_req_t *req) {
    const char *prefix = "/api/v1/upload/";
    const char *requested_job_id = req->uri + strlen(prefix);

    char job_id[32];
    char state[32];
    char message[96];
    char error[40];
    int progress = 0;

    xSemaphoreTake(upload_mutex, portMAX_DELAY);

    strncpy(job_id, upload_job_id, sizeof(job_id) - 1);
    job_id[sizeof(job_id) - 1] = '\0';

    strncpy(state, upload_state, sizeof(state) - 1);
    state[sizeof(state) - 1] = '\0';

    strncpy(message, upload_message, sizeof(message) - 1);
    message[sizeof(message) - 1] = '\0';

    strncpy(error, upload_error, sizeof(error) - 1);
    error[sizeof(error) - 1] = '\0';

    progress = upload_progress;

    xSemaphoreGive(upload_mutex);

    if (requested_job_id[0] == '\0' ||
        job_id[0] == '\0' ||
        strcmp(requested_job_id, job_id) != 0) {
        return send_json_status(req, "404 Not Found", "{\"detail\":\"job not found\"}");
    }

    char json[320];

    if (error[0] != '\0') {
        snprintf(
            json,
            sizeof(json),
            "{"
                "\"job_id\":\"%s\","
                "\"state\":\"%s\","
                "\"progress\":%d,"
                "\"message\":\"%s\","
                "\"error\":\"%s\""
            "}",
            job_id,
            state,
            progress,
            message,
            error
        );
    } else {
        snprintf(
            json,
            sizeof(json),
            "{"
                "\"job_id\":\"%s\","
                "\"state\":\"%s\","
                "\"progress\":%d,"
                "\"message\":\"%s\","
                "\"error\":null"
            "}",
            job_id,
            state,
            progress,
            message
        );
    }

    return send_json(req, json);
}

static esp_err_t logs_handler(httpd_req_t *req) {
    char json[1024];
    int offset = 0;
    bool first = true;

    offset += snprintf(json + offset, sizeof(json) - offset, "{\"lines\":[");

    for (int i = 0; i < LOG_LINE_COUNT; i++) {
        int index = (log_line_index + i) % LOG_LINE_COUNT;

        if (log_lines[index][0] == '\0') {
            continue;
        }

        if (!first) {
            offset += snprintf(json + offset, sizeof(json) - offset, ",");
        }

        offset += snprintf(
            json + offset,
            sizeof(json) - offset,
            "\"%s\"",
            log_lines[index]
        );

        first = false;
    }

    snprintf(json + offset, sizeof(json) - offset, "]}");

    return send_json(req, json);
}

static httpd_handle_t start_http_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;

    esp_err_t result = httpd_start(&server, &config);

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(result));
        return NULL;
    }

    httpd_uri_t status_uri = {
        .uri = "/api/v1/status",
        .method = HTTP_GET,
        .handler = status_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t target_power_uri = {
        .uri = "/api/v1/target/power",
        .method = HTTP_POST,
        .handler = target_power_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t target_reset_uri = {
        .uri = "/api/v1/target/reset",
        .method = HTTP_POST,
        .handler = target_reset_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t upload_uri = {
        .uri = "/api/v1/upload",
        .method = HTTP_POST,
        .handler = upload_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t upload_status_uri = {
        .uri = "/api/v1/upload/*",
        .method = HTTP_GET,
        .handler = upload_status_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t logs_uri = {
        .uri = "/api/v1/logs",
        .method = HTTP_GET,
        .handler = logs_handler,
        .user_ctx = NULL,
    };

    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &target_power_uri);
    httpd_register_uri_handler(server, &target_reset_uri);
    httpd_register_uri_handler(server, &upload_uri);
    httpd_register_uri_handler(server, &upload_status_uri);
    httpd_register_uri_handler(server, &logs_uri);

    bench_log("HTTP server started");
    bench_log("registered GET /api/v1/status");
    bench_log("registered POST /api/v1/target/power");
    bench_log("registered POST /api/v1/target/reset");
    bench_log("registered POST /api/v1/upload");
    bench_log("registered GET /api/v1/upload/{job_id}");
    bench_log("registered GET /api/v1/logs");

    return server;
}

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
) {
    (void) arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        bench_log("Wi-Fi started, connecting");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (wifi_retry_count < WIFI_MAX_RETRIES) {
            wifi_retry_count++;
            ESP_LOGW(TAG, "Wi-Fi disconnected, retrying... attempt %d", wifi_retry_count);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "Wi-Fi connection failed");
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;

        ESP_LOGI(
            TAG,
            "Wi-Fi connected. IP address: " IPSTR,
            IP2STR(&event->ip_info.ip)
        );

        wifi_retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void) {
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_config));

    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &wifi_event_handler,
            NULL,
            NULL
        )
    );

    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &wifi_event_handler,
            NULL,
            NULL
        )
    );

    wifi_config_t wifi_config = {0};

    strncpy(
        (char *) wifi_config.sta.ssid,
        WIFI_SSID,
        sizeof(wifi_config.sta.ssid)
    );

    strncpy(
        (char *) wifi_config.sta.password,
        WIFI_PASS,
        sizeof(wifi_config.sta.password)
    );

    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    bench_log("Wi-Fi init complete");

    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY
    );

    if (bits & WIFI_CONNECTED_BIT) {
        bench_log("connected to Wi-Fi network");
    } else if (bits & WIFI_FAIL_BIT) {
        bench_log("failed to connect to Wi-Fi network");
    } else {
        bench_log("unexpected Wi-Fi event");
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Bench firmware booting");
    ESP_LOGI(TAG, "Target MCU: ESP32-S3");

    upload_mutex = xSemaphoreCreateMutex();
    cdc_rx_mutex = xSemaphoreCreateMutex();
    cdc_rx_sem = xSemaphoreCreateCounting(CDC_RX_BUF_SIZE, 0);

    if (upload_mutex == NULL || cdc_rx_mutex == NULL || cdc_rx_sem == NULL) {
        ESP_LOGE(TAG, "Failed to create synchronization primitives");
        abort();
    }

    esp_err_t nvs_result = nvs_flash_init();

    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_result = nvs_flash_init();
    }

    ESP_ERROR_CHECK(nvs_result);

    bench_log("Bench firmware booted");

    ESP_ERROR_CHECK(bench_gpio_init());
    ESP_ERROR_CHECK(bench_usb_init());

    wifi_init_sta();

    start_http_server();

    while (1) {
        bench_log(
            "alive: target_present=%d power_enabled=%d power_fault=%d upload_running=%d",
            target_present,
            target_power_enabled,
            target_power_fault,
            upload_running
        );

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
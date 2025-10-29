// main/ex3_error_handling.c
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"

static const char *TAG = "EX3_ERROR";

static void error_handling_demo(void)
{
    ESP_LOGI(TAG, "=== Error Handling Demo ===");

    // 1) Success case
    esp_err_t result = ESP_OK;
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "Operation completed successfully");
    }

    // 2) Fatal check (ตัวอย่าง: ถ้าอยากหยุดโปรแกรมจริง ให้ใช้ ESP_ERROR_CHECK ด้วยค่าที่ไม่ใช่ ESP_OK)
    // หมายเหตุ: ถ้า uncomment บรรทัดล่าง โปรแกรมจะ abort ทันที (ตั้งใจเดโม)
    // ESP_ERROR_CHECK(ESP_ERR_INVALID_STATE);

    // 3) Non-fatal error (ใช้ WITHOUT_ABORT)
    result = ESP_ERR_INVALID_ARG;
    ESP_ERROR_CHECK_WITHOUT_ABORT(result);  // จะ log error แต่ไม่ abort
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Non-fatal error: %s", esp_err_to_name(result));
    }

    // 4) ตัวอย่างการแปลงรหัส error เป็นข้อความ
    result = ESP_ERR_NO_MEM;
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Error: %s", esp_err_to_name(result));
    }

    // 5) ตัวอย่างจริง: init NVS แบบตรวจ error
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase, trying...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized successfully");
}

void app_main(void)
{
    // เรียกเดโมครั้งเดียวพอ (หรือจะวน loop ก็ได้)
    error_handling_demo();

    // วนเบา ๆ กัน watchdog
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
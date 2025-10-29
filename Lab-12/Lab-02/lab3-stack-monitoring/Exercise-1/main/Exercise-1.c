// ex1_test_stack_sizes.c  (Exercise-1.c)
#include <stdio.h>
#include <string.h>
#include <inttypes.h>               // <— เพิ่มไฟล์นี้
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "EX1_STACK_SIZES";

static void heavy_stack_task_once(void *pvParameters)
{
    (void)pvParameters;
    char bufA[600];
    int  nums[150];       // ~600 bytes (มีใช้งานให้คอมไพเลอร์ไม่เตือน)
    char bufB[400];

    memset(bufA, 'X', sizeof(bufA));
    memset(bufB, 'Y', sizeof(bufB));
    for (int i = 0; i < 150; ++i) nums[i] = i * 3;   // ใช้งานจริง

    UBaseType_t rem_words = uxTaskGetStackHighWaterMark(NULL);
    uint32_t    rem_bytes = rem_words * sizeof(StackType_t);

    ESP_LOGI(TAG, "heavy_stack_task_once running");
    ESP_LOGI(TAG, "Stack remaining: %" PRIu32 " bytes (HWM)", rem_bytes);

    for (int r = 0; r < 3; ++r) {
        vTaskDelay(pdMS_TO_TICKS(500));
        rem_words = uxTaskGetStackHighWaterMark(NULL);
        rem_bytes = rem_words * sizeof(StackType_t);
        ESP_LOGI(TAG, "[round %d] HWM: %" PRIu32 " bytes", r+1, rem_bytes);
    }

    ESP_LOGI(TAG, "Task done, deleting self");
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Exercise 1: Test different stack sizes sequentially ===");

    const uint32_t sizes_bytes[] = {512, 1024, 2048, 4096};

    for (int i = 0; i < (int)(sizeof(sizes_bytes)/sizeof(sizes_bytes[0])); ++i) {
        char name[32];
        // แก้ฟอร์แมตให้ถูกต้อง
        snprintf(name, sizeof(name), "Heavy_%" PRIu32 "B", (uint32_t)sizes_bytes[i]);

        ESP_LOGI(TAG, "\n--- Creating task with stack = %" PRIu32 " bytes ---", sizes_bytes[i]);
        TaskHandle_t h = NULL;
        BaseType_t ok = xTaskCreate(
            heavy_stack_task_once,
            name,
            sizes_bytes[i],   // (คงสไตล์เดิมของโปรเจกต์คุณที่คิดหน่วยเป็น bytes)
            NULL,
            3,
            &h
        );

        if (ok != pdPASS || h == NULL) {
            ESP_LOGE(TAG, "Create FAIL for %s", name);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        } else {
            ESP_LOGI(TAG, "Create PASS for %s", name);
        }

        vTaskDelay(pdMS_TO_TICKS(2200));  // รอให้ task จบ
        ESP_LOGI(TAG, "--- Done for %s ---\n", name);
        vTaskDelay(pdMS_TO_TICKS(400));
    }

    ESP_LOGI(TAG, "All sizes tested. Exercise 1 complete.");
}
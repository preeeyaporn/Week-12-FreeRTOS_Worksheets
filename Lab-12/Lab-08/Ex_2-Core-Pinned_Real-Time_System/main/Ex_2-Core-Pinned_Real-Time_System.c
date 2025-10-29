#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "REALTIME_EX2";

// ─── CONTROL TASK (1kHz) ─────────────────────────────────────────────
void control_task(void *pv)
{
    const TickType_t period = pdMS_TO_TICKS(1); // 1 ms
    TickType_t last = xTaskGetTickCount();
    int64_t prev = esp_timer_get_time();
    int loop = 0;

    while (1)
    {
        int64_t now = esp_timer_get_time();
        int64_t diff = now - prev;
        prev = now;

        // log ทุก 100 รอบเพื่อลดภาระ UART
        if (++loop % 100 == 0)
            ESP_LOGI(TAG, "Control loop (1kHz) Core %d | Period: %lld µs",
                     xPortGetCoreID(), diff);

        vTaskDelayUntil(&last, period);
    }
}

// ─── DATA ACQUISITION TASK (500Hz) ───────────────────────────────────
void data_acq_task(void *pv)
{
    const TickType_t period = pdMS_TO_TICKS(2); // 2 ms
    TickType_t last = xTaskGetTickCount();
    int loop = 0;

    while (1)
    {
        // จำลองอ่านค่าเซนเซอร์
        if (++loop % 250 == 0)
            ESP_LOGI(TAG, "Data acquisition (500Hz) Core %d",
                     xPortGetCoreID());
        vTaskDelayUntil(&last, period);
    }
}

// ─── COMMUNICATION TASK (CORE 1) ─────────────────────────────────────
void comm_task(void *pv)
{
    while (1)
    {
        ESP_LOGI(TAG, "Communication task running on Core %d",
                 xPortGetCoreID());
        vTaskDelay(pdMS_TO_TICKS(200)); // 5 Hz
    }
}

// ─── BACKGROUND TASK (NO AFFINITY) ───────────────────────────────────
void background_task(void *pv)
{
    while (1)
    {
        ESP_LOGI(TAG, "Background processing task (no affinity) on Core %d",
                 xPortGetCoreID());
        vTaskDelay(pdMS_TO_TICKS(1000)); // 1 Hz
    }
}

// ─── APP_MAIN ────────────────────────────────────────────────────────
void app_main(void)
{
    ESP_LOGI(TAG, "Starting Exercise 2: Core-Pinned Real-Time System (Full Version)");

    // Core 0 → Real-time tasks
    xTaskCreatePinnedToCore(control_task, "ControlTask", 4096, NULL, 10, NULL, 0);
    xTaskCreatePinnedToCore(data_acq_task, "DataAcqTask", 4096, NULL, 9, NULL, 0);

    // Core 1 → Communication
    xTaskCreatePinnedToCore(comm_task, "CommTask", 4096, NULL, 7, NULL, 1);

    // Background task → ไม่ fix core
    xTaskCreate(background_task, "BackgroundTask", 4096, NULL, 5, NULL);
}
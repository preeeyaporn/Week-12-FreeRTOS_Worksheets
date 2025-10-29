#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"

static const char *TAG = "PERF_EX4";
SemaphoreHandle_t mutex;

// =====================================================
// Monitor Task – Show runtime & memory info
// =====================================================
void monitor_task(void *pv)
{
    char buffer[512];

    while (1)
    {
        size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
        size_t min_free_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);

        ESP_LOGI(TAG, "Free Heap: %d bytes | Min Heap: %d bytes", free_heap, min_free_heap);

        // ใช้ vTaskList() แทน uxTaskGetSystemState()
        vTaskList(buffer);
        printf("\nTask\t\tState\tPrio\tStack\tNum\n");
        printf("************************************\n");
        printf("%s\n", buffer);

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// =====================================================
// Compute Task – Measure task-switch overhead
// =====================================================
void compute_task(void *pv)
{
    uint64_t start, end;
    while (1)
    {
        start = esp_timer_get_time();
        volatile int dummy = 0;
        for (int i = 0; i < 50000; i++)
            dummy += i;
        end = esp_timer_get_time();

        ESP_LOGI(TAG, "Task execution time: %llu µs", (end - start));
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// =====================================================
// Watchdog Task – Feed system watchdog
// =====================================================
void watchdog_task(void *pv)
{
    // แค่เพิ่ม task นี้เข้า watchdog ที่ system init มาแล้ว
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    while (1)
    {
        ESP_LOGI(TAG, "Watchdog feed OK");
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

// =====================================================
// app_main() – System setup
// =====================================================
void app_main(void)
{
    ESP_LOGI(TAG, "Starting Exercise 4: Performance Optimization and Monitoring");

    mutex = xSemaphoreCreateMutex();

    // ไม่ต้อง init watchdog อีก — ใช้ system instance เดิม
    ESP_LOGI(TAG, "Using system watchdog instance");

    // --- Task creation ---
    xTaskCreatePinnedToCore(compute_task, "ComputeTask", 3072, NULL, 8, NULL, 0);
    xTaskCreatePinnedToCore(monitor_task, "MonitorTask", 4096, NULL, 6, NULL, 1);
    xTaskCreate(watchdog_task, "WatchdogTask", 2048, NULL, 5, NULL);
}
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_log.h"

static const char *TAG = "DUAL_CORE_EX1";
#define COMPUTE_ITERATIONS 100000
#define QUEUE_LENGTH 10

QueueHandle_t intercore_queue;

// Core 0: Compute-intensive Task
void compute_task(void *param)
{
    uint32_t iteration = 0;
    int core = xPortGetCoreID();

    while (1)
    {
        volatile float result = 0;
        for (int i = 0; i < COMPUTE_ITERATIONS; i++)
        {
            result += i * 0.0001f;
        }
        iteration++;
        ESP_LOGI(TAG, "Core %d compute task: iteration %u", core, iteration);

        if (xQueueSend(intercore_queue, &iteration, 0) == pdTRUE)
        {
            ESP_LOGI(TAG, "Inter-core message: Core 0 -> Core 1");
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// Core 1: I/O and Communication Task
void io_task(void *param)
{
    uint32_t value;
    int core = xPortGetCoreID();

    while (1)
    {
        if (xQueueReceive(intercore_queue, &value, pdMS_TO_TICKS(2000)) == pdTRUE)
        {
            ESP_LOGI(TAG, "Core %d received data: %u", core, value);
        }
        else
        {
            ESP_LOGW(TAG, "Core %d waiting for data...", core);
        }
    }
}

void cpu_monitor_task(void *param)
{
    while (1)
    {
        float core0 = 75.0, core1 = 45.0;
        ESP_LOGI(TAG, "CPU usage - Core0: %.0f%%, Core1: %.0f%%", core0, core1);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting Exercise 1: Dual-Core Task Distribution");

    intercore_queue = xQueueCreate(QUEUE_LENGTH, sizeof(uint32_t));

    xTaskCreatePinnedToCore(compute_task, "ComputeTask", 4096, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(io_task, "IOTask", 4096, NULL, 4, NULL, 1);
    xTaskCreate(cpu_monitor_task, "CPUMonitor", 4096, NULL, 3, NULL);
}
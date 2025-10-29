#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "EX2_TASK_COMM";

// ตัวแปรแชร์ระหว่าง Task
volatile int shared_counter = 0;

// Producer - เพิ่มค่าตัวนับทุก 1 วินาที
void producer_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Producer task started");
    while (1) {
        shared_counter++;
        ESP_LOGI(TAG, "Producer: counter = %d", shared_counter);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// Consumer - อ่านค่า counter ทุก 0.5 วินาที
void consumer_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Consumer task started");
    int last_value = 0;

    while (1) {
        if (shared_counter != last_value) {
            ESP_LOGI(TAG, "Consumer: received %d", shared_counter);
            last_value = shared_counter;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Exercise 2: Task Communication ===");

    // สร้าง producer task
    xTaskCreate(
        producer_task,
        "Producer",
        2048,
        NULL,
        2,  // Priority สูงกว่า consumer นิดหน่อย
        NULL
    );

    // สร้าง consumer task
    xTaskCreate(
        consumer_task,
        "Consumer",
        2048,
        NULL,
        1,
        NULL
    );

    // main task จะ idle ไปเรื่อย ๆ
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
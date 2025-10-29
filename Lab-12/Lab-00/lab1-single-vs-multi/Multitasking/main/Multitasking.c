#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define LED1_PIN GPIO_NUM_2
#define LED2_PIN GPIO_NUM_4
#define BUTTON_PIN GPIO_NUM_0

static const char *TAG = "MULTITASK";

// Task 1: Sensor Reading
void sensor_task(void *pvParameters)
{
    while (1) {
        ESP_LOGI(TAG, "Reading sensor...");
        gpio_set_level(LED1_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(LED1_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(900)); // 1 second total
    }
}

// Task 2: Data Processing
void processing_task(void *pvParameters)
{
    while (1) {
        ESP_LOGI(TAG, "Processing data...");
        for (int i = 0; i < 500000; i++) {
            volatile int dummy = i * i;
            if (i % 100000 == 0) {
                vTaskDelay(1); // Yield to other tasks
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// Task 3: Actuator Control
void actuator_task(void *pvParameters)
{
    while (1) {
        ESP_LOGI(TAG, "Controlling actuator...");
        gpio_set_level(LED2_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level(LED2_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(800)); // 1 second total
    }
}

// Task 4: Emergency Response (High Priority)
void emergency_task(void *pvParameters)
{
    while (1) {
        if (gpio_get_level(BUTTON_PIN) == 0) {
            ESP_LOGW(TAG, "EMERGENCY! Button pressed - Immediate response!");
            // Immediate response because this task has high priority
            gpio_set_level(LED1_PIN, 1);
            gpio_set_level(LED2_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(LED1_PIN, 0);
            gpio_set_level(LED2_PIN, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(10)); // Check every 10ms
    }
}

void app_main(void)
{
    // GPIO Configuration
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED1_PIN) | (1ULL << LED2_PIN),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&io_conf);

    // Button configuration
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = 1ULL << BUTTON_PIN;
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "Multitasking System Started");

    // Create tasks with different priorities
    xTaskCreate(sensor_task, "sensor", 2048, NULL, 2, NULL);
    xTaskCreate(processing_task, "processing", 2048, NULL, 1, NULL);
    xTaskCreate(actuator_task, "actuator", 2048, NULL, 2, NULL);
    xTaskCreate(emergency_task, "emergency", 2048, NULL, 5, NULL); // Highest priority
}
// Preemptive-Multitasking.c  (C ล้วน)
#include <stdio.h>
#include <string.h>              // <-- สำหรับ memset
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#define LED1_PIN GPIO_NUM_2
#define LED2_PIN GPIO_NUM_4
#define LED3_PIN GPIO_NUM_5
#define BUTTON_PIN GPIO_NUM_0

static const char *PREEMPT_TAG = "PREEMPTIVE";
static volatile bool preempt_emergency = false;
static uint64_t preempt_start_time = 0;
static uint32_t preempt_max_response = 0;

void preemptive_task1(void *pvParameters)
{
    uint32_t count = 0;
    while (1) {
        ESP_LOGI(PREEMPT_TAG, "Preempt Task1: %u", count++);

        gpio_set_level(LED1_PIN, 1);
        for (int i = 0; i < 5; i++) {
            for (int j = 0; j < 50000; j++) {
                volatile int dummy = j * 2;
                (void)dummy;
            }
        }
        gpio_set_level(LED1_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void preemptive_task2(void *pvParameters)
{
    uint32_t count = 0;
    while (1) {
        ESP_LOGI(PREEMPT_TAG, "Preempt Task2: %u", count++);

        gpio_set_level(LED2_PIN, 1);
        for (int i = 0; i < 20; i++) {
            for (int j = 0; j < 30000; j++) {
                volatile int dummy = j + i;
                (void)dummy;
            }
        }
        gpio_set_level(LED2_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

void preemptive_emergency_task(void *pvParameters)
{
    while (1) {
        if (gpio_get_level(BUTTON_PIN) == 0 && !preempt_emergency) {
            preempt_emergency = true;
            preempt_start_time = esp_timer_get_time();

            uint64_t response_time = esp_timer_get_time() - preempt_start_time;
            uint32_t response_ms = (uint32_t)(response_time / 1000);

            if (response_ms > preempt_max_response) {
                preempt_max_response = response_ms;
            }

            ESP_LOGW(PREEMPT_TAG, "IMMEDIATE EMERGENCY! Response: %u ms (Max: %u ms)",
                     response_ms, preempt_max_response);

            gpio_set_level(LED3_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(200));
            gpio_set_level(LED3_PIN, 0);

            preempt_emergency = false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static void init_gpio_c(void)
{
    gpio_config_t io;
    memset(&io, 0, sizeof(io));

    // LEDs as output
    io.intr_type = GPIO_INTR_DISABLE;
    io.mode = GPIO_MODE_OUTPUT;
    io.pin_bit_mask = (1ULL << LED1_PIN) | (1ULL << LED2_PIN) | (1ULL << LED3_PIN);
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io);

    gpio_set_level(LED1_PIN, 0);
    gpio_set_level(LED2_PIN, 0);
    gpio_set_level(LED3_PIN, 0);

    // Button as input + pull-up
    memset(&io, 0, sizeof(io));
    io.intr_type = GPIO_INTR_DISABLE;
    io.mode = GPIO_MODE_INPUT;
    io.pin_bit_mask = (1ULL << BUTTON_PIN);
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io);
}

static void test_preemptive_multitasking(void)
{
    ESP_LOGI(PREEMPT_TAG, "=== Preemptive Multitasking Demo ===");
    xTaskCreate(preemptive_task1, "PreTask1", 2048, NULL, 2, NULL);
    xTaskCreate(preemptive_task2, "PreTask2", 2048, NULL, 1, NULL);
    xTaskCreate(preemptive_emergency_task, "Emergency", 2048, NULL, 5, NULL);
    vTaskDelete(NULL);
}

void app_main(void)
{
    init_gpio_c();

    ESP_LOGI("MAIN", "Multitasking Comparison Demo");
    // test_cooperative_multitasking();  // ถ้าจะลองโหมด coop ให้คอมเมนต์บรรทัดล่าง
    test_preemptive_multitasking();
}
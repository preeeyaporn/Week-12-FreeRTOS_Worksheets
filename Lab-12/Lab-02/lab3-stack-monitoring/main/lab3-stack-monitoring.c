// ex3_optimized_stack.c
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"

#define LED_OK       GPIO_NUM_2
#define LED_WARNING  GPIO_NUM_4

static const char *TAG = "EX3_OPT_STACK";

// เกณฑ์เตือนสแตก
#define STACK_WARNING_THRESHOLD_BYTES   512
#define STACK_CRITICAL_THRESHOLD_BYTES  256

// ---------- Optimized Heavy Task: ใช้ heap แทน stack ----------
static TaskHandle_t optimized_task_handle = NULL;

static void optimized_heavy_task(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "Optimized Heavy Task started (use heap for large buffers)");

    // ใช้ heap แทน local arrays ขนาดใหญ่
    char *large_buffer   = (char*) malloc(1024);
    int  *large_numbers  = (int*)  malloc(200 * sizeof(int));
    char *another_buffer = (char*) malloc(512);

    if (!large_buffer || !large_numbers || !another_buffer) {
        ESP_LOGE(TAG, "Heap allocation failed (large_buffer=%p, large_numbers=%p, another_buffer=%p)",
                 (void*)large_buffer, (void*)large_numbers, (void*)another_buffer);
        // เก็บกวาดเท่าที่มี
        if (large_buffer)   free(large_buffer);
        if (large_numbers)  free(large_numbers);
        if (another_buffer) free(another_buffer);
        vTaskDelete(NULL);
        return;
    }

    int cycle = 0;
    while (1) {
        cycle++;

        // ใช้ heap memory
        memset(large_buffer, 'Y', 1023);
        large_buffer[1023] = '\0';

        for (int i = 0; i < 200; ++i) {
            large_numbers[i] = i * cycle;
        }

        snprintf(another_buffer, 512, "Optimized cycle %d: using heap buffers", cycle);

        ESP_LOGI(TAG, "%s | large_buffer_len=%d | last_number=%d",
                 another_buffer, (int)strlen(large_buffer), large_numbers[199]);

        // รายงาน High-Water Mark ของ task ตนเอง
        UBaseType_t hwm_words = uxTaskGetStackHighWaterMark(NULL);
        uint32_t    hwm_bytes = hwm_words * sizeof(StackType_t);
        ESP_LOGI(TAG, "[Optimized] stack HWM: %u bytes remaining", hwm_bytes);

        vTaskDelay(pdMS_TO_TICKS(1500));
    }

    // จุดนี้ปกติไม่ถึง (เพราะลูปไม่จบ) — เผื่อไว้
    free(large_buffer);
    free(large_numbers);
    free(another_buffer);
    vTaskDelete(NULL);
}

// ---------- Stack Monitor: เฝ้าดู task เป้าหมาย + อัพเดต LED ----------
static void stack_monitor_task(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "Stack Monitor started");

    while (1) {
        bool warn = false;
        bool crit = false;

        if (optimized_task_handle != NULL) {
            UBaseType_t hwm_words = uxTaskGetStackHighWaterMark(optimized_task_handle);
            uint32_t    hwm_bytes = hwm_words * sizeof(StackType_t);

            ESP_LOGI(TAG, "[Monitor] Optimized task HWM: %u bytes remaining", hwm_bytes);

            if (hwm_bytes < STACK_CRITICAL_THRESHOLD_BYTES) {
                ESP_LOGE(TAG, "CRITICAL: Optimized task stack very low!");
                crit = true;
            } else if (hwm_bytes < STACK_WARNING_THRESHOLD_BYTES) {
                ESP_LOGW(TAG, "WARNING: Optimized task stack low");
                warn = true;
            }
        } else {
            ESP_LOGW(TAG, "optimized_task_handle is NULL");
        }

        // อัพเดต LED
        if (crit) {
            for (int i = 0; i < 6; ++i) {
                gpio_set_level(LED_WARNING, 1);
                vTaskDelay(pdMS_TO_TICKS(70));
                gpio_set_level(LED_WARNING, 0);
                vTaskDelay(pdMS_TO_TICKS(70));
            }
            gpio_set_level(LED_OK, 0);
        } else if (warn) {
            gpio_set_level(LED_WARNING, 1);
            gpio_set_level(LED_OK, 0);
        } else {
            gpio_set_level(LED_OK, 1);
            gpio_set_level(LED_WARNING, 0);
        }

        // รายงาน heap system-wide
        extern uint32_t esp_get_free_heap_size(void);
        extern uint32_t esp_get_minimum_free_heap_size(void);
        ESP_LOGI(TAG, "Free heap: %u bytes | Min free heap: %u bytes",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)esp_get_minimum_free_heap_size());

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// ---------- (แนะนำ) Stack Overflow Hook ----------
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    ESP_LOGE("STACK_OVERFLOW", "Task %s overflowed its stack!", pcTaskName);

    // กระพริบไฟเตือนเร็ว ๆ แล้วรีสตาร์ท
    for (int i = 0; i < 20; ++i) {
        gpio_set_level(LED_WARNING, 1);
        vTaskDelay(pdMS_TO_TICKS(40));
        gpio_set_level(LED_WARNING, 0);
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    esp_restart();
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Step 3: Stack Optimization (Heap instead of large stack arrays) ===");

    // ตั้งค่า GPIO สำหรับ LED
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED_OK) | (1ULL << LED_WARNING),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&io_conf);
    gpio_set_level(LED_OK, 0);
    gpio_set_level(LED_WARNING, 0);

    // สร้าง Optimized Heavy Task
    // เนื่องจากใช้ heap แล้ว เราสามารถลดขนาดสแตกลงได้มาก
    const uint32_t optimized_stack_bytes = 1536; // พอสำหรับโลจิก + printf
    BaseType_t ok = xTaskCreate(
        optimized_heavy_task,
        "OptimizedHeavy",
        optimized_stack_bytes,
        NULL,
        3,
        &optimized_task_handle
    );
    if (ok != pdPASS || optimized_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create OptimizedHeavy task");
        return;
    }

    // สร้าง Stack Monitor (สแตกมากหน่อยสำหรับ logging/formatting)
    ok = xTaskCreate(
        stack_monitor_task,
        "StackMonitor",
        4096,
        NULL,
        4,
        NULL
    );
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create StackMonitor task");
        return;
    }

    ESP_LOGI(TAG, "Tasks created. Watch logs & LEDs (GPIO2 OK, GPIO4 WARNING/CRITICAL).");
}

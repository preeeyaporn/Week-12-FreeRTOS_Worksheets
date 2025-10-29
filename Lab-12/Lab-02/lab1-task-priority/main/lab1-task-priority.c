#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"

// ================== Pin Map ==================
#define LED_HIGH_PIN GPIO_NUM_2    // High Priority Task
#define LED_MED_PIN  GPIO_NUM_4    // Medium Priority Task
#define LED_LOW_PIN  GPIO_NUM_5    // Low Priority Task
#define BUTTON_PIN   GPIO_NUM_0    // Active-Low button

// ================== Demo Switches ==================
#define USE_MUTEX_FIX  1   // 0 = โชว์บั๊ก Priority Inversion, 1 = แก้ด้วย Mutex

// ================== Globals ==================
static const char *TAG = "PRIORITY_DEMO";

volatile uint32_t high_task_count = 0;
volatile uint32_t med_task_count  = 0;
volatile uint32_t low_task_count  = 0;
volatile bool priority_test_running = false;

static volatile bool shared_resource_busy = false;  // ใช้เฉพาะตอน USE_MUTEX_FIX=0
static SemaphoreHandle_t g_resource_mutex = NULL;   // ใช้เมื่อ USE_MUTEX_FIX=1

// ============== Utilities ==============
static void use_shared_resource(const char *who, TickType_t hold_ms)
{
    ESP_LOGI(TAG, "%s: acquired shared resource", who);
    vTaskDelay(pdMS_TO_TICKS(hold_ms));
    ESP_LOGI(TAG, "%s: released shared resource", who);
}

// ============== Step 1: Different Priorities ==============
static void high_priority_task(void *pvParameters)
{
    ESP_LOGI(TAG, "High Priority Task started (prio 5)");
    for (;;) {
        if (priority_test_running) {
            high_task_count++;
            ESP_LOGI(TAG, "HIGH RUN (%d)", high_task_count);
            gpio_set_level(LED_HIGH_PIN, 1);

            for (int i = 0; i < 100000; i++) {
                volatile int dummy = i * 2; (void)dummy;
            }

            gpio_set_level(LED_HIGH_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(200));
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

static void medium_priority_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Medium Priority Task started (prio 3)");
    for (;;) {
        if (priority_test_running) {
            med_task_count++;
            ESP_LOGI(TAG, "MED RUN (%d)", med_task_count);
            gpio_set_level(LED_MED_PIN, 1);

            for (int i = 0; i < 200000; i++) {
                volatile int dummy = i + 100; (void)dummy;
            }

            gpio_set_level(LED_MED_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(300));
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

static void low_priority_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Low Priority Task started (prio 1)");
    for (;;) {
        if (priority_test_running) {
            low_task_count++;
            ESP_LOGI(TAG, "LOW RUN (%d)", low_task_count);
            gpio_set_level(LED_LOW_PIN, 1);

            for (int i = 0; i < 500000; i++) {
                volatile int dummy = i - 50; (void)dummy;
                if (i % 100000 == 0) vTaskDelay(1); // กัน WDT
            }

            gpio_set_level(LED_LOW_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// ควบคุมเริ่ม/หยุดการทดลอง (กดปุ่มแล้วรัน 10 วินาที)
static void control_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Control Task started (prio 4)");
    for (;;) {
        if (gpio_get_level(BUTTON_PIN) == 0) { // active-low
            if (!priority_test_running) {
                ESP_LOGW(TAG, "=== START TEST (10s) ===");
                high_task_count = med_task_count = low_task_count = 0;
                priority_test_running = true;

                vTaskDelay(pdMS_TO_TICKS(10000));

                priority_test_running = false;
                ESP_LOGW(TAG, "=== RESULT ===");
                ESP_LOGI(TAG, "High runs: %u", high_task_count);
                ESP_LOGI(TAG, "Med  runs: %u", med_task_count);
                ESP_LOGI(TAG, "Low  runs: %u", low_task_count);
                uint32_t total = high_task_count + med_task_count + low_task_count;
                if (total) {
                    ESP_LOGI(TAG, "High %%: %.1f", (float)high_task_count/total*100.0f);
                    ESP_LOGI(TAG, "Med  %%: %.1f", (float)med_task_count /total*100.0f);
                    ESP_LOGI(TAG, "Low  %%: %.1f", (float)low_task_count /total*100.0f);
                }
                ESP_LOGW(TAG, "=== END TEST ===");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ============== Step 2: Round-Robin (Same Priority) ==============
static void equal_priority_task1(void *pvParameters)
{
    const char *who = "EQ-1";
    for (;;) {
        if (priority_test_running) {
            ESP_LOGI(TAG, "%s running", who);
            for (int i = 0; i < 300000; i++) { volatile int d = i; (void)d; }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void equal_priority_task2(void *pvParameters)
{
    const char *who = "EQ-2";
    for (;;) {
        if (priority_test_running) {
            ESP_LOGI(TAG, "%s running", who);
            for (int i = 0; i < 300000; i++) { volatile int d = i; (void)d; }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void equal_priority_task3(void *pvParameters)
{
    const char *who = "EQ-3";
    for (;;) {
        if (priority_test_running) {
            ESP_LOGI(TAG, "%s running", who);
            for (int i = 0; i < 300000; i++) { volatile int d = i; (void)d; }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ============== Step 3: Priority Inversion Demo ==============
static void pi_low_holder(void *pvParameters)
{
    const char *who = "PI-LOW";  // prio 1
    ESP_LOGI(TAG, "%s started", who);
    for (;;) {
        if (priority_test_running) {
        #if USE_MUTEX_FIX
            if (xSemaphoreTake(g_resource_mutex, portMAX_DELAY) == pdTRUE) {
                use_shared_resource(who, 2000); // จับยาว 2s
                xSemaphoreGive(g_resource_mutex);
            }
        #else
            ESP_LOGI(TAG, "%s: take shared resource (no mutex)", who);
            shared_resource_busy = true;
            use_shared_resource(who, 2000);
            shared_resource_busy = false;
        #endif
            vTaskDelay(pdMS_TO_TICKS(3000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

static void pi_medium_noise(void *pvParameters)
{
    const char *who = "PI-MED";  // prio 3
    ESP_LOGI(TAG, "%s started", who);
    for (;;) {
        if (priority_test_running) {
            for (int i = 0; i < 300000; i++) { volatile int d = i * 3; (void)d; }
            vTaskDelay(pdMS_TO_TICKS(20));
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

static void pi_high_waiter(void *pvParameters)
{
    const char *who = "PI-HIGH"; // prio 5
    ESP_LOGI(TAG, "%s started", who);
    for (;;) {
        if (priority_test_running) {
        #if USE_MUTEX_FIX
            ESP_LOGW(TAG, "%s needs resource (mutex path)", who);
            if (xSemaphoreTake(g_resource_mutex, portMAX_DELAY) == pdTRUE) {
                use_shared_resource(who, 200); // ใช้สั้น 0.2s
                xSemaphoreGive(g_resource_mutex);
            }
        #else
            ESP_LOGW(TAG, "%s needs resource (bug path)", who);
            while (shared_resource_busy) {
                ESP_LOGW(TAG, "%s BLOCKED by LOW!", who);
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            use_shared_resource(who, 200);
        #endif
            vTaskDelay(pdMS_TO_TICKS(500));
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// ============== app_main ==============
void app_main(void)
{
    ESP_LOGI(TAG, "=== FreeRTOS Priority Scheduling Demo (Step1+2+3) ===");

    // LEDs
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED_HIGH_PIN) | (1ULL << LED_MED_PIN) | (1ULL << LED_LOW_PIN),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&io_conf);
    gpio_set_level(LED_HIGH_PIN, 0);
    gpio_set_level(LED_MED_PIN, 0);
    gpio_set_level(LED_LOW_PIN, 0);

    // Button (pull-up)
    gpio_config_t button_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BUTTON_PIN),
        .pull_up_en = 1,
        .pull_down_en = 0,
    };
    gpio_config(&button_conf);

#if USE_MUTEX_FIX
    g_resource_mutex = xSemaphoreCreateMutex();
    if (!g_resource_mutex) {
        ESP_LOGE(TAG, "Failed to create resource mutex!");
    }
#endif

    // Step 1
    xTaskCreate(high_priority_task,   "HighPrio", 3072, NULL, 5, NULL);
    xTaskCreate(medium_priority_task, "MedPrio",  3072, NULL, 3, NULL);
    xTaskCreate(low_priority_task,    "LowPrio",  3072, NULL, 1, NULL);
    xTaskCreate(control_task,         "Control",  3072, NULL, 4, NULL);

    // Step 2 (Round-Robin, priority เท่ากัน = 2)
    xTaskCreate(equal_priority_task1, "Equal1",   2048, NULL, 2, NULL);
    xTaskCreate(equal_priority_task2, "Equal2",   2048, NULL, 2, NULL);
    xTaskCreate(equal_priority_task3, "Equal3",   2048, NULL, 2, NULL);

    // Step 3 (Priority Inversion Demo)
    xTaskCreate(pi_low_holder,   "PI_LOW",  3072, NULL, 1, NULL);
    xTaskCreate(pi_medium_noise, "PI_MED",  3072, NULL, 3, NULL);
    xTaskCreate(pi_high_waiter,  "PI_HIGH", 3072, NULL, 5, NULL);

    ESP_LOGI(TAG, "Press button (GPIO0) to run 10s test; watch logs and LEDs.");
}
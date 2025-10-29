#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define LED_HIGH_PIN  GPIO_NUM_2
#define LED_MED_PIN   GPIO_NUM_4
#define LED_LOW_PIN   GPIO_NUM_5
#define BUTTON_PIN    GPIO_NUM_0

static const char *TAG = "LAB1_EX2";

volatile uint32_t high_task_count = 0;
volatile uint32_t med_task_count  = 0;
volatile uint32_t low_task_count  = 0;
volatile bool     priority_test_running = false;

static TaskHandle_t g_low_task_handle = NULL;

// ===== Tasks เหมือนเดิม แต่จะใช้ xTaskCreatePinnedToCore แทน =====
static void high_priority_task(void *pv)
{
    ESP_LOGI(TAG, "High start (prio=%d, core=%d)", uxTaskPriorityGet(NULL), xPortGetCoreID());
    while (1) {
        if (priority_test_running) {
            high_task_count++;
            gpio_set_level(LED_HIGH_PIN, 1);
            for (volatile int i = 0; i < 100000; i++) { }
            gpio_set_level(LED_HIGH_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(200));
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

static void medium_priority_task(void *pv)
{
    ESP_LOGI(TAG, "Med  start (prio=%d, core=%d)", uxTaskPriorityGet(NULL), xPortGetCoreID());
    while (1) {
        if (priority_test_running) {
            med_task_count++;
            gpio_set_level(LED_MED_PIN, 1);
            for (volatile int i = 0; i < 200000; i++) { }
            gpio_set_level(LED_MED_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(300));
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

static void low_priority_task(void *pv)
{
    ESP_LOGI(TAG, "Low  start (prio=%d, core=%d)", uxTaskPriorityGet(NULL), xPortGetCoreID());
    while (1) {
        if (priority_test_running) {
            low_task_count++;
            gpio_set_level(LED_LOW_PIN, 1);
            for (volatile int i = 0; i < 500000; i++) {
                if ((i % 100000) == 0) vTaskDelay(1);
            }
            gpio_set_level(LED_LOW_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

static void control_task(void *pv)
{
    ESP_LOGI(TAG, "Control start (core=%d)", xPortGetCoreID());
    int last_btn = 1;
    while (1) {
        int btn = gpio_get_level(BUTTON_PIN);
        if (last_btn == 1 && btn == 0) {
            ESP_LOGW(TAG, "=== START TEST (10s) ===");
            high_task_count = med_task_count = low_task_count = 0;
            priority_test_running = true;
            vTaskDelay(pdMS_TO_TICKS(10000));
            priority_test_running = false;

            ESP_LOGW(TAG, "=== RESULT ===");
            uint32_t total = high_task_count + med_task_count + low_task_count;
            ESP_LOGI(TAG, "High:%u  Med:%u  Low:%u  Total:%u",
                     high_task_count, med_task_count, low_task_count, total);
            if (total) {
                ESP_LOGI(TAG, "High: %.1f%%  Med: %.1f%%  Low: %.1f%%",
                         (float)high_task_count/total*100.f,
                         (float)med_task_count /total*100.f,
                         (float)low_task_count /total*100.f);
            }
        }
        last_btn = btn;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// (ออปชัน) Task ปรับ priority แบบเดิมก็ใช้ร่วมได้ หากอยากดูผลควบคู่ affinity
static void dynamic_priority_demo(void *pv)
{
    TaskHandle_t low_task = (TaskHandle_t)pv;
    if (!low_task) vTaskDelete(NULL);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGW(TAG, "[Dynamic] Boost LOW -> prio 4 (core=%d)", xPortGetCoreID());
        vTaskPrioritySet(low_task, 4);

        vTaskDelay(pdMS_TO_TICKS(2000));
        ESP_LOGW(TAG, "[Dynamic] Restore LOW -> prio 1 (core=%d)", xPortGetCoreID());
        vTaskPrioritySet(low_task, 1);
    }
}

static void setup_gpio(void)
{
    gpio_config_t out = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED_HIGH_PIN) | (1ULL << LED_MED_PIN) | (1ULL << LED_LOW_PIN),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&out);

    gpio_config_t in = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BUTTON_PIN),
        .pull_down_en = 0,
        .pull_up_en = 1,
    };
    gpio_config(&in);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== LAB1 EX2: Task Affinity (Dual-Core) ===");
    setup_gpio();

    // กำหนด core ตามต้องการ:
    //   - ให้ High อยู่ Core 0
    //   - ให้ Control อยู่ Core 0 (เรียนรู้ลำดับ scheduling ฝั่งเดียวกัน)
    //   - ให้ Medium/Low อยู่ Core 1 เพื่อแยกโหลด
    // หมายเหตุ: สามารถสลับผังนี้เพื่อทดลองผลลัพธ์อื่น ๆ ได้
    xTaskCreatePinnedToCore(high_priority_task,   "HighPrio", 3072, NULL, 5, NULL, 0); // Core 0
    xTaskCreatePinnedToCore(control_task,         "Control",  3072, NULL, 4, NULL, 0); // Core 0
    xTaskCreatePinnedToCore(medium_priority_task, "MedPrio",  3072, NULL, 3, NULL, 1); // Core 1
    xTaskCreatePinnedToCore(low_priority_task,    "LowPrio",  3072, NULL, 1, &g_low_task_handle, 1); // Core 1

    // (ออปชัน) อยากดู dynamic + affinity พร้อมกัน ให้เปิดบรรทัดนี้
    // xTaskCreatePinnedToCore(dynamic_priority_demo, "DynPrio", 2048, (void*)g_low_task_handle, 2, NULL, 1);

    ESP_LOGI(TAG, "Pinned: High/Control->Core0, Med/Low->Core1. กดปุ่มเพื่อเริ่ม 10s test");
}
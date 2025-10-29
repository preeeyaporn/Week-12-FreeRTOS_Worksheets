#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

// ===== Pins (เปลี่ยนตามบอร์ดได้) =====
#define LED_HIGH_PIN  GPIO_NUM_2
#define LED_MED_PIN   GPIO_NUM_4
#define LED_LOW_PIN   GPIO_NUM_5
#define BUTTON_PIN    GPIO_NUM_0   // กดค้างเริ่ม/รีสตาร์ทการทดสอบ

static const char *TAG = "LAB1_EX1";

// ===== Globals =====
volatile uint32_t high_task_count = 0;
volatile uint32_t med_task_count  = 0;
volatile uint32_t low_task_count  = 0;
volatile bool     priority_test_running = false;

// เก็บ handle ของ low task เพื่อใช้ปรับ priority แบบ dynamic
static TaskHandle_t g_low_task_handle = NULL;

// ===== High / Medium / Low Tasks =====
static void high_priority_task(void *pv)
{
    ESP_LOGI(TAG, "High task start (prio=%d)", uxTaskPriorityGet(NULL));
    while (1) {
        if (priority_test_running) {
            high_task_count++;
            gpio_set_level(LED_HIGH_PIN, 1);
            // งานจำลองสั้น ๆ
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
    ESP_LOGI(TAG, "Med  task start (prio=%d)", uxTaskPriorityGet(NULL));
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
    ESP_LOGI(TAG, "Low  task start (prio=%d)", uxTaskPriorityGet(NULL));
    while (1) {
        if (priority_test_running) {
            low_task_count++;
            gpio_set_level(LED_LOW_PIN, 1);
            // งานฉากหลังยาวหน่อย แต่คอยพักเพื่อกัน watchdog
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

// ===== Control Task: กดปุ่มเพื่อเริ่ม/เก็บสถิติ =====
static void control_task(void *pv)
{
    ESP_LOGI(TAG, "Control start");
    int last_btn = 1;
    while (1) {
        int btn = gpio_get_level(BUTTON_PIN);
        if (last_btn == 1 && btn == 0) { // กดลง-ขอบหน้าชาย
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

// ===== Exercise 1: Dynamic Priority Demo =====
// ปรับ priority ของ low task เป็นช่วง ๆ : +boost เป็น 4 แล้วลดกลับ 1
static void dynamic_priority_demo(void *pv)
{
    TaskHandle_t low_task = (TaskHandle_t)pv;
    configASSERT(low_task != NULL);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGW(TAG, "[Dynamic] Boost LOW -> prio 4");
        vTaskPrioritySet(low_task, 4);

        vTaskDelay(pdMS_TO_TICKS(2000));
        ESP_LOGW(TAG, "[Dynamic] Restore LOW -> prio 1");
        vTaskPrioritySet(low_task, 1);
    }
}

// ===== Setup GPIO =====
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
        .pull_up_en = 1,  // ปุ่มต่อ GND → ใช้ pull-up
    };
    gpio_config(&in);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== LAB1 EX1: Dynamic Priority ===");
    setup_gpio();

    // Create base tasks (ไม่ pin core)
    xTaskCreate(high_priority_task,   "HighPrio", 3072, NULL, 5, NULL);
    xTaskCreate(medium_priority_task, "MedPrio",  3072, NULL, 3, NULL);
    xTaskCreate(low_priority_task,    "LowPrio",  3072, NULL, 1, &g_low_task_handle);
    xTaskCreate(control_task,         "Control",  3072, NULL, 4, NULL);

    // Exercise 1: สร้าง task ปรับ priority แบบ dynamic โดยส่ง handle ของ low task เข้าไป
    xTaskCreate(dynamic_priority_demo, "DynPrio", 2048, (void*)g_low_task_handle, 2, NULL);

    ESP_LOGI(TAG, "Press BUTTON to run 10s test; watch priority effects when LOW is boosted.");
}
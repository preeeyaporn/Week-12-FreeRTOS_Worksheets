#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"

// ===== Pins =====
#define LED_RUNNING   GPIO_NUM_2
#define LED_READY     GPIO_NUM_4
#define LED_BLOCKED   GPIO_NUM_5
#define LED_SUSPENDED GPIO_NUM_18

#define BUTTON1_PIN   GPIO_NUM_0   // Suspend/Resume
#define BUTTON2_PIN   GPIO_NUM_35  // Give semaphore (⚠ ไม่มี internal pull-up ต้องมีตัวต้านทานภายนอก)

static const char *TAG = "LAB2_EX1";

// ===== Handles / Globals =====
static TaskHandle_t state_demo_task_handle = NULL;
static TaskHandle_t ready_demo_task_handle = NULL;
static TaskHandle_t control_task_handle    = NULL;
static TaskHandle_t watcher_task_handle    = NULL;

static SemaphoreHandle_t demo_semaphore    = NULL;

static const char* state_names[6] = { "Running","Ready","Blocked","Suspended","Deleted","Invalid" };
static inline const char* get_state_name(eTaskState s){ return (s<=eDeleted)? state_names[s]: state_names[5]; }

// Exercise 1: Counter ต่อ "สถานะใหม่"
volatile uint32_t state_changes[6] = {0};  // index 0..4 = eRunning..eDeleted, 5 = Invalid
static inline void count_state_change(eTaskState old_state, eTaskState new_state){
    if (old_state != new_state) {
        int idx = (new_state <= eDeleted) ? (int)new_state : 5;
        state_changes[idx]++;
        ESP_LOGI(TAG, "Transition: %s -> %s (count[%s]=%u)",
                 get_state_name(old_state), get_state_name(new_state),
                 get_state_name(new_state), state_changes[idx]);
    }
}

// ===== Demo tasks (ปรับ LED ภายใน task นี้ตามสถานะ) =====
static void state_demo_task(void *pv){
    ESP_LOGI(TAG, "State Demo start (prio=%d)", uxTaskPriorityGet(NULL));
    int cycle = 0;
    while (1){
        cycle++;
        ESP_LOGI(TAG, "=== Cycle %d ===", cycle);

        // RUNNING
        gpio_set_level(LED_RUNNING, 1);
        gpio_set_level(LED_READY, 0);
        gpio_set_level(LED_BLOCKED, 0);
        gpio_set_level(LED_SUSPENDED, 0);
        for (volatile int i=0;i<400000;i++){}

        // READY (yield ให้ task พอ ๆ กัน)
        gpio_set_level(LED_RUNNING, 0);
        gpio_set_level(LED_READY, 1);
        taskYIELD();
        vTaskDelay(pdMS_TO_TICKS(100)); // ช่วงสั้น ๆ → Blocked(Delay)

        // BLOCKED (รอ semaphore)
        gpio_set_level(LED_READY, 0);
        gpio_set_level(LED_BLOCKED, 1);
        if (xSemaphoreTake(demo_semaphore, pdMS_TO_TICKS(1500)) == pdTRUE){
            // กลับมาทำงานช่วงสั้น
            gpio_set_level(LED_BLOCKED, 0);
            gpio_set_level(LED_RUNNING, 1);
            vTaskDelay(pdMS_TO_TICKS(300));
        } else {
            ESP_LOGI(TAG, "Semaphore timeout");
        }

        // BLOCKED (Delay ปกติ)
        gpio_set_level(LED_RUNNING, 0);
        gpio_set_level(LED_BLOCKED, 1);
        vTaskDelay(pdMS_TO_TICKS(800));
        gpio_set_level(LED_BLOCKED, 0);
    }
}

static void ready_state_demo_task(void *pv){
    ESP_LOGI(TAG, "Ready Demo start (prio=%d)", uxTaskPriorityGet(NULL));
    while (1){
        for (volatile int i=0;i<120000;i++){}
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

static void control_task(void *pv){
    ESP_LOGI(TAG, "Control start (prio=%d)", uxTaskPriorityGet(NULL));
    bool suspended = false;
    int tick = 0;
    while (1){
        // BTN1: Suspend/Resume
        if (gpio_get_level(BUTTON1_PIN) == 0){
            vTaskDelay(pdMS_TO_TICKS(40));
            if (!suspended){
                ESP_LOGW(TAG, "=== SUSPEND StateDemo ===");
                gpio_set_level(LED_SUSPENDED, 1);
                vTaskSuspend(state_demo_task_handle);
                suspended = true;
            } else {
                ESP_LOGW(TAG, "=== RESUME StateDemo ===");
                vTaskResume(state_demo_task_handle);
                gpio_set_level(LED_SUSPENDED, 0);
                suspended = false;
            }
            while (gpio_get_level(BUTTON1_PIN) == 0) vTaskDelay(pdMS_TO_TICKS(10));
        }
        // BTN2: Give semaphore
        if (gpio_get_level(BUTTON2_PIN) == 0){
            vTaskDelay(pdMS_TO_TICKS(40));
            ESP_LOGW(TAG, "=== GIVE SEMAPHORE ===");
            xSemaphoreGive(demo_semaphore);
            while (gpio_get_level(BUTTON2_PIN) == 0) vTaskDelay(pdMS_TO_TICKS(10));
        }

        // พิมพ์สรุป counter ทุก ~3 วิ
        if (++tick % 30 == 0){
            ESP_LOGI(TAG, "== State Change Counter ==");
            for (int s=0;s<6;s++){
                ESP_LOGI(TAG, "%-9s : %u", state_names[s], state_changes[s]);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// Watcher: เฝ้าดู StateDemo แล้วนับ transition
static void state_watcher_task(void *pv){
    ESP_LOGI(TAG, "Watcher start (prio=%d)", uxTaskPriorityGet(NULL));
    eTaskState last = eDeleted; // ค่าเริ่มอะไรก็ได้
    while (1){
        if (state_demo_task_handle){
            eTaskState cur = eTaskGetState(state_demo_task_handle);
            count_state_change(last, cur);
            last = cur;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ===== Setup / main =====
static void setup_gpio(void){
    gpio_config_t out_cfg = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL<<LED_RUNNING)|(1ULL<<LED_READY)|(1ULL<<LED_BLOCKED)|(1ULL<<LED_SUSPENDED),
        .pull_down_en = 0, .pull_up_en = 0
    };
    gpio_config(&out_cfg);
    gpio_config_t in_cfg = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL<<BUTTON1_PIN)|(1ULL<<BUTTON2_PIN),
        .pull_down_en = 0, .pull_up_en = 1 // ⚠ ไม่มีผลกับ GPIO35
    };
    gpio_config(&in_cfg);
}

void app_main(void){
    ESP_LOGI(TAG, "=== Lab2 Ex1: State Transition Counter ===");
    setup_gpio();

    demo_semaphore = xSemaphoreCreateBinary();
    if (!demo_semaphore){ ESP_LOGE(TAG, "Create semaphore failed"); return; }

    ESP_LOGI(TAG, "LED: RUN=2 READY=4 BLOCK=5 SUSP=18 | BTN: 0=Suspend/Resume, 35=Give sema");

    xTaskCreate(state_demo_task,       "StateDemo", 4096, NULL, 3, &state_demo_task_handle);
    xTaskCreate(ready_state_demo_task, "ReadyDemo", 2048, NULL, 3, &ready_demo_task_handle);
    xTaskCreate(control_task,          "Control",   3072, NULL, 4, &control_task_handle);
    xTaskCreate(state_watcher_task,    "Watcher",   2048, NULL, 2, &watcher_task_handle);
}
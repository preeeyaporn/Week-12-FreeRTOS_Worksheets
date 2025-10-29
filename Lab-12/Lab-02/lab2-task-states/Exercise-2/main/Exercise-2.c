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

#define BUTTON1_PIN   GPIO_NUM_0    // Suspend/Resume
#define BUTTON2_PIN   GPIO_NUM_35   // Give semaphore (⚠ ต้องมี pull-up ภายนอก)

// ===== Globals =====
static const char *TAG = "LAB2_EX2";

static TaskHandle_t state_demo_task_handle = NULL;
static TaskHandle_t ready_demo_task_handle = NULL;
static TaskHandle_t control_task_handle    = NULL;
static TaskHandle_t watcher_task_handle    = NULL;

static SemaphoreHandle_t demo_semaphore    = NULL;

static const char* state_names[] = { "Running","Ready","Blocked","Suspended","Deleted","Invalid" };
static inline const char* get_state_name(eTaskState s){ return (s<=eDeleted)? state_names[s] : state_names[5]; }

// Exercise 2: แสดงสถานะผ่าน LED แบบรวมศูนย์
static void update_state_display(eTaskState current_state)
{
    // ดับก่อน
    gpio_set_level(LED_RUNNING,   0);
    gpio_set_level(LED_READY,     0);
    gpio_set_level(LED_BLOCKED,   0);
    gpio_set_level(LED_SUSPENDED, 0);

    switch (current_state) {
        case eRunning:   gpio_set_level(LED_RUNNING,   1); break;
        case eReady:     gpio_set_level(LED_READY,     1); break;
        case eBlocked:   gpio_set_level(LED_BLOCKED,   1); break;
        case eSuspended: gpio_set_level(LED_SUSPENDED, 1); break;
        case eDeleted:
        default:
            for (int i=0;i<3;i++){
                gpio_set_level(LED_RUNNING,1); gpio_set_level(LED_READY,1);
                gpio_set_level(LED_BLOCKED,1); gpio_set_level(LED_SUSPENDED,1);
                vTaskDelay(pdMS_TO_TICKS(100));
                gpio_set_level(LED_RUNNING,0); gpio_set_level(LED_READY,0);
                gpio_set_level(LED_BLOCKED,0); gpio_set_level(LED_SUSPENDED,0);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            break;
    }
}

// ===== Demo Tasks =====
static void state_demo_task(void *pv)
{
    ESP_LOGI(TAG, "State Demo start (prio %u)", (unsigned)uxTaskPriorityGet(NULL));

    while (1) {
        // RUNNING
        update_state_display(eRunning);
        for (volatile int i=0;i<400000;i++) { }

        // READY (ยอมสละ CPU)
        update_state_display(eReady);
        taskYIELD();
        vTaskDelay(pdMS_TO_TICKS(100)); // ช่วง Blocked จาก delay

        // BLOCKED: รอ semaphore
        update_state_display(eBlocked);
        if (xSemaphoreTake(demo_semaphore, pdMS_TO_TICKS(1500)) == pdTRUE) {
            update_state_display(eRunning);
            vTaskDelay(pdMS_TO_TICKS(300));
        }

        // BLOCKED: delay ปกติ
        update_state_display(eBlocked);
        vTaskDelay(pdMS_TO_TICKS(800));
    }
}

static void ready_state_demo_task(void *pv)
{
    ESP_LOGI(TAG, "Ready Demo start (prio %u)", (unsigned)uxTaskPriorityGet(NULL));
    while (1) {
        for (volatile int i=0;i<120000;i++) { }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

static void control_task(void *pv)
{
    ESP_LOGI(TAG, "Control start (prio %u)", (unsigned)uxTaskPriorityGet(NULL));
    bool suspended = false;

    while (1) {
        // ปุ่ม 1: Suspend/Resume
        if (gpio_get_level(BUTTON1_PIN)==0){
            vTaskDelay(pdMS_TO_TICKS(40));
            if (!suspended){
                ESP_LOGW(TAG, "SUSPEND StateDemo");
                update_state_display(eSuspended);  // โชว์ก่อนสั่ง suspend
                vTaskSuspend(state_demo_task_handle);
                suspended = true;
            }else{
                ESP_LOGW(TAG, "RESUME StateDemo");
                vTaskResume(state_demo_task_handle);
                suspended = false;
            }
            while (gpio_get_level(BUTTON1_PIN)==0) vTaskDelay(pdMS_TO_TICKS(10));
        }

        // ปุ่ม 2: Give semaphore
        if (gpio_get_level(BUTTON2_PIN)==0){
            vTaskDelay(pdMS_TO_TICKS(40));
            ESP_LOGW(TAG, "GIVE semaphore");
            xSemaphoreGive(demo_semaphore);
            while (gpio_get_level(BUTTON2_PIN)==0) vTaskDelay(pdMS_TO_TICKS(10));
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// Watcher: คอยอ่านสถานะจริงของ StateDemo แล้วสั่ง update_state_display ซ้ำให้ตรง
static void state_watcher_task(void *pv)
{
    eTaskState last = eInvalid;
    ESP_LOGI(TAG, "Watcher start (prio %u)", (unsigned)uxTaskPriorityGet(NULL));

    while (1) {
        if (state_demo_task_handle){
            eTaskState cur = eTaskGetState(state_demo_task_handle);
            if (cur != last){
                ESP_LOGI(TAG, "[StateDemo] %s -> %s", get_state_name(last), get_state_name(cur));
                update_state_display(cur);
                last = cur;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ===== Setup & main =====
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
        .pull_down_en = 0, .pull_up_en = 1   // ⚠ ไม่มีผลกับ GPIO35
    };
    gpio_config(&in_cfg);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Lab 2 EX2: Custom State Indicator ===");
    setup_gpio();

    demo_semaphore = xSemaphoreCreateBinary();
    if (!demo_semaphore){ ESP_LOGE(TAG, "Create semaphore failed"); return; }

    xTaskCreate(state_demo_task,      "StateDemo", 4096, NULL, 3, &state_demo_task_handle);
    xTaskCreate(ready_state_demo_task,"ReadyDemo", 2048, NULL, 3, &ready_demo_task_handle);
    xTaskCreate(control_task,         "Control",   3072, NULL, 4, &control_task_handle);
    xTaskCreate(state_watcher_task,   "Watcher",   3072, NULL, 2, &watcher_task_handle);

    ESP_LOGI(TAG, "LED: RUN=2 READY=4 BLOCK=5 SUSP=18 | BTN: 0=Suspend  35=GiveSem");
}
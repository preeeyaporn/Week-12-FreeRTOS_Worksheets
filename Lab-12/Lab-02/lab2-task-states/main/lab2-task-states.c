#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"

// ===================== Pin Mapping =====================
#define LED_RUNNING     GPIO_NUM_2
#define LED_READY       GPIO_NUM_4
#define LED_BLOCKED     GPIO_NUM_5
#define LED_SUSPENDED   GPIO_NUM_18

#define BUTTON1_PIN     GPIO_NUM_0     // ปุ่ม Suspend/Resume
#define BUTTON2_PIN     GPIO_NUM_35    // ปุ่ม Give Semaphore (input only)

// ===================== Log Tag =====================
static const char *TAG = "TASK_STATES_LAB2_S3";

// ===================== Globals =====================
static TaskHandle_t state_demo_task_handle   = NULL;
static TaskHandle_t ready_demo_task_handle   = NULL;
static TaskHandle_t control_task_handle      = NULL;
static TaskHandle_t monitor_task_handle      = NULL;
static TaskHandle_t states_watcher_handle    = NULL;
static TaskHandle_t external_delete_handle   = NULL;

static SemaphoreHandle_t demo_semaphore = NULL;

static const char* state_names[] = {
    "Running", "Ready", "Blocked", "Suspended", "Deleted", "Invalid"
};

// ===================== Helpers =====================
static const char* get_state_name(eTaskState st) {
    if (st <= eDeleted) return state_names[st];
    return state_names[5];
}

static void all_led_off(void) {
    gpio_set_level(LED_RUNNING, 0);
    gpio_set_level(LED_READY, 0);
    gpio_set_level(LED_BLOCKED, 0);
    gpio_set_level(LED_SUSPENDED, 0);
}

static void indicate_running(void)  { all_led_off(); gpio_set_level(LED_RUNNING, 1); }
static void indicate_ready(void)    { all_led_off(); gpio_set_level(LED_READY, 1); }
static void indicate_blocked(void)  { all_led_off(); gpio_set_level(LED_BLOCKED, 1); }
static void indicate_suspended(void){ all_led_off(); gpio_set_level(LED_SUSPENDED, 1); }

// ===================== Step 1–2 Tasks =====================
static void state_demo_task(void *pv) {
    ESP_LOGI(TAG, "StateDemo started");
    int cycle = 0;
    while (1) {
        cycle++;
        ESP_LOGI(TAG, "[Cycle %d] RUNNING", cycle);
        indicate_running();
        for (volatile int i = 0; i < 200000; i++) {}

        ESP_LOGI(TAG, "READY (yield)");
        indicate_ready();
        taskYIELD();
        vTaskDelay(pdMS_TO_TICKS(100));

        ESP_LOGI(TAG, "BLOCKED (waiting semaphore)");
        indicate_blocked();
        if (xSemaphoreTake(demo_semaphore, pdMS_TO_TICKS(2000)) == pdTRUE) {
            ESP_LOGI(TAG, "Got semaphore -> RUNNING short work");
            indicate_running();
            vTaskDelay(pdMS_TO_TICKS(300));
        } else {
            ESP_LOGW(TAG, "Semaphore timeout");
        }

        ESP_LOGI(TAG, "BLOCKED (vTaskDelay)");
        indicate_blocked();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void ready_state_demo_task(void *pv) {
    ESP_LOGI(TAG, "ReadyDemo started");
    while (1) {
        for (volatile int i = 0; i < 80000; i++) {}
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

static void control_task(void *pv) {
    ESP_LOGI(TAG, "Control started");
    bool suspended = false;
    uint32_t ticks = 0;
    bool external_deleted = false;

    while (1) {
        ticks++;

        // Suspend / Resume
        if (gpio_get_level(BUTTON1_PIN) == 0) {
            vTaskDelay(pdMS_TO_TICKS(40));
            if (!suspended) {
                ESP_LOGW(TAG, ">>> SUSPEND StateDemo");
                vTaskSuspend(state_demo_task_handle);
                indicate_suspended();
                suspended = true;
            } else {
                ESP_LOGW(TAG, ">>> RESUME StateDemo");
                vTaskResume(state_demo_task_handle);
                suspended = false;
            }
            while (gpio_get_level(BUTTON1_PIN) == 0) vTaskDelay(pdMS_TO_TICKS(10));
        }

        // Give semaphore
        if (gpio_get_level(BUTTON2_PIN) == 0) {
            vTaskDelay(pdMS_TO_TICKS(40));
            ESP_LOGW(TAG, ">>> GIVE semaphore");
            xSemaphoreGive(demo_semaphore);
            while (gpio_get_level(BUTTON2_PIN) == 0) vTaskDelay(pdMS_TO_TICKS(10));
        }

        // Status report
        if (ticks % 30 == 0) {
            ESP_LOGI(TAG, "--- STATUS REPORT ---");
            if (state_demo_task_handle)
                ESP_LOGI(TAG, "StateDemo: %s", get_state_name(eTaskGetState(state_demo_task_handle)));
            if (ready_demo_task_handle)
                ESP_LOGI(TAG, "ReadyDemo: %s", get_state_name(eTaskGetState(ready_demo_task_handle)));
        }

        // Delete external task after ~15s
        if (!external_deleted && ticks == 150) {
            if (external_delete_handle) {
                ESP_LOGW(TAG, ">>> Deleting external task (~15s)");
                vTaskDelete(external_delete_handle);
                external_delete_handle = NULL;
            }
            external_deleted = true;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void system_monitor_task(void *pv) {
    ESP_LOGI(TAG, "SysMonitor started");
    char *list_buf  = malloc(1024);
    char *stats_buf = malloc(1024);
    if (!list_buf || !stats_buf) {
        ESP_LOGE(TAG, "malloc failed");
        vTaskDelete(NULL);
        return;
    }
    while (1) {
        ESP_LOGI(TAG, "\n=== SYSTEM MONITOR ===");
#if (configUSE_TRACE_FACILITY == 1) && (configUSE_STATS_FORMATTING_FUNCTIONS == 1)
        vTaskList(list_buf);
        ESP_LOGI(TAG, "Task List:\nName\t\tState\tPrio\tStack\tNum\n%s", list_buf);
        vTaskGetRunTimeStats(stats_buf);
        ESP_LOGI(TAG, "Runtime Stats:\nTask\t\tAbs Time\t%%Time\n%s", stats_buf);
#else
        ESP_LOGW(TAG, "Trace/Stats not enabled in menuconfig.");
#endif
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void self_deleting_task(void *pv) {
    int lifetime = *((int*)pv);
    ESP_LOGI(TAG, "SelfDelete: will live %d s", lifetime);
    for (int i = lifetime; i > 0; i--) {
        ESP_LOGI(TAG, "Countdown: %d", i);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "SelfDelete going to DELETED");
    vTaskDelete(NULL);
}

static void external_delete_task(void *pv) {
    int tick = 0;
    ESP_LOGI(TAG, "ExtDelete started");
    while (1) {
        ESP_LOGI(TAG, "ExtDelete running %d", tick++);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ===================== STEP 3: State Watcher =====================
typedef struct {
    const char *name;
    TaskHandle_t *handle_ptr;
    eTaskState last_state;
    uint32_t state_counts[5];
} task_watch_t;

static task_watch_t watched[] = {
    { "StateDemo", &state_demo_task_handle, eInvalid, {0} },
    { "ReadyDemo", &ready_demo_task_handle, eInvalid, {0} },
    { "Control",   &control_task_handle,    eInvalid, {0} },
    { "Monitor",   &monitor_task_handle,    eInvalid, {0} },
    { "ExtDelete", &external_delete_handle, eInvalid, {0} },
};
static const int watched_count = sizeof(watched) / sizeof(watched[0]);

static void monitor_task_states(void) {
    ESP_LOGI(TAG, "=== DETAILED TASK STATE MONITOR ===");
    for (int i = 0; i < watched_count; i++) {
        TaskHandle_t h = (watched[i].handle_ptr) ? *(watched[i].handle_ptr) : NULL;
        if (h != NULL) {
            eTaskState st = eTaskGetState(h);
            UBaseType_t prio = uxTaskPriorityGet(h);
            UBaseType_t stack_rem = uxTaskGetStackHighWaterMark(h);
            ESP_LOGI(TAG, "%s: State=%s, Priority=%u, Stack=%u bytes",
                     watched[i].name, get_state_name(st),
                     (unsigned)prio, (unsigned)(stack_rem * sizeof(StackType_t)));
        } else {
            ESP_LOGI(TAG, "%s: Handle=NULL (maybe deleted)", watched[i].name);
        }
    }
}

static void count_state_change(task_watch_t *w, eTaskState old_state, eTaskState new_state) {
    if (!w) return;
    if (new_state <= eDeleted && old_state != new_state) {
        w->state_counts[new_state]++;
        ESP_LOGI(TAG, "[TRANSITION] %s: %s -> %s (Count[%s]=%u)",
                 w->name, get_state_name(old_state), get_state_name(new_state),
                 get_state_name(new_state), (unsigned)w->state_counts[new_state]);
    }
}

static void states_watcher_task(void *pv) {
    const TickType_t poll_every = pdMS_TO_TICKS(250);
    uint32_t ticks = 0;

    for (int i = 0; i < watched_count; i++) {
        TaskHandle_t h = (watched[i].handle_ptr) ? *(watched[i].handle_ptr) : NULL;
        watched[i].last_state = (h != NULL) ? eTaskGetState(h) : eInvalid;
        if (watched[i].last_state <= eDeleted)
            watched[i].state_counts[watched[i].last_state]++;
    }

    while (1) {
        for (int i = 0; i < watched_count; i++) {
            TaskHandle_t h = (watched[i].handle_ptr) ? *(watched[i].handle_ptr) : NULL;
            eTaskState cur = (h != NULL) ? eTaskGetState(h) : eInvalid;
            count_state_change(&watched[i], watched[i].last_state, cur);
            watched[i].last_state = cur;
        }

        if (++ticks % 8 == 0) monitor_task_states();

        if (ticks % 20 == 0) {
            ESP_LOGI(TAG, "--- STATE COUNTS SUMMARY ---");
            for (int i = 0; i < watched_count; i++) {
                ESP_LOGI(TAG, "%-10s | Run:%u Ready:%u Block:%u Susp:%u Del:%u",
                         watched[i].name,
                         watched[i].state_counts[eRunning],
                         watched[i].state_counts[eReady],
                         watched[i].state_counts[eBlocked],
                         watched[i].state_counts[eSuspended],
                         watched[i].state_counts[eDeleted]);
            }
        }

        vTaskDelay(poll_every);
    }
}

// ===================== app_main =====================
void app_main(void) {
    ESP_LOGI(TAG, "=== FreeRTOS Task States Demo (Step 3) ===");

    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL<<LED_RUNNING)|(1ULL<<LED_READY)|(1ULL<<LED_BLOCKED)|(1ULL<<LED_SUSPENDED),
        .pull_down_en = 0, .pull_up_en = 0,
    };
    gpio_config(&io_conf);
    all_led_off();

    gpio_config_t btn1 = { .intr_type = GPIO_INTR_DISABLE, .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL<<BUTTON1_PIN), .pull_up_en = 1, .pull_down_en = 0 };
    gpio_config(&btn1);

    gpio_config_t btn2 = { .intr_type = GPIO_INTR_DISABLE, .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL<<BUTTON2_PIN), .pull_up_en = 0, .pull_down_en = 0 };
    gpio_config(&btn2);

    demo_semaphore = xSemaphoreCreateBinary();
    if (!demo_semaphore) { ESP_LOGE(TAG, "Semaphore create failed"); return; }

    static int self_delete_time = 10;

    xTaskCreate(state_demo_task, "StateDemo", 4096, NULL, 3, &state_demo_task_handle);
    xTaskCreate(ready_state_demo_task, "ReadyDemo", 2048, NULL, 3, &ready_demo_task_handle);
    xTaskCreate(control_task, "Control", 3072, NULL, 4, &control_task_handle);
    xTaskCreate(system_monitor_task, "Monitor", 4096, NULL, 1, &monitor_task_handle);
    xTaskCreate(self_deleting_task, "SelfDelete", 2048, &self_delete_time, 2, NULL);
    xTaskCreate(external_delete_task, "ExtDelete", 2048, NULL, 2, &external_delete_handle);
    xTaskCreate(states_watcher_task, "StatesWatcher", 3072, NULL, 2, &states_watcher_handle);

    ESP_LOGI(TAG, "All tasks created. Observe LEDs & Serial logs.");
}


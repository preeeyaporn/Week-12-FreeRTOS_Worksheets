// ex2_dynamic_stack_monitor.c
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "EX2_DYN_MON";

// ---------- Worker: สลับเบา/หนักเพื่อให้ HWM เปลี่ยน ----------
static void worker_task(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "worker_task started (alternating light/heavy)");

    int cycle = 0;
    while (1) {
        cycle++;

        if (cycle % 2 == 1) {
            // โหมดเบา: ใช้ local เล็ก ๆ
            char small_buf[128];
            memset(small_buf, 'a', sizeof(small_buf));
            small_buf[sizeof(small_buf)-1] = '\0';
            ESP_LOGI(TAG, "[worker] LIGHT mode, buf=%u", (unsigned)sizeof(small_buf));
        } else {
            // โหมดหนัก: ใช้ local ใหญ่กว่าเพื่อกด HWM ลง
            char bigA[700];
            int  bigNums[180];   // ~720 bytes
            char bigB[300];

            memset(bigA, 'X', sizeof(bigA));
            memset(bigB, 'Y', sizeof(bigB));
            for (int i = 0; i < 180; ++i) bigNums[i] = i * cycle;

            ESP_LOGW(TAG, "[worker] HEAVY mode, bigA=%u, bigNums=%u*4, bigB=%u",
                     (unsigned)sizeof(bigA), (unsigned)180, (unsigned)sizeof(bigB));
        }

        // รายงาน HWM ปัจจุบัน
        UBaseType_t rem_words = uxTaskGetStackHighWaterMark(NULL);
        uint32_t    rem_bytes = rem_words * sizeof(StackType_t);
        ESP_LOGI(TAG, "[worker] HWM now: %u bytes", rem_bytes);

        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}

// ---------- Monitor: วัดการเปลี่ยนแปลงของ HWM ----------
typedef struct {
    TaskHandle_t handle;
    const char  *name;
    uint32_t     warn_threshold_bytes;   // ถ้าใช้เพิ่มขึ้นเกินเท่านี้ให้เตือนแรง
} monitor_cfg_t;

static void monitor_task(void *pvParameters)
{
    monitor_cfg_t cfg = *(monitor_cfg_t *)pvParameters;

    ESP_LOGI(TAG, "monitor_task started for '%s'", cfg.name);

    // เก็บค่า HWM ครั้งก่อน (เป็น words)
    UBaseType_t prev_words = 0;

    while (1) {
        if (cfg.handle == NULL) {
            ESP_LOGW(TAG, "Target handle is NULL, waiting...");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        UBaseType_t cur_words = uxTaskGetStackHighWaterMark(cfg.handle);
        uint32_t    cur_bytes = cur_words * sizeof(StackType_t);

        if (prev_words != 0 && cur_words < prev_words) {
            // HWM ลดลง => ใช้สแตกมากขึ้น
            uint32_t inc_bytes = (prev_words - cur_words) * sizeof(StackType_t);
            if (inc_bytes >= cfg.warn_threshold_bytes) {
                ESP_LOGE(TAG, "[%s] Stack usage INCREASED by %u bytes (CRITICAL)",
                         cfg.name, inc_bytes);
            } else {
                ESP_LOGW(TAG, "[%s] Stack usage increased by %u bytes",
                         cfg.name, inc_bytes);
            }
        }

        prev_words = cur_words;

        ESP_LOGI(TAG, "[%s] HWM: %u bytes (min free on stack)", cfg.name, cur_bytes);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Exercise 2: Dynamic Stack Monitoring ===");

    // สร้าง worker ก่อน เพื่อให้ได้ handle
    TaskHandle_t worker_h = NULL;
    // ขนาดสแตกกะทัดรัด แต่เผื่อพอสำหรับโหมดหนักข้างบน
    const uint32_t worker_stack_bytes = 3072;

    BaseType_t ok = xTaskCreate(
        worker_task,
        "Worker",
        worker_stack_bytes,
        NULL,
        3,
        &worker_h
    );
    if (ok != pdPASS || worker_h == NULL) {
        ESP_LOGE(TAG, "Failed to create Worker task");
        return;
    }

    // ตั้ง monitor ให้เฝ้า Worker โดยแจ้งเตือนแรงเมื่อเพิ่มขึ้น >= 256 ไบต์
    monitor_cfg_t cfg = {
        .handle = worker_h,
        .name   = "Worker",
        .warn_threshold_bytes = 256
    };

    // หมายเหตุ: ส่งค่า cfg แบบ copy (stack ของ caller) จึงสร้าง struct ชั่วคราวใน static ให้ก็ได้
    static monitor_cfg_t cfg_copy;
    cfg_copy = cfg;

    TaskHandle_t mon_h = NULL;
    ok = xTaskCreate(
        monitor_task,
        "StackMonitor",
        4096,           // เผื่อ logger + formatting
        &cfg_copy,
        4,
        &mon_h
    );
    if (ok != pdPASS || mon_h == NULL) {
        ESP_LOGE(TAG, "Failed to create Monitor task");
        return;
    }

    ESP_LOGI(TAG, "Worker + Monitor created. Watch logs for HWM deltas.");
}
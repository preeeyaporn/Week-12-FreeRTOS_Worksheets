// main.c - Real-time Scheduler + Adaptive Performance (Auto-Tune Periods)
// ESP-IDF v5.x

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"

static const char *TAG = "RT_ADAPT";

// ====== Demo GPIO (optional visual) ======
#define LED_OK      GPIO_NUM_2     // แจ้งสถานะปกติ (วิ่งงาน)
#define LED_MISS    GPIO_NUM_4     // กระพริบเมื่อ deadline miss
#define LED_SCHED   GPIO_NUM_5     // กระพริบเมื่อ scheduler dispatch

// ====== Config ======
#define NUM_WORKERS         2
#define WORKER_STACK        4096
#define SCHED_STACK         6144
#define MON_STACK           4096
#define ADAPT_STACK         4096
#define LOAD_STACK          3072

#define SCHED_TICK_MS       10
#define DISPATCH_BUDGET     8
#define WORKER_QUEUE_LEN    16
#define COMPLETE_QUEUE_LEN  32

// ====== Job Model ======
typedef struct {
    int     id;
    char    name[16];
    int     priority;          // มาก = สำคัญกว่า
    volatile uint32_t period_ms;        // คาบของงาน (จะถูก "ปรับ" โดย Adaptive)
    uint32_t wcet_ms;          // เวลาใช้จริงโดยประมาณ (เดโม)
    uint32_t deadline_ms;      // relative deadline จาก release

    // bounds สำหรับ Adaptive
    uint32_t min_period_ms;
    uint32_t max_period_ms;

    // runtime
    volatile int64_t next_release_us;

    // stats (เคลียร์โดย Adaptiveรอบ/วินาทีเพื่อคำนวณ metrics)
    volatile uint32_t releases;
    volatile uint32_t completions;
    volatile uint32_t deadline_miss;
    volatile uint64_t sum_response_ms;
    volatile uint32_t max_response_ms;

    // snapshot (สำหรับ Adaptive อ่านแบบเสถียร)
    uint32_t snap_releases;
    uint32_t snap_completions;
    uint32_t snap_deadline_miss;
    uint64_t snap_sum_response_ms;
    uint32_t snap_max_response_ms;
} job_desc_t;

typedef struct {
    int     job_id;
    int     priority;
    uint32_t exec_ms;
    int64_t  abs_deadline_us;
    int64_t  release_us;
} worker_cmd_t;

typedef struct {
    int     job_id;
    int64_t finish_us;
    int64_t abs_deadline_us;
    int64_t release_us;
} completion_t;

// ====== Demo jobs ======
enum { JOB_A = 0, JOB_B, JOB_C, NUM_JOBS };
static job_desc_t g_jobs[NUM_JOBS] = {
    //  id  name prio period wcet ddl  minP maxP
    { .id=JOB_A, .name="A", .priority=3, .period_ms=50,  .wcet_ms=12, .deadline_ms=40,  .min_period_ms=30,  .max_period_ms=120 },
    { .id=JOB_B, .name="B", .priority=2, .period_ms=100, .wcet_ms=20, .deadline_ms=60,  .min_period_ms=60,  .max_period_ms=300 },
    { .id=JOB_C, .name="C", .priority=1, .period_ms=200, .wcet_ms=60, .deadline_ms=150, .min_period_ms=120, .max_period_ms=600 },
};

// ====== Queues & Handles ======
static QueueHandle_t q_worker[NUM_WORKERS];
static QueueHandle_t q_complete;
static TaskHandle_t  h_worker[NUM_WORKERS];

// ====== Helpers ======
static inline int64_t now_us(void) { return esp_timer_get_time(); }

static void busy_exec_ms(uint32_t ms)
{
    // เดโม: ใช้ vTaskDelay เพื่อไม่บล็อก scheduler จริง
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void blink_once(gpio_num_t pin, int ms)
{
    gpio_set_level(pin, 1);
    vTaskDelay(pdMS_TO_TICKS(ms));
    gpio_set_level(pin, 0);
}

// เลือก worker ตามความยาวคิว
static int select_worker(void)
{
    int best = 0;
    UBaseType_t best_len = uxQueueMessagesWaiting(q_worker[0]);
    for (int i=1;i<NUM_WORKERS;i++){
        UBaseType_t l = uxQueueMessagesWaiting(q_worker[i]);
        if (l < best_len) { best = i; best_len = l; }
    }
    return best;
}

// ====== Worker ======
static void worker_task(void *arg)
{
    int wid = (int)(intptr_t)arg;
    gpio_num_t led = LED_OK;

    ESP_LOGI(TAG, "Worker %d start", wid);
    while (1) {
        worker_cmd_t cmd;
        if (xQueueReceive(q_worker[wid], &cmd, portMAX_DELAY) == pdTRUE) {
            // ทำงาน
            busy_exec_ms(cmd.exec_ms);
            int64_t fin_us = now_us();

            // ส่งผลกลับ
            completion_t c = {
                .job_id = cmd.job_id,
                .finish_us = fin_us,
                .abs_deadline_us = cmd.abs_deadline_us,
                .release_us = cmd.release_us
            };
            xQueueSend(q_complete, &c, 0);

            blink_once(led, 5);
        }
    }
}

// ====== Scheduler ======
typedef struct {
    int job_id;
    int priority;
    int64_t abs_deadline_us;
    int64_t release_us;
} ready_item_t;

static void scheduler_task(void *arg)
{
    int64_t t0 = now_us();
    for (int i=0;i<NUM_JOBS;i++){
        g_jobs[i].next_release_us = t0; // เริ่มพร้อมกัน
    }

    gpio_set_direction(LED_SCHED, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_OK, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_MISS, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_SCHED, 0);
    gpio_set_level(LED_OK, 0);
    gpio_set_level(LED_MISS, 0);

    ready_item_t ready[NUM_JOBS];

    ESP_LOGI(TAG, "Scheduler start (tick=%dms)", SCHED_TICK_MS);

    while (1) {
        int64_t tnow = now_us();

        // 1) Collect ready jobs
        int nready = 0;
        for (int i=0;i<NUM_JOBS;i++){
            if (tnow >= g_jobs[i].next_release_us) {
                ready[nready].job_id = g_jobs[i].id;
                ready[nready].priority = g_jobs[i].priority;
                ready[nready].release_us = g_jobs[i].next_release_us;
                ready[nready].abs_deadline_us = g_jobs[i].next_release_us + (int64_t)g_jobs[i].deadline_ms*1000;
                nready++;

                g_jobs[i].next_release_us += (int64_t)g_jobs[i].period_ms*1000;
                g_jobs[i].releases++;
            }
        }

        // 2) Sort: priority desc, deadline asc
        for (int i=0;i<nready;i++){
            for (int j=i+1;j<nready;j++){
                bool swap = false;
                if (ready[j].priority > ready[i].priority) swap = true;
                else if (ready[j].priority == ready[i].priority &&
                         ready[j].abs_deadline_us < ready[i].abs_deadline_us) swap = true;
                if (swap){ ready_item_t tmp = ready[i]; ready[i]=ready[j]; ready[j]=tmp; }
            }
        }

        // 3) Dispatch (budget)
        int dispatched = 0;
        for (int k=0;k<nready && dispatched<DISPATCH_BUDGET; k++){
            int id = ready[k].job_id;
            job_desc_t *jb = &g_jobs[id];

            worker_cmd_t cmd = {
                .job_id = id,
                .priority = jb->priority,
                .exec_ms = jb->wcet_ms,
                .abs_deadline_us = ready[k].abs_deadline_us,
                .release_us = ready[k].release_us
            };

            int w = select_worker();
            if (xQueueSend(q_worker[w], &cmd, 0) == pdPASS){
                dispatched++;
                gpio_set_level(LED_SCHED, 1);
                esp_rom_delay_us(300);
                gpio_set_level(LED_SCHED, 0);
            } else {
                ESP_LOGW(TAG, "Worker %d queue full, defer job %s", w, jb->name);
            }
        }

        // 4) Collect completions + deadline check
        completion_t comp;
        while (xQueueReceive(q_complete, &comp, 0) == pdTRUE) {
            job_desc_t *jb = &g_jobs[comp.job_id];
            jb->completions++;

            bool miss = (comp.finish_us > comp.abs_deadline_us);
            if (miss) {
                jb->deadline_miss++;
                blink_once(LED_MISS, 8);
                ESP_LOGW(TAG, "DEADLINE MISS job %s", jb->name);
            }

            uint32_t resp_ms = (uint32_t)((comp.finish_us - comp.release_us)/1000);
            jb->sum_response_ms += resp_ms;
            if (resp_ms > jb->max_response_ms) jb->max_response_ms = resp_ms;
        }

        vTaskDelay(pdMS_TO_TICKS(SCHED_TICK_MS));
    }
}

// ====== Monitor (เหมือนเดิม) ======
static void monitor_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "===== MONITOR =====");
        float util_sum = 0.0f;
        for (int i=0;i<NUM_JOBS;i++){
            job_desc_t *jb = &g_jobs[i];
            float util = (float)jb->wcet_ms / (float)jb->period_ms * 100.0f;
            util_sum += util;
            float avg_resp = (jb->completions>0) ? (float)jb->sum_response_ms/(float)jb->completions : 0.0f;
            ESP_LOGI(TAG, "Job %s (P%d): period=%ums wcet=%ums ddl=%ums | rel=%u comp=%u miss=%u | util=%.1f%% resp(avg=%.1f max=%u) ms",
                     jb->name, jb->priority, jb->period_ms, jb->wcet_ms, jb->deadline_ms,
                     jb->releases, jb->completions, jb->deadline_miss, util, avg_resp, jb->max_response_ms);
        }
        for (int w=0; w<NUM_WORKERS; w++){
            ESP_LOGI(TAG, "Worker %d queue depth: %u", w, (unsigned)uxQueueMessagesWaiting(q_worker[w]));
        }
        ESP_LOGI(TAG, "Total sched utilization ≈ %.1f%%", util_sum);
        ESP_LOGI(TAG, "====================");
    }
}

// ====== Adaptive Controller ======
// ปรับ period ของแต่ละงานแบบมี guard rails + hysteresis + cooldown
typedef struct {
    // global targets
    float target_total_util_pct;   // เป้าหมาย utilization รวม (เช่น 70%)
    float low_total_util_pct;      // ถ้าต่ำกว่านี้ → ค่อยๆ เร่ง (ลด period)

    // per-job thresholds
    float miss_hi_pct;             // ถ้า miss rate > ค่านี้ → ผ่อน (เพิ่ม period)
    float resp_hi_ratio;           // ถ้า avg_resp > ratio*deadline → ผ่อน
    float resp_lo_ratio;           // ถ้า avg_resp < ratio*deadline และไม่มี miss → เร่ง

    // step & hysteresis
    float step_up_pct;             // ผ่อน: period *= (1 + step_up_pct)
    float step_down_pct;           // เร่ง: period *= (1 - step_down_pct)
    uint32_t cooldown_ms;          // เวลาขั้นต่ำระหว่างการปรับของแต่ละงาน

    // queue sensitivity
    float qdepth_hi;               // ถ้าคิวเฉลี่ยของ worker > ค่า → ผ่อน
    float qdepth_lo;               // ถ้าคิวเฉลี่ย < ค่า และระบบว่าง → เร่ง
} adapt_cfg_t;

static adapt_cfg_t g_adapt_cfg = {
    .target_total_util_pct = 75.0f,
    .low_total_util_pct    = 55.0f,
    .miss_hi_pct           = 1.5f,      // >1.5% ถือว่าสูง
    .resp_hi_ratio         = 0.80f,     // >80% ของ deadline
    .resp_lo_ratio         = 0.45f,     // <45% ของ deadline
    .step_up_pct           = 0.20f,     // +20%
    .step_down_pct         = 0.10f,     // -10%
    .cooldown_ms           = 5000,      // 5s ต่อ job
    .qdepth_hi             = 4.0f,
    .qdepth_lo             = 0.5f,
};

static uint32_t g_last_adjust_ms[NUM_JOBS] = {0};
static float ema_qdepth = 0.0f;           // EMA คิวเฉลี่ยของระบบ
static const float ema_q_alpha = 0.3f;    // ค่ากรอง

static uint32_t millis(void){ return (uint32_t)(now_us()/1000ULL); }

static void adaptive_task(void *arg)
{
    ESP_LOGI(TAG, "Adaptive controller started.");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000)); // รอบควบคุมทุก 2s

        // --- 1) snapshot metrics และ clear ช่วง ---
        float total_util = 0.0f;
        for (int i=0;i<NUM_JOBS;i++){
            job_desc_t *jb = &g_jobs[i];
            jb->snap_releases        = jb->releases;
            jb->snap_completions     = jb->completions;
            jb->snap_deadline_miss   = jb->deadline_miss;
            jb->snap_sum_response_ms = jb->sum_response_ms;
            jb->snap_max_response_ms = jb->max_response_ms;

            // เคลียร์สะสมเพื่อทำรอบถัดไป (window แบบเคลื่อนที่ 2s)
            jb->releases = 0;
            jb->completions = 0;
            jb->deadline_miss = 0;
            jb->sum_response_ms = 0;
            jb->max_response_ms = 0;

            total_util += (float)jb->wcet_ms / (float)jb->period_ms * 100.0f;
        }

        // --- 2) queue depth EMA (จากทุก worker) ---
        float qdepth_inst = 0.0f;
        for (int w=0; w<NUM_WORKERS; w++){
            qdepth_inst += (float)uxQueueMessagesWaiting(q_worker[w]);
        }
        qdepth_inst /= (float)NUM_WORKERS;
        ema_qdepth = ema_q_alpha*qdepth_inst + (1.0f-ema_q_alpha)*ema_qdepth;

        // --- 3) ตัดสินใจต่อ job ---
        for (int i=0;i<NUM_JOBS;i++){
            job_desc_t *jb = &g_jobs[i];
            uint32_t now_ms = millis();
            if (now_ms - g_last_adjust_ms[i] < g_adapt_cfg.cooldown_ms) {
                continue; // เคารพ cooldown
            }

            float miss_rate_pct = 0.0f;
            if (jb->snap_releases > 0) {
                miss_rate_pct = (float)jb->snap_deadline_miss * 100.0f / (float)jb->snap_releases;
            }
            float avg_resp = (jb->snap_completions>0) ? (float)jb->snap_sum_response_ms/(float)jb->snap_completions : 0.0f;
            float resp_ratio = (jb->deadline_ms>0) ? (avg_resp / (float)jb->deadline_ms) : 0.0f;

            bool should_relax =
                (miss_rate_pct > g_adapt_cfg.miss_hi_pct) ||
                (resp_ratio > g_adapt_cfg.resp_hi_ratio)   ||
                (ema_qdepth > g_adapt_cfg.qdepth_hi)       ||
                (total_util > g_adapt_cfg.target_total_util_pct + 3.0f);

            bool should_tighten =
                (miss_rate_pct == 0.0f) &&
                (resp_ratio < g_adapt_cfg.resp_lo_ratio)   &&
                (ema_qdepth < g_adapt_cfg.qdepth_lo)       &&
                (total_util < g_adapt_cfg.low_total_util_pct);

            uint32_t oldP = jb->period_ms;
            uint32_t newP = oldP;

            if (should_relax) {
                // เพิ่ม period (ลดโหลด)
                float p = (float)oldP * (1.0f + g_adapt_cfg.step_up_pct);
                if (p > (float)jb->max_period_ms) p = (float)jb->max_period_ms;
                newP = (uint32_t)(p + 0.5f);
            } else if (should_tighten) {
                // ลด period (เพิ่ม performance)
                float p = (float)oldP * (1.0f - g_adapt_cfg.step_down_pct);
                if (p < (float)jb->min_period_ms) p = (float)jb->min_period_ms;
                newP = (uint32_t)(p + 0.5f);
            }

            if (newP != oldP) {
                // ปรับอย่างปลอดภัย: เขียน 32-bit เดี่ยว (atomic บน Xtensa)
                jb->period_ms = newP;
                g_last_adjust_ms[i] = now_ms;

                ESP_LOGW(TAG,
                    "ADAPT %s: period %u -> %u ms | miss=%.2f%% resp=%.1f/ddl(%u) qdepth≈%.1f utilTot=%.1f%%",
                    jb->name, oldP, newP, miss_rate_pct, avg_resp, jb->deadline_ms, ema_qdepth, total_util);
            }
        }
    }
}

// ====== Optional: Load Generator (สร้างโหลดแบบเป็นช่วง) ======
static void load_gen_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        ESP_LOGW(TAG, "LOAD: temporary increase wcet for job B/C");
        g_jobs[JOB_B].wcet_ms = 35;
        g_jobs[JOB_C].wcet_ms = 90;
        vTaskDelay(pdMS_TO_TICKS(8000));
        ESP_LOGW(TAG, "LOAD: restore wcet");
        g_jobs[JOB_B].wcet_ms = 20;
        g_jobs[JOB_C].wcet_ms = 60;
    }
}

// ====== app_main ======
void app_main(void)
{
    ESP_LOGI(TAG, "Adaptive Scheduler demo starting...");

    // GPIO
    gpio_set_direction(LED_OK, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_MISS, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_SCHED, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_OK, 0);
    gpio_set_level(LED_MISS, 0);
    gpio_set_level(LED_SCHED, 0);

    // Queues
    for (int i=0;i<NUM_WORKERS;i++){
        q_worker[i] = xQueueCreate(WORKER_QUEUE_LEN, sizeof(worker_cmd_t));
    }
    q_complete = xQueueCreate(COMPLETE_QUEUE_LEN, sizeof(completion_t));

    // Workers
    xTaskCreatePinnedToCore(worker_task, "worker0", WORKER_STACK, (void*)0, 4, &h_worker[0], 0);
    xTaskCreatePinnedToCore(worker_task, "worker1", WORKER_STACK, (void*)1, 4, &h_worker[1], 1);

    // Scheduler
    xTaskCreatePinnedToCore(scheduler_task, "scheduler", SCHED_STACK, NULL, 5, NULL, 0);

    // Monitor
    xTaskCreate(monitor_task, "monitor", MON_STACK, NULL, 3, NULL);

    // Adaptive Controller
    xTaskCreate(adaptive_task, "adaptive", ADAPT_STACK, NULL, 3, NULL);

    // Optional load gen
    xTaskCreate(load_gen_task, "loadgen", LOAD_STACK, NULL, 2, NULL);

    ESP_LOGI(TAG, "Setup complete.");
}
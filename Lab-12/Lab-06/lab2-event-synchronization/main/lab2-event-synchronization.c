// main.c - Event Synchronization + Fault-Tolerant Synchronization (ESP-IDF v5.x)

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_system.h"

#include "driver/gpio.h"

static const char *TAG = "EVENT_SYNC";

// ======================= GPIO INDICATORS =======================
#define LED_BARRIER_SYNC    GPIO_NUM_2    // Barrier synchronization indicator
#define LED_PIPELINE_STAGE1 GPIO_NUM_4    // Pipeline stage 1
#define LED_PIPELINE_STAGE2 GPIO_NUM_5    // Pipeline stage 2
#define LED_PIPELINE_STAGE3 GPIO_NUM_18   // Pipeline stage 3
#define LED_WORKFLOW_ACTIVE GPIO_NUM_19   // Workflow active

// ======================= EVENT GROUPS =======================
EventGroupHandle_t barrier_events;
EventGroupHandle_t pipeline_events;
EventGroupHandle_t workflow_events;

// ---- Barrier Synchronization bits (4 workers: 0..3) ----
#define WORKER_A_READY_BIT  (1 << 0)
#define WORKER_B_READY_BIT  (1 << 1)
#define WORKER_C_READY_BIT  (1 << 2)
#define WORKER_D_READY_BIT  (1 << 3)
#define ALL_WORKERS_READY   (WORKER_A_READY_BIT | WORKER_B_READY_BIT | WORKER_C_READY_BIT | WORKER_D_READY_BIT)

// ---- Pipeline Processing bits ----
#define STAGE1_COMPLETE_BIT (1 << 0)
#define STAGE2_COMPLETE_BIT (1 << 1)
#define STAGE3_COMPLETE_BIT (1 << 2)
#define STAGE4_COMPLETE_BIT (1 << 3)
#define DATA_AVAILABLE_BIT  (1 << 4)
#define PIPELINE_RESET_BIT  (1 << 5)
#define SYSTEM_DEGRADED_BIT (1 << 6)  // NEW: degraded mode flag

// ---- Workflow Management bits ----
#define WORKFLOW_START_BIT  (1 << 0)
#define APPROVAL_READY_BIT  (1 << 1)
#define RESOURCES_FREE_BIT  (1 << 2)
#define QUALITY_OK_BIT      (1 << 3)
#define WORKFLOW_DONE_BIT   (1 << 4)

// ======================= DATA STRUCTURES =======================
typedef struct {
    uint32_t worker_id;
    uint32_t cycle_number;
    uint32_t work_duration;
    uint64_t timestamp;
} worker_data_t;

typedef struct {
    uint32_t pipeline_id;
    uint32_t stage;
    float    processing_data[4];
    uint32_t quality_score;
    uint64_t stage_timestamps[4];
} pipeline_data_t;

typedef struct {
    uint32_t workflow_id;
    char     description[32];
    uint32_t priority;
    uint32_t estimated_duration;
    bool     requires_approval;
} workflow_item_t;

// ======================= QUEUES =======================
QueueHandle_t pipeline_queue;
QueueHandle_t workflow_queue;

// ======================= STATS =======================
typedef struct {
    uint32_t barrier_cycles;
    uint32_t pipeline_completions;
    uint32_t workflow_completions;
    uint32_t synchronization_time_max;
    uint32_t synchronization_time_avg;
    uint64_t total_processing_time; // us sum
} sync_stats_t;

static sync_stats_t stats = {0};

// ======================= FAULT-TOLERANCE CONFIG =======================
#define WORKER_COUNT                4
#define REQUIRED_BARRIER_QUORUM     3      // ผ่าน barrier เมื่อได้อย่างน้อย 3/4
#define HEARTBEAT_PERIOD_MS         500
#define HEARTBEAT_TIMEOUT_MS        3000
#define MAX_CONSECUTIVE_MISSES      2
#define RESTART_COOLDOWN_MS         2000
#define FAILURE_INJECT_PROB_PCT     10     // 0 เพื่อปิด fault injection

typedef struct {
    TaskHandle_t handle;
    uint32_t     id;
    volatile uint32_t last_hb_ms;
    volatile uint8_t  miss_count;
    volatile bool     alive;
    volatile bool     restarting;
} worker_health_t;

static worker_health_t g_workers[WORKER_COUNT];
static volatile uint8_t g_alive_workers = WORKER_COUNT;

static inline uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static inline void heartbeat_touch(uint32_t worker_id) {
    if (worker_id < WORKER_COUNT) {
        g_workers[worker_id].last_hb_ms = now_ms();
    }
}

// ======================= FORWARD DECLARATIONS =======================
static void restart_worker(uint32_t id);
static void supervisor_task(void *pv);
void barrier_worker_task(void *pvParameters);
void pipeline_stage_task(void *pvParameters);
void pipeline_data_generator_task(void *pvParameters);
void workflow_manager_task(void *pvParameters);
void approval_task(void *pvParameters);
void resource_manager_task(void *pvParameters);
void workflow_generator_task(void *pvParameters);
void statistics_monitor_task(void *pvParameters);

// ======================= QUORUM WAIT UTIL =======================
static bool eventgroup_quorum_wait(EventGroupHandle_t grp,
                                   EventBits_t mask,
                                   uint32_t quorum,
                                   TickType_t timeout_ticks)
{
    uint32_t start = now_ms();
    while ((now_ms() - start) < (timeout_ticks * portTICK_PERIOD_MS)) {
        EventBits_t got = xEventGroupGetBits(grp);
        EventBits_t m = (got & mask);
        uint32_t cnt = 0;
        for (uint32_t b = 0; b < 32; ++b) {
            if (m & (1u << b)) cnt++;
        }
        if (cnt >= quorum) {
            // ล้างบิตที่อ่านมาแล้ว เพื่อลดสะสม
            xEventGroupClearBits(grp, m);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

// ======================= WORKER (FAULT-TOLERANT) =======================
void barrier_worker_task(void *pvParameters) {
    uint32_t worker_id = (uint32_t)pvParameters;
    EventBits_t my_ready_bit = (1 << worker_id);
    uint32_t cycle = 0;

    // init health
    g_workers[worker_id].id         = worker_id;
    g_workers[worker_id].handle     = xTaskGetCurrentTaskHandle();
    g_workers[worker_id].alive      = true;
    g_workers[worker_id].restarting = false;
    g_workers[worker_id].miss_count = 0;
    g_workers[worker_id].last_hb_ms = now_ms();

    ESP_LOGI(TAG, "🏃 FT Barrier Worker %lu started", worker_id);

    while (1) {
        cycle++;

        // heartbeat
        heartbeat_touch(worker_id);

        // independent work
        uint32_t work_duration = 800 + (esp_random() % 2500);
        ESP_LOGI(TAG, "👷 Worker %lu: Cycle %lu independent (%lums)", worker_id, cycle, work_duration);
        vTaskDelay(pdMS_TO_TICKS(work_duration));

#if FAILURE_INJECT_PROB_PCT > 0
        // fault injection for testing
        if ((esp_random() % 100) < FAILURE_INJECT_PROB_PCT) {
            ESP_LOGE(TAG, "🧪 Worker %lu: simulating failure (no heartbeat)", worker_id);
            vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_TIMEOUT_MS + 1000));
            continue; // รอบนี้ถือว่าตายชั่วคราว
        }
#endif

        // ready for barrier
        ESP_LOGI(TAG, "🚧 Worker %lu: ready for barrier (cycle %lu)", worker_id, cycle);
        xEventGroupSetBits(barrier_events, my_ready_bit);

        // quorum barrier wait
        uint64_t t0 = esp_timer_get_time();
        bool ok = eventgroup_quorum_wait(
            barrier_events,
            ALL_WORKERS_READY,
            REQUIRED_BARRIER_QUORUM,
            pdMS_TO_TICKS(10000)
        );
        uint32_t waited_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);

        if (ok) {
            ESP_LOGI(TAG, "🎯 Worker %lu: QUORUM barrier passed (wait=%lums)", worker_id, waited_ms);

            if (waited_ms > stats.synchronization_time_max) {
                stats.synchronization_time_max = waited_ms;
            }
            stats.synchronization_time_avg = (stats.synchronization_time_avg + waited_ms) / 2;

            if (worker_id == 0) {
                stats.barrier_cycles++;
                gpio_set_level(LED_BARRIER_SYNC, 1);
                vTaskDelay(pdMS_TO_TICKS(150));
                gpio_set_level(LED_BARRIER_SYNC, 0);
            }

            // synced work
            vTaskDelay(pdMS_TO_TICKS(300 + (esp_random() % 500)));
        } else {
            ESP_LOGW(TAG, "⏰ Worker %lu: QUORUM barrier timeout (wait=%lums)", worker_id, waited_ms);
        }

        // cooldown + heartbeat ticks
        for (int i = 0; i < 4; ++i) {
            heartbeat_touch(worker_id);
            vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS));
        }
    }
}

// ======================= SUPERVISOR =======================
static void restart_worker(uint32_t id) {
    if (id >= WORKER_COUNT) return;
    worker_health_t *w = &g_workers[id];
    if (w->restarting) return;

    w->restarting = true;
    ESP_LOGW(TAG, "♻️  Supervisor: restarting worker %lu", id);

    if (w->handle) {
        vTaskDelete(w->handle);
        w->handle = NULL;
    }
    vTaskDelay(pdMS_TO_TICKS(RESTART_COOLDOWN_MS));

    char task_name[16];
    sprintf(task_name, "BarrierWork%lu", id);
    xTaskCreate(barrier_worker_task, task_name, 2048, (void*)id, 5, &w->handle);

    w->miss_count = 0;
    w->last_hb_ms = now_ms();
    w->alive      = true;
    w->restarting = false;

    ESP_LOGI(TAG, "✅ Supervisor: worker %lu is back", id);
}

static void supervisor_task(void *pv) {
    ESP_LOGI(TAG, "🩺 Supervisor started (fault-tolerance on)");
    for (;;) {
        uint8_t alive_now = 0;
        uint32_t t = now_ms();

        for (uint32_t i = 0; i < WORKER_COUNT; ++i) {
            worker_health_t *w = &g_workers[i];
            if (!w->handle) continue;

            if ((t - w->last_hb_ms) > HEARTBEAT_TIMEOUT_MS) {
                if (w->miss_count < 255) w->miss_count++;
            } else if (w->miss_count > 0) {
                w->miss_count = 0;
            }

            if (w->miss_count > MAX_CONSECUTIVE_MISSES) {
                if (w->alive) {
                    ESP_LOGE(TAG, "💥 Worker %lu considered FAILED (miss=%u)", i, w->miss_count);
                    w->alive = false;
                    if (g_alive_workers > 0) g_alive_workers--;
                    if (g_alive_workers < REQUIRED_BARRIER_QUORUM) {
                        ESP_LOGW(TAG, "⚠️ System entering DEGRADED mode (alive=%u)", g_alive_workers);
                        xEventGroupSetBits(pipeline_events, SYSTEM_DEGRADED_BIT);
                    }
                }
                restart_worker(i);
            } else {
                if (w->alive) alive_now++;
            }
        }

        if (alive_now >= REQUIRED_BARRIER_QUORUM) {
            if (g_alive_workers < alive_now) g_alive_workers = alive_now;
            if (xEventGroupGetBits(pipeline_events) & SYSTEM_DEGRADED_BIT) {
                xEventGroupClearBits(pipeline_events, SYSTEM_DEGRADED_BIT);
                ESP_LOGI(TAG, "🟢 System recovered from DEGRADED (alive=%u)", alive_now);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// ======================= PIPELINE TASKS =======================
void pipeline_stage_task(void *pvParameters) {
    uint32_t stage_id = (uint32_t)pvParameters;
    EventBits_t stage_complete_bit = (1 << stage_id);
    EventBits_t prev_stage_bit = (stage_id > 0) ? (1 << (stage_id - 1)) : DATA_AVAILABLE_BIT;

    const char* stage_names[] = {"Input", "Processing", "Filtering", "Output"};
    gpio_num_t stage_leds[] = {LED_PIPELINE_STAGE1, LED_PIPELINE_STAGE2, LED_PIPELINE_STAGE3, LED_WORKFLOW_ACTIVE};

    ESP_LOGI(TAG, "🏭 Pipeline Stage %lu (%s) started", stage_id, stage_names[stage_id]);

    while (1) {
        ESP_LOGI(TAG, "⏳ Stage %lu: waiting for input...", stage_id);
        EventBits_t bits = xEventGroupWaitBits(
            pipeline_events,
            prev_stage_bit,
            pdTRUE,
            pdTRUE,
            portMAX_DELAY
        );

        if (bits & prev_stage_bit) {
            gpio_set_level(stage_leds[stage_id], 1);

            pipeline_data_t pipeline_data;
            if (xQueueReceive(pipeline_queue, &pipeline_data, pdMS_TO_TICKS(100)) == pdTRUE) {
                ESP_LOGI(TAG, "📦 Stage %lu: pipeline ID %lu", stage_id, pipeline_data.pipeline_id);

                // degraded mode?
                EventBits_t sys = xEventGroupGetBits(pipeline_events);
                bool degraded = (sys & SYSTEM_DEGRADED_BIT);
                if (degraded) {
                    ESP_LOGW(TAG, "⚠️ Stage %lu running in DEGRADED mode", stage_id);
                }

                pipeline_data.stage_timestamps[stage_id] = esp_timer_get_time();
                pipeline_data.stage = stage_id;

                // base processing time
                uint32_t processing_time = 500 + (esp_random() % 1000);
                if (degraded) {
                    // ลดงานลง
                    processing_time = processing_time / 2;
                }

                switch (stage_id) {
                    case 0: // Input
                        ESP_LOGI(TAG, "📥 Stage %lu: input & validation", stage_id);
                        for (int i = 0; i < 4; i++) {
                            pipeline_data.processing_data[i] = (esp_random() % 1000) / 10.0f;
                        }
                        pipeline_data.quality_score = 70 + (esp_random() % 30);
                        break;

                    case 1: // Processing
                        ESP_LOGI(TAG, "⚙️ Stage %lu: transform", stage_id);
                        for (int i = 0; i < 4; i++) {
                            float mul = degraded ? 1.05f : 1.10f;
                            pipeline_data.processing_data[i] *= mul;
                        }
                        pipeline_data.quality_score += (esp_random() % 20) - 10;
                        break;

                    case 2: // Filtering
                        ESP_LOGI(TAG, "🔍 Stage %lu: filtering & validation", stage_id);
                        {
                            float avg = 0;
                            for (int i = 0; i < 4; i++) avg += pipeline_data.processing_data[i];
                            avg /= 4.0f;
                            ESP_LOGI(TAG, "Avg=%.2f, Quality=%lu", avg, pipeline_data.quality_score);
                        }
                        break;

                    case 3: // Output
                        ESP_LOGI(TAG, "📤 Stage %lu: output", stage_id);
                        stats.pipeline_completions++;
                        {
                            uint64_t total_time_us = esp_timer_get_time() - pipeline_data.stage_timestamps[0];
                            stats.total_processing_time += total_time_us;
                            ESP_LOGI(TAG, "✅ Pipeline %lu done in %llu ms (Q=%lu)",
                                     pipeline_data.pipeline_id, total_time_us / 1000ULL, pipeline_data.quality_score);
                        }
                        break;
                }

                vTaskDelay(pdMS_TO_TICKS(processing_time));

                if (stage_id < 3) {
                    if (xQueueSend(pipeline_queue, &pipeline_data, pdMS_TO_TICKS(100)) == pdTRUE) {
                        xEventGroupSetBits(pipeline_events, stage_complete_bit);
                        ESP_LOGI(TAG, "➡️ Stage %lu: pass to next", stage_id);
                    } else {
                        ESP_LOGW(TAG, "⚠️ Stage %lu: queue full, data lost", stage_id);
                    }
                }
            } else {
                ESP_LOGW(TAG, "⚠️ Stage %lu: no data", stage_id);
            }

            gpio_set_level(stage_leds[stage_id], 0);
        }

        // pipeline reset?
        EventBits_t reset_bits = xEventGroupGetBits(pipeline_events);
        if (reset_bits & PIPELINE_RESET_BIT) {
            ESP_LOGI(TAG, "🔄 Stage %lu: pipeline reset", stage_id);
            xEventGroupClearBits(pipeline_events, PIPELINE_RESET_BIT);
            pipeline_data_t dummy;
            while (xQueueReceive(pipeline_queue, &dummy, 0) == pdTRUE) { /* drain */ }
        }
    }
}

void pipeline_data_generator_task(void *pvParameters) {
    uint32_t pipeline_id = 0;
    ESP_LOGI(TAG, "🏭 Pipeline data generator started");

    while (1) {
        pipeline_data_t data = {0};
        data.pipeline_id = ++pipeline_id;
        data.stage       = 0;
        data.stage_timestamps[0] = esp_timer_get_time();

        ESP_LOGI(TAG, "🚀 Generate pipeline data ID: %lu", pipeline_id);

        if (xQueueSend(pipeline_queue, &data, pdMS_TO_TICKS(1000)) == pdTRUE) {
            xEventGroupSetBits(pipeline_events, DATA_AVAILABLE_BIT);
            ESP_LOGI(TAG, "✅ Pipeline data %lu injected", pipeline_id);
        } else {
            ESP_LOGW(TAG, "⚠️ Pipeline queue full, drop %lu", pipeline_id);
        }

        uint32_t interval = 3000 + (esp_random() % 4000);
        vTaskDelay(pdMS_TO_TICKS(interval));
    }
}

// ======================= WORKFLOW TASKS =======================
void workflow_manager_task(void *pvParameters) {
    ESP_LOGI(TAG, "📋 Workflow manager started");

    while (1) {
        workflow_item_t workflow;

        if (xQueueReceive(workflow_queue, &workflow, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "📝 New workflow: ID %lu - %s (P%lu)",
                     workflow.workflow_id, workflow.description, workflow.priority);

            xEventGroupSetBits(workflow_events, WORKFLOW_START_BIT);
            gpio_set_level(LED_WORKFLOW_ACTIVE, 1);

            EventBits_t required = RESOURCES_FREE_BIT;
            if (workflow.requires_approval) {
                required |= APPROVAL_READY_BIT;
                ESP_LOGI(TAG, "📋 Workflow %lu requires approval", workflow.workflow_id);
            }

            ESP_LOGI(TAG, "⏳ Waiting requirements (0x%08X)...", required);
            EventBits_t bits = xEventGroupWaitBits(
                workflow_events,
                required,
                pdFALSE,
                pdTRUE,
                pdMS_TO_TICKS(workflow.estimated_duration * 2)
            );

            if ((bits & required) == required) {
                ESP_LOGI(TAG, "✅ Workflow %lu: requirements met", workflow.workflow_id);
                uint32_t exec_ms = workflow.estimated_duration + (esp_random() % 1000);
                ESP_LOGI(TAG, "⚙️ Executing workflow %lu (%lums)", workflow.workflow_id, exec_ms);
                vTaskDelay(pdMS_TO_TICKS(exec_ms));

                uint32_t quality = 60 + (esp_random() % 40);
                if (quality > 80) {
                    xEventGroupSetBits(workflow_events, QUALITY_OK_BIT);
                    ESP_LOGI(TAG, "✅ Workflow %lu OK (Quality %lu%%)", workflow.workflow_id, quality);
                    xEventGroupSetBits(workflow_events, WORKFLOW_DONE_BIT);
                    stats.workflow_completions++;
                } else {
                    ESP_LOGW(TAG, "⚠️ Workflow %lu quality fail (%lu%%) -> retry", workflow.workflow_id, quality);
                    if (xQueueSend(workflow_queue, &workflow, 0) != pdTRUE) {
                        ESP_LOGE(TAG, "❌ Re-queue workflow %lu failed", workflow.workflow_id);
                    }
                }
            } else {
                ESP_LOGW(TAG, "⏰ Workflow %lu timeout: requirements not met", workflow.workflow_id);
            }

            gpio_set_level(LED_WORKFLOW_ACTIVE, 0);
            xEventGroupClearBits(workflow_events, WORKFLOW_START_BIT | WORKFLOW_DONE_BIT | QUALITY_OK_BIT);
        }
    }
}

void approval_task(void *pvParameters) {
    ESP_LOGI(TAG, "👨‍💼 Approval task started");
    while (1) {
        xEventGroupWaitBits(workflow_events, WORKFLOW_START_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        ESP_LOGI(TAG, "📋 Approval process started...");

        uint32_t approval_time = 1000 + (esp_random() % 2000);
        vTaskDelay(pdMS_TO_TICKS(approval_time));

        bool approved = (esp_random() % 100) > 20;
        if (approved) {
            ESP_LOGI(TAG, "✅ Approval granted (%lums)", approval_time);
            xEventGroupSetBits(workflow_events, APPROVAL_READY_BIT);
        } else {
            ESP_LOGW(TAG, "❌ Approval denied");
            xEventGroupClearBits(workflow_events, APPROVAL_READY_BIT);
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
        xEventGroupClearBits(workflow_events, APPROVAL_READY_BIT);
    }
}

void resource_manager_task(void *pvParameters) {
    ESP_LOGI(TAG, "🏗️ Resource manager started");
    bool resources_available = true;

    while (1) {
        if (resources_available) {
            xEventGroupSetBits(workflow_events, RESOURCES_FREE_BIT);
            ESP_LOGI(TAG, "🟢 Resources available");

            uint32_t usage_time = 2000 + (esp_random() % 8000);
            vTaskDelay(pdMS_TO_TICKS(usage_time));

            if ((esp_random() % 100) > 70) {
                resources_available = false;
                xEventGroupClearBits(workflow_events, RESOURCES_FREE_BIT);
                ESP_LOGI(TAG, "🔴 Resources temporarily unavailable");
            }
        } else {
            ESP_LOGI(TAG, "⏳ Waiting resources recovery...");
            uint32_t recovery_time = 3000 + (esp_random() % 5000);
            vTaskDelay(pdMS_TO_TICKS(recovery_time));
            resources_available = true;
            ESP_LOGI(TAG, "🟢 Resources recovered");
        }
    }
}

void workflow_generator_task(void *pvParameters) {
    uint32_t workflow_counter = 0;
    ESP_LOGI(TAG, "📋 Workflow generator started");

    const char* workflow_types[] = {
        "Data Processing", "Report Generation", "System Backup",
        "Quality Analysis", "Performance Test", "Security Scan"
    };

    while (1) {
        workflow_item_t wf = {0};
        wf.workflow_id = ++workflow_counter;
        wf.priority = 1 + (esp_random() % 5);
        wf.estimated_duration = 2000 + (esp_random() % 4000);
        wf.requires_approval = (esp_random() % 100) > 60;
        strcpy(wf.description, workflow_types[esp_random() % 6]);

        ESP_LOGI(TAG, "🚀 New workflow: %s (ID=%lu, P=%lu, %s)",
                 wf.description, wf.workflow_id, wf.priority,
                 wf.requires_approval ? "Approval" : "No-Approval");

        if (xQueueSend(workflow_queue, &wf, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "⚠️ Workflow queue full, drop %lu", wf.workflow_id);
        }

        uint32_t interval = 4000 + (esp_random() % 6000);
        vTaskDelay(pdMS_TO_TICKS(interval));
    }
}

// ======================= STATISTICS MONITOR =======================
void statistics_monitor_task(void *pvParameters) {
    ESP_LOGI(TAG, "📊 Statistics monitor started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15000));

        ESP_LOGI(TAG, "\n📈 ═══ SYNCHRONIZATION STATISTICS ═══");
        ESP_LOGI(TAG, "Barrier cycles:        %lu", stats.barrier_cycles);
        ESP_LOGI(TAG, "Pipeline completions:  %lu", stats.pipeline_completions);
        ESP_LOGI(TAG, "Workflow completions:  %lu", stats.workflow_completions);
        ESP_LOGI(TAG, "Max sync time:         %lu ms", stats.synchronization_time_max);
        ESP_LOGI(TAG, "Avg sync time:         %lu ms", stats.synchronization_time_avg);

        if (stats.pipeline_completions > 0) {
            uint32_t avg_pipeline_time_ms = (uint32_t)((stats.total_processing_time / 1000ULL) / stats.pipeline_completions);
            ESP_LOGI(TAG, "Avg pipeline time:     %lu ms", avg_pipeline_time_ms);
        }

        ESP_LOGI(TAG, "Free heap:             %d bytes", esp_get_free_heap_size());
        ESP_LOGI(TAG, "System uptime:         %llu ms", esp_timer_get_time() / 1000ULL);
        ESP_LOGI(TAG, "═══════════════════════════════════════\n");

        ESP_LOGI(TAG, "📊 Event Group Status:");
        ESP_LOGI(TAG, "  Barrier events:   0x%08X", xEventGroupGetBits(barrier_events));
        ESP_LOGI(TAG, "  Pipeline events:  0x%08X", xEventGroupGetBits(pipeline_events));
        ESP_LOGI(TAG, "  Workflow events:  0x%08X", xEventGroupGetBits(workflow_events));
    }
}

// ======================= APP MAIN =======================
void app_main(void) {
    ESP_LOGI(TAG, "🚀 Event Synchronization Lab + Fault-Tolerance Starting...");

    // GPIO init
    gpio_set_direction(LED_BARRIER_SYNC,    GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_PIPELINE_STAGE1, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_PIPELINE_STAGE2, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_PIPELINE_STAGE3, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_WORKFLOW_ACTIVE, GPIO_MODE_OUTPUT);

    gpio_set_level(LED_BARRIER_SYNC,    0);
    gpio_set_level(LED_PIPELINE_STAGE1, 0);
    gpio_set_level(LED_PIPELINE_STAGE2, 0);
    gpio_set_level(LED_PIPELINE_STAGE3, 0);
    gpio_set_level(LED_WORKFLOW_ACTIVE, 0);

    // Event groups
    barrier_events  = xEventGroupCreate();
    pipeline_events = xEventGroupCreate();
    workflow_events = xEventGroupCreate();
    if (!barrier_events || !pipeline_events || !workflow_events) {
        ESP_LOGE(TAG, "Failed to create event groups!");
        return;
    }

    // Queues
    pipeline_queue = xQueueCreate(5, sizeof(pipeline_data_t));
    workflow_queue = xQueueCreate(8, sizeof(workflow_item_t));
    if (!pipeline_queue || !workflow_queue) {
        ESP_LOGE(TAG, "Failed to create queues!");
        return;
    }

    // Init worker health
    for (uint32_t i = 0; i < WORKER_COUNT; ++i) {
        g_workers[i].handle     = NULL;
        g_workers[i].id         = i;
        g_workers[i].alive      = true;
        g_workers[i].restarting = false;
        g_workers[i].miss_count = 0;
        g_workers[i].last_hb_ms = now_ms();
    }
    g_alive_workers = WORKER_COUNT;

    // Create Barrier workers (fault-tolerant)
    ESP_LOGI(TAG, "Creating fault-tolerant barrier workers...");
    for (uint32_t i = 0; i < WORKER_COUNT; ++i) {
        char task_name[16];
        sprintf(task_name, "BarrierWork%lu", i);
        xTaskCreate(barrier_worker_task, task_name, 2048, (void*)i, 5, &g_workers[i].handle);
    }

    // Create Pipeline tasks
    ESP_LOGI(TAG, "Creating pipeline tasks...");
    for (uint32_t i = 0; i < 4; ++i) {
        char task_name[16];
        sprintf(task_name, "PipeStage%lu", i);
        xTaskCreate(pipeline_stage_task, task_name, 3072, (void*)i, 6, NULL);
    }
    xTaskCreate(pipeline_data_generator_task, "PipeGen", 2048, NULL, 4, NULL);

    // Create Workflow tasks
    ESP_LOGI(TAG, "Creating workflow tasks...");
    xTaskCreate(workflow_manager_task,   "WorkflowMgr", 3072, NULL, 7, NULL);
    xTaskCreate(approval_task,          "Approval",     2048, NULL, 6, NULL);
    xTaskCreate(resource_manager_task,  "ResourceMgr",  2048, NULL, 6, NULL);
    xTaskCreate(workflow_generator_task,"WorkflowGen",  2048, NULL, 4, NULL);

    // Supervisor + Monitor
    xTaskCreate(supervisor_task,        "Supervisor",   3072, NULL, 8, NULL);
    xTaskCreate(statistics_monitor_task,"StatsMon",     3072, NULL, 3, NULL);

    ESP_LOGI(TAG, "\n🎯 LED Indicators:");
    ESP_LOGI(TAG, "  GPIO2  - Barrier Synchronization");
    ESP_LOGI(TAG, "  GPIO4  - Pipeline Stage 1");
    ESP_LOGI(TAG, "  GPIO5  - Pipeline Stage 2");
    ESP_LOGI(TAG, "  GPIO18 - Pipeline Stage 3");
    ESP_LOGI(TAG, "  GPIO19 - Workflow Active");

    ESP_LOGI(TAG, "\n🔄 System Features:");
    ESP_LOGI(TAG, "  • Barrier Synchronization (Quorum %d/%d + Auto-Restart)", REQUIRED_BARRIER_QUORUM, WORKER_COUNT);
    ESP_LOGI(TAG, "  • Pipeline Processing (4 stages + Degraded Mode)");
    ESP_LOGI(TAG, "  • Workflow Management (approval & resources)");
    ESP_LOGI(TAG, "  • Real-time Statistics Monitoring");

    ESP_LOGI(TAG, "Fault-tolerance enabled: HB timeout=%dms, inject=%d%%", HEARTBEAT_TIMEOUT_MS, FAILURE_INJECT_PROB_PCT);
    ESP_LOGI(TAG, "Event Synchronization System operational!");
}
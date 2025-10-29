#include <stdio.h>

void app_main(void)
{

}
// main.c — Lab 2 (Upgraded): Priority + Graceful Shutdown + Performance Monitoring
// ESP-IDF v5.x

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_random.h"
#include "esp_timer.h"   // สำหรับ us timestamp

static const char *TAG = "PROD_CONS_UP";

// ===== Pins ===== (ปรับตามบอร์ด)
#define LED_PRODUCER_1   GPIO_NUM_2
#define LED_PRODUCER_2   GPIO_NUM_4
#define LED_PRODUCER_3   GPIO_NUM_5
#define LED_CONSUMER_1   GPIO_NUM_18
#define LED_CONSUMER_2   GPIO_NUM_19
#define BUTTON_SHUTDOWN  GPIO_NUM_0   // ปุ่มสั่งปิด (active low)

// ===== Queue & Mutex =====
static QueueHandle_t xProductQueue = NULL;
static SemaphoreHandle_t xPrintMutex = NULL;

// ===== Shutdown flag =====
static volatile bool system_shutdown = false;

// ===== สถิติระบบ =====
typedef struct {
    uint32_t produced;
    uint32_t consumed;
    uint32_t dropped;
} stats_t;

static stats_t global_stats = {0, 0, 0};

// ===== Performance Monitoring =====
typedef struct {
    uint64_t total_processing_time_ms; // สะสมเวลา processing จริง (ms)
    uint32_t processed_count;          // จำนวนที่ประมวลผลแล้ว
    uint32_t max_queue_size;           // ขนาดคิวสูงสุดที่เคยถึง
    uint32_t throughput_counter;       // นับจำนวน consumed ใน window ปัจจุบัน
    uint64_t throughput_window_start_us; // เวลาเริ่ม window (us)
    uint32_t throughput_per_minute;    // สรุปต่อ 1 นาที (ตัวล่าสุด)
} performance_t;

static performance_t perf = {0};

// ===== โครงสร้างสินค้า (เพิ่ม priority) =====
typedef struct {
    int      producer_id;
    int      product_id;
    char     product_name[30];
    uint32_t production_time;      // tick ที่ผลิต
    int      processing_time_ms;   // เวลาที่ "ควร" ใช้ในการประมวลผล
    int      priority;             // 0 = normal, 1 = high
} product_t;

// ===== พิมพ์ log แบบกันชน (mutex) =====
static void safe_printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    if (xSemaphoreTake(xPrintMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        vprintf(fmt, args);
        xSemaphoreGive(xPrintMutex);
    }
    va_end(args);
}

// ===== Producer =====
static void producer_task(void *pvParameters) {
    int producer_id = *((int*)pvParameters);
    product_t product;
    int product_counter = 0;
    gpio_num_t led_pin;

    switch (producer_id) {
        case 1: led_pin = LED_PRODUCER_1; break;
        case 2: led_pin = LED_PRODUCER_2; break;
        case 3: led_pin = LED_PRODUCER_3; break;
        default: led_pin = LED_PRODUCER_1; break;
    }

    safe_printf("Producer %d started\n", producer_id);

    while (!system_shutdown) {
        // สร้างสินค้า
        product.producer_id = producer_id;
        product.product_id = product_counter++;
        snprintf(product.product_name, sizeof(product.product_name),
                 "Product-P%d-#%d", producer_id, product.product_id);
        product.production_time = (uint32_t)xTaskGetTickCount();
        product.processing_time_ms = 500 + (esp_random() % 2000); // 0.5–2.5s

        // กำหนด priority: สุ่ม 20% เป็น high
        product.priority = ((esp_random() % 100) < 20) ? 1 : 0;

        // ส่งเข้า queue — high ใช้ SendToFront เพื่อแทรกหน้า
        BaseType_t ok;
        if (product.priority == 1) {
            ok = xQueueSendToFront(xProductQueue, &product, pdMS_TO_TICKS(100));
        } else {
            ok = xQueueSend(xProductQueue, &product, pdMS_TO_TICKS(100));
        }

        if (ok == pdPASS) {
            global_stats.produced++;
            safe_printf("✓ Producer %d: %s (prio:%s, proc:%dms)\n",
                        producer_id, product.product_name,
                        product.priority ? "HIGH" : "norm",
                        product.processing_time_ms);

            // กระพริบไฟ producer
            gpio_set_level(led_pin, 1);
            vTaskDelay(pdMS_TO_TICKS(50));
            gpio_set_level(led_pin, 0);
        } else {
            global_stats.dropped++;
            safe_printf("✗ Producer %d: Queue full! Dropped %s (prio:%s)\n",
                        producer_id, product.product_name,
                        product.priority ? "HIGH" : "norm");
        }

        // delay การผลิตแบบสุ่ม 1–3s
        int delay_ms = 1000 + (esp_random() % 2000);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    safe_printf("[Producer %d] Shutdown acknowledged. Exiting.\n", producer_id);
    vTaskDelete(NULL);
}

// ===== Consumer =====
static void consumer_task(void *pvParameters) {
    int consumer_id = *((int*)pvParameters);
    product_t product;
    gpio_num_t led_pin = (consumer_id == 1) ? LED_CONSUMER_1 : LED_CONSUMER_2;

    safe_printf("Consumer %d started\n", consumer_id);

    while (1) {
        // ถ้า shutdown แล้วและคิวว่าง → จบงาน
        if (system_shutdown && uxQueueMessagesWaiting(xProductQueue) == 0) {
            break;
        }

        BaseType_t ok = xQueueReceive(xProductQueue, &product, pdMS_TO_TICKS(500));
        if (ok == pdPASS) {
            // วัดเวลา queue time (ดูเฉย ๆ)
            uint32_t queue_ticks = (uint32_t)xTaskGetTickCount() - product.production_time;
            uint32_t queue_ms = queue_ticks * portTICK_PERIOD_MS;

            // เริ่มจับเวลาประมวลผลจริง
            uint64_t t0 = (uint64_t)esp_timer_get_time(); // us
            safe_printf("→ C%d: %s (prio:%s, queue:%lums)\n",
                        consumer_id, product.product_name,
                        product.priority ? "HIGH" : "norm",
                        (unsigned long)queue_ms);

            gpio_set_level(led_pin, 1);
            vTaskDelay(pdMS_TO_TICKS(product.processing_time_ms)); // จำลองประมวลผล
            gpio_set_level(led_pin, 0);

            uint64_t t1 = (uint64_t)esp_timer_get_time();
            uint64_t proc_ms = (t1 - t0) / 1000ULL;

            global_stats.consumed++;

            // อัพเดท performance
            perf.total_processing_time_ms += proc_ms;
            perf.processed_count += 1;

            // throughput ต่อ 1 นาที (rolling window แบบง่าย)
            uint64_t now_us = (uint64_t)esp_timer_get_time();
            if (perf.throughput_window_start_us == 0) {
                perf.throughput_window_start_us = now_us;
            }
            perf.throughput_counter++;
            if ((now_us - perf.throughput_window_start_us) >= 60ULL * 1000000ULL) {
                // สรุปค่าและรีเซ็ต window
                perf.throughput_per_minute = perf.throughput_counter;
                perf.throughput_counter = 0;
                perf.throughput_window_start_us = now_us;
            }

            safe_printf("✓ C%d: Finished %s (proc_real:%llums)\n",
                        consumer_id, product.product_name,
                        (unsigned long long)proc_ms);
        } else {
            // timeout — ว่าง หรือ shutdown รอ drain คิว
            if (system_shutdown && uxQueueMessagesWaiting(xProductQueue) == 0) {
                break;
            }
        }

        // อัพเดท max queue size
        UBaseType_t qi = uxQueueMessagesWaiting(xProductQueue);
        if (qi > perf.max_queue_size) perf.max_queue_size = qi;
    }

    safe_printf("[Consumer %d] Shutdown acknowledged. Exiting.\n", consumer_id);
    vTaskDelete(NULL);
}

// ===== สรุปสถิติทุก 5s =====
static void statistics_task(void *pvParameters) {
    safe_printf("Statistics task started\n");
    while (!system_shutdown) {
        UBaseType_t q_items = uxQueueMessagesWaiting(xProductQueue);
        if (q_items > perf.max_queue_size) perf.max_queue_size = q_items;

        float eff = (global_stats.produced > 0)
                    ? ((float)global_stats.consumed / (float)global_stats.produced) * 100.0f
                    : 0.0f;

        float avg_proc = (perf.processed_count > 0)
                         ? (float)perf.total_processing_time_ms / (float)perf.processed_count
                         : 0.0f;

        safe_printf("\n═══ SYSTEM STATISTICS ═══\n");
        safe_printf("Produced: %lu | Consumed: %lu | Dropped: %lu\n",
                    (unsigned long)global_stats.produced,
                    (unsigned long)global_stats.consumed,
                    (unsigned long)global_stats.dropped);
        safe_printf("Queue Backlog: %d | Max Queue Size: %u\n",
                    (int)q_items, perf.max_queue_size);
        safe_printf("Efficiency: %.1f%%\n", eff);
        safe_printf("Avg Proc Time: %.1f ms | Throughput/min(last): %u\n",
                    avg_proc, perf.throughput_per_minute);

        // วาด bar ขนาด 10 ช่อง
        printf("Queue: [");
        int filled = (q_items > 10) ? 10 : (int)q_items;
        for (int i = 0; i < 10; i++) putchar(i < filled ? '■' : '□');
        printf("]\n");
        safe_printf("═══════════════════════════\n\n");

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    safe_printf("[Statistics] Shutdown acknowledged. Exiting.\n");
    vTaskDelete(NULL);
}

// ===== Load balancer (แจ้งเตือนเมื่อคิวสูง) =====
static void load_balancer_task(void *pvParameters) {
    const int WARN_THRESHOLD = 8;
    safe_printf("Load balancer started\n");

    while (!system_shutdown) {
        UBaseType_t q_items = uxQueueMessagesWaiting(xProductQueue);
        if (q_items > WARN_THRESHOLD) {
            safe_printf("⚠️  HIGH LOAD DETECTED! Queue size: %d\n", (int)q_items);
            safe_printf("💡 Suggestion: Add more consumers or optimize processing\n");

            // กระพริบไฟทั้งหมดเป็นสัญญาณเตือน
            gpio_set_level(LED_PRODUCER_1, 1);
            gpio_set_level(LED_PRODUCER_2, 1);
            gpio_set_level(LED_PRODUCER_3, 1);
            gpio_set_level(LED_CONSUMER_1, 1);
            gpio_set_level(LED_CONSUMER_2, 1);
            vTaskDelay(pdMS_TO_TICKS(200));
            gpio_set_level(LED_PRODUCER_1, 0);
            gpio_set_level(LED_PRODUCER_2, 0);
            gpio_set_level(LED_PRODUCER_3, 0);
            gpio_set_level(LED_CONSUMER_1, 0);
            gpio_set_level(LED_CONSUMER_2, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    safe_printf("[LoadBalancer] Shutdown acknowledged. Exiting.\n");
    vTaskDelete(NULL);
}

// ===== Watcher ปุ่มสั่งปิดแบบ Graceful =====
static void shutdown_watcher_task(void *pvParameters) {
    safe_printf("Shutdown watcher started (hold BTN to stop)\n");

    // ปุ่มเป็น active-low พร้อม pullup
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BUTTON_SHUTDOWN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io);

    int stable_low_count = 0;
    while (!system_shutdown) {
        int level = gpio_get_level(BUTTON_SHUTDOWN);
        if (level == 0) { // กดค้าง
            stable_low_count++;
            if (stable_low_count >= 30) { // ~3 วินาที (30 * 100ms)
                system_shutdown = true;
                safe_printf("🔻 Shutdown button pressed. Initiating graceful shutdown...\n");
            }
        } else {
            stable_low_count = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // รอให้คิวว่างแล้วประกาศจบระบบ
    while (uxQueueMessagesWaiting(xProductQueue) > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    safe_printf("✅ Queue drained. System halted.\n");
    vTaskDelete(NULL);
}

// ===== app_main =====
void app_main(void) {
    ESP_LOGI(TAG, "Starting Producer-Consumer (Upgraded)");

    // ตั้งค่า GPIO LEDs
    gpio_set_direction(LED_PRODUCER_1, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_PRODUCER_2, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_PRODUCER_3, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_CONSUMER_1, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_CONSUMER_2, GPIO_MODE_OUTPUT);

    // ดับไฟทั้งหมด
    gpio_set_level(LED_PRODUCER_1, 0);
    gpio_set_level(LED_PRODUCER_2, 0);
    gpio_set_level(LED_PRODUCER_3, 0);
    gpio_set_level(LED_CONSUMER_1, 0);
    gpio_set_level(LED_CONSUMER_2, 0);

    // Queue ขนาด 10
    xProductQueue = xQueueCreate(10, sizeof(product_t));
    xPrintMutex   = xSemaphoreCreateMutex();

    if (xProductQueue != NULL && xPrintMutex != NULL) {
        ESP_LOGI(TAG, "Queue and mutex created successfully");

        // IDs
        static int producer1_id = 1, producer2_id = 2, producer3_id = 3;
        static int consumer1_id = 1, consumer2_id = 2;

        // === Producers ===
        xTaskCreate(producer_task, "Producer1", 3072, &producer1_id, 3, NULL);
        xTaskCreate(producer_task, "Producer2", 3072, &producer2_id, 3, NULL);
        xTaskCreate(producer_task, "Producer3", 3072, &producer3_id, 3, NULL);

        // === Consumers === (จะลด/เพิ่มได้ตามการทดลอง)
        xTaskCreate(consumer_task, "Consumer1", 3072, &consumer1_id, 2, NULL);
        xTaskCreate(consumer_task, "Consumer2", 3072, &consumer2_id, 2, NULL);

        // === Monitoring ===
        xTaskCreate(statistics_task, "Statistics",    3072, NULL, 1, NULL);
        xTaskCreate(load_balancer_task, "LoadBalancer",2048, NULL, 1, NULL);

        // === Shutdown watcher ===
        xTaskCreate(shutdown_watcher_task, "Shutdown", 2048, NULL, 1, NULL);

        ESP_LOGI(TAG, "System operational. Hold BTN to stop (≈3s).");
    } else {
        ESP_LOGE(TAG, "Failed to create queue or mutex!");
    }
}
    #include <stdio.h>
    #include <stdint.h>
    #include <string.h>
    #include "freertos/FreeRTOS.h"
    #include "freertos/task.h"
    #include "freertos/timers.h"
    #include "esp_log.h"
    #include "driver/gpio.h"
    #include "esp_random.h"
    #include "esp_timer.h"   // สำหรับวัดเวลา (Performance Analysis)

    static const char *TAG = "SW_TIMERS";

    // === ENABLE CHALLENGES (one at a time) =====================================
    // ตั้งค่าเป็น 1 เฉพาะ "ขั้น" ที่อยากทดสอบ (ครั้งละขั้น)
    // ตัวอย่าง: ทดสอบขั้นที่ 1 → STEP1_SYNC=1, ที่เหลือ=0
    #define STEP1_SYNC       1 // Timer Synchronization
    #define STEP2_PERF       1  // Performance Analysis
    #define STEP3_ERR        1  // Error Handling
    #define STEP4_CHAIN      1  // Complex Scheduling]
    #define STEP5_RESOURCE   1  // Resource Management
    // ===========================================================================

    // LED pins for different timers
    #define LED_BLINK     GPIO_NUM_2      // Fast blink timer
    #define LED_HEARTBEAT GPIO_NUM_4      // Heartbeat timer
    #define LED_STATUS    GPIO_NUM_5      // Status timer
    #define LED_ONESHOT   GPIO_NUM_18     // One-shot timer

    // Timer handles
    TimerHandle_t xBlinkTimer;
    TimerHandle_t xHeartbeatTimer;
    TimerHandle_t xStatusTimer;
    TimerHandle_t xOneShotTimer;
    TimerHandle_t xDynamicTimer;

    #if STEP1_SYNC
    // Step 1: Sync timer ใช้สั่งเริ่ม Blink + Heartbeat พร้อมกัน
    TimerHandle_t xSyncTimer;
    #endif

    #if STEP4_CHAIN
    // Step 4: Chain เริ่มจากตัวจุดชนวน A → B → C
    TimerHandle_t xChainStarter;
    #endif

    #if STEP5_RESOURCE
    // Step 5: Temp timer + task สาธิตสร้าง/ลบ resource
    static void temp_resource_task(void *pvParameters);
    static void temp_oneshot_cb(TimerHandle_t xTimer);
    #endif

    // Timer periods (in milliseconds)
    #define BLINK_PERIOD     500
    #define HEARTBEAT_PERIOD 2000
    #define STATUS_PERIOD    5000
    #define ONESHOT_DELAY    3000

    // Statistics
    typedef struct {
        uint32_t blink_count;
        uint32_t heartbeat_count;
        uint32_t status_count;
        uint32_t oneshot_count;
        uint32_t dynamic_count;
    } timer_stats_t;

    timer_stats_t stats = {0, 0, 0, 0, 0};

    // LED states
    bool led_blink_state = false;
    bool led_heartbeat_state = false;

    #if STEP2_PERF
    // วัดความแม่นยำ/จังหวะจริงของ timer (μs)
    static int64_t last_blink_us     = 0;
    static int64_t last_heartbeat_us = 0;
    static int64_t last_status_us    = 0;
    #endif

    #if STEP3_ERR
    // Wrapper สำหรับตรวจ error ของ FreeRTOS timer APIs
    static BaseType_t TRY_WRAP(BaseType_t rc, const char *msg) {
        if (rc != pdPASS) {
            ESP_LOGE(TAG, "❌ Timer error: %s", msg);
        }
        return rc;
    }
    #define TRY(call, msg) TRY_WRAP((call), (msg))
    #else
    #define TRY(call, msg) (call)
    #endif

    // ===== Forward declarations
    static void dynamic_timer_callback(TimerHandle_t xTimer);
    // ==========================

    // Blink timer callback (auto-reload)
    void blink_timer_callback(TimerHandle_t xTimer) {

    #if STEP2_PERF
        int64_t now = esp_timer_get_time();
        if (last_blink_us > 0) {
            ESP_LOGI(TAG, "⏲️ Blink interval actual: %lld ms", (now - last_blink_us) / 1000);
        }
        last_blink_us = now;
    #endif

    #if STEP4_CHAIN
        // Chain: เมื่อ Blink เกิดครั้งแรก → start Heartbeat
        if (stats.blink_count == 0) {
            ESP_LOGI(TAG, "Chain: A (Blink) first tick → start B (Heartbeat)");
            TRY(xTimerStart(xHeartbeatTimer, 0), "Start Heartbeat (chain)");
        }
    #endif

        stats.blink_count++;

        // Toggle LED state
        led_blink_state = !led_blink_state;
        gpio_set_level(LED_BLINK, led_blink_state);

        ESP_LOGI(TAG, "💫 Blink Timer: Toggle #%lu (LED: %s)",
                stats.blink_count, led_blink_state ? "ON" : "OFF");

        // Every 20 blinks, create a one-shot timer
        if (stats.blink_count % 20 == 0) {
            ESP_LOGI(TAG, "🚀 Creating one-shot timer (3 second delay)");
            if (TRY(xTimerStart(xOneShotTimer, 0), "Start one-shot timer") != pdPASS) {
                ESP_LOGW(TAG, "Failed to start one-shot timer");
            }
        }
    }

    // Heartbeat timer callback (auto-reload)
    void heartbeat_timer_callback(TimerHandle_t xTimer) {

    #if STEP2_PERF
        int64_t now = esp_timer_get_time();
        if (last_heartbeat_us > 0) {
            ESP_LOGI(TAG, "⏲️ Heartbeat interval actual: %lld ms", (now - last_heartbeat_us) / 1000);
        }
        last_heartbeat_us = now;
    #endif

    #if STEP4_CHAIN
        // Chain: เมื่อ Heartbeat เกิดครั้งแรก → start Status
        if (stats.heartbeat_count == 0) {
            ESP_LOGI(TAG, "Chain: B (Heartbeat) first tick → start C (Status)");
            TRY(xTimerStart(xStatusTimer, 0), "Start Status (chain)");
        }
    #endif

        stats.heartbeat_count++;

        ESP_LOGI(TAG, "💓 Heartbeat Timer: Beat #%lu", stats.heartbeat_count);

        // Double blink for heartbeat
        gpio_set_level(LED_HEARTBEAT, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(LED_HEARTBEAT, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(LED_HEARTBEAT, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(LED_HEARTBEAT, 0);

        // Randomly adjust blink timer period
        if (esp_random() % 4 == 0) { // 25% chance
            uint32_t new_period = 300 + (esp_random() % 400); // 300-700ms
            ESP_LOGI(TAG, "🔧 Adjusting blink period to %lums", new_period);

            if (TRY(xTimerChangePeriod(xBlinkTimer, pdMS_TO_TICKS(new_period), 100),
                    "Change blink timer period") != pdPASS) {
                ESP_LOGW(TAG, "Failed to change blink timer period");
            }
        }
    }

    // Status timer callback (auto-reload)
    void status_timer_callback(TimerHandle_t xTimer) {

    #if STEP2_PERF
        int64_t now = esp_timer_get_time();
        if (last_status_us > 0) {
            ESP_LOGI(TAG, "⏲️ Status interval actual: %lld ms", (now - last_status_us) / 1000);
        }
        last_status_us = now;
    #endif

        stats.status_count++;

        ESP_LOGI(TAG, "📊 Status Timer: Update #%lu", stats.status_count);

        // Flash status LED
        gpio_set_level(LED_STATUS, 1);
        vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level(LED_STATUS, 0);

        // Print system statistics
        ESP_LOGI(TAG, "═══ TIMER STATISTICS ═══");
        ESP_LOGI(TAG, "Blink events:     %lu", stats.blink_count);
        ESP_LOGI(TAG, "Heartbeat events: %lu", stats.heartbeat_count);
        ESP_LOGI(TAG, "Status updates:   %lu", stats.status_count);
        ESP_LOGI(TAG, "One-shot events:  %lu", stats.oneshot_count);
        ESP_LOGI(TAG, "Dynamic events:   %lu", stats.dynamic_count);
        ESP_LOGI(TAG, "═══════════════════════");

        // Show timer states
        ESP_LOGI(TAG, "Timer States:");
        ESP_LOGI(TAG, "  Blink:     %s (Period: %lums)",
                xTimerIsTimerActive(xBlinkTimer) ? "ACTIVE" : "INACTIVE",
                xTimerGetPeriod(xBlinkTimer) * portTICK_PERIOD_MS);
        ESP_LOGI(TAG, "  Heartbeat: %s (Period: %lums)",
                xTimerIsTimerActive(xHeartbeatTimer) ? "ACTIVE" : "INACTIVE",
                xTimerGetPeriod(xHeartbeatTimer) * portTICK_PERIOD_MS);
        ESP_LOGI(TAG, "  Status:    %s (Period: %lums)",
                xTimerIsTimerActive(xStatusTimer) ? "ACTIVE" : "INACTIVE",
                xTimerGetPeriod(xStatusTimer) * portTICK_PERIOD_MS);
        ESP_LOGI(TAG, "  One-shot:  %s",
                xTimerIsTimerActive(xOneShotTimer) ? "ACTIVE" : "INACTIVE");
    }

    // One-shot timer callback
    void oneshot_timer_callback(TimerHandle_t xTimer) {
        stats.oneshot_count++;

        ESP_LOGI(TAG, "⚡ One-shot Timer: Event #%lu", stats.oneshot_count);

        // Flash one-shot LED pattern
        for (int i = 0; i < 5; i++) {
            gpio_set_level(LED_ONESHOT, 1);
            vTaskDelay(pdMS_TO_TICKS(50));
            gpio_set_level(LED_ONESHOT, 0);
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        // Create a dynamic timer with random period
        uint32_t random_period = 1000 + (esp_random() % 3000); // 1-4 seconds
        ESP_LOGI(TAG, "🎲 Creating dynamic timer (period: %lums)", random_period);

        xDynamicTimer = xTimerCreate("DynamicTimer",
                                    pdMS_TO_TICKS(random_period),
                                    pdFALSE, // One-shot
                                    (void*)0,
                                    dynamic_timer_callback);

        if (xDynamicTimer != NULL) {
            if (TRY(xTimerStart(xDynamicTimer, 0), "Start dynamic timer") != pdPASS) {
                ESP_LOGW(TAG, "Failed to start dynamic timer");
            }
        }
    }

    // Dynamic timer callback (created at runtime)
    static void dynamic_timer_callback(TimerHandle_t xTimer) {
        stats.dynamic_count++;

        ESP_LOGI(TAG, "🌟 Dynamic Timer: Event #%lu", stats.dynamic_count);

        // Flash all LEDs briefly
        gpio_set_level(LED_BLINK, 1);
        gpio_set_level(LED_HEARTBEAT, 1);
        gpio_set_level(LED_STATUS, 1);
        gpio_set_level(LED_ONESHOT, 1);

        vTaskDelay(pdMS_TO_TICKS(300));

        gpio_set_level(LED_BLINK, led_blink_state); // Restore blink state
        gpio_set_level(LED_HEARTBEAT, 0);
        gpio_set_level(LED_STATUS, 0);
        gpio_set_level(LED_ONESHOT, 0);

        // Delete the dynamic timer
        if (TRY(xTimerDelete(xTimer, 100), "Delete dynamic timer") != pdPASS) {
            ESP_LOGW(TAG, "Failed to delete dynamic timer");
        } else {
            ESP_LOGI(TAG, "Dynamic timer deleted");
        }

        xDynamicTimer = NULL;
    }

    // Control task for timer management (ของเดิม)
    void timer_control_task(void *pvParameters) {
        ESP_LOGI(TAG, "Timer control task started");

        while (1) {
            vTaskDelay(pdMS_TO_TICKS(15000)); // Every 15 seconds

            ESP_LOGI(TAG, "\n🎛️  TIMER CONTROL: Performing maintenance...");

            // Randomly stop and restart timers for demonstration
            int action = esp_random() % 3;

            switch (action) {
                case 0:
                    ESP_LOGI(TAG, "⏸️  Stopping heartbeat timer for 5 seconds");
                    TRY(xTimerStop(xHeartbeatTimer, 100), "Stop Heartbeat");
                    vTaskDelay(pdMS_TO_TICKS(5000));
                    ESP_LOGI(TAG, "▶️  Restarting heartbeat timer");
                    TRY(xTimerStart(xHeartbeatTimer, 100), "Restart Heartbeat");
                    break;

                case 1:
                    ESP_LOGI(TAG, "🔄 Reset status timer");
                    TRY(xTimerReset(xStatusTimer, 100), "Reset Status");
                    break;

                case 2: {
                    ESP_LOGI(TAG, "⚙️  Changing blink timer period");
                    uint32_t new_period = 200 + (esp_random() % 600); // 200-800ms
                    TRY(xTimerChangePeriod(xBlinkTimer, pdMS_TO_TICKS(new_period), 100),
                        "Change Blink (control)");
                    ESP_LOGI(TAG, "New blink period: %lums", new_period);
                    break;
                }
            }

            ESP_LOGI(TAG, "Maintenance completed\n");
        }
    }

    #if STEP1_SYNC
    // Step 1: Sync callback → start Blink & Heartbeat พร้อมกัน
    static void sync_timer_callback(TimerHandle_t xTimer) {
        ESP_LOGI(TAG, "⏱️ Sync: start Blink + Heartbeat together");
        TRY(xTimerStart(xBlinkTimer, 0),    "Start Blink (sync)");
        TRY(xTimerStart(xHeartbeatTimer, 0),"Start Heartbeat (sync)");
    }
    #endif

    #if STEP4_CHAIN
    // Step 4: จุดชนวน A → เริ่มลำดับ A→B→C
    static void chain_starter_callback(TimerHandle_t xTimer) {
        ESP_LOGI(TAG, "Chain: start A (Blink)");
        TRY(xTimerStart(xBlinkTimer, 0), "Start Blink (chain)");
    }
    #endif

    #if STEP5_RESOURCE
    // Step 5: Temp timer fired
    static void temp_oneshot_cb(TimerHandle_t xTimer) {
        ESP_LOGI(TAG, "⏳ Temp one-shot fired");
    }
    // Step 5: Task สาธิตสร้าง/ลบ timer ป้องกัน resource leak
    static void temp_resource_task(void *pv) {
        ESP_LOGI(TAG, "Resource demo: create temp timer");
        TimerHandle_t t = xTimerCreate("TempOnce", pdMS_TO_TICKS(2000), pdFALSE, NULL, temp_oneshot_cb);
        if (t) {
            TRY(xTimerStart(t, 0), "Start TempOnce");
            vTaskDelay(pdMS_TO_TICKS(2500)); // รอให้ยิง callback เสร็จ
            TRY(xTimerDelete(t, 0), "Delete TempOnce");
            ESP_LOGI(TAG, "🧹 Temp timer deleted (resource freed)");
        }
        vTaskDelete(NULL);
    }
    #endif

    void app_main(void) {
        ESP_LOGI(TAG, "Software Timers Lab Starting...");

        // Configure LED pins
        gpio_set_direction(LED_BLINK, GPIO_MODE_OUTPUT);
        gpio_set_direction(LED_HEARTBEAT, GPIO_MODE_OUTPUT);
        gpio_set_direction(LED_STATUS, GPIO_MODE_OUTPUT);
        gpio_set_direction(LED_ONESHOT, GPIO_MODE_OUTPUT);

        // Turn off all LEDs
        gpio_set_level(LED_BLINK, 0);
        gpio_set_level(LED_HEARTBEAT, 0);
        gpio_set_level(LED_STATUS, 0);
        gpio_set_level(LED_ONESHOT, 0);

        ESP_LOGI(TAG, "Creating software timers...");

        // Create blink timer (auto-reload)
        xBlinkTimer = xTimerCreate("BlinkTimer",
                                pdMS_TO_TICKS(BLINK_PERIOD),
                                pdTRUE,  // Auto-reload
                                (void*)1,
                                blink_timer_callback);

        // Create heartbeat timer (auto-reload)
        xHeartbeatTimer = xTimerCreate("HeartbeatTimer",
                                    pdMS_TO_TICKS(HEARTBEAT_PERIOD),
                                    pdTRUE,  // Auto-reload
                                    (void*)2,
                                    heartbeat_timer_callback);

        // Create status timer (auto-reload)
        xStatusTimer = xTimerCreate("StatusTimer",
                                    pdMS_TO_TICKS(STATUS_PERIOD),
                                    pdTRUE,  // Auto-reload
                                    (void*)3,
                                    status_timer_callback);

        // Create one-shot timer (initially stopped)
        xOneShotTimer = xTimerCreate("OneShotTimer",
                                    pdMS_TO_TICKS(ONESHOT_DELAY),
                                    pdFALSE, // One-shot
                                    (void*)4,
                                    oneshot_timer_callback);

        // Check if all timers were created successfully
        if (xBlinkTimer && xHeartbeatTimer && xStatusTimer && xOneShotTimer) {
            ESP_LOGI(TAG, "All timers created successfully");

            ESP_LOGI(TAG, "Starting timers...");

            // เริ่ม timer ตาม "ขั้น" ที่เลือก
    #if STEP1_SYNC || STEP4_CHAIN
            // ยังไม่เริ่ม Blink/Heartbeat ที่นี่
    #else
            TRY(xTimerStart(xBlinkTimer, 0),     "Start Blink");
            TRY(xTimerStart(xHeartbeatTimer, 0), "Start Heartbeat");
    #endif

    #if STEP4_CHAIN
            // ยังไม่เริ่ม Status ที่นี่ ให้เริ่มจาก chain ใน heartbeat ครั้งแรก
    #else
            TRY(xTimerStart(xStatusTimer, 0),    "Start Status");
    #endif

            // Note: One-shot timer จะถูก start โดย blink timer callback (ทุกๆ 20 blinks)

            // Step 1: ตั้งเวลาซิงค์ให้เริ่ม Blink+Heartbeat พร้อมกันหลัง 3s
    #if STEP1_SYNC
            xSyncTimer = xTimerCreate("Sync", pdMS_TO_TICKS(3000), pdFALSE, NULL, sync_timer_callback);
            if (xSyncTimer) {
                TRY(xTimerStart(xSyncTimer, 0), "Start Sync");
            }
    #endif

            // Step 4: จุดชนวนลำดับ A→B→C
    #if STEP4_CHAIN
            xChainStarter = xTimerCreate("ChainStarter", pdMS_TO_TICKS(1000), pdFALSE, NULL, chain_starter_callback);
            if (xChainStarter) {
                TRY(xTimerStart(xChainStarter, 0), "Start ChainStarter");
            }
    #endif

            // Step 5: สาธิตสร้าง/ลบ timer (resource management)
    #if STEP5_RESOURCE
            xTaskCreate(temp_resource_task, "TempRes", 2048, NULL, 2, NULL);
    #endif

            // Create control task (ของเดิม)
            xTaskCreate(timer_control_task, "TimerControl", 2048, NULL, 2, NULL);

            ESP_LOGI(TAG, "Timer system operational!");
            ESP_LOGI(TAG, "LEDs indicate different timer activities:");
            ESP_LOGI(TAG, "  GPIO2  - Blink Timer (toggles every 500ms)");
            ESP_LOGI(TAG, "  GPIO4  - Heartbeat Timer (double blink every 2s)");
            ESP_LOGI(TAG, "  GPIO5  - Status Timer (flash every 5s)");
            ESP_LOGI(TAG, "  GPIO18 - One-shot Timer (5 quick flashes)");

        } else {
            ESP_LOGE(TAG, "Failed to create one or more timers");
            ESP_LOGE(TAG, "Check CONFIG_FREERTOS_USE_TIMERS=y in sdkconfig");
        }
    }
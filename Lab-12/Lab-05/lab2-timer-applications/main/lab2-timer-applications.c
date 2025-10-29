// ============================
//  Lab 2 Plus: Timers++
//  - Advanced LED Patterns (LEDC PWM)
//  - Multi-Sensor with Priority via QueueSet
//  - Network Watchdog (UDP Heartbeat)
//  - Pattern Learning (NVS persisted)
// ============================

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "driver/ledc.h"
#include "esp_system.h"
#include "esp_timer.h"

// ---- NVS / Wi-Fi / UDP ----
#include "nvs_flash.h"
#include "nvs.h"

#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

// ============================
// Build-time options
// ============================
#define ENABLE_WIFI 1   // 1 = ใช้ Network Watchdog, 0 = ปิด
#define WIFI_SSID   "YOUR_SSID"
#define WIFI_PASS   "YOUR_PASSWORD"

#define HEARTBEAT_TARGET_IP   "192.168.1.255"  // ใช้ broadcast หรือเป้าเฉพาะ
#define HEARTBEAT_TARGET_PORT 33333
#define HEARTBEAT_INTERVAL_MS 3000
#define HEARTBEAT_FAIL_LIMIT  3   // ล้มเหลวติดกันเกินนี้ → trigger watchdog event

// ============================
// Pins (ตามโจทย์เดิม + LEDC PWM)
// ============================
#define STATUS_LED       GPIO_NUM_2
#define WATCHDOG_LED     GPIO_NUM_4
#define PATTERN_LED_1    GPIO_NUM_5   // LEDC Channel 0
#define PATTERN_LED_2    GPIO_NUM_18  // LEDC Channel 1
#define PATTERN_LED_3    GPIO_NUM_19  // LEDC Channel 2

// หมายเหตุ ADC1 CH0=GPIO36, CH3=GPIO39, CH6=GPIO34 (บน ESP32)
#define SENSOR_TEMP_CH   ADC1_CHANNEL_0  // GPIO36
#define SENSOR_HUM_CH    ADC1_CHANNEL_3  // GPIO39
#define SENSOR_LUX_CH    ADC1_CHANNEL_6  // GPIO34

// (ถ้าจะจ่ายไฟ sensor แยกค่อยเพิ่ม GPIO21/ฯลฯ)

// ============================
// Timings
// ============================
#define WATCHDOG_TIMEOUT_MS     5000
#define WATCHDOG_FEED_MS        2000
#define STATUS_UPDATE_MS        3000
#define SENSOR_PERIOD_MS        500   // base; จะปรับตาม priority
#define PATTERN_TICK_MS         50    // พัลส์ละเอียดสำหรับ advanced patterns

// ============================
// LED Patterns
// ============================
typedef enum {
    PATTERN_OFF = 0,
    PATTERN_SLOW_BLINK,
    PATTERN_FAST_BLINK,
    PATTERN_HEARTBEAT,
    PATTERN_SOS,
    PATTERN_RAINBOW,
    // Advanced:
    PATTERN_BREATHE,      // LEDC PWM sinusoidal
    PATTERN_LARSON,       // KITT scanner
    PATTERN_SPARKLE,      // random spark
    PATTERN_MAX
} led_pattern_t;

static const char *TAG = "TIMER_APPS_PLUS";

// ============================
// Globals
// ============================
static TimerHandle_t t_watchdog, t_feed, t_status, t_pattern, t_netbeat;
static QueueHandle_t q_temp, q_hum, q_lux;
static QueueSetHandle_t qset_sensors;

static led_pattern_t current_pattern = PATTERN_SLOW_BLINK;
static int pattern_step = 0;

// ADC
static esp_adc_cal_characteristics_t *adc_chars;

// NVS (pattern learning)
static nvs_handle_t nvs_h;
static const char *NVS_NS = "lab2plus";
static const char *NVS_KEY_PATTERN_SECS = "psecs"; // blob เก็บวินาทีสะสมของแต่ละแพทเทิร์น
static uint32_t pattern_secs[PATTERN_MAX] = {0};

// System health / stats
typedef struct {
    uint32_t watchdog_feeds;
    uint32_t watchdog_timeouts;
    uint32_t sensor_total_events;
    uint32_t sensor_temp_events;
    uint32_t sensor_hum_events;
    uint32_t sensor_lux_events;
    uint32_t pattern_changes;
    uint32_t uptime_sec;
    bool system_healthy;
} system_stats_t;

static system_stats_t stats = {0};

// Network Watchdog
static int netbeat_fail_streak = 0;
static int net_sock = -1;
static struct sockaddr_in net_addr;

// ============================
// LEDC PWM Setup
// ============================
static void ledc_init(void) {
    // 5 kHz, 10-bit resolution
    ledc_timer_config_t tcfg = {
        .speed_mode = LEDC_HIGH_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tcfg));

    ledc_channel_config_t ch[3] = {
        {
            .gpio_num = PATTERN_LED_1,
            .speed_mode = LEDC_HIGH_SPEED_MODE,
            .channel = LEDC_CHANNEL_0,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
            .hpoint = 0
        },
        {
            .gpio_num = PATTERN_LED_2,
            .speed_mode = LEDC_HIGH_SPEED_MODE,
            .channel = LEDC_CHANNEL_1,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
            .hpoint = 0
        },
        {
            .gpio_num = PATTERN_LED_3,
            .speed_mode = LEDC_HIGH_SPEED_MODE,
            .channel = LEDC_CHANNEL_2,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
            .hpoint = 0
        }
    };
    for (int i = 0; i < 3; i++) ESP_ERROR_CHECK(ledc_channel_config(&ch[i]));
}

static inline void ledc_set3(uint32_t d0, uint32_t d1, uint32_t d2) {
    // duty: 0..1023
    ledc_set_duty(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0, d0);
    ledc_update_duty(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0);
    ledc_set_duty(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_1, d1);
    ledc_update_duty(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_1);
    ledc_set_duty(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_2, d2);
    ledc_update_duty(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_2);
}

static void leds_off(void) { ledc_set3(0,0,0); }

// ============================
// Utility
// ============================
static void flash_gpio(gpio_num_t pin, int times, int on_ms, int off_ms) {
    for (int i = 0; i < times; i++) {
        gpio_set_level(pin, 1);
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        gpio_set_level(pin, 0);
        vTaskDelay(pdMS_TO_TICKS(off_ms));
    }
}

// ============================
// NVS: Pattern Learning
// ============================
static void nvs_load_pattern_secs(void) {
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &nvs_h);
    if (err != ESP_OK) { ESP_LOGW(TAG, "NVS open failed (%s)", esp_err_to_name(err)); return; }

    size_t sz = sizeof(pattern_secs);
    err = nvs_get_blob(nvs_h, NVS_KEY_PATTERN_SECS, pattern_secs, &sz);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Loaded pattern_secs from NVS");
    } else {
        ESP_LOGW(TAG, "No prior pattern_secs; initializing");
        memset(pattern_secs, 0, sizeof(pattern_secs));
        nvs_set_blob(nvs_h, NVS_KEY_PATTERN_SECS, pattern_secs, sizeof(pattern_secs));
        nvs_commit(nvs_h);
    }
}

static void nvs_save_pattern_secs(void) {
    if (!nvs_h) return;
    nvs_set_blob(nvs_h, NVS_KEY_PATTERN_SECS, pattern_secs, sizeof(pattern_secs));
    nvs_commit(nvs_h);
}

static led_pattern_t choose_favorite_pattern(void) {
    // เลือกแพทเทิร์นที่สะสมเวลามากสุด (ยกเว้น OFF)
    uint32_t best = 0; int best_idx = PATTERN_SLOW_BLINK;
    for (int i = 1; i < PATTERN_MAX; i++) {
        if (pattern_secs[i] > best) { best = pattern_secs[i]; best_idx = i; }
    }
    return (led_pattern_t)best_idx;
}

// เรียกทุก 1s เพื่อสะสมเวลาของแพทเทิร์นปัจจุบัน
static void account_pattern_1s(void) {
    if (current_pattern >= 0 && current_pattern < PATTERN_MAX) {
        pattern_secs[current_pattern] += 1;
    }
}

// ============================
// Watchdog
// ============================
static void watchdog_timeout_cb(TimerHandle_t xTimer) {
    stats.watchdog_timeouts++;
    stats.system_healthy = false;
    ESP_LOGE(TAG, "WATCHDOG TIMEOUT! feeds=%" PRIu32 " timeouts=%" PRIu32,
             stats.watchdog_feeds, stats.watchdog_timeouts);
    flash_gpio(WATCHDOG_LED, 10, 50, 50);
    // ตัวอย่าง: esp_restart(); // ใน production
    xTimerReset(t_watchdog, 0);
    stats.system_healthy = true;
}

static void feed_watchdog_cb(TimerHandle_t xTimer) {
    stats.watchdog_feeds++;
    xTimerReset(t_watchdog, 0);
    gpio_set_level(STATUS_LED, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level(STATUS_LED, 0);
}

// ============================
// Sensors + Priority (QueueSet)
// ============================
typedef enum { SRC_TEMP=0, SRC_HUM=1, SRC_LUX=2 } sensor_src_t;
typedef struct {
    sensor_src_t src;
    float value;
    uint32_t tick_ms;
} sensor_evt_t;

static float adc_read_as_volt(adc1_channel_t ch) {
    uint32_t raw = adc1_get_raw(ch);
    uint32_t mv  = esp_adc_cal_raw_to_voltage(raw, adc_chars);
    return mv / 1000.0f;
}

static void temp_task(void *arg) {
    const TickType_t period = pdMS_TO_TICKS(SENSOR_PERIOD_MS); // High priority src
    sensor_evt_t evt; evt.src = SRC_TEMP;
    while (1) {
        float v = adc_read_as_volt(SENSOR_TEMP_CH);
        // map 0..3.3V -> 0..50 C (สมมติ)
        evt.value = (v / 3.3f) * 50.0f;
        evt.tick_ms = pdTICKS_TO_MS(xTaskGetTickCount());
        xQueueSend(q_temp, &evt, 0);
        vTaskDelay(period);
    }
}

static void hum_task(void *arg) {
    const TickType_t period = pdMS_TO_TICKS(SENSOR_PERIOD_MS * 2); // Medium
    sensor_evt_t evt; evt.src = SRC_HUM;
    while (1) {
        float v = adc_read_as_volt(SENSOR_HUM_CH);
        // map 0..3.3V -> 0..100 %RH (สมมติ)
        evt.value = (v / 3.3f) * 100.0f;
        evt.tick_ms = pdTICKS_TO_MS(xTaskGetTickCount());
        xQueueSend(q_hum, &evt, 0);
        vTaskDelay(period);
    }
}

static void lux_task(void *arg) {
    const TickType_t period = pdMS_TO_TICKS(SENSOR_PERIOD_MS * 4); // Low
    sensor_evt_t evt; evt.src = SRC_LUX;
    while (1) {
        float v = adc_read_as_volt(SENSOR_LUX_CH);
        // map 0..3.3V -> 0..1000 lux (สมมติ)
        evt.value = (v / 3.3f) * 1000.0f;
        evt.tick_ms = pdTICKS_TO_MS(xTaskGetTickCount());
        xQueueSend(q_lux, &evt, 0);
        vTaskDelay(period);
    }
}

static void sensor_consumer_task(void *arg) {
    ESP_LOGI(TAG, "Sensor consumer with priority via QueueSet");
    QueueSetMemberHandle_t activated;
    sensor_evt_t evt;
    while (1) {
        // รับจาก QueueSet แล้ว "จัดลำดับความสำคัญ" ด้วยการเช็คคิวตามลำดับ TEMP > HUM > LUX
        activated = xQueueSelectFromSet(qset_sensors, portMAX_DELAY);

        // ดึง TEMP ก่อน ถ้าเป็นคิวนี้
        if (activated == (QueueSetMemberHandle_t)q_temp && xQueueReceive(q_temp, &evt, 0) == pdTRUE) {
            stats.sensor_total_events++; stats.sensor_temp_events++;
            ESP_LOGI(TAG, "TEMP  : %.2f C @ %ums", evt.value, evt.tick_ms);
            if (evt.value > 40.0f) {
                ESP_LOGW(TAG, "High temp! change to FAST_BLINK");
                current_pattern = PATTERN_FAST_BLINK;
                pattern_step = 0;
            }
            continue;
        }
        // รองลงมา HUM
        if (activated == (QueueSetMemberHandle_t)q_hum && xQueueReceive(q_hum, &evt, 0) == pdTRUE) {
            stats.sensor_total_events++; stats.sensor_hum_events++;
            ESP_LOGI(TAG, "HUM   : %.1f %%RH @ %ums", evt.value, evt.tick_ms);
            if (evt.value < 20.0f) { // แห้งมาก
                ESP_LOGW(TAG, "Low humidity! change to SOS");
                current_pattern = PATTERN_SOS;
                pattern_step = 0;
            }
            continue;
        }
        // สุดท้าย LUX
        if (activated == (QueueSetMemberHandle_t)q_lux && xQueueReceive(q_lux, &evt, 0) == pdTRUE) {
            stats.sensor_total_events++; stats.sensor_lux_events++;
            ESP_LOGI(TAG, "LUX   : %.0f lux @ %ums", evt.value, evt.tick_ms);
            if (evt.value < 50.0f) { // มืดมาก
                ESP_LOGI(TAG, "Dark env → BREATHE pattern");
                current_pattern = PATTERN_BREATHE;
                pattern_step = 0;
            }
            continue;
        }
    }
}

// ============================
// Network Watchdog (UDP Heartbeat)
// ============================
static void wifi_init_sta(void) {
#if ENABLE_WIFI
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wcfg = {0};
    snprintf((char*)wcfg.sta.ssid, sizeof(wcfg.sta.ssid), WIFI_SSID);
    snprintf((char*)wcfg.sta.password, sizeof(wcfg.sta.password), WIFI_PASS);
    wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wcfg.sta.pmf_cfg.capable = true;
    wcfg.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI(TAG, "WiFi STA init, connecting to %s…", WIFI_SSID);
#endif
}

static void net_watchdog_open_socket(void) {
#if ENABLE_WIFI
    net_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (net_sock < 0) { ESP_LOGW(TAG, "socket() failed"); return; }

    int yes = 1;
    setsockopt(net_sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    memset(&net_addr, 0, sizeof(net_addr));
    net_addr.sin_family = AF_INET;
    net_addr.sin_port = htons(HEARTBEAT_TARGET_PORT);
    net_addr.sin_addr.s_addr = inet_addr(HEARTBEAT_TARGET_IP);

    ESP_LOGI(TAG, "UDP heartbeat to %s:%d", HEARTBEAT_TARGET_IP, HEARTBEAT_TARGET_PORT);
#endif
}

static void heartbeat_cb(TimerHandle_t xTimer) {
#if ENABLE_WIFI
    if (net_sock < 0) return;
    const char *hb = "lab2plus-heartbeat";
    int ret = sendto(net_sock, hb, strlen(hb), 0, (struct sockaddr*)&net_addr, sizeof(net_addr));
    if (ret < 0) {
        netbeat_fail_streak++;
        ESP_LOGW(TAG, "Heartbeat send failed (%d/%d)", netbeat_fail_streak, HEARTBEAT_FAIL_LIMIT);
        if (netbeat_fail_streak >= HEARTBEAT_FAIL_LIMIT) {
            // ทริกเกอร์ watchdog สาย network
            ESP_LOGE(TAG, "Network heartbeat FAIL → trigger SOS & watchdog");
            current_pattern = PATTERN_SOS;
            pattern_step = 0;
            // กระพริบ WATCHDOG_LED เตือน
            flash_gpio(WATCHDOG_LED, 5, 80, 80);
            netbeat_fail_streak = 0; // reset นับใหม่
        }
    } else {
        netbeat_fail_streak = 0;
        ESP_LOGI(TAG, "Heartbeat sent (%d bytes)", ret);
    }
#endif
}

// ============================
// Status
// ============================
static void status_cb(TimerHandle_t xTimer) {
    stats.uptime_sec = pdTICKS_TO_MS(xTaskGetTickCount()) / 1000;
    account_pattern_1s(); // เพิ่มเวลาสะสม pattern

    ESP_LOGI(TAG,
             "\n--- STATUS ---\n"
             "uptime=%" PRIu32 "s healthy=%s feeds=%" PRIu32 " timeouts=%" PRIu32 "\n"
             "sensors total=%" PRIu32 " (T=%" PRIu32 ",H=%" PRIu32 ",L=%" PRIu32 ")\n"
             "pattern=%d changes=%" PRIu32 "\n"
#if ENABLE_WIFI
             "net fail-streak=%d sock=%d\n"
#endif
             "--------------",
             stats.uptime_sec,
             stats.system_healthy ? "OK" : "ISSUE",
             stats.watchdog_feeds, stats.watchdog_timeouts,
             stats.sensor_total_events, stats.sensor_temp_events,
             stats.sensor_hum_events, stats.sensor_lux_events,
             current_pattern, stats.pattern_changes
#if ENABLE_WIFI
             , netbeat_fail_streak, net_sock
#endif
    );

    // เซฟ NVS ทุก ๆ 10 วินาที (STATUS_UPDATE_MS = 3000 → ~3รอบ)
    static int sec_acc = 0; sec_acc += STATUS_UPDATE_MS/1000;
    if (sec_acc >= 10) { nvs_save_pattern_secs(); sec_acc = 0; }
}

// ============================
// Advanced Pattern Engine
// เรียกทุก PATTERN_TICK_MS
// ============================
static void pattern_tick_cb(TimerHandle_t xTimer) {
    pattern_step++;

    switch (current_pattern) {
        case PATTERN_OFF:
            leds_off();
            break;

        case PATTERN_SLOW_BLINK: {
            // on/off ทุก ~1s
            bool on = ((pattern_step / (1000 / PATTERN_TICK_MS)) % 2) == 0;
            ledc_set3(on ? 1023 : 0, 0, 0);
            break;
        }
        case PATTERN_FAST_BLINK: {
            bool on = ((pattern_step / (200 / PATTERN_TICK_MS)) % 2) == 0;
            ledc_set3(0, on ? 1023 : 0, 0);
            break;
        }
        case PATTERN_HEARTBEAT: {
            // double pulse
            int beat = pattern_step % (1000 / PATTERN_TICK_MS);
            bool pulse = (beat < 80) || (beat >= 150 && beat < 230);
            ledc_set3(0, 0, pulse ? 1023 : 0);
            break;
        }
        case PATTERN_SOS: {
            // ... --- ... ด้วย PWM สูงพร้อมกัน
            static const char *s = "...---...";
            static int idx = 0; static int remain = 0;
            if (remain <= 0) {
                char c = s[idx];
                int dur = (c == '.') ? 200 : 600;
                remain = dur / PATTERN_TICK_MS;
                idx = (idx + 1) % strlen(s);
            }
            ledc_set3(1023, 1023, 1023);
            remain--;
            if (remain <= 0) ledc_set3(0,0,0);
            break;
        }
        case PATTERN_RAINBOW: {
            // 8 สเต็ป bit-mask
            int st = (pattern_step / (300 / PATTERN_TICK_MS)) % 8;
            ledc_set3((st & 1) ? 1023 : 0, (st & 2) ? 1023 : 0, (st & 4) ? 1023 : 0);
            break;
        }
        case PATTERN_BREATHE: {
            // sine wave 0..1023
            float t = (pattern_step % 200) / 200.0f; // 200 ticks ~ 10s ที่ 50ms/tick
            uint32_t duty = (uint32_t)((sinf(t * 2.0f * (float)M_PI) * 0.5f + 0.5f) * 1023.0f);
            ledc_set3(duty, duty/2, duty/4);
            break;
        }
        case PATTERN_LARSON: {
            // สแกนซ้าย-ขวา 3 ดวง
            static int pos = 0; static int dir = 1;
            uint32_t d0=0,d1=0,d2=0;
            if (pos == 0) d0 = 1023;
            else if (pos == 1) d1 = 1023;
            else d2 = 1023;
            ledc_set3(d0, d1, d2);
            if ((pattern_step % (150 / PATTERN_TICK_MS)) == 0) {
                pos += dir;
                if (pos >= 2) { pos = 2; dir = -1; }
                if (pos <= 0) { pos = 0; dir = 1; }
            }
            break;
        }
        case PATTERN_SPARKLE: {
            // สุ่มติดสั้น ๆ
            static int remain = 0;
            if (remain <= 0) {
                int pick = esp_random() % 3;
                uint32_t d0=0,d1=0,d2=0;
                if (pick==0) d0=1023; else if (pick==1) d1=1023; else d2=1023;
                ledc_set3(d0,d1,d2);
                remain = 2; // ~100ms
            } else {
                remain--;
                if (remain==0) ledc_set3(0,0,0);
            }
            break;
        }
        default:
            leds_off();
            break;
    }

    // เปลี่ยน pattern อัตโนมัติทุก ~20s (ถ้าอยากดู cycle)
    if ((pattern_step % (20000 / PATTERN_TICK_MS)) == 0) {
        led_pattern_t next = (current_pattern + 1) % PATTERN_MAX;
        if (next == PATTERN_OFF) next = PATTERN_SLOW_BLINK; // ข้าม OFF
        current_pattern = next;
        stats.pattern_changes++;
        ESP_LOGI(TAG, "Pattern → %d", current_pattern);
    }
}

// ============================
// Init
// ============================
static void init_gpio(void) {
    gpio_config_t io = {
        .pin_bit_mask = (1ULL<<STATUS_LED) | (1ULL<<WATCHDOG_LED),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0, .pull_down_en = 0, .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(STATUS_LED, 0);
    gpio_set_level(WATCHDOG_LED, 0);
}

static void init_adc(void) {
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(SENSOR_TEMP_CH, ADC_ATTEN_DB_11);
    adc1_config_channel_atten(SENSOR_HUM_CH,  ADC_ATTEN_DB_11);
    adc1_config_channel_atten(SENSOR_LUX_CH,  ADC_ATTEN_DB_11);

    adc_chars = calloc(1, sizeof(esp_adc_cal_characteristics_t));
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, adc_chars);
}

static void create_queues(void) {
    q_temp = xQueueCreate(16, sizeof(sensor_evt_t));
    q_hum  = xQueueCreate(16, sizeof(sensor_evt_t));
    q_lux  = xQueueCreate(16, sizeof(sensor_evt_t));
    qset_sensors = xQueueCreateSet(16*3);
    xQueueAddToSet(q_temp, qset_sensors);
    xQueueAddToSet(q_hum,  qset_sensors);
    xQueueAddToSet(q_lux,  qset_sensors);
}

static void create_timers(void) {
    t_watchdog = xTimerCreate("wd", pdMS_TO_TICKS(WATCHDOG_TIMEOUT_MS), pdFALSE, 0, watchdog_timeout_cb);
    t_feed     = xTimerCreate("feed", pdMS_TO_TICKS(WATCHDOG_FEED_MS), pdTRUE, 0, feed_watchdog_cb);
    t_status   = xTimerCreate("stat", pdMS_TO_TICKS(STATUS_UPDATE_MS), pdTRUE, 0, status_cb);
    t_pattern  = xTimerCreate("ptn", pdMS_TO_TICKS(PATTERN_TICK_MS), pdTRUE, 0, pattern_tick_cb);
#if ENABLE_WIFI
    t_netbeat  = xTimerCreate("hb", pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS), pdTRUE, 0, heartbeat_cb);
#endif
}

static void start_all(void) {
    xTimerStart(t_watchdog, 0);
    xTimerStart(t_feed, 0);
    xTimerStart(t_status, 0);
    xTimerStart(t_pattern, 0);
#if ENABLE_WIFI
    xTimerStart(t_netbeat, 0);
#endif

    // tasks
    xTaskCreate(temp_task, "temp", 2048, NULL, 7, NULL); // สูงสุด
    xTaskCreate(hum_task,  "hum",  2048, NULL, 6, NULL);
    xTaskCreate(lux_task,  "lux",  2048, NULL, 5, NULL);
    xTaskCreate(sensor_consumer_task, "sensor_cons", 3072, NULL, 8, NULL);
}

// ============================
// app_main
// ============================
void app_main(void) {
    ESP_LOGI(TAG, "=== Lab 2 PLUS Starting ===");

        // NVS
    ESP_ERROR_CHECK(nvs_flash_init());
    nvs_load_pattern_secs();

    // Hardware
    init_gpio();
    ledc_init();
    init_adc();

    // Choose favorite pattern (เรียนรู้)
    current_pattern = choose_favorite_pattern();
    ESP_LOGI(TAG, "Favorite pattern from NVS = %d", current_pattern);

    // Wi-Fi + UDP
    wifi_init_sta();
    net_watchdog_open_socket();

    // Queues + Timers + Tasks
    create_queues();
    create_timers();
    start_all();

    stats.system_healthy = true;
    ESP_LOGI(TAG, "System up. Watch LEDs & logs.");
}
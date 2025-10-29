// SPDX-License-Identifier: MIT
// Build: ESP-IDF v5.x

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h> // <-- ใช้ PRIu32 / PRIu64
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/timers.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "driver/gpio.h"

#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "esp_http_client.h"
// ถ้าจะใช้ HTTPS พร้อม cert bundle ให้เปิดบรรทัดนี้ + menuconfig
// #include "esp_crt_bundle.h"

static const char *TAG = "COMPLEX_EVENTS";

/* ===================== User Config ===================== */
#define WIFI_SSID "Test"
#define WIFI_PASS "0954276527"

// ใช้ HTTP แบบไม่เข้ารหัสเพื่อลดปัญหา cert ช่วงทดสอบ
#define CLOUD_URL "http://httpbin.org/post"
/* ======================================================= */

// GPIO สำหรับ Smart Home System
#define LED_LIVING_ROOM GPIO_NUM_2
#define LED_KITCHEN GPIO_NUM_4
#define LED_BEDROOM GPIO_NUM_5
#define LED_SECURITY GPIO_NUM_18
#define LED_EMERGENCY GPIO_NUM_19
#define MOTION_SENSOR GPIO_NUM_21
#define DOOR_SENSOR GPIO_NUM_22

// Smart Home State Machine States
typedef enum
{
    HOME_STATE_IDLE = 0,
    HOME_STATE_OCCUPIED,
    HOME_STATE_AWAY,
    HOME_STATE_SLEEP,
    HOME_STATE_SECURITY_ARMED,
    HOME_STATE_EMERGENCY,
    HOME_STATE_MAINTENANCE
} home_state_t;

// Event Groups
EventGroupHandle_t sensor_events;
EventGroupHandle_t system_events;
EventGroupHandle_t pattern_events;

// Sensor Events
#define MOTION_DETECTED_BIT (1 << 0)
#define DOOR_OPENED_BIT (1 << 1)
#define DOOR_CLOSED_BIT (1 << 2)
#define LIGHT_ON_BIT (1 << 3)
#define LIGHT_OFF_BIT (1 << 4)
#define TEMPERATURE_HIGH_BIT (1 << 5)
#define TEMPERATURE_LOW_BIT (1 << 6)
#define SOUND_DETECTED_BIT (1 << 7)
#define PRESENCE_CONFIRMED_BIT (1 << 8)

// System Events
#define SYSTEM_INIT_BIT (1 << 0)
#define USER_HOME_BIT (1 << 1)
#define USER_AWAY_BIT (1 << 2)
#define SLEEP_MODE_BIT (1 << 3)
#define SECURITY_ARMED_BIT (1 << 4)
#define EMERGENCY_MODE_BIT (1 << 5)
#define MAINTENANCE_MODE_BIT (1 << 6)

// Pattern Events
#define PATTERN_NORMAL_ENTRY_BIT (1 << 0)
#define PATTERN_BREAK_IN_BIT (1 << 1)
#define PATTERN_EMERGENCY_BIT (1 << 2)
#define PATTERN_GOODNIGHT_BIT (1 << 3)
#define PATTERN_WAKE_UP_BIT (1 << 4)
#define PATTERN_LEAVING_BIT (1 << 5)
#define PATTERN_RETURNING_BIT (1 << 6)

// Event & State
static home_state_t current_home_state = HOME_STATE_IDLE;
static SemaphoreHandle_t state_mutex;

// Event History
#define EVENT_HISTORY_SIZE 20
typedef struct
{
    EventBits_t event_bits;
    uint64_t timestamp;
    home_state_t state_at_time;
} event_record_t;

static event_record_t event_history[EVENT_HISTORY_SIZE];
static int history_index = 0;

// Pattern Recognition Data
typedef struct
{
    const char *name;
    EventBits_t required_events[4]; // up to 4 in sequence
    uint32_t time_window_ms;
    EventBits_t result_event;
    void (*action_callback)(void);
} event_pattern_t;

// Adaptive params
typedef struct
{
    float motion_sensitivity;
    uint32_t auto_light_timeout;
    uint32_t security_delay;
    bool learning_mode;
    uint32_t pattern_confidence[10];
} adaptive_params_t;

static adaptive_params_t adaptive_params = {
    .motion_sensitivity = 0.7f,
    .auto_light_timeout = 300000,
    .security_delay = 30000,
    .learning_mode = true,
    .pattern_confidence = {0}};

// Smart devices status
typedef struct
{
    bool living_room_light;
    bool kitchen_light;
    bool bedroom_light;
    bool security_system;
    bool emergency_mode;
    uint32_t temperature_celsius;
    uint32_t light_level_percent;
} smart_home_status_t;

static smart_home_status_t home_status = {0};

/* ================= Pattern Actions ================== */
static void normal_entry_action(void)
{
    ESP_LOGI(TAG, "🏠 Normal entry pattern detected - Welcome home!");
    home_status.living_room_light = true;
    gpio_set_level(LED_LIVING_ROOM, 1);
    xEventGroupSetBits(system_events, USER_HOME_BIT);
}

static void break_in_action(void)
{
    ESP_LOGW(TAG, "🚨 Break-in pattern detected - Security alert!");
    home_status.security_system = true;
    home_status.emergency_mode = true;
    gpio_set_level(LED_SECURITY, 1);
    gpio_set_level(LED_EMERGENCY, 1);
    xEventGroupSetBits(system_events, EMERGENCY_MODE_BIT);
}

static void goodnight_action(void)
{
    ESP_LOGI(TAG, "🌙 Goodnight pattern detected - Sleep mode activated");
    home_status.living_room_light = false;
    home_status.kitchen_light = false;
    gpio_set_level(LED_LIVING_ROOM, 0);
    gpio_set_level(LED_KITCHEN, 0);
    gpio_set_level(LED_BEDROOM, 1);
    xEventGroupSetBits(system_events, SLEEP_MODE_BIT);
}

static void wake_up_action(void)
{
    ESP_LOGI(TAG, "☀️ Wake-up pattern detected - Good morning!");
    home_status.bedroom_light = true;
    home_status.kitchen_light = true;
    gpio_set_level(LED_BEDROOM, 1);
    gpio_set_level(LED_KITCHEN, 1);
    xEventGroupClearBits(system_events, SLEEP_MODE_BIT);
}

static void leaving_action(void)
{
    ESP_LOGI(TAG, "🚪 Leaving pattern detected - Securing home");
    home_status.living_room_light = false;
    home_status.kitchen_light = false;
    home_status.bedroom_light = false;
    home_status.security_system = true;

    gpio_set_level(LED_LIVING_ROOM, 0);
    gpio_set_level(LED_KITCHEN, 0);
    gpio_set_level(LED_BEDROOM, 0);
    gpio_set_level(LED_SECURITY, 1);

    xEventGroupSetBits(system_events, USER_AWAY_BIT | SECURITY_ARMED_BIT);
}

static void returning_action(void)
{
    ESP_LOGI(TAG, "🔓 Returning pattern detected - Disabling security");
    home_status.security_system = false;
    gpio_set_level(LED_SECURITY, 0);
    xEventGroupClearBits(system_events, USER_AWAY_BIT | SECURITY_ARMED_BIT);
}

/* ================= Patterns ================== */
static event_pattern_t event_patterns[] = {
    {.name = "Normal Entry",
     .required_events = {DOOR_OPENED_BIT, MOTION_DETECTED_BIT, DOOR_CLOSED_BIT, 0},
     .time_window_ms = 10000,
     .result_event = PATTERN_NORMAL_ENTRY_BIT,
     .action_callback = normal_entry_action},
    {.name = "Break-in Attempt",
     .required_events = {DOOR_OPENED_BIT, MOTION_DETECTED_BIT, 0, 0},
     .time_window_ms = 5000,
     .result_event = PATTERN_BREAK_IN_BIT,
     .action_callback = break_in_action},
    {.name = "Goodnight Routine",
     .required_events = {LIGHT_OFF_BIT, MOTION_DETECTED_BIT, LIGHT_OFF_BIT, 0},
     .time_window_ms = 30000,
     .result_event = PATTERN_GOODNIGHT_BIT,
     .action_callback = goodnight_action},
    {.name = "Wake-up Routine",
     .required_events = {MOTION_DETECTED_BIT, LIGHT_ON_BIT, 0, 0},
     .time_window_ms = 5000,
     .result_event = PATTERN_WAKE_UP_BIT,
     .action_callback = wake_up_action},
    {.name = "Leaving Home",
     .required_events = {LIGHT_OFF_BIT, DOOR_OPENED_BIT, DOOR_CLOSED_BIT, 0},
     .time_window_ms = 15000,
     .result_event = PATTERN_LEAVING_BIT,
     .action_callback = leaving_action},
    {.name = "Returning Home",
     .required_events = {DOOR_OPENED_BIT, MOTION_DETECTED_BIT, DOOR_CLOSED_BIT, 0},
     .time_window_ms = 8000,
     .result_event = PATTERN_RETURNING_BIT,
     .action_callback = returning_action}};
#define NUM_PATTERNS (sizeof(event_patterns) / sizeof(event_pattern_t))

/* ================= Helpers ================== */
static const char *get_state_name(home_state_t s)
{
    switch (s)
    {
    case HOME_STATE_IDLE:
        return "Idle";
    case HOME_STATE_OCCUPIED:
        return "Occupied";
    case HOME_STATE_AWAY:
        return "Away";
    case HOME_STATE_SLEEP:
        return "Sleep";
    case HOME_STATE_SECURITY_ARMED:
        return "Security Armed";
    case HOME_STATE_EMERGENCY:
        return "Emergency";
    case HOME_STATE_MAINTENANCE:
        return "Maintenance";
    default:
        return "Unknown";
    }
}

static void change_home_state(home_state_t new_state)
{
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
    {
        home_state_t old = current_home_state;
        current_home_state = new_state;
        ESP_LOGI(TAG, "🏠 State changed: %s → %s", get_state_name(old), get_state_name(new_state));
        xSemaphoreGive(state_mutex);
    }
}

static void add_event_to_history(EventBits_t bits)
{
    event_history[history_index].event_bits = bits;
    event_history[history_index].timestamp = esp_timer_get_time();
    event_history[history_index].state_at_time = current_home_state;
    history_index = (history_index + 1) % EVENT_HISTORY_SIZE;
}

/* =============== Pattern Engine Task =============== */
static void pattern_recognition_task(void *arg)
{
    ESP_LOGI(TAG, "🧠 Pattern recognition engine started");
    while (1)
    {
        EventBits_t sensor_bits = xEventGroupWaitBits(
            sensor_events, 0xFFFFFF, pdFALSE, pdFALSE, portMAX_DELAY);

        if (sensor_bits)
        {
            ESP_LOGI(TAG, "🔍 Sensor event detected: 0x%08X", sensor_bits);
            add_event_to_history(sensor_bits);

            for (int p = 0; p < NUM_PATTERNS; ++p)
            {
                event_pattern_t *pat = &event_patterns[p];

                bool state_ok = true;
                if (strcmp(pat->name, "Break-in Attempt") == 0)
                {
                    state_ok = (current_home_state == HOME_STATE_SECURITY_ARMED);
                }
                else if (strcmp(pat->name, "Wake-up Routine") == 0)
                {
                    state_ok = (current_home_state == HOME_STATE_SLEEP);
                }
                else if (strcmp(pat->name, "Returning Home") == 0)
                {
                    state_ok = (current_home_state == HOME_STATE_AWAY);
                }
                if (!state_ok)
                    continue;

                uint64_t now = esp_timer_get_time();
                int event_idx = 0;

                for (int h = 0; h < EVENT_HISTORY_SIZE && pat->required_events[event_idx] != 0; ++h)
                {
                    int idx = (history_index - 1 - h + EVENT_HISTORY_SIZE) % EVENT_HISTORY_SIZE;
                    event_record_t *rec = &event_history[idx];

                    if ((now - rec->timestamp) > (uint64_t)pat->time_window_ms * 1000ULL)
                        break;

                    if (rec->event_bits & pat->required_events[event_idx])
                    {
                        ++event_idx;
                        ESP_LOGI(TAG, "✅ Pattern '%s': Found event %d/x (0x%08X)",
                                 pat->name, event_idx, pat->required_events[event_idx - 1]);
                        if (pat->required_events[event_idx] == 0)
                            break;
                    }
                }

                if (pat->required_events[event_idx] == 0)
                {
                    ESP_LOGI(TAG, "🎯 Pattern matched: %s", pat->name);
                    xEventGroupSetBits(pattern_events, pat->result_event);
                    if (pat->action_callback)
                        pat->action_callback();
                    if (p < 10)
                        adaptive_params.pattern_confidence[p]++;
                    xEventGroupClearBits(sensor_events, 0xFFFFFF);
                    break;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* =============== Sensor Simulation =============== */
static void motion_sensor_task(void *arg)
{
    ESP_LOGI(TAG, "🏃 Motion sensor simulation started");
    while (1)
    {
        if ((esp_random() % 100) < 15)
        {
            ESP_LOGI(TAG, "👥 Motion detected!");
            xEventGroupSetBits(sensor_events, MOTION_DETECTED_BIT);
            vTaskDelay(pdMS_TO_TICKS(1000 + (esp_random() % 2000)));
            if ((esp_random() % 100) < 60)
            {
                ESP_LOGI(TAG, "✅ Presence confirmed");
                xEventGroupSetBits(sensor_events, PRESENCE_CONFIRMED_BIT);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(3000 + (esp_random() % 5000)));
    }
}

static void door_sensor_task(void *arg)
{
    ESP_LOGI(TAG, "🚪 Door sensor simulation started");
    bool door_open = false;
    while (1)
    {
        if ((esp_random() % 100) < 8)
        {
            if (!door_open)
            {
                ESP_LOGI(TAG, "🔓 Door opened");
                xEventGroupSetBits(sensor_events, DOOR_OPENED_BIT);
                door_open = true;
                vTaskDelay(pdMS_TO_TICKS(2000 + (esp_random() % 8000)));
                if ((esp_random() % 100) < 85)
                {
                    ESP_LOGI(TAG, "🔒 Door closed");
                    xEventGroupSetBits(sensor_events, DOOR_CLOSED_BIT);
                    door_open = false;
                }
            }
            else
            {
                ESP_LOGI(TAG, "🔒 Door closed");
                xEventGroupSetBits(sensor_events, DOOR_CLOSED_BIT);
                door_open = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000 + (esp_random() % 10000)));
    }
}

static void light_control_task(void *arg)
{
    ESP_LOGI(TAG, "💡 Light control system started");
    while (1)
    {
        if ((esp_random() % 100) < 12)
        {
            bool on = (esp_random() % 2);
            if (on)
            {
                ESP_LOGI(TAG, "💡 Light turned ON");
                xEventGroupSetBits(sensor_events, LIGHT_ON_BIT);
                int which = esp_random() % 3;
                switch (which)
                {
                case 0:
                    home_status.living_room_light = true;
                    gpio_set_level(LED_LIVING_ROOM, 1);
                    break;
                case 1:
                    home_status.kitchen_light = true;
                    gpio_set_level(LED_KITCHEN, 1);
                    break;
                case 2:
                    home_status.bedroom_light = true;
                    gpio_set_level(LED_BEDROOM, 1);
                    break;
                }
            }
            else
            {
                ESP_LOGI(TAG, "💡 Light turned OFF");
                xEventGroupSetBits(sensor_events, LIGHT_OFF_BIT);
                int which = esp_random() % 3;
                switch (which)
                {
                case 0:
                    home_status.living_room_light = false;
                    gpio_set_level(LED_LIVING_ROOM, 0);
                    break;
                case 1:
                    home_status.kitchen_light = false;
                    gpio_set_level(LED_KITCHEN, 0);
                    break;
                case 2:
                    home_status.bedroom_light = false;
                    gpio_set_level(LED_BEDROOM, 0);
                    break;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(4000 + (esp_random() % 8000)));
    }
}

static void environmental_sensor_task(void *arg)
{
    ESP_LOGI(TAG, "🌡️ Environmental sensors started");
    while (1)
    {
        home_status.temperature_celsius = 20 + (esp_random() % 15); // 20-35
        if (home_status.temperature_celsius > 28)
        {
            ESP_LOGI(TAG, "🔥 High temperature: %" PRIu32 "°C", home_status.temperature_celsius);
            xEventGroupSetBits(sensor_events, TEMPERATURE_HIGH_BIT);
        }
        else if (home_status.temperature_celsius < 22)
        {
            ESP_LOGI(TAG, "🧊 Low temperature: %" PRIu32 "°C", home_status.temperature_celsius);
            xEventGroupSetBits(sensor_events, TEMPERATURE_LOW_BIT);
        }
        if ((esp_random() % 100) < 5)
        {
            ESP_LOGI(TAG, "🔊 Sound detected");
            xEventGroupSetBits(sensor_events, SOUND_DETECTED_BIT);
        }
        home_status.light_level_percent = esp_random() % 100;
        vTaskDelay(pdMS_TO_TICKS(8000 + (esp_random() % 7000)));
    }
}

/* =============== State Machine Task =============== */
static void state_machine_task(void *arg)
{
    ESP_LOGI(TAG, "🏠 Home state machine started");
    while (1)
    {
        EventBits_t sys = xEventGroupWaitBits(system_events, 0xFFFFFF, pdTRUE, pdFALSE, pdMS_TO_TICKS(5000));
        if (sys)
        {
            ESP_LOGI(TAG, "🔄 System event: 0x%08X", sys);
            if (sys & USER_HOME_BIT)
            {
                if (current_home_state == HOME_STATE_AWAY || current_home_state == HOME_STATE_IDLE)
                {
                    change_home_state(HOME_STATE_OCCUPIED);
                }
            }
            if (sys & USER_AWAY_BIT)
                change_home_state(HOME_STATE_AWAY);
            if (sys & SLEEP_MODE_BIT)
            {
                if (current_home_state == HOME_STATE_OCCUPIED)
                    change_home_state(HOME_STATE_SLEEP);
            }
            if (sys & SECURITY_ARMED_BIT)
            {
                if (current_home_state == HOME_STATE_AWAY)
                    change_home_state(HOME_STATE_SECURITY_ARMED);
            }
            if (sys & EMERGENCY_MODE_BIT)
                change_home_state(HOME_STATE_EMERGENCY);
            if (sys & MAINTENANCE_MODE_BIT)
                change_home_state(HOME_STATE_MAINTENANCE);
        }

        switch (current_home_state)
        {
        case HOME_STATE_EMERGENCY:
            vTaskDelay(pdMS_TO_TICKS(10000));
            ESP_LOGI(TAG, "🆘 Emergency cleared");
            home_status.emergency_mode = false;
            gpio_set_level(LED_EMERGENCY, 0);
            change_home_state(HOME_STATE_OCCUPIED);
            break;
        case HOME_STATE_IDLE:
        {
            EventBits_t s = xEventGroupGetBits(sensor_events);
            if (s & (MOTION_DETECTED_BIT | PRESENCE_CONFIRMED_BIT))
                change_home_state(HOME_STATE_OCCUPIED);
        }
        break;
        default:
            break;
        }
    }
}

/* =============== Adaptive Learning =============== */
static void adaptive_learning_task(void *arg)
{
    ESP_LOGI(TAG, "🧠 Adaptive learning started");
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(30000));
        if (!adaptive_params.learning_mode)
            continue;

        uint32_t recent_motion = 0;
        uint64_t now = esp_timer_get_time();
        for (int h = 0; h < EVENT_HISTORY_SIZE; ++h)
        {
            int idx = (history_index - 1 - h + EVENT_HISTORY_SIZE) % EVENT_HISTORY_SIZE;
            event_record_t *rec = &event_history[idx];
            if ((now - rec->timestamp) < 300000000ULL)
            {
                if (rec->event_bits & MOTION_DETECTED_BIT)
                    recent_motion++;
            }
            else
                break;
        }

        if (recent_motion > 10)
        {
            adaptive_params.motion_sensitivity *= 0.95f;
            ESP_LOGI(TAG, "🔧 High motion, sensitivity=%.2f", adaptive_params.motion_sensitivity);
        }
        else if (recent_motion < 2)
        {
            adaptive_params.motion_sensitivity *= 1.05f;
            ESP_LOGI(TAG, "🔧 Low motion, sensitivity=%.2f", adaptive_params.motion_sensitivity);
        }

        if (adaptive_params.motion_sensitivity > 1.0f)
            adaptive_params.motion_sensitivity = 1.0f;
        if (adaptive_params.motion_sensitivity < 0.3f)
            adaptive_params.motion_sensitivity = 0.3f;
    }
}

/* =============== Status Monitor =============== */
static void status_monitor_task(void *arg)
{
    ESP_LOGI(TAG, "📊 Status monitor started");
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(20000));
        ESP_LOGI(TAG, "\n🏠 ═══ SMART HOME STATUS ═══");
        ESP_LOGI(TAG, "State: %s", get_state_name(current_home_state));
        ESP_LOGI(TAG, "Living:  %s", home_status.living_room_light ? "ON" : "OFF");
        ESP_LOGI(TAG, "Kitchen: %s", home_status.kitchen_light ? "ON" : "OFF");
        ESP_LOGI(TAG, "Bedroom: %s", home_status.bedroom_light ? "ON" : "OFF");
        ESP_LOGI(TAG, "Security:%s", home_status.security_system ? "ARMED" : "DISARMED");
        ESP_LOGI(TAG, "Emergency:%s", home_status.emergency_mode ? "ACTIVE" : "NORMAL");
        ESP_LOGI(TAG, "Temp:     %" PRIu32 "°C", home_status.temperature_celsius);
        ESP_LOGI(TAG, "Light:    %" PRIu32 "%%", home_status.light_level_percent);
        ESP_LOGI(TAG, "SensorBits: 0x%08X  SystemBits: 0x%08X  PatternBits: 0x%08X",
                 xEventGroupGetBits(sensor_events),
                 xEventGroupGetBits(system_events),
                 xEventGroupGetBits(pattern_events));

        ESP_LOGI(TAG, "Motion Sensitivity: %.2f", adaptive_params.motion_sensitivity);
        ESP_LOGI(TAG, "Light Timeout:      %" PRIu32 " ms", adaptive_params.auto_light_timeout);
        ESP_LOGI(TAG, "Security Delay:     %" PRIu32 " ms", adaptive_params.security_delay);

        ESP_LOGI(TAG, "Pattern Confidence:");
        for (int i = 0; i < NUM_PATTERNS; ++i)
        {
            if (adaptive_params.pattern_confidence[i] > 0)
            {
                ESP_LOGI(TAG, "  %s: %" PRIu32, event_patterns[i].name, adaptive_params.pattern_confidence[i]);
            }
        }
        ESP_LOGI(TAG, "Free Heap: %d bytes", esp_get_free_heap_size());
        ESP_LOGI(TAG, "════════════════════════════════════════\n");
    }
}

/* =============== Wi-Fi =============== */
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "📶 Wi-Fi got IP");
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGW(TAG, "📶 Wi-Fi disconnected, retry...");
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {0};
    strcpy((char *)wifi_config.sta.ssid, WIFI_SSID);
    strcpy((char *)wifi_config.sta.password, WIFI_PASS);

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "📡 Wi-Fi connecting to SSID:%s ...", WIFI_SSID);
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));
    if (!(bits & WIFI_CONNECTED_BIT))
    {
        ESP_LOGE(TAG, "Wi-Fi connect timeout");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* =============== Cloud Uploader =============== */
typedef struct
{
    char device_id[32];
    uint64_t ts_ms;
    int living_on, kitchen_on, bedroom_on;
    int temperature_c;
    int light_percent;
    uint32_t motion_count;
} cloud_metrics_t;

static uint64_t get_time_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

static void build_metrics(cloud_metrics_t *m)
{
    snprintf(m->device_id, sizeof(m->device_id), "esp32-%04X", (unsigned)(esp_random() & 0xFFFF));
    m->ts_ms = get_time_ms();
    m->living_on = home_status.living_room_light ? 1 : 0;
    m->kitchen_on = home_status.kitchen_light ? 1 : 0;
    m->bedroom_on = home_status.bedroom_light ? 1 : 0;
    m->temperature_c = (int)home_status.temperature_celsius;
    m->light_percent = (int)home_status.light_level_percent;
    // นับ motion ล่าสุด 10 รายการใน history อย่างหยาบ ๆ
    uint32_t cnt = 0;
    for (int h = 0; h < 10; ++h)
    {
        int idx = (history_index - 1 - h + EVENT_HISTORY_SIZE) % EVENT_HISTORY_SIZE;
        if (event_history[idx].event_bits & MOTION_DETECTED_BIT)
            cnt++;
    }
    m->motion_count = cnt;
}

static esp_err_t post_json(const char *url, const char *json)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 8000,
        // ถ้าใช้ HTTPS + bundle:
        // .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli)
        return ESP_FAIL;

    ESP_ERROR_CHECK(esp_http_client_set_header(cli, "Content-Type", "application/json"));
    ESP_ERROR_CHECK(esp_http_client_set_post_field(cli, json, strlen(json)));

    esp_err_t err = esp_http_client_perform(cli);
    if (err == ESP_OK)
    {
        int status = esp_http_client_get_status_code(cli);
        int len = esp_http_client_get_content_length(cli);
        ESP_LOGI(TAG, "🌩️ POST %s -> status=%d len=%d", url, status, len);
    }
    else
    {
        ESP_LOGE(TAG, "POST error: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(cli);
    return err;
}

static void uploader_task(void *arg)
{
    ESP_LOGI(TAG, "☁️ Cloud uploader started → %s", CLOUD_URL);
    while (1)
    {
        // รอให้มี Wi-Fi
        EventBits_t b = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
        if (!(b & WIFI_CONNECTED_BIT))
        {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        cloud_metrics_t m = {0};
        build_metrics(&m);

        char json[512];
        int n = snprintf(json, sizeof(json),
                         "{"
                         "\"device_id\":\"%s\","
                         "\"ts_ms\":%" PRIu64 ","
                         "\"lights\":{\"living\":%d,\"kitchen\":%d,\"bedroom\":%d},"
                         "\"sensors\":{\"temp_c\":%d,\"light_pct\":%d,\"motion_count\":%" PRIu32 "},"
                         "\"state\":\"%s\""
                         "}",
                         m.device_id, m.ts_ms,
                         m.living_on, m.kitchen_on, m.bedroom_on,
                         m.temperature_c, m.light_percent, (uint32_t)m.motion_count,
                         get_state_name(current_home_state));
        if (n < 0 || n >= (int)sizeof(json))
        {
            ESP_LOGE(TAG, "JSON overflow");
        }
        else
        {
            (void)post_json(CLOUD_URL, json);
        }

        vTaskDelay(pdMS_TO_TICKS(15000));
    }
}

/* =============== app_main =============== */
void app_main(void)
{
    ESP_LOGI(TAG, "🚀 Complex Event Patterns - Smart Home System Starting...");

    // GPIO
    gpio_set_direction(LED_LIVING_ROOM, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_KITCHEN, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_BEDROOM, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_SECURITY, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_EMERGENCY, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_LIVING_ROOM, 0);
    gpio_set_level(LED_KITCHEN, 0);
    gpio_set_level(LED_BEDROOM, 0);
    gpio_set_level(LED_SECURITY, 0);
    gpio_set_level(LED_EMERGENCY, 0);

    // Mutex
    state_mutex = xSemaphoreCreateMutex();
    if (!state_mutex)
    {
        ESP_LOGE(TAG, "state mutex failed");
        return;
    }

    // Event groups
    sensor_events = xEventGroupCreate();
    system_events = xEventGroupCreate();
    pattern_events = xEventGroupCreate();
    wifi_event_group = xEventGroupCreate();
    if (!sensor_events || !system_events || !pattern_events || !wifi_event_group)
    {
        ESP_LOGE(TAG, "event groups failed");
        return;
    }

    // NVS + Wi-Fi
    ESP_ERROR_CHECK(nvs_flash_init());
    if (wifi_init_sta() != ESP_OK)
    {
        ESP_LOGW(TAG, "Wi-Fi not connected yet, continue anyway...");
    }

    // Init state
    xEventGroupSetBits(system_events, SYSTEM_INIT_BIT);
    change_home_state(HOME_STATE_IDLE);

    // Tasks
    xTaskCreate(pattern_recognition_task, "PatternEngine", 4096, NULL, 8, NULL);
    xTaskCreate(state_machine_task, "StateMachine", 3072, NULL, 7, NULL);
    xTaskCreate(adaptive_learning_task, "Learning", 3072, NULL, 5, NULL);
    xTaskCreate(status_monitor_task, "Monitor", 3072, NULL, 3, NULL);

    xTaskCreate(motion_sensor_task, "MotionSensor", 2048, NULL, 6, NULL);
    xTaskCreate(door_sensor_task, "DoorSensor", 2048, NULL, 6, NULL);
    xTaskCreate(light_control_task, "LightControl", 2048, NULL, 6, NULL);
    xTaskCreate(environmental_sensor_task, "EnvSensors", 2048, NULL, 5, NULL);

    xTaskCreate(uploader_task, "Uploader", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "\n🎯 Smart Home LED Indicators:");
    ESP_LOGI(TAG, "  GPIO2  - Living Room Light");
    ESP_LOGI(TAG, "  GPIO4  - Kitchen Light");
    ESP_LOGI(TAG, "  GPIO5  - Bedroom Light");
    ESP_LOGI(TAG, "  GPIO18 - Security System");
    ESP_LOGI(TAG, "  GPIO19 - Emergency Mode");

    ESP_LOGI(TAG, "\n🤖 Features:");
    ESP_LOGI(TAG, "  • Event-driven State Machine");
    ESP_LOGI(TAG, "  • Pattern Recognition Engine");
    ESP_LOGI(TAG, "  • Adaptive Learning System");
    ESP_LOGI(TAG, "  • Cloud Uploader (HTTP)");
    ESP_LOGI(TAG, "  • Complex Event Correlation");

    ESP_LOGI(TAG, "\n🔍 Patterns:");
    for (int i = 0; i < NUM_PATTERNS; ++i)
    {
        ESP_LOGI(TAG, "  • %s", event_patterns[i].name);
    }
    ESP_LOGI(TAG, "System operational!");
}
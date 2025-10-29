#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_timer.h"

#define TAG "PERIPHERAL_EX3"

// =================== Globals ===================
SemaphoreHandle_t spi_i2c_mutex;
QueueHandle_t gpio_event_queue;

// =================== WiFi ===================
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_STA_START)
        esp_wifi_connect();
    else if (event_id == WIFI_EVENT_STA_CONNECTED)
        ESP_LOGI(TAG, "WiFi Connected");
    else if (event_id == IP_EVENT_STA_GOT_IP)
        ESP_LOGI(TAG, "Got IP Address");
}

void wifi_task(void *pv)
{
    ESP_LOGI(TAG, "WiFi task running on Core %d", xPortGetCoreID());
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(2000));
        ESP_LOGI(TAG, "WiFi still connected...");
    }
}

// =================== GPIO ISR ===================
static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t)arg;
    xQueueSendFromISR(gpio_event_queue, &gpio_num, NULL);
}

void gpio_task(void *pv)
{
    uint32_t io_num;
    while (1)
    {
        if (xQueueReceive(gpio_event_queue, &io_num, portMAX_DELAY))
        {
            ESP_LOGI(TAG, "GPIO Interrupt on pin %ld", io_num);
        }
    }
}

// =================== Timer ===================
void timer_callback(void *arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR((TaskHandle_t)arg, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR();
}

// =================== I2C/SPI Task ===================
void sensor_task(void *pv)
{
    while (1)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(spi_i2c_mutex, portMAX_DELAY);
        ESP_LOGI(TAG, "Sensor reading via I2C on Core %d", xPortGetCoreID());
        xSemaphoreGive(spi_i2c_mutex);
    }
}

// =================== Main ===================
void app_main(void)
{
    ESP_LOGI(TAG, "Starting Exercise 3: ESP32 Peripheral Integration");

    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();

    // WiFi setup
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    // GPIO setup
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_POSEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .pull_up_en = 1,
    };
    gpio_config(&io_conf);
    gpio_event_queue = xQueueCreate(10, sizeof(uint32_t));
    gpio_install_isr_service(0);
    gpio_isr_handler_add(GPIO_NUM_0, gpio_isr_handler, (void *)GPIO_NUM_0);

    // Create resources
    spi_i2c_mutex = xSemaphoreCreateMutex();

    // Create tasks
    TaskHandle_t sensorTaskHandle;
    xTaskCreatePinnedToCore(sensor_task, "SensorTask", 4096, NULL, 5, &sensorTaskHandle, 0);
    xTaskCreatePinnedToCore(wifi_task, "WiFiTask", 4096, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(gpio_task, "GPIO_Task", 4096, NULL, 6, NULL, 0);

    // Create hardware timer
    const esp_timer_create_args_t timer_args = {
        .callback = &timer_callback,
        .arg = sensorTaskHandle,
        .name = "sensor_timer"};
    esp_timer_handle_t periodic_timer;
    esp_timer_create(&timer_args, &periodic_timer);
    esp_timer_start_periodic(periodic_timer, 1000000); // 1 Hz (1 sec)
}
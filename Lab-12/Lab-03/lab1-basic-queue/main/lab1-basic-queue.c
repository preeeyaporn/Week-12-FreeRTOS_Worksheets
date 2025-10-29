#include <stdio.h>

void app_main(void)
{

}
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/gpio.h"

static const char *TAG = "QUEUE_LAB";

// ==== HW Pins ====
#define LED_SENDER   GPIO_NUM_2
#define LED_RECEIVER GPIO_NUM_4

// ==== Queue Config ====
#define QUEUE_LENGTH   5
#define QUEUE_ITEMSIZE sizeof(queue_message_t)

// ==== Message Type ====
typedef struct {
    int id;
    char message[50];
    uint32_t timestamp; // Tick count when enqueued
} queue_message_t;

// ==== Globals ====
static QueueHandle_t xQueue = NULL;

// ==== Sender Task ====
static void sender_task(void *pvParameters) {
    queue_message_t msg;
    int counter = 0;

    ESP_LOGI(TAG, "Sender task started");

    while (1) {
        // เตรียมข้อมูล
        msg.id = counter++;
        snprintf(msg.message, sizeof(msg.message), "Hello from sender #%d", msg.id);
        msg.timestamp = xTaskGetTickCount();

        // ---- Queue Overflow Protection (non-blocking send) ----
        if (xQueueSend(xQueue, &msg, 0) != pdPASS) {
            ESP_LOGW(TAG, "Queue full! Dropping message ID=%d", msg.id);
        } else {
            ESP_LOGI(TAG, "Sent: ID=%d, MSG=%s, Time=%lu",
                     msg.id, msg.message, (unsigned long)msg.timestamp);

            // กระพริบไฟฝั่งผู้ส่ง
            gpio_set_level(LED_SENDER, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(LED_SENDER, 0);
        }

        // ส่งทุก ๆ 500 ms
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// ==== Receiver Task ====
static void receiver_task(void *pvParameters) {
    queue_message_t rx;

    ESP_LOGI(TAG, "Receiver task started");

    while (1) {
        // ---- Non-blocking Receive ----
        if (xQueueReceive(xQueue, &rx, 0) == pdPASS) {
            ESP_LOGI(TAG, "Received: ID=%d, MSG=%s, Time=%lu",
                     rx.id, rx.message, (unsigned long)rx.timestamp);

            // กระพริบไฟฝั่งผู้รับ
            gpio_set_level(LED_RECEIVER, 1);
            vTaskDelay(pdMS_TO_TICKS(200));
            gpio_set_level(LED_RECEIVER, 0);

            // จำลองการประมวลผล
            vTaskDelay(pdMS_TO_TICKS(100));
        } else {
            // ไม่มีข้อความในคิว -> ไปทำงานอย่างอื่น
            ESP_LOGI(TAG, "No message available, doing other work...");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

// ==== Queue Monitor Task ====
static void queue_monitor_task(void *pvParameters) {
    ESP_LOGI(TAG, "Queue monitor task started");

    while (1) {
        UBaseType_t waiting  = uxQueueMessagesWaiting(xQueue);
        UBaseType_t freeSlot = uxQueueSpacesAvailable(xQueue);

        ESP_LOGI(TAG, "Queue Status - Messages: %u, Free spaces: %u",
                 (unsigned)waiting, (unsigned)freeSlot);

        // แสดง bar เล็ก ๆ ให้เห็นความเต็มของคิว (ความยาว 5)
        printf("Queue: [");
        for (UBaseType_t i = 0; i < QUEUE_LENGTH; i++) {
            printf(i < waiting ? "■" : "□");
        }
        printf("]\n");

        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

// ==== App Main ====
void app_main(void) {
    ESP_LOGI(TAG, "Basic Queue Operations Lab Starting...");

    // ตั้งค่า GPIO
    gpio_set_direction(LED_SENDER, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_RECEIVER, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_SENDER, 0);
    gpio_set_level(LED_RECEIVER, 0);

    // สร้างคิว (ความจุ 5 ข้อความ)
    xQueue = xQueueCreate(QUEUE_LENGTH, QUEUE_ITEMSIZE);

    if (xQueue == NULL) {
        ESP_LOGE(TAG, "Failed to create queue!");
        return;
    }

    ESP_LOGI(TAG, "Queue created successfully (size: %d messages)", QUEUE_LENGTH);

    // สร้าง Tasks
    // - ให้ sender priority สูงกว่าเล็กน้อยเพื่อเน้น overflow ได้ง่ายเมื่อ receiver ช้าลง
    xTaskCreate(sender_task, "Sender", 2048, NULL, 3, NULL);
    xTaskCreate(receiver_task, "Receiver", 2048, NULL, 2, NULL);
    xTaskCreate(queue_monitor_task, "Monitor", 2048, NULL, 1, NULL);

    ESP_LOGI(TAG, "All tasks created. Scheduler running.");
}
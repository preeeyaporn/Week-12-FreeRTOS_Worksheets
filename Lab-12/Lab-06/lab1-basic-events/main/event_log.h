#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
  uint32_t    ts_ms;
  EventBits_t before_bits;
  EventBits_t set_bits;
  EventBits_t after_bits;
  const char* source;
} ev_record_t;

bool   evlog_init(size_t capacity);          // init ring + internal queue
void   evlog_add(EventGroupHandle_t g, EventBits_t set_bits, const char* src); // non-block send to logger
size_t evlog_dump(ev_record_t *out, size_t max);

// Perf 5.2: worker task (ทำงานเบื้องหลัง)
void   evlog_worker_task(void *pv);
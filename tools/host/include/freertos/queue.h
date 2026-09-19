#pragma once
#include "FreeRTOS.h"
typedef struct { unsigned size, capacity, head, tail, count; uint8_t *data; } StaticQueue_t;
typedef StaticQueue_t *QueueHandle_t;
static inline QueueHandle_t xQueueCreateStatic(unsigned count, unsigned size,
    uint8_t *data, StaticQueue_t *q) { *q = (StaticQueue_t){size, count, 0, 0, 0, data}; return q; }
static inline int xQueueSend(QueueHandle_t q, const void *data, unsigned timeout) {
    if(q->count == q->capacity) return 0;
    memcpy(q->data + q->tail * q->size, data, q->size);
    q->tail = (q->tail + 1) % q->capacity; ++q->count; return 1;
}
static inline int xQueueReceive(QueueHandle_t q, void *data, unsigned timeout) {
    if(!q->count) return 0;
    memcpy(data, q->data + q->head * q->size, q->size);
    q->head = (q->head + 1) % q->capacity; --q->count; return 1;
}

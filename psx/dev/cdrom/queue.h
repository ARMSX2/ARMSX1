#ifndef QUEUE_H
#define QUEUE_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint8_t* buf;
    size_t read_index;
    size_t write_index;
    size_t size;
} queue_t;

queue_t* queue_create(void);
void queue_init(queue_t* queue, size_t size);
void queue_push(queue_t* queue, uint8_t value);
uint8_t queue_pop(queue_t* queue);
uint8_t queue_peek(queue_t* queue);
int queue_is_empty(queue_t* queue);
int queue_is_full(queue_t* queue);
void queue_reset(queue_t* queue);
void queue_clear(queue_t* queue);
int queue_size(queue_t* queue);
int queue_max_size(queue_t* queue);
void queue_destroy(queue_t* queue);

#endif
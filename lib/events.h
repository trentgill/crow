#pragma once

#include <stdint.h>

union Data{
    void* p;
    int i;
    float f;
    uint8_t u8s[4];
};

typedef struct event{
    void (*handler)( struct event* e );
    union Data   index;
    union Data   data;
} event_t;

extern void events_init(void);
extern void events_clear(void);
extern uint8_t event_post(event_t *e);
extern void event_next( void );
extern int event_queue_space( void );

// events.c adapted from github.com/monome/libavr32

#include <stdio.h>
#include <stm32f7xx.h>
#include "events.h"
#include "lualink.h"
#include "caw.h" // Caw_send_luachunk
#include "telemetry.h"


/// NOTE: if we are ever over-filling the event queue, we have problems.
/// making the event queue bigger not likely to solve the problems.
#define MAX_EVENTS   40

// macro for incrementing an index into a circular buffer.
#define INCR_EVENT_INDEX( x )  { if ( ++x == MAX_EVENTS ) x = 0; }

// get/put indexes into sysEvents[] array
volatile static int putIdx = 0;
volatile static int getIdx = 0;

// the system event queue is a circular array of event records
// NOTE be aware of event_t and MAX_EVENTS for RAM usage
volatile static event_t sysEvents[ MAX_EVENTS ];

// initialize event handler
void events_init() {
    printf("\ninitializing event handler\n");

    events_clear();
}

void events_clear(void)
{
    // set queue (circular list) to empty
    putIdx = 0;
    getIdx = 0;

    // zero out the event records
    for ( int k = 0; k < MAX_EVENTS; k++ ) {
        sysEvents[ k ].data.i  = 0;
        sysEvents[ k ].handler = NULL;
    }
}

// get next event
// returns non-zero if an event was available
void event_next( void ){
    event_t* e = NULL;

    BLOCK_IRQS(
        // if pointers are equal, the queue is empty... don't allow idx's to wrap!
        if ( getIdx != putIdx ) {
            INCR_EVENT_INDEX( getIdx );
            e = (event_t*)&sysEvents[ getIdx ];
        }
    );

    if( e != NULL ){ (*e->handler)(e); } // call the event handler after enabling IRQs
}


// add event to queue, return success status
uint8_t event_post( event_t *e ) {
    uint8_t status = 0;

    BLOCK_IRQS(
        // increment write idx, posbily wrapping
        int saveIndex = putIdx;
        INCR_EVENT_INDEX( putIdx );
        if ( putIdx != getIdx  ) {
            sysEvents[ putIdx ].handler = e->handler;
            sysEvents[ putIdx ].index   = e->index;
            sysEvents[ putIdx ].data    = e->data;
            status = 1;
        } else {
            // idx wrapped, so queue is full, restore idx
            putIdx = saveIndex;
        }
    );

    Telemetry_event_depth( (putIdx - getIdx + MAX_EVENTS) % MAX_EVENTS );
    if( !status ){
        Telemetry_count( TM_event_full, 1 );
        printf("event queue full!\n");
        Caw_send_luachunk("event queue full!");
    }

    return status;
}

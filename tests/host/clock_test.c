// host test for the clock_update() resume cap, using the real lib/clock.c,
// lib/clock_ll.c and lib/events.c. run: sh tests/host/run.sh
//
// the main loop is simulated as: one clock_update() per ms, then a fixed
// number of event_next() calls (main-loop passes) per ms.

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "lib/clock.h"
#include "lib/events.h"

#define MAX_IDS     128
#define QUEUE_SIZE  39 // usable slots of events.c's MAX_EVENTS

static uint32_t fake_tick = 0;
uint32_t HAL_GetTick( void ){ return fake_tick; }

static int queue_full = 0;
void Caw_send_luachunk( char* text ){ if( strstr( text, "queue full" ) ){ queue_full++; } }

static int order[1024], n_order;
static int resumed[MAX_IDS];
static int resumes_this_tick, max_resumes_per_tick, max_used;
static int others_posted, others_handled;

static void handle_resume( event_t* e ){ order[n_order++] = e->index.i; resumed[e->index.i]++; }
static void handle_other( event_t* e ){ others_handled++; }

// the lualink.c side of clock.c
void L_queue_clock_resume( int coro_id )
{
    event_t e = { .handler = handle_resume, .index.i = coro_id };
    if( event_post( &e ) ){ resumes_this_tick++; }
}
void L_queue_tempo_change( float tempo ){}
void L_queue_clock_start( void ){}
void L_queue_clock_stop( void ){}

static void post_other( void )
{
    event_t e = { .handler = handle_other };
    if( event_post( &e ) ){ others_posted++; }
}

static int failures = 0;
#define CHECK( cond ) do{ if( !(cond) ){ failures++; \
    printf( "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond ); } }while(0)

static void reset( void )
{
    n_order = 0;
    memset( resumed, 0, sizeof resumed );
    max_resumes_per_tick = max_used = 0;
    others_posted = others_handled = 0;
    queue_full = 0;
}

// run the main loop for ms milliseconds
static void run_ms( int ms, int passes_per_ms, int others_per_ms )
{
    while( ms-- ){
        fake_tick++;
        resumes_this_tick = 0;
        clock_update( fake_tick );
        for( int i = 0; i < others_per_ms; i++ ){ post_other(); }
        int used = QUEUE_SIZE - event_queue_space();
        if( used > max_used ){ max_used = used; }
        if( resumes_this_tick > max_resumes_per_tick ){ max_resumes_per_tick = resumes_this_tick; }
        for( int i = 0; i < passes_per_ms; i++ ){ event_next(); }
    }
}

static int all_once( int n )
{
    for( int i = 0; i < n; i++ ){ if( resumed[i] != 1 ){ return 0; } }
    return 1;
}

int main( void )
{
    events_init();
    clock_init( 100 );
    CHECK( event_queue_space() == QUEUE_SIZE );

    printf( "50 clocks waking in the same ms, 3 main-loop passes per ms\n" );
    reset();
    fake_tick = 1000;
    for( int i = 0; i < 50; i++ ){ clock_schedule_resume_sleep( i, 0.1 ); }
    run_ms( 200, 3, 0 );
    printf( "  resumed %d, max %d per tick, max queue %d, queue full %d\n"
          , n_order, max_resumes_per_tick, max_used, queue_full );
    CHECK( n_order == 50 && all_once( 50 ) );
    CHECK( queue_full == 0 );
    CHECK( max_resumes_per_tick <= 10 );
    for( int i = 0; i < 50; i++ ){ CHECK( order[i] == i ); } // wake (FIFO) order

    // the queue never drains below ~30 here, so the deferred clocks wait for
    // the drain at the end rather than push other events out of the queue.
    printf( "50 clocks, 1 pass per ms, other events posted every ms\n" );
    reset();
    for( int i = 0; i < 50; i++ ){ clock_schedule_resume_sleep( i, 0.1 ); }
    run_ms( 200, 1, 1 );
    run_ms( 50, 5, 0 ); // drain
    printf( "  resumed %d, max queue %d, queue full %d, others %d/%d\n"
          , n_order, max_used, queue_full, others_handled, others_posted );
    CHECK( n_order == 50 && all_once( 50 ) );
    CHECK( queue_full == 0 );
    CHECK( others_posted == 200 && others_handled == 200 );
    CHECK( max_used <= QUEUE_SIZE - 9 ); // room left for other events
    for( int i = 0; i < 50; i++ ){ CHECK( order[i] == i ); }

    printf( "60 clocks over 3 wake times: deferred clocks keep wake order\n" );
    reset();
    for( int i = 0; i < 60; i++ ){ clock_schedule_resume_sleep( i, 0.050 + (i % 3) * 0.001 ); }
    run_ms( 100, 1, 0 );
    run_ms( 50, 5, 0 );
    CHECK( n_order == 60 && all_once( 60 ) );
    CHECK( queue_full == 0 );
    { int k = 0;
      for( int m = 0; m < 3; m++ ){
          for( int i = m; i < 60; i += 3 ){ CHECK( order[k++] == i ); }
      }
    }

    printf( "50 synced clocks plus 20 sleepers on the same beat\n" );
    reset();
    for( int i = 0; i < 50; i++ ){ clock_schedule_resume_sync( i, 1.0, 0.0 ); }
    float to_beat = (float)(1.0 - fmod( clock_get_time_beats(), 1.0 )) * 0.5f; // s at 120bpm
    for( int i = 50; i < 70; i++ ){ clock_schedule_resume_sleep( i, to_beat ); }
    run_ms( 1200, 2, 0 );
    run_ms( 50, 5, 0 );
    printf( "  resumed %d, max %d per tick, queue full %d\n", n_order, max_resumes_per_tick, queue_full );
    CHECK( n_order == 70 && all_once( 70 ) );
    CHECK( queue_full == 0 );
    CHECK( max_resumes_per_tick <= 10 );

    printf( "queue already busy: clocks wait for space, none lost\n" );
    reset();
    for( int i = 0; i < 35; i++ ){ post_other(); }
    for( int i = 0; i < 20; i++ ){ clock_schedule_resume_sleep( i, 0.01 ); }
    run_ms( 20, 0, 0 ); // nothing drains: clocks are due but must not post
    CHECK( n_order == 0 && event_queue_space() == QUEUE_SIZE - 35 );
    run_ms( 100, 1, 0 );
    run_ms( 50, 5, 0 );
    CHECK( n_order == 20 && all_once( 20 ) );
    CHECK( others_handled == 35 && queue_full == 0 );

    printf( failures ? "clock_test: %d FAILED\n" : "clock_test: ok\n", failures );
    return failures ? 1 : 0;
}

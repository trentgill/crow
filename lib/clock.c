#include "clock.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "lualink.h"
#include <stm32f7xx_hal.h> // HAL_GetTick
#include "clock_ll.h" // linked list for clocks

#include "caw.h" // Caw_printf
#include "events.h" // event_queue_space

// clock_update() resumes at most this many clocks per tick. the rest stay in
// their list (in wake order) and resume on following ticks.
#define CLOCK_RESUMES_PER_TICK 10

// and never fills the event queue past this many free slots, leaving room for
// other events (metro, input, asl, ii). the main loop only handles one event
// per pass, so the cap alone can still overflow the queue if Lua is slow.
#define CLOCK_EVENT_RESERVE    10

///////////////////////////////
// private types

typedef struct{
    // clock_update_reference is called every beat, so we're constantly updating beat & last_beat_time
    double beat; // set in clock_update_reference(beat,_). this is the refence beat (ie. count since start)
    double last_beat_time; // seconds_since_boot at last clock_update_reference() call
    double beat_duration_inverse; // inverse of beat_duration
    float  beat_duration; // seconds_per_beat (ie tempo)
} clock_reference_t;

typedef struct{
    double wakeup;
    int    coro_id;
    bool   running;
} clock_thread_HD_t;

////////////////////////////////////
// global data

static clock_thread_HD_t internal; // for internal clocksource
static clock_source_t clock_source = CLOCK_SOURCE_INTERNAL;

static clock_reference_t reference;

// fp64 representation of beat count with a floating sub-beat count
static double precise_beat_now = 0;

/////////////////////////////////////////////
// private declarations

static void clock_internal_run(uint32_t ms);

/////////////////////////////////////////////
// public defs

void clock_init( int max_clocks )
{
    ll_init(max_clocks); // init linked-list for managing clock threads

    clock_set_source( CLOCK_SOURCE_INTERNAL );
    clock_update_reference(0, 0.5); // set to zero beats, at 120bpm (0.5s/beat)

    // start clock sources
    clock_internal_init();
    clock_crow_init();
}

static double precision_beat_of_now(uint32_t time_now){
    double now_seconds = (double)time_now * (double)0.001; // ms to seconds
    double time_since_beat = now_seconds - reference.last_beat_time;
    double beat_fraction = time_since_beat * reference.beat_duration_inverse;
    return reference.beat + beat_fraction;
}

// This function must only be called when time_now changes!
// TIME SENSITIVE. this function is run every 1ms, so optimize it for speed.
void clock_update(uint32_t time_now)
{
    // increments the beat count if we've crossed into the next beat
    clock_internal_run(time_now);

    // calculate the fp64 beat count for .syncing checks
    precise_beat_now = precision_beat_of_now(time_now);

    // TODO can we use <= for time comparison or does it create double-trigs?
    // this should reduce latency by 1ms if it works.
    double dtime_now = (double)time_now;
    int resume_count = CLOCK_RESUMES_PER_TICK;
sleep_next:
    if(resume_count > 0
    && event_queue_space() > CLOCK_EVENT_RESERVE
    && sleep_head // list is not empty
    && sleep_head->wakeup < dtime_now){ // time to awaken
        // pop the node before queueing the resume, so the list is consistent
        int id = sleep_head->coro_id;
        ll_insert_idle(ll_pop(&sleep_head)); // return to idle list
        L_queue_clock_resume(id); // event!
        resume_count--;
        goto sleep_next; // check the next sleeper too!
    }
sync_next:
    if(resume_count > 0
    && event_queue_space() > CLOCK_EVENT_RESERVE
    && sync_head // list is not empty
    && sync_head->wakeup < precise_beat_now){ // time to awaken
        int id = sync_head->coro_id;
        ll_insert_idle(ll_pop(&sync_head)); // return to idle list
        L_queue_clock_resume(id); // event!
        resume_count--;
        goto sync_next; // check the next syncer too!
    }
}

bool clock_schedule_resume_sleep( int coro_id, float seconds )
{
    double wakeup = (double)HAL_GetTick() + (double)seconds * (double)1000.0;
    return ll_insert_event(&sleep_head, coro_id, wakeup);
}

bool clock_schedule_resume_sync( int coro_id, float beats, float offset ){
    double dbeats = beats;

    // modulo sync time against base beat
    double awaken = floor(reference.beat / dbeats);
    awaken *= dbeats;
    awaken += dbeats;
    awaken += (double)offset;

    // check we haven't already passed it in the sub-beat & add another step if we have
    // we have to loop because fractional beats values may occur >2 times within a beat
    while(awaken <= precise_beat_now){
        awaken += dbeats;
    }

    return ll_insert_event(&sync_head, coro_id, awaken);
}

// this function directly sleeps for an amount of beats (not sync'd to the beat)
bool clock_schedule_resume_beatsync( int coro_id, float beats ){
    return clock_schedule_resume_sleep(coro_id, beats * reference.beat_duration);
}

void clock_update_reference(double beats, double beat_duration)
{
    float prev_beat_duration = reference.beat_duration;
    reference.beat_duration         = beat_duration;
    reference.beat_duration_inverse = (double)1.0 / (double)beat_duration; // for optimized precision_beat_of_now (called every ms)
    reference.last_beat_time        = clock_get_time_seconds(); // seconds since system boot
    reference.beat                  = beats;

    // external clock can easily result in fluctuations below this threshold
    if(fabsf((float)beat_duration - prev_beat_duration) >= 0.00001){
        L_queue_tempo_change(60.0 * (float)reference.beat_duration_inverse);
    }
}

void clock_update_reference_from(double beats, double beat_duration, clock_source_t source)
{
    if( clock_source == source ){
        clock_update_reference( beats, beat_duration );
    }
}

void clock_start_from( clock_source_t source )
{
    if( clock_source == source ){
        L_queue_clock_start();
    }
}

void clock_stop_from( clock_source_t source )
{
    if( clock_source == source ){
        L_queue_clock_stop();
    }
}

void clock_set_source( clock_source_t source )
{
    if( source >= 0 && source < CLOCK_SOURCE_LIST_LENGTH ){
        clock_source = source;
    }
}

float clock_get_time_beats(void)
{
    return (float)precise_beat_now;
}

double clock_get_time_seconds(void)
{
    return (double)HAL_GetTick() / (double)1000.0;
}

float clock_get_tempo(void)
{
    return 60.0 * (float)reference.beat_duration_inverse;
}

void clock_cancel_coro( int coro_id )
{
    ll_remove_by_id(coro_id);
}

void clock_cancel_coro_all( void )
{
    ll_cleanup();
}


/////////////////////////////////////////////////
// in clock_internal.h

static double internal_interval_seconds; // seconds. defined by internal BPM
static double internal_beat; // set by clock_internal_start(beat,_). count of beats since 0

void clock_internal_init(void)
{
    internal.running = false;
    clock_internal_set_tempo(120);
    clock_internal_start(0.0, true);
}

void clock_internal_set_tempo( float bpm )
{
    internal_interval_seconds = (double)60.0 / (double)bpm;
    clock_internal_start( internal_beat, false );
}

void clock_internal_start( float new_beat, bool transport_start )
{
    internal_beat = new_beat;
    clock_update_reference_from( internal_beat
                               , internal_interval_seconds
                               , CLOCK_SOURCE_INTERNAL );

    if( transport_start ){
        clock_start_from( CLOCK_SOURCE_INTERNAL ); // user callback
    }
    internal.wakeup  = 0; // force event
    internal.running = true;
}

void clock_internal_stop(void)
{
    internal.running = false; // actually stop the sync clock
    clock_stop_from( CLOCK_SOURCE_INTERNAL ); // user callback
}


/////////////////////////////////////
// private clock_internal

// note how we have to track the quantization error of the clock over cycles
// long-term precision is accurate to a double, while each clock pulse will
// be quantized to the tick *before* it's absolute position.
// this is important so that the beat division counter leads the userspace
// sync() calls & ensures they don't double-trigger.
static void clock_internal_run(uint32_t ms)
{
    static double error = 0.0; // track ms error across clock pulses
    if( internal.running ){
        double time_now = ms;
        if( internal.wakeup < time_now ){ // TODO can this be <= for 1ms less latency?
            internal_beat += 1;
            clock_update_reference_from( internal_beat
                                       , internal_interval_seconds
                                       , CLOCK_SOURCE_INTERNAL );
            internal.wakeup = time_now + internal_interval_seconds * (double)1000.0;
            error += 1+ floor(internal.wakeup) - internal.wakeup;
            if(error > (double)0.0){
                internal.wakeup -= (double)1.0;
                error--;
            }
        }
    }
}


/////////////////////////////////////////////////
// in clock_input.h

static bool clock_crow_last_time_set;
static int clock_crow_counter;
static double clock_crow_last_time;

#define DURATION_BUFFER_LENGTH 4

static float duration_buf[DURATION_BUFFER_LENGTH] = {0};
static uint8_t beat_duration_buf_pos = 0;
static uint8_t beat_duration_buf_len = 0;
static float mean_sum;
static float mean_scale;

static float crow_in_div = 4.0;


void clock_crow_init(void)
{
    clock_crow_counter = 0;
    clock_crow_last_time_set = false;
    mean_sum = 0;
}

// called by an event received on input
void clock_input_handler( int id, float freq )
{
    // this stub function just ignores the args
    clock_crow_handle_clock();
}
void clock_crow_handle_clock(void)
{
    float beat_duration;
    double current_time = clock_get_time_seconds();

    if( clock_crow_last_time_set == false ){
        clock_crow_last_time_set = true;
        clock_crow_last_time = current_time;
    } else {
        beat_duration = (float)(current_time - clock_crow_last_time)
                            * crow_in_div;
        if( beat_duration > 4.0 ){ // assume clock stopped
            clock_crow_last_time = current_time;
        } else {
            if( beat_duration_buf_len < DURATION_BUFFER_LENGTH ){
                beat_duration_buf_len++;
                mean_scale = 1.0 / beat_duration_buf_len;
            }

            float a = beat_duration * mean_scale;
            mean_sum = mean_sum + a;
            mean_sum = mean_sum - duration_buf[beat_duration_buf_pos];
            duration_buf[beat_duration_buf_pos] = a;
            beat_duration_buf_pos = (beat_duration_buf_pos + 1) % DURATION_BUFFER_LENGTH;

            clock_crow_counter++;
            clock_crow_last_time = current_time;

            double beat = clock_crow_counter / crow_in_div;
            clock_update_reference_from(beat, (double)mean_sum, CLOCK_SOURCE_CROW);
        }
    }
}

void clock_crow_in_div( float div )
{
    crow_in_div = 1.0/div;
}

double clock_get_crow_last_time(void)
{
    if( clock_crow_last_time_set == false ){
        return -1.0; // No clock received yet
    }
    return clock_get_time_seconds() - clock_crow_last_time;
}

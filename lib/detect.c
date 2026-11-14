#include "detect.h"
#include "freqm.h"
#include "../ll/ads131.h" // ADC_SAMPLE_RATE

#include <stdlib.h>
#include <math.h>
#include <stdio.h>

uint8_t channel_count = 0;

Detect_t*  selves = NULL;

// helpers
static void scale_bounds( Detect_t* self, int ix, int oct );

////////////////////////////////////////////////
// signal processor declarations

static void d_none( Detect_t* self, float level );
static void d_stream( Detect_t* self, float level );
static void d_change( Detect_t* self, float level );
static void d_window( Detect_t* self, float level );
static void d_scale( Detect_t* self, float level );
static void d_volume( Detect_t* self, float level );
static void d_peak( Detect_t* self, float level );
static void d_freq( Detect_t* self, float level );


///////////////////////////////////////////
// init

void Detect_init( int channels )
{
    channel_count = channels;
    selves = malloc( sizeof ( Detect_t ) * channels );
    for( int j=0; j<channels; j++ ){
        selves[j].channel = j;
        selves[j].action  = NULL;
        selves[j].last    = 0.0;
        selves[j].state   = 0;
        Detect_none( &(selves[j]) );
        selves[j].win.lastWin = 0;
        selves[j].vu = VU_init();
    }
}

void Detect_deinit( void )
{
    free(selves); selves = NULL;
}


/////////////////////////////////////////
// global helpers

Detect_t* Detect_ix_to_p( uint8_t index )
{
    if( index < 0 || index >= channel_count ){ return NULL; } // TODO error msg
    return &(selves[index]);
}

int8_t Detect_str_to_dir( const char* str )
{
    if( *str == 'r' ){ return 1; }
    else if( *str == 'f'){ return -1; }
    else{ return 0; } // default to 'both'
}


//////////////////////////////////////////
// mode configuration


void Detect_none( Detect_t* self )
{
    FreqM_track( self->channel, 0.0f ); // stop any freq tracking
    self->modefn = d_none;
}

void Detect_stream( Detect_t*         self
                  , Detect_callback_t cb
                  , float             interval
                  )
{
    FreqM_track( self->channel, 0.0f ); // stop any freq tracking
    self->modefn         = d_stream;
    self->action         = cb;
    // SAMPLE_RATE * i / BLOCK_SIZE
    self->stream.blocks  = (int)((48000.0 * interval) / 32.0);
    if( self->stream.blocks <= 0 ){ self->stream.blocks = 1; }
    self->stream.countdown = self->stream.blocks;
}

void Detect_change( Detect_t*         self
                  , Detect_callback_t cb
                  , float             threshold
                  , float             hysteresis
                  , int8_t            direction
                  )
{
    FreqM_track( self->channel, 0.0f ); // stop any freq tracking
    self->modefn            = d_change;
    self->action            = cb;
    self->change.threshold  = threshold;
    self->change.hysteresis = hysteresis;
    self->change.direction  = direction;
    // TODO need to reset state params?
    // can force update based on global struct members?
}

static void scale_bounds( Detect_t* self, int ix, int oct )
{
    D_scale_t* s = &self->scale; // readability

    // find ideal voltage for this window
    float ideal = ((float)oct * s->scaling) + ix * s->win;
    ideal = ideal - s->offset; // shift start of window down

    // calculate bounds
    s->lower = ideal - s->hyst;
    s->upper = ideal + s->hyst + s->win;
}

void Detect_scale( Detect_t*         self
                 , Detect_callback_t cb
                 , float*            scale
                 , int               sLen
                 , float             divs
                 , float             scaling
                 )
{
    FreqM_track( self->channel, 0.0f ); // stop any freq tracking
    self->modefn = d_scale;
    self->action = cb;

    D_scale_t* s = &self->scale; // readability

    s->sLen    = (sLen > SCALE_MAX_COUNT) ? SCALE_MAX_COUNT : sLen;
    s->divs    = divs;
    s->scaling = scaling;

    if( sLen == 0 ){ // assume chromatic
        s->sLen = (divs > SCALE_MAX_COUNT) ? SCALE_MAX_COUNT : (int)divs;
        for( int i=0; i<(s->sLen); i++ ){
            s->scale[i] = (float)i; // build chromatic
        }
    } else {
        for( int i=0; i<(s->sLen); i++ ){
            s->scale[i] = *scale++; // copy array into local struct
        }
    }

    // nb: 'div' refers to divisions of the octave (eg. 12 in 12TET)
    //     'window' refers to divisions of the scale (eg. 7 in major)
    s->offset = 0.5 * scaling / divs; // half a div, in volts
    s->win = scaling / ((float)s->sLen); // a window in terms of voltage
    s->hyst = s->win / 20.0; // 5% hysteresis on either side of window
    s->hyst = s->hyst < 0.006 ? 0.006 : s->hyst; // clamp to 1LSB at 12bit

    scale_bounds(self, 0, -10); // set to invalid note
}

void Detect_window( Detect_t*         self
                  , Detect_callback_t cb
                  , float*            windows
                  , int               wLen
                  , float             hysteresis
                  )
{
    FreqM_track( self->channel, 0.0f ); // stop any freq tracking
    printf("TODO need to sort the windows!\n");
    self->modefn         = d_window;
    self->action         = cb;
    self->win.wLen       = (wLen > WINDOW_MAX_COUNT) ? WINDOW_MAX_COUNT : wLen;
    self->win.hysteresis = hysteresis;
    for( int i=0; i<self->win.wLen; i++ ){
        self->win.windows[i] = *windows++;
    }
}

void Detect_volume( Detect_t*         self
                  , Detect_callback_t cb
                  , float             interval
                  )
{
    FreqM_track( self->channel, 0.0f ); // stop any freq tracking
    self->modefn         = d_volume;
    self->action         = cb;

    VU_time( self->vu, 0.018 );
    // SAMPLE_RATE * i / BLOCK_SIZE
    self->volume.blocks  = (int)((48000.0 * interval) / 32.0);
    if( self->volume.blocks <= 0 ){ self->volume.blocks = 1; }
    self->volume.countdown = self->volume.blocks;
}

void Detect_peak( Detect_t*         self
                , Detect_callback_t cb
                , float             threshold
                , float             hysteresis
                )
{
    FreqM_track( self->channel, 0.0f ); // stop any freq tracking
    self->modefn            = d_peak;
    self->action            = cb;
    // TODO perhaps a abs->2lpf (no RMS averaging) is better?
    // set 30ms settling time for peak envelope behaviour
    VU_time( self->vu, 0.18 );
    self->peak.threshold  = threshold;
    self->peak.hysteresis = hysteresis;
    self->peak.release    = 0.01; // TODO tune this
    self->peak.envelope   = 0.0;
}

void Detect_freq( Detect_t*         self
                , Detect_callback_t cb
                , float             interval
                )
{
    // both channels: zero-crossing timing on the continuous ADC stream (freqm.c)
    // `interval` is both the measurement window and the report rate
    if( interval < 0.01f ){ interval = 0.01f; }
    self->modefn = d_freq;
    self->action = cb;
    self->stream.blocks  = (int)((ADC_SAMPLE_RATE * interval) / 32.0f);
    if( self->stream.blocks <= 0 ){ self->stream.blocks = 1; }
    self->stream.countdown = self->stream.blocks;
    FreqM_track( self->channel, interval );
}

//////////////////////////////////////////////
// signal processors
static void d_none( Detect_t* self, float level ){ return; }

static void d_stream( Detect_t* self, float level )
{
    if( --self->stream.countdown <= 0 ){
        self->stream.countdown = self->stream.blocks; // reset counter
        (*self->action)( self->channel
                       , level
                       ); // callback!
    }
}

static void d_change( Detect_t* self, float level )
{
    if( self->state ){ // high to low
        if( level < (self->change.threshold - self->change.hysteresis) ){
            self->state = 0;
            if( self->change.direction != 1 ){ // not 'rising' only
                (*self->action)( self->channel, (float)self->state );
            }
        }
    } else { // low to high
        if( level > (self->change.threshold + self->change.hysteresis) ){
            self->state = 1;
            if( self->change.direction != -1 ){ // not 'falling' only
                (*self->action)( self->channel, (float)self->state );
            }
        }
    }
}

static void d_window( Detect_t* self, float level )
{
    // search index containing 'level'
    int ix = 0;
        // TODO optimize: start from 'lastWin' rather than 0
    for(; ix<self->win.wLen; ix++ ){
        if( level < self->win.windows[ix] ){
            break;
        }
    }
    ix++; // 1-base the index so it can be passed with sign
    // compare the found win with 'lastWin'
    int lW = self->win.lastWin;
    if( ix != lW ){ // window has changed
        (*self->action)( self->channel
                       , (ix > lW) // sign of index determines direction
                            ? ix
                            : -ix
                       ); // callback!
        self->win.lastWin = ix; // save newly entered window
    }
}

static void d_scale( Detect_t* self, float level )
{
    D_scale_t* s = &self->scale; // readability

    if( level > s->upper
     || level < s->lower ){
        // offset input to ensure we capture noisy notes at the divs
        level += self->scale.offset;

        // calculate index of input
        float norm   = level / s->scaling;       // normalize scaling
        s->lastOct   = (int)floorf(norm);        // # of folds around scaling
        float phase  = norm - (float)s->lastOct; // position in win [0,1.0)
        float fix    = phase * s->sLen;          // map phase to #scale
        s->lastIndex = (int)floorf(fix);         // select octave at or beneath selection

        // perform scale lookup & prepare outs
        float note    = s->scale[s->lastIndex]; // lookup within octave
        s->lastNote  = note + (float)s->lastOct * s->divs;
        s->lastVolts = (note/s->divs + (float)s->lastOct) * s->scaling;

        // call action
        (*self->action)( self->channel, 0.0 ); // callback! 0.0 is ignored

        // calculate new bounds
        scale_bounds(self, s->lastIndex, s->lastOct);
    }
}

static void d_volume( Detect_t* self, float level )
{
    level = VU_step( self->vu, level );
    if( --self->volume.countdown <= 0 ){
        self->volume.countdown = self->volume.blocks; // reset counter
        (*self->action)( self->channel, level ); // callback!
    }
}

static void d_peak( Detect_t* self, float level )
{
    level = VU_step( self->vu, level );
    if( level > self->last ){ // instant attack
        self->peak.envelope = level;
    } else { // release as 1lpf slew
        self->peak.envelope = level + self->peak.release
                                      * (self->peak.envelope - level);
    }

    if( self->state ){ // high to low
        if( self->peak.envelope < (self->peak.threshold - self->peak.hysteresis) ){
            self->state = 0;
        }
    } else { // low to high
        if( self->peak.envelope > (self->peak.threshold + self->peak.hysteresis) ){
            self->state = 1;
            (*self->action)( self->channel, 0.0 ); // callback! 0.0 is ignored
        }
    }
}

static void d_freq( Detect_t* self, float level )
{
    float f = FreqM_tracked( self->channel );
    if( --self->stream.countdown <= 0 ){
        self->stream.countdown = self->stream.blocks; // reset counter
        (*self->action)( self->channel
                       , f
                       ); // callback!
    }
}

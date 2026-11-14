#pragma once

#include <stdint.h>

// Frequency measurement on the continuous 46.875kHz ADC stream.
//
// Rising zero crossings (relative to an adaptive mid-level, with hysteresis)
// are timed with sub-sample linear interpolation. A measurement window fits
// crossing time against crossing index (least squares): the slope is the
// period. Both input channels are measured over the same window.

#define FREQM_CHANNELS 2

typedef struct{
    float    freq;    // Hz (0 if fewer than 2 crossings)
    int      count;   // number of crossings used
    float    jitter;  // std-dev of individual periods, in cents
    float    vpp;     // peak-to-peak volts seen during the window
    float    resid;   // rms residual of the linear fit, in samples
    float    width;   // mean time from rising to falling mid crossing, seconds
} FreqM_result_t;

void FreqM_init( void );

// call regularly (every audio block) to consume new ADC frames
void FreqM_process( void );

// start a one-shot window on both channels, `seconds` long
void FreqM_start( float seconds );
// returns 1 when the window has completed and results are valid
int  FreqM_ready( void );
const FreqM_result_t* FreqM_result( int channel );

// continuous tracking for input.mode('freq'): rolling windows of `seconds`
void  FreqM_track( int channel, float seconds );
float FreqM_tracked( int channel ); // latest completed rolling result (Hz)

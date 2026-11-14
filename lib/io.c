#include "io.h"

#include <stdio.h>
#include "stm32f7xx_hal.h"     // HAL_Delay()

#include "../ll/adda.h"        // _Init(), _Start(), _GetADCValue(), IO_block_t
#include "slopes.h"            // S_init(), S_step_v()
#include "ashapes.h"           // AShaper_init(), AShaper_v()
#include "detect.h"            // Detect_init(), Detect(), Detect_ix_to_p()
#include "metro.h"
#include "caw.h"
#include "casl.h"
#include "freqm.h"

#include "lualink.h"           // L_handle_in_stream (pass this in as ptr?)

#define IN_CHANNELS ADDA_ADC_CHAN_COUNT

static void public_update( void );

void IO_Init( int adc_timer_ix )
{
    // hardware layer
    ADDA_Init(adc_timer_ix);

    // dsp objects
    Detect_init( IN_CHANNELS );
    FreqM_init();
    for(int i=0; i<SLOPE_CHANNELS; i++){
        casl_init(i);
    }
    S_init( SLOPE_CHANNELS );
    AShaper_init( SLOPE_CHANNELS );
}

void IO_Start( void )
{
    ADDA_Start();
}

// DSP process
IO_block_t* IO_BlockProcess( IO_block_t* b )
{
    FreqM_process(); // consume new frames from the continuous ADC ring
    for( int j=0; j<IN_CHANNELS; j++ ){
        Detect_t* d = Detect_ix_to_p(j);
        (*d->modefn)( d, b->in[j][b->size-1] );
    }
    for( int j=0; j<SLOPE_CHANNELS; j++ ){
        S_step_v( j
                , b->out[j]
                , b->size
                );
    }
    for( int j=0; j<SLOPE_CHANNELS; j++ ){
        AShaper_v( j
                 , b->out[j]
                 , b->size
                 );
    }
    public_update();
    return b;
}
float IO_GetADC( uint8_t channel )
{
    return ADDA_GetADCValue( channel );
}
typedef enum{ In_none
            , In_stream
            , In_change
            , In_window
            , In_scale
            , In_quantize
            , In_justintonation
} In_mode_t;
static In_mode_t _parse_mode( const char* mode )
{
    if( *mode == 's' ){
        if( mode[1] == 'c' ){  return In_scale;
        } else {               return In_stream; }
    } else if( *mode == 'c' ){ return In_change;
    } else if( *mode == 'w' ){ return In_window;
    } else if( *mode == 'q' ){ return In_quantize;
    } else if( *mode == 'j' ){ return In_justintonation;
    } else {                   return In_none;
    }
}
void IO_SetADCaction( uint8_t channel, const char* mode )
{
    switch( _parse_mode(mode) ){
        case In_none:
            break;
        case In_stream:
            break;
        case In_change:
            break;
        case In_window:
            break;
        case In_scale:
            break;
        case In_quantize:
            break;
        case In_justintonation:
            break;
        default: break;
    }
    // set the appropriate fn to be called in ADC dsp loop
}

static bool view_chans[6] = {[0 ... 5]=false};
static float last_chans[6];
void IO_public_set_view( int chan, bool state )
{
    if(chan < 0 || chan >= 6){ return; }
    view_chans[chan] = state;
    if(state){ last_chans[chan] = -6.0; } // reset out of range, to force update
}

static void public_update( void )
{
    // boils down to ~15fps update per parameter, spread out evenly
    const float VDIFF = 0.1; // hysteresis distance. pretty coarse
    static int bcount = 0;
    static int chan = 0; // outputs*4 then inputs*2
    bcount++;
    if(bcount >= 16){ // time to send a new update
        bcount = 0;
        if(view_chans[chan]){
            if(chan<4){ // outputs
                float new = AShaper_get_state(chan);
                if( new + VDIFF < last_chans[chan]
                 || new - VDIFF > last_chans[chan] ){
                    last_chans[chan] = new;
                    Caw_printf("^^pubview('output',%i,%g)", chan+1, (double)new);
                }
            } else {
                float new = IO_GetADC(chan-4);
                if( new + VDIFF < last_chans[chan]
                 || new - VDIFF > last_chans[chan] ){
                    last_chans[chan] = new;
                    Caw_printf("^^pubview('input',%i,%g)", chan-3, (double)new);
                }
            }
        }
        chan = (chan + 1) % 6;
    }
}

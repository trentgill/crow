#include "adda.h"

#include <string.h>

#include "debug_pin.h"
#include "ads131.h"
#include "dac8565.h"

#include "../lib/io.h"     // IO_Slope_ptr_from_ix
#include "../lib/flash.h"  // FLASH_*_t
#include "cal_ll.h"        // CAL_LL_Init(),
#include "../lib/slopes.h" // S_toward()
#include "../lib/caw.h"    // Caw_send_raw

float _CAL_ADC_GetAverage( uint8_t chan );

typedef enum{ CAL_none
            , CAL_in_zero
            , CAL_in_scale
            , CAL_process_in_scale
            , CAL_in0_shift
            , CAL_in1_shift
            , CAL_process_in_shift
            , CAL_dac0_shift
            , CAL_dac0_scale
            , CAL_dac1_shift
            , CAL_dac1_scale
            , CAL_dac2_shift
            , CAL_dac2_scale
            , CAL_dac3_shift
            , CAL_dac3_scale
            , CAL_dac_scales
            , CAL_dac0_z
            , CAL_dac1_z
            , CAL_dac2_z
            , CAL_dac3_z
            , CAL_complete
} CAL_stage_t;

typedef struct {
    float shift;
    float scale;
} CAL_chan_t;

#define AVERAGE_COUNT 400
#define AVERAGE_IGNORE 40
#define AVERAGE_USABLE (AVERAGE_COUNT - AVERAGE_IGNORE)
typedef struct {
    CAL_chan_t  adc[2];
    CAL_chan_t  dac[4];
    CAL_stage_t stage;
    int16_t     avg_count;
} CAL_t;

CAL_t cal; // state of the calibration to be used directly, stored & recalled

CAL_stage_t CAL_is_calibrating( void );
IO_block_t* CAL_BlockProcess( IO_block_t* b );

void CAL_ReadFlash( void )
{
    if( !Flash_read_calibration( (uint8_t*)(&cal)
                               , sizeof(CAL_chan_t) * (2+4)
                               ) ){
        for( int j=0; j<2; j++ ){
            ADC_CalibrateShift( j, cal.adc[j].shift );
            ADC_CalibrateScalar( j, cal.adc[j].scale );
        }
        for( int j=0; j<4; j++ ){
            DAC_CalibrateOffset( j, cal.dac[j].shift );
            DAC_CalibrateScalar( j, cal.dac[j].scale );
        }
        //CAL_PrintCalibration();
        cal.stage = CAL_none;
    } else {
        printf("calibration readout failed\n");
    }
}

void CAL_WriteFlash( void )
{
    if( Flash_write_calibration( (uint8_t*)(&cal)
                               , sizeof(CAL_chan_t) * (2+4)
                               ) ){
        char msg[] = "Error saving Cal data to flash!\n";
        Caw_send_raw( (uint8_t*)msg, strlen(msg) );
        printf(msg);
    } else {
        char msg[] = "Calibration ok!\n\r";
        Caw_send_raw( (uint8_t*)msg, strlen(msg) );
        printf(msg);
    }
}

uint16_t ADDA_Init( void )
{
    ADC_Init( ADDA_BLOCK_SIZE
            , ADDA_ADC_CHAN_COUNT
            );
    DAC_Init( ADDA_BLOCK_SIZE
            , ADDA_DAC_CHAN_COUNT
            );
    CAL_LL_Init();
    cal.stage = CAL_none;
    if( !Flash_is_calibrated() ){ CAL_Recalibrate(0); }
    else{                         CAL_ReadFlash(); }
    return ADDA_BLOCK_SIZE;
}

void ADDA_Start( void )
{
    DAC_Start();
}

void ADDA_BlockProcess( uint32_t* dac_pickle_ptr )
{
    IO_block_t b = { .size = ADDA_BLOCK_SIZE };

    ADC_UnpickleBlock( b.in[0]
                     , ADDA_BLOCK_SIZE
                     );
    if( CAL_is_calibrating() ){ CAL_BlockProcess( &b ); }
    else {                      IO_BlockProcess( &b ); }
    DAC_PickleBlock( dac_pickle_ptr
                   , b.out[0]
                   , ADDA_BLOCK_SIZE
                   );
}

float ADDA_GetADCValue( uint8_t channel )
{
    return ADC_GetValue( channel );
}

__weak IO_block_t* IO_BlockProcess( IO_block_t* b )
{
    static float count = 1.0;
    for( uint16_t i=0; i<(b->size); i++ ){
        b->out[0][i] = b->in[0][i];
        b->out[1][i] = b->in[1][i];
        b->out[2][i] = 2.0;
        b->out[3][i] = 3.0;

        count += 0.9; if(count>=3.0){ count -= 6.0; }
    }
    return b;
}

// Calibration process

int CAL_ValidateData( void )
{
    int errorcode = 0;
    for( int j=0; j<2; j++ ){
        if( cal.adc[j].shift < -0.1
         || cal.adc[j].shift > 0.1
         || cal.adc[j].scale < 0.9
         || cal.adc[j].scale > 1.1 ){
            errorcode |= 1 << j;
        }
    }
    for( int j=0; j<4; j++ ){
        if( cal.dac[j].shift < -0.1
         || cal.dac[j].shift > 0.1
         || cal.dac[j].scale < 0.9
         || cal.dac[j].scale > 1.1 ){
            errorcode |= 1 << (2+j);
        }
    }
    return errorcode;
}

// hacked this in for testing input2
uint8_t CAL_StepB( IO_block_t* b, float* value )
{
    const float input_resistor = 100.0; // 100k
    const float mux_resistor   = 0.33 + 0.135; // 330r + MUX508 internal r
    float resistor_scale = (input_resistor + mux_resistor)
                                    / input_resistor;
    if( cal.avg_count == AVERAGE_COUNT ){
        cal.avg_count--;
        return 1; // FIXME gross logic flow here
    } else if( cal.avg_count <= AVERAGE_USABLE ){
        *value += b->in[1][b->size-1];
    }
    if( !(--cal.avg_count) ){
        *value /= AVERAGE_USABLE;
        *value *= resistor_scale;
        cal.stage++;
        cal.avg_count = AVERAGE_COUNT;
    }
    return 0;
}

// resistor values in kOhms
#define INPUT_RESISTOR 100.0
#define MUX_RESISTOR   0.33 // 0.66 + 0.135 + -0.5; // 2*330r + MUX508 internal r
#define RESISTOR_SCALE ((INPUT_RESISTOR + MUX_RESISTOR) / INPUT_RESISTOR)
uint8_t CAL_Step( IO_block_t* b, float* value )
{
    if( cal.avg_count == AVERAGE_COUNT ){
        cal.avg_count--;
        return 1; // FIXME gross logic flow here
    } else if( cal.avg_count <= AVERAGE_USABLE ){
        *value += b->in[0][b->size-1];
    }
    if( !(--cal.avg_count) ){
        *value /= AVERAGE_USABLE;
        *value *= RESISTOR_SCALE;
        cal.stage++;
        cal.avg_count = AVERAGE_COUNT;
    }
    return 0;
}

IO_block_t* CAL_BlockProcess( IO_block_t* b )
{
    const float vref           = 2.5;
    const float output_ref     = 7.0;
    Slope_t* s0 = IO_Slope_ptr_from_ix(0);
    Slope_t* s1 = IO_Slope_ptr_from_ix(0);
    Slope_t* s2 = IO_Slope_ptr_from_ix(0);
    Slope_t* s3 = IO_Slope_ptr_from_ix(0);
    switch( CAL_is_calibrating() ){
        case CAL_in_zero:
            if( CAL_Step( b, &(cal.adc[0].shift) ) ){
                S_toward( s0, 0.0, 0.0, SHAPE_Linear, NULL );
                S_toward( s1, 0.0, 0.0, SHAPE_Linear, NULL );
                S_toward( s2, 0.0, 0.0, SHAPE_Linear, NULL );
                S_toward( s3, 0.0, 0.0, SHAPE_Linear, NULL );
                CAL_LL_ActiveChannel( CAL_LL_Ground ); } break;

        case CAL_in_scale:
            if( CAL_Step( b, &(cal.adc[0].scale) ) ){
                CAL_LL_ActiveChannel( CAL_LL_2v5 ); } break;

        case CAL_process_in_scale:
            cal.adc[0].scale -= cal.adc[0].shift;
            cal.adc[0].scale  = vref / cal.adc[0].scale;
            ADC_CalibrateScalar( 0, cal.adc[0].scale );
            cal.adc[1].scale  = cal.adc[0].scale; // copy ch0 to ch1
            ADC_CalibrateScalar( 1, cal.adc[1].scale );
            cal.adc[0].shift = 0.0; // reset before scaled calibration
            cal.stage++;
            break;

        case CAL_in0_shift:
            if( CAL_Step( b, &(cal.adc[0].shift) ) ){
                CAL_LL_ActiveChannel( CAL_LL_Ground ); } break;

        case CAL_in1_shift:
            if( CAL_StepB( b, &(cal.adc[1].shift) ) ){
                CAL_LL_ActiveChannel( CAL_LL_Ground ); } break;

        case CAL_process_in_shift:
            cal.adc[0].shift = -cal.adc[0].shift;

// FIXME!!!
                cal.adc[0].shift += 0.0022; // FIXME hardcoded kludge


            ADC_CalibrateShift( 0, cal.adc[0].shift );
            cal.adc[1].shift = -cal.adc[1].shift;

// FIXME!!!
                cal.adc[1].shift += 0.0022; // FIXME hardcoded kludge


            ADC_CalibrateShift( 1, cal.adc[1].shift );
            cal.stage++;
            break;

    // adc is now calibrated
    // proceed to sample each dac

        case CAL_dac0_shift:
            if( CAL_Step( b, &(cal.dac[0].shift) ) ){
                CAL_LL_ActiveChannel( CAL_LL_dac0 );
                S_toward( s0, 0.0, 0.0, SHAPE_Linear, NULL ); } break;
        case CAL_dac0_scale:
            if( CAL_Step( b, &(cal.dac[0].scale) ) ){
                S_toward( s0, output_ref, 0.0, SHAPE_Linear, NULL ); } break;

        case CAL_dac1_shift:
            if( CAL_Step( b, &(cal.dac[1].shift) ) ){
                S_toward( s0, 0.0, 0.0, SHAPE_Linear, NULL );
                CAL_LL_ActiveChannel( CAL_LL_dac1 );
                S_toward( s1, 0.0, 0.0, SHAPE_Linear, NULL ); } break;
        case CAL_dac1_scale:
            if( CAL_Step( b, &(cal.dac[1].scale) ) ){
                S_toward( s1, output_ref, 0.0, SHAPE_Linear, NULL ); } break;

        case CAL_dac2_shift:
            if( CAL_Step( b, &(cal.dac[2].shift) ) ){
                S_toward( s1, 0.0, 0.0, SHAPE_Linear, NULL );
                CAL_LL_ActiveChannel( CAL_LL_dac2 );
                S_toward( s2, 0.0, 0.0, SHAPE_Linear, NULL ); } break;
        case CAL_dac2_scale:
            if( CAL_Step( b, &(cal.dac[2].scale) ) ){
                S_toward( s2, output_ref, 0.0, SHAPE_Linear, NULL ); } break;

        case CAL_dac3_shift:
            if( CAL_Step( b, &(cal.dac[3].shift) ) ){
                S_toward( s2, 0.0, 0.0, SHAPE_Linear, NULL );
                CAL_LL_ActiveChannel( CAL_LL_dac3 );
                S_toward( s3, 0.0, 0.0, SHAPE_Linear, NULL ); } break;
        case CAL_dac3_scale:
            if( CAL_Step( b, &(cal.dac[3].scale) ) ){
                S_toward( s3, output_ref, 0.0, SHAPE_Linear, NULL ); } break;

        case CAL_dac_scales:
            S_toward( s3, 0.0, 0.0, SHAPE_Linear, NULL );
            CAL_LL_ActiveChannel( CAL_LL_Ground ); // mux off / gnd input

            for( uint8_t j=0; j<4; j++ ){
                cal.dac[j].scale = output_ref / (cal.dac[j].scale - cal.dac[j].shift);

// FIXME!!!
                cal.dac[j].scale *= 0.9952; // FIXME hardcoded kludge


                DAC_CalibrateScalar( j, cal.dac[j].scale );
                cal.dac[j].shift = 0.0; // reset shift for scaled calibration
            }
            cal.stage++;
            break;

        case CAL_dac0_z:
            if( CAL_Step( b, &(cal.dac[0].shift) ) ){
                CAL_LL_ActiveChannel( CAL_LL_dac0 );
                S_toward( s0, 0.0, 0.0, SHAPE_Linear, NULL ); } break;
            break;

        case CAL_dac1_z:
            if( CAL_Step( b, &(cal.dac[1].shift) ) ){
                CAL_LL_ActiveChannel( CAL_LL_dac1 );
                S_toward( s1, 0.0, 0.0, SHAPE_Linear, NULL ); } break;
            break;

        case CAL_dac2_z:
            if( CAL_Step( b, &(cal.dac[2].shift) ) ){
                CAL_LL_ActiveChannel( CAL_LL_dac2 );
                S_toward( s2, 0.0, 0.0, SHAPE_Linear, NULL ); } break;
            break;

        case CAL_dac3_z:
            if( CAL_Step( b, &(cal.dac[3].shift) ) ){
                CAL_LL_ActiveChannel( CAL_LL_dac3 );
                S_toward( s3, 0.0, 0.0, SHAPE_Linear, NULL ); } break;
            break;

        case CAL_complete:
            for( uint8_t j=0; j<4; j++ ){
                cal.dac[j].shift = -cal.dac[j].shift;

// FIXME!!!
                cal.dac[j].shift += 0.0022; // FIXME hardcoded kludge (input comp)
                cal.dac[j].shift += 0.0027; // FIXME hardcoded kludge


                DAC_CalibrateOffset( j, cal.dac[j].shift );
            }
            int error = 0;
            if( !(error = CAL_ValidateData()) ){
                CAL_WriteFlash();
            } else {
                char msg[] = "Calibration failed. Unplug the below jacks & retry!\n";
                Caw_send_raw( (uint8_t*)msg, strlen(msg) );

                char pc[32];
                for( int i=0; i<2; i++ ){
                    if( error & (1<<i) ){
                        int len = snprintf( pc, 31, " in %i\n\r", i+1 );
                        Caw_send_raw( (uint8_t*)pc, len );
                    }
                }
                for( int i=2; i<6; i++ ){
                    if( error & (1<<i) ){
                        int len = snprintf( pc, 31, " out %i\n\r", i-1 );
                        Caw_send_raw( (uint8_t*)pc, len );
                    }
                }
                CAL_Recalibrate( 1 );
            }
            cal.stage = CAL_none;
            CAL_LL_ActiveChannel( CAL_LL_Ground ); // mux off / gnd input
            break;

        default: break;
    }

    // run slopes to set output levels
    //for( int j=0; j<SLOPE_CHANNELS; j++ ){
    //    S_step_v( j
    //            , b->out[j]
    //            , b->size
    //            );
    //}
    Slope_v( s0, b->out[0], b->size );
    Slope_v( s1, b->out[1], b->size );
    Slope_v( s2, b->out[2], b->size );
    Slope_v( s3, b->out[3], b->size );
    return b;
}

CAL_stage_t CAL_is_calibrating( void )
{
    return cal.stage;
}

void CAL_Recalibrate( uint8_t use_defaults )
{
    // use default values
    for( int j=0; j<2; j++ ){
        cal.adc[j].shift = 0.0;
        ADC_CalibrateShift( j, 0.0 );
        cal.adc[j].scale = 0.0; // use zero! otherwise will affect average.
        ADC_CalibrateScalar( j, 1.0 );
    }
    for( int j=0; j<4; j++ ){
        cal.dac[j].shift = 0.0;
        DAC_CalibrateOffset( j, 0.0 );
        cal.dac[j].scale = 0.0; // use zero! otherwise will affect average.
        DAC_CalibrateScalar( j, 1.0 );
    }
    if( !use_defaults ){ // causes recalibration to run
        char msg[] = "Recalibrating IO...\n";
        Caw_send_raw( (uint8_t*)msg, strlen(msg) );
        printf(msg);
        cal.stage = CAL_in_zero;
        cal.avg_count = AVERAGE_COUNT;
    } else {
        char msg[] = "Using default calibration\n";
        Caw_send_raw( (uint8_t*)msg, strlen(msg) );
        printf(msg);
        Flash_clear_calibration();
    }
}

void CAL_PrintCalibration( void )
{
    {
        char pc[64];
        uint8_t len = snprintf(pc,63,"IO calibration data:\n\r");
        Caw_send_raw( (uint8_t*)pc, len );
    }
    if( Flash_is_calibrated() ){
        char pc[64];
        uint8_t len = 0;
        for( int j=0; j<2; j++ ){
            len = snprintf(pc, 63, " adc%i %f, %f\n\r"
                          , j+1, (double)cal.adc[j].shift, (double)cal.adc[j].scale );
            if(len > 63){ len = 63; }
            Caw_send_raw( (uint8_t*)pc, len );
        }
        for( int j=0; j<4; j++ ){
            len = snprintf(pc, 63, " dac%i %f, %f\n\r"
                          , j+1, (double)cal.dac[j].shift, (double)cal.dac[j].scale );
            if(len > 63){ len = 63; }
            Caw_send_raw( (uint8_t*)pc, len );
        }
    } else {
        char pc[] = " Using defaults.\n\r";
        Caw_send_raw( (uint8_t*)pc, strlen(pc) );
    }
}

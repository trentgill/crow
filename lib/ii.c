#include "ii.h"

#include <string.h> // memcpy
#include <stdbool.h>

#include "../ll/i2c.h"
#include "../build/ii_c_layer.h" // GENERATED BY BUILD PROCESS
#include "lualink.h"
#include "wrQueue.h"
#include "wrMath.h" // lim_f
#include "caw.h" // Caw_send_luachunk

// for follow getters
#include "io.h"
#include "slopes.h"

#define II_MAX_BROADCAST_LEN 9 // cmd byte + 4*s16 args
#define II_MAX_RECEIVE_LEN 10
#define II_QUEUE_LENGTH 16
#define II_GET 128  // cmd >= are getter requests
#define II_TT_VOLT  ((float)1638.3)
#define II_TT_iVOLT ((float)1.0/II_TT_VOLT)


///////////////////////////
// type declarations

typedef struct{
    uint8_t address;
    uint8_t length;
    uint8_t query_length; // 0 for broadcast, >0 is return type byte size
    uint8_t data[II_MAX_BROADCAST_LEN];
    uint8_t arg; // just carrying this through for the follower response
} ii_q_t;


////////////////////////////////////////
// private declarations

static void lead_callback( uint8_t address, uint8_t command, uint8_t* rx_data );
static int follow_request( uint8_t* pdata );
static int follow_action( uint8_t* pdata );
static void error_action( int error_code );

static uint8_t type_size( ii_Type_t t );
static float decode( uint8_t* data, ii_Type_t type );
static uint8_t encode( uint8_t* dest, ii_Type_t type, float data );
static float* decode_packet( float* decoded, uint8_t* data, const ii_Cmd_t* c, int is_following);
static uint8_t encode_packet( uint8_t* dest, const ii_Cmd_t* c, uint8_t cmd, float* data );


////////////////////////////////
// local variables

queue_t* l_qix;
queue_t* f_qix;
ii_q_t   l_iq[II_QUEUE_LENGTH];
uint8_t  f_iq[II_QUEUE_LENGTH][II_MAX_RECEIVE_LEN];
uint8_t  rx_arg = 0; // FIXME is there a better solution?


////////////////////////
// setup

uint8_t ii_init( uint8_t address )
{
    if( address != II_CROW
     && address != II_CROW2
     && address != II_CROW3
     && address != II_CROW4
      ){ address = II_CROW; } // ensure a valid address
    if( I2C_Init( (uint8_t)address
                , &lead_callback
                , &follow_action
                , &follow_request
                , &error_action
                ) ){ printf("I2C Failed to Init\n"); }

    l_qix = queue_init( II_QUEUE_LENGTH );
    f_qix = queue_init( II_QUEUE_LENGTH );
    for( int i=0; i<II_QUEUE_LENGTH; i++ ){
        l_iq[i].length = 0; // mark as no-packet
    }

    return address;
}
void ii_deinit( void ){
    I2C_DeInit();
}
const char* ii_list_modules( void ){
    return ii_module_list;
}
const char* ii_list_cmds( uint8_t address ){
    return ii_list_commands(address);
}
void ii_set_pullups( uint8_t state ){
    I2C_SetPullups(state);
}
uint8_t ii_get_address( void ){
    switch(I2C_GetAddress()){
        case II_CROW2: return 2;
        case II_CROW3: return 3;
        case II_CROW4: return 4;
        default:       return 1;
    }
}
void ii_set_address( uint8_t index ){
    uint8_t i2c = II_CROW;
    switch(index){
        case 2: i2c = II_CROW2; break;
        case 3: i2c = II_CROW3; break;
        case 4: i2c = II_CROW4; break;
        default: break;
    }
    I2C_SetAddress( i2c );
}


////////////////////////////////
// leader commands

uint8_t ii_leader_enqueue( uint8_t address
                         , uint8_t cmd
                         , float*  data
                         )
{
    int ix = queue_enqueue( l_qix );
    if( ix < 0 ){ printf("queue full\n"); return 1; }

    ii_q_t* q = &l_iq[ix];

    q->address = address;
    const ii_Cmd_t* c = ii_find_command(address, cmd);
    q->query_length = type_size( c->return_type );
    q->arg = data[0]; // save a copy of the first argument
    q->length = encode_packet( q->data
                             , c
                             , cmd
                             , data
                             );
    ii_pickle( &q->address, q->data, &q->length );
    return 0;
}

void ii_leader_process( void )
{
    if( !I2C_is_ready() ){ return; } // I2C lib is busy
    int ix = queue_dequeue(l_qix);
    if( ix < 0 ){ return; } // queue is empty!
    ii_q_t* q = &l_iq[ix];

    int error = 0;
    if( q->query_length ){
        rx_arg = q->arg;
        if( (error = I2C_LeadRx( q->address
                      , q->data
                      , q->length
                      , q->query_length
                      )) ){
            if( error & 0x6 ){ error_action( 1 ); }
            printf("leadRx failed %i\n",error);
        }
    } else {
        if( (error = I2C_LeadTx( q->address
                      , q->data
                      , q->length
                      )) ){
            if( error & 2 ){ error_action( 1 ); }
            printf("leadTx failed %i\n",error);
        }
    }
}


///////////////////////////////////
// follower: polling mode

uint8_t* ii_processFollowRx( void )
{
    int ix = queue_dequeue( f_qix );
    if( ix < 0 ){ return NULL; } // queue is empty!
    return f_iq[ix];
}

// TODO localize queue
volatile int lead_has_data = 0;
uint8_t lead_data[I2C_MAX_CMD_BYTES];
uint8_t* ii_processLeadRx( void )
{
    uint8_t* pRetval = NULL;
    if( lead_has_data ){
        pRetval = lead_data;
        lead_has_data = 0;
    }
    return pRetval; // NULL for finished
}


////////////////////////////////////////////
// LL driver callbacks

static void lead_callback( uint8_t address, uint8_t command, uint8_t* rx_data )
{
    ii_unpickle( &address, &command, rx_data );
    L_queue_ii_leadRx( address
                     , command
                     , decode( rx_data
                             , ii_find_command(address, command)->return_type
                             )
                     , rx_arg
                     );
}

static int follow_request( uint8_t* pdata )
{
    const ii_Cmd_t* c = ii_find_command(ii_get_address(), *pdata);

    float args[c->args];
    decode_packet( args, &pdata[1], c, 1 );
    float response;
    switch( c->cmd ){
        case II_GET+3: // 'input'
            response = IO_GetADC( (int)args[0] - 1 ); // i2c is 1-based
            break;
        case II_GET+4: // 'output'
            response = S_get_state( (int)args[0] - 1 ); // i2c is 1-based
            break;
        default: // 'query'
            // DANGER!! run the Lua callback directly!
            response = L_handle_ii_followRxTx( c->cmd
                            , c->args
                            , args
                            );
            break;
    }
    return encode( pdata, c->return_type, response );
}

static int follow_action( uint8_t* pdata )
{
    int ix = queue_enqueue( f_qix );
    if( ix < 0 ){
        printf("ii_follow queue overflow\n");
        return 1;
    } else {
        memcpy( f_iq[ix], pdata, II_MAX_RECEIVE_LEN );
        L_queue_ii_followRx();
    }
    return 0;
}

// call this from the event system
void ii_process_dequeue_decode( void )
{
    uint8_t* pdata = ii_processFollowRx();
    const ii_Cmd_t* c = ii_find_command(ii_get_address(), *pdata++);
    float args[c->args];
    L_handle_ii_followRx_cont( c->cmd
                             , c->args
                             , decode_packet( args, pdata, c, 1 )
                             );
}

static void error_action( int error_code )
{
    switch( error_code ){
        case 0: // Ack Failed
            printf("I2C_ERROR_AF\n"); // means can't find device
            // TODO make this a global variable which can be checked by user
            // becomes a basic way to ask "was the message received"
            break;
        case 1: // Bus is busy. Could this also be ARLO?
            if( I2C_GetPullups() ){
                Caw_send_luachunk("ii: lines are low.");
                Caw_send_luachunk("  check ii devices are connected correctly.");
                Caw_send_luachunk("  check no ii devices are frozen.");
            } else {
                Caw_send_luachunk("ii: lines are low. try ii.pullup(true)");
            }
            break;
        default: // Unknown (ARLO?)
            Caw_send_luachunk("ii: unknown error.");
            printf("I2C_ERROR %i\n", error_code);
            break;
    }
}


/////////////////////////////////////
// ii Type Encode/Decode

static uint8_t type_size( ii_Type_t t )
{
    switch(t){ case ii_void:  return 0;
               case ii_u8:    return 1;
               case ii_s8:    return 1;
               case ii_u16:   return 2;
               case ii_s16:   return 2;
               case ii_s16V:  return 2;
               case ii_float: return 4;
    }
    return 0;
}

static float decode( uint8_t* data, ii_Type_t type )
{
    float val = 0; // return value default to zero
    uint16_t u16 = 0;
    switch( type ){
        case ii_u8:
            val = (float)(*data++);
            break;
        case ii_s8:
            val = (float)(*(int8_t*)data++);
            break;
        case ii_u16:
            u16  = ((uint16_t)*data++)<<8;
            u16 |= *data++;
            val = (float)u16;
            break;
        case ii_s16:
            u16  = ((uint16_t)*data++)<<8;
            u16 |= *data++;
            val = (float)*(int16_t*)&u16;
            break;
        case ii_s16V:
            u16  = ((uint16_t)*data++)<<8;
            u16 |= *data++;
            val = ((float)*(int16_t*)&u16)*II_TT_iVOLT; // Scale Teletype down to float
            break;
        case ii_float:
            val = *(float*)data;
            break;
        default: printf("ii_decode unmatched\n"); break;
    }
    return val;
}

static uint8_t encode( uint8_t* dest, ii_Type_t type, float data )
{
    uint8_t len = 0;
    uint8_t* d = dest;

    uint16_t u16; int16_t s16;
    switch( type ){
        case ii_u8: d[len++] = (uint8_t)lim_f(data,0.0,255.0);
            break;
        case ii_s8: d[len++] = (int8_t)lim_f(data,-128.0,127.0);
            break;
        case ii_u16:
            // clamp range to 16bit
            u16 = (uint16_t)lim_f(data, 0.0, 65535.0);
            d[len++] = (uint8_t)(u16>>8);          // High byte first
            d[len++] = (uint8_t)(u16 & 0x00FF);    // Low byte
            break;
        case ii_s16V:
            data *= II_TT_VOLT; // Scale float up to Teletype
            // FLOWS THROUGH
        case ii_s16:
            s16 = (int16_t)lim_f(data, -32768.0, 32767.0);
            u16 = *(uint16_t*)&s16;
            d[len++] = (uint8_t)(u16>>8);          // High byte first
            d[len++] = (uint8_t)(u16 & 0x00FF);    // Low byte
            break;
        case ii_float:
            memcpy( &(d[len]), &data, 4 );
            len += 4;
            break;
        default: printf("no retval found\n"); return 0;
            // FIXME: should this really print directly? or pass to caller?
    }
    return len;
}

static float* decode_packet( float* decoded
                           , uint8_t* data
                           , const ii_Cmd_t* c
                           , int is_following
                           )
{
    float* d = decoded;
    if( is_following ){
        int len = 0;
        for( int i=0; i<(c->args); i++ ){
            *d++ = decode( &data[len], c->argtype[i] );
            len += type_size( c->argtype[i] );
        }
    } else {
        *d = decode( data, c->return_type );
    }
    return decoded;
}

static uint8_t encode_packet( uint8_t* dest
                            , const ii_Cmd_t* c
                            , uint8_t cmd
                            , float* data
                            )
{
    uint8_t len = 0;
    dest[len++] = cmd; // first byte is command
    for( int i=0; i<(c->args); i++ ){
        len += encode( &dest[len], c->argtype[i], *data++ );
    }
    return len;
}

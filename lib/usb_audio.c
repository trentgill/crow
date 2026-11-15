#include "usb_audio.h"

#include <string.h>

#include "stm32f7xx.h"         // BLOCK_IRQS, __DMB()
#include "../usbd/usbd_map.h"  // AUDIO_*, SRAM_NOINIT
#include "../ll/dac8565.h"     // DAC_GetFrame()

// The OUT ring holds what the host has sent and the outputs haven't played.
// It runs at OUT_TARGET frames (1.7ms): one audio block takes 32 frames at a
// time and packets bring ~48 a millisecond, so the fill swings by ~80 frames
// around its middle, and a whole lost packet still leaves a block to play.
#define OUT_RING     256 // frames. power of two: the indices below run freely
#ifndef OUT_TARGET
#define OUT_TARGET   80  // frames buffered before playing starts, and kept.
                         // down from 128: 1ms less latency, a 5 min soak clean
#endif
#define OUT_HIGH     (OUT_RING - 32) // above this, drop back to OUT_TARGET
#define OUT_GAPS     4   // lost packets in a row patched over, see USB_Audio_IsoMissed

// The IN ring holds input blocks the host hasn't collected. A packet takes up
// to 49 frames, blocks land 32 at a time, and when a packet is built depends
// on when the host polls and how late the OTG interrupt runs (a block can
// land either side of it): so the ring sits at IN_TARGET (2.0ms), well above
// a packet plus a block. 64 underran a dozen times in 24s; 96 ran clean.
#define IN_RING      256 // frames. power of two
#ifndef IN_TARGET
#define IN_TARGET    96  // frames waiting when a packet is built
#endif
#define IN_HIGH      (IN_TARGET + 96) // the host stopped reading: drop the backlog

#define FRAME_NOM    (AUDIO_RATE_NOMINAL / 1000) // 48 samples per USB frame
#define FRAME_MASK   0x7FF // full speed frame numbers are 11 bits

// Rate measurement and feedback, see USB_Audio_Frame & USB_Audio_Feedback
#define FB_WINDOW      256  // frames per measurement window
#define FB_SMOOTH      16   // windows averaged: ~4s
#define FB_CORR_MAX    4096 // ring correction limit: 1/16 sample per frame (16.16)

#define OUT_MASK     (OUT_RING - 1)
#define IN_MASK      (IN_RING - 1)
#define S16_TO_VOLTS (10.0f / 32768.0f)
#define VOLTS_TO_S16 (32768.0f / 10.0f)

#ifdef TELEMETRY
#define COUNT(c) (st.stats.c++)
#else
#define COUNT(c) do{}while(0)
#endif

// rings and state are in SRAM: DTCM is full. neither is zeroed at startup, so
// USB_Audio_Init sets the state, and nothing reads a ring before it's written
typedef struct{
    int16_t out[OUT_RING][AUDIO_OUT_CHANNELS]; // 2KB
    int16_t in[IN_RING][AUDIO_IN_CHANNELS];    // 1KB
} Rings_t;
static Rings_t rings SRAM_NOINIT;

typedef struct{
    // OUT ring. head: OTG interrupt. tail & out_run: audio block
    volatile uint32_t out_head;
    volatile uint32_t out_tail;
    volatile uint8_t  out_on;  // host has the OUT stream open
    volatile uint8_t  out_run; // ring has filled: playing (else 0V)
    uint8_t           out_gaps; // lost packets patched since the last arrival

    // IN ring. head: audio block. tail: OTG interrupt
    volatile uint32_t in_head;
    volatile uint32_t in_tail;
    volatile uint8_t  in_on;   // host has the IN stream open

    // DAC position, from the audio block. see dac_position()
    volatile uint32_t pos_base; // frames played before the half playing now
    volatile uint32_t pos_seq;  // bumped on every update, for readers
    volatile uint8_t  pos_half; // which half of the DMA ring is playing
    uint8_t           pos_valid;

    // OTG interrupt only
    uint32_t rate;      // measured DAC rate. 16.16 samples per USB frame
    uint8_t  windows;   // windows measured, up to FB_SMOOTH
    uint8_t  win_open;
    uint16_t win_frame; // USB frame the window started on
    uint32_t win_pos;   // DAC position then
    int32_t  out_avg;   // OUT ring fill, smoothed. frames, 24.8
    int32_t  in_avg;    // IN ring fill at packet time, smoothed. 24.8
    uint32_t in_acc;    // IN packet sizing: fraction of a frame owed. 16.16

    USB_Audio_Stats_t stats;
} State_t;
static State_t st SRAM_NOINIT;

// in DTCM, so they're zero until USB_Audio_Init
static volatile uint8_t ready    = 0;
static volatile uint8_t enabled  = 0;
static volatile uint8_t out_mask = 0; // bit per output the host stream drives

static void track_position( void );
static uint32_t dac_position( void );


// LIFECYCLE

void USB_Audio_Init( void )
{
    // the audio block may already be running
    BLOCK_IRQS(
        memset( &st, 0, sizeof(st) );
        st.rate    = FRAME_NOM << 16;
        st.out_avg = OUT_TARGET << 8;
        st.in_avg  = IN_TARGET << 8;
        st.stats.feedback = FRAME_NOM << 14;
        ready = 1;
    );
}

void USB_Audio_Enable( uint8_t on )
{
    if( on ){ out_mask = (1 << AUDIO_OUT_CHANNELS) - 1; }
    enabled = on;
}

uint8_t USB_Audio_IsEnabled( void ){ return enabled; }

void USB_Audio_SetOutput( int ch, uint8_t usb )
{
    if( ch < 0 || ch >= AUDIO_OUT_CHANNELS ){ return; }
    if( usb ){ out_mask |=  (1 << ch); }
    else {     out_mask &= ~(1 << ch); }
}

uint8_t USB_Audio_GetOutput( int ch )
{
    if( ch < 0 || ch >= AUDIO_OUT_CHANNELS ){ return 0; }
    return (out_mask >> ch) & 1;
}


// THE AUDIO BLOCK

static inline int16_t volts_to_s16( float v )
{
    float s = v * VOLTS_TO_S16;
    if( s >=  32767.0f ){ return  32767; }
    if( s <= -32768.0f ){ return -32768; }
    return (int16_t)(s + ((s < 0.0f) ? -0.5f : 0.5f));
}

static uint8_t loopback = 0;
void USB_Audio_Loopback( uint8_t on ){ loopback = on; }

void USB_Audio_BlockIn( IO_block_t* b )
{
    if( !ready ){ return; }
    track_position(); // every block, so the feedback measurement never stops

    if( !enabled || !st.in_on ){ return; }
    uint32_t n    = b->size;
    uint32_t head = st.in_head;
    if( head - st.in_tail + n > IN_RING ){ // host hasn't been reading
        COUNT(in_overruns);
        return;
    }
    // loopback (a test mode): send what is about to go to outputs 1 & 2
    // instead of the inputs, to measure the path without the analog parts
    float* src0 = loopback ? b->out[0] : b->in[0];
    float* src1 = loopback ? b->out[1] : b->in[1];
    for( uint32_t i=0; i<n; i++ ){
        int16_t* f = rings.in[(head + i) & IN_MASK];
        f[0] = volts_to_s16( src0[i] );
        f[1] = volts_to_s16( src1[i] );
    }
    __DMB(); // the samples are in place before the index says so
    st.in_head = head + n;
}

void USB_Audio_BlockOut( IO_block_t* b )
{
    if( !ready || !enabled ){ return; }
    uint8_t  mask = out_mask;
    uint32_t n    = b->size;

    if( st.out_on ){
        uint32_t head = st.out_head;
        uint32_t tail = st.out_tail;
        uint32_t fill = head - tail;
        if( !st.out_run ){
            if( fill >= OUT_TARGET ){ st.out_run = 1; } // filled: start playing
        } else if( fill < n ){
            // ran dry: wait for the ring to fill again, so a single late packet
            // costs one gap rather than a gap every block
            st.out_run = 0;
            COUNT(out_underruns);
        }
        if( st.out_run ){
            if( fill > OUT_HIGH ){ // latency has crept up: drop back
                tail = head - OUT_TARGET;
                COUNT(out_overruns);
            }
            __DMB(); // read samples only after the index that published them
            for( int ch=0; ch<AUDIO_OUT_CHANNELS; ch++ ){
                if( !(mask & (1 << ch)) ){ continue; }
                float* o = b->out[ch];
                for( uint32_t i=0; i<n; i++ ){
                    o[i] = (float)rings.out[(tail + i) & OUT_MASK][ch] * S16_TO_VOLTS;
                }
            }
            st.out_tail = tail + n; // the ring moves whichever jacks listen
            return;
        }
    }
    // no stream, or still filling: the host's jacks sit at 0V
    for( int ch=0; ch<AUDIO_OUT_CHANNELS; ch++ ){
        if( mask & (1 << ch) ){
            memset( b->out[ch], 0, sizeof(float) * n );
        }
    }
}


// DAC POSITION
//
// How many sample frames the DAC has played, as a free-running count. The DMA
// ring holds two blocks: the audio block fills one half while the DMA plays
// the other. Each block notes which half is playing and moves pos_base on by
// the frames played since the last block. A reader adds the DMA's position
// inside the half. Reading happens in the OTG interrupt, which the audio
// block preempts but never the reverse: so a block either ran entirely
// between two reads (pos_seq changed: read again), or its DMA crossing is
// still pending (the DMA is in the other half: add that half).

static void track_position( void )
{
    uint32_t half = DAC_GetFrame() / ADDA_BLOCK_SIZE;
    if( !st.pos_valid ){
        st.pos_valid = 1;
    } else {
        // the half changed: one block played. it didn't: two did, and a
        // block interrupt was lost along the way
        st.pos_base += (half != st.pos_half) ? ADDA_BLOCK_SIZE
                                             : 2 * ADDA_BLOCK_SIZE;
    }
    st.pos_half = half;
    st.pos_seq++;
}

static uint32_t dac_position( void )
{
    uint32_t seq, base, half, frame;
    do{
        seq   = st.pos_seq;
        base  = st.pos_base;
        half  = st.pos_half;
        frame = DAC_GetFrame();
    } while( seq != st.pos_seq );
    uint32_t pos = base + (frame % ADDA_BLOCK_SIZE);
    if( frame / ADDA_BLOCK_SIZE != half ){ pos += ADDA_BLOCK_SIZE; }
    return pos;
}


// THE USB CLASS

void USB_Audio_OutStart( void )
{
    if( !ready ){ USB_Audio_Init(); } // rate & stats need setting up
    BLOCK_IRQS(
        st.out_head = 0;
        st.out_tail = 0;
        st.out_run  = 0;
        st.out_avg  = OUT_TARGET << 8;
        st.out_on   = 1;
    );
}

void USB_Audio_OutStop( void )
{
    BLOCK_IRQS(
        st.out_on  = 0;
        st.out_run = 0;
    );
}

void USB_Audio_OutPacket( const uint8_t* buf, uint32_t len )
{
    if( !st.out_on ){ return; }
    COUNT(out_packets);
    uint32_t frames = len / AUDIO_OUT_FRAME;
    uint32_t head   = st.out_head;
    if( head - st.out_tail + frames > OUT_RING ){
        COUNT(out_overruns); // host far ahead of the feedback: drop it
        return;
    }
    st.out_gaps = 0;
    uint32_t at    = head & OUT_MASK;
    uint32_t first = OUT_RING - at; // frames before the ring wraps
    if( first > frames ){ first = frames; }
    memcpy( rings.out[at], buf, first * AUDIO_OUT_FRAME );
    memcpy( rings.out[0], buf + first * AUDIO_OUT_FRAME
          , (frames - first) * AUDIO_OUT_FRAME );
    __DMB();
    st.out_head = head + frames;
}

void USB_Audio_InStart( void )
{
    if( !ready ){ USB_Audio_Init(); } // rate & stats need setting up
    // start centred, on silence, so the first packets go out whole
    BLOCK_IRQS(
        memset( rings.in, 0, IN_TARGET * AUDIO_IN_FRAME );
        st.in_tail = 0;
        st.in_head = IN_TARGET;
        st.in_avg  = IN_TARGET << 8;
        st.in_acc  = 0;
        st.in_on   = 1;
    );
}

void USB_Audio_InStop( void )
{
    st.in_on = 0;
}

// IN packets carry the frames the DAC clock produced in one USB frame: the
// measured rate, accumulated (48 most frames, 47 now and then at 47.992kHz),
// so the host sees crow's real rate. The rate is nudged by the ring's distance
// from IN_TARGET, the same way the feedback is (USB_Audio_Feedback), so
// whatever error the measurement has can't walk the ring away.
uint32_t USB_Audio_InPacket( uint8_t* buf )
{
    uint32_t head = st.in_head;
    uint32_t tail = st.in_tail;
    uint32_t fill = head - tail;
    if( fill > IN_HIGH ){ // host stopped collecting for a while
        tail = head - IN_TARGET;
        fill = IN_TARGET;
        st.in_avg = IN_TARGET << 8;
        COUNT(in_overruns);
    }
    st.in_avg += ((int32_t)(fill << 8) - st.in_avg) / 16;
    int32_t corr = (st.in_avg - (IN_TARGET << 8)) / 4; // fuller: send more
    if( corr >  FB_CORR_MAX ){ corr =  FB_CORR_MAX; }
    if( corr < -FB_CORR_MAX ){ corr = -FB_CORR_MAX; }
    st.in_acc += (uint32_t)((int32_t)st.rate + corr);
    uint32_t n = st.in_acc >> 16;
    st.in_acc &= 0xFFFF;
    if( n < FRAME_NOM - 1 ){ n = FRAME_NOM - 1; }
    if( n > FRAME_NOM + 1 ){ n = FRAME_NOM + 1; } // the endpoint's max packet

    uint32_t take = (fill < n) ? fill : n;
    if( take < n ){ COUNT(in_underruns); }
    __DMB(); // the samples behind head are complete
    uint32_t at    = tail & IN_MASK;
    uint32_t first = IN_RING - at;
    if( first > take ){ first = take; }
    memcpy( buf, rings.in[at], first * AUDIO_IN_FRAME );
    memcpy( buf + first * AUDIO_IN_FRAME, rings.in[0]
          , (take - first) * AUDIO_IN_FRAME );
    memset( buf + take * AUDIO_IN_FRAME, 0, (n - take) * AUDIO_IN_FRAME );
    st.in_tail = tail + take;
    COUNT(in_packets);
    return n * AUDIO_IN_FRAME;
}

// Measures the DAC clock against the host's 1ms frames. Every FB_WINDOW
// frames: rate = samples played / frames elapsed, as 16.16 samples per frame.
// Frames are counted by the SOF frame number, so a missed SOF interrupt
// doesn't skew it. The position is read when the SOF interrupt runs, which
// the audio block can hold off by a few hundred us: up to ~15 samples of
// error at each edge of a window. But windows share their edges, so the
// error taken in at one edge is given back at the next: averaging N windows
// is the same as one window N times as long, and the error never accumulates.
// The first FB_SMOOTH windows are averaged exactly like that (the k-th moves
// the estimate 1/k of the way), so it's usable after the first 256ms and
// good to a few ppm after ~4s. After that each window moves it 1/FB_SMOOTH,
// which follows the crystal's drift and keeps averaging the jitter.
void USB_Audio_Frame( uint16_t frame )
{
    if( !ready ){ return; }
    frame &= FRAME_MASK;
    uint32_t pos = dac_position();

    if( !st.win_open ){
        st.win_open  = 1;
        st.win_frame = frame;
        st.win_pos   = pos;
    } else {
        uint32_t frames = (uint32_t)(frame - st.win_frame) & FRAME_MASK;
        if( frames >= FB_WINDOW ){
            uint32_t played = pos - st.win_pos;
            // a stalled DAC or a lost stretch of frames shows up as nonsense
            if( played >= frames * (FRAME_NOM - 1)
             && played <= frames * (FRAME_NOM + 1) ){
                uint32_t meas = (uint32_t)(((uint64_t)played << 16) / frames);
                if( st.windows < FB_SMOOTH ){ st.windows++; }
                st.rate += ((int32_t)(meas - st.rate)) / (int32_t)st.windows;
            } else {
                COUNT(fb_rejected);
            }
            st.win_frame = frame;
            st.win_pos   = pos;
        }
    }

    // OUT ring fill, smoothed over ~32 frames for the feedback correction
    if( st.out_run ){
        uint32_t fill = st.out_head - st.out_tail;
        st.out_avg += ((int32_t)(fill << 8) - st.out_avg) / 32;
    } else {
        st.out_avg = OUT_TARGET << 8;
    }
}

// What the host should send per frame, in the full speed feedback format:
// 10.14 fixed point samples per frame, 3 bytes. 48.0 is 0x0C0000, and crow's
// 47.992kHz is ~0x0BFF7D.
//
//   feedback = rate - (fill - OUT_TARGET) / 1024
//
// rate is the measured DAC clock: matching it alone keeps the ring's fill
// where it is. The second term pulls the fill back to the centre: a ring 64
// frames full asks for 1/16 sample less per frame, which the host's packets
// take back out over a second. It's limited to that, so a burst of lost
// packets can't swing the host's rate far. The loop's time constant (~1s) is
// far slower than the fill smoothing (32ms) and the host's own reaction (a few
// queued URBs), so it settles without ringing.
//
// Linux works out the feedback format by comparing the first value against
// the nominal rate (it takes 10.14 at full speed if within -25%..+50% of 48),
// so the value stays within one sample of 48: also what the 49 frame packet
// size allows.
uint32_t USB_Audio_Feedback( void )
{
    int32_t fb = (int32_t)st.rate;
    if( st.out_run ){
        int32_t corr = -(st.out_avg - (OUT_TARGET << 8)) / 4; // 24.8 frames -> 16.16 / 1024
        if( corr >  FB_CORR_MAX ){ corr =  FB_CORR_MAX; }
        if( corr < -FB_CORR_MAX ){ corr = -FB_CORR_MAX; }
        fb += corr;
    }
    if( fb < ((FRAME_NOM - 1) << 16) ){ fb = (FRAME_NOM - 1) << 16; }
    if( fb > ((FRAME_NOM + 1) << 16) ){ fb = (FRAME_NOM + 1) << 16; }
    uint32_t fb1014 = ((uint32_t)fb + 2) >> 2; // 16.16 -> 10.14, rounded
    st.stats.feedback = fb1014;
    return fb1014;
}

// in: the host didn't collect an IN packet in its frame. The class throws it
// away and sends a fresh one: the host loses those samples, but the latency
// from the jacks stays put.
// out: a frame passed without the host's packet. Those samples are gone (the
// host thinks it sent them), so the ring gets a packet's worth of the last
// frame held, to keep its timing; else every lost packet would take ~1s of
// feedback correction to win back. Only a few in a row: a host that stops
// sending without closing the stream gets 0V, from the ring running dry.
void USB_Audio_IsoMissed( uint8_t in )
{
    if( in ){
        COUNT(iso_in_missed);
        return;
    }
    COUNT(iso_out_missed);
    if( !st.out_on || !st.out_run || st.out_gaps >= OUT_GAPS ){ return; }
    st.out_gaps++;
    uint32_t head   = st.out_head;
    uint32_t frames = (st.rate + 0x8000) >> 16;
    if( head - st.out_tail + frames > OUT_RING ){ return; }
    const int16_t* last = rings.out[(head - 1) & OUT_MASK];
    for( uint32_t i=0; i<frames; i++ ){
        memcpy( rings.out[(head + i) & OUT_MASK], last, AUDIO_OUT_FRAME );
    }
    __DMB();
    st.out_head = head + frames;
}


// STATS

void USB_Audio_Stats( USB_Audio_Stats_t* s )
{
    if( !ready ){ memset( s, 0, sizeof(*s) ); return; }
    BLOCK_IRQS(
        *s = st.stats;
        s->rate          = st.rate;
        s->out_fill      = (uint16_t)(st.out_head - st.out_tail);
        s->in_fill       = (uint16_t)(st.in_head - st.in_tail);
        s->out_streaming = st.out_on;
        s->out_playing   = st.out_run;
        s->in_streaming  = st.in_on;
    );
}

void USB_Audio_StatsReset( void )
{
    if( !ready ){ return; }
    BLOCK_IRQS(
        uint32_t fb = st.stats.feedback;
        memset( &st.stats, 0, sizeof(st.stats) );
        st.stats.feedback = fb;
    );
}

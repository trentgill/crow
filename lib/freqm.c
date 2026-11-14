#include "freqm.h"

#include <math.h>
#include "../ll/ads131.h" // adc_ring, adc_widx, ADC_SAMPLE_RATE, ADC_RawToVolts

// envelope follower decay, raw ADC units per sample (~10V/s)
#define ENV_DECAY    1.0f
// minimum peak-to-peak (raw units, ~0.1V) before crossings are trusted
#define MIN_PP       440.0f
// hysteresis as a fraction of peak-to-peak
#define HYST_FRAC    0.15f

typedef struct{
    // window
    volatile int active;
    volatile int done;
    uint32_t start;   // first sample index in window
    uint32_t end;     // first sample index after window
    // regression sums (t relative to first crossing, k from 0)
    int      n;
    double   t0;
    double   sk, st, skk, skt, stt;
    // period statistics (Welford)
    double   lastt;
    double   pmean, pm2;
    int      pn;
    // pulse width (rise -> fall)
    double   wsum;
    int      wn;
    // amplitude
    float    vmax, vmin;
    FreqM_result_t res;
} Acc_t;

typedef struct{
    float    prev;
    float    hi, lo;
    int      armed;    // went below mid-h: next mid crossing upward is a rise
    int      armed_hi; // went above mid+h after a rise: next downward is a fall
    int      has_rise;
    uint32_t rise_n;
    float    rise_frac;
} Det_t;

static Det_t    det[FREQM_CHANNELS];
static Acc_t    shot[FREQM_CHANNELS];
static Acc_t    track[FREQM_CHANNELS];
static uint32_t track_len[FREQM_CHANNELS];
static volatile float tracked[FREQM_CHANNELS];
static uint32_t rd; // next sample index to process
static int      started = 0;

static void acc_reset( Acc_t* a, uint32_t start, uint32_t len )
{
    a->start = start;
    a->end   = start + len;
    a->n = 0;
    a->t0 = 0.0;
    a->sk = a->st = a->skk = a->skt = a->stt = 0.0;
    a->lastt = 0.0;
    a->pmean = a->pm2 = 0.0;
    a->pn = 0;
    a->wsum = 0.0;
    a->wn = 0;
    a->vmax = -40000.0f;
    a->vmin =  40000.0f;
    a->done = 0;
}

static void acc_cross( Acc_t* a, double t )
{
    if( a->n == 0 ){
        a->t0 = t;
    } else {
        double p = t - a->lastt;
        a->pn++;
        double d = p - a->pmean;
        a->pmean += d / (double)a->pn;
        a->pm2   += d * (p - a->pmean);
    }
    a->lastt = t;
    double k  = (double)a->n;
    double tt = t - a->t0;
    a->sk  += k;
    a->st  += tt;
    a->skk += k*k;
    a->skt += k*tt;
    a->stt += tt*tt;
    a->n++;
}

static void acc_finish( Acc_t* a, int ch )
{
    FreqM_result_t* r = &a->res;
    r->count = a->n;
    r->vpp = (a->vmax > a->vmin)
           ? ADC_RawToVolts(ch, (int16_t)a->vmax) - ADC_RawToVolts(ch, (int16_t)a->vmin)
           : 0.0f;
    r->width = (a->wn > 0) ? (float)(a->wsum / (double)a->wn / (double)ADC_SAMPLE_RATE) : 0.0f;
    if( a->n < 2 ){
        r->freq = 0.0f; r->jitter = 0.0f; r->resid = 0.0f;
    } else {
        double N = (double)a->n;
        double den = N*a->skk - a->sk*a->sk;
        double slope = (N*a->skt - a->sk*a->st) / den; // samples per period
        r->freq = (float)((double)ADC_SAMPLE_RATE / slope);
        double sse = (a->stt - a->st*a->st/N)
                   - slope*(a->skt - a->sk*a->st/N);
        r->resid = (float)sqrt( (sse > 0.0 ? sse : 0.0) / N );
        if( a->pn > 1 && a->pmean > 0.0 ){
            double sd = sqrt( a->pm2 / (double)(a->pn - 1) );
            r->jitter = (float)(1731.234 * sd / a->pmean); // ~cents
        } else {
            r->jitter = 0.0f;
        }
    }
}

void FreqM_init( void )
{
    for( int j=0; j<FREQM_CHANNELS; j++ ){
        det[j].prev = 0.0f; det[j].hi = 0.0f; det[j].lo = 0.0f; det[j].armed = 0;
        det[j].armed_hi = 0; det[j].has_rise = 0;
        shot[j].active = 0; shot[j].done = 0;
        track[j].active = 0; track_len[j] = 0;
        tracked[j] = 0.0f;
    }
    started = 0;
}

static inline void acc_sample( Acc_t* a, uint32_t n, float x, int crossed, float frac, float width, int ch, int is_track )
{
    if( !a->active ){ return; }
    if( (int32_t)(n - a->start) < 0 ){ return; }
    if( (int32_t)(n - a->end) >= 0 ){
        acc_finish( a, ch );
        if( is_track ){
            tracked[ch] = a->res.freq;
            acc_reset( a, n, track_len[ch] );
        } else {
            a->active = 0;
            a->done = 1;
            return;
        }
    }
    if( x > a->vmax ){ a->vmax = x; }
    if( x < a->vmin ){ a->vmin = x; }
    // crossing lies between samples n-1 and n; time relative to window start
    if( width > 0.0f ){ a->wsum += (double)width; a->wn++; }
    if( crossed ){ acc_cross( a, (double)(int32_t)(n - 1 - a->start) + (double)frac ); }
}

void FreqM_process( void )
{
    uint32_t w = adc_widx;
    if( !started ){ rd = w; started = 1; return; }
    if( (uint32_t)(w - rd) > (ADC_RING_LEN - 64) ){
        rd = w - (ADC_RING_LEN - 64); // fell behind: skip (should not happen)
    }
    while( rd != w ){
        uint32_t n = rd++;
        for( int j=0; j<FREQM_CHANNELS; j++ ){
            Det_t* d = &det[j];
            float x = (float)adc_ring[j][n & ADC_RING_MASK];
            // envelope
            if( x > d->hi ){ d->hi = x; } else { d->hi -= ENV_DECAY; }
            if( x < d->lo ){ d->lo = x; } else { d->lo += ENV_DECAY; }
            if( d->lo > d->hi ){ float m = 0.5f*(d->hi+d->lo); d->hi = m; d->lo = m; }
            float pp  = d->hi - d->lo;
            float mid = 0.5f*(d->hi + d->lo);
            int crossed = 0;
            float frac = 0.0f;
            float width = 0.0f;
            if( pp > MIN_PP ){
                float h = HYST_FRAC * pp;
                if( x < mid - h ){
                    d->armed = 1;
                } else if( d->armed && x >= mid ){
                    d->armed = 0;
                    float dx = x - d->prev;
                    frac = (dx > 0.0f && d->prev < mid) ? (mid - d->prev)/dx : 1.0f;
                    d->has_rise = 1;
                    d->rise_n = n;
                    d->rise_frac = frac;
                    d->armed_hi = 0;
                    crossed = 1;
                }
                if( d->has_rise ){
                    if( x > mid + h ){
                        d->armed_hi = 1;
                    } else if( d->armed_hi && x < mid ){
                        float dx = d->prev - x;
                        float ff = (dx > 0.0f && d->prev >= mid) ? (d->prev - mid)/dx : 1.0f;
                        width = (float)(int32_t)(n - d->rise_n) + ff - d->rise_frac;
                        d->has_rise = 0;
                        d->armed_hi = 0;
                    }
                }
            } else {
                d->armed = 0;
                d->armed_hi = 0;
                d->has_rise = 0;
            }
            d->prev = x;
            acc_sample( &shot[j],  n, x, crossed, frac, width, j, 0 );
            acc_sample( &track[j], n, x, crossed, frac, width, j, 1 );
        }
    }
}

void FreqM_start( float seconds )
{
    uint32_t len = (uint32_t)(seconds * ADC_SAMPLE_RATE);
    if( len < 16 ){ len = 16; }
    uint32_t now = adc_widx;
    for( int j=0; j<FREQM_CHANNELS; j++ ){
        shot[j].active = 0;
        acc_reset( &shot[j], now, len );
        shot[j].active = 1;
    }
}

int FreqM_ready( void )
{
    for( int j=0; j<FREQM_CHANNELS; j++ ){
        if( !shot[j].done ){ return 0; }
    }
    return 1;
}

const FreqM_result_t* FreqM_result( int channel )
{
    return &shot[channel & 1].res;
}

void FreqM_track( int channel, float seconds )
{
    channel &= 1;
    track[channel].active = 0;
    if( seconds <= 0.0f ){ tracked[channel] = 0.0f; return; }
    uint32_t len = (uint32_t)(seconds * ADC_SAMPLE_RATE);
    if( len < 16 ){ len = 16; }
    track_len[channel] = len;
    acc_reset( &track[channel], adc_widx, len );
    track[channel].active = 1;
}

float FreqM_tracked( int channel )
{
    return tracked[channel & 1];
}

#pragma once

// crow's USB function layout. One place for every interface number, endpoint
// address and FIFO size, so the descriptor builder, the classes and the FIFO
// split agree.
//
// Default mode: serial (CDC ACM) + MIDI.
// Audio mode:   serial + MIDI + USB Audio 1 (4 out, 2 in, 16 bit, async).
//
// Serial keeps interfaces 0 and 1 and the endpoints crow has always used, so
// hosts that find crow by its serial port see it where it was.

// interfaces
#define ITF_CDC_CMD       0x00
#define ITF_CDC_DATA      0x01
#define ITF_MIDI_AC       0x02 // audio control, required ahead of MIDI streaming
#define ITF_MIDI_MS       0x03
#define ITF_AUDIO_AC      0x04 // audio mode only
#define ITF_AUDIO_OUT     0x05 // host -> crow outputs
#define ITF_AUDIO_IN      0x06 // crow inputs -> host

#define ITF_COUNT_DEFAULT 4
#define ITF_COUNT_AUDIO   7

// endpoints. the F722's OTG_FS has 6 IN and 6 OUT including EP0
#define EP_CDC_IN         0x81
#define EP_CDC_OUT        0x01
#define EP_CDC_CMD        0x82
#define EP_MIDI_IN        0x83
#define EP_MIDI_OUT       0x02
#define EP_AUDIO_OUT      0x03 // isochronous, asynchronous
#define EP_AUDIO_IN       0x84 // isochronous, asynchronous
#define EP_AUDIO_FB       0x85 // isochronous feedback for EP_AUDIO_OUT

// audio format
#define AUDIO_RATE_NOMINAL 48000 // advertised. the real rate (DAC) is reported by feedback
#define AUDIO_OUT_CHANNELS 4
#define AUDIO_IN_CHANNELS  2
#define AUDIO_BYTES        2     // 16 bit
#define AUDIO_OUT_FRAME    (AUDIO_OUT_CHANNELS * AUDIO_BYTES)
#define AUDIO_IN_FRAME     (AUDIO_IN_CHANNELS * AUDIO_BYTES)
// room for one extra sample per packet, so the host can catch up
#define AUDIO_OUT_PACKET   ((AUDIO_RATE_NOMINAL/1000 + 1) * AUDIO_OUT_FRAME) // 392
#define AUDIO_IN_PACKET    ((AUDIO_RATE_NOMINAL/1000 + 1) * AUDIO_IN_FRAME)  // 196
#define AUDIO_FB_PACKET    3     // 10.14 samples per frame, full speed

// interface strings. 0..5 belong to the core (langid, maker, product, serial,
// configuration, interface), so ours start at 6
#define STR_IDX_CDC       0x06
#define STR_IDX_MIDI      0x07
#define STR_IDX_AUDIO     0x08
#define STR_IDX_AUDIO_OUT 0x09
#define STR_IDX_AUDIO_IN  0x0A

// big CPU-only buffers (packet rings) live in SRAM rather than the nearly-full
// DTCM. not zeroed at startup: their owner initialises them before use
#define SRAM_NOINIT __attribute__((section(".sram_noinit")))

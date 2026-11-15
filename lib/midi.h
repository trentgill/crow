#pragma once

#include <stdint.h>

// crow's MIDI engine. Messages from the host arrive here from the main loop.
// For now every message is handed straight to Lua's midi_handler; the per-channel
// behaviours (v8 & gate, clock, CCs, ii forwarding) build on this.

void MIDI_init( void );
void MIDI_receive( const uint8_t* msg, uint8_t len );

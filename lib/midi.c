#include "midi.h"

#include "lualink.h" // L_handle_midi()

void MIDI_init( void )
{
}

void MIDI_receive( const uint8_t* msg, uint8_t len )
{
    // stub: pass everything to Lua untouched
    L_handle_midi( msg, len );
}

#pragma once

#include "midi/usbd_midi.h"

extern USBD_MIDI_ItfTypeDef USBD_MIDI_fops;

// Queue one MIDI message (1 to 3 bytes, status first, no sysex) for the host.
// Safe from thread mode and from interrupts. Returns 0 if it was dropped:
// no host has configured MIDI, or the queue is full.
uint8_t USB_midi_send( const uint8_t* msg, uint8_t len );

// main loop: hand received messages to MIDI_receive() & keep transmitting
void USB_midi_process( void );
void USB_midi_tick( uint32_t now_ms );

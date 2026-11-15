#pragma once

// the subset of atrium's AL94 composite configuration the vendored classes in
// usbd/cdc_acm and usbd/midi still read. crow's composite (usbd_composite.c)
// does its own descriptor building and dispatch.
#define _USBD_USE_HS            false
#define _USBD_CDC_ACM_COUNT     1
#define _USBD_USE_CDC_ACM       true
#define _USBD_USE_MIDI          true

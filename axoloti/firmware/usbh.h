#ifndef USBH_H
#define USBH_H

#include "usbh_core.h"

extern USBH_HandleTypeDef hUSBHost;

void MY_USBH_Init(void);

// Initialize the USB host MIDI output ring buffers (sets their notify
// callback). Must be called after USBH_Init() and before the first
// MidiSend() routed to a USB host port.
void usbhmidi_init_buffers(void);

#endif

# KassutronicsQuantizer
Firmware for the Kassutronics Quantizer module.

Documentation and installation instructions can be found here: https://github.com/kassu/kassutronics/tree/master/documentation/Quantizer

There are also [instructions for programming the firmware using an Arduino Uno](https://github.com/kassu/kassutronics/blob/master/documentation/Quantizer/Programming%20using%20an%20Arduino%20Uno.md)

## Changes in this fork

This fork is based on [kassu/KassutronicsQuantizer](https://github.com/kassu/KassutronicsQuantizer). It changes the following:

### CV control of gate length
When a CV input is assigned to gate length, the CV now shifts the gate length set in the menu instead of replacing it.

- 0V gives the gate length selected in the menu.
- Each ~1.33V moves the gate length by one menu step. Over ±5V that is about 4 steps down or up.
- Between steps the gate length changes smoothly.
- The result is limited to the shortest (~1ms) and longest (~2.08s) gate length.

Example: with the menu on step 6 (147ms), −5V gives ~18ms and +5V gives ~1.2s.

In the original firmware the menu setting was ignored in this mode.

### Note flash
In the normal scale display, when a channel outputs a new note, that note's LED goes dark for ~80ms. This shows which notes are being played. If both channels play the same note, it still flashes.

### Startup animation
At power-up the note LEDs light one after another, ~2 seconds in total. This also makes it easy to confirm that new firmware was flashed.

### MIDI out
The quantized notes are sent as MIDI on the TX pin of the serial header J2 (pin 5). Channel A plays on MIDI channel 1, channel B on channel 2.

- 0V is MIDI note 36 (C2). Notes above MIDI note 127 are not sent.
- A note starts when the gate turns on and stops when it turns off. Velocity is always 100.
- With legato on, the new note is sent before the old one is released, so mono synths slide instead of retriggering.

Wiring to a MIDI jack:

| J2 pin | Connect to |
|---|---|
| 3 (+5V) | 220Ω to DIN pin 4 (TRS type A: ring) |
| 5 (TX) | 220Ω to DIN pin 5 (TRS type A: tip) |
| 1 (GND) | DIN pin 2 (TRS: sleeve) |

Leave DTR (pin 6) unconnected. MIDI out uses the UART, so it can't be enabled together with `DEBUGPRINT`. Disconnect the MIDI cable before uploading through J2. To turn MIDI out off, comment out `#define MIDI_OUT` in `KassutronicsQuantizer.ino`.

### Channel B mirror
Channel B can quantize channel A's input, so it works as a second voice. Press Shift+8 (quantize mode menu), then key 11 to toggle. LED 11 is lit when mirroring is on. The setting is saved.

- IN B is ignored while mirroring is on. The module can't detect whether IN B is patched, so turn this on only when you want B to follow A.
- With nothing in TRIG B, B updates together with A: same timing and gates, including repeated notes.
- With a clock in TRIG B, B samples A's input on B's own clock.
- B's transpose, offset and CV settings still apply, e.g. set transpose B to 7 semitones for a parallel fifth.

### Fixes
- Gate length could overflow at long settings with CV applied, which could stop the gate from firing.
- An invalid quantizer result could read past the end of a lookup table.
- The flash timer is updated with interrupts disabled, so a new flash can't be cut short.

### Gate length reference
Gate length for each menu step (key), measured on the module:

| Step | Gate length |
|---:|---:|
| 0 | 1ms |
| 1 | 10ms |
| 2 | 17.7ms |
| 3 | 30ms |
| 4 | 51ms |
| 5 | 86.5ms |
| 6 | 147ms |
| 7 | 250ms |
| 8 | 424ms |
| 9 | 720ms |
| 10 | 1.22s |
| 11 | 2.08s |

### Flashing
The sketch builds with the board set to **Arduino Uno**. When programming through an Uno running the ArduinoISP sketch:

1. Select **Tools → Programmer → Arduino as ISP**.
2. Use **Sketch → Upload Using Programmer** (Ctrl+Shift+U).

A plain **Upload** (Ctrl+U) writes the firmware to the Uno itself, not to the quantizer. In the verbose upload log, the avrdude command should contain `-cstk500v1` and `-b19200`. If it contains `-carduino`, the firmware went to the Uno.

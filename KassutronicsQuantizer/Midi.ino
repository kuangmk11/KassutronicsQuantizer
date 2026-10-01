/* Midi.ino - MIDI output
 * Added 2026-09-29 (fork)
 *
 * Sends MIDI notes on the hardware UART TX pin (PD1), which is pin 5 of the
 * serial header J2. Channel A plays on MIDI channel 1, channel B on channel 2.
 *
 * Notes follow the gate outputs: a note starts when the gate turns on and
 * stops when it turns off. When the gate stays on for a new note (legato), the
 * new note is sent before the old note is released, so mono synths slide
 * instead of retriggering.
 *
 * The UART handles the bit timing, so the ADC interrupt only has to put bytes
 * in a buffer. The buffer is sent by the UART data register empty interrupt.
 */
#ifdef MIDI_OUT

#ifdef DEBUGPRINT
#error "MIDI_OUT and DEBUGPRINT both use the UART, enable only one"
#endif

const byte midibase = 36;      // MIDI note number for 0V (36 = C2)
const byte midivelocity = 100;
const byte midinone = 255;     // No note playing

// Transmit ring buffer. Only the ADC interrupt writes, only the UDRE interrupt reads.
#define MIDI_BUFSIZE 64 // Must be a power of 2
byte midibuf[MIDI_BUFSIZE];
volatile byte midihead = 0;
volatile byte miditail = 0;

// Last status byte put in the buffer, for running status
byte midistatus = 0;

// MIDI note currently playing on each channel
byte midinote[2] = {midinone, midinone};

void setupMidi() {
  UBRR0 = 31; // 16MHz / (16 * 31250) - 1, exact
  UCSR0A = 0;
  UCSR0C = _BV(UCSZ01) | _BV(UCSZ00); // 8N1
  UCSR0B = _BV(TXEN0);
}

byte midiFree() {
  return (miditail - midihead - 1) & (MIDI_BUFSIZE - 1);
}

void midiPut(byte data) {
  midibuf[midihead] = data;
  midihead = (midihead + 1) & (MIDI_BUFSIZE - 1);
}

// Queue a note message. Note off is sent as note on with velocity 0, so running status
// can be used for everything. Caller must check there is room for 3 bytes.
void midiSendNote(byte channel, byte note, byte velocity) {
  byte status = 0x90 | channel;
  if (status != midistatus) {
    midiPut(status);
    midistatus = status;
  }
  midiPut(note);
  midiPut(velocity);
  sbi(UCSR0B, UDRIE0);
}

// Called from processChannel when gate i turns on (or stays on for a new note)
void midiGateOn(byte i, byte outval) {
  byte note = outval + midibase;
  if (outval > 127 - midibase || note == midinote[i]) {
    return;
  }
  /* A note on needs room for itself, a legato note off, and the note offs of
   * both channels afterwards. Note offs then always fit, so a full buffer can
   * drop notes but never leave a note stuck. */
  if (midiFree() < 12) {
    return;
  }
  midiSendNote(i, note, midivelocity);
  if (midinote[i] != midinone) {
    midiSendNote(i, midinote[i], 0);
  }
  midinote[i] = note;
}

// Called from processChannel when gate i turns off
void midiGateOff(byte i) {
  if (midinote[i] == midinone) {
    return;
  }
  midiSendNote(i, midinote[i], 0);
  midinote[i] = midinone;
}

ISR(USART_UDRE_vect) {
  if (miditail == midihead) {
    cbi(UCSR0B, UDRIE0);
  } else {
    UDR0 = midibuf[miditail];
    miditail = (miditail + 1) & (MIDI_BUFSIZE - 1);
  }
}

#endif

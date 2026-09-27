/* Core.ino - I/O processing etc
 *  
 * This file mainly contains the processChannel and processCV functions,
 * which process the ADC data for the channel inputs and CV inputs, respectively,
 * and update the outputs and/or system state depending on the ADC data.
 * 
 * This file also contains all global system state, which is divided into a
 * persistantState structure (which gets autosaved to EEPROM), and non-persistent
 * state variables.  
 */

// Enums
// Menu/operational modes
enum Mode : byte {
  normal = 0,
  rotate = 1,
  transpose = 2,
  transposeB = 3,
  offset = 4,
  offsetB = 5,  
  keyboard = 6,
  load = 7,
  save = 8,
  cvA = 9,
  cvB = 10,
  gatelengthmenu = 11,
  qmodemenu = 12,
  triggerdelaymenu = 13
};

// CV modes
enum CVMode : byte {
  cvgatelength = 0,
  cvrotate = 1,
  cvtranspose = 2,
  cvtransposechannel = 3,
  cvoffset = 4,
  cvoffsetchannel = 5,
  cvload = 12,
  cvoff = 255,
};
#define MAX_CV_MODE 5

// Quantization modes
enum QMode : byte {
  qmnearest = 0,
  qmskip = 1,
  qmequal = 2
};
#define MAX_Q_MODE 2

// Persistent state variables (autosaved to EEPROM)
struct PersistentState {
  unsigned int scale;
  byte gatelengthindex;
  signed char rotatesemitones;
  signed char transposesemitones;
  signed char transposeBsemitones;
  signed char offsetsemitones;
  signed char offsetBsemitones;
  bool gatelegato;
  QMode qmode;
  byte triggerdelay;
  CVMode cvmode[2];
  byte reserved[3];
};
volatile PersistentState state = {
  0b1010110101010000, 0, 0, 0, 0, 0, 0, false, qmnearest, 0, {cvoff, cvoff}, {0,0,0}
};

// Non-persistent global state variables
// Further state is maintained locally in static variables
volatile unsigned int rotatedscale = 0b1010110101010000;
volatile Mode mode = normal;
volatile signed char keyboardoctaves = 0;
volatile signed char keyboardsemitones = 0;
volatile byte keyboardtriggered = 0;
volatile byte rotatesemitonesCV = 0;
volatile signed char transposesemitonesCV[2] = {0,0};
volatile signed char offsetsemitonesCV[2] = {0,0};
volatile int gatelength;
volatile int cvgatelengthindex;
volatile int cvgatelengthremainder;

// Flash feedback state (for LED visual feedback on quantization)
volatile byte flashcounter[2] = {0, 0};  // Flash duration counter per channel
volatile byte flashnote[2] = {255, 255}; // Note being flashed (0-11, or 255 for none)
const byte flashduration = 80; // Flash duration in ~1ms units (~80ms)

// System constants
// Gate lengths in loop counts (208us), i.e. ~1ms to ~2.08s
const int gatelengths[12] = {5, 50, 85, 144, 245, 416, 707, 1201, 2040, 3466, 5887, 10000};
const int quartertone = 4; // The size of one half semitone measured in ADC counts
const int hysteresis = 1; // Hysteresis in averaged ADC counts. Free running mode only
const int gatedelay = 2; // Delay of gate output w.r.t. updating PWM register, in loop counts (208us)
const int triggerdebounce = 6; // Dead time after trigger edge, in loop counts
const int triggerautotime = 10000; // If trigger stays high this long, we switch to free running mode.
const byte triggerdelayscale = 5; // Conversion between trigger delay setting (0-11) and loop counts (208us)
const unsigned int autosavetime = 10000;

// Must be called whenever rotation or scale changes
void updateRotation() {
  byte rotation = mod12(state.rotatesemitones + rotatesemitonesCV);
  unsigned int myscale = state.scale;
  if (rotation > 0) {
    myscale = ((myscale >> rotation) | (myscale << (12 - rotation))) & 0xFFF0;
  }
  rotatedscale = myscale;
}

// Must be called whenver the gate length changes, either through menu or through CV
void updateGatelength() {
  int i = state.gatelengthindex + cvgatelengthindex;

  // Clamp to valid range
  if (i < 0) {
    gatelength = gatelengths[0];
  } else if (i >= 11) {
    gatelength = gatelengths[11];
  } else {
    // Interpolate between gatelengths[i] and gatelengths[i+1] (remainder is 0-127)
    // 32-bit math, since remainder * step size overflows an int for the longer steps
    gatelength = gatelengths[i] + (int)(((long)cvgatelengthremainder * (gatelengths[i+1] - gatelengths[i])) >> 7);
  }
}

// Called when CV mode is changed, so any effect of the old CV mode is removed
void resetCVState(byte i) {
  rotatesemitonesCV = 0;
  updateRotation();
  transposesemitonesCV[i] = 0;
  offsetsemitonesCV[i] = 0;
  cvgatelengthindex = 0;
  cvgatelengthremainder = 0;
}

// Runs at 9.6 kHz (alternating channel i=0,1). 
// Time-critical function, must complete within 52 us!
void processChannel(byte i, int newadcval) {
  // State variables
  static byte outval[2] = {0, 0};
  static byte lastgateoutval[2] = {0, 0};
  static int gatecounter[2] = {0, 0}; 
  static bool freerunning[2] = {true, true};
  static int triggercounter[2] = {0, 0};
  static byte triggerdelay[2] = {0, 0};
  
  // Scale the current output value to ADC units for scaling
  int oldval = outval[i] << 3;
  
  // Add or subtract hysteresis
  int newvalhyst = newadcval;
  if (freerunning[i]) {
    if (newadcval > oldval) {
      newvalhyst -= hysteresis;
    } else if (newadcval < oldval) {
      newvalhyst += (hysteresis - 1);
    }
  }

  // Quantize the new value according to the scale or whatever quantization rules we want to define
  byte candidate;
  byte scaleNote = 255;  // Pre-transpose note for LED flash (255 = no flash)
  bool trig = false;
  if (mode==keyboard) {
    candidate = 63 + (byte)keyboardsemitones + (byte)(12*keyboardoctaves);
    scaleNote = mod12(keyboardsemitones);  // Flash the keyboard note
    trig = keyboardtriggered > 0;
  } else {
    // TODO: Not sure how to elegantly do the offset. Does this even work???
    int newvalhystoffset = newvalhyst + (((int)state.offsetsemitones + (int)offsetsemitonesCV[i]) << 3);
    if (i==1) {
      newvalhystoffset += ((int)state.offsetBsemitones) << 3;
    }
    if (state.qmode==qmnearest) {
      candidate = quantizeNearest(newvalhystoffset);
    } else if (state.qmode==qmskip) {
      candidate = quantizeSkip(newvalhystoffset);
    } else { // qmequal
      candidate = quantizeEqual(newvalhystoffset);
    }
    // Capture pre-transpose note for LED flash
    // candidate is in semitones, use +8 offset to match scale index (same as Quantize.ino line 34)
    // Skip invalid candidates (e.g. 255), which would overrun mod12table
    scaleNote = (candidate < 128) ? mod12(candidate + 8) : 255;

    candidate += state.transposesemitones + transposesemitonesCV[i] - 1;
    if (i==1) {
      candidate += state.transposeBsemitones;
    }
    if (candidate < 128) {
      trig = (freerunning[i] && (candidate != outval[i])) || (triggerdelay[i] == 1);
    } else if (triggerdelay[i] == 1) {
      triggerdelay[i] = 0;
    }
  }
  
  // Decide if we should update the value
  // Note: we ignore any value outside the DAC range
  if (trig) {
   
    // Update the value, rounding to the nearest semitone
    outval[i] = candidate;

    // Trigger LED flash feedback using pre-transpose scale note
    flashnote[i] = scaleNote;
    flashcounter[i] = flashduration;

    // Update the output value
    setDAC(i, outval[i]);

    // If legato is disabled, we turn off the gate now; it will be turned on again after gatedelay.
    if (not state.gatelegato) {
      GATE_OFF(i);
    }

    // Start a delay counter for the gate output, such that the gate only starts once the DAC filter has settled
    gatecounter[i] = -gatedelay;    

    // Reset trigger flag
    triggerdelay[i] = 0;

    // Decrement keyboard trigger flag
    keyboardtriggered--;
  }
  
  // Check if gate should do something. Does not depend on mode.
  if (gatecounter[i] == 0) {
    /* It may happen that outval has changed several times during
     *  gatedelay, and returned to the original value before any gate has been output. 
     *  This happens in particular with a noisy input signal.
     *  In this case we suppress the gate output, making the quantizer more resilient to input noise.
     */
    if (!freerunning[i] || (outval[i] != lastgateoutval[i])) {
      GATE_ON(i);
      lastgateoutval[i] = outval[i];
    }
  }
  if (gatecounter[i] == gatelength) {
    GATE_OFF(i);
  }
  if (gatecounter[i] <= gatelength) {
    gatecounter[i] += 1;
  }

  // Check trigger and mode
  if (freerunning[i]) {
    // In freerunning mode, the DINA/DINB pin is always low (because the jack is normalled high)
    // If we see a high value, a cable has been connected and we switch to triggered mode
    if (DIN_READ(i)) {
      freerunning[i] = false;
      triggercounter[i] = 0;
    }
  } else { // Triggered mode
    // Check if an edge has occured using the interrupt flag
    if ( (EIFR & (1<<i)) ) {
      // Clear interrupt flag by SETTING the bit
      EIFR = (1<<i);
      
      // Debounce
      if (triggercounter[i] >= 0) {
        // Accept as a trigger. It will be used only the next round, such that the input is always sampled after the trigger is recieved
        triggerdelay[i] = (1 + triggerdelayscale*state.triggerdelay);
      }
      // Reset debounce counter
      triggercounter[i] = -triggerdebounce;
    } else {
      if (triggerdelay[i] > 1) {
        triggerdelay[i] --;
      }
      // track if trigger is still low from a previous trigger
      if (!DIN_READ(i)) {
        // Count debounce/timeout counter
        if (triggercounter[i] <= triggerautotime) {
          triggercounter[i] += 1;
        } else {
          // If we reach the timeout, go back to freerunning mode (very long trigger = cable was removed)
          freerunning[i] = true;
        }
      } else {
        // Trigger has ended, but keep counting debounce counter (not above 0 in this case)
        if (triggercounter[i] < 0) {
          triggercounter[i] += 1;
        }
      }
    }
  }
}

// Runs at 9.6 kHz, must complete in 52 us!
void processCV(byte i, int newadcval) {
  static int CV[2] = {0, 0};
  static byte CVquantized[2] = {0, 0};

  if (state.cvmode[i] == cvgatelength) {
    DEBUG_ON(2);

    // CV offsets the menu gate length: 128 ADC counts (~1.33V) per step, 0V (ADC ~512) = menu setting
    // So roughly -5V..+5V gives -4..+4 steps around the menu value
    // The remainder (0-127) is used in updateGatelength to interpolate between the logarithmic steps
    cvgatelengthindex = (newadcval >> 7) - 4;
    cvgatelengthremainder = newadcval & 127;

    updateGatelength();
    DEBUG_OFF(2);
  } else {
  
    // Scale the current quantized value to ADC units
    int oldval = CVquantized[i] << 3;
    
    // Add or subtract hysteresis
    int newvalhyst = newadcval;
    if (newadcval > oldval) {
      newvalhyst -= hysteresis;
    } else if (newadcval < oldval) {
      newvalhyst += (hysteresis - 1);
    }
    
    // Quantize the new value to semitones
    byte candidate = quantizeSemitones(newvalhyst) - 1;
    boolean trig = (candidate != CVquantized[i]);
    CVquantized[i] = candidate;
  
    // Decide if we should do something
    if (trig) {
      switch(state.cvmode[i]) {
        case cvrotate:
          rotatesemitonesCV = mod12(CVquantized[i] - 3);
          updateRotation();
          break;
        case cvtranspose:
          transposesemitonesCV[0] = CVquantized[i] - 63;
          transposesemitonesCV[1] = CVquantized[i] - 63;
          break;
        case cvtransposechannel: 
          transposesemitonesCV[i] = CVquantized[i] - 63;
          break;
        case cvoffset:
          offsetsemitonesCV[0] = CVquantized[i] - 63;
          offsetsemitonesCV[1] = CVquantized[i] - 63;
          break;
        case cvoffsetchannel: 
          offsetsemitonesCV[i] = CVquantized[i] - 63;
          break;
        case cvload:
          byte index = mod12(CVquantized[i] - 3);
          loadScale(index);
          break;
      }
    }
  }
}

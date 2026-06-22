#include "M5Cardputer.h"
#include "AMY-Arduino.h"
#include <vector>
#include "USB.h"
#include "USBMIDI.h"
#include <Preferences.h>

USBMIDI MIDI;
Preferences preferences;

int   currentPatch  = 0;
int   currentBank   = 0; // 0 = Analog (0-127), 128 = FM DX7 (128-255)
int   currentNote   = 60;
float currentVolume = 0.95f;
std::vector<int> activeNotes;

// --- Triple buffer for audio streaming ---
static constexpr size_t BUF_SIZE = 512;
static int16_t tri_buf[3][BUF_SIZE * 2];
static int tri_index   = 0;
static int tri_buf_pos = 0;

// --- Envelope State Variables ---
int   env_attack_ms  = 50;
int   env_decay_ms   = 200;
float env_sustain    = 0.8f;
int   env_release_ms = 400;

// --- Effects State Variables (On/Off) ---
bool fx_chorus_on = false;

// --- MIDI Learn & Abstraction Structures ---
enum SynthParam { 
  PARAM_NONE = 0,
  PARAM_CUTOFF,    // Key 1
  PARAM_RESONANCE, // Key 2
  PARAM_ATTACK,    // Key 3
  PARAM_DECAY,     // Key 4
  PARAM_SUSTAIN,   // Key 5
  PARAM_RELEASE,   // Key 6
  PARAM_CHORUS     // Key 0
};

struct MidiBinding {
  bool is_nrpn;
  int ctrl_id;
};

// Routing table
MidiBinding bindings[10] = {
  {false, -1}, {false, -1}, {false, -1}, {false, -1}, {false, -1},
  {false, -1}, {false, -1}, {false, -1}, {false, -1}, {false, -1}
};

SynthParam learning_param = PARAM_NONE;
int nrpn_param_msb = -1;
int nrpn_param_lsb = -1;
int nrpn_value_msb = -1;

// --- Persistent Memory (NVS) Functions ---
void saveMidiBindings() {
  preferences.begin("amyputer", false); // Open namespace in write mode
  preferences.putBytes("bindings", bindings, sizeof(bindings));
  preferences.end();
}

void loadMidiBindings() {
  preferences.begin("amyputer", true); // Open namespace in read mode
  if (preferences.getBytesLength("bindings") == sizeof(bindings)) {
    preferences.getBytes("bindings", bindings, sizeof(bindings));
  }
  preferences.end();
}

// --- On-screen debug ---
void debugLog(const char* fmt, ...) {
  char buf[128];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  M5Cardputer.Display.fillRect(0, 110, 240, 25, BLACK);
  M5Cardputer.Display.setCursor(0, 115);
  M5Cardputer.Display.setTextColor(YELLOW);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.print(buf);
}

// --- Wire Protocol Helper for Effects ---
void sendAmyMessage(const char* msg) {
  char buffer[128];
  strncpy(buffer, msg, sizeof(buffer) - 1);
  buffer[sizeof(buffer) - 1] = '\0';
  amy_add_message(buffer); 
}

// --- Effects ---
void applyChorus() {
  if (fx_chorus_on) {
    // BOOSTED: Level 1.0, max_delay 320, lfo 0.5Hz, extreme depth 1.0
    sendAmyMessage("y0k1.0,320,0.5,1.0Z");
  } else {
    sendAmyMessage("y0k0Z");  
  }
  debugLog("Chorus: %s", fx_chorus_on ? "ON" : "OFF");
}

// --- Patch Type Deductor ---
const char* getPatchType(int patch_id) {
  if (patch_id < 128) {
    return "Analog (Juno/JX)";
  } else if (patch_id >= 128 && patch_id < 256) {
    return "FM (DX7)";
  } else {
    return "PCM / Other";
  }
}

// --- Display ---
void updateDisplay() {
  M5Cardputer.Display.fillScreen(BLACK);
  M5Cardputer.Display.setCursor(0, 10);
  M5Cardputer.Display.setTextColor(GREEN);
  M5Cardputer.Display.setTextSize(1.5);
  M5Cardputer.Display.printf("AmyPuter\n\n");
  
  M5Cardputer.Display.printf("Patch:  %d\n", currentPatch);
  M5Cardputer.Display.setTextColor(CYAN);
  M5Cardputer.Display.printf("Type:   %s\n", getPatchType(currentPatch));
  
  M5Cardputer.Display.setTextColor(GREEN);
  M5Cardputer.Display.printf("Note:   %d\n", currentNote);
  // --- VISUALIZZAZIONE VOLUME RIPRISTINATA ---
  M5Cardputer.Display.printf("Volume: %.0f%%\n", currentVolume * 100); 
  
  // --- Internal SRAM Monitor ---
  M5Cardputer.Display.setTextColor(ORANGE);
  // Total free internal RAM
  M5Cardputer.Display.printf("RAM:    %d KB free\n", ESP.getFreeHeap() / 1024);
  // The absolute largest contiguous block available for AMY's buffers
  M5Cardputer.Display.printf("MaxBlk: %d KB\n", ESP.getMaxAllocHeap() / 1024);
  
  // FX status bar
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(fx_chorus_on ? GREEN : DARKGREY);
  M5Cardputer.Display.printf("CHO\n");
}

// --- Audio ---
void flushAudioBuffer() {
  if (tri_buf_pos == 0) return;
  bool success = false;
  while (!success) {
    success = M5Cardputer.Speaker.playRaw(
      tri_buf[tri_index], tri_buf_pos, AMY_SAMPLE_RATE,
      true, 1, 0, false 
    );
    if (!success) vTaskDelay(1);
  }
  tri_index = (tri_index + 1) % 3;
  tri_buf_pos = 0;
}

void feedAudio() {
  int16_t* amy_buf = (int16_t*)amy_update();
  if (amy_buf == nullptr) return;
  int samples = AMY_BLOCK_SIZE * 2;
  int src = 0;
  
  while (src < samples) {
    int space = (BUF_SIZE * 2) - tri_buf_pos;
    int copy  = min(space, samples - src);
    memcpy(&tri_buf[tri_index][tri_buf_pos], &amy_buf[src], copy * sizeof(int16_t));
    tri_buf_pos += copy;
    src         += copy;
    if (tri_buf_pos >= (int)(BUF_SIZE * 2)) flushAudioBuffer();
  }
}

// --- Keyboard MIDI mapping ---
int getMidiNote(char key) {
  switch(key) {
    case 'a': return 60;
    case 'w': return 61;
    case 's': return 62; case 'e': return 63;
    case 'd': return 64; case 'f': return 65;
    case 't': return 66; case 'g': return 67;
    case 'y': return 68; case 'h': return 69;
    case 'u': return 70;
    case 'j': return 71;
    case 'k': return 72; case 'i': return 73;
    case 'l': return 74; default:  return -1;
  }
}

// --- Synth setup ---
void setupSynth(int patch) {
  amy_event s = amy_default_event();
  s.synth        = 1;
  s.patch_number = patch;
  s.num_voices   = 10;
  amy_add_event(&s);
  debugLog("Patch: %d", patch);
}

void noteOn(int note) {
  amy_event on = amy_default_event();
  on.synth     = 1;
  on.midi_note = note;
  on.velocity  = 1;
  amy_add_event(&on);
}

void noteOff(int note) {
  amy_event off = amy_default_event();
  off.synth     = 1;
  off.midi_note = note;
  off.velocity  = 0;
  amy_add_event(&off);
}

// --- Parameter update ---
void updateSynthParameter(SynthParam param, float normalizedValue) {
  amy_event mod = amy_default_event();
  mod.synth = 1;
  bool updateEnvelope = false;

  switch (param) {
    case PARAM_CUTOFF: {
      mod.filter_freq_coefs[0] = 20.0f + (normalizedValue * normalizedValue * 10000.0f);
      amy_add_event(&mod);
      debugLog("Cutoff: %.0f Hz", mod.filter_freq_coefs[0]);
      break;
    }
    case PARAM_RESONANCE:
      mod.resonance = 0.5f + (normalizedValue * 9.5f);
      amy_add_event(&mod);
      debugLog("Resonance: %.2f", mod.resonance);
      break;
    case PARAM_ATTACK:
      env_attack_ms = max(5, (int)(normalizedValue * 3000.0f));
      updateEnvelope = true; break;
    case PARAM_DECAY:
      env_decay_ms = max(5, (int)(normalizedValue * 3000.0f));
      updateEnvelope = true;
      break;
    case PARAM_SUSTAIN:
      env_sustain = normalizedValue;
      updateEnvelope = true; break;
    case PARAM_RELEASE:
      env_release_ms = max(5, (int)(normalizedValue * 5000.0f));
      updateEnvelope = true; break;
    case PARAM_CHORUS:
    case PARAM_NONE:
      break;
  }

  if (updateEnvelope) {
    // Declare ONLY 3 pairs. AMY will automatically treat the last one (index 2) as Release.
    mod.eg0_times[0] = env_attack_ms;   mod.eg0_values[0] = 1.0f;
    mod.eg0_times[1] = env_decay_ms;    mod.eg0_values[1] = env_sustain;
    mod.eg0_times[2] = env_release_ms;  mod.eg0_values[2] = 0.0f;
    
    amy_add_event(&mod);
    debugLog("ENV A:%d D:%d S:%.1f R:%d", env_attack_ms, env_decay_ms, env_sustain, env_release_ms);
  }
}

// --- Setup ---
void setup() {
  MIDI.begin();
  USB.begin();

  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Speaker.setVolume((uint8_t)(currentVolume * 255));
  M5Cardputer.Display.setRotation(1);
  
  // Load saved MIDI mappings from NVRAM
  loadMidiBindings();

  // AMY Engine Initialization
  amy_config_t amyCfg = amy_default_config();
  amyCfg.audio = AMY_AUDIO_IS_NONE;
  amyCfg.features.default_synths = 1;
  amyCfg.features.chorus = 1;
  amy_start(amyCfg);              

  setupSynth(currentPatch);
  delay(100);
  updateDisplay();
}

// --- Main Loop ---
void loop() {
  // 1. USB MIDI engine
  midiEventPacket_t rx;
  while (MIDI.readPacket(&rx)) {

    // Note ON
    if (rx.header == 0x09) {
      uint8_t note     = rx.byte2;
      uint8_t velocity = rx.byte3;
      if (velocity == 0) {
        noteOff(note);
      } else {
        amy_event on = amy_default_event();
        on.synth     = 1;
        on.midi_note = note;
        on.velocity  = (float)velocity / 127.0f;
        amy_add_event(&on);
        currentNote = note;
        updateDisplay();
      }
    }
    // Note OFF
    else if (rx.header == 0x08) {
      noteOff(rx.byte2);
    }
    // CC / NRPN
    else if (rx.header == 0x0B) {
      uint8_t cc_num = rx.byte2;
      uint8_t cc_val = rx.byte3;

      bool  is_nrpn_event  = false;
      int   current_ctrl_id = -1;
      float normalized_val  = 0.0f;

      // NRPN state machine
      if (cc_num == 99) { nrpn_param_msb = cc_val;
        continue; 
      }
      if (cc_num == 98) { nrpn_param_lsb = cc_val; 
        continue;
      }

      if (cc_num == 6) {
        nrpn_value_msb = cc_val;
        if (nrpn_param_msb != -1 && nrpn_param_lsb != -1) {
          is_nrpn_event   = true;
          current_ctrl_id = (nrpn_param_msb << 7) | nrpn_param_lsb;
          normalized_val  = (float)cc_val / 127.0f;
        }
      } else if (cc_num == 38) {
        if (nrpn_param_msb != -1 && nrpn_param_lsb != -1 && nrpn_value_msb != -1) {
          is_nrpn_event   = true;
          current_ctrl_id = (nrpn_param_msb << 7) | nrpn_param_lsb;
          int full_value  = (nrpn_value_msb << 7) | cc_val;
          normalized_val  = (float)full_value / 16383.0f;
        }
      } else {
        // Standard CC
        is_nrpn_event   = false;
        current_ctrl_id = cc_num;
        normalized_val  = (float)cc_val / 127.0f;
      }

      if (current_ctrl_id != -1) {
        // Learn mode mapping
        if (learning_param != PARAM_NONE) {
          bindings[learning_param].is_nrpn = is_nrpn_event;
          bindings[learning_param].ctrl_id = current_ctrl_id;
          debugLog("LEARN P%d -> %s%d", learning_param, is_nrpn_event ? "NRPN" : "CC", current_ctrl_id);
          
          learning_param = PARAM_NONE;
          saveMidiBindings();
          // Save mapping persistently
          
          updateDisplay();
          continue;
        }

        // Route control
        for (int i = 1; i <= 9; i++) {
          if (bindings[i].ctrl_id  == current_ctrl_id &&
              bindings[i].is_nrpn  == is_nrpn_event) {
            updateSynthParameter((SynthParam)i, normalized_val);
            break;
          }
        }
      }
    }
  }

  // 2. Audio streaming
  feedAudio();
  M5Cardputer.update();

  // 3. Cardputer keyboard
  if (M5Cardputer.Keyboard.isChange()) {
    Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
    std::vector<int> currentPressedNotes;
    bool triggerLearnDisplay = false;

    for (auto key : status.word) {
      int note = getMidiNote(key);
      if (note != -1) currentPressedNotes.push_back(note);

      // Volume
      if (key == '=') {
        currentVolume = min(1.0f, currentVolume + 0.1f);
        M5Cardputer.Speaker.setVolume((uint8_t)(currentVolume * 255));
        updateDisplay();
      }
      if (key == '-') {
        currentVolume = max(0.0f, currentVolume - 0.1f);
        M5Cardputer.Speaker.setVolume((uint8_t)(currentVolume * 255));
        updateDisplay();
      }

      // MIDI Learn & FX Toggles
      switch(key) {
        case '1': learning_param = PARAM_CUTOFF;
          triggerLearnDisplay = true; break;
        case '2': learning_param = PARAM_RESONANCE; triggerLearnDisplay = true; break;
        case '3': learning_param = PARAM_ATTACK;
          triggerLearnDisplay = true; break;
        case '4': learning_param = PARAM_DECAY;     triggerLearnDisplay = true; break;
        case '5': learning_param = PARAM_SUSTAIN;
          triggerLearnDisplay = true; break;
        case '6': learning_param = PARAM_RELEASE;   triggerLearnDisplay = true; break;
        case '0': fx_chorus_on = !fx_chorus_on; applyChorus(); updateDisplay(); break;
        case 'b': 
          currentBank = (currentBank == 0) ?
          128 : 0; // Switch 0 <-> 128
          currentPatch = currentBank;
          // Go to the first patch of the bank
          setupSynth(currentPatch);
          updateDisplay();
          break;
      }
    }

    if (triggerLearnDisplay && learning_param != PARAM_NONE) {
      M5Cardputer.Display.fillRect(0, 80, 240, 30, RED);
      M5Cardputer.Display.setCursor(10, 88);
      M5Cardputer.Display.setTextColor(WHITE);
      M5Cardputer.Display.setTextSize(1.5);
      M5Cardputer.Display.printf("LEARN: Move a knob...");
    }

    // Note ON
    for (int note : currentPressedNotes) {
      if (std::find(activeNotes.begin(), activeNotes.end(), note) == activeNotes.end()) {
        noteOn(note);
        activeNotes.push_back(note);
      }
    }
    
    // Note OFF
    for (auto it = activeNotes.begin(); it != activeNotes.end(); ) {
      if (std::find(currentPressedNotes.begin(), currentPressedNotes.end(), *it) == currentPressedNotes.end()) {
        noteOff(*it);
        it = activeNotes.erase(it);
      } else {
        ++it;
      }
    }

    // --- Patch navigation ---
    
    // Forward (Enter key)
    if (status.enter) {
      int offset = (currentPatch - currentBank + 1) % 128;
      currentPatch = currentBank + offset;
      
      setupSynth(currentPatch);
      updateDisplay();
    }

    // Backward (Backspace / Delete key)
    if (status.del) {
      // We add 128 before the modulo operator to safely wrap around negative values in C++
      int offset = (currentPatch - currentBank - 1 + 128) % 128;
      currentPatch = currentBank + offset;
      
      setupSynth(currentPatch);
      updateDisplay();
    }
  }
}
#include "M5Cardputer.h"
#include "AMY-Arduino.h"
#include "USB.h"
#include "USBMIDI.h"
#include <Preferences.h>

// --- Global Objects ---
USBMIDI MIDI;
Preferences preferences;

// --- Synthesizer State ---
int   currentPatch       = 0;
int   currentBank        = 0; // 0 = Analog (0-127), 128 = FM DX7 (128-255)
float currentVolume      = 0.80f;
int   currentMidiChannel = 0; // 0 = OMNI (All channels), 1-16 = Specific channel

// --- System & Display State ---
bool need_display_update = false;
char lastDebugMsg[64] = "Ready";
unsigned long last_display_time = 0;

// --- Optimized Keyboard Tracking (No std::vector!) ---
bool activeNotes[128] = {false};

// --- Triple buffer for audio streaming ---
static constexpr size_t BUF_SIZE = 1024;
static int16_t tri_buf[3][BUF_SIZE * 2];
static int tri_index   = 0;
static int tri_buf_pos = 0;

// --- Envelope State Variables ---
int   env_attack_ms  = 50;
int   env_decay_ms   = 200;
float env_sustain    = 0.8f;
int   env_release_ms = 400;
float env_filter_amt = 0.0f;
float lfo_amount     = 0.0f;
float lfo_rate_hz    = 1.0f;
int   lfo_wave       = 0;

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
  PARAM_ENV_FILTER,// Key 7
  PARAM_LFO_AMOUNT,// Key 8
  PARAM_LFO_RATE,  // Key 9
  PARAM_LFO_WAVE,  // 
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
  preferences.begin("amyputer", false);
  preferences.putBytes("bindings", bindings, sizeof(bindings));
  preferences.end();
}

void loadMidiBindings() {
  preferences.begin("amyputer", true);
  if (preferences.getBytesLength("bindings") == sizeof(bindings)) {
    preferences.getBytes("bindings", bindings, sizeof(bindings));
  }
  preferences.end();
}

// --- On-screen debug (Deferred to prevent XRUNS) ---
void debugLog(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(lastDebugMsg, sizeof(lastDebugMsg), fmt, args);
  va_end(args);  
  need_display_update = true;
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

// --- Parameter Name Deductor ---
const char* getParamName(SynthParam param) {
  switch(param) {
    case PARAM_CUTOFF:     return "Cutoff";
    case PARAM_RESONANCE:  return "Resonance";
    case PARAM_ATTACK:     return "Attack";
    case PARAM_DECAY:      return "Decay";
    case PARAM_SUSTAIN:    return "Sustain";
    case PARAM_RELEASE:    return "Release";
    case PARAM_ENV_FILTER: return "Env Filter";
    case PARAM_LFO_AMOUNT: return "LFO Amt";
    case PARAM_LFO_RATE:   return "LFO Rate";
    case PARAM_LFO_WAVE:   return "LFO Wave";
    case PARAM_CHORUS:     return "Chorus";
    default:               return "Unknown";
  }
}

// --- Display ---
void updateDisplay() {
  M5Cardputer.Display.setCursor(0, 10);
  M5Cardputer.Display.setTextColor(GREEN, BLACK);
  M5Cardputer.Display.setTextSize(1.5);
  M5Cardputer.Display.printf("AMYPUTER\n\n");
  
  M5Cardputer.Display.printf("Patch:  %-3d\n", currentPatch);
  
  M5Cardputer.Display.setTextColor(CYAN, BLACK);
  M5Cardputer.Display.printf("Type:   %-20s\n", getPatchType(currentPatch));
  
  M5Cardputer.Display.setTextColor(GREEN, BLACK);
  M5Cardputer.Display.printf("Volume: %-3.0f%% \n", currentVolume * 100); 
  
  M5Cardputer.Display.setTextColor(ORANGE, BLACK);
  M5Cardputer.Display.printf("RAM:    %-3d KB free \n", ESP.getFreeHeap() / 1024);
  M5Cardputer.Display.printf("MaxBlk: %-3d KB    \n", ESP.getMaxAllocHeap() / 1024);
  
  M5Cardputer.Display.setTextColor(YELLOW, BLACK);
  if (currentMidiChannel == 0) {
    M5Cardputer.Display.printf("MIDI Ch: OMNI \n");
  } else {
    M5Cardputer.Display.printf("MIDI Ch: %-2d   \n", currentMidiChannel);
  }

  M5Cardputer.Display.setTextSize(1.5);
  M5Cardputer.Display.setTextColor(fx_chorus_on ? GREEN : DARKGREY, BLACK);
  M5Cardputer.Display.printf("Chorus: %-3s \n", fx_chorus_on ? "ON " : "OFF");
}

// --- Audio Engine (Mapped to Fast RAM) ---
void IRAM_ATTR flushAudioBuffer() {
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

void IRAM_ATTR feedAudio() {
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
    case 'a': return 60; case 'w': return 61;
    case 's': return 62; case 'e': return 63;
    case 'd': return 64; case 'f': return 65;
    case 't': return 66; case 'g': return 67;
    case 'y': return 68; case 'h': return 69;
    case 'u': return 70; case 'j': return 71;
    case 'k': return 72; case 'i': return 73;
    case 'l': return 74; default:  return -1;
  }
}

// --- Synth setup ---
void setupSynth(int patch) {
  amy_event s = amy_default_event();
  s.synth        = 1;
  s.patch_number = patch;
  s.num_voices   = 4;
  amy_add_event(&s);
  debugLog("Patch: %d", patch);
}

void noteOn(int note) {
  amy_event on = amy_default_event();
  on.synth     = 1;
  on.midi_note = note;
  on.velocity  = 1.0f; // Internal keyboard always plays at max velocity
  amy_add_event(&on);
}

void noteOff(int note) {
  amy_event off = amy_default_event();
  off.synth     = 1;
  off.midi_note = note;
  off.velocity  = 0;
  amy_add_event(&off);
}

// --- Parameter update (Ultra-Optimized) ---
void updateSynthParameter(SynthParam param, float normalizedValue) {
  // FM SHIELD: If we are in the DX7 bank, ignore analog ADSR/Filter controls
  if (currentBank == 128 && param != PARAM_CHORUS) return;

  amy_event mod = amy_default_event();
  mod.synth = 1;
  bool updateEnvelope = false;

  switch (param) {
    case PARAM_CUTOFF: {
      mod.filter_freq_coefs[0] = 20.0f + (normalizedValue * normalizedValue * 10000.0f);
      amy_add_event(&mod);
      break;
    }
    case PARAM_RESONANCE:
      mod.resonance = 0.5f + (normalizedValue * 9.5f);
      amy_add_event(&mod);
      break;
    case PARAM_ENV_FILTER:
      env_filter_amt = normalizedValue * 12000.0f; // Aumentato a 12000 per più botta!
      mod.filter_freq_coefs[4] = env_filter_amt; 
      amy_add_event(&mod);
      break;
    case PARAM_ATTACK:
      env_attack_ms = max(5, (int)(normalizedValue * 3000.0f));
      updateEnvelope = true; break;
    case PARAM_DECAY:
      env_decay_ms = max(5, (int)(normalizedValue * 3000.0f));
      updateEnvelope = true; break;
    case PARAM_SUSTAIN:
      env_sustain = normalizedValue;
      updateEnvelope = true; break;
    case PARAM_RELEASE:
      env_release_ms = max(5, (int)(normalizedValue * 5000.0f));
      updateEnvelope = true; break;
    case PARAM_LFO_AMOUNT:
    case PARAM_LFO_RATE:
    case PARAM_LFO_WAVE:
    case PARAM_CHORUS:
    case PARAM_NONE:
      break;
  }

  if (updateEnvelope) {
    mod.eg0_times[0] = env_attack_ms;   mod.eg0_values[0] = 1.0f;
    mod.eg0_times[1] = env_decay_ms;    mod.eg0_values[1] = env_sustain;
    mod.eg0_times[2] = env_release_ms;  mod.eg0_values[2] = 0.0f;
    
    mod.eg1_times[0] = env_attack_ms;   mod.eg1_values[0] = 1.0f;
    mod.eg1_times[1] = env_decay_ms;    mod.eg1_values[1] = env_sustain;
    mod.eg1_times[2] = env_release_ms;  mod.eg1_values[2] = 0.0f;
    
    amy_add_event(&mod);
  }
}

// --- Setup ---
void setup() {
  MIDI.begin();
  USB.begin();

  // Hardware optimizations: disable unused components to save RAM
  auto cfg = M5.config();
  cfg.internal_mic = false; // Frees up I2S/PDM buffers
  cfg.internal_rtc = false; // Frees up I2C traffic
  M5Cardputer.begin(cfg, true);
  
  M5Cardputer.Speaker.setVolume((uint8_t)(currentVolume * 180));
  M5Cardputer.Display.setRotation(1);
  
  loadMidiBindings();
  
  amy_config_t amyCfg = amy_default_config();
  amyCfg.audio = AMY_AUDIO_IS_NONE;
  amyCfg.features.default_synths = 1;
  amyCfg.features.chorus = 1;
  amy_start(amyCfg);              

  setupSynth(currentPatch);
  delay(100);
  need_display_update = true;
}

// --- Main Loop ---
void loop() {
  // 1. USB MIDI engine
  midiEventPacket_t rx;
  while (MIDI.readPacket(&rx)) {
    
    // --- NEW: Global MIDI Channel Filter ---
    // The lower 4 bits of rx.byte1 contain the channel (0-15). We add 1 to get 1-16.
    uint8_t msg_channel = (rx.byte1 & 0x0F) + 1; 

    // If we are not in OMNI mode (0) and the channel doesn't match, drop the packet!
    if (currentMidiChannel != 0 && msg_channel != currentMidiChannel) {
      continue; 
    }

    if (rx.header == 0x09) {
      uint8_t note     = rx.byte2;
      uint8_t velocity = rx.byte3;
      
      if (velocity == 0) {
        noteOff(note);
      } else {
        amy_event on = amy_default_event();
        on.synth     = 1;
        on.midi_note = note;
        
        // --- MIDI VELOCITY NORMALIZATION ---
        // Compresses dynamics: minimum velocity starts at 60% (0.6)
        // and scales up to 100% (1.0). Perfect for external sequencers!
        on.velocity  = 0.6f + (((float)velocity / 127.0f) * 0.4f);
        amy_add_event(&on);
      }
    }
    else if (rx.header == 0x08) {
      noteOff(rx.byte2);
    }
    else if (rx.header == 0x0B) {
      uint8_t cc_num = rx.byte2;
      uint8_t cc_val = rx.byte3;
      bool  is_nrpn_event  = false;
      int   current_ctrl_id = -1;
      float normalized_val  = 0.0f;

      if (cc_num == 99) { nrpn_param_msb = cc_val; continue; }
      if (cc_num == 98) { nrpn_param_lsb = cc_val; continue; }

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
        is_nrpn_event   = false;
        current_ctrl_id = cc_num;
        normalized_val  = (float)cc_val / 127.0f;
      }

      if (current_ctrl_id != -1) {
        if (learning_param != PARAM_NONE) {
          bindings[learning_param].is_nrpn = is_nrpn_event;
          bindings[learning_param].ctrl_id = current_ctrl_id;
          debugLog("LEARN P%d -> %s%d", learning_param, is_nrpn_event ? "NRPN" : "CC", current_ctrl_id);
          learning_param = PARAM_NONE;
          saveMidiBindings();
          need_display_update = true;
          continue;
        }
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

  // 2. Audio Engine processing
  feedAudio();
  
  M5Cardputer.update();

  // 3. Cardputer keyboard
  if (M5Cardputer.Keyboard.isChange()) {
    Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
    
    // Super fast temporary array (allocated on stack, not heap)
    bool currentPressedNotes[128] = {false};
    bool triggerLearnDisplay = false;

    for (auto key : status.word) {
      // 1. Check if it's a musical note
      int note = getMidiNote(key);
      if (note != -1) {
        currentPressedNotes[note] = true;
      }

      // 2. Check system shortcuts
      if (key == '=') {
        currentVolume += 0.1f;
        if (currentVolume > 1.0f) currentVolume = 1.0f; // 100% safe maximum ceiling
        M5Cardputer.Speaker.setVolume((uint8_t)(currentVolume * 180));
        need_display_update = true;
      }
      if (key == '-') {
        currentVolume -= 0.1f;
        if (currentVolume < 0.0f) currentVolume = 0.0f; // 0% floor
        M5Cardputer.Speaker.setVolume((uint8_t)(currentVolume * 180));
        need_display_update = true;
      }

      // 3. Check Synthesizer modifiers
      switch(key) {
        case '1': learning_param = PARAM_CUTOFF; triggerLearnDisplay = true; break;
        case '2': learning_param = PARAM_RESONANCE; triggerLearnDisplay = true; break;
        case '3': learning_param = PARAM_ATTACK; triggerLearnDisplay = true; break;
        case '4': learning_param = PARAM_DECAY; triggerLearnDisplay = true; break;
        case '5': learning_param = PARAM_SUSTAIN; triggerLearnDisplay = true; break;
        case '6': learning_param = PARAM_RELEASE; triggerLearnDisplay = true; break;
        case '7': learning_param = PARAM_ENV_FILTER; triggerLearnDisplay = true; break;
        case '8': learning_param = PARAM_LFO_AMOUNT; triggerLearnDisplay = true; break;
        case '9': learning_param = PARAM_LFO_RATE; triggerLearnDisplay = true; break;
        case '0': fx_chorus_on = !fx_chorus_on; applyChorus(); need_display_update = true; break;
        case 'c': 
          currentMidiChannel++;
          if (currentMidiChannel > 16) currentMidiChannel = 0; // Wrap back to OMNI
          need_display_update = true;
          break;
        case 'b': 
          currentBank = (currentBank == 0) ? 128 : 0; 
          currentPatch = currentBank;
          setupSynth(currentPatch);
          need_display_update = true;
          break;
      }
    }

    // Update Display for Learn mode
    if (triggerLearnDisplay && learning_param != PARAM_NONE) {
      M5Cardputer.Display.fillRect(0, 80, 240, 30, RED);
      M5Cardputer.Display.setCursor(10, 88);
      M5Cardputer.Display.setTextColor(WHITE);
      M5Cardputer.Display.setTextSize(1.5);
      M5Cardputer.Display.printf("LEARN [%s]: Move knob", getParamName(learning_param));
    }

    // --- Differential Polyphonic Engine (No dynamic allocations!) ---
    for (int i = 0; i < 128; i++) {
      if (currentPressedNotes[i] && !activeNotes[i]) {
        // Key just pressed
        noteOn(i);
        activeNotes[i] = true;
      } else if (!currentPressedNotes[i] && activeNotes[i]) {
        // Key just released
        noteOff(i);
        activeNotes[i] = false;
      }
    }
    
    // --- Patch Navigation (Continuous 0-255 loop) ---
    if (status.enter) {
      currentPatch = (currentPatch + 1) % 256;
      currentBank = (currentPatch >= 128) ? 128 : 0; 
      setupSynth(currentPatch);
      need_display_update = true;
    }

    if (status.del) {
      currentPatch = (currentPatch - 1 + 256) % 256;
      currentBank = (currentPatch >= 128) ? 128 : 0; 
      setupSynth(currentPatch);
      need_display_update = true;
    }
  }

  // 4. DISPLAY FPS LIMITER
  if (need_display_update && (millis() - last_display_time > 30)) {
    updateDisplay();
    need_display_update = false;
    last_display_time = millis();
  }
}
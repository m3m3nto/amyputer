#include "M5Cardputer.h"
#include "AMY-Arduino.h"
#include <vector>
#include "USB.h"
#include "USBMIDI.h"

USBMIDI MIDI;
int   currentPatch  = 0;
int   currentNote   = 60;
float currentVolume = 0.95f;
std::vector<int> activeNotes;

// ─── Triple buffer — pattern ufficiale M5Unified per streaming continuo ───────
// Tre buffer che si alternano: mentre M5Unified riproduce uno,
// AMY riempie il successivo, eliminando gap e glitch
static constexpr size_t BUF_SIZE = 512; // campioni stereo per buffer
static int16_t tri_buf[3][BUF_SIZE * 2]; // *2 perché stereo (L+R)
static int tri_index    = 0;
static int tri_buf_pos  = 0;

// ─── Display ──────────────────────────────────────────────────────────────────
void updateDisplay() {
  M5Cardputer.Display.fillScreen(BLACK);
  M5Cardputer.Display.setCursor(0, 10);
  M5Cardputer.Display.setTextColor(GREEN);
  M5Cardputer.Display.setTextSize(1.5);
  M5Cardputer.Display.printf("AMY Synth\n\n");
  M5Cardputer.Display.printf("Patch:  %d\n",     currentPatch);
  M5Cardputer.Display.printf("Note:   %d\n",     currentNote);
  M5Cardputer.Display.printf("Volume: %.0f%%\n", currentVolume * 100);
}

// ─── Flush buffer corrente verso M5Unified ────────────────────────────────────
void flushAudioBuffer() {
  if (tri_buf_pos == 0) return;

  bool success = false;
  
  // Tenta di accodare il buffer. 
  // Se la coda di M5Unified è piena, success diventerà false.
  while (!success) {
    success = M5Cardputer.Speaker.playRaw(
      tri_buf[tri_index],
      tri_buf_pos,
      AMY_SAMPLE_RATE,
      true,     // stereo
      1,        // repeat
      0,        // canale virtuale 0
      false     // FONDAMENTALE: false significa "accoda", non interrompere l'audio in esecuzione!
    );
    
    // Se la coda è temporaneamente piena, attendiamo 1 tick di sistema.
    // A differenza di prima, mentre aspettiamo l'audio STA ANCORA SUONANDO in background, 
    // quindi non si verificheranno gap o fastidiosi ronzi!
    if (!success) {
      vTaskDelay(1); 
    }
  }

  tri_index = (tri_index + 1) % 3;
  tri_buf_pos = 0;
}

// ─── Consuma un blocco AMY e lo copia nel triple buffer ───────────────────────
void feedAudio() {
  int16_t* amy_buf = (int16_t*)amy_update();
  if (amy_buf == nullptr) return;

  // AMY_BLOCK_SIZE = frame stereo, ogni frame = 2 campioni (L, R)
  int samples = AMY_BLOCK_SIZE * 2;
  int src = 0;

  while (src < samples) {
    int space = (BUF_SIZE * 2) - tri_buf_pos;
    int copy  = min(space, samples - src);
    memcpy(&tri_buf[tri_index][tri_buf_pos], &amy_buf[src], copy * sizeof(int16_t));
    tri_buf_pos += copy;
    src         += copy;

    if (tri_buf_pos >= (int)(BUF_SIZE * 2)) {
      flushAudioBuffer();
    }
  }
}

// ─── Midi mapping ───────────────────────
int getMidiNote(char key) {
  switch(key) {
    case 'a': return 60; // DO  (C4)
    case 'w': return 61; // DO#
    case 's': return 62; // RE  (D4)
    case 'e': return 63; // RE#
    case 'd': return 64; // MI  (E4)
    case 'f': return 65; // FA  (F4)
    case 't': return 66; // FA#
    case 'g': return 67; // SOL (G4)
    case 'y': return 68; // SOL#
    case 'h': return 69; // LA  (A4)
    case 'u': return 70; // LA#
    case 'j': return 71; // SI  (B4)
    case 'k': return 72; // DO  (C5)
    case 'i': return 73; // DO#
    case 'l': return 74; // RE  (D5)
    default:  return -1; // Tasto non musicale
  }
}

// ─── Synth setup ──────────────────────────────────────────────────────────────
void setupSynth(int patch) {
  amy_event s = amy_default_event();
  s.synth        = 1;
  s.patch_number = patch;
  s.num_voices   = 4;
  amy_add_event(&s);
  Serial.printf("Synth setup: patch %d\n", patch);
}

// ─── Controllo Note (ON / OFF) ────────────────────────────────────────────────
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
  off.midi_note = note; // Ora specifichiamo quale nota stiamo rilasciando!
  off.velocity  = 0;
  amy_add_event(&off);
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("--- BOOT ---");

  MIDI.begin(); 
  USB.begin();  
  Serial.println("USB MIDI Avviato");

  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Speaker.setVolume((uint8_t)(currentVolume * 255));
  Serial.println("M5Cardputer OK");

  M5Cardputer.Display.setRotation(1);

  // AMY in modalità AMY_AUDIO_IS_NONE: genera PCM puro, non tocca I2S
  amy_config_t amyCfg = amy_default_config();
  amyCfg.audio = AMY_AUDIO_IS_NONE;
  amyCfg.features.default_synths = 1;
  amy_start(amyCfg);
  Serial.println("AMY started OK");

  setupSynth(currentPatch);
  delay(100);

  updateDisplay();
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
  // 1. Lettura dei pacchetti USB MIDI in arrivo da Ardour
  midiEventPacket_t rx;
  
  while (MIDI.readPacket(&rx)) {
    // rx.header contiene il tipo di comando
    // rx.byte2 contiene il numero della nota MIDI
    // rx.byte3 contiene la velocity
    
    if (rx.header == 0x09) { // 0x09 = Note ON
      uint8_t note = rx.byte2;
      uint8_t velocity = rx.byte3;
      
      // Alcuni software inviano Note ON con velocity 0 per indicare un Note OFF
      if (velocity == 0) {
        noteOff(note); 
      } else {
        amy_event on = amy_default_event();
        on.synth     = 1;
        on.midi_note = note;
        on.velocity  = (float)velocity / 127.0f; // Adattiamo la scala 1-127 a 0.0-1.0
        amy_add_event(&on);
        
        currentNote = note;
        updateDisplay();
      }
    } 
    else if (rx.header == 0x08) { // 0x08 = Note OFF
      uint8_t note = rx.byte2;
      noteOff(note);
    }
  }

  // 2. Continuiamo con la riproduzione audio e la tastiera fisica
  feedAudio();
  M5Cardputer.update();

  if (M5Cardputer.Keyboard.isChange()) {
    Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
    
    // 1. Raccogliamo tutte le note valide attualmente premute
    std::vector<int> currentPressedNotes;
    
    for (auto key : status.word) {
      int note = getMidiNote(key);
      if (note != -1) {
        currentPressedNotes.push_back(note);
      }
      
      // Controlli Volume
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
    }

    // 2. Accendiamo le NUOVE note (presenti ora, ma non attive prima)
    for (int note : currentPressedNotes) {
      if (std::find(activeNotes.begin(), activeNotes.end(), note) == activeNotes.end()) {
        noteOn(note);
        activeNotes.push_back(note); // Aggiungiamola alla lista delle note attive
      }
    }

    // 3. Spegniamo le VECCHIE note (erano attive, ma ora il tasto è rilasciato)
    for (auto it = activeNotes.begin(); it != activeNotes.end(); ) {
      if (std::find(currentPressedNotes.begin(), currentPressedNotes.end(), *it) == currentPressedNotes.end()) {
        noteOff(*it);
        it = activeNotes.erase(it); // Rimuoviamola dalla lista
      } else {
        ++it;
      }
    }

    // 4. Cambio Patch (Viene riassegnato il setupSynth SOLO qui!)
    if (status.enter) {
      currentPatch = (currentPatch + 1) % 128;
      setupSynth(currentPatch); 
      updateDisplay();
    }
  }
}
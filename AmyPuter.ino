#include "M5Cardputer.h"
#include "AMY-Arduino.h"

int   currentPatch  = 0;
int   currentNote   = 60;
float currentVolume = 0.8f;
bool isNotePlaying = false;

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
void noteOn(int note, int patch) {
  setupSynth(patch); // Assicura che la patch sia applicata
  
  amy_event on = amy_default_event();
  on.synth     = 1;
  on.midi_note = note;
  on.velocity  = 1;  // Accende il suono
  amy_add_event(&on);
  
  Serial.printf("Note ON: %d  Patch: %d\n", note, patch);
}

void noteOff() {
  amy_event off = amy_default_event();
  off.synth    = 1;
  off.velocity = 0;  // Spegne il suono (fase di release dell'inviluppo)
  amy_add_event(&off);
  
  Serial.println("Note OFF");
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("--- BOOT ---");

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
  // Feeding audio continuo 
  feedAudio();
  M5Cardputer.update();

  // Verifica se lo stato della tastiera è CAMBIATO (tasto premuto o rilasciato)
  if (M5Cardputer.Keyboard.isChange()) {
    
    // Se c'è un tasto PREMUTO in questo momento:
    if (M5Cardputer.Keyboard.isPressed()) {
      Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
      
      for (auto key : status.word) {
        if (key >= 'a' && key <= 'z') {
          // Monofonia: se stava già suonando una nota, spegnila prima di avviare la nuova
          if (isNotePlaying) noteOff();

          currentNote = 60 + (key - 'a');
          noteOn(currentNote, currentPatch);
          isNotePlaying = true;
          updateDisplay();
        }
        
        // Controllo Volume
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

      // Cambio Patch (Tasto Enter)
      if (status.enter) {
        currentPatch = (currentPatch + 1) % 128;
        updateDisplay();
      }
      
    } 
    // Se lo stato è cambiato ma NON c'è alcun tasto premuto (Tasto RILASCIATO):
    else {
      if (isNotePlaying) {
        noteOff();
        isNotePlaying = false;
      }
    }
  }
}
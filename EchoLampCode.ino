
// NOTES:
// - MAX9814 has Automatic Gain Control, so I used noise-floor tracking + envelope + peak-hold.
// - analogVal is the final "audio energy" mapped to 0..1023 for the effects.
// - LED 0 is the INNER start of the spiral.


#include <FastLED.h>     // Blending colours...
FASTLED_USING_NAMESPACE  // So I can write functions without "FastLED:"


// HARDWARE (LEDS + MIC)

// LED strip parameters
#define NUM_LEDS 120
#define DATA_PIN 7
#define LED_TYPE WS2812B
#define COLOR_ORDER GRB
#define ENVELOPE_PIN A0

#define BRIGHTNESS 200  // BRIGHTNESS limits max current draw and overall intensity.

// FastLED framebuffer. To write colors into leds[] and call FastLED.show() to display.
CRGB leds[NUM_LEDS];


// LOOK (IDLE)
// Start in idle mode (dim warm white) so the strip never goes fully black.
const CRGB IDLE_WARM = CRGB(5, 3, 0);

// AUDIO PIPELINE (MAX9814 w/ AGC)

// ---------- MADE WITH CHAT GPT ------------
// Estimated "silence level" (noise floor / DC offset).
// I subtract this from the raw analogRead so silence doesn't trigger the LEDs.
int analogVal = 0;   // final "audio energy" mapped to 0..1023 for the effects.

float floorVal = 0;
float env = 0;
float envHold = 0;  // Makes peaks visible for a short time ("flare") and then fades out smoothly.

float peak = 140;   // Keeps the effect usable across different volumes (quiet vs loud music).

// Noise floor adapts only while quiet.
const float FLOOR_ALPHA_QUIET = 0.01f;
// -----------------------------------------

// ATTACK higher  -> reacts faster to rising sound
// RELEASE smaller -> slower fade (smoother, less flicker)
const float ATTACK = 0.35f;
const float RELEASE = 0.02f;

// Peak-hold behaviour
// ACTIVE decay: how long peaks "hang" during music
// QUIET decay : how fast peaks disappear in silence (kills random flashes)
// FEED margin : only feed hold when clearly above gate (noise protection)
const float HOLD_DECAY_ACTIVE = 0.85f;
const float HOLD_DECAY_QUIET = 0.70f;
const float HOLD_FEED_MARGIN = 4.0f;

// PEAK_MIN higher -> less sensitive / more stable
const float PEAK_RISE = 0.03f;
const float PEAK_FALL_QUIET = 0.05f;
const float PEAK_MIN = 120.0f;

// ---------- MADE WITH CHAT GPT ------------
// Gate state: when false it shows a steady warm-white idle.
// When true it runs the reactive effect.
const float GATE_ON = 27.0f;
const float GATE_OFF = 22.0f;
bool reacting = false;
// -----------------------------------------


// VISUAL SMOOTHING

// Base stability smoothing
int activeLEDsSmooth = 0;
const uint8_t ACTIVE_SMOOTH = 45;  // lower = more stable, higher = more reactive


// HELPERS

static inline void renderIdle() {
  for (int i = 0; i < NUM_LEDS; i++) {
    nblend(leds[i], IDLE_WARM, 25); // Idle: gently blend everything back to warm white
  }
  FastLED.show();
  delay(5);
}

static inline void renderReactive() {
  // Main reactive effect:
  // - Smooth that length to reduce flicker
  // - Blend active LEDs toward a warm colour based on loudness
  // - Keep inactive LEDs near warm-white

  float loudness = analogVal / 1023.0f;  // 0..1

  // How many LEDs are active (nonlinear + smoothed)
  uint16_t aa = analogVal;
  uint32_t a2 = (uint32_t)aa * aa;
  int activeLEDs = map((int)(a2 >> 10), 0, 1023, 0, NUM_LEDS);

  activeLEDsSmooth = lerp8by8(activeLEDsSmooth, activeLEDs, ACTIVE_SMOOTH);
  activeLEDs = activeLEDsSmooth; // Smooth active LED count so it doesn't jump around frame-to-frame.

  // COLOURS
  // Pick two warm colours for the inner "core" and blend between them based on loudness.
  CRGB coreA = CRGB(255, 60, 0);   // orange
  CRGB coreB = CRGB(255, 140, 0);  // yellow-orange
  CRGB coreColor = blend(coreA, coreB, (uint8_t)(loudness * 255));

  // The outer "shell" stays red to visualise strong energy at the edges.
  CRGB shellColor = CRGB(255, 0, 0);

  // Where does the core end inside the active area?
  // Small loudness -> larger red dominance; high loudness -> more warm core
  float coreFrac = 0.25f + 0.45f * loudness;
  int coreCount = (int)(activeLEDs * coreFrac);

  // Render strip: first "activeLEDs" pixels react to audio, the rest fades to warm idle.
  for (int i = 0; i < NUM_LEDS; i++) {
    if (i < activeLEDs) { // active (music-driven) area
      if (i < coreCount) {
        // Core region: warm/bright, slightly brighter when loud
        uint8_t v = (uint8_t)constrain(80 + (int)(175 * loudness), 0, 255);
        CRGB c = coreColor;
        c.nscale8_video(v);
        nblend(leds[i], c, 22);
      } else {
        // Shell region: red, fade a bit toward the edge for depth
        // edgeFade goes 255 -> ~120 across shell
        int shellPos = i - coreCount;
        int shellLen = max(1, activeLEDs - coreCount);
        uint8_t edgeFade = (uint8_t)constrain(255 - (shellPos * 135) / shellLen, 120, 255);

        CRGB c = shellColor;
        c.nscale8_video(edgeFade);
        nblend(leds[i], c, 22);
      }
    } else {
      // Inactive region: calm warm idle
      nblend(leds[i], IDLE_WARM, 10);
    }
  }

  FastLED.show();
  delay(5);
}


// ARDUINO (SETUP)

void setup() {
  pinMode(ENVELOPE_PIN, INPUT);

  // Initialize LED strip
  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS)
    .setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(BRIGHTNESS);

  // Start in idle warm-white (so it's never fully black at boot)
  fill_solid(leds, NUM_LEDS, IDLE_WARM);
  FastLED.show();

  // Startup calibration: estimate initial noise floor by averaging samples
  long s = 0;
  for (int i = 0; i < 300; i++) {
    s += analogRead(ENVELOPE_PIN);
    delay(2);
  }
  floorVal = s / 300.0f;

  // Reset audio state
  env = 0;
  envHold = 0;
  peak = 140;

  activeLEDsSmooth = 0;
}


// ARDUINO LOOP ()

void loop() {
  // 1) Read mic (raw)
  // 2) Subtract noise floor (x)
  // 3) Smooth level (env)
  // 4) Gate active/idle
  // 5) Peak-hold (envHold)
  // 6) Auto-range normalise (peak)
  // 7) Render effect or idle warm-white

  // 1) Read mic (raw)
  int raw = analogRead(ENVELOPE_PIN);

  // 2) Subtract noise floor (x)
  // Signal above baseline (silence becomes ~0)
  float x = raw - floorVal;
  if (x < 0) x = 0;

  // 3) Smooth level (env)
  // Envelope follower: fast up, slow down (main anti-flicker)
  float a = (x > env) ? ATTACK : RELEASE;
  env = (1.0f - a) * env + a * x;

  // 4) Gate active/idle
  // Gate: decide if it's "reacting" or just showing idle warm-white
  if (!reacting && env > GATE_ON) reacting = true;
  if (reacting && env < GATE_OFF) reacting = false;

  // 5) Peak-hold (envHold)
  // Peak-hold: keeps peaks visible briefly (prevents choppy visuals)
  // Only feed hold if env is clearly above the gate (noise protection)
  float gateForFeed = GATE_ON + HOLD_FEED_MARGIN;

  envHold *= reacting ? HOLD_DECAY_ACTIVE : HOLD_DECAY_QUIET;

  if (env > gateForFeed && env > envHold) {
    envHold = env;
  }

  // ---------- MADE WITH CHAT GPT ------------
  // 6) Auto-range normalise (peak)
  // Let peak rise during music, but mainly relax downward while quiet.
  if (!reacting) {
    // Update noise floor only while quiet (prevents Automatic Gain Control during music)
    floorVal = (1.0f - FLOOR_ALPHA_QUIET) * floorVal + FLOOR_ALPHA_QUIET * raw;

    if (envHold > peak) peak = (1.0f - PEAK_RISE) * peak + PEAK_RISE * envHold;
    else                peak = (1.0f - PEAK_FALL_QUIET) * peak + PEAK_FALL_QUIET * envHold;
  } else {
    if (envHold > peak) peak = (1.0f - PEAK_RISE) * peak + PEAK_RISE * envHold;
  }

  if (peak < PEAK_MIN) peak = PEAK_MIN;

  // Map energy
  // Final "energy" used by the effect:
  analogVal = (int)constrain(map((int)envHold, 0, (int)peak, 0, 1023), 0, 1023);
  //------------------------------------------
  
  // 7) Render effect or idle warm-white
  if (!reacting) renderIdle();
  else           renderReactive();
}

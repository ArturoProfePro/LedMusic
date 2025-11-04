/* Merged: user's Serial-driven Visualizer + AlexGyver ColorMusic effects
   - WS2812B, 29 LEDs, LED_PIN = 12
   - Button on BTN_PIN = 3 (GyverButton)
   - Audio input comes only via Serial in format: "vu:340;bands:10,20,30\n"
   - No IR, no analog microphone, no FHT (FFT). Modes 0..8 from AlexGyver preserved.
   - EEPROM: saves this_mode, BRIGHTNESS, EMPTY_BRIGHT on changes (and on start read).
*/

#include <FastLED.h>
#include <EEPROMex.h>
#include "GyverButton.h"

#define NUM_LEDS 29
#define LED_PIN 12
#define BTN_PIN 3

// Keep many of AlexGyver's defines, adapt for serial mode
#define MODE_AMOUNT 9      // режимов (0..8)
#define MAIN_LOOP 5        // ms основной цикл
// signal smoothing params (can be tuned)
float SMOOTH = 0.3;               // VU
float SMOOTH_FREQ = 0.8;          // freq smoothing
float MAX_COEF = 1.8;             // VU max coef
float MAX_COEF_FREQ = 1.2;        // colormusic threshold
#define SMOOTH_STEP 20            // colormusic decay step

#define RAINBOW_STEP 6
#define RAINBOW_STEP_2 0.5
#define RAINBOW_PERIOD 1

#define LOW_COLOR HUE_RED
#define MID_COLOR HUE_GREEN
#define HIGH_COLOR HUE_YELLOW

#define STROBE_PERIOD 140
#define STROBE_DUTY 20
#define STROBE_COLOR HUE_YELLOW
#define STROBE_SAT 0
byte STROBE_SMOOTH = 200;

byte EMPTY_BRIGHT = 30;
#define EMPTY_COLOR HUE_PURPLE

// power / brightness
byte BRIGHTNESS = 200;
#define CURRENT_LIMIT 3000

// misc
#define LIGHT_SMOOTH 2
byte RUNNING_SPEED = 11;
byte HUE_STEP = 5;
byte HUE_START = 0;

CRGB leds[NUM_LEDS];
GButton butt1(BTN_PIN);

// palette (green to red)
DEFINE_GRADIENT_PALETTE(soundlevel_gp) {
  0,    0,    255,  0,
  100,  255,  255,  0,
  150,  255,  100,  0,
  200,  255,  50,   0,
  255,  255,  0,    0
};
CRGBPalette32 myPal = soundlevel_gp;

float RsoundLevel_f = 0;
float LsoundLevel_f = 0;
float averageLevel = 50;
int maxLevel = 100;
int MAX_CH = NUM_LEDS / 2;
int hue = 0;
unsigned long main_timer = 0, hue_timer = 0, strobe_timer = 0, running_timer = 0, color_timer = 0, rainbow_timer = 0, eeprom_timer = 0;
float averK = 0.006;
byte count = 0;
float indexPal = (float)255 / (float)MAX_CH;

int Rlenght = 0, Llenght = 0;
int colorMusic[3] = {0,0,0};              // bands low, mid, high (from Serial)
float colorMusic_f[3] = {0,0,0}, colorMusic_aver[3] = {0,0,0};
boolean colorMusicFlash[3] = {false,false,false};
byte this_mode = 0;
int thisBright[3] = {0,0,0};
int strobe_bright = 0;
unsigned int light_time = STROBE_PERIOD * STROBE_DUTY / 100;
boolean strobeUp_flag = false, strobeDwn_flag = false;

// freq buffer (simulated from bands)
int freq_f[32] = {0};
int freq_max = 5;
float freq_max_f = 5;

// running flags (for running effects)
boolean running_flag[3] = {false,false,false};

// EEPROM / settings
boolean eeprom_flag = false;

// Serial input buffer
String inputString = "";
bool stringComplete = false;

// forward declarations
void updateVisuals();
void animation();
void parseSerialData(String data);
void buttonTick();
void updateEEPROM();
void readEEPROM();
void eepromTick();
void fillFreqFromBands(); // simulate freq_f[] from colorMusic[]

void setup() {
  Serial.begin(115200);

  FastLED.addLeds<WS2811, LED_PIN, GRB>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  if (CURRENT_LIMIT > 0) FastLED.setMaxPowerInVoltsAndMilliamps(5, CURRENT_LIMIT);
  FastLED.setBrightness(BRIGHTNESS);

  pinMode(13, OUTPUT);
  digitalWrite(13, LOW);
  butt1.setTimeout(900);

  // read EEPROM settings if present
  if (EEPROM.read(100) == 100) {
    readEEPROM();
  } else {
    // mark first run and save defaults
    EEPROM.write(100, 100);
    updateEEPROM();
  }

  // initialize timers
  main_timer = millis();
  hue_timer = millis();
  rainbow_timer = millis();
  eeprom_timer = millis();
}

void loop() {
  serialEvent();      // read Serial (from Python)
  butt1.tick();
  buttonTick();       // button handling (also increments mode on single press)

  // rainbow hue update
  if (millis() - hue_timer > RAINBOW_STEP) {
    if (++hue >= 255) hue = 0;
    hue_timer = millis();
  }

  // main loop
  if (millis() - main_timer > MAIN_LOOP) {
    main_timer = millis();
    updateVisuals();
  }

  eepromTick();
}

// --------------------- Serial ---------------------
void serialEvent() {
  while (Serial.available()) {
    char inChar = (char)Serial.read();
    if (inChar == '\n') {
      stringComplete = true;
      break;
    } else {
      inputString += inChar;
    }
  }

  if (stringComplete) {
    parseSerialData(inputString);
    inputString = "";
    stringComplete = false;
  }
}

void parseSerialData(String data) {
  // ожидается строка вида: vu:340;bands:10,20,30
  int vuIndex = data.indexOf("vu:");
  int bandsIndex = data.indexOf("bands:");

  if (vuIndex != -1) {
    int semicolon = data.indexOf(';', vuIndex);
    if (semicolon == -1) semicolon = data.length();
    String val = data.substring(vuIndex + 3, semicolon);
    val.trim();
    float v = val.toFloat();
    // Serial provides a raw value — we use it as-is and filter later
    RsoundLevel_f = v;
    LsoundLevel_f = v;
  }

  if (bandsIndex != -1) {
    String bandsStr = data.substring(bandsIndex + 6);
    bandsStr.trim();
    // parse three comma separated ints
    int b0 = bandsStr.indexOf(',');
    int b1 = bandsStr.indexOf(',', b0 + 1);
    if (b0 == -1 || b1 == -1) {
      // not enough bands, try single value
      int val = bandsStr.toInt();
      colorMusic[0] = val;
      colorMusic[1] = val;
      colorMusic[2] = val;
    } else {
      String s0 = bandsStr.substring(0, b0);
      String s1 = bandsStr.substring(b0 + 1, b1);
      String s2 = bandsStr.substring(b1 + 1);
      colorMusic[0] = s0.toInt();
      colorMusic[1] = s1.toInt();
      colorMusic[2] = s2.toInt();
    }
    // after receiving bands, derive freq_f[] approximation
    fillFreqFromBands();
  }
}

// create an approximated freq_f[] array from 3-band data
void fillFreqFromBands() {
  // Normalize band values to 0..255 range (bands might be 0..1000 etc.)
  int maxBand = max(max(colorMusic[0], colorMusic[1]), colorMusic[2]);
  float scale = 1.0;
  if (maxBand > 0) scale = 255.0 / maxBand;
  for (int i = 0; i < 32; i++) {
    // distribute: lower indices -> low band, middle -> mid, high -> high
    if (i < 10) freq_f[i] = constrain((int)(colorMusic[0] * scale), 0, 255);
    else if (i < 20) freq_f[i] = constrain((int)(colorMusic[1] * scale), 0, 255);
    else freq_f[i] = constrain((int)(colorMusic[2] * scale), 0, 255);
  }
  // update freq_max
  freq_max = 5;
  for (int i = 0; i < 30; i++) if (freq_f[i + 2] > freq_max) freq_max = freq_f[i + 2];
  if (freq_max < 5) freq_max = 5;
}

// --------------------- Visuals / main logic ---------------------
void updateVisuals() {
  switch (this_mode) {
    case 0: // VU — green->red
    case 1: { // VU — rainbow
      // RsoundLevel_f и LsoundLevel_f пришли из Serial
      averageLevel = (RsoundLevel_f + LsoundLevel_f) / 2.0 * averK + averageLevel * (1.0 - averK);
      maxLevel = (int)(averageLevel * MAX_COEF);
      if (maxLevel <= 0) maxLevel = 1;

      Rlenght = map((int)RsoundLevel_f, 0, maxLevel, 0, MAX_CH);
      Llenght = map((int)LsoundLevel_f, 0, maxLevel, 0, MAX_CH);
      Rlenght = constrain(Rlenght, 0, MAX_CH);
      Llenght = constrain(Llenght, 0, MAX_CH);

      animation();
      break;
    }

    case 2: // 5 stripes (uses colorMusic)
    case 3: // 3 stripes
    case 4: { // 1 stripe (flash)
      for (byte i = 0; i < 3; i++) {
        colorMusic_aver[i] = colorMusic[i] * averK + colorMusic_aver[i] * (1 - averK);
        colorMusic_f[i] = colorMusic[i] * SMOOTH_FREQ + colorMusic_f[i] * (1 - SMOOTH_FREQ);
        if (colorMusic_f[i] > ((float)colorMusic_aver[i] * MAX_COEF_FREQ)) {
          thisBright[i] = 255;
          colorMusicFlash[i] = true;
          running_flag[i] = true;
        } else {
          colorMusicFlash[i] = false;
        }
        thisBright[i] = max(0, thisBright[i] - SMOOTH_STEP);
        if (thisBright[i] < EMPTY_BRIGHT) {
          thisBright[i] = EMPTY_BRIGHT;
          running_flag[i] = false;
        }
      }
      animation();
      break;
    }

    case 5: { // strobe
      if ((long)millis() - strobe_timer > STROBE_PERIOD) {
        strobe_timer = millis();
        strobeUp_flag = true;
        strobeDwn_flag = false;
      }
      if ((long)millis() - strobe_timer > light_time) strobeDwn_flag = true;
      if (strobeUp_flag) {
        strobe_bright = min(255, strobe_bright + STROBE_SMOOTH);
        if (strobe_bright >= 255) strobeUp_flag = false;
      }
      if (strobeDwn_flag) {
        strobe_bright = max(0, strobe_bright - STROBE_SMOOTH);
        if (strobe_bright <= 0) strobeDwn_flag = false;
      }
      animation();
      break;
    }

    case 6: // static / lighting modes
    case 7: // running frequency
    case 8: // spectrum analyzer
      // these modes rely on freq_f[] which is simulated from bands[]
      // keep a small decay on freq_f to emulate smoothing
      for (int i = 0; i < 30; i++) {
        if (freq_f[i] > 0) freq_f[i] -= LIGHT_SMOOTH;
        if (freq_f[i] < 0) freq_f[i] = 0;
      }
      animation();
      break;
  }

  FastLED.show();
  // clear for next draw except for mode 7 (running) as AlexGyver did - but we will clear always to be safe
  FastLED.clear();
}

void animation() {
  switch (this_mode) {
    case 0:
      for (int i = 0; i < Rlenght; i++)
        leds[MAX_CH - i - 1] = ColorFromPalette(myPal, (uint8_t)(count * indexPal));
      for (int i = 0; i < Llenght; i++)
        leds[MAX_CH + i] = ColorFromPalette(myPal, (uint8_t)(count * indexPal));
      break;

    case 1:
      if (millis() - rainbow_timer > 30) {
        rainbow_timer = millis();
        hue = (hue + RAINBOW_STEP) & 255;
      }
      for (int i = 0; i < Rlenght; i++)
        leds[MAX_CH - i - 1] = ColorFromPalette(RainbowColors_p, (uint8_t)((i * indexPal) / 2) - hue);
      for (int i = 0; i < Llenght; i++)
        leds[MAX_CH + i] = ColorFromPalette(RainbowColors_p, (uint8_t)((i * indexPal) / 2) - hue);
      break;

    case 2: // 5 stripes
      {
        int STRIPE = NUM_LEDS / 5;
        for (int i = 0; i < NUM_LEDS; i++) {
          if (i < STRIPE) leds[i] = CHSV(HIGH_COLOR, 255, thisBright[2]);
          else if (i < STRIPE * 2) leds[i] = CHSV(MID_COLOR, 255, thisBright[1]);
          else if (i < STRIPE * 3) leds[i] = CHSV(LOW_COLOR, 255, thisBright[0]);
          else if (i < STRIPE * 4) leds[i] = CHSV(MID_COLOR, 255, thisBright[1]);
          else leds[i] = CHSV(HIGH_COLOR, 255, thisBright[2]);
        }
      }
      break;

    case 3: // 3 stripes
      for (int i = 0; i < NUM_LEDS; i++) {
        if (i < NUM_LEDS / 3) leds[i] = CHSV(HIGH_COLOR, 255, thisBright[2]);
        else if (i < NUM_LEDS * 2 / 3) leds[i] = CHSV(MID_COLOR, 255, thisBright[1]);
        else leds[i] = CHSV(LOW_COLOR, 255, thisBright[0]);
      }
      break;

    case 4: // 1 stripe flash
      if (colorMusicFlash[2]) fill_solid(leds, NUM_LEDS, CHSV(HIGH_COLOR, 255, thisBright[2]));
      else if (colorMusicFlash[1]) fill_solid(leds, NUM_LEDS, CHSV(MID_COLOR, 255, thisBright[1]));
      else if (colorMusicFlash[0]) fill_solid(leds, NUM_LEDS, CHSV(LOW_COLOR, 255, thisBright[0]));
      else fill_solid(leds, NUM_LEDS, CHSV(EMPTY_COLOR, 255, EMPTY_BRIGHT));
      break;

    case 5: // strobe
      if (strobe_bright > 0)
        fill_solid(leds, NUM_LEDS, CHSV(STROBE_COLOR, STROBE_SAT, strobe_bright));
      else
        fill_solid(leds, NUM_LEDS, CHSV(EMPTY_COLOR, 255, EMPTY_BRIGHT));
      break;

    case 6: // Lighting modes (simple implementation)
      {
        static int this_color = 0;
        static unsigned long last_color_timer = 0;
        byte light_mode = 2; // keep rainbow variant by default
        switch (light_mode) {
          case 0:
            fill_solid(leds, NUM_LEDS, CHSV(0, 255, 255));
            break;
          case 1:
            if (millis() - last_color_timer > 100) {
              last_color_timer = millis();
              if (++this_color > 255) this_color = 0;
            }
            for (int i = 0; i < NUM_LEDS; i++) leds[i] = CHSV(this_color, 255, 255);
            break;
          case 2:
          default:
            if (millis() - rainbow_timer > 30) {
              rainbow_timer = millis();
              this_color += RAINBOW_PERIOD;
              if (this_color > 255) this_color = 0;
            }
            {
              float rainbow_steps = this_color;
              for (int i = 0; i < NUM_LEDS; i++) {
                leds[i] = CHSV((int)floor(rainbow_steps), 255, 255);
                rainbow_steps += RAINBOW_STEP_2;
                if (rainbow_steps > 255) rainbow_steps = 0;
              }
            }
            break;
        }
      }
      break;

    case 7: // running frequencies (simulate using running_flag)
      {
        // center LED reacts
        CRGB centerCol = CHSV(EMPTY_COLOR, 255, EMPTY_BRIGHT);
        if (running_flag[2]) centerCol = CHSV(HIGH_COLOR, 255, thisBright[2]);
        else if (running_flag[1]) centerCol = CHSV(MID_COLOR, 255, thisBright[1]);
        else if (running_flag[0]) centerCol = CHSV(LOW_COLOR, 255, thisBright[0]);

        leds[NUM_LEDS / 2] = centerCol;
        leds[(NUM_LEDS / 2) - 1] = centerCol;

        if (millis() - running_timer > RUNNING_SPEED) {
          running_timer = millis();
          // shift outwards
          for (int i = 0; i < NUM_LEDS / 2 - 1; i++) {
            leds[i] = leds[i + 1];
            leds[NUM_LEDS - i - 1] = leds[i];
          }
        }
      }
      break;

    case 8: // spectrum analyzer (approx using freq_f)
      {
        byte HUEindex = HUE_START;
        float freq_to_stripe = (float)(NUM_LEDS / 2) / 20.0; // emulate
        for (int i = 0; i < NUM_LEDS / 2; i++) {
          int idx = (int)floor((NUM_LEDS / 2 - i) / freq_to_stripe);
          if (idx < 0) idx = 0;
          if (idx > 29) idx = 29;
          byte this_bright = map(freq_f[idx], 0, max(1, freq_max), 0, 255);
          this_bright = constrain(this_bright, 0, 255);
          leds[i] = CHSV(HUEindex, 255, this_bright);
          leds[NUM_LEDS - i - 1] = leds[i];
          HUEindex += HUE_STEP;
          if (HUEindex > 255) HUEindex = 0;
        }
      }
      break;
  }
}

// --------------------- Button handling & EEPROM ---------------------
void buttonTick() {
  // use GyverButton: already butt1.tick() called in loop(); here check states
  if (butt1.isSingle()) {
    if (++this_mode >= MODE_AMOUNT) this_mode = 0;
    eeprom_flag = true;
    eeprom_timer = millis();
    updateEEPROM();
  }
  if (butt1.isHolded()) {
    // on hold: reduce to auto low pass calibration in original code.
    // We don't have mic, so we simulate a reset of averageLevel
    averageLevel = 50;
    // store to EEPROM maybe
    eeprom_flag = true;
    eeprom_timer = millis();
  }
}

void updateEEPROM() {
  EEPROM.updateByte(1, this_mode);
  EEPROM.updateByte(2, BRIGHTNESS);
  EEPROM.updateInt(4, EMPTY_BRIGHT);
  EEPROM.updateInt(8, (int)SMOOTH);
  // mark settings saved time
  eeprom_flag = false;
  eeprom_timer = millis();
}

void readEEPROM() {
  this_mode = EEPROM.readByte(1);
  BRIGHTNESS = EEPROM.readByte(2);
  int eb = EEPROM.readInt(4);
  if (eb >= 0 && eb <= 255) EMPTY_BRIGHT = eb;
  float s = EEPROM.readFloat(8);
  if (s > 0 && s <= 1.0) SMOOTH = s;
  FastLED.setBrightness(BRIGHTNESS);
}

void eepromTick() {
  if (eeprom_flag) {
    if (millis() - eeprom_timer > 30000) {
      eeprom_flag = false;
      eeprom_timer = millis();
      updateEEPROM();
    }
  }
}

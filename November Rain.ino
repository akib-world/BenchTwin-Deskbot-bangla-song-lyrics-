// Lyrics animation - BenchTwin N16R8 + ILI9341 (320x240, rotation 1)
// Pink/purple gradient background (sampled from the video), white lyrics with soft shadow.
// Cue timings, effects, button sync, serial sync and repeat logic are the same as the original code.
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <math.h>

#define CS 1
#define DC 2
#define RST 6
#define SYNC_PIN 7

Adafruit_ILI9341 tft(CS, DC, RST);

const int SW = 320;
const int SH = 240;
GFXcanvas16 canvas(SW, SH);   // off-screen buffer -> no flicker (lives in PSRAM on N16R8)

// ---------------- Background gradient (from video) ----------------
const uint8_t TOP_R = 180, TOP_G = 98, TOP_B = 215;   // purple (top)
const uint8_t BOT_R = 237, BOT_G = 95, BOT_B = 155;   // pink (bottom)

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 248) << 8) | ((g & 252) << 3) | (b >> 3);
}

void gradAt(int y, uint8_t &r, uint8_t &g, uint8_t &b) {
  if (y < 0) y = 0;
  if (y > SH - 1) y = SH - 1;
  float t = (float)y / (SH - 1);
  r = TOP_R + (BOT_R - TOP_R) * t;
  g = TOP_G + (BOT_G - TOP_G) * t;
  b = TOP_B + (BOT_B - TOP_B) * t;
}

uint16_t gradRow[SH];

void buildGradient() {
  for (int y = 0; y < SH; y++) {
    uint8_t r, g, b;
    gradAt(y, r, g, b);
    gradRow[y] = rgb(r, g, b);
  }
}

// ---------------- Lyrics (from the video) ----------------
const int MAX_LINES = 5;
struct Lyric {
  uint8_t n;
  const char* line[MAX_LINES];
};

const Lyric lyrics[6] = {
  {4, {"WHEN I LOOK", "INTO YOUR EYES", "I CAN SEE A", "LOVE RESTRAINED", ""}},
  {5, {"BUT, DARLIN',", "WHEN I HOLD YOU", "DON'T YOU KNOW", "I FEEL THE SAME?", "YEAH"}},
  {3, {"NOTHIN' LASTS FOREVER", "AND WE BOTH KNOW", "HEARTS CAN CHANGE", "", ""}},
  {4, {"AND IT'S HARD TO", "HOLD A CANDLE", "IN THE COLD", "NOVEMBER RAIN", ""}},
  {5, {"WE'VE BEEN THROUGH", "THIS SUCH A LONG,", "LONG TIME, JUST", "TRYNA KILL THE PAIN,", "OOH YEAH"}},
  {3, {"BUT LOVERS ALWAYS", "COME AND LOVERS", "ALWAYS GO", "", ""}}
};

// ---------------- Cues (timing unchanged) ----------------
struct Cue {
  uint32_t start;
  uint32_t end;
  uint8_t lyric;
  uint8_t effect;
};

// Effects: 0 = Fade, 1 = Letter spread, 2 = Curved entry, 3 = Wobble
const Cue cues[] = {
  {1000, 4400, 0, 0},
  {4700, 8700, 1, 1},
  {9000, 12400, 2, 2},
  {12700, 16400, 3, 0},

  {16800, 20200, 0, 1},
  {20500, 24100, 1, 0},
  {24400, 27800, 2, 2},
  {28100, 31600, 3, 0},

  {31700, 38500, 4, 1},
  {39200, 42900, 5, 0}
};
const int CUES = 10;

const uint32_t CLIP_END = 45558;
const bool REPEAT = true;
const uint32_t FRAME_INTERVAL = 40;

uint32_t origin = 0;
uint32_t lastFrame = 0;
uint32_t lastButton = 0;
bool priorButton = HIGH;

// ---------------- Helpers ----------------
float clamped(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
float ease(float v) { v = clamped(v); return v * v * (3 - 2 * v); }

int advanceOf(const GFXfont* f, char c) {
  if (c < f->first || c > f->last) return 0;
  return f->glyph[c - f->first].xAdvance;
}

int measure(const GFXfont* f, const char* s) {
  int w = 0;
  for (int i = 0; s[i]; i++) w += advanceOf(f, s[i]);
  return w;
}

// pick the biggest font where every line fits
const GFXfont* pickFont(const Lyric& L) {
  const GFXfont* opts[2] = {&FreeSansBold12pt7b, &FreeSansBold9pt7b};
  for (int k = 0; k < 2; k++) {
    bool ok = true;
    for (int i = 0; i < L.n; i++)
      if (measure(opts[k], L.line[i]) > SW - 24) ok = false;
    if (ok) return opts[k];
  }
  return opts[1];
}

uint16_t mixToWhite(int y, float alpha, bool shadow) {
  uint8_t r, g, b;
  gradAt(y, r, g, b);
  float tr = 255, tg = 255, tb = 255;
  if (shadow) { tr = r * 0.45; tg = g * 0.45; tb = b * 0.45; }
  return rgb(r + (tr - r) * alpha, g + (tg - g) * alpha, b + (tb - b) * alpha);
}

void drawLine(const GFXfont* f, const char* s, int baseY, uint8_t effect,
              float inP, float alpha, uint32_t at) {
  int tw = measure(f, s);
  int x0 = (SW - tw) / 2;
  int cx = SW / 2;
  int cum = 0;

  uint16_t colMain = mixToWhite(baseY - 8, alpha, false);
  uint16_t colShad = mixToWhite(baseY - 8, alpha, true);

  canvas.setFont(f);
  canvas.setTextSize(1);

  for (int i = 0; s[i]; i++) {
    int adv = advanceOf(f, s[i]);
    float xf = x0 + cum;
    float x = xf, y = baseY;

    if (effect == 1) {                       // letter spread
      x = cx + (xf - cx) * inP;
    } else if (effect == 2) {                // curved entry
      float k = 1.0f - inP;
      x = xf - k * 120.0f;
      y = baseY - k * k * 34.0f;
    } else if (effect == 3) {                // wobble
      float amp = 4.0f * (1.0f - inP) + 1.5f;
      y = baseY + sinf(at * 0.012f + i * 0.6f) * amp;
    }

    if (s[i] != ' ') {
      canvas.drawChar((int)x + 1, (int)y + 2, s[i], colShad, colShad, 1);
      canvas.drawChar((int)x, (int)y, s[i], colMain, colMain, 1);
    }
    cum += adv;
  }
}

void drawCue(const Cue& c, uint32_t at) {
  float in = ease((at - c.start) / 560.0f);
  float out = ease((c.end - at) / 550.0f);
  float alpha = in < out ? in : out;

  const Lyric& L = lyrics[c.lyric];
  const GFXfont* f = pickFont(L);
  int lh = f->yAdvance + 4;
  int blockH = L.n * lh;
  int top = (SH - blockH) / 2 - 8;

  for (int i = 0; i < L.n; i++) {
    float inL = in;
    if (c.effect == 2) inL = ease((at - c.start - i * 90) / 560.0f);   // staggered curved entry
    int baseY = top + lh * (i + 1) - 6;
    drawLine(f, L.line[i], baseY, c.effect, inL, alpha, at);
  }
}

void render(uint32_t at) {
  for (int y = 0; y < SH; y++) canvas.drawFastHLine(0, y, SW, gradRow[y]);

  for (int i = 0; i < CUES; i++) {
    if (at >= cues[i].start && at < cues[i].end) {
      drawCue(cues[i], at);
      break;
    }
  }
  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), SW, SH);
}

void setup() {
  Serial.begin(115200);
  pinMode(SYNC_PIN, INPUT_PULLUP);
  SPI.begin(12, 13, 11, CS);
  tft.begin(40000000);
  tft.setRotation(1);
  buildGradient();
  if (canvas.getBuffer() == nullptr) {
    Serial.println("Canvas alloc failed - enable PSRAM (OPI) in Arduino IDE!");
  }
  tft.fillScreen(ILI9341_BLACK);
  origin = millis();
  Serial.println("Lyrics animation ready - N16R8 / ILI9341");
}

void loop() {
  uint32_t now = millis();
  bool button = digitalRead(SYNC_PIN);

  if (priorButton == HIGH && button == LOW && (now - lastButton) > 200) {
    origin = now;
    lastButton = now;
    Serial.println("Animation restarted");
  }
  priorButton = button;

  while (Serial.available()) {
    if (Serial.read() == 's') {
      origin = now;
      Serial.println("Serial synchronization");
    }
  }

  uint32_t at = now - origin;
  if (at >= CLIP_END && REPEAT) {
    origin = now;
    at = 0;
  }

  if (now - lastFrame >= FRAME_INTERVAL) {
    lastFrame = now;
    render(at);
  }
  delay(2);
}

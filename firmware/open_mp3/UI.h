/*
  Screens and buttons for the round 240x240 GC9A01 display (LovyanGFX).
  Runs as its own task on core 0, so drawing never holds up the player on core 1.

  Wiring (display -> ESP32-S3):  SCL->GPIO17  SDA->GPIO18  DC->GPIO39  CS->GPIO21  RST->GPIO40
                                 VCC->3V3  GND->GND   (SDA/SCL are SPI, not I2C; no backlight pin)

  Screens: album browser (covers), track list, and now playing, which is either the album cover
  with a progress ring, or the countdown dial. BOOT switches between those two.
*/
#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class LGFX_GC9A01 : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01 panel;
  lgfx::Bus_SPI bus;

public:
  LGFX_GC9A01() {
    auto b = bus.config();
    b.spi_host    = SPI3_HOST;   // its own bus, separate from the SD card's
    b.spi_mode    = 0;
    b.freq_write  = 40000000;
    b.pin_sclk    = 17;
    b.pin_mosi    = 18;
    b.pin_miso    = -1;
    b.pin_dc      = 39;
    b.dma_channel = SPI_DMA_CH_AUTO;
    bus.config(b);
    panel.setBus(&bus);

    auto p = panel.config();
    p.pin_cs       = 21;
    p.pin_rst      = 40;
    p.pin_busy     = -1;
    p.panel_width  = 240;
    p.panel_height = 240;
    p.invert       = true;
    p.readable     = false;
    p.bus_shared   = false;
    panel.config(p);
    setPanel(&panel);
  }
};

// A button from a GPIO to GND. poll() reports a short press when it's let go,
// or HOLD after 1 s (and again every 200 ms while still held, if repeat is set).
enum Press { NONE, SHORT, HOLD };

struct Button {
  int pin;
  bool repeat;
  unsigned long pressedAt = 0, nextRepeat = 0;
  bool held = false;

  Button(int pin, bool repeat) : pin(pin), repeat(repeat) {}
  void begin() { pinMode(pin, INPUT_PULLUP); }

  Press poll() {
    bool down = digitalRead(pin) == LOW;
    unsigned long now = millis();
    if (down && pressedAt == 0) {
      pressedAt = now;
    } else if (down && now - pressedAt >= 1000 && (!held || (repeat && (long)(now - nextRepeat) >= 0))) {
      held = true;
      nextRepeat = now + 200;
      return HOLD;
    } else if (!down && pressedAt != 0) {
      bool shortPress = !held && now - pressedAt >= 30;   // 30 ms debounce
      pressedAt = 0;
      held = false;
      if (shortPress) return SHORT;
    }
    return NONE;
  }
};

namespace ui {

const int FPS = 25;
const int CX = 120, CY = 120;
const uint32_t IDLE_BACK_MS = 15000;   // browsing with no presses: back to now playing after this

LGFX_GC9A01 lcd;
LGFX_Sprite frame(&lcd);

Button btnBoot(0, false), btnPlay(47, false), btnNext(1, true), btnPrev(42, true);

// Palette: near-black olive, muted khaki, amber
const uint32_t BG         = 0x0D0F0A;
const uint32_t GHOST      = 0x1A1D14;   // unlit digit segments
const uint32_t TRACK      = 0x23271B;   // the used-up part of a ring, highlight bars
const uint32_t TICK_DIM   = 0x3A3B2B;
const uint32_t TICK       = 0x77735A;
const uint32_t TICK_MAJOR = 0x9C9673;
const uint32_t AMBER      = 0xFFA51F;
const uint32_t AMBER_RED  = 0xFF6A1C;   // last 10 seconds

enum Screen { MESSAGE, ALBUMS, TRACKS, NOW };
volatile Screen screen = MESSAGE;
const char *volatile message = "LOADING";
volatile bool ready = false;
bool dialView = false;          // now playing: false = cover view, true = countdown dial

int selAlbum = 0, selTrack = 0;
uint32_t lastPressMs = 0, marqueeStartMs = 0, volUntilMs = 0;
bool wasActive = false;

// ---------- drawing helpers ----------

uint32_t mix(uint32_t a, uint32_t b, float t) {
  auto ch = [&](int s) { return (uint32_t)(((a >> s) & 0xFF) * (1 - t) + ((b >> s) & 0xFF) * t) << s; };
  return ch(16) | ch(8) | ch(0);
}

// Point at radius r and angle deg (0 = 12 o'clock, clockwise)
void polar(float r, float deg, int &x, int &y) {
  float a = (deg - 90) * DEG_TO_RAD;
  x = CX + lroundf(r * cosf(a));
  y = CY + lroundf(r * sinf(a));
}

// Usable text width at height y, keeping clear of the round edge
int widthAt(int y) {
  int dy = abs(y - CY);
  return dy >= 104 ? 0 : 2 * (int)sqrtf(104 * 104 - dy * dy) - 12;
}

// Draws s centred at (cx, y) within maxW: cut short with "..." or, if scroll is set, scrolling sideways
void drawFitted(const String &s, int cx, int y, int maxW, bool scroll) {
  int w = frame.textWidth(s);
  if (w <= maxW) {
    frame.setTextDatum(textdatum_t::middle_center);
    frame.drawString(s, cx, y);
    return;
  }
  if (!scroll) {
    String t = s;
    while (t.length() > 1 && frame.textWidth(t + "...") > maxW) t.remove(t.length() - 1);
    t.trim();
    frame.setTextDatum(textdatum_t::middle_center);
    frame.drawString(t + "...", cx, y);
    return;
  }
  // Wait 1.5 s, then scroll at 35 px/s, around and around
  const int gap = 40, period = w + gap;
  uint32_t t = millis() - marqueeStartMs;
  int off = t < 1500 ? 0 : ((t - 1500) * 35 / 1000) % period;
  int x0 = cx - maxW / 2;
  int h = frame.fontHeight();
  frame.setClipRect(x0, y - h, maxW, h * 2);
  frame.setTextDatum(textdatum_t::middle_left);
  frame.drawString(s, x0 - off, y);
  frame.drawString(s, x0 - off + period, y);
  frame.clearClipRect();
}

void drawCover(const uint16_t *img, int size, int x, int y) {
  if (img) {
    frame.pushImage(x, y, size, size, (const lgfx::swap565_t *)img);
  } else {
    frame.fillRect(x, y, size, size, TRACK);
    frame.drawRect(x, y, size, size, TICK_DIM);
  }
}

// Volume as a row of small bars, lit up to the current level
void drawVolume(int y, uint32_t accent) {
  const int w = 5, gap = 2, h = 10;
  int x = CX - (VOLUME_LEVELS * (w + gap) - gap) / 2;
  for (int i = 0; i < VOLUME_LEVELS; i++, x += w + gap)
    frame.fillRect(x, y - h / 2, w, h, i < volumeLevel ? accent : TRACK);
}

bool showingVolume() { return (int32_t)(millis() - volUntilMs) < 0; }

String mmss(uint32_t s) {
  char t[12];
  snprintf(t, sizeof(t), "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  return t;
}

// ---------- screens ----------

void drawMessage() {
  frame.setFont(&fonts::Font2);
  frame.setTextColor(TICK_MAJOR);
  frame.setTextDatum(textdatum_t::middle_center);
  frame.drawString(message, CX, CY);
}

void drawAlbums() {
  const Album &a = albums[selAlbum];
  frame.setFont(&fonts::Font2);
  frame.setTextColor(TICK);
  frame.setTextDatum(textdatum_t::middle_center);
  frame.drawString(String(selAlbum + 1) + " / " + albums.size(), CX, 18);

  // Little arrows each side: the next/previous album buttons
  frame.fillTriangle(16, CY, 24, CY - 6, 24, CY + 6, TICK_DIM);
  frame.fillTriangle(224, CY, 216, CY - 6, 216, CY + 6, TICK_DIM);

  int x = CX - COVER_BIG / 2, y = 30;
  drawCover(a.coverBig, COVER_BIG, x, y);

  PlayerState s = getState();
  if (s.active && s.album == selAlbum) {   // this album is playing: a small amber marker
    frame.fillRect(x, y + COVER_BIG + 3, COVER_BIG, 2, AMBER);
  }

  frame.setFont(&fonts::FreeSansBold9pt7b);
  frame.setTextColor(AMBER);
  drawFitted(a.title, CX, 186, widthAt(186), true);
  frame.setFont(&fonts::Font2);
  frame.setTextColor(TICK_MAJOR);
  drawFitted(a.artist + "  " + a.year, CX, 207, widthAt(212), false);
}

void drawTracks() {
  const Album &a = albums[selAlbum];
  PlayerState s = getState();

  frame.setFont(&fonts::Font2);
  frame.setTextColor(TICK);
  drawFitted(a.title, CX, 26, widthAt(20), false);
  frame.setTextColor(TICK_DIM);
  frame.setTextDatum(textdatum_t::middle_center);
  frame.drawString(String(selTrack + 1) + " / " + a.tracks.size(), CX, 214);

  const int ROW = 30;
  for (int d = -2; d <= 2; d++) {
    int t = selTrack + d;
    if (t < 0 || t >= (int)a.tracks.size()) continue;
    int y = CY + d * ROW;
    int w = widthAt(y) - 8;
    bool sel = d == 0;
    bool playingThis = s.active && s.album == selAlbum && s.track == t;

    if (sel) frame.fillRoundRect(CX - w / 2 - 6, y - 13, w + 12, 26, 6, TRACK);

    // Track number on the left, title after it
    char num[4];
    snprintf(num, sizeof(num), "%02d", t + 1);
    frame.setFont(&fonts::Font2);
    frame.setTextColor(sel ? TICK_MAJOR : TICK_DIM);
    frame.setTextDatum(textdatum_t::middle_left);
    int x0 = CX - w / 2;
    frame.drawString(num, x0, y);
    if (playingThis) frame.fillTriangle(x0 + 20, y - 5, x0 + 20, y + 5, x0 + 27, y, AMBER);

    int tx = x0 + 32, tw = w - 32;
    frame.setFont(sel ? &fonts::FreeSansBold9pt7b : &fonts::FreeSans9pt7b);
    frame.setTextColor(sel ? AMBER : (abs(d) == 1 ? TICK_MAJOR : TICK));
    // Left-aligned in its column: draw centred in the column's middle
    if (frame.textWidth(a.tracks[t].title) <= tw) {
      frame.setTextDatum(textdatum_t::middle_left);
      frame.drawString(a.tracks[t].title, tx, y);
    } else {
      drawFitted(a.tracks[t].title, tx + tw / 2, y, tw, sel);
    }
  }
}

// Now playing, cover view: cover, title, time, and a progress ring round the edge
void drawNowCover(const PlayerState &s, float played) {
  const Album &a = albums[s.album];
  float frac = s.totalFrames ? constrain(played / s.totalFrames, 0.0f, 1.0f) : 0;

  frame.fillArc(CX, CY, 113, 116, 0, 360, TRACK);
  if (frac > 0) frame.fillArc(CX, CY, 113, 116, -90, -90 + 360 * frac, AMBER);

  frame.setFont(&fonts::Font2);
  frame.setTextColor(TICK);
  frame.setTextDatum(textdatum_t::middle_center);
  frame.drawString(String(s.track + 1) + " / " + a.tracks.size(), CX, 30);

  int x = CX - COVER_SMALL / 2, y = 44;
  drawCover(a.coverSmall, COVER_SMALL, x, y);
  if (s.paused) {   // pause sign over the cover
    frame.fillRoundRect(CX - 20, y + COVER_SMALL / 2 - 20, 40, 40, 6, BG);
    frame.fillRect(CX - 10, y + COVER_SMALL / 2 - 11, 7, 22, AMBER);
    frame.fillRect(CX + 3, y + COVER_SMALL / 2 - 11, 7, 22, AMBER);
  }

  frame.setFont(&fonts::FreeSansBold9pt7b);
  frame.setTextColor(AMBER);
  drawFitted(a.tracks[s.track].title, CX, 174, widthAt(174), true);

  if (showingVolume()) {
    drawVolume(198, AMBER);
  } else if (!(s.paused && (millis() / 500) % 2)) {   // paused: the time blinks
    frame.setFont(&fonts::Font2);
    frame.setTextColor(TICK_MAJOR);
    frame.setTextDatum(textdatum_t::middle_center);
    uint32_t sec = s.rate ? (uint32_t)(played / s.rate) : 0, total = s.rate ? s.totalFrames / s.rate : 0;
    frame.drawString(mmss(sec) + " / " + mmss(total), CX, 198);
  }
}

// Now playing, countdown dial: a gauge sweeping round as the track plays, time left in the middle
void drawNowDial(const PlayerState &s, float played) {
  bool known = s.totalFrames > 0 && s.rate > 0;
  float left = known ? constrain(1.0f - played / s.totalFrames, 0.0f, 1.0f) : 0;
  float secondsLeft = known ? max(0.0f, (s.totalFrames - played) / s.rate) : 0;
  float needleDeg = 360.0f * (1.0f - left);   // sweeps clockwise from 12 o'clock as the track plays

  uint32_t accent = AMBER;
  if (known && secondsLeft < 10) accent = mix(AMBER, AMBER_RED, 1.0f - secondsLeft / 10.0f);

  // Gauge ticks: 60 around the edge, every 5th longer. Ticks already passed are dimmer.
  for (int i = 0; i < 60; i++) {
    float deg = i * 6.0f;
    bool major = i % 5 == 0;
    bool passed = known && deg < needleDeg;
    int x0, y0, x1, y1;
    polar(major ? 104 : 109, deg, x0, y0);
    polar(116, deg, x1, y1);
    uint32_t c = passed ? TICK_DIM : (major ? TICK_MAJOR : TICK);
    if (major) frame.drawWideLine(x0, y0, x1, y1, 1.0f, c);
    else       frame.drawLine(x0, y0, x1, y1, c);
  }

  // Thin ring: the time left in amber, shrinking toward 12 o'clock
  frame.fillArc(CX, CY, 96, 98, 0, 360, TRACK);
  if (known && left > 0) {
    frame.fillArc(CX, CY, 96, 98, needleDeg - 90, 270, accent);
    int x0, y0, x1, y1;
    polar(66, needleDeg, x0, y0);
    polar(100, needleDeg, x1, y1);
    frame.drawWideLine(x0, y0, x1, y1, 0.9f, accent);
  }

  // Faint inner circle and crosshair notches, HUD style
  frame.drawCircle(CX, CY, 64, TRACK);
  for (int deg = 0; deg < 360; deg += 90) {
    int x0, y0, x1, y1;
    polar(58, deg, x0, y0);
    polar(64, deg, x1, y1);
    frame.drawLine(x0, y0, x1, y1, TICK_DIM);
  }

  // MM:SS in seven-segment digits, over dim "88:88" ghost segments like an old LCD
  char text[12];
  if (known) {
    int sec = (int)ceilf(secondsLeft);
    snprintf(text, sizeof(text), "%02d:%02d", min(sec / 60, 99), sec % 60);
  } else {
    strcpy(text, "--:--");
  }
  frame.setFont(&fonts::Font7);
  frame.setTextSize(0.85f);
  frame.setTextDatum(textdatum_t::middle_center);
  frame.setTextColor(GHOST);
  frame.drawString("88:88", CX, CY);
  if (!(s.paused && (millis() / 500) % 2)) {   // paused: the time blinks
    frame.setTextColor(known ? accent : TICK);
    frame.drawString(text, CX, CY);
  }
  frame.setTextSize(1);

  // Under the time: the volume bars just after a change, otherwise PAUSED when paused
  if (showingVolume()) {
    drawVolume(CY + 39, accent);
  } else if (s.paused) {
    frame.setFont(&fonts::Font2);
    frame.setTextColor(TICK_MAJOR);
    frame.drawString("PAUSED", CX, CY + 39);
  }
}

void draw() {
  frame.fillScreen(frame.color888(BG >> 16, (BG >> 8) & 0xFF, BG & 0xFF));
  frame.setTextSize(1);
  PlayerState s = getState();

  if (s.cardError && screen != MESSAGE) {
    frame.setFont(&fonts::Font2);
    frame.setTextColor(AMBER);
    frame.setTextDatum(textdatum_t::middle_center);
    frame.drawString("SD CARD ERROR", CX, CY - 10);
    frame.setTextColor(TICK);
    frame.drawString((millis() / 500) % 2 ? "reconnecting..." : "reconnecting", CX, CY + 12);
  } else if (screen == MESSAGE) {
    drawMessage();
  } else if (screen == ALBUMS) {
    drawAlbums();
  } else if (screen == TRACKS) {
    drawTracks();
  } else if (!s.active) {
    frame.setFont(&fonts::Font2);
    frame.setTextColor(TICK);
    frame.setTextDatum(textdatum_t::middle_center);
    frame.drawString("NOTHING PLAYING", CX, CY);
  } else {
    // Move on smoothly from the player's last update (not while paused). Capped so a stall can't run ahead.
    uint32_t since = s.paused ? 0 : min<uint32_t>(millis() - s.posAtMs, 200);
    float played = s.posFrames + since * (s.rate / 1000.0f);
    if (dialView) drawNowDial(s, played);
    else          drawNowCover(s, played);
  }
  frame.pushSprite(0, 0);
}

// ---------- buttons ----------

void goTo(Screen sc) {
  screen = sc;
  marqueeStartMs = millis();
}

void changeVolume(int delta) {
  volumeLevel = constrain(volumeLevel + delta, 1, VOLUME_LEVELS);
  volUntilMs = millis() + 1500;
  prefs.putUChar("volume", volumeLevel);
}

void handleButtons() {
  Press boot = btnBoot.poll(), play = btnPlay.poll(), nxt = btnNext.poll(), prv = btnPrev.poll();
  if (boot || play || nxt || prv) lastPressMs = millis();
  PlayerState s = getState();
  int nAlbums = albums.size();

  if (boot == SHORT) {
    if (screen == NOW) {
      dialView = !dialView;
      prefs.putBool("dial", dialView);
    } else if (s.active) {
      goTo(NOW);
    }
    return;
  }

  switch (screen) {
    case ALBUMS:
      if (nxt) { selAlbum = (selAlbum + 1) % nAlbums; marqueeStartMs = millis(); }
      if (prv) { selAlbum = (selAlbum + nAlbums - 1) % nAlbums; marqueeStartMs = millis(); }
      if (play == SHORT) {
        selTrack = (s.active && s.album == selAlbum) ? s.track : 0;
        goTo(TRACKS);
      }
      if (play == HOLD && s.active) goTo(NOW);
      break;

    case TRACKS: {
      int n = albums[selAlbum].tracks.size();
      if (nxt) { selTrack = (selTrack + 1) % n; marqueeStartMs = millis(); }
      if (prv) { selTrack = (selTrack + n - 1) % n; marqueeStartMs = millis(); }
      if (play == SHORT) {
        sendCmd(CMD_PLAY, selAlbum, selTrack);
        goTo(NOW);
      }
      if (play == HOLD) goTo(ALBUMS);
      break;
    }

    case NOW:
      if (play == SHORT && s.active) sendCmd(CMD_TOGGLE_PAUSE);
      if (play == HOLD) {
        if (s.active) { selAlbum = s.album; selTrack = s.track; }
        goTo(TRACKS);
      }
      if (nxt == SHORT) sendCmd(CMD_NEXT);
      if (prv == SHORT) sendCmd(CMD_PREV);
      if (nxt == HOLD) changeVolume(+1);
      if (prv == HOLD) changeVolume(-1);
      break;

    default:
      break;
  }
}

// ---------- remembering what's playing ----------

// Saves album, track, place and pause state, so a restart carries on from there. Only what changed
// is written, and the place at most every 5 s (flash wear is negligible at that rate).
void saveResume(const PlayerState &s) {
  static int lastAlbum = -2, lastTrack = -2;
  static int lastPaused = -1;
  static uint32_t posDueMs = 0;
  if (!s.active || s.cardError) return;
  bool changed = false;
  if (s.album != lastAlbum) { prefs.putString("rdir", albums[s.album].dir); lastAlbum = s.album; lastTrack = -2; changed = true; }
  if (s.track != lastTrack) { prefs.putUShort("rtrack", s.track); lastTrack = s.track; changed = true; }
  if ((int)s.paused != lastPaused) { prefs.putBool("rpaused", s.paused); lastPaused = s.paused; changed = true; }
  if (changed || (!s.paused && (int32_t)(millis() - posDueMs) >= 0)) {
    prefs.putUInt("rpos", s.posFrames);
    posDueMs = millis() + 5000;
  }
}

// ---------- task ----------

void task(void *) {
  TickType_t wake = xTaskGetTickCount();
  uint32_t frames = 0, worstUs = 0, reportAt = millis() + 10000;
  int lastTrack = -1, lastAlbum = -1;
  while (true) {
    if (uploading) screen = MESSAGE;   // music arriving over USB: show its progress
    else if (ready && screen == MESSAGE) goTo(getState().active ? NOW : ALBUMS);

    if (ready) {
      PlayerState s = getState();
      saveResume(s);
      if (s.track != lastTrack || s.album != lastAlbum) {   // new track: restart the title's scrolling
        lastTrack = s.track;
        lastAlbum = s.album;
        if (screen == NOW) marqueeStartMs = millis();
      }
      // Left in the albums or track list while music plays: drift back to now playing
      if (s.active && screen != NOW && millis() - lastPressMs > IDLE_BACK_MS) goTo(NOW);
    }

    uint32_t t0 = micros();
    draw();
    frames++;
    worstUs = max<uint32_t>(worstUs, micros() - t0);
    if ((int32_t)(millis() - reportAt) >= 0) {
      Serial.printf("display: %.1f fps, slowest frame %lu ms\n", frames / 10.0f, (unsigned long)(worstUs / 1000));
      frames = worstUs = 0;
      reportAt += 10000;
    }

    // Check the buttons every 5 ms until the next frame is due
    TickType_t next = wake + pdMS_TO_TICKS(1000 / FPS);
    do {
      if (ready && !uploading) handleButtons();
      vTaskDelay(pdMS_TO_TICKS(5));
    } while ((int32_t)(xTaskGetTickCount() - next) < 0);
    wake = next;
  }
}

void begin() {
  btnBoot.begin();
  btnPlay.begin();
  btnNext.begin();
  btnPrev.begin();
  lcd.init();
  lcd.setRotation(0);
  lcd.fillScreen(TFT_BLACK);
  frame.setColorDepth(16);
  frame.setPsram(true);   // 115 KB frame buffer, kept out of the RAM the audio uses
  frame.createSprite(240, 240);
  // Core 0, low priority: the player runs on core 1 and is never made to wait
  xTaskCreatePinnedToCore(task, "ui", 8192, nullptr, 1, nullptr, 0);
}

void setMessage(const char *text) { message = text; }
void setReady() { lastPressMs = millis(); ready = true; }

}  // namespace ui

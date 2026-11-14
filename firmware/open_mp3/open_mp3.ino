/*
  SD card album player for ESP32-S3 + PCM5102A DAC + round GC9A01 display (ESP32 Arduino core 2.x, LovyanGFX)

  Plays albums from the SD card's /music folder (made by prepare_sd.py): browse albums by cover,
  pick a track, and it plays on through the album and into the next one.
  Tracks are WAV files: 16-bit PCM, or IMA ADPCM (4x smaller). Music can be copied onto the card
  over the USB cable with upload_music.py (see "Copying music" below), no card reader needed.

  Buttons (each from the GPIO to GND, internal pull-ups):
                   Albums screen        Track list            Now playing
    GPIO47 press   open album           play track            pause / play
    GPIO47 hold    back to now playing  back to albums        track list
    GPIO1  press   next album           down                  next track       hold: volume up
    GPIO42 press   previous album       up                    previous track   hold: volume down
    BOOT   press   go to now playing    go to now playing     switch cover view / countdown dial
  Holding GPIO1/42 in the albums screen or track list scrolls quickly.
  The volume and the chosen now-playing view are remembered across power-offs.

  Wiring:
    PCM5102A:  VIN->3V3  GND->GND  BCK->GPIO9  LRCK->GPIO10  DIN->GPIO11  SCK->GND
               FLT->GND  DEMP->GND  XSMT->3V3  FMT->GND
    SD module: CS->GPIO12  SCK->GPIO13  MOSI->GPIO38  MISO->GPIO15  (own SPI bus), VCC, GND
    Display:   see UI.h
    Left alone: GPIO4 (battery ADC)
*/

#include <driver/i2s.h>
#include <driver/uart.h>
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>
#include <vector>
#include <algorithm>
#include "Types.h"

const int PIN_BCK = 9, PIN_LRCK = 10, PIN_DIN = 11;
const int SD_CS = 12, SD_SCK = 13, SD_MOSI = 38, SD_MISO = 15;

const int COVER_BIG = 136;     // album browser
const int COVER_SMALL = 112;   // now playing

// Volume: 12 steps of 2.5 dB. The top is 0.45 of full scale (see setupI2S).
const int VOLUME_LEVELS = 12;
const int VOLUME_DEFAULT = 9;
volatile int volumeLevel = VOLUME_DEFAULT;
float volumeGain(int level) { return 0.45f * powf(10.0f, -(VOLUME_LEVELS - level) * 2.5f / 20.0f); }

Preferences prefs;

// ---------- Library ----------

std::vector<Album> albums;

uint16_t *loadCover(const String &path, int size) {
  File f = SD.open(path);
  if (!f) return nullptr;
  size_t bytes = size * size * 2;
  uint16_t *buf = (uint16_t *)ps_malloc(bytes);
  if (buf && f.read((uint8_t *)buf, bytes) != bytes) { free(buf); buf = nullptr; }
  return buf;
}

int loadLibrary() {
  albums.clear();
  File root = SD.open("/music");
  if (!root || !root.isDirectory()) return 0;
  std::vector<String> dirs;
  for (File f = root.openNextFile(); f; f = root.openNextFile())
    if (f.isDirectory()) dirs.push_back(f.path());
  std::sort(dirs.begin(), dirs.end());

  for (auto &dir : dirs) {
    File info = SD.open(dir + "/album.txt");
    if (!info) continue;
    Album a;
    a.dir = dir;
    a.title = info.readStringUntil('\n'); a.title.trim();
    a.artist = info.readStringUntil('\n'); a.artist.trim();
    a.year = info.readStringUntil('\n'); a.year.trim();
    while (info.available()) {
      String line = info.readStringUntil('\n');
      line.trim();
      int bar = line.indexOf('|');
      if (bar > 0) a.tracks.push_back({ line.substring(0, bar), line.substring(bar + 1) });
    }
    if (a.tracks.empty()) continue;
    a.coverBig = loadCover(dir + "/cover136.rgb", COVER_BIG);
    a.coverSmall = loadCover(dir + "/cover112.rgb", COVER_SMALL);
    Serial.printf("  %s (%s, %s): %d tracks%s\n", a.title.c_str(), a.artist.c_str(), a.year.c_str(),
                  (int)a.tracks.size(), a.coverBig ? "" : ", no cover");
    albums.push_back(a);
  }
  return albums.size();
}

// ---------- Player (its own task on core 1) ----------

QueueHandle_t cmdQueue;

PlayerState state;
portMUX_TYPE stateLock = portMUX_INITIALIZER_UNLOCKED;

PlayerState getState() {
  portENTER_CRITICAL(&stateLock);
  PlayerState s = state;
  portEXIT_CRITICAL(&stateLock);
  return s;
}

void sendCmd(CmdType type, int album = 0, int track = 0) {
  Cmd c = { type, album, track };
  xQueueSend(cmdQueue, &c, 0);
}

const int DMA_BUFS = 8, DMA_LEN = 512;
const uint32_t BUFFERED = DMA_BUFS * DMA_LEN;   // frames waiting in the I2S buffers (about 93 ms)

void setupI2S(uint32_t rate) {
  i2s_config_t config = {};
  config.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  config.sample_rate          = rate;
  config.bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT;  // 32-bit slots -> bit clock = 64 x sample rate
  config.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
  // Left-justified, with the volume capped at 0.45: plays cleanly whether the DAC's FMT pin reads
  // low (I2S) or high (left-justified).
  config.communication_format = I2S_COMM_FORMAT_STAND_MSB;
  config.dma_buf_count        = DMA_BUFS;
  config.dma_buf_len          = DMA_LEN;
  config.tx_desc_auto_clear   = true;   // plays silence when nothing is written (paused / stopped)

  i2s_pin_config_t pins = {};
  pins.mck_io_num   = I2S_PIN_NO_CHANGE;  // no master clock (SCK is tied to GND). Unset, this would default to GPIO0!
  pins.bck_io_num   = PIN_BCK;
  pins.ws_io_num    = PIN_LRCK;
  pins.data_out_num = PIN_DIN;
  pins.data_in_num  = I2S_PIN_NO_CHANGE;

  i2s_driver_install(I2S_NUM_0, &config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_zero_dma_buffer(I2S_NUM_0);
}

// IMA ADPCM (Microsoft WAV layout), mono or stereo
const int8_t INDEX_TABLE[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
const int16_t STEP_TABLE[89] = {
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
  50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
  337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
  2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
  15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

// Decodes one block into interleaved samples. Returns the number of frames.
// Layout: a 4-byte header per channel, then 4 bytes (8 samples) of each channel in turn.
int decodeImaBlock(const uint8_t *b, int size, int ch, int16_t *out) {
  int pred[2], index[2];
  for (int c = 0; c < ch; c++) {
    pred[c] = (int16_t)(b[4 * c] | (b[4 * c + 1] << 8));
    index[c] = constrain(b[4 * c + 2], 0, 88);
    out[c] = pred[c];
  }
  const uint8_t *p = b + 4 * ch;
  int groups = (size - 4 * ch) / (4 * ch);
  int frame = 1;
  for (int g = 0; g < groups; g++, frame += 8) {
    for (int c = 0; c < ch; c++) {
      for (int k = 0; k < 8; k++) {   // low nibble first, then high
        int nib  = (p[k / 2] >> ((k & 1) * 4)) & 0x0F;
        int step = STEP_TABLE[index[c]];
        int diff = ((2 * (nib & 7) + 1) * step) >> 3;   // same rounding as ffmpeg's encoder
        pred[c]  = constrain((nib & 8) ? pred[c] - diff : pred[c] + diff, -32768, 32767);
        index[c] = constrain(index[c] + INDEX_TABLE[nib], 0, 88);
        out[(frame + k) * ch + c] = pred[c];
      }
      p += 4;
    }
  }
  return frame;
}

const int MAX_BLOCK = 2048;      // largest ADPCM block in bytes (prepare_sd.py uses 2048)
const int MAX_FRAMES = 4096;     // frames per chunk (a 2048-byte ADPCM block holds up to 4089 mono frames)

void playerTask(void *) {
  static uint8_t raw[MAX_BLOCK];
  static int16_t pcm[MAX_FRAMES * 2];
  static int32_t out[MAX_FRAMES * 2];
  const uint32_t PCM_CHUNK = 1024;   // frames read at a time from a PCM file

  File f;
  int album = -1, track = -1, channels = 2;
  bool adpcm = false, paused = false;
  uint32_t blockAlign = 0, framesPerBlock = 1;
  uint32_t dataStart = 0, dataSize = 0, totalFrames = 0, pos = 0, rate = 0, clockRate = 44100;
  uint32_t sinceStart = 0;            // frames written since the last start/resume, to know how full the buffers are
  uint32_t worstUs = 0, reportAt = millis() + 10000;

  auto publish = [&]() {
    uint32_t heard = pos - min(sinceStart, min(BUFFERED, pos));
    portENTER_CRITICAL(&stateLock);
    state.active = album >= 0;
    state.paused = paused;
    state.album = album;
    state.track = track;
    state.posFrames = heard;
    state.totalFrames = totalFrames;
    state.rate = rate;
    state.posAtMs = millis();
    portEXIT_CRITICAL(&stateLock);
  };

  // Goes to frame `to` (rounded down to a block for ADPCM)
  auto seekTo = [&](uint32_t to) {
    if (adpcm) {
      uint32_t block = to / framesPerBlock;
      pos = block * framesPerBlock;
      f.seek(dataStart + block * blockAlign);
    } else {
      pos = to;
      f.seek(dataStart + pos * channels * 2);
    }
  };

  // Opens albums[a].tracks[t]. Returns false if the file can't be played.
  auto open = [&](int a, int t) -> bool {
    if (f) f.close();
    album = a; track = t; pos = 0; sinceStart = 0; totalFrames = 0;
    String path = albums[a].dir + "/" + albums[a].tracks[t].file;
    f = SD.open(path);
    if (!f) { Serial.printf("can't open %s\n", path.c_str()); return false; }
    uint8_t h[12];
    if (f.read(h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { Serial.printf("not a WAV: %s\n", path.c_str()); return false; }
    int format = 0, bits = 0;
    while (true) {   // walk the chunks to "fmt " and "data"
      uint8_t ch[8];
      if (f.read(ch, 8) != 8) { Serial.printf("no data in %s\n", path.c_str()); return false; }
      uint32_t size = ch[4] | ch[5] << 8 | ch[6] << 16 | (uint32_t)ch[7] << 24;
      if (!memcmp(ch, "fmt ", 4)) {
        uint8_t fmt[16];
        f.read(fmt, 16);
        format = fmt[0] | fmt[1] << 8;
        channels = fmt[2];
        rate = fmt[4] | fmt[5] << 8 | fmt[6] << 16;
        blockAlign = fmt[12] | fmt[13] << 8;
        bits = fmt[14];
        f.seek(f.position() + size - 16 + (size & 1));
      } else if (!memcmp(ch, "data", 4)) {
        dataStart = f.position();
        dataSize = size;
        break;
      } else {
        f.seek(f.position() + size + (size & 1));
      }
    }
    adpcm = format == 0x11;
    bool ok = channels >= 1 && channels <= 2 &&
              ((format == 1 && bits == 16) ||
               (adpcm && bits == 4 && blockAlign > 4u * channels && blockAlign <= (uint32_t)MAX_BLOCK));
    if (!ok) { Serial.printf("%s: needs 16-bit PCM or IMA ADPCM, mono or stereo\n", path.c_str()); return false; }
    if (adpcm) {
      framesPerBlock = (blockAlign - 4 * channels) * 2 / channels + 1;
      totalFrames = dataSize / blockAlign * framesPerBlock;
    } else {
      totalFrames = dataSize / (channels * 2);
    }
    if (rate != clockRate) {
      clockRate = rate;
      i2s_set_clk(I2S_NUM_0, rate, I2S_BITS_PER_SAMPLE_32BIT, I2S_CHANNEL_STEREO);
    }
    Serial.printf("Playing %s - %s (%lu:%02lu)\n", albums[a].title.c_str(), albums[a].tracks[t].title.c_str(),
                  (unsigned long)(totalFrames / rate / 60), (unsigned long)(totalFrames / rate % 60));
    return true;
  };

  // Next track, carrying on into the next album after the last one. Skips files that won't open.
  auto next = [&]() {
    for (int tries = 0; tries < 200; tries++) {
      int a = album, t = track + 1;
      if (t >= (int)albums[a].tracks.size()) { a = (a + 1) % albums.size(); t = 0; }
      if (open(a, t)) return;
      album = a; track = t;
    }
    album = -1;   // nothing plays
  };

  while (true) {
    Cmd c;
    bool idle = album < 0 || paused;
    if (xQueueReceive(cmdQueue, &c, idle ? portMAX_DELAY : 0) == pdTRUE) {
      switch (c.type) {
        case CMD_PLAY:
          paused = false;
          i2s_zero_dma_buffer(I2S_NUM_0);
          if (!open(c.album, c.track)) { album = c.album; track = c.track; next(); }
          break;
        case CMD_TOGGLE_PAUSE:
          if (album < 0) break;
          paused = !paused;
          if (paused) {
            // Stop now: drop what's queued and step back so nothing is skipped on resume
            i2s_zero_dma_buffer(I2S_NUM_0);
            seekTo(pos - min(sinceStart, min(BUFFERED, pos)));
          }
          sinceStart = 0;
          break;
        case CMD_NEXT:
          if (album >= 0) { paused = false; i2s_zero_dma_buffer(I2S_NUM_0); next(); }
          break;
        case CMD_PREV:
          if (album >= 0) {
            paused = false;
            i2s_zero_dma_buffer(I2S_NUM_0);
            // Within the first 3 s, go to the previous track; after that, restart this one
            int t = (pos < rate * 3 && track > 0) ? track - 1 : track;
            if (!open(album, t)) next();
          }
          break;
      }
      publish();
      continue;
    }

    // Play one chunk: an ADPCM block, or 1024 frames of PCM
    uint32_t t0 = micros();
    uint32_t frames = 0;
    if (pos < totalFrames) {
      if (adpcm) {
        if (f.read(raw, blockAlign) == blockAlign) frames = decodeImaBlock(raw, blockAlign, channels, pcm);
      } else {
        uint32_t want = min<uint32_t>(PCM_CHUNK, totalFrames - pos);
        frames = f.read((uint8_t *)pcm, want * channels * 2) / (channels * 2);
      }
    }
    if (frames == 0) {   // end of the track: straight on to the next, no gap
      next();
      publish();
      continue;
    }
    const float vol = volumeGain(volumeLevel);
    for (uint32_t i = 0; i < frames; i++) {
      int16_t l = pcm[i * channels], r = pcm[i * channels + channels - 1];
      out[2 * i]     = (int32_t)(l * vol) * 65536;   // 16-bit audio in the top half of each 32-bit slot
      out[2 * i + 1] = (int32_t)(r * vol) * 65536;
    }
    worstUs = max<uint32_t>(worstUs, micros() - t0);
    if ((int32_t)(millis() - reportAt) >= 0) {
      Serial.printf("audio: slowest read+decode %.1f ms for %.0f ms of sound\n", worstUs / 1000.0f, frames * 1000.0f / rate);
      worstUs = 0;
      reportAt = millis() + 10000;   // from now, so time spent idle or paused is not caught up in a burst
    }
    size_t written;
    i2s_write(I2S_NUM_0, out, frames * 8, &written, portMAX_DELAY);   // waits until the DAC needs more
    pos += frames;
    sinceStart += frames;
    publish();
  }
}

// ---------- UI (screens and buttons, on core 0) ----------
extern volatile bool uploading;
#include "UI.h"

// ---------- Copying music over the USB cable (used by upload_music.py) ----------
//
// Line commands on the serial port, answers start with '@':
//   PING                 -> @PONG
//   BAUD <rate>          -> @OK, then switches to that baud rate
//   STAT <path>          -> @SIZE <bytes>   (-1 if missing)
//   PUT <bytes> <path>   -> @GO, then per 16 KB chunk received: @K, and at the end @CRC <crc32 hex>
//   MSG <text>           -> shows the text on the screen
//   REBOOT               -> @OK, restarts

volatile bool sdMounted = false;
volatile bool uploading = false;
char uploadMsg[40] = "";

// Standard CRC-32 (same as Python's zlib.crc32)
uint32_t crc32Update(uint32_t crc, const uint8_t *p, size_t n) {
  static uint32_t table[256];
  if (!table[1]) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
  }
  crc = ~crc;
  while (n--) crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

void makeParents(const String &path) {
  for (int i = path.indexOf('/', 1); i > 0; i = path.indexOf('/', i + 1)) SD.mkdir(path.substring(0, i));
}

void receiveFile(uint32_t size, const String &path) {
  static uint8_t buf[16384];
  if (!sdMounted) { Serial.println("@ERR no SD card"); return; }
  makeParents(path);
  String part = path + ".part";
  SD.remove(part);
  File f = SD.open(part, FILE_WRITE);
  if (!f) { Serial.println("@ERR can't create file"); return; }
  Serial.println("@GO");
  uint32_t crc = 0, left = size;
  while (left > 0) {
    size_t n = min<uint32_t>(sizeof(buf), left);
    if (Serial.readBytes(buf, n) != n) {   // timed out: the sender stopped
      f.close();
      SD.remove(part);
      Serial.println("@ERR timeout");
      return;
    }
    crc = crc32Update(crc, buf, n);
    if (f.write(buf, n) != n) {
      f.close();
      SD.remove(part);
      Serial.println("@ERR write failed (card full?)");
      return;
    }
    left -= n;
    Serial.println("@K");
  }
  f.close();
  SD.remove(path);
  SD.rename(part, path);
  Serial.printf("@CRC %08lx\n", (unsigned long)crc);
}

void serialTask(void *) {
  while (true) {
    if (!Serial.available()) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line == "PING") {
      Serial.println("@PONG");
    } else if (line.startsWith("BAUD ")) {
      Serial.println("@OK");
      Serial.flush();
      delay(50);
      Serial.updateBaudRate(line.substring(5).toInt());
    } else if (line.startsWith("STAT ")) {
      File f = sdMounted ? SD.open(line.substring(5)) : File();
      Serial.printf("@SIZE %ld\n", f && !f.isDirectory() ? (long)f.size() : -1L);
    } else if (line.startsWith("PUT ")) {
      int sp = line.indexOf(' ', 4);
      uploading = true;
      receiveFile(line.substring(4, sp).toInt(), line.substring(sp + 1));
    } else if (line.startsWith("MSG ")) {
      uploading = true;
      strlcpy(uploadMsg, line.c_str() + 4, sizeof(uploadMsg));
      ui::setMessage(uploadMsg);
    } else if (line == "REBOOT") {
      Serial.println("@OK");
      Serial.flush();
      delay(100);
      ESP.restart();
    }
  }
}

// ---------- Setup ----------

SPIClass sdSPI(FSPI);

bool mountSD() {
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  for (uint32_t freq : { 20000000, 10000000, 4000000 }) {
    if (SD.begin(SD_CS, sdSPI, freq)) {
      Serial.printf("SD card: %llu MB, SPI at %lu MHz\n", SD.cardSize() / (1024 * 1024), (unsigned long)(freq / 1000000));
      return true;
    }
    SD.end();
  }
  return false;
}

void setup() {
  Serial.setRxBufferSize(65536);   // room for incoming music while the card is busy writing
  Serial.begin(115200);
  Serial.setTimeout(3000);
  uart_set_rx_full_threshold(UART_NUM_0, 64);   // empty the 128-byte hardware FIFO early: copes with fast baud rates
  delay(300);
  prefs.begin("sdplayer", false);
  volumeLevel = constrain(prefs.getUChar("volume", VOLUME_DEFAULT), 1, VOLUME_LEVELS);
  ui::dialView = prefs.getBool("dial", false);

  ui::begin();   // shows a message until the library is loaded
  xTaskCreatePinnedToCore(serialTask, "serial", 6144, nullptr, 2, nullptr, 0);

  while (true) {
    if (uploading) {               // music is arriving: wait for the uploader's REBOOT
      delay(500);
      continue;
    }
    if (!sdMounted) {
      sdMounted = mountSD();
      if (!sdMounted) {
        ui::setMessage("NO SD CARD");
        Serial.println("No SD card");
        delay(2000);
        continue;
      }
    }
    Serial.println("Albums:");
    if (loadLibrary() > 0) break;
    ui::setMessage("NO MUSIC");
    Serial.println("No albums in /music. Copy them with upload_music.py, or onto the card with a card reader.");
    delay(2000);
  }

  setupI2S(44100);
  cmdQueue = xQueueCreate(8, sizeof(Cmd));
  // Core 1, above the Arduino loop: the player only waits on the DAC
  xTaskCreatePinnedToCore(playerTask, "player", 8192, nullptr, 5, nullptr, 1);
  ui::setReady();
}

void loop() {
  vTaskDelete(nullptr);   // everything runs in the player, UI and serial tasks
}

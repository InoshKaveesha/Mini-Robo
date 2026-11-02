#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP_I2S.h>
#include <vector>

// ======================== WiFi & Server Config ========================
const char* WIFI_SSID     = "Redmi Note 12";
const char* WIFI_PASSWORD = "123123123";

// Your EC2 / local server URL (no trailing slash)
const char* SERVER_URL    = "http://54.242.120.106:8000";

// ======================== Pin Definitions ========================
#define OLED_SDA    21
#define OLED_SCL    22
#define BUTTON_PIN  18
#define LED_PIN     19

// INMP441 I2S Microphone
#define MIC_BCLK    26
#define MIC_WS      25
#define MIC_SD      33

// MAX98357A I2S Amplifier
#define SPK_BCLK    27
#define SPK_WS      14
#define SPK_DOUT    32

// ======================== Audio Settings ========================
// Must match server.py: 16 kHz, 16-bit mono PCM
#define MIC_RATE            16000
#define SPK_RATE            16000
#define MAX_RECORD_SECONDS  4

// Small 2 KB chunks allocated dynamically to avoid DRAM overflow & heap fragmentation
#define CHUNK_SIZE          2048

Adafruit_SSD1306 display(128, 64, &Wire, -1);

// Separate I2S controllers on ESP32
I2SClass mic(I2S_NUM_0);
I2SClass speaker(I2S_NUM_1);

bool micReady     = false;
bool speakerReady = false;

// ======================== OLED Face Expressions ========================
void face(const char* mood) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  if (!strcmp(mood, "happy")) {
    display.fillRoundRect(22, 17, 29, 22, 9, SSD1306_WHITE);
    display.fillRoundRect(77, 17, 29, 22, 9, SSD1306_WHITE);
    display.fillCircle(36, 28, 4, SSD1306_BLACK);
    display.fillCircle(90, 28, 4, SSD1306_BLACK);
    display.drawRoundRect(46, 43, 36, 12, 6, SSD1306_WHITE);
  } else if (!strcmp(mood, "listen")) {
    display.fillCircle(37, 27, 12, SSD1306_WHITE);
    display.fillCircle(91, 27, 12, SSD1306_WHITE);
    display.fillCircle(37, 27, 5, SSD1306_BLACK);
    display.fillCircle(91, 27, 5, SSD1306_BLACK);
    display.drawRoundRect(55, 44, 18, 8, 4, SSD1306_WHITE);
  } else if (!strcmp(mood, "think")) {
    display.drawRoundRect(22, 19, 30, 17, 7, SSD1306_WHITE);
    display.drawRoundRect(77, 19, 30, 17, 7, SSD1306_WHITE);
    display.drawLine(30, 28, 43, 28, SSD1306_WHITE);
    display.drawLine(85, 28, 98, 28, SSD1306_WHITE);
    display.drawCircle(64, 48, 5, SSD1306_WHITE);
  } else if (!strcmp(mood, "talk")) {
    display.fillRoundRect(22, 17, 29, 22, 9, SSD1306_WHITE);
    display.fillRoundRect(77, 17, 29, 22, 9, SSD1306_WHITE);
    display.fillCircle(36, 28, 4, SSD1306_BLACK);
    display.fillCircle(90, 28, 4, SSD1306_BLACK);
    display.fillRoundRect(48, 43, 32, 15, 6, SSD1306_WHITE);
    display.drawLine(52, 49, 76, 49, SSD1306_BLACK);
  } else { // "sad" / error
    display.drawRoundRect(22, 19, 30, 17, 7, SSD1306_WHITE);
    display.drawRoundRect(77, 19, 30, 17, 7, SSD1306_WHITE);
    display.drawLine(26, 25, 47, 31, SSD1306_WHITE);
    display.drawLine(81, 31, 102, 25, SSD1306_WHITE);
    display.drawLine(49, 53, 79, 53, SSD1306_WHITE);
  }

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("AI ROBOT");
  display.display();
}

// ======================== WiFi Connection ========================
bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  face("think");
  Serial.print("Connecting to Wi-Fi");

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(300);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi connected! IP: ");
    Serial.println(WiFi.localIP());
    face("happy");
    return true;
  }

  Serial.println("Wi-Fi connection failed!");
  face("sad");
  return false;
}

// ======================== Record Audio from Mic ========================
// Records raw 16 kHz 16-bit mono PCM into small 2 KB heap chunks
size_t recordAudio(std::vector<uint8_t*>& chunks) {
  if (!micReady) {
    Serial.println("Mic I2S not ready!");
    return 0;
  }

  // Clear any existing chunks
  for (uint8_t* c : chunks) free(c);
  chunks.clear();

  size_t totalBytes = 0;
  const size_t maxBytes = MIC_RATE * sizeof(int16_t) * MAX_RECORD_SECONDS;

  int32_t raw[128];
  unsigned long start = millis();
  bool buttonHeld = (digitalRead(BUTTON_PIN) == LOW);

  face("listen");
  Serial.println("Listening... Speak now. (Release button when done)");

  uint8_t* currentChunk = (uint8_t*)malloc(CHUNK_SIZE);
  if (!currentChunk) {
    Serial.println("Error: Insufficient memory to start recording!");
    return 0;
  }
  size_t chunkPos = 0;

  while (millis() - start < MAX_RECORD_SECONDS * 1000UL) {
    // Stop recording as soon as button is released (push-to-talk)
    if (buttonHeld && digitalRead(BUTTON_PIN) == HIGH) {
      break;
    }

    size_t bytes = mic.readBytes((char*)raw, sizeof(raw));
    size_t samples = bytes / sizeof(int32_t);

    for (size_t i = 0; i + 1 < samples; i += 2) {
      // INMP441 left channel: use raw[i + 1] if mic L/R is tied to 3.3V
      int32_t s = raw[i] >> 14;

      if (s > 32767)  s = 32767;
      if (s < -32768) s = -32768;

      int16_t val = (int16_t)s;

      currentChunk[chunkPos++] = (uint8_t)(val & 0xFF);
      currentChunk[chunkPos++] = (uint8_t)((val >> 8) & 0xFF);
      totalBytes += 2;

      // When chunk is full, push to list and allocate next chunk
      if (chunkPos >= CHUNK_SIZE) {
        chunks.push_back(currentChunk);
        chunkPos = 0;

        if (totalBytes >= maxBytes) {
          currentChunk = nullptr;
          break;
        }

        currentChunk = (uint8_t*)malloc(CHUNK_SIZE);
        if (!currentChunk) {
          Serial.println("Memory limit reached, finishing recording.");
          break;
        }
      }
    }

    if (!currentChunk || totalBytes >= maxBytes) {
      break;
    }
  }

  // Keep any remaining recorded bytes in the active chunk
  if (currentChunk) {
    if (chunkPos > 0) {
      chunks.push_back(currentChunk);
    } else {
      free(currentChunk);
    }
  }

  // Need at least 0.5s of audio (4000 samples = 8000 bytes)
  if (totalBytes < (MIC_RATE * sizeof(int16_t) / 2)) {
    Serial.println("Recording too short (< 0.5s)");
    for (uint8_t* c : chunks) free(c);
    chunks.clear();
    return 0;
  }

  Serial.printf("Recorded %u bytes PCM (%.1f sec, %u chunks)\n",
                (unsigned int)totalBytes,
                (float)totalBytes / (MIC_RATE * sizeof(int16_t)),
                (unsigned int)chunks.size());
  return totalBytes;
}

// ======================== Send Voice & Play Reply ========================
// Sends recorded audio to /voice, downloads the full WAV response into RAM,
// then plays it back from memory for smooth, uninterrupted audio.
bool sendVoiceAndPlay(const std::vector<uint8_t*>& chunks, size_t totalBytes) {
  if (chunks.empty() || totalBytes == 0) return false;

  if (WiFi.status() != WL_CONNECTED) {
    if (!connectWiFi()) return false;
  }

  // Parse host and port from SERVER_URL (e.g. "http://54.242.120.106:8000")
  String urlStr = String(SERVER_URL);
  bool isHttps = urlStr.startsWith("https://");
  urlStr.replace("http://", "");
  urlStr.replace("https://", "");

  int slashIdx = urlStr.indexOf('/');
  if (slashIdx != -1) urlStr = urlStr.substring(0, slashIdx);

  int colonIdx = urlStr.indexOf(':');
  String host = (colonIdx != -1) ? urlStr.substring(0, colonIdx) : urlStr;
  uint16_t port = (colonIdx != -1) ? urlStr.substring(colonIdx + 1).toInt() : (isHttps ? 443 : 80);

  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  WiFiClient* client = nullptr;

  if (isHttps) {
    secureClient.setInsecure();
    if (!secureClient.connect(host.c_str(), port)) {
      Serial.println("HTTPS connection failed!");
      return false;
    }
    client = &secureClient;
  } else {
    if (!plainClient.connect(host.c_str(), port)) {
      Serial.println("HTTP connection failed!");
      return false;
    }
    client = &plainClient;
  }

  client->setTimeout(180000);  // 3-minute timeout for Whisper + LLM + TTS

  face("think");
  Serial.println("Sending audio to AI server...");

  // Send HTTP POST headers
  client->printf(
    "POST /voice HTTP/1.1\r\n"
    "Host: %s:%u\r\n"
    "Content-Type: application/octet-stream\r\n"
    "Content-Length: %u\r\n"
    "Connection: close\r\n\r\n",
    host.c_str(), port, (unsigned int)totalBytes
  );

  // Send all audio chunks
  size_t bytesSent = 0;
  for (size_t i = 0; i < chunks.size(); i++) {
    size_t toWrite = min((size_t)CHUNK_SIZE, totalBytes - bytesSent);
    client->write(chunks[i], toWrite);
    bytesSent += toWrite;
  }

  // Wait for server response
  unsigned long respStart = millis();
  while (!client->available() && client->connected()) {
    if (millis() - respStart > 180000) {
      Serial.println("Server response timeout!");
      client->stop();
      return false;
    }
    delay(10);
  }

  // Parse status line: "HTTP/1.1 200 OK"
  String statusLine = client->readStringUntil('\n');
  statusLine.trim();
  int statusCode = 0;
  int space1 = statusLine.indexOf(' ');
  if (space1 != -1) {
    statusCode = statusLine.substring(space1 + 1, space1 + 4).toInt();
  }

  if (statusCode != 200) {
    Serial.printf("Voice API HTTP error: %d\n", statusCode);
    Serial.println(statusLine);

    // Skip HTTP headers until blank line
    while (client->connected()) {
      String h = client->readStringUntil('\n');
      h.trim();
      if (h.length() == 0) break;
    }

    // Read and print the server's error message body
    unsigned long errStart = millis();
    while ((client->connected() || client->available()) && millis() - errStart < 3000) {
      if (client->available()) {
        String errLine = client->readStringUntil('\n');
        errLine.trim();
        if (errLine.length() > 0) {
          Serial.println("Server says: " + errLine);
        }
      } else {
        delay(10);
      }
    }

    client->stop();
    return false;
  }

  // Parse HTTP response headers (extract X-Transcript and X-Robot-Reply)
  String transcript = "";
  String reply = "";
  while (client->connected()) {
    String line = client->readStringUntil('\n');
    line.trim();
    if (line.length() == 0) break; // empty line marks end of headers

    if (line.startsWith("X-Transcript:")) {
      transcript = line.substring(13);
      transcript.trim();
    } else if (line.startsWith("X-Robot-Reply:")) {
      reply = line.substring(14);
      reply.trim();
    }
  }

  // ---- Print conversation to Serial Monitor ----
  Serial.println("========================================");
  if (transcript.length() > 0) {
    Serial.println("  YOU:   " + transcript);
  } else {
    Serial.println("  YOU:   (no speech detected)");
  }
  if (reply.length() > 0) {
    Serial.println("  ROBOT: " + reply);
  } else {
    Serial.println("  ROBOT: (no reply)");
  }
  Serial.println("========================================");

  // ============================================================
  // Download the ENTIRE WAV response into RAM before playing.
  // This eliminates choppy audio caused by Wi-Fi jitter/latency.
  // ============================================================
  Serial.println("Downloading audio response...");

  std::vector<uint8_t*> audioChunks;
  size_t audioTotalBytes = 0;
  uint8_t* curChunk = (uint8_t*)malloc(CHUNK_SIZE);
  size_t curPos = 0;

  if (!curChunk) {
    Serial.println("Not enough memory for audio buffer!");
    client->stop();
    return false;
  }

  unsigned long dlTimeout = millis();
  while (client->connected() || client->available()) {
    int avail = client->available();
    if (avail > 0) {
      size_t wanted = min((int)(CHUNK_SIZE - curPos), avail);
      int got = client->readBytes((char*)(curChunk + curPos), wanted);
      if (got > 0) {
        curPos += got;
        audioTotalBytes += got;
        dlTimeout = millis();  // reset timeout on data received

        if (curPos >= CHUNK_SIZE) {
          audioChunks.push_back(curChunk);
          curChunk = (uint8_t*)malloc(CHUNK_SIZE);
          curPos = 0;
          if (!curChunk) {
            Serial.println("Memory limit during download, playing what we have.");
            break;
          }
        }
      }
    } else {
      if (!client->connected()) break;
      if (millis() - dlTimeout > 5000) break;  // 5-second idle timeout
      delay(2);
    }
  }

  // Keep any partial last chunk
  if (curChunk) {
    if (curPos > 0) {
      audioChunks.push_back(curChunk);
    } else {
      free(curChunk);
    }
  }

  client->stop();  // Done with network — free the socket immediately

  Serial.printf("Downloaded %u bytes audio (%u chunks)\n",
                (unsigned int)audioTotalBytes,
                (unsigned int)audioChunks.size());

  if (audioTotalBytes <= 44) {
    Serial.println("No audio data received!");
    for (uint8_t* c : audioChunks) free(c);
    return false;
  }

  // Skip the 44-byte WAV header so we play pure PCM
  size_t skipRemaining = 44;
  size_t chunkIdx = 0;
  size_t byteIdx = 0;

  while (skipRemaining > 0 && chunkIdx < audioChunks.size()) {
    size_t chunkLen = (chunkIdx == audioChunks.size() - 1)
        ? (audioTotalBytes - chunkIdx * CHUNK_SIZE)
        : CHUNK_SIZE;
    size_t canSkip = min(skipRemaining, chunkLen - byteIdx);
    byteIdx += canSkip;
    skipRemaining -= canSkip;
    if (byteIdx >= chunkLen) {
      chunkIdx++;
      byteIdx = 0;
    }
  }

  // Ensure 16-bit sample alignment
  if (byteIdx % 2 != 0) {
    byteIdx++;
    if (chunkIdx < audioChunks.size()) {
      size_t chunkLen = (chunkIdx == audioChunks.size() - 1)
          ? (audioTotalBytes - chunkIdx * CHUNK_SIZE)
          : CHUNK_SIZE;
      if (byteIdx >= chunkLen) {
        chunkIdx++;
        byteIdx = 0;
      }
    }
  }

  face("talk");
  digitalWrite(LED_PIN, HIGH);

  // Play back from RAM — perfectly smooth, no network dependency
  while (chunkIdx < audioChunks.size() && speakerReady) {
    size_t chunkLen = (chunkIdx == audioChunks.size() - 1)
        ? (audioTotalBytes - chunkIdx * CHUNK_SIZE)
        : CHUNK_SIZE;

    size_t playLen = chunkLen - byteIdx;

    // Maintain 16-bit alignment
    if (playLen % 2 != 0) playLen--;

    if (playLen > 0) {
      speaker.write(audioChunks[chunkIdx] + byteIdx, playLen);
    }

    chunkIdx++;
    byteIdx = 0;
  }

  // Free all downloaded audio chunks
  for (uint8_t* c : audioChunks) free(c);
  audioChunks.clear();

  digitalWrite(LED_PIN, LOW);
  face("happy");
  return true;
}

// ======================== Setup ========================
void setup() {
  Serial.begin(115200);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Wire.begin(OLED_SDA, OLED_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED not detected at 0x3C");
  } else {
    face("happy");
  }

  // Microphone: I2S Controller 0 (16 kHz, 32-bit slot, Stereo)
  mic.setPins(MIC_BCLK, MIC_WS, -1, MIC_SD);
  micReady = mic.begin(
      I2S_MODE_STD,
      MIC_RATE,
      I2S_DATA_BIT_WIDTH_32BIT,
      I2S_SLOT_MODE_STEREO);

  // Amplifier: I2S Controller 1 (16 kHz, 16-bit, Mono - matches server.py output)
  speaker.setPins(SPK_BCLK, SPK_WS, SPK_DOUT, -1);
  speakerReady = speaker.begin(
      I2S_MODE_STD,
      SPK_RATE,
      I2S_DATA_BIT_WIDTH_16BIT,
      I2S_SLOT_MODE_MONO);

  Serial.printf("Mic I2S: %s\n", micReady ? "OK" : "FAILED");
  Serial.printf("Speaker I2S: %s\n", speakerReady ? "OK" : "FAILED");

  connectWiFi();

  Serial.printf("Free Heap: %u bytes (Largest Block: %u bytes)\n",
                ESP.getFreeHeap(),
                ESP.getMaxAllocHeap());

  Serial.println("Ready! Press and hold the button to speak (or send 'r' in Serial).");
}

// ======================== Main Loop ========================
void loop() {
  bool trigger = false;

  // Trigger via physical Push Button (active LOW)
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(35); // debounce
    if (digitalRead(BUTTON_PIN) == LOW) {
      trigger = true;
    }
  }

  // Trigger via Serial Monitor for easy testing without button
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'r' || c == 'R') {
      trigger = true;
    }
  }

  if (trigger) {
    digitalWrite(LED_PIN, HIGH);

    std::vector<uint8_t*> chunks;
    size_t totalBytes = recordAudio(chunks);

    if (totalBytes > 0) {
      // Free recording chunks BEFORE downloading response to maximize available RAM for playback
      std::vector<uint8_t*> sendChunks = chunks;
      chunks.clear();

      if (!sendVoiceAndPlay(sendChunks, totalBytes)) {
        Serial.println("AI voice request failed");
        face("sad");
      }

      // Free recording chunks (sendVoiceAndPlay doesn't free these)
      for (uint8_t* c : sendChunks) free(c);
      sendChunks.clear();
    } else {
      face("happy");
    }

    digitalWrite(LED_PIN, LOW);

    // Wait until button is released before accepting next command
    while (digitalRead(BUTTON_PIN) == LOW) {
      delay(10);
    }
  }

  delay(10);
}

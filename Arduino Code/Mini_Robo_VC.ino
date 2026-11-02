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
// Streams chunks directly via WiFiClient to /voice and plays back returned WAV
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

  if (transcript.length() > 0) {
    Serial.println("You said: " + transcript);
  }
  if (reply.length() > 0) {
    Serial.println("AI Reply: " + reply);
  }

  // Skip the 44-byte WAV header so we stream pure PCM to the speaker
  int headerToSkip = 44;
  unsigned long skipStart = millis();
  while (headerToSkip > 0 && (client->connected() || client->available())) {
    int avail = client->available();
    if (avail > 0) {
      int toRead = min(avail, headerToSkip);
      uint8_t dummy[44];
      int r = client->readBytes((char*)dummy, toRead);
      if (r > 0) headerToSkip -= r;
    } else {
      if (millis() - skipStart > 10000) break;
      delay(2);
    }
  }

  face("talk");
  digitalWrite(LED_PIN, HIGH);

  // Pre-buffer: wait until at least 4 KB is received so playback starts smoothly without stutter
  unsigned long preBufferStart = millis();
  while (client->connected() && client->available() < 4096 && millis() - preBufferStart < 1500) {
    delay(10);
  }

  uint8_t buffer[1024];
  uint8_t carryByte = 0;
  bool hasCarry = false;
  unsigned long lastDataTime = millis();

  // Stream audio chunks directly to I2S speaker with strict 16-bit sample alignment
  while (client->connected() || client->available() || hasCarry) {
    int avail = client->available();
    if (avail > 0 || hasCarry) {
      size_t offset = 0;
      if (hasCarry) {
        buffer[0] = carryByte;
        offset = 1;
        hasCarry = false;
      }

      size_t wanted = min((int)(sizeof(buffer) - offset), avail);
      int got = 0;
      if (wanted > 0) {
        got = client->readBytes((char*)buffer + offset, wanted);
      }

      size_t total = offset + (got > 0 ? got : 0);

      // 16-bit PCM samples must be strictly 2-byte aligned.
      // If an odd byte arrived, save it for the next chunk to prevent High/Low byte swapping & harsh static!
      if (total % 2 != 0) {
        carryByte = buffer[total - 1];
        hasCarry = true;
        total--;
      }

      if (total > 0 && speakerReady) {
        speaker.write(buffer, total);
      }
      lastDataTime = millis();
    } else {
      if (!client->connected() && client->available() == 0) break;
      if (millis() - lastDataTime > 3000) break; // 3-second stream timeout
      delay(2);
    }
  }

  digitalWrite(LED_PIN, LOW);
  client->stop();
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

  Serial.printf("Free Heap: %u bytes (Largest Contiguous Block: %u bytes)\n",
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
      if (!sendVoiceAndPlay(chunks, totalBytes)) {
        Serial.println("AI voice request failed");
        face("sad");
      }
    } else {
      face("happy");
    }

    // Clean up all allocated chunks from heap
    for (uint8_t* c : chunks) {
      free(c);
    }
    chunks.clear();

    digitalWrite(LED_PIN, LOW);

    // Wait until button is released before accepting next command
    while (digitalRead(BUTTON_PIN) == LOW) {
      delay(10);
    }
  }

  delay(10);
}

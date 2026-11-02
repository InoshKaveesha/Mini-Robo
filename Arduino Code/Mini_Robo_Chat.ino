#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ======================== WiFi & Server Config ========================
const char* ssid     = "Redmi Note 12";
const char* password = "123123123";

// NOTE: Update this IP to match your PC's IP address (from `ipconfig`)
// If on home Wi-Fi: use "http://192.168.8.131:8000/chat"
// If on phone hotspot: use your PC's hotspot IP (e.g., "http://172.17.42.140:8000/chat")
const char* serverURL = "http://54.242.120.106:8000/chat";

// ======================== Display Config ========================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

void showMessage(String line1, String line2 = "") {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 10);
  display.println(line1);
  if (line2.length() > 0) {
    display.setCursor(0, 30);
    display.println(line2);
  }
  display.display();
}

void showResponse(String title, String message) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(title);
  display.println("---------------------");
  display.println(message);
  display.display();
}

void testAI() {
  HTTPClient http;
  http.setTimeout(180000);  // 3-minute timeout for LLM inference
  http.begin(serverURL);
  http.addHeader("Content-Type", "application/json");

  Serial.println("Sending prompt to AI server...");
  showMessage("AI Robot", "Thinking...");

  int code = http.POST(
    "{\"message\":\"Say hello to my ESP32 robot in one short sentence.\"}"
  );

  Serial.printf("HTTP status: %d\n", code);

  if (code == 200) {
    String body = http.getString();
    Serial.println("Server Response: " + body);

    // Extract the reply text from {"reply":"..."}
    String reply = "";
    int startIdx = body.indexOf("\"reply\":");
    if (startIdx != -1) {
      startIdx = body.indexOf("\"", startIdx + 8); // Opening quote of the reply
      if (startIdx != -1) {
        startIdx++;
        int endIdx = body.lastIndexOf("\"");
        if (endIdx > startIdx) {
          reply = body.substring(startIdx, endIdx);
          reply.replace("\\n", " ");
          reply.replace("\\\"", "\"");
        }
      }
    }

    if (reply.length() > 0) {
      showResponse("Robot Reply:", reply);
    } else {
      showResponse("AI Connected!", "Reply received");
    }
  } else {
    String err = http.getString();
    Serial.println("Error Body: " + err);
    showResponse("AI Error", "HTTP Code: " + String(code));
  }

  http.end();
}

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED not found!");
    while (true) delay(100);
  }

  showMessage("AI Robot", "Connecting WiFi...");

  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("ESP32 Connected! IP: ");
    Serial.println(WiFi.localIP());
    showMessage("WiFi connected!", "Testing AI...");
    delay(1000);
    testAI();
  } else {
    showMessage("WiFi failed!", "Check hotspot");
    Serial.println("WiFi connection failed!");
  }
}

void loop() {
  delay(1000);
}

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include "HX711.h"

// ===============================================================
//  SECTION 1 — CONFIGURATION
// ===============================================================

// --- WiFi ---
const char* WIFI_SSID     = "";
const char* WIFI_PASSWORD = "";

// --- Telegram ---
#define BOT_TOKEN   ""
#define CHAT_ID     ""

// --- Pin Definitions ---
#define LED_PIN         2       // Onboard LED
#define MQ2_DO_PIN      15      // MQ2 Digital Output → GPIO15 (ADC2_CH3)
#define MQ2_AO_PIN      34      // MQ2 Analog Output  → GPIO34 (ADC1_CH6, input-only)
#define LOAD_CELL_DT    14      // HX711 Data
#define LOAD_CELL_SCK   27      // HX711 Clock

// --- MQ2 Thresholds ---
#define MQ2_AO_THRESHOLD    200   // ADC raw value (0–4095). Tune after calibration.
#define MQ2_POLL_INTERVAL_MS  200   // How often to read MQ2 (ms)

// --- HX711 ---
#define SCALE_FACTOR        -38653.5 / 1.36

#define WEIGHT_POLL_MS      500     // How often to read weight (ms)

// --- LED Blink ---
#define LED_BLINK_INTERVAL_MS  200  // Blink every 200 ms (heartbeat when running)

// --- Telegram poll rate ---
#define TELEGRAM_POLL_MS    1000

// --- Weight Alert Threshold ---
#define WEIGHT_ALERT_PCT    20.0    // Alert when fill level drops below this %


// ===============================================================
//  SECTION 2 — GLOBALS
// ===============================================================

#include <Preferences.h>

WiFiClientSecure  secureClient;
UniversalTelegramBot bot(BOT_TOKEN, secureClient);

HX711 scale;
Preferences prefs;

// State flags
bool gasAlertSent      = false;   // True once over-threshold alert has been sent
bool gasAlertActive    = false;   // True while gas level is above threshold
bool weightAlertSent   = false;   // True once low-weight alert has been sent per refill cycle

// Recorded (full) weight loaded from NVS
float recordedWeight   = 0.0;

// Timestamps for non-blocking timing
unsigned long lastMQ2Check     = 0;
unsigned long lastWeightCheck  = 0;
unsigned long lastTelegramPoll = 0;
unsigned long lastLedToggle    = 0;

// LED state for blink
bool ledState = false;


// ===============================================================
//  SECTION 3 — WiFi
// ===============================================================

void connectWiFi() {
  Serial.print("[WiFi] Connecting to ");
  Serial.print(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("[WiFi] Connected! IP: ");
  Serial.println(WiFi.localIP());
}


// ===============================================================
//  SECTION 4 — TELEGRAM BOT
// ===============================================================

void initBot() {
  secureClient.setInsecure();   // Skip SSL cert validation (simpler for ESP32)
  bot.sendMessage(CHAT_ID, "🚀 ESP32 LPG Monitor online and running!", "");
  Serial.println("[Bot] Startup message sent.");
}

void sendTelegramAlert(const String& message) {
  bool ok = bot.sendMessage(CHAT_ID, message, "");
  Serial.print("[Bot] Alert sent → ");
  Serial.println(ok ? "OK" : "FAILED");
}

void handleCommand(int i) {
  String text    = bot.messages[i].text;
  String chat_id = String(bot.messages[i].chat_id);

  Serial.print("[Bot] Command received: ");
  Serial.println(text);

  if (text == "/start") {
    bot.sendMessage(chat_id,
      "👋 ESP32 LPG Monitor\n"
      "Commands:\n"
      "  /status  — System status\n"
      "  /gas     — Current gas reading\n"
      "  /tare    — Tare weight\n"
      "  /weight  — Current weight\n"
      "  /record  — Save current weight as full reference\n"
      "  /led_on  — LED on\n"
      "  /led_off — LED off", "");
  }
  else if (text == "/status") {
    String msg = "✅ System running\n";
    msg += gasAlertActive ? "⚠️ Gas ALERT active!\n" : "✅ Gas level normal\n";
    msg += "WiFi: " + WiFi.localIP().toString();
    bot.sendMessage(chat_id, msg, "");
  }
  else if (text == "/gas") {
    int aoVal = readMQ2Analog();
    int doVal = readMQ2Digital();
    String msg = "🔬 Gas Reading\n";
    msg += "Analog (AO): " + String(aoVal) + " / 4095\n";
    msg += "Digital (DO): " + String(doVal) + (doVal == LOW ? " (DETECTED)" : " (clear)");
    bot.sendMessage(chat_id, msg, "");
  }
  else if (text == "/weight") {
    if (scale.is_ready()) {
      float w = scale.get_units(5);
      if (w < 0) w = 0;
      String msg = "⚖️ Weight: " + String(w, 2) + " kg";
      if (recordedWeight > 0) {
        float pct = (w / recordedWeight) * 100.0;
        msg += "\n📊 Fill level: " + String(pct, 1) + "%";
      } else {
        msg += "\n⚠️ No reference weight recorded. Use /record on a full cylinder.";
      }
      bot.sendMessage(chat_id, msg, "");
    } else {
      bot.sendMessage(chat_id, "❌ HX711 not ready — check wiring.", "");
    }
  }
  else if (text == "/tare") {
    if (scale.is_ready()) {
      scale.set_scale(SCALE_FACTOR);
      delay(500);
      scale.tare();
      bot.sendMessage(chat_id, "⚖️ Scale tared successfully.", "");
      Serial.print("[Scale] Raw after tare: ");
      Serial.println(scale.get_units(10));
    } else {
      bot.sendMessage(chat_id, "❌ HX711 not ready.", "");
    }
  }
  else if (text == "/record") {
    if (scale.is_ready()) {
      scale.set_scale(SCALE_FACTOR);
      float w = scale.get_units(10);
      if (w < 0) w = 0;
      recordedWeight = w;
      prefs.putFloat("recWeight", recordedWeight);
      weightAlertSent = false;   // Reset alert so it fires again when level drops
      bot.sendMessage(chat_id,
        "💾 Reference weight saved: " + String(recordedWeight, 2) + " g\n"
        "ℹ️ You will be alerted when fill level drops below " + String(WEIGHT_ALERT_PCT, 0) + "%.", "");
      Serial.printf("[NVS] Saved recorded weight: %.2f g\n", recordedWeight);
    } else {
      bot.sendMessage(chat_id, "❌ HX711 not ready.", "");
    }
  }
  else if (text == "/led_on") {
    digitalWrite(LED_PIN, HIGH);
    ledState = true;
    bot.sendMessage(chat_id, "💡 LED ON", "");
  }
  else if (text == "/led_off") {
    digitalWrite(LED_PIN, LOW);
    ledState = false;
    bot.sendMessage(chat_id, "🌑 LED OFF", "");
  }
  else {
    bot.sendMessage(chat_id, "❓ Unknown command. Send /start for help.", "");
  }
}

void pollTelegram() {
  int numNew = bot.getUpdates(bot.last_message_received + 1);
  while (numNew) {
    for (int i = 0; i < numNew; i++) {
      handleCommand(i);
    }
    numNew = bot.getUpdates(bot.last_message_received + 1);
  }
}


// ===============================================================
//  SECTION 5 — MQ2 GAS SENSOR
// ===============================================================

int readMQ2Analog() {
  return analogRead(MQ2_AO_PIN);   // 0 – 4095
}

int readMQ2Digital() {
  return digitalRead(MQ2_DO_PIN);
}

void handleGasSensor() {
  int aoValue = readMQ2Analog();
  int doValue = readMQ2Digital();

  Serial.printf("[MQ2] AO=%d  DO=%s\n",
    aoValue,
    doValue == LOW ? "GAS DETECTED" : "clear");

  bool overThreshold = (aoValue >= MQ2_AO_THRESHOLD) || (doValue == LOW);

  if (overThreshold) {
    gasAlertActive = true;

    if (!gasAlertSent) {
      String alertMsg =
        "🚨 *LPG/GAS ALERT!*\n"
        "High alcohol/gas level detected!\n"
        "Analog reading: " + String(aoValue) + " / 4095\n"
        "⚠️ Please *TURN OFF the LPG* immediately!\n"
        "Monitoring started — updates will follow.";
      sendTelegramAlert(alertMsg);
      gasAlertSent = true;
    }

    static unsigned long lastMonitorMsg = 0;
    if (millis() - lastMonitorMsg >= 5000) {
      String monitorMsg =
        "📊 *Gas Monitor Update*\n"
        "AO: " + String(aoValue) + " / 4095\n"
        "DO: " + (doValue == HIGH ? "GAS DETECTED 🔴" : "clear 🟢");
      sendTelegramAlert(monitorMsg);
      lastMonitorMsg = millis();
    }

  } else {
    if (gasAlertActive) {
      sendTelegramAlert("✅ Gas level back to normal. Alert cleared.");
      gasAlertActive = false;
      gasAlertSent   = false;
    }
  }
}


// ===============================================================
//  SECTION 6 — HX711 LOAD CELL / SCALE
// ===============================================================

void initScale() {
  scale.begin(LOAD_CELL_DT, LOAD_CELL_SCK);
  delay(4000);   // Let HX711 stabilise

  if (scale.is_ready()) {
    scale.set_scale();
    delay(500);
    scale.tare();
    Serial.println("[Scale] HX711 ready and tared ✅");
  } else {
    Serial.println("[Scale] HX711 NOT FOUND ❌ — check wiring!");
  }
}

void handleScale() {
  if (!scale.is_ready()) {
    Serial.println("[Scale] HX711 not ready.");
    return;
  }

  scale.set_scale(SCALE_FACTOR);
  float weight = scale.get_units(20);
  if (weight < 0) weight = 0;   // Clamp negatives to zero
  Serial.printf("[Scale] Weight: %.3f kg\n", weight);

  // --- Fill level monitoring ---
  if (recordedWeight > 0) {
    float pct = (weight / recordedWeight) * 100.0;
    if (pct < 0) pct = 0;   // Clamp negative percentage to zero
    Serial.printf("[Scale] Fill level: %.1f%%\n", pct);

    if (pct < WEIGHT_ALERT_PCT && !weightAlertSent) {
      String msg =
        "🛒 *Time to Buy!*\n"
        "LPG fill level is critically low!\n"
        "Current: " + String(weight, 2) + " g\n"
        "Fill level: " + String(pct, 1) + "%\n"
        "⚠️ Please refill your cylinder soon.";
      sendTelegramAlert(msg);
      weightAlertSent = true;
      Serial.println("[Scale] Low-weight alert sent.");
    }

    // Reset alert flag once cylinder is refilled (level goes back above threshold)
    if (pct >= WEIGHT_ALERT_PCT && weightAlertSent) {
      weightAlertSent = false;
      Serial.println("[Scale] Fill level restored — alert flag reset.");
    }
  }
}


// ===============================================================
//  SECTION 7 — LED HEARTBEAT BLINK
// ===============================================================

void handleLedBlink() {
  unsigned long now = millis();
  if (now - lastLedToggle >= LED_BLINK_INTERVAL_MS) {
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState ? HIGH : LOW);
    lastLedToggle = now;
  }
}


// ===============================================================
//  SECTION 8 — SETUP
// ===============================================================

void setup() {
  Serial.begin(115200);
  Serial.println("\n========== ESP32 LPG Monitor ==========");

  // GPIO setup
  pinMode(LED_PIN,    OUTPUT);
  pinMode(MQ2_DO_PIN, INPUT);     // MQ2 digital output (active-LOW)
  // GPIO34 is input-only, no pinMode needed for analogRead

  // Peripherals
  initScale();

  // Load saved reference weight from NVS
  prefs.begin("lpg", false);
  recordedWeight = prefs.getFloat("recWeight", 0.0);
  Serial.printf("[NVS] Loaded recorded weight: %.2f kg\n", recordedWeight);

  // Network + Bot
  connectWiFi();
  initBot();

  Serial.println("[Setup] System ready. Entering main loop.");
}


// ===============================================================
//  SECTION 9 — LOOP  (non-blocking, millis()-based)
// ===============================================================

void loop() {
  unsigned long now = millis();

  // 1. Always blink LED as system heartbeat
  handleLedBlink();

  // 2. Poll MQ2 sensor
  if (now - lastMQ2Check >= MQ2_POLL_INTERVAL_MS) {
    lastMQ2Check = now;
    handleGasSensor();
  }

  // 3. Read weight
  if (now - lastWeightCheck >= WEIGHT_POLL_MS) {
    lastWeightCheck = now;
    handleScale();
    Serial.printf("Weight Raw: %d\n", scale.read());
  }

  // 4. Check for Telegram messages
  if (now - lastTelegramPoll >= TELEGRAM_POLL_MS) {
    lastTelegramPoll = now;
    pollTelegram();
  }
}

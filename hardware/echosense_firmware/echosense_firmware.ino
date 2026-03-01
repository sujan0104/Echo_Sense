/*
 * EchoSense Firmware v1.0
 * NodeMCU ESP8266 + HC-SR04 Ultrasonic Sensor
 * 
 * Features:
 *  - Continuous distance measurement
 *  - WiFi Access Point (no router needed)
 *  - HTTP JSON API for Android app
 *  - WebSocket for real-time streaming
 *  - Accelerometer-ready data packet (for Gait-Aware AI)
 *  - Timestamp + session ID for Occupancy Grid Mapping
 * 
 * Wiring:
 *  HC-SR04 TRIG  -> D1 (GPIO5)
 *  HC-SR04 ECHO  -> D2 (GPIO4)
 *  HC-SR04 VCC   -> 3.3V (use voltage divider on ECHO if 5V sensor)
 *  HC-SR04 GND   -> GND
 *  LED (optional) -> D4 (GPIO2) - built-in LED for status
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>

// ─────────────────────────────────────────
//  PIN DEFINITIONS
// ─────────────────────────────────────────
#define TRIG_PIN        5    // D1
#define ECHO_PIN        4    // D2
#define STATUS_LED      2    // D4 (built-in, active LOW)

// ─────────────────────────────────────────
//  WIFI CONFIGURATION (Access Point mode)
//  Phone connects directly to NodeMCU
//  No internet router required
// ─────────────────────────────────────────
const char* AP_SSID     = "EchoSense";
const char* AP_PASSWORD = "echosense123";
const IPAddress AP_IP(192, 168, 4, 1);

// ─────────────────────────────────────────
//  SENSOR CONFIGURATION
// ─────────────────────────────────────────
#define MAX_DISTANCE_CM     400   // HC-SR04 max range
#define MIN_DISTANCE_CM     2     // HC-SR04 min range
#define TIMEOUT_US          25000 // ~4m max range timeout
#define SAMPLE_INTERVAL_MS  50    // 20 readings/sec
#define MEDIAN_SAMPLES      5     // Median filter window size

// ─────────────────────────────────────────
//  SLIDING WINDOW FOR AI (Tier 1 + Tier 3)
//  Stores last N readings for the app to
//  feed into the temporal AI model
// ─────────────────────────────────────────
#define WINDOW_SIZE         10
float distanceWindow[WINDOW_SIZE];
int   windowIndex = 0;
bool  windowFull  = false;

// ─────────────────────────────────────────
//  SESSION & TIMING
// ─────────────────────────────────────────
unsigned long sessionStartTime = 0;
unsigned long readingCount     = 0;
unsigned long lastSampleTime   = 0;

// ─────────────────────────────────────────
//  CURRENT SENSOR STATE
// ─────────────────────────────────────────
float currentDistance   = 0.0;
float filteredDistance  = 0.0;
bool  sensorError       = false;

// ─────────────────────────────────────────
//  SERVER INSTANCES
// ─────────────────────────────────────────
ESP8266WebServer  httpServer(80);
WebSocketsServer  wsServer(81);

// ─────────────────────────────────────────
//  GAIT DETECTION STATE
//  Simple step counter using timing patterns
//  between distance fluctuations while walking
// ─────────────────────────────────────────
unsigned long lastStepTime    = 0;
int           stepCount       = 0;
float         stepCadence     = 0.0;  // steps/sec
float         lastDistForStep = 0.0;
bool          stepDetected    = false;

// ═══════════════════════════════════════════════════════
//  UTILITY: Sort array for median calculation
// ═══════════════════════════════════════════════════════
void bubbleSort(float arr[], int n) {
  for (int i = 0; i < n - 1; i++) {
    for (int j = 0; j < n - i - 1; j++) {
      if (arr[j] > arr[j + 1]) {
        float tmp = arr[j];
        arr[j]    = arr[j + 1];
        arr[j + 1] = tmp;
      }
    }
  }
}

// ═══════════════════════════════════════════════════════
//  SENSOR: Single raw distance reading (cm)
//  Returns -1.0 on timeout/error
// ═══════════════════════════════════════════════════════
float readRawDistance() {
  // Trigger pulse
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  // Measure echo
  long duration = pulseIn(ECHO_PIN, HIGH, TIMEOUT_US);

  if (duration == 0) {
    return -1.0;  // Timeout = out of range
  }

  float distance = (duration * 0.0343) / 2.0;

  if (distance < MIN_DISTANCE_CM || distance > MAX_DISTANCE_CM) {
    return -1.0;
  }

  return distance;
}

// ═══════════════════════════════════════════════════════
//  SENSOR: Median-filtered distance reading
//  Takes MEDIAN_SAMPLES readings and returns median
//  This removes spike noise from the sensor
// ═══════════════════════════════════════════════════════
float readFilteredDistance() {
  float samples[MEDIAN_SAMPLES];
  int   validCount = 0;

  for (int i = 0; i < MEDIAN_SAMPLES; i++) {
    float d = readRawDistance();
    if (d > 0) {
      samples[validCount++] = d;
    }
    delayMicroseconds(500);
  }

  if (validCount == 0) {
    sensorError = true;
    return -1.0;
  }

  sensorError = false;

  if (validCount == 1) return samples[0];

  bubbleSort(samples, validCount);
  return samples[validCount / 2];
}

// ═══════════════════════════════════════════════════════
//  SLIDING WINDOW: Add new reading to circular buffer
//  The Android app pulls this window for AI inference
// ═══════════════════════════════════════════════════════
void updateWindow(float distance) {
  distanceWindow[windowIndex] = distance;
  windowIndex = (windowIndex + 1) % WINDOW_SIZE;
  if (windowIndex == 0) windowFull = true;
}

// ═══════════════════════════════════════════════════════
//  GAIT DETECTION: Estimate step cadence
//  Based on rhythmic distance oscillation as user walks
//  NodeMCU detects the oscillation, sends cadence to app
//  App uses this for Gait-Aware Hazard Prediction
// ═══════════════════════════════════════════════════════
void updateGaitDetection(float distance) {
  float delta = abs(distance - lastDistForStep);

  // A step causes ~1-3cm oscillation in sensor reading
  // due to body bob while walking
  if (delta > 1.5 && delta < 8.0) {
    unsigned long now = millis();
    unsigned long timeSinceLastStep = now - lastStepTime;

    // Valid step cadence: 0.3s to 1.5s between steps
    if (timeSinceLastStep > 300 && timeSinceLastStep < 1500) {
      stepCadence = 1000.0 / timeSinceLastStep;  // steps/sec
      stepCount++;
      stepDetected = true;
      lastStepTime = now;
    }
  }

  lastDistForStep = distance;
}

// ═══════════════════════════════════════════════════════
//  JSON BUILDER: Full sensor data packet
//  This is what the Android app receives
// ═══════════════════════════════════════════════════════
String buildDataPacket() {
  StaticJsonDocument<512> doc;

  // Core distance data
  doc["distance_cm"]    = filteredDistance;
  doc["sensor_ok"]      = !sensorError;
  doc["timestamp_ms"]   = millis() - sessionStartTime;
  doc["reading_id"]     = readingCount;

  // Sliding window array (for temporal AI model)
  JsonArray window = doc.createNestedArray("window");
  int start = windowFull ? windowIndex : 0;
  int count = windowFull ? WINDOW_SIZE : windowIndex;
  for (int i = 0; i < count; i++) {
    window.add(distanceWindow[(start + i) % WINDOW_SIZE]);
  }

  // Gait data (for Gait-Aware Hazard Prediction)
  doc["step_cadence"]   = stepCadence;
  doc["step_count"]     = stepCount;
  doc["step_detected"]  = stepDetected;

  // Session metadata (for Occupancy Grid + Fingerprinting)
  doc["session_id"]     = sessionStartTime;
  doc["uptime_ms"]      = millis();

  stepDetected = false;  // Reset one-shot flag

  String output;
  serializeJson(doc, output);
  return output;
}

// ═══════════════════════════════════════════════════════
//  HTTP ROUTES
// ═══════════════════════════════════════════════════════

// GET /data  — single reading, used by app polling
void handleData() {
  httpServer.sendHeader("Access-Control-Allow-Origin", "*");
  httpServer.send(200, "application/json", buildDataPacket());
}

// GET /window  — returns only the sliding window array
void handleWindow() {
  StaticJsonDocument<256> doc;
  JsonArray window = doc.createNestedArray("window");
  int start = windowFull ? windowIndex : 0;
  int count = windowFull ? WINDOW_SIZE : windowIndex;
  for (int i = 0; i < count; i++) {
    window.add(distanceWindow[(start + i) % WINDOW_SIZE]);
  }
  doc["full"] = windowFull;
  String output;
  serializeJson(doc, output);
  httpServer.sendHeader("Access-Control-Allow-Origin", "*");
  httpServer.send(200, "application/json", output);
}

// GET /status  — device info, useful for app connection screen
void handleStatus() {
  StaticJsonDocument<256> doc;
  doc["device"]       = "EchoSense NodeMCU v1.0";
  doc["ip"]           = AP_IP.toString();
  doc["uptime_ms"]    = millis();
  doc["readings"]     = readingCount;
  doc["sensor_ok"]    = !sensorError;
  doc["ws_port"]      = 81;
  String output;
  serializeJson(doc, output);
  httpServer.sendHeader("Access-Control-Allow-Origin", "*");
  httpServer.send(200, "application/json", output);
}

// GET /reset  — reset session (new mapping session)
void handleReset() {
  sessionStartTime = millis();
  readingCount     = 0;
  stepCount        = 0;
  stepCadence      = 0.0;
  windowIndex      = 0;
  windowFull       = false;
  memset(distanceWindow, 0, sizeof(distanceWindow));
  httpServer.sendHeader("Access-Control-Allow-Origin", "*");
  httpServer.send(200, "application/json", "{\"status\":\"reset\"}");
}

void handleNotFound() {
  httpServer.send(404, "application/json", "{\"error\":\"not found\"}");
}

// ═══════════════════════════════════════════════════════
//  WEBSOCKET: Real-time streaming to Android app
//  App connects once, receives data every SAMPLE_INTERVAL
// ═══════════════════════════════════════════════════════
void onWebSocketEvent(uint8_t clientNum, WStype_t type,
                      uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      digitalWrite(STATUS_LED, LOW);  // LED ON (active low)
      { String connMsg = "{\"status\":\"connected\",\"device\":\"EchoSense\"}";
        wsServer.sendTXT(clientNum, connMsg); }
      break;

    case WStype_DISCONNECTED:
      digitalWrite(STATUS_LED, HIGH); // LED OFF
      break;

    case WStype_TEXT:
      // Handle commands from app
      String cmd = String((char*)payload);
      if (cmd == "reset") {
        handleReset();
      } else if (cmd == "ping") {
        String pong = "{\"pong\":true}";
        wsServer.sendTXT(clientNum, pong);
      }
      break;
  }
}

// ═══════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial.println("\n\n=== EchoSense Firmware v1.0 ===");

  // Pin setup
  pinMode(TRIG_PIN,   OUTPUT);
  pinMode(ECHO_PIN,   INPUT);
  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, HIGH); // Off initially
  digitalWrite(TRIG_PIN,   LOW);

  // WiFi Access Point
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.println("WiFi AP started: " + String(AP_SSID));
  Serial.println("IP: " + AP_IP.toString());

  // HTTP routes
  httpServer.on("/data",   handleData);
  httpServer.on("/window", handleWindow);
  httpServer.on("/status", handleStatus);
  httpServer.on("/reset",  handleReset);
  httpServer.onNotFound(handleNotFound);
  httpServer.begin();
  Serial.println("HTTP server started on port 80");

  // WebSocket server
  wsServer.begin();
  wsServer.onEvent(onWebSocketEvent);
  Serial.println("WebSocket server started on port 81");

  // Initialize session
  sessionStartTime = millis();
  memset(distanceWindow, 0, sizeof(distanceWindow));

  // Blink LED 3x = ready
  for (int i = 0; i < 3; i++) {
    digitalWrite(STATUS_LED, LOW);
    delay(200);
    digitalWrite(STATUS_LED, HIGH);
    delay(200);
  }

  Serial.println("EchoSense ready.");
  Serial.println("Connect phone to WiFi: " + String(AP_SSID));
  Serial.println("API: http://192.168.4.1/data");
  Serial.println("WS:  ws://192.168.4.1:81");
}

// ═══════════════════════════════════════════════════════
//  MAIN LOOP
// ═══════════════════════════════════════════════════════
void loop() {
  httpServer.handleClient();
  wsServer.loop();

  unsigned long now = millis();

  if (now - lastSampleTime >= SAMPLE_INTERVAL_MS) {
    lastSampleTime = now;

    // Take filtered reading
    filteredDistance = readFilteredDistance();

    if (filteredDistance > 0) {
      currentDistance = filteredDistance;
      updateWindow(filteredDistance);
      updateGaitDetection(filteredDistance);
      readingCount++;

      // Stream over WebSocket to all connected clients
      String packet = buildDataPacket();
      wsServer.broadcastTXT(packet);

      // Serial debug
      Serial.printf("[%lu] dist=%.1f cm | cadence=%.2f sps | steps=%d\n",
                    readingCount, filteredDistance, stepCadence, stepCount);
    } else {
      Serial.println("[SENSOR] No reading / out of range");
      String errMsg = "{\"error\":\"out_of_range\"}";
      wsServer.broadcastTXT(errMsg);
    }
  }
}

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <wifimanager.h>
#include <deque>
#include <string>

// Pin definitions
#define RELAY_K1 16
#define RELAY_K2 17
#define LED 23
#define START_SIGNAL 35
#define STOP_SIGNAL 34

// Create the WiFi Manager instance
WIFIMANAGER WifiManager;

// Configurable durations (default values)
unsigned long powerUpDuration = 10000;  // 10 seconds
unsigned long powerDownDuration = 10000; // 10 seconds

// WiFi credentials
const char* ssid = "Verges";
const char* password = "WeAreFamily";

// Web server
AsyncWebServer webServer(80);

// Variables for state tracking
bool lastStartState = HIGH; // Initial state for START signal
bool lastStopState = HIGH;  // Initial state for STOP signal

// Define maximum number of log entries
const size_t LOG_BUFFER_MAX_SIZE = 100;

// Use a deque to store log entries
std::deque<String> logBuffer;

// Function to log messages
void logMessage(const String& message) {
  // Add the new message to the buffer
  logBuffer.push_back(message);

  // Remove the oldest entry if the buffer exceeds the maximum size
  if (logBuffer.size() > LOG_BUFFER_MAX_SIZE) {
    logBuffer.pop_front();
  }

  // Print to Serial for debugging
  Serial.println(message);
}

// Blink function
void blinkLED(int times, int interval) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED, HIGH);
    delay(interval);
    digitalWrite(LED, LOW);
    delay(interval);
  }
}

// WiFi connection setup
void setupWiFi() {
  logMessage("Starting WiFi Manager...");

  WifiManager.startBackgroundTask();        // Run the background task to take care of our Wifi
  WifiManager.fallbackToSoftAp(true);       // Run a SoftAP if no known AP can be reached
  WifiManager.attachWebServer(&webServer);  // Attach our API to the Webserver 

  // Configure the default AP for the WiFi Manager
  WifiManager.addWifi("GensetControlPanel", "Auralis", false);
}

// Start the generator by turning on the K1 relay for the configured duration
void startGenerator() {
  logMessage("Starting generator...");
  digitalWrite(RELAY_K1, HIGH); // Turn on K1 relay
  delay(powerUpDuration);       // Wait for the configured duration
  digitalWrite(RELAY_K1, LOW);  // Turn off K1 relay
  logMessage("Generator started");
}

// Stop the generator by turning on the K2 relay for the configured duration
void stopGenerator() {
  logMessage("Stopping generator...");
  digitalWrite(RELAY_K2, HIGH); // Turn on K2 relay
  delay(powerDownDuration);     // Wait for the configured duration
  digitalWrite(RELAY_K2, LOW);  // Turn off K2 relay
  logMessage("Generator stopped");
}

// Setup web server
void setupWebServer() {
  // Main control page
  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    String html = "<html><body>";
    html += "<h1>ESP32 Control Panel</h1>";
    html += "<h2>Controls</h2>";
    html += "<button onclick=\"fetch('/start').then(() => location.reload())\">Start Generator</button>";
    html += "<button onclick=\"fetch('/stop').then(() => location.reload())\">Stop Generator</button>";
    html += "<h2>Log</h2><pre>";

    // Display log entries
    for (const auto& logEntry : logBuffer) {
      html += logEntry + "\n";
    }

    html += "</pre></body></html>";
    request->send(200, "text/html", html);
  });

  // Start Generator action
  webServer.on("/start", HTTP_GET, [](AsyncWebServerRequest* request) {
    logMessage("Start Generator button clicked");
    startGenerator();  // Call the start generator function
    request->send(200, "text/plain", "Start command received");
  });

  // Stop Generator action
  webServer.on("/stop", HTTP_GET, [](AsyncWebServerRequest* request) {
    logMessage("Stop Generator button clicked");
    stopGenerator();  // Call the stop generator function
    request->send(200, "text/plain", "Stop command received");
  });

  webServer.onNotFound([&](AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
  });

  webServer.begin();
  logMessage("Web server started");
}

void setup() {
  // Initialize serial monitor
  Serial.begin(115200);
  
  // Configure pins
  pinMode(RELAY_K1, OUTPUT);
  pinMode(RELAY_K2, OUTPUT);
  pinMode(LED, OUTPUT);
  pinMode(START_SIGNAL, INPUT_PULLUP);
  pinMode(STOP_SIGNAL, INPUT_PULLUP);

  // Initialize all relays and LED
  digitalWrite(RELAY_K1, LOW);
  digitalWrite(RELAY_K2, LOW);
  digitalWrite(LED, LOW);

  // Boot sequence
  logMessage("Booting...");
  blinkLED(2, 500);

  // Start WiFi Manager
  setupWiFi();

  // Start the web server
  setupWebServer();
}

void loop() {
  static bool lastStartState = HIGH; // Initial state for START signal
  static bool lastStopState = HIGH;  // Initial state for STOP signal

  bool currentStartState = digitalRead(START_SIGNAL);
  bool currentStopState = digitalRead(STOP_SIGNAL);

  // POWER-UP: Detect START signal transition from HIGH to LOW
  if (lastStartState == HIGH && currentStartState == LOW) {
    startGenerator();
  }

  // POWER-DOWN: Detect START signal transition from LOW to HIGH
  if (lastStartState == LOW && currentStartState == HIGH) {
    stopGenerator();
  }

  // Update the last state of START and STOP signals
  lastStartState = currentStartState;
  lastStopState = currentStopState;

  // Small delay for debouncing
  delay(50);
}

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <wifimanager.h>
#include <deque>
#include <string>
#include <ReactESP.h>
#include <LittleFS.h>

// Pin definitions
#define RELAY_K1 16
#define RELAY_K2 17
#define LED 23
#define RUNNING_SIGNAL 25
#define START_SIGNAL 26
#define STOP_SIGNAL 27

// Create the WiFi Manager instance
WIFIMANAGER WifiManager;

// Configurable durations (default values)
// defines how long the Relay should be turned on
unsigned long powerUpDuration = 10000;  // 10 seconds
unsigned long powerDownDuration = 10000; // 10 seconds

// Debounce variables to prevent false transition to start
const unsigned int debounceCount = 5;  // Number of signals to ignore between transitions
unsigned long debounceStart = 0; // Start time of the debounce interval

// Web server
AsyncWebServer webServer(80);

// Variables for state tracking
bool lastStartState = LOW; // Initial state for START signal - request to start up the Generator
bool lastStopState = LOW;  // Initial state for STOP signal - request to stop the Generator
bool runningState = LOW;   // Initial state for RUNNING signal - status if the Generator is running

// Define maximum number of log entries
const size_t LOG_BUFFER_MAX_SIZE = 100;

// Use a deque to store log entries
std::deque<String> logBuffer;

// ReactESP event loop
using namespace reactesp;
EventLoop event_loop;


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

// WiFi connection setup
void setupWiFi() {
  logMessage("Starting WiFi Manager...");

  // Configure the default AP for the WiFi Manager
  WifiManager.addWifi("Verges", "WeAreFamily");

  WifiManager.startBackgroundTask();        // Run the background task to take care of our Wifi
  WifiManager.fallbackToSoftAp(true);       // Run a SoftAP if no known AP can be reached
  WifiManager.attachWebServer(&webServer);  // Attach our API to the Webserver 
}

// Start the generator by turning on the K1 relay for the configured duration
void startGenerator() {
  bool currentStopState = digitalRead(RELAY_K2);
  if (currentStopState == HIGH) {
    logMessage("Generator is currently shutting down. Ignoring START signal");
    return;
  }
  logMessage("Starting generator...");
  digitalWrite(RELAY_K1, HIGH); // Turn on K1 relay
  event_loop.onDelay(powerUpDuration, []() {
    digitalWrite(RELAY_K1, LOW);  // Turn off K1 relay
    logMessage("Generator started");
  });
}

// Stop the generator by turning on the K2 relay for the configured duration
void stopGenerator() {
  logMessage("Stopping generator...");
  digitalWrite(RELAY_K2, HIGH); // Turn on K2 relayy
  digitalWrite(RELAY_K1, LOW);  // Turn off K1 relay (in case it was on)
  event_loop.onDelay(powerDownDuration, []() {
    digitalWrite(RELAY_K2, LOW);  // Turn off K2 relay
    logMessage("Generator stopped");
  });
}

// Setup web server
void setupWebServer() {
  // Main control page
  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    String html = R"html(
<!DOCTYPE html>
<html lang="de">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Scrollbare Log-Textbox</title>
    <style>
        body {
            font-family: Arial, sans-serif;
            margin: 20px;
        }
        .log-box {
            width: 100%;
            max-width: 600px;
            height: 300px;
            border: 1px solid #ccc;
            border-radius: 5px;
            padding: 10px;
            background-color: #f9f9f9;
            overflow-y: scroll;
            font-family: monospace;
            white-space: pre-wrap;
        }
    </style>
</head>
<body>
  <h1>ESP32 Control Panel</h1>
  <h2>Controls</h2>
  <button onclick="fetch('/start').then(() => location.reload())">Start Generator</button>
  <button onclick="fetch('/stop').then(() => location.reload())">Stop Generator</button>
  <h2>Log</h2>
  <div class="log-box" id="logBox">loading...</div>
  <script>
    function updateLogBox() {
      fetch('/log')
        .then(response => response.text())
        .then(data => {
          document.getElementById('logBox').innerHTML = data;
        });
    }
    setInterval(updateLogBox, 1000);
  </script>
</body>
</html>
)html";
    request->send(200, "text/html", html);
  });

  webServer.on("/log", HTTP_GET, [](AsyncWebServerRequest* request) {
    String html = "";
    // Display log entries
    for (const auto& logEntry : logBuffer) {
      html += logEntry + "\n";
    }
    request->send(200, "text/plain", html);
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

// Check for transitions on the START and STOP signals to control the generator.
//
// This function is meant to be called frequently, such as in loop().
//
// The function detects the following signal transitions:
//   - POWER-UP: START signal transition from LOW to HIGH
//   - POWER-DOWN: START signal transition from HIGH to LOW
//
// When a transition is detected, the corresponding function is called:
//   - POWER-UP: startGenerator()
//   - POWER-DOWN: stopGenerator()
//
// The last state of the START and STOP signals is stored in the variables
// lastStartState and lastStopState, respectively.
void checkForSignals() {
  bool currentStartState = digitalRead(START_SIGNAL);
  bool currentStopState = digitalRead(STOP_SIGNAL);

  if (currentStopState == HIGH) {
    if (currentStartState == HIGH) {
      logMessage("Generator stopped by priority STOP signal, ignoring START signal");
      return;
    }
  }

  if (lastStartState == HIGH && currentStartState == LOW) { 
    // POWER-DOWN: Detect START signal transition from HIGH to LOW
    stopGenerator();
    lastStartState = currentStartState;

  } else if (lastStartState == LOW && currentStartState == HIGH) { 
    // POWER-UP: Detect START signal transition from LOW to HIGH
    if (debounceStart < debounceCount) {
      debounceStart++;
      return;
    }
    startGenerator();
    lastStartState = currentStartState;
  }
  if(debounceStart) debounceStart = 0;
}

void IRAM_ATTR receiveRunningSignal() {
  runningState = digitalRead(RUNNING_SIGNAL);
  logMessage("Current running signal: " + String(runningState));
}

void setup() {
  // Initialize serial monitor
  Serial.begin(115200);
  
  // Initialize all relays and LED
  digitalWrite(RELAY_K1, LOW);
  digitalWrite(RELAY_K2, LOW);
  digitalWrite(LED, LOW);

  // Configure pins
  pinMode(RELAY_K1, OUTPUT);
  pinMode(RELAY_K2, OUTPUT);
  pinMode(LED, OUTPUT);
  pinMode(START_SIGNAL, INPUT_PULLDOWN);
  pinMode(STOP_SIGNAL, INPUT_PULLDOWN);
  pinMode(RUNNING_SIGNAL, INPUT_PULLDOWN);

  attachInterrupt(RUNNING_SIGNAL, receiveRunningSignal, CHANGE);

  // Make sure we can persist the configuration into the NVS (Non Volatile Storage)
  if (!LittleFS.begin(true)) {
    Serial.println("[ERROR] Unable to open spiffs partition or run LittleFS");
    ESP.deepSleep(15 * 1000 * 1000); // 15 seconds deepSleep then retry
  }

  // Boot sequence
  logMessage("Booting...");
  for (int i = 0; i < 6; i++) {
    auto delay = 25 + i * 250;
    event_loop.onDelay(delay, []() { 
      if(digitalRead(LED) == LOW) {
        digitalWrite(LED, HIGH); 
      } else {
        digitalWrite(LED, LOW); 
      }
    });
  }

  // Start WiFi Manager
  setupWiFi();

  // Start the web server
  setupWebServer();

  // Check for START/STOP signals every 50ms
  event_loop.onDelay(5, receiveRunningSignal);
  event_loop.onRepeat(50, checkForSignals);
}

void loop() {
  event_loop.tick();
}

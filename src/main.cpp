/**
 * Genset control
 * (c) 2024 Martin Verges
 *
 * Licensed under CC BY-NC-SA 4.0
 * (Attribution-NonCommercial-ShareAlike 4.0 International)
**/
#if !(defined(AUTO_FW_VERSION))
  #define AUTO_FW_VERSION "v0.0.0-00000000"
#endif
#if !(defined(AUTO_FW_DATE))
  #define AUTO_FW_DATE "2024-01-01"
#endif

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <wifimanager.h>
#include <deque>
#include <string>
#include <ReactESP.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <otaWebUpdater.h>

// #include <ModbusMaster.h>

// Pin definitions
#define RELAY_K1 16
#define RELAY_K2 17
#define LED 23
#define RUNNING_SIGNAL 25
#define START_SIGNAL 26
#define STOP_SIGNAL 27
// #define MODBUS_ENABLED true
// #define MODBUS_TX 32 // GPIO pin for MODBUS TX
// #define MODBUS_RX 33 // GPIO pin for MODBUS RX
// #define MODBUS_BAUDRATE 19200 // https://www.ccontrols.com/support/dp/modbus2300.pdf

// Predefined Settings
const char* MDNS_NAME = "genset-control";         // Name used for mDNS
const char* NVS_GENSET_CONTROL = "Genset";        // Name of the NVS namespace
const char* WIFI_SOFTAP_SSID = "Genset Control";  // Default name of the SoftAP
const char* WIFI_SOFTAP_PASS = "";                // Default password of the SoftAP

void logMessage(const String& message);

// Create the WiFi Manager instance
class MyWifiManager : public WIFIMANAGER {
protected:
    void logMessage(String message) override {
        ::logMessage(message);
    }
};
MyWifiManager WifiManager;

// OTA Update Managers
// Create the WiFi Manager instance
class MyOtaWebUpdater : public OtaWebUpdater {
protected:
    void logMessage(String message) override {
        ::logMessage(message);
    }
};
MyOtaWebUpdater otaWebUpdater;

// Create the NVS instance
Preferences preferences;

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
bool ledState = LOW;       // Initial state for LED
bool allowStart = true;    // Allow the generator to start

// Define maximum number of log entries
const size_t LOG_BUFFER_MAX_SIZE = 100;

// Use a deque to store log entries
std::deque<String> logBuffer;

// ReactESP event loop
using namespace reactesp;
EventLoop event_loop;

// MODBUS configuration and data structure
// struct CumminsOnanData {
//     uint16_t engine_hours;
//     uint16_t battery_voltage;
//     uint16_t engine_rpm;
//     uint16_t generator_load;
// };
// CumminsOnanData gensetData;
// ModbusMaster modbus;


/**
 * Polls the MODBUS data from the generator.
 *
 * The function connects to the serial interface, sends a request to read
 * the holding registers starting at address 0x1000, and reads the response
 * into the gensetData struct.
 *
 * The function is called every 1000ms to poll the MODBUS data.
 */
/*void queryModbus() {
    modbus.begin(&Serial1, MODBUS_BAUDRATE, SERIAL_8N1);
    modbus.readHoldingRegisters(1, 0x1000, 4); // Device ID 1, start address 0x1000, read 4 registers
    gensetData.engine_hours = modbus.getResponseBuffer(0);
    gensetData.battery_voltage = modbus.getResponseBuffer(1);
    gensetData.engine_rpm = modbus.getResponseBuffer(2);
    gensetData.generator_load = modbus.getResponseBuffer(3);
}*/

// Function to log messages
void logMessage(const String& msg) {
  // remove unnecessary newlines
  auto message = msg.endsWith("\n") ? msg.substring(0, msg.length() - 1) : msg;

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
  logMessage("[WIFI] Starting WiFi Manager...");

  WifiManager.configueSoftAp(WIFI_SOFTAP_SSID, WIFI_SOFTAP_PASS);
  WifiManager.fallbackToSoftAp(true);       // Run a SoftAP if no known AP can be reached

  WifiManager.startBackgroundTask();        // Run the background task to take care of our Wifi
  WifiManager.attachWebServer(&webServer);  // Attach our API to the Webserver

  logMessage("[mDNS] Starting mDNS...");
  MDNS.begin(MDNS_NAME);
  MDNS.addService("http", "tcp", 80);
}

// Set whether the generator is allowed to start.
//
// This setting is stored in the non-volatile storage (NVS)
//
// @param state Whether the generator is allowed to start.
// @return true if the setting was successfully written to NVS.
bool setAllowStart(bool state) {
  if (preferences.begin(NVS_GENSET_CONTROL, false)) {
    bool success = preferences.putBool("allowStart", state);
    logMessage("[NVS] Start allowance set to " + String(state));
    allowStart = state;
    preferences.end();
    return success;
  } else {
    return false;
  }
}

  /**
   * Gets whether the generator is allowed to start from NVS, setting the 
   * global allowStart variable to the result and returning it.
   *
   * @return true if the generator is allowed to start, false otherwise
   */
bool getAllowStart() {
  if (preferences.begin(NVS_GENSET_CONTROL, true)) {
    allowStart = preferences.getBool("allowStart", true);
    logMessage("[NVS] Loaded start allowance from NVS: " + String(allowStart));
    preferences.end();
    return allowStart;
  } else {
    return false;
  }
}


// Start the generator by turning on the K1 relay for the configured duration
void startGenerator() {
  if (allowStart == false) {
    logMessage("[CONTROL] Generator is not allowed to start. Ignoring START signal");
    return;
  }
  bool currentStopState = digitalRead(RELAY_K2);
  if (currentStopState == HIGH) {
    logMessage("[CONTROL] Generator is currently shutting down. Ignoring START signal");
    return;
  }
  logMessage("[CONTROL] Starting generator...");
  digitalWrite(RELAY_K1, HIGH); // Turn on K1 relay
  event_loop.onDelay(powerUpDuration, []() {
    digitalWrite(RELAY_K1, LOW);  // Turn off K1 relay
    logMessage("[CONTROL] Generator started");
  });
  digitalWrite(LED, HIGH);
  event_loop.onDelay(2500, []() { digitalWrite(LED, LOW); });
}

// Stop the generator by turning on the K2 relay for the configured duration
void stopGenerator() {
  logMessage("[CONTROL] Stopping generator...");
  digitalWrite(RELAY_K2, HIGH); // Turn on K2 relayy
  digitalWrite(RELAY_K1, LOW);  // Turn off K1 relay (in case it was on)
  event_loop.onDelay(powerDownDuration, []() {
    digitalWrite(RELAY_K2, LOW);  // Turn off K2 relay
    logMessage("[CONTROL] Generator stopped");
  });
  digitalWrite(LED, HIGH);
  event_loop.onDelay(2500, []() { digitalWrite(LED, LOW); });
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
    <title>Genset Control</title>
    <style>
      body {
        font-family: Arial, sans-serif;
        margin: 20px;
      }
      .logbox {
        width: 100%;
        max-width: 900px;
        height: 300px;
        border: 1px solid #ccc;
        border-radius: 5px;
        padding: 10px;
        background: #f9f9f9;
        overflow-y: auto;
        font-family: monospace;
        white-space: pre-wrap;
      }
      button {
        background: #4CAF50;
        color: #fff;
        border: none;
        border-radius: 4px;
        padding: 10px 20px;
        font-size: 16px;
        cursor: pointer;
        transition: 0.3s;
        box-shadow: 0 4px 6px rgba(0, 0, 0, 0.1);
      }
      button:hover {
        background: #45a049;
        box-shadow: 0 6px 10px rgba(0, 0, 0, 0.15);
        transform: translateY(-2px);
      }
      button:disabled, button[disabled] {
        background-color: #cccccc;
        color: #666666;
      }
      .red {
        background: #f44336;
      }
      .red:hover {
        background: #e53935;
      }
    </style>
</head>
<body>
  <h1>Genset Control</h1>
  <h2>Controls</h2>
)html";
    if (!allowStart) {
      html += R"html(
  <button disabled>Start Generator</button>
  <button disabled>Stop Generator</button>
  <h2>Settings</h2>
  <button onclick="fetch('/allowStart').then(() => location.reload())">Startup disabled<br>click to enable</button>
)html";
    } else {
      html += R"html(
  <button onclick="fetch('/start').then(() => location.reload())">Start Generator</button>
  <button onclick="fetch('/stop').then(() => location.reload())">Stop Generator</button>
  <h2>Settings</h2>
  <button class="red" onclick="fetch('/disallowStart').then(() => location.reload())">Startup enabled<br>click to disable</button>
)html";
    }
    html += R"html(
  <h2>Log</h2>
  <div class="logbox" id="logBox">loading...</div>
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

  webServer.on("/allowStart", HTTP_GET, [](AsyncWebServerRequest* request) {
    setAllowStart(true);
    request->send(200, "text/plain", "Startup enabled");
  });

  webServer.on("/disallowStart", HTTP_GET, [](AsyncWebServerRequest* request) {
    setAllowStart(false);
    stopGenerator();
    request->send(200, "text/plain", "Startup disabled");
  });

  webServer.on("/log", HTTP_GET, [](AsyncWebServerRequest* request) {
    String html = "";
    // Display log entries
    //for (const auto& logEntry : logBuffer) {
    //  html += logEntry + "\n";
    //}
    for (auto it = logBuffer.rbegin(); it != logBuffer.rend(); ++it) {
        html += *it + "\n";
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

  webServer.onNotFound([](AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
  });

  webServer.begin();
  logMessage("[STATUS] Web server started");
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

/**
 * Interrupt service routine to read the current state of the RUNNING signal
 * and log the state. Updates the runningState variable with the current
 * digital reading from the RUNNING_SIGNAL pin.
 */
void IRAM_ATTR receiveRunningSignal() {
  runningState = digitalRead(RUNNING_SIGNAL);
  if (runningState == HIGH) {
    logMessage("[SIGNAL] Genset is running - signal HIGH");
  } else {
    logMessage("[SIGNAL] Genset is not running - signal HIGH");
  }
}

// Interrupt service routine to read the current state of the LED and log it.
void IRAM_ATTR receiveLEDStatus() {
  ledState = digitalRead(LED);
  logMessage("[LED] Current state: " + String(ledState));
}

void setup() {
  // Initialize serial monitor
  Serial.begin(115200);
  logMessage("\n\n==== starting ESP32 setup() ====");
  logMessage("Firmware build date: " + String(__DATE__) + " " + String(__TIME__));
  logMessage("Firmware Version: " + String(AUTO_FW_VERSION) + " (" + String(AUTO_FW_DATE) + ")");
  logMessage("[STATUS] Initializing...");
  
  // Configure pins
  pinMode(RELAY_K1, OUTPUT);
  pinMode(RELAY_K2, OUTPUT);
  pinMode(LED, OUTPUT);
  pinMode(START_SIGNAL, INPUT_PULLDOWN);
  pinMode(STOP_SIGNAL, INPUT_PULLDOWN);
  pinMode(RUNNING_SIGNAL, INPUT_PULLDOWN);

  // Initialize all relays and LED
  digitalWrite(RELAY_K1, LOW);
  digitalWrite(RELAY_K2, LOW);
  digitalWrite(LED, HIGH);

  attachInterrupt(RUNNING_SIGNAL, receiveRunningSignal, CHANGE);
  attachInterrupt(LED, receiveLEDStatus, CHANGE);

  logMessage("[STATUS] Booting...");
  
  // Start WiFi Manager
  setupWiFi();

  // Start the web server
  setupWebServer();

  otaWebUpdater.setFirmware(AUTO_FW_DATE, AUTO_FW_VERSION);
  otaWebUpdater.startBackgroundTask();
  otaWebUpdater.attachWebServer(&webServer);

  // Load allowStart from NVS
  allowStart = getAllowStart();

  // Initialize the MODBUS connection
//   if (MODBUS_ENABLED) {
//     Serial1.begin(MODBUS_BAUDRATE, SERIAL_8N1, MODBUS_RX, MODBUS_TX);
//     logMessage("[MODBUS] Initialized MODBUS connection");

    // Poll MODBUS data every 1000ms
//     event_loop.onRepeat(1000, queryModbus);
//   }

  // Check for START/STOP signals every 50ms
  event_loop.onDelay(5, receiveRunningSignal);
  event_loop.onRepeat(50, checkForSignals);
  
  // Boot sequence, blinking the LED 3 times
  for (int i = 0; i < 6; i++) {
    auto delay = 100 + i * 500;
    event_loop.onDelay(delay, []() { 
      if(digitalRead(LED) == LOW) {
        digitalWrite(LED, HIGH); 
      } else {
        digitalWrite(LED, LOW); 
      }
    });
  }
}

void loop() {
  // Do not continue regular operation as long as a OTA is running
  // Reason: Background workload can cause upgrade issues that we want to avoid!
  if (otaWebUpdater.otaIsRunning) { yield(); delay(50); return; };

  event_loop.tick();
}

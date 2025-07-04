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
#include <mdns.h>
#include <esp_event.h>
#include <ReactESP.h>
#include <Preferences.h>
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
const char* OTA_BASE_URL = "";                    // Base URL for OTA updates (if empty, OTA updates are disabled)

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
class MyOtaWebUpdater : public OTAWEBUPDATER {
protected:
    void logMessage(String message) override {
        ::logMessage(message);
    }
};
MyOtaWebUpdater* otaWebUpdater = nullptr;

// Create the NVS instance
Preferences preferences;

// Configurable durations (default values)
// defines how long the Relay should be turned on
uint32_t powerUpDuration = 10000;  // 10 seconds
uint32_t powerDownDuration = 10000; // 10 seconds

uint32_t retryStartCount = 0;  // Amount of retries since the last state transition

// Web server
AsyncWebServer webServer(80);

// Variables for state tracking
bool lastStartState = LOW; // START signal - request to start up the Generator
bool lastStopState = LOW;  // STOP signal - request to stop the Generator
bool runningState = LOW;   // RUNNING signal - status if the Generator is running
bool ledState = LOW;       // State of the LED
bool allowStart = true;    // Allow the generator to start
uint8_t retryCount = 1;    // Retry count

volatile bool runningSignalChanged = false;
bool generatorStopping = false;
bool generatorStarting = false;

// Define maximum number of log entries
const uint16_t LOG_BUFFER_MAX_SIZE = 100;

// Use a deque to store log entries
std::deque<String> logBuffer;

// ReactESP event loop
using namespace reactesp;
EventLoop event_loop;

// Functions
void logMessage(const String& msg);
void setupWiFi();
bool setPowerUpDuration(uint32_t duration);
uint32_t getPowerUpDuration();
bool setPowerDownDuration(uint32_t duration);
uint32_t getPowerDownDuration();
bool setAllowStart(bool state);
bool getAllowStart();
bool setRetryCount(uint8_t count);
uint8_t getRetryCount();
void checkGeneratorStateAndRetry();
void startGenerator();
void stopGenerator();
void setupWebServer();
void checkForSignals();
void IRAM_ATTR receiveRunningSignal();
void IRAM_ATTR receiveLEDStatus();
void setup();
void loop();

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

  WiFi.onEvent(
    [&](WiFiEvent_t event, WiFiEventInfo_t info) {
      // Wifi connected and got an IP address
      if (mdns_init() == ESP_OK) {
        logMessage("[mDNS] Starting mDNS for '" + String(MDNS_NAME) + ".local'...");
        if (mdns_hostname_set(MDNS_NAME) != ESP_OK) logMessage("[mDNS] Failed to set hostname!");
        if (mdns_service_exists("_http", "_tcp", NULL) == false) {
          if (mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0) != ESP_OK) {
            logMessage("[mDNS] Failed to add service!");
          }
        }
      } else logMessage("[mDNS] Failed to start mDNS!");
    },
    WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP
  );
  WiFi.onEvent(
    [&](WiFiEvent_t event, WiFiEventInfo_t info) {
      // Wifi disconnected
      // mdns_service_remove("_http", "_tcp");
      logMessage("[mDNS] Stopping mDNS...");
      mdns_service_remove_all();
      mdns_free();
    },
    WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED
  );

  WifiManager.configueSoftAp(WIFI_SOFTAP_SSID, WIFI_SOFTAP_PASS);
  WifiManager.fallbackToSoftAp(true);       // Run a SoftAP if no known AP can be reached

  WifiManager.startBackgroundTask();        // Run the background task to take care of our Wifi
  WifiManager.attachWebServer(&webServer);  // Attach our API to the Webserver
  WifiManager.attachUI();                   // Attach the UI to the Webserver
}

/**
 * Sets the power-up duration for the generator.
 *
 * Stores the specified duration in non-volatile storage (NVS) under the 
 * "powerUpDuration" key and logs the operation.
 *
 * @param duration The duration in milliseconds for which the K1 relay should be turned on.
 * @return true if the duration was successfully written to NVS, false otherwise.
 */
bool setPowerUpDuration(uint32_t duration) {
  if (preferences.begin(NVS_GENSET_CONTROL, false)) {
    bool success = preferences.putUInt("powerUpDuration", duration);
    powerUpDuration = duration;  // Move this BEFORE return
    logMessage("[NVS] Power up duration set to " + String(duration));
    preferences.end();
    return success;
  } else {
    return false;
  }
}
/**
 * Retrieves the power-up duration from non-volatile storage (NVS).
 *
 * This function accesses the NVS to obtain the stored duration for which the K1 relay should be 
 * activated to start the generator. If no value has been stored previously, it returns the default
 * power-up duration defined in the global variable `powerUpDuration`.
 *
 * @return The duration in milliseconds for which the K1 relay is to be turned on, or 0 if the 
 *         NVS could not be accessed.
 */
uint32_t getPowerUpDuration() {
  if (preferences.begin(NVS_GENSET_CONTROL, true)) {
    powerUpDuration = preferences.getUInt("powerUpDuration", powerUpDuration);
    logMessage("[NVS] Loaded power up duration from NVS: " + String(powerUpDuration));
    preferences.end();
  }
  return powerUpDuration;  // Always return the value
}

/**
 * Sets the power-down duration for the generator.
 *
 * Stores the specified duration in non-volatile storage (NVS) under the
 * "powerDownDuration" key and logs the operation.
 *
 * @param duration The duration in milliseconds for which the K2 relay should be turned on.
 * @return true if the duration was successfully written to NVS, false otherwise.
 */
bool setPowerDownDuration(uint32_t duration) {
  if (preferences.begin(NVS_GENSET_CONTROL, false)) {
    bool success = preferences.putUInt("powerDownDuration", duration);
    logMessage("[NVS] Power down duration set to " + String(duration));
    preferences.end();
    return success;
  } else {
    return false;
  }
  powerDownDuration = duration;
}

/**
 * Retrieves the power-down duration from non-volatile storage (NVS).
 *
 * This function accesses the NVS to obtain the stored duration for which the K2 relay should be 
 * activated to stop the generator. If no value has been stored previously, it returns the default
 * power-down duration defined in the global variable `powerDownDuration`.
 *
 * @return The duration in milliseconds for which the K2 relay is to be turned on, or 0 if the 
 *         NVS could not be accessed.
 */
uint32_t getPowerDownDuration() {
  if (preferences.begin(NVS_GENSET_CONTROL, true)) {
    uint32_t duration = preferences.getUInt("powerDownDuration", powerDownDuration);
    logMessage("[NVS] Loaded power down duration from NVS: " + String(duration));
    preferences.end();
    return duration;
  } else {
    return 0;
  }
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

  /**
   * Sets the retry count of the generator to the given value.
   *
   * The retry count is the number of times the generator will be restarted after
   * a failure before giving up. This value is stored in the non-volatile storage
   * (NVS) and can be retrieved with getRetryCount.
   *
   * @param count The number of times the generator should be restarted before giving up.
   * @return true if the setting was successfully written to NVS.
   */
bool setRetryCount(uint8_t count) {
  if (preferences.begin(NVS_GENSET_CONTROL, false)) {
    bool success = preferences.putUInt("retryCount", count);
    logMessage("[NVS] Retry count set to " + String(count));
    retryCount = count;
    preferences.end();
    return success;
  } else {
    return false;
  }
}

  /**
   * Gets the retry count from NVS, setting the global retryCount variable to the result
   * and returning it.
   *
   * The retry count is the number of times the generator will be restarted after
   * a failure before giving up. This value can be changed with setRetryCount.
   *
   * @return The number of times the generator should be restarted before giving up.
   */
uint8_t getRetryCount() {
  if (preferences.begin(NVS_GENSET_CONTROL, true)) {
    retryCount = (uint8_t)preferences.getUInt("retryCount", 3);
    logMessage("[NVS] Loaded retry count from NVS: " + String(retryCount));
    preferences.end();
    return retryCount;
  } else {
    return 0;
  }
}

void checkGeneratorStateAndRetry() {
  if (allowStart && runningState == LOW && lastStartState == HIGH) {
    // Generator should be running, but it's not. Retry until retryCount is reached
    if (retryStartCount < retryCount) {
      retryStartCount++;
      logMessage("[CONTROL] Generator is not running. Retrying... (" + String(retryStartCount) + "/" + String(retryCount) + ")");
      startGenerator();

      // Retry if the generator is not running
      event_loop.onDelay(15000, checkGeneratorStateAndRetry);
    }
  }
}

// Start the generator by turning on the K1 relay for the configured duration
void startGenerator() {
  if (allowStart == false) {
    logMessage("[CONTROL] Generator is not allowed to start. Ignoring START signal");
    return;
  }
  
  // Prevent starting while stopping
  if (generatorStopping) {
    logMessage("[CONTROL] Generator is currently shutting down. Ignoring START signal");
    return;
  }
  
  // Prevent multiple start operations
  if (generatorStarting) {
    logMessage("[CONTROL] Generator start already in progress, ignoring duplicate request");
    return;
  }
    
  generatorStarting = true;
  logMessage("[CONTROL] Starting generator...");
  digitalWrite(RELAY_K1, HIGH); // Turn on K1 relay
  
  event_loop.onDelay(powerUpDuration, []() {
    digitalWrite(RELAY_K1, LOW);  // Turn off K1 relay
    logMessage("[CONTROL] Generator started");
    generatorStarting = false;  // Reset flag after completion
  });

  // Retry if the generator is not running
  event_loop.onDelay(15000, checkGeneratorStateAndRetry);

  digitalWrite(LED, HIGH);
  event_loop.onDelay(2500, []() { digitalWrite(LED, LOW); });
}

// Stop the generator by turning on the K2 relay for the configured duration
void stopGenerator() {
  // Prevent multiple stop operations
  if (generatorStopping) {
    logMessage("[CONTROL] Generator stop already in progress, ignoring duplicate request");
    return;
  }

  // Cancel any pending start operations
  if (generatorStarting) {
    generatorStarting = false;
    digitalWrite(RELAY_K1, LOW);  // Ensure K1 is off
  }
  
  generatorStopping = true;
  logMessage("[CONTROL] Stopping generator...");
  digitalWrite(RELAY_K2, HIGH); // Turn on K2 relay
  digitalWrite(RELAY_K1, LOW);  // Turn off K1 relay (in case it was on)
  
  event_loop.onDelay(powerDownDuration, []() {
    digitalWrite(RELAY_K2, LOW);  // Turn off K2 relay
    logMessage("[CONTROL] Generator stopped");
    generatorStopping = false;  // Reset flag after completion
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
        margin-top: 0.67em;
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
      input {
        margin-top: 0.67em;
        border-width: 1px;
        border-radius: 4px;
        padding: 9px;
        font-size: 16px;
        transition: 0.3s;
        box-shadow: 0 4px 6px rgba(0, 0, 0, 0.1);
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
  <button class="red" onclick="fetch('/disallowStart').then(() => location.reload())">Startup is enabled, click to disable</button>
)html";
    }
    html += R"html(
    <br>
  <input type="number" id="retryCountInput" placeholder="Retry count" value=")html" + String(retryCount)+ R"html(">
  <button onclick="fetch('/setRetryCount?count=' + document.getElementById('retryCountInput').value).then(() => location.reload())">Set retry count</button>
  <br>
  <input type="number" id="powerUpDurationInput" placeholder="Power up duration" value=")html" + String(powerUpDuration)+ R"html(">
  <button onclick="fetch('/setPowerUpDuration?duration=' + document.getElementById('powerUpDurationInput').value).then(() => location.reload())">Set power up duration</button>
  <br>
  <input type="number" id="powerDownDurationInput" placeholder="Power down duration" value=")html" + String(powerDownDuration)+ R"html(">
  <button onclick="fetch('/setPowerDownDuration?duration=' + document.getElementById('powerDownDurationInput').value).then(() => location.reload())">Set power down duration</button>
)html";
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

  webServer.on("/setRetryCount", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (!request->hasParam("count")) {
      request->send(400, "text/plain", "Missing count parameter");
      return;
    }
    int count = request->getParam("count")->value().toInt();
    if (count < 0 || count > 10) {  // Reasonable bounds
      request->send(400, "text/plain", "Count must be between 0 and 10");
      return;
    }
    retryCount = count;
    setRetryCount(retryCount);
    request->send(200, "text/plain", "Retry count set to " + String(count));
  });

  webServer.on("/setPowerUpDuration", HTTP_GET, [](AsyncWebServerRequest* request) {
    String duration = request->getParam("duration")->value();
    setPowerUpDuration(duration.toInt());
    request->send(200, "text/plain", "Power up duration set to " + duration);
  });

  webServer.on("/setPowerDownDuration", HTTP_GET, [](AsyncWebServerRequest* request) {
    String duration = request->getParam("duration")->value();
    setPowerDownDuration(duration.toInt());
    request->send(200, "text/plain", "Power down duration set to " + duration);
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

  /**
   * This function is used to debounce the RUNNING_SIGNAL pin.
   * It checks for state changes and after a short delay (DEBOUNCE_DELAY) will update the runningState variable.
   * It also logs each state change to the serial console.
   */
void checkRunningSignal() {
  static unsigned long lastChangeTime = 0;
  static bool lastReading = LOW;
  static bool stableState = LOW;
  const unsigned long DEBOUNCE_DELAY = 50;
  
  if (runningSignalChanged) {
    runningSignalChanged = false;
    unsigned long currentTime = millis();
    bool currentReading = digitalRead(RUNNING_SIGNAL);
    
    if (currentReading != lastReading) {
      lastChangeTime = currentTime;
      lastReading = currentReading;
    }
    
    if ((currentTime - lastChangeTime) > DEBOUNCE_DELAY) {
      if (stableState != lastReading) {
        stableState = lastReading;
        runningState = stableState;
        
        if (runningState == HIGH) {
          logMessage("[SIGNAL] Genset is running - signal HIGH");
        } else {
          logMessage("[SIGNAL] Genset is not running - signal LOW");
        }
      }
    }
  }
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
  // Debounce variables
  static unsigned long lastStartChangeTime = 0;
  static unsigned long lastStopChangeTime = 0;
  static bool lastStartReading = LOW;
  static bool lastStopReading = LOW;
  static bool stableStartState = LOW;
  static bool stableStopState = LOW;
  static bool initialized = false;
  const unsigned long DEBOUNCE_DELAY = 50; // ms
  
  // Initialize static variables on first run
  if (!initialized) {
    initialized = true;
    // Read current states to initialize properly
    bool currentStart = digitalRead(START_SIGNAL);
    bool currentStop = digitalRead(STOP_SIGNAL);
    
    lastStartReading = currentStart;
    lastStopReading = currentStop;
    stableStartState = currentStart;
    stableStopState = currentStop;
    
    logMessage("[INIT] checkForSignals initialized with START: " + String(currentStart) + 
               ", STOP: " + String(currentStop));
    return; // Skip first iteration
  }

  unsigned long currentTime = millis();
  bool currentStartState = digitalRead(START_SIGNAL);
  bool currentStopState = digitalRead(STOP_SIGNAL);
  
  // Debounce START signal
  if (currentStartState != lastStartReading) {
    lastStartChangeTime = currentTime;
    lastStartReading = currentStartState;
  }
  if ((currentTime - lastStartChangeTime) > DEBOUNCE_DELAY) {
    stableStartState = lastStartReading;
  }
  
  // Debounce STOP signal  
  if (currentStopState != lastStopReading) {
    lastStopChangeTime = currentTime;
    lastStopReading = currentStopState;
  }
  if ((currentTime - lastStopChangeTime) > DEBOUNCE_DELAY) {
    stableStopState = lastStopReading;
  }
  
  // Use stable states for the rest of the logic
  currentStartState = stableStartState;
  currentStopState = stableStopState;
  
  // If the STOP signal is HIGH, ignore the START signal
  if (currentStopState == HIGH && currentStartState == HIGH) {
    logMessage("[WARN] Generator stopped by priority STOP signal, ignoring simultaneous START signal");
    return;
  }

  // Detect STOP signal transition from LOW to HIGH (rising edge only)
  if (currentStopState == HIGH && lastStopState == LOW) {
    logMessage("[STATUS] STOP signal detected");
    stopGenerator();
    lastStartState = LOW;  // Reset start state when stopping
    lastStopState = currentStopState;
    return;
  }

  // Detect START signal transition from LOW to HIGH
  if (currentStartState == HIGH && lastStartState == LOW && !generatorStopping) { 
    logMessage("[STATUS] START signal detected");
    retryStartCount = 0;  // reset retry count
    startGenerator();
  }
  
  // Always update states at the end
  lastStartState = currentStartState;
  lastStopState = currentStopState;
}

/**
 * Interrupt service routine to read the current state of the RUNNING signal
 * and log the state. Updates the runningState variable with the current
 * digital reading from the RUNNING_SIGNAL pin.
 */
void IRAM_ATTR receiveRunningSignal() {
  runningSignalChanged = true;
}

// Interrupt service routine to read the current state of the LED and log it.
void IRAM_ATTR receiveLEDStatus() {
  ledState = digitalRead(LED);
}

/**
 * This function monitors the state of the LED and logs any changes.
 * It uses a static variable to track the last logged state of the LED.
 * If the current state differs from the last logged state, it updates
 * the last logged state and logs the new state to the serial console.
 */

void checkLEDStatus() {
  static bool lastLoggedLedState = LOW;
  
  if (ledState != lastLoggedLedState) {
    lastLoggedLedState = ledState;
    logMessage("[LED] Current state: " + String(ledState));
  }
}

void initializeStates() {
  // Read actual pin states with debouncing
  delay(100); // Allow pins to stabilize after boot
  
  // Read multiple times to ensure stable reading
  bool startReading = LOW;
  bool stopReading = LOW;
  bool runningReading = LOW;
  
  for (int i = 0; i < 5; i++) {
    startReading = digitalRead(START_SIGNAL);
    stopReading = digitalRead(STOP_SIGNAL);
    runningReading = digitalRead(RUNNING_SIGNAL);
    delay(10);
  }
  
  // Initialize global states to match actual pin states
  lastStartState = startReading;
  lastStopState = stopReading;
  runningState = runningReading;
  
  logMessage("[INIT] Initial states - START: " + String(lastStartState) + 
             ", STOP: " + String(lastStopState) + 
             ", RUNNING: " + String(runningState));
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

  initializeStates();

  attachInterrupt(RUNNING_SIGNAL, receiveRunningSignal, CHANGE);
  attachInterrupt(LED, receiveLEDStatus, CHANGE);

  logMessage("[STATUS] Booting...");
  
  // Start WiFi Manager
  setupWiFi();

  // Start the web server
  setupWebServer();

  otaWebUpdater = new MyOtaWebUpdater();
  otaWebUpdater->setBaseUrl(OTA_BASE_URL);
  otaWebUpdater->setFirmware(AUTO_FW_DATE, AUTO_FW_VERSION);
  otaWebUpdater->startBackgroundTask();
  otaWebUpdater->attachWebServer(&webServer);
  otaWebUpdater->attachUI();

  // Load from NVS
  allowStart = getAllowStart();
  retryCount = getRetryCount();
  powerUpDuration = getPowerUpDuration();
  powerDownDuration = getPowerDownDuration();
  
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
  event_loop.onRepeat(10, checkRunningSignal);
  event_loop.onRepeat(100, checkLEDStatus);
  
  // Boot sequence, blinking the LED 3 times
  for (uint8_t i = 0; i < 5; i++) {
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
  if (otaWebUpdater->otaIsRunning) { yield(); delay(50); return; };

  event_loop.tick();
}

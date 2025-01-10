/**
 * Wifi Manager
 * (c) 2022-2024 Martin Verges
 *
 * Licensed under CC BY-NC-SA 4.0
 * (Attribution-NonCommercial-ShareAlike 4.0 International)
**/

#include "otaWebUpdater.h"

#include <AsyncJson.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_err.h>
#include <new>          // ::operator new[]

#if OTAWEBUPDATER_USE_NVS == true
#include <Preferences.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif
uint8_t temprature_sens_read();
#ifdef __cplusplus
}
#endif
uint8_t temprature_sens_read();

/**
 * @brief Write a message to the Serial interface
 * @param msg The message to be written
 *
 * This function is a simple wrapper around Serial.print() to write a message
 * to the serial console. It can be overwritten by a custom implementation for 
 * enhanced logging.
 */
void OtaWebUpdater::logMessage(String msg) {
  Serial.print(msg);
}


/**
 * @brief Set a new baseUrl
 * @param newUrl The new baseUrl
 *
 * This function updates the baseUrl used for OTA updates. The new baseUrl is
 * stored in the NVS and will be used on the next OTA update.
 */
void OtaWebUpdater::setBaseUrl(String newUrl) {
#if OTAWEBUPDATER_USE_NVS == true
  if (preferences.begin(NVS, true)) {
    if (preferences.putString("baseUrl", newUrl)) {
      logMessage("[OTA] Updated baseUrl in NVS to " + newUrl + "\n");
    } else logMessage("[OTA] Failed to update baseUrl in NVS\n");
    preferences.end();
  }
#endif
  baseUrl = newUrl;
}

void OtaWebUpdater::setVersionCheckInterval(uint32_t minutes) {
#if OTAWEBUPDATER_USE_NVS == true
  if (preferences.begin(NVS, true)) {
    if (preferences.putULong64("VersChkIntvl", minutes * 60 * 1000)) {
      logMessage("[OTA] Updated VersionCheckInterval in NVS to " + String(minutes) + " minutes\n");
    } else logMessage("[OTA] Failed to update VersionCheckInterval in NVS\n");
    preferences.end();
  }
#endif
    intervalVersionCheckMillis = minutes * 60 * 1000;
}

void OtaWebUpdater::setOtaPassword(String newPass) {
#if OTAWEBUPDATER_USE_NVS == true
  if (preferences.begin(NVS, true)) {
    if (preferences.putString("OtaPassword", newPass)) {
      logMessage("[OTA] Updated OtaPassword in NVS to " + newPass + "\n");
    } else logMessage("[OTA] Failed to update OtaPassword in NVS\n");
    preferences.end();
  }
#endif
  otaPassword = newPass;
}


/**
 * @brief Construct a new OtaWebUpdater::OtaWebUpdater object
 *
 * @param ns The namespace for the NVS storage of the baseUrl
 *
 * This constructor initializes a new OtaWebUpdater object. It loads the baseUrl
 * from the NVS storage if OTAWEBUPDATER_USE_NVS is set to true. Then it
 * registers two WiFi event handlers to monitor network connectivity. When a
 * network connection is established, the networkReady flag is set to true. When
 * the network connection is lost, the networkReady flag is set to false.
 */
OtaWebUpdater::OtaWebUpdater(const char * ns) {

#if OTAWEBUPDATER_USE_NVS == true
  // Restore settings from NVS when loading the class
  NVS = (char *)ns;
  if (preferences.begin(NVS, true)) {
    baseUrl = preferences.getString("baseUrl", baseUrl);
    logMessage("[OTA] Loaded baseUrl from NVS: " + baseUrl);

    intervalVersionCheckMillis = preferences.getULong64("VersChkIntvl", intervalVersionCheckMillis);
    logMessage("[OTA] Loaded VersionCheckInterval from NVS: " + String(intervalVersionCheckMillis/60/1000) + " minutes");

    otaPassword = preferences.getString("OtaPassword", otaPassword);
    logMessage("[OTA] Loaded OtaPassword from NVS: " + otaPassword);
    
    preferences.end();
  }
#else
  logMessage("[OTA] NVS is not used, ignoring namespace '" + String(ns) + "' settings");
#endif

  auto data = esp_ota_get_running_partition();
  logMessage("[OTA] Running partition: " + String(data->label) + " (" + String(data->subtype) + ")");

  logMessage("[OTA] Created, registering WiFi events");
  if (WiFi.isConnected()) networkReady = true;

  auto eventHandlerUp = [&](WiFiEvent_t event, WiFiEventInfo_t info) {
    logMessage("[OTA][WIFI] onEvent() Network connected");
    networkReady = true;
  };
  WiFi.onEvent(eventHandlerUp, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.onEvent(eventHandlerUp, ARDUINO_EVENT_WIFI_STA_GOT_IP6);
  WiFi.onEvent(eventHandlerUp, ARDUINO_EVENT_ETH_GOT_IP);
  WiFi.onEvent(eventHandlerUp, ARDUINO_EVENT_ETH_GOT_IP6);

  auto eventHandlerDown = [&](WiFiEvent_t event, WiFiEventInfo_t info) {
    logMessage("[OTA][WIFI] onEvent() Network disconnected");
    networkReady = false;
  };
  WiFi.onEvent(eventHandlerUp, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.onEvent(eventHandlerUp, ARDUINO_EVENT_WIFI_AP_STADISCONNECTED);
  WiFi.onEvent(eventHandlerUp, ARDUINO_EVENT_ETH_DISCONNECTED);
}

/**
 * @brief Destroy the OtaWebUpdater::OtaWebUpdater object
 * @details will stop the background task as well but not cleanup the AsyncWebserver
 */
OtaWebUpdater::~OtaWebUpdater() {
  stopBackgroundTask();
  // FIXME: get rid of the registered Webserver AsyncCallbackWebHandlers
}

/**
 * @brief Attach to a webserver and register the API routes
 */
void OtaWebUpdater::attachWebServer(AsyncWebServer * srv) {
  webServer = srv; // store it in the class for later use

  webServer->on((apiPrefix + "/config").c_str(), HTTP_GET, [&](AsyncWebServerRequest *request) {
    String output;
    DynamicJsonDocument doc(256);
    doc["baseUrl"] = getBaseUrl();
    doc["otaPassword"] = "";
    doc["intervalVersionCheck"] = intervalVersionCheckMillis / 60 / 1000;
    serializeJson(doc, output);
    request->send(200, "application/json", output);
  });

  webServer->on((apiPrefix + "/config").c_str(), HTTP_POST, [&](AsyncWebServerRequest * request){}, NULL,
    [&](AsyncWebServerRequest * request, uint8_t *data, size_t len, size_t index, size_t total) {
    JsonDocument jsonBuffer;
    deserializeJson(jsonBuffer, (const char*)data);
    auto resp = request;
    auto changes = 0;

    if (jsonBuffer["baseUrl"].is<String>()) {
      setBaseUrl(jsonBuffer["baseUrl"].as<String>());
      logMessage("[OTA][CONFIG] baseUrl changed to " + baseUrl);
      changes++;
    }
    if (jsonBuffer["otaPassword"].is<String>()) {
      setOtaPassword(jsonBuffer["otaPassword"].as<String>());
      logMessage("[OTA][CONFIG] otaPassword changed to " + otaPassword);
      changes++;
    }
    if (jsonBuffer["intervalVersionCheck"].is<int>()) {
      setVersionCheckInterval(jsonBuffer["intervalVersionCheck"].as<int>());
      logMessage("[OTA][CONFIG] intervalVersionCheck changed to " + String(intervalVersionCheckMillis/60/1000) + " minutes");
      changes++;
    }
    
    if (!changes) {
      resp->send(422, "application/json", "{\"message\":\"Invalid data\"}");
    } else {
      resp->send(200, "application/json", "{\"message\":\"Config updated\"}");
    } 
  });

  webServer->on((apiPrefix + "/firmware/info").c_str(), HTTP_GET, [&](AsyncWebServerRequest *request) {
    auto data = esp_ota_get_running_partition();
    String output;
    DynamicJsonDocument doc(256);
    doc["partition_type"] = data->type;
    doc["partition_subtype"] = data->subtype;
    doc["address"] = data->address;
    doc["size"] = data->size;
    doc["label"] = data->label;
    doc["encrypted"] = data->encrypted;
    doc["firmware_version"] = AUTO_FW_VERSION;
    doc["firmware_date"] = AUTO_FW_DATE;
    serializeJson(doc, output);
    request->send(200, "application/json", output);
  });

  webServer->on((apiPrefix + "/partition/switch").c_str(), HTTP_POST, [&](AsyncWebServerRequest * request) {
    logMessage("[OTA] Switching boot partition");
    auto next = esp_ota_get_next_update_partition(NULL);
    auto error = esp_ota_set_boot_partition(next);
    if (error == ESP_OK) {
      logMessage("[OTA] New partition ready for boot");
      request->send(200, "application/json", "{\"message\":\"New partition ready for boot. Rebooting....\"}");
      yield();
      delay(250);

      logMessage("[OTA] Rebooting now!");
      Serial.flush();
      ESP.restart();
    } else {
      logMessage("[OTA] Error switching boot partition - " + String(esp_err_to_name(error)));
      request->send(500, "application/json", String("{\"message\":\"Error switching boot partition - ") + String(esp_err_to_name(error)) + "\"}");
    }
  });

  webServer->on((apiPrefix + "/esp").c_str(), HTTP_GET, [&](AsyncWebServerRequest * request) {
    String output;
    DynamicJsonDocument json(2048);

    JsonObject booting = json.createNestedObject("booting");
    booting["rebootReason"] = esp_reset_reason();
    booting["partitionCount"] = esp_ota_get_app_partition_count();

    auto partition = esp_ota_get_boot_partition();
    JsonObject bootPartition = json.createNestedObject("bootPartition");
    bootPartition["address"] = partition->address;
    bootPartition["size"] = partition->size;
    bootPartition["label"] = partition->label;
    bootPartition["encrypted"] = partition->encrypted;
    switch (partition->type) {
      case ESP_PARTITION_TYPE_APP:  bootPartition["type"] = "app"; break;
      case ESP_PARTITION_TYPE_DATA: bootPartition["type"] = "data"; break;
      default: bootPartition["type"] = "any";
    }
    bootPartition["subtype"] = partition->subtype;

    partition = esp_ota_get_running_partition();
    JsonObject runningPartition = json.createNestedObject("runningPartition");
    runningPartition["address"] = partition->address;
    runningPartition["size"] = partition->size;
    runningPartition["label"] = partition->label;
    runningPartition["encrypted"] = partition->encrypted;
    switch (partition->type) {
      case ESP_PARTITION_TYPE_APP:  runningPartition["type"] = "app"; break;
      case ESP_PARTITION_TYPE_DATA: runningPartition["type"] = "data"; break;
      default: runningPartition["type"] = "any";
    }
    runningPartition["subtype"] = partition->subtype;

    JsonObject build = json.createNestedObject("build");
    build["date"] = __DATE__;
    build["time"] = __TIME__;

    JsonObject ram = json.createNestedObject("ram");
    ram["heapSize"] = ESP.getHeapSize();
    ram["freeHeap"] = ESP.getFreeHeap();
    ram["usagePercent"] = (float)ESP.getFreeHeap() / (float)ESP.getHeapSize() * 100.f;
    ram["minFreeHeap"] = ESP.getMinFreeHeap();
    ram["maxAllocHeap"] = ESP.getMaxAllocHeap();

    JsonObject spi = json.createNestedObject("spi");
    spi["psramSize"] = ESP.getPsramSize();
    spi["freePsram"] = ESP.getFreePsram();
    spi["minFreePsram"] = ESP.getMinFreePsram();
    spi["maxAllocPsram"] = ESP.getMaxAllocPsram();

    JsonObject chip = json.createNestedObject("chip");
    chip["revision"] = ESP.getChipRevision();
    chip["model"] = ESP.getChipModel();
    chip["cores"] = ESP.getChipCores();
    chip["cpuFreqMHz"] = ESP.getCpuFreqMHz();
    chip["cycleCount"] = ESP.getCycleCount();
    chip["sdkVersion"] = ESP.getSdkVersion();
    chip["efuseMac"] = ESP.getEfuseMac();
    chip["temperature"] = (temprature_sens_read() - 32) / 1.8;

    JsonObject flash = json.createNestedObject("flash");
    flash["flashChipSize"] = ESP.getFlashChipSize();
    flash["flashChipRealSize"] = spi_flash_get_chip_size();
    flash["flashChipSpeedMHz"] = ESP.getFlashChipSpeed() / 1000000;
    flash["flashChipMode"] = ESP.getFlashChipMode();
    flash["sdkVersion"] = ESP.getFlashChipSize();

    JsonObject sketch = json.createNestedObject("sketch");
    sketch["size"] = ESP.getSketchSize();
    sketch["maxSize"] = ESP.getFreeSketchSpace();
    sketch["usagePercent"] = (float)ESP.getSketchSize() / (float)ESP.getFreeSketchSpace() * 100.f;
    sketch["md5"] = ESP.getSketchMD5();

    serializeJson(json, output);
    request->send(200, "application/json", output);
  });

  webServer->on((apiPrefix + "/upload").c_str(), HTTP_POST,
    [&](AsyncWebServerRequest *request) { },
    [&](AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final) {

    if (otaPassword.length()) {
      if(!request->authenticate("ota", otaPassword.c_str())) {
        logMessage("[OTA] Incorrect OTA request: Invalid password provided!");
        return request->send(401, "application/json", "{\"message\":\"Invalid OTA password provided!\"}");
      }
    } // else logMessage("[OTA] No password confirequest->authenticategured, no authentication requested!");

    if (!index) {
      otaIsRunning = true;
      logMessage("[OTA] Begin firmware update with filename: " + filename);
      // if filename includes spiffs|littlefs, update the spiffs|littlefs partition
      int cmd = (filename.indexOf("spiffs") > -1 || filename.indexOf("littlefs") > -1) ? U_SPIFFS : U_FLASH;
      if (!Update.begin(UPDATE_SIZE_UNKNOWN, cmd)) {
        logMessage("[OTA] Error: " + String(Update.errorString()));
        request->send(500, "application/json", "{\"message\":\"Unable to begin firmware update!\"}");
        otaIsRunning = false;
      }
    }

    if (Update.write(data, len) != len) {
      logMessage("[OTA] Error: " + String(Update.errorString()));
      request->send(500, "application/json", "{\"message\":\"Unable to write firmware update data!\"}");
      otaIsRunning = false;
    }

    if (final) {
      if (!Update.end(true)) {
        String output;
        DynamicJsonDocument doc(32);
        doc["message"] = "Update error";
        doc["error"] = Update.errorString();
        serializeJson(doc, output);
        request->send(500, "application/json", output);

        logMessage("[OTA] Error when calling calling Update.end().");
        logMessage("[OTA] Error: " + String(Update.errorString()));
        otaIsRunning = false;
      } else {
        logMessage("[OTA] Firmware update successful.");
        request->send(200, "application/json", "{\"message\":\"Please wait while the device reboots!\"}");
        yield();
        delay(250);

        logMessage("[OTA] Update complete, rebooting now!");
        Serial.flush();
        ESP.restart();
      }
    }
  });
}

/**
 * @brief Start a background task to regulary check for updates
 */
bool OtaWebUpdater::startBackgroundTask() {
  stopBackgroundTask();
  BaseType_t xReturned = xTaskCreatePinnedToCore(
    otaTask,
    "OtaWebUpdater",
    4000,   // Stack size in words
    this,   // Task input parameter
    0,      // Priority of the task
    &otaCheckTask,  // Task handle.
    0       // Core where the task should run
  );
  if( xReturned != pdPASS ) {
    logMessage("[OTA] Unable to run the background Task");
    return false;
  }
  return true;
}

/**
 * @brief Stops a background task if existing
 */
void OtaWebUpdater::stopBackgroundTask() {
  if (otaCheckTask != NULL) { // make sure there is no task running
    vTaskDelete(otaCheckTask);
    logMessage("[OTA] Stopped the background Task");
  }
}

/**
 * @brief Background Task running as a loop forever
 * @param param needs to be a valid OtaWebUpdater instance
 */
void otaTask(void* param) {
  yield();
  delay(1500); // Do not execute immediately
  yield();

  OtaWebUpdater * otaWebUpdater = (OtaWebUpdater *) param;
  for(;;) {
    yield();
    otaWebUpdater->loop();
    yield();
    vTaskDelay(otaWebUpdater->xDelay);
  }
}

/**
 * @brief Run our internal routine
 */
void OtaWebUpdater::loop() {
  if (newReleaseAvailable) executeUpdate();

  if (networkReady) {
    if (initialCheck) {
      if (millis() - lastVersionCheckMillis < intervalVersionCheckMillis) return;
      lastVersionCheckMillis = millis();
    } else initialCheck = true;

    if (baseUrl.isEmpty()) return;
    logMessage("[OTA] Searching a new firmware release");
    checkAvailableVersion();
  }
}

/**
 * @brief Execute the version check from the external Webserver
 * @return true if the check was successfull
 * @return false on error
 */
bool OtaWebUpdater::checkAvailableVersion() {
  if (baseUrl.isEmpty()) {
    logMessage("[OTA] No baseUrl configured");
    return false;
  }

  WiFiClient client;
  HTTPClient http;
  
  // Send request
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.useHTTP10(true);
  http.begin(client, baseUrl + "/current-version.json");
  http.GET();

  // Parse response
  DynamicJsonDocument doc(2048);
  deserializeJson(doc, http.getStream());

  // Disconnect
  http.end();

  auto date = doc["date"].as<String>();
  auto revision = doc["revision"].as<String>();

  if (date.isEmpty() || revision.isEmpty() || date == "null" || revision == "null") {
    logMessage("[OTA] Invalid response or json in " + baseUrl + "/current-version.json");
    return false;
  }
  if (date > currentFwDate) { // a newer Version is available!
    logMessage("[OTA] Newer firmware available: " + date + " vs " + currentFwDate);
    newReleaseAvailable = true;
  }
  logMessage("[OTA] No newer firmware available");
  return true;
}

/**
 * @brief Download a file from a url and execute the firmware update
 * 
 * @param baseUrl HTTPS url to download from
 * @param filename  The filename to download
 * @return true 
 * @return false 
 */
bool OtaWebUpdater::updateFile(String baseUrl, String filename) {
  if (baseUrl.isEmpty()) {
    logMessage("[OTA] No baseUrl configured");
    return false;
  }

  otaIsRunning = true;
  int filetype = (filename.indexOf("spiffs") > -1 || filename.indexOf("littlefs") > -1) ? U_SPIFFS : U_FLASH;

  String firmwareUrl = baseUrl + "/" + filename;
  WiFiClient client;
  HTTPClient http;
  
  // Reserve some memory to download the file
  auto bufferAllocationLen = 128*1024;
  uint8_t * buffer;
  try {
    buffer = new uint8_t[bufferAllocationLen];
  } catch (std::bad_alloc& ba) {
    logMessage("[OTA] Unable to request memory with malloc(" + String(bufferAllocationLen+1) + ")");
    otaIsRunning = false;
    return false;
  }
  
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.begin(client, firmwareUrl);

  logMessage("[OTA] Firmware type: " + String(filetype == U_SPIFFS ? "spiffs" : "flash"));
  logMessage("[OTA] Firmware url:  " + firmwareUrl);

  if (http.GET() == 200) {
    // get length of document (is -1 when Server sends no Content-Length header)
    auto totalLength = http.getSize();
    auto len = totalLength;
    auto currentLength = 0;

    // this is required to start firmware update process
    Update.begin(UPDATE_SIZE_UNKNOWN, filetype);
    logMessage("[OTA] Firmware size: " + String(totalLength));

    // create buffer for read
    //uint8_t buff[4096] = { 0 };
    WiFiClient * stream = http.getStreamPtr();

    // read all data from server
    logMessage("[OTA] Begin firmware upgrade...");
    while(http.connected() && (len > 0 || len == -1)) {
      // get available data size
      size_t size = stream->available();
      if(size) {
        // read up to 4096 byte
        int readBufLen = stream->readBytes(buffer, ((size > bufferAllocationLen) ? bufferAllocationLen : size));
        if(len > 0) len -= readBufLen;

        Update.write(buffer, readBufLen);
        logMessage("[OTA] Status: " + String(currentLength));

        currentLength += readBufLen;
        if(currentLength != totalLength) continue;
        // Update completed
        Update.end(true);
        http.end();
        logMessage("\n");
        logMessage("[OTA] Upgrade successfully executed. Wrote bytes: " + String(currentLength));

        otaIsRunning = false;
        delete[] buffer;
        return true;
      }
      delay(1);
    }
  }

  otaIsRunning = false;
  delete[] buffer;
  return false;
}

/**
 * @brief Execute the update with a firmware from the external Webserver
 */
void OtaWebUpdater::executeUpdate() {
  if (baseUrl.isEmpty()) {
    logMessage("[OTA] No baseUrl configured");
    return;
  }

  otaIsRunning = true;
  if (updateFile(baseUrl, "littlefs.bin") && updateFile(baseUrl, "firmware.bin") ) {
    ESP.restart();
  } else {
    otaIsRunning = false;
    logMessage("[OTA] Failed to update firmware");
  }
}

void OtaWebUpdater::attachUI() {
  webServer->on((uiPrefix).c_str(), HTTP_GET, [&](AsyncWebServerRequest* request) {
    String html = R"html(
  <!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32 OTA Updater</title>
    <style>
        :root {
            --primary-color: #2563eb;
            --success-color: #16a34a;
            --warning-color: #ca8a04;
            --error-color: #dc2626;
            --bg-color: #f8fafc;
            --card-bg: #ffffff;
            --text-color: #1e293b;
            --border-color: #e2e8f0;
        }

        body {
            font-family: system-ui, -apple-system, sans-serif;
            background: var(--bg-color);
            color: var(--text-color);
            margin: 0;
            padding: 16px;
            line-height: 1.5;
        }

        .container {
            max-width: 800px;
            margin: 0 auto;
        }

        .card {
            background: var(--card-bg);
            border-radius: 8px;
            padding: 16px;
            margin-bottom: 16px;
            box-shadow: 0 1px 3px rgba(0,0,0,0.1);
            border: 1px solid var(--border-color);
        }

        h1, h2 {
            margin: 0 0 16px 0;
            color: var(--text-color);
        }

        .upload-zone {
            border: 2px dashed var(--border-color);
            border-radius: 8px;
            padding: 32px;
            text-align: center;
            cursor: pointer;
            transition: all 0.2s;
        }

        .upload-zone:hover {
            border-color: var(--primary-color);
            background: #f8fafc;
        }

        .upload-zone.drag-over {
            border-color: var(--primary-color);
            background: #eff6ff;
        }

        button {
            background: var(--primary-color);
            color: white;
            border: none;
            padding: 8px 16px;
            border-radius: 4px;
            cursor: pointer;
            font-size: 0.875rem;
            transition: opacity 0.2s;
        }

        button:hover {
            opacity: 0.9;
        }

        button:disabled {
            opacity: 0.5;
            cursor: not-allowed;
        }

        .status {
            padding: 8px;
            border-radius: 4px;
            margin: 8px 0;
            display: none;
        }

        .status.error {
            background: #fee2e2;
            color: #991b1b;
            display: block;
        }

        .status.success {
            background: #dcfce7;
            color: #166534;
            display: block;
        }

        .status.info {
            background: #e0f2fe;
            color: #075985;
            display: block;
        }

        .grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
            gap: 16px;
            margin-top: 16px;
        }

        .stat-card {
            background: #f8fafc;
            padding: 12px;
            border-radius: 6px;
            border: 1px solid var(--border-color);
        }

        .stat-title {
            font-size: 0.875rem;
            color: #64748b;
            margin-bottom: 4px;
        }

        .stat-value {
            font-weight: 500;
        }

        .progress-bar {
            width: 100%;
            height: 4px;
            background: #e2e8f0;
            border-radius: 2px;
            overflow: hidden;
            margin-top: 16px;
        }

        .progress-bar .progress {
            width: 0%;
            height: 100%;
            background: var(--primary-color);
            transition: width 0.3s ease;
        }

        input {
            padding: 8px;
            margin: 8px 0 16px;
            border: 1px solid var(--border-color);
            border-radius: 4px;
            box-sizing: border-box;
        }

        #switchPartitionBtn {
            background: var(--warning-color);
        }

        .small-text {
            font-size: 0.875rem;
            color: #64748b;
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="card">
            <h1>ESP32 OTA Updater</h1>
            <div id="status"></div>
            
            <div id="uploadZone" class="upload-zone">
                <div>Drag and drop firmware file here or click to select</div>
                <input type="file" id="fileInput" style="display: none" accept=".bin">
                <div class="progress-bar">
                    <div class="progress" id="uploadProgress"></div>
                </div>
            </div>

            <label for="otaPassword">(Optional) OTA Password:</label>
            <input type="text" id="otaPassword" required>
        </div>

        <div class="card">
            <h2>Firmware Information</h2>
            <div id="firmwareInfo"></div>
            <button id="switchPartitionBtn" onclick="switchPartition()">Switch Active Partition</button>
        </div>

        <div class="card">
            <h2>System Information</h2>
            <div id="systemInfo" class="grid"></div>
        </div>
  )html";
  #if OTAWEBUPDATER_USE_NVS == true
  html += R"html(
        <div class="card">
            <h2>Configuration</h2>
            <div id="configuration"></div>
            <button onclick="saveConfig()">Save Settings</button>
        </div>
    )html";
  #endif
  html += R"html(
    </div>

    <script>
        const API_BASE = '/api/ota';
        
        // Initialize the page
        window.addEventListener('load', () => {
            loadFirmwareInfo();
            loadSystemInfo();
            setupFileUpload();
            if (document.getElementById('configuration')) getConfig();
        });

        async function getConfig() {
            try {
                const response = await fetch(`${API_BASE}/config`);
                const data = await response.json();

                const element = document.getElementById('configuration');
                if (element) {
                    element.innerHTML = `
                    <div class="grid">
                      <div class="stat-card">
                          <div class="stat-title">Automatic Update URL</div>
                          <div class="stat-value">
                            <input type="text" id="baseUrl" value="${data.baseUrl}">
                          </div>
                      </div>
                      <div class="stat-card">
                          <div class="stat-title">Automatic Update Interval in Minutes</div>
                          <div class="stat-value">
                            <input type="number" id="intervalVersionCheck" value="${data.intervalVersionCheck}">
                          </div>
                      </div>
                    </div>
                `;
                } // else NVS disabled
            } catch (error) {
                showStatus('Failed to load config');
            }
        }

        async function saveConfig() {
          try {
            const response = await fetch(`${API_BASE}/config`, {
              method: 'POST',
              headers: {
                'Content-Type': 'application/json'
              },
              body: JSON.stringify({
                baseUrl: document.getElementById('baseUrl').value,
                intervalVersionCheck: document.getElementById('intervalVersionCheck').value
              })
            })
          } catch (error) {
              showStatus('Failed to save config');
          }
        }

        async function loadFirmwareInfo() {
            try {
                const response = await fetch(`${API_BASE}/firmware/info`);
                const data = await response.json();
                
                document.getElementById('firmwareInfo').innerHTML = `
                    <div class="grid">
                        <div class="stat-card">
                            <div class="stat-title">Partition</div>
                            <div class="stat-value">${data.label}</div>
                        </div>
                        <div class="stat-card">
                            <div class="stat-title">Version</div>
                            <div class="stat-value">${data.firmware_version}</div>
                        </div>
                        <div class="stat-card">
                            <div class="stat-title">Build Date</div>
                            <div class="stat-value">${data.firmware_date}</div>
                        </div>
                    </div>
                `;
            } catch (error) {
                showStatus('Failed to load firmware info: ' + error.message, 'error');
            }
        }

        async function loadSystemInfo() {
            try {
                const response = await fetch(`${API_BASE}/esp`);
                const data = await response.json();
                
                const systemInfoHtml = `
                    <div class="stat-card">
                        <div class="stat-title">CPU</div>
                        <div class="stat-value">${data.chip.model}</div>
                        <div class="small-text">${data.chip.cores} cores @ ${data.chip.cpuFreqMHz}MHz</div>
                    </div>
                    <div class="stat-card">
                        <div class="stat-title">Temperature</div>
                        <div class="stat-value">${data.chip.temperature.toFixed(1)}°C</div>
                    </div>
                    <div class="stat-card">
                        <div class="stat-title">RAM Usage</div>
                        <div class="stat-value">${(100 - data.ram.usagePercent).toFixed(1)}%</div>
                        <div class="small-text">${(data.ram.freeHeap/1024).toFixed(1)}KB free of ${(data.ram.heapSize/1024).toFixed(1)}KB</div>
                    </div>
                    <div class="stat-card">
                        <div class="stat-title">Flash</div>
                        <div class="stat-value">${(data.flash.flashChipSize/1048576).toFixed(1)} MB</div>
                        <div class="small-text">${data.flash.flashChipSpeedMHz} MHz</div>
                    </div>
                    <div class="stat-card">
                        <div class="stat-title">Sketch Size</div>
                        <div class="stat-value">${(data.sketch.usagePercent).toFixed(1)}%</div>
                        <div class="small-text">${(data.sketch.size/1024).toFixed(1)}KB of ${(data.sketch.maxSize/1024).toFixed(1)}KB</div>
                    </div>
                    <div class="stat-card">
                        <div class="stat-title">Next Boot Partition</div>
                        <div class="stat-value">${data.bootPartition.label}</div>
                        <div class="small-text">${data.bootPartition.type}</div>
                    </div>
                `;

                document.getElementById('systemInfo').innerHTML = systemInfoHtml;
            } catch (error) {
                showStatus('Failed to load system info: ' + error.message, 'error');
            }
        }

        function setupFileUpload() {
            const uploadZone = document.getElementById('uploadZone');
            const fileInput = document.getElementById('fileInput');

            uploadZone.addEventListener('click', () => fileInput.click());
            
            uploadZone.addEventListener('dragover', (e) => {
                e.preventDefault();
                uploadZone.classList.add('drag-over');
            });

            uploadZone.addEventListener('dragleave', () => {
                uploadZone.classList.remove('drag-over');
            });

            uploadZone.addEventListener('drop', (e) => {
                e.preventDefault();
                uploadZone.classList.remove('drag-over');
                const file = e.dataTransfer.files[0];
                if (file) handleFile(file);
            });

            fileInput.addEventListener('change', (e) => {
                const file = e.target.files[0];
                if (file) handleFile(file);
            });
        }

        async function handleFile(file) {
            if (!file.name.endsWith('.bin')) {
                showStatus('Please select a valid firmware file (.bin)', 'error');
                return;
            }

            const formData = new FormData();
            formData.append('file', file);

            try {
                const otaPassword = document.getElementById('otaPassword').value;

                const xhr = new XMLHttpRequest();
                if (otaPassword.length) {
                  xhr.open('POST', `${API_BASE}/upload`, true, 'ota', otaPassword);
                } else {
                  xhr.open('POST', `${API_BASE}/upload`, true);
                }

                xhr.upload.onprogress = (e) => {
                    if (e.lengthComputable) {
                        const percentComplete = (e.loaded / e.total) * 100;
                        document.getElementById('uploadProgress').style.width = percentComplete + '%';
                    }
                };

                xhr.onload = function() {
                    if (xhr.status === 200) {
                        showStatus('Firmware uploaded successfully. Device will reboot...', 'success');
                        // Reset progress bar after short delay
                        setTimeout(() => {
                            document.getElementById('uploadProgress').style.width = '0%';
                        }, 2000);
                    } else {
                        showStatus('Upload failed: ' + xhr.responseText, 'error');
                    }
                };

                xhr.onerror = function() {
                    showStatus('Upload failed', 'error');
                };

                showStatus('Uploading firmware...', 'info');
                xhr.send(formData);
            } catch (error) {
                showStatus('Upload failed: ' + error.message, 'error');
            }
        }

        async function switchPartition() {
            try {
                const response = await fetch(`${API_BASE}/partition/switch`, {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json',
                    }
                });
                
                if (response.ok) {
                    const data = await response.json();
                    showStatus(data.message + ' Device will reboot...', 'success');
                    setTimeout(() => location.reload(), 5000);
                } else {
                    const json = await response.json();
                    if (json.message) {
                      throw new Error(json.message);
                    } else {
                      throw new Error('Failed to switch partition');
                    }
                }
            } catch (error) {
                showStatus('Failed to switch partition: ' + error.message, 'error');
            }
        }

        function showStatus(message, type) {
            const statusElement = document.getElementById('status');
            statusElement.innerHTML = message;
            statusElement.className = `status ${type}`;
        }
    </script>
</body>
</html>)html";

    request->send(200, "text/html", html);
  });
}
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
#include <new>          // ::operator new[]

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
 * @brief Construct a new OtaWebUpdater::OtaWebUpdater object
 */
OtaWebUpdater::OtaWebUpdater() {
  logMessage("[OTAWEBUPDATER] Created, registering WiFi events");

  if (WiFi.isConnected()) networkReady = true;

  auto eventHandlerUp = [&](WiFiEvent_t event, WiFiEventInfo_t info) {
    logMessage("[OTAWEBUPDATER][WIFI] onEvent() Network connected");
    networkReady = true;
  };
  WiFi.onEvent(eventHandlerUp, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.onEvent(eventHandlerUp, ARDUINO_EVENT_WIFI_STA_GOT_IP6);
  WiFi.onEvent(eventHandlerUp, ARDUINO_EVENT_ETH_GOT_IP);
  WiFi.onEvent(eventHandlerUp, ARDUINO_EVENT_ETH_GOT_IP6);

  auto eventHandlerDown = [&](WiFiEvent_t event, WiFiEventInfo_t info) {
    logMessage("[OTAWEBUPDATER][WIFI] onEvent() Network disconnected");
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

  webServer->on((apiPrefix + "/partition/switch").c_str(), HTTP_POST, [&](AsyncWebServerRequest * request){}, NULL,
    [&](AsyncWebServerRequest * request, uint8_t *data, size_t len, size_t index, size_t total) {
    auto next = esp_ota_get_next_update_partition(NULL);
    auto error = esp_ota_set_boot_partition(next);
    if (error == ESP_OK) {
      logMessage("[OTA] New partition ready for boot");
      request->send(200, "application/json", "{\"message\":\"New partition ready for boot\"}");
    } else {
      logMessage("[OTA] Error switching boot partition");
      request->send(500, "application/json", "{\"message\":\"Error switching boot partition\"}");
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
    } else logMessage("[OTA] No password configured, no authentication requested!");

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
    logMessage("[OTAWEBUPDATER] Unable to run the background Task");
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
    logMessage("[OTAWEBUPDATER] Stopped the background Task");
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
    logMessage("[OTAWEBUPDATER] Searching a new firmware release");
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
    logMessage("[OTAWEBUPDATER] No baseUrl configured");
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
    logMessage("[OTAWEBUPDATER] Invalid response or json in " + baseUrl + "/current-version.json");
    return false;
  }
  if (date > currentFwDate) { // a newer Version is available!
    logMessage("[OTAWEBUPDATER] Newer firmware available: " + date + " vs " + currentFwDate);
    newReleaseAvailable = true;
  }
  logMessage("[OTAWEBUPDATER] No newer firmware available");
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
    logMessage("[OTAWEBUPDATER] No baseUrl configured");
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
    logMessage("[OTAWEBUPDATER] Unable to request memory with malloc(" + String(bufferAllocationLen+1) + ")");
    otaIsRunning = false;
    return false;
  }
  
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.begin(client, firmwareUrl);

  logMessage("[OTAWEBUPDATER] Firmware type: " + String(filetype == U_SPIFFS ? "spiffs" : "flash"));
  logMessage("[OTAWEBUPDATER] Firmware url:  " + firmwareUrl);

  if (http.GET() == 200) {
    // get length of document (is -1 when Server sends no Content-Length header)
    auto totalLength = http.getSize();
    auto len = totalLength;
    auto currentLength = 0;

    // this is required to start firmware update process
    Update.begin(UPDATE_SIZE_UNKNOWN, filetype);
    logMessage("[OTAWEBUPDATER] Firmware size: " + String(totalLength));

    // create buffer for read
    //uint8_t buff[4096] = { 0 };
    WiFiClient * stream = http.getStreamPtr();

    // read all data from server
    logMessage("[OTAWEBUPDATER] Begin firmware upgrade...");
    while(http.connected() && (len > 0 || len == -1)) {
      // get available data size
      size_t size = stream->available();
      if(size) {
        // read up to 4096 byte
        int readBufLen = stream->readBytes(buffer, ((size > bufferAllocationLen) ? bufferAllocationLen : size));
        if(len > 0) len -= readBufLen;

        Update.write(buffer, readBufLen);
        logMessage("[OTAWEBUPDATER] Status: " + String(currentLength));

        currentLength += readBufLen;
        if(currentLength != totalLength) continue;
        // Update completed
        Update.end(true);
        http.end();
        logMessage("\n");
        logMessage("[OTAWEBUPDATER] Upgrade successfully executed. Wrote bytes: " + String(currentLength));

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
    logMessage("[OTAWEBUPDATER] No baseUrl configured");
    return;
  }

  otaIsRunning = true;
  if (updateFile(baseUrl, "littlefs.bin") && updateFile(baseUrl, "firmware.bin") ) {
    ESP.restart();
  } else {
    otaIsRunning = false;
    logMessage("[OTAWEBUPDATER] Failed to update firmware");
  }
}

#include <Arduino.h>
#include <EEPROM.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <HX711.h>
#include <WiFiClientSecureBearSSL.h>

namespace Serena {

constexpr uint32_t EEPROM_MAGIC = 0x53455241;  // "SERA"
constexpr size_t EEPROM_SIZE = 512;

constexpr uint8_t HX711_DOUT_PIN = D6;
constexpr uint8_t HX711_SCK_PIN = D5;

const char* WIFI_SSID = "SERENA_CareHome";
const char* WIFI_PASSWORD = "Serena@2024SafeBed";

const char* IFTTT_EVENT_NAME = "serena_bed_alert";
const char* IFTTT_KEY = "tdkhrpT7TgqawPvq43hG6";

const char* DEVICE_NAME = "SERENA_BED_01";

struct Settings {
  uint32_t magic;
  float calibrationFactor;
  int32_t offset;
  float occupiedThresholdKg;
  uint32_t readIntervalMs;
  uint32_t emptyDelayMs;
};

HX711 scale;
Settings settings{};

float lastRawKg = 0.0f;
float filteredKg = 0.0f;
bool hasFilteredValue = false;

bool bedOccupied = false;
bool alertSentForCurrentAbsence = false;
uint32_t emptySinceMs = 0;
uint32_t lastReadMs = 0;
uint32_t lastWifiAttemptMs = 0;

String readCommandLine() {
  String line = Serial.readStringUntil('\n');
  line.trim();
  return line;
}

void saveSettings() {
  EEPROM.put(0, settings);
  EEPROM.commit();
}

void applySettingsToScale() {
  scale.set_scale(settings.calibrationFactor);
  scale.set_offset(settings.offset);
}

void setDefaultSettings() {
  settings.magic = EEPROM_MAGIC;
  settings.calibrationFactor = -2280.0f;
  settings.offset = 0;
  settings.occupiedThresholdKg = 10.0f;
  settings.readIntervalMs = 60000;
  settings.emptyDelayMs = 900000;
}

bool loadSettings() {
  EEPROM.get(0, settings);
  if (settings.magic != EEPROM_MAGIC) {
    return false;
  }
  if (settings.readIntervalMs < 5000 || settings.readIntervalMs > 3600000) {
    settings.readIntervalMs = 60000;
  }
  if (settings.emptyDelayMs < 60000 || settings.emptyDelayMs > 86400000UL) {
    settings.emptyDelayMs = 900000;
  }
  if (settings.occupiedThresholdKg <= 0.5f || settings.occupiedThresholdKg > 200.0f) {
    settings.occupiedThresholdKg = 10.0f;
  }
  if (settings.calibrationFactor == 0.0f) {
    settings.calibrationFactor = -2280.0f;
  }
  return true;
}

void printBanner() {
  Serial.println();
  Serial.println(F("========================================"));
  Serial.println(F("SERENA Smart Bed for Dementia Patient"));
  Serial.println(F("NodeMCU ESP8266 + HX711 + IFTTT"));
  Serial.println(F("========================================"));
}

void printHelp() {
  Serial.println();
  Serial.println(F("Commands:"));
  Serial.println(F("  help                 -> show commands"));
  Serial.println(F("  status               -> show current settings and state"));
  Serial.println(F("  read                 -> read current weight"));
  Serial.println(F("  tare                 -> set current load as zero / empty bed"));
  Serial.println(F("  cal <known_kg>       -> calibrate using a known weight"));
  Serial.println(F("  threshold <kg>       -> set occupied threshold in kg"));
  Serial.println(F("  interval <sec>       -> set read interval in seconds"));
  Serial.println(F("  delay <min>          -> set alert delay in minutes"));
  Serial.println(F("  defaults             -> restore default settings"));
  Serial.println(F("  trigger              -> send a test IFTTT notification"));
  Serial.println(F("  wifi                 -> show WiFi status"));
}

void printWiFiStatus() {
  Serial.print(F("WiFi status: "));
  Serial.println(WiFi.status() == WL_CONNECTED ? F("CONNECTED") : F("DISCONNECTED"));
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("SSID: "));
    Serial.println(WIFI_SSID);
    Serial.print(F("IP: "));
    Serial.println(WiFi.localIP());
    Serial.print(F("RSSI: "));
    Serial.println(WiFi.RSSI());
  }
}

void printStatus() {
  Serial.println();
  Serial.println(F("Current status"));
  Serial.println(F("----------------------------"));
  Serial.print(F("Calibration factor : "));
  Serial.println(settings.calibrationFactor, 4);
  Serial.print(F("Offset             : "));
  Serial.println(settings.offset);
  Serial.print(F("Threshold (kg)     : "));
  Serial.println(settings.occupiedThresholdKg, 2);
  Serial.print(F("Read interval (s)  : "));
  Serial.println(settings.readIntervalMs / 1000UL);
  Serial.print(F("Alert delay (min)  : "));
  Serial.println(settings.emptyDelayMs / 60000UL);
  Serial.print(F("Last raw kg        : "));
  Serial.println(lastRawKg, 2);
  Serial.print(F("Filtered kg        : "));
  Serial.println(filteredKg, 2);
  Serial.print(F("Bed occupied       : "));
  Serial.println(bedOccupied ? F("YES") : F("NO"));
  Serial.print(F("Alert sent         : "));
  Serial.println(alertSentForCurrentAbsence ? F("YES") : F("NO"));
  printWiFiStatus();
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.hostname(DEVICE_NAME);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print(F("Connecting to WiFi"));
  uint8_t retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 30) {
    delay(500);
    Serial.print('.');
    ++retries;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("WiFi connected. IP: "));
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("WiFi not connected yet. The sketch will keep retrying."));
  }
}

void ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  const uint32_t now = millis();
  if (now - lastWifiAttemptMs < 10000UL) {
    return;
  }

  lastWifiAttemptMs = now;
  Serial.println(F("Reconnecting WiFi..."));
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

bool scaleReady() {
  if (scale.is_ready()) {
    return true;
  }
  Serial.println(F("HX711 not ready."));
  return false;
}

float readWeightKg(uint8_t samples = 10) {
  if (!scaleReady()) {
    return NAN;
  }
  return scale.get_units(samples);
}

float readRawUnits(uint8_t samples = 10) {
  if (!scaleReady()) {
    return NAN;
  }

  const float originalScale = settings.calibrationFactor;
  scale.set_scale(1.0f);
  const float rawUnits = scale.get_units(samples);
  scale.set_scale(originalScale);
  return rawUnits;
}

void tareEmptyBed() {
  if (!scaleReady()) {
    return;
  }

  Serial.println(F("Taring... keep the bed empty and still."));
  scale.set_scale();
  scale.tare(20);
  settings.offset = scale.get_offset();
  applySettingsToScale();
  saveSettings();

  Serial.print(F("New offset saved: "));
  Serial.println(settings.offset);
}

void calibrateWithKnownWeight(float knownKg) {
  if (knownKg <= 0.0f) {
    Serial.println(F("Known weight must be greater than 0."));
    return;
  }

  if (!scaleReady()) {
    return;
  }

  Serial.println(F("Calibration started."));
  Serial.println(F("1) Run 'tare' with empty bed first."));
  Serial.println(F("2) Place the known weight on the bed."));
  delay(2000);

  const float measuredRaw = readRawUnits(15);
  if (isnan(measuredRaw) || fabs(measuredRaw) < 0.001f) {
    Serial.println(F("Calibration failed: raw reading too small."));
    return;
  }

  settings.calibrationFactor = measuredRaw / knownKg;
  applySettingsToScale();
  saveSettings();

  Serial.print(F("Calibration factor saved: "));
  Serial.println(settings.calibrationFactor, 6);
  Serial.print(F("Measured weight now: "));
  Serial.println(readWeightKg(10), 2);
}

String urlEncode(const String& value) {
  String encoded;
  encoded.reserve(value.length() * 3);

  const char* hex = "0123456789ABCDEF";

  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value.charAt(i);
    if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += c;
    } else {
      encoded += '%';
      encoded += hex[(c >> 4) & 0x0F];
      encoded += hex[c & 0x0F];
    }
  }

  return encoded;
}

bool sendIFTTTEvent(const String& value1, const String& value2, const String& value3) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("IFTTT send failed: WiFi disconnected."));
    return false;
  }

  BearSSL::WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  const String url =
      String(F("https://maker.ifttt.com/trigger/")) + IFTTT_EVENT_NAME +
      F("/with/key/") + IFTTT_KEY +
      F("?value1=") + urlEncode(value1) +
      F("&value2=") + urlEncode(value2) +
      F("&value3=") + urlEncode(value3);

  if (!https.begin(client, url)) {
    Serial.println(F("IFTTT send failed: HTTPS begin failed."));
    return false;
  }

  https.setTimeout(10000);
  https.addHeader("User-Agent", "SERENA-ESP8266/1.0");

  const int httpCode = https.GET();
  const String response = https.getString();
  https.end();

  Serial.print(F("IFTTT HTTP code: "));
  Serial.println(httpCode);

  if (httpCode > 0 && httpCode < 400) {
    Serial.println(F("IFTTT notification sent."));
    return true;
  }

  Serial.println(F("IFTTT notification failed."));
  if (response.length() > 0) {
    Serial.println(response);
  }
  return false;
}

void handlePatientReturned(float currentKg) {
  if (!bedOccupied) {
    Serial.println(F("Patient detected on bed again. Resetting absence timer."));
  }

  bedOccupied = true;
  emptySinceMs = 0;
  alertSentForCurrentAbsence = false;

  Serial.print(F("Weight: "));
  Serial.print(currentKg, 2);
  Serial.println(F(" kg -> BED OCCUPIED"));
}

void handlePatientAbsent(float currentKg) {
  const uint32_t now = millis();

  if (bedOccupied || emptySinceMs == 0) {
    emptySinceMs = now;
    Serial.println(F("Bed became empty. Starting absence timer."));
  }

  bedOccupied = false;

  const uint32_t absentMs = now - emptySinceMs;
  const uint32_t absentMinutes = absentMs / 60000UL;

  Serial.print(F("Weight: "));
  Serial.print(currentKg, 2);
  Serial.print(F(" kg -> BED EMPTY for "));
  Serial.print(absentMinutes);
  Serial.println(F(" minute(s)"));

  if (!alertSentForCurrentAbsence && absentMs >= settings.emptyDelayMs) {
    const bool ok = sendIFTTTEvent(
        String(currentKg, 2) + F(" kg"),
        String(absentMinutes) + F(" minute(s)"),
        DEVICE_NAME);

    if (ok) {
      alertSentForCurrentAbsence = true;
    }
  }
}

void evaluateBedState(float currentKg) {
  if (isnan(currentKg)) {
    Serial.println(F("Invalid weight reading."));
    return;
  }

  if (currentKg >= settings.occupiedThresholdKg) {
    handlePatientReturned(currentKg);
  } else {
    handlePatientAbsent(currentKg);
  }
}

void sampleAndProcess() {
  const float rawKg = readWeightKg(10);
  if (isnan(rawKg)) {
    return;
  }

  lastRawKg = rawKg;

  if (!hasFilteredValue) {
    filteredKg = rawKg;
    hasFilteredValue = true;
  } else {
    filteredKg = (filteredKg * 0.7f) + (rawKg * 0.3f);
  }

  evaluateBedState(filteredKg);
}

void restoreDefaults() {
  setDefaultSettings();
  applySettingsToScale();
  saveSettings();
  Serial.println(F("Defaults restored."));
}

void handleCommand(const String& command) {
  if (command.length() == 0) {
    return;
  }

  if (command.equalsIgnoreCase("help")) {
    printHelp();
    return;
  }

  if (command.equalsIgnoreCase("status")) {
    printStatus();
    return;
  }

  if (command.equalsIgnoreCase("read")) {
    const float kg = readWeightKg(10);
    if (!isnan(kg)) {
      Serial.print(F("Current weight: "));
      Serial.print(kg, 2);
      Serial.println(F(" kg"));
    }
    return;
  }

  if (command.equalsIgnoreCase("tare")) {
    tareEmptyBed();
    return;
  }

  if (command.equalsIgnoreCase("defaults")) {
    restoreDefaults();
    return;
  }

  if (command.equalsIgnoreCase("wifi")) {
    printWiFiStatus();
    return;
  }

  if (command.equalsIgnoreCase("trigger")) {
    sendIFTTTEvent(F("manual_test"), F("0"), DEVICE_NAME);
    return;
  }

  if (command.startsWith("cal ")) {
    const float knownKg = command.substring(4).toFloat();
    calibrateWithKnownWeight(knownKg);
    return;
  }

  if (command.startsWith("threshold ")) {
    const float thresholdKg = command.substring(10).toFloat();
    if (thresholdKg <= 0.0f) {
      Serial.println(F("Threshold must be greater than 0."));
      return;
    }
    settings.occupiedThresholdKg = thresholdKg;
    saveSettings();
    Serial.print(F("New threshold saved: "));
    Serial.println(settings.occupiedThresholdKg, 2);
    return;
  }

  if (command.startsWith("interval ")) {
    const uint32_t seconds = static_cast<uint32_t>(command.substring(9).toInt());
    if (seconds < 5 || seconds > 3600) {
      Serial.println(F("Interval must be between 5 and 3600 seconds."));
      return;
    }
    settings.readIntervalMs = seconds * 1000UL;
    saveSettings();
    Serial.print(F("New read interval saved: "));
    Serial.print(seconds);
    Serial.println(F(" second(s)"));
    return;
  }

  if (command.startsWith("delay ")) {
    const uint32_t minutes = static_cast<uint32_t>(command.substring(6).toInt());
    if (minutes < 1 || minutes > 1440) {
      Serial.println(F("Delay must be between 1 and 1440 minutes."));
      return;
    }
    settings.emptyDelayMs = minutes * 60000UL;
    saveSettings();
    Serial.print(F("New alert delay saved: "));
    Serial.print(minutes);
    Serial.println(F(" minute(s)"));
    return;
  }

  Serial.println(F("Unknown command. Type 'help'."));
}

void handleSerial() {
  if (!Serial.available()) {
    return;
  }

  const String command = readCommandLine();
  handleCommand(command);
}

void initializeSettingsAndScale() {
  EEPROM.begin(EEPROM_SIZE);

  if (!loadSettings()) {
    setDefaultSettings();
    saveSettings();
    Serial.println(F("No saved settings found. Default settings created."));
  }

  scale.begin(HX711_DOUT_PIN, HX711_SCK_PIN);
  applySettingsToScale();

  if (settings.offset == 0 && scale.is_ready()) {
    Serial.println(F("First startup detected. Auto-taring with current empty bed."));
    tareEmptyBed();
  }
}

}  // namespace Serena

void setup() {
  using namespace Serena;

  Serial.begin(115200);
  delay(300);

  printBanner();
  initializeSettingsAndScale();
  connectWiFi();
  printHelp();
  printStatus();

  sampleAndProcess();
  lastReadMs = millis();
}

void loop() {
  using namespace Serena;

  ensureWiFiConnected();
  handleSerial();

  const uint32_t now = millis();
  if (now - lastReadMs >= settings.readIntervalMs) {
    lastReadMs = now;
    sampleAndProcess();
  }
}

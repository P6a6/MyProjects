#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <EEPROM.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// Pin definitions
#define STEP_PIN 2
#define DIR_PIN 4
#define EN_PIN 16
#define HOME_SENSOR_PIN 18
#define PSU_PIN 5        // PC Power Supply control
#define CEILING_LAMP_PIN 13  // Ceiling lamp toggle (existing pulse-based logic)

// MOSFET outputs (12V DC loads)
#define DESK_LAMP_PIN     25  // PWM dimming via LEDC
#define CHARGER_PIN       26  // Phone charger on/off
#define CORRIDOR_PIN      27  // Corridor lamp on/off
#define DESK_LAMP2_PIN    14  // Desk light 2 - PWM dimming via LEDC

#define DESK_LAMP_LEDC_CHANNEL  0
#define DESK_LAMP2_LEDC_CHANNEL 1
#define DESK_LAMP_LEDC_FREQ     1000
#define DESK_LAMP_LEDC_RES      8    // 8-bit: 0-255

// WiFi credentials
const char* ssid = "VM6159022";
const char* password = "rcgc4yyYdfrt";

// ===== BME680 sensor (I2C on pins 32/33) =====
#define SDA_PIN 32
#define SCL_PIN 33
// BME680 I2C address: 0x77 if SDO is floating/high, 0x76 if SDO is grounded
#define BME680_ADDR 0x77

// ===== MQTT — fill in your Home Assistant details =====
const char* MQTT_BROKER    = "192.168.0.41";
const int   MQTT_PORT      = 1883;
const char* MQTT_USER      = "";              // ← Mosquitto username (leave blank if none)
const char* MQTT_PASS      = "";              // ← Mosquitto password (leave blank if none)
const char* MQTT_CLIENT_ID = "smartcurtain_esp32";
#define MQTT_STATE_TOPIC       "smartcurtain/sensor/state"
#define MQTT_COVER_CMD_TOPIC   "smartcurtain/cover/command"     // HA → ESP: OPEN/CLOSE/STOP
#define MQTT_COVER_SET_TOPIC   "smartcurtain/cover/set_position" // HA → ESP: 0-100
#define MQTT_COVER_POS_TOPIC   "smartcurtain/cover/position"    // ESP → HA: 0-100
#define MQTT_COVER_STATE_TOPIC "smartcurtain/cover/state"        // ESP → HA: open/closed/opening/closing/stopped

// Web server
WebServer server(80);

// System state
int currentPosition = 0;
int closedPosition = 0;
bool motorRunning = false;
bool isHomed = false;
bool positionLost = false;

// MOSFET state
int  deskBrightness   = 0;    // 0-100 percent (user-facing)
int  deskCurrentDuty  = 0;    // 0-255 actual LEDC duty (tracks real current level for fading)
int  desk2Brightness  = 0;
int  desk2CurrentDuty = 0;
bool chargerOn        = false;
bool corridorOn       = false;

// Cover command queue (set from MQTT callback, executed in loop)
enum CoverCommand { CMD_NONE, CMD_OPEN, CMD_CLOSE, CMD_SET_POS };
volatile CoverCommand pendingCoverCmd = CMD_NONE;
volatile int          pendingCoverPos = 0;  // target steps for CMD_SET_POS

// BME680 + MQTT
Adafruit_BME680 bme;
WiFiClient      mqttNet;
PubSubClient    mqtt(mqttNet);
bool            bmeAvailable        = false;
unsigned long   lastSensorPublish   = 0;
const unsigned long SENSOR_INTERVAL = 30000;  // ms

// Constants (1/16 microstepping - MS1/MS2/MS3 all wired to 3.3V)
const int HOMING_TIMEOUT_MS = 90000;
const int HOMING_SPEED_DELAY = 1600;
const int NORMAL_SPEED_DELAY = 950;
const int HEAVY_LOAD_DELAY = 1600;
const int MANUAL_STEP_SIZE = 1600;  // ~0.5 rev per manual tap at 1/16 step
const int EEPROM_ADDRESS          = 0;  // closedPosition (int, 4 bytes)
const int EEPROM_CURR_POS_ADDRESS = 4;  // currentPosition (int, 4 bytes)

// Embedded HTML
const char* HTML_PAGE = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Smart Curtain Controller</title>
    <style>
        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
        }
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
            padding: 20px;
        }
        .container {
            max-width: 600px;
            margin: 0 auto;
        }
        .header {
            text-align: center;
            color: white;
            margin-bottom: 30px;
        }
        .header h1 {
            font-size: 28px;
            margin-bottom: 5px;
        }
        .card {
            background: rgba(255, 255, 255, 0.95);
            border-radius: 15px;
            padding: 25px;
            margin-bottom: 20px;
            box-shadow: 0 8px 32px rgba(0, 0, 0, 0.1);
        }
        .status-grid {
            display: grid;
            grid-template-columns: 1fr;
            gap: 15px;
            margin-bottom: 20px;
        }
        .status-item {
            border: 2px solid #e0e0e0;
            border-radius: 10px;
            padding: 15px;
            text-align: center;
        }
        .status-label {
            color: #666;
            font-size: 12px;
            text-transform: uppercase;
            margin-bottom: 5px;
        }
        .status-value {
            color: #10b981;
            font-size: 22px;
            font-weight: bold;
        }
        .status-value.warning {
            color: #f59e0b;
        }
        .section-title {
            font-size: 18px;
            font-weight: 600;
            color: #333;
            margin-bottom: 15px;
            padding-bottom: 10px;
            border-bottom: 2px solid #e0e0e0;
        }
        .button-grid {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 15px;
            margin-bottom: 15px;
        }
        .btn {
            padding: 15px;
            font-size: 16px;
            font-weight: 600;
            border: none;
            border-radius: 10px;
            cursor: pointer;
            transition: all 0.3s;
            text-transform: uppercase;
        }
        .btn:active {
            transform: scale(0.95);
        }
        .btn-open {
            background: linear-gradient(135deg, #10b981, #059669);
            color: white;
        }
        .btn-open:hover {
            background: linear-gradient(135deg, #059669, #047857);
        }
        .btn-close {
            background: linear-gradient(135deg, #f59e0b, #d97706);
            color: white;
        }
        .btn-close:hover {
            background: linear-gradient(135deg, #d97706, #b45309);
        }
        .btn-stop {
            background: linear-gradient(135deg, #ef4444, #dc2626);
            color: white;
            grid-column: span 2;
        }
        .btn-stop:hover {
            background: linear-gradient(135deg, #dc2626, #b91c1c);
        }
        .manual-control {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 10px;
            margin-bottom: 15px;
        }
        .btn-manual {
            padding: 12px;
            background: #6366f1;
            color: white;
            border: none;
            border-radius: 8px;
            cursor: pointer;
            font-weight: 600;
        }
        .btn-manual:hover {
            background: #4f46e5;
        }
        .btn-save {
            padding: 12px;
            background: #8b5cf6;
            color: white;
            border: none;
            border-radius: 8px;
            cursor: pointer;
            font-weight: 600;
            margin-top: 10px;
        }
        .btn-save:hover {
            background: #7c3aed;
        }
        .info-box {
            background: #fef3c7;
            border-left: 4px solid #f59e0b;
            padding: 12px;
            border-radius: 5px;
            margin-top: 15px;
            font-size: 14px;
            color: #78350f;
        }
        .refresh-btn {
            position: fixed;
            bottom: 20px;
            right: 20px;
            width: 50px;
            height: 50px;
            background: white;
            border: none;
            border-radius: 50%;
            box-shadow: 0 4px 12px rgba(0, 0, 0, 0.2);
            cursor: pointer;
            font-size: 24px;
        }
        .refresh-btn:active {
            transform: scale(0.9);
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>🪟 Smart Curtain Controller</h1>
            <p>Simple. Reliable. Smart.</p>
        </div>
        <div class="card">
            <div class="status-grid">
                <div class="status-item">
                    <div class="status-label">System Status</div>
                    <div class="status-value">{SYSTEM_STATUS}</div>
                </div>
                <div class="status-item">
                    <div class="status-label">Curtain Position</div>
                    <div class="status-value">{CURTAIN_POSITION}</div>
                </div>
                <div class="status-item">
                    <div class="status-label">Connection</div>
                    <div class="status-value">{CONNECTION_STATUS}</div>
                </div>
            </div>
        </div>
        <div class="card">
            <div class="section-title">Curtain Control</div>
            <div class="button-grid">
                <button class="btn btn-open" onclick="sendCommand('open')">▲ Open</button>
                <button class="btn btn-close" onclick="sendCommand('close')">▼ Close</button>
                <button class="btn btn-stop" onclick="sendCommand('stop')">■ Emergency Stop</button>
            </div>
        </div>
        <div class="card">
            <div class="section-title">Manual Control & Calibration</div>
            <div class="manual-control">
                <button class="btn-manual" onclick="manualMove('up')">▲ Up</button>
                <button class="btn-manual" onclick="manualMove('down')">▼ Down</button>
            </div>
            <button class="btn-save" onclick="savePosition()">💾 Save Current Position as CLOSED</button>
            <div class="info-box">
                <strong>Calibration:</strong><br>
                1. Use manual controls to move curtain to desired closed position<br>
                2. Click "Save Current Position"<br>
                3. Test with Open/Close buttons
            </div>
        </div>
    </div>
    <button class="refresh-btn" onclick="location.reload()">🔄</button>
    <script>
        function sendCommand(action) {
            fetch('/control', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: 'action=' + action
            }).then(() => setTimeout(() => location.reload(), 1000));
        }
        function manualMove(direction) {
            fetch('/control', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: 'action=manual&direction=' + direction
            }).then(() => setTimeout(() => location.reload(), 500));
        }
        function savePosition() {
            if (confirm('Save current position as CLOSED position?')) {
                fetch('/control', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                    body: 'action=save'
                }).then(() => { alert('Saved!'); location.reload(); });
            }
        }
    </script>
</body>
</html>
)rawliteral";

// Function declarations
void mqttConnect();
void publishDiscovery();
void publishSensorData();
void publishCoverState();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void runMotorSteps(int steps, int delayMicros, bool direction, bool checkSensor = false);
void handleRoot();
void handleControl();
void handleStatus();
void handleSiriUp();
void handleSiriDown();
void handlePsuOn();
void handlePsuOff();
void handleCeilingLampToggle();
void autoHome();
void moveToPosition(int targetPosition);
void motorBeep(int frequency, int durationMs);
void fadeDesk(int targetPercent);
void fadeDesk2(int targetPercent);
void handleDeskOn();
void handleDeskOff();
void handleDeskBrightness();
void handleChargerOn();
void handleChargerOff();
void handleCorridorOn();
void handleCorridorOff();
void handleDesk2On();
void handleDesk2Off();
void handleDesk2Brightness();

void setup() {
  Serial.begin(115200);
  EEPROM.begin(512);

  pinMode(STEP_PIN, OUTPUT);
  pinMode(DIR_PIN, OUTPUT);
  pinMode(EN_PIN, OUTPUT);
  pinMode(HOME_SENSOR_PIN, INPUT_PULLUP);
  pinMode(PSU_PIN, INPUT);  // PSU OFF by default (floating)
  pinMode(CEILING_LAMP_PIN, INPUT);  // Ceiling lamp - floating to prevent boot trigger

  // MOSFET outputs — LOW before anything else so they don't fire at boot
  pinMode(DESK_LAMP_PIN, OUTPUT);  digitalWrite(DESK_LAMP_PIN, LOW);
  pinMode(CHARGER_PIN,   OUTPUT);  digitalWrite(CHARGER_PIN,   LOW);
  pinMode(CORRIDOR_PIN,  OUTPUT);  digitalWrite(CORRIDOR_PIN,  LOW);
  pinMode(DESK_LAMP2_PIN, OUTPUT);  digitalWrite(DESK_LAMP2_PIN, LOW);

  // PWM for desk lamps
  ledcSetup(DESK_LAMP_LEDC_CHANNEL,  DESK_LAMP_LEDC_FREQ, DESK_LAMP_LEDC_RES);
  ledcAttachPin(DESK_LAMP_PIN,  DESK_LAMP_LEDC_CHANNEL);
  ledcSetup(DESK_LAMP2_LEDC_CHANNEL, DESK_LAMP_LEDC_FREQ, DESK_LAMP_LEDC_RES);
  ledcAttachPin(DESK_LAMP2_PIN, DESK_LAMP2_LEDC_CHANNEL);

  digitalWrite(STEP_PIN, LOW);
  digitalWrite(DIR_PIN, HIGH);
  digitalWrite(EN_PIN, LOW);

  Serial.println("Smart Curtain Controller Starting...");

  EEPROM.get(EEPROM_ADDRESS, closedPosition);
  if (closedPosition < 0 || closedPosition > 50000) closedPosition = 0;
  Serial.println("Loaded closed position: " + String(closedPosition) + " steps");

  EEPROM.get(EEPROM_CURR_POS_ADDRESS, currentPosition);
  if (currentPosition < 0 || currentPosition > 50000) currentPosition = 0;
  Serial.println("Loaded last position: " + String(currentPosition) + " steps");

  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  // Startup chime
  motorBeep(440, 120);
  delay(60);
  motorBeep(660, 120);
  delay(60);
  motorBeep(880, 180);

  server.on("/", handleRoot);
  server.on("/control", HTTP_POST, handleControl);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/curtain/up", HTTP_GET, handleSiriUp);
  server.on("/curtain/down", HTTP_GET, handleSiriDown);
  server.on("/psu/on", HTTP_GET, handlePsuOn);
  server.on("/psu/off", HTTP_GET, handlePsuOff);
  server.on("/lamp/toggle", HTTP_GET, handleCeilingLampToggle);
  server.on("/desk/on", HTTP_GET, handleDeskOn);
  server.on("/desk/off", HTTP_GET, handleDeskOff);
  server.on("/desk/brightness", HTTP_GET, handleDeskBrightness);
  server.on("/charger/on", HTTP_GET, handleChargerOn);
  server.on("/charger/off", HTTP_GET, handleChargerOff);
  server.on("/corridor/on", HTTP_GET, handleCorridorOn);
  server.on("/corridor/off", HTTP_GET, handleCorridorOff);
  server.on("/desk2/on", HTTP_GET, handleDesk2On);
  server.on("/desk2/off", HTTP_GET, handleDesk2Off);
  server.on("/desk2/brightness", HTTP_GET, handleDesk2Brightness);

  server.begin();
  Serial.println("Web server started!");
  Serial.println("\n=== Curtain URLs ===");
  Serial.println("Up:     http://" + WiFi.localIP().toString() + "/curtain/up");
  Serial.println("Down:   http://" + WiFi.localIP().toString() + "/curtain/down");
  Serial.println("Status: http://" + WiFi.localIP().toString() + "/status");
  Serial.println("\n=== PSU URLs ===");
  Serial.println("ON:  http://" + WiFi.localIP().toString() + "/psu/on");
  Serial.println("OFF: http://" + WiFi.localIP().toString() + "/psu/off");
  Serial.println("\n=== Ceiling Lamp ===");
  Serial.println("Toggle: http://" + WiFi.localIP().toString() + "/lamp/toggle");
  Serial.println("\n=== MOSFET Devices ===");
  Serial.println("Desk ON:     http://" + WiFi.localIP().toString() + "/desk/on");
  Serial.println("Desk OFF:    http://" + WiFi.localIP().toString() + "/desk/off");
  Serial.println("Desk Dim:    http://" + WiFi.localIP().toString() + "/desk/brightness?level=75");
  Serial.println("Charger ON:  http://" + WiFi.localIP().toString() + "/charger/on");
  Serial.println("Charger OFF: http://" + WiFi.localIP().toString() + "/charger/off");
  Serial.println("Corridor ON: http://" + WiFi.localIP().toString() + "/corridor/on");
  Serial.println("Corridor OFF:http://" + WiFi.localIP().toString() + "/corridor/off");
  Serial.println("Desk2 ON:    http://" + WiFi.localIP().toString() + "/desk2/on");
  Serial.println("Desk2 OFF:   http://" + WiFi.localIP().toString() + "/desk2/off");
  Serial.println("Desk2 Dim:   http://" + WiFi.localIP().toString() + "/desk2/brightness?level=75");

  // BME680
  Wire.begin(SDA_PIN, SCL_PIN);
  if (bme.begin(BME680_ADDR, &Wire)) {
    bmeAvailable = true;
    bme.setTemperatureOversampling(BME680_OS_8X);
    bme.setHumidityOversampling(BME680_OS_2X);
    bme.setPressureOversampling(BME680_OS_4X);
    bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
    bme.setGasHeater(320, 150);  // 320°C for 150 ms — standard air-quality setting
    Serial.println("BME680 ready");
  } else {
    Serial.println("BME680 NOT found — check wiring or try address 0x76");
  }

  // MQTT
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setBufferSize(800);  // cover discovery payload is ~620 bytes; 512 was too small
  mqttConnect();

  Serial.println("Auto-homing...");
  autoHome();
}

void loop() {
  server.handleClient();

  // WiFi watchdog
  static unsigned long lastWiFiCheck = 0;
  if (millis() - lastWiFiCheck > 60000) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi reconnecting...");
      WiFi.reconnect();
    }
    lastWiFiCheck = millis();
  }

  // MQTT keep-alive + reconnect
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqtt.connected()) {
      static unsigned long lastMqttRetry = 0;
      if (millis() - lastMqttRetry > 10000) {
        lastMqttRetry = millis();
        mqttConnect();
      }
    }
    mqtt.loop();
  }

  // Publish sensor data every 30 s
  if (bmeAvailable && mqtt.connected() &&
      millis() - lastSensorPublish > SENSOR_INTERVAL) {
    publishSensorData();
    lastSensorPublish = millis();
  }

  // Process pending MQTT cover commands (set by callback, executed here so motor
  // functions run in loop() context — safe to call mqtt.loop() inside them)
  if (pendingCoverCmd != CMD_NONE && !motorRunning) {
    CoverCommand cmd = (CoverCommand)pendingCoverCmd;
    int          pos = pendingCoverPos;
    pendingCoverCmd  = CMD_NONE;

    switch (cmd) {
      case CMD_OPEN:
        if (mqtt.connected()) mqtt.publish(MQTT_COVER_STATE_TOPIC, "opening", true);
        autoHome();
        publishCoverState();
        break;

      case CMD_CLOSE:
        if (closedPosition > 0) {
          if (mqtt.connected()) mqtt.publish(MQTT_COVER_STATE_TOPIC, "closing", true);
          if (positionLost) autoHome();
          moveToPosition(closedPosition);
          publishCoverState();
        }
        break;

      case CMD_SET_POS:
        if (closedPosition > 0 && !positionLost) {
          bool goingUp = (pos < currentPosition);
          if (mqtt.connected())
            mqtt.publish(MQTT_COVER_STATE_TOPIC, goingUp ? "opening" : "closing", true);
          moveToPosition(pos);
          publishCoverState();
        }
        break;

      default: break;
    }
  }
}

// ===== BME680 / MQTT =====

void publishDiscovery() {
  // All four sensors share one state topic; HA picks values via value_template.
  // Payloads are retained so HA picks them up even after an HA restart.
  struct SensorDef {
    const char* id;
    const char* name;
    const char* device_class;  // empty string = no device_class key
    const char* unit;
    const char* value_template;
    const char* icon;
    const char* state_class;
  };

  const SensorDef sensors[] = {
    {"temperature", "Room Temperature", "temperature", "°C",
     "{{ value_json.temperature | round(1) }}", "mdi:thermometer", "measurement"},
    {"humidity",    "Room Humidity",    "humidity",    "%",
     "{{ value_json.humidity | round(1) }}",    "mdi:water-percent", "measurement"},
    {"pressure",    "Room Pressure",    "pressure",    "hPa",
     "{{ value_json.pressure | round(1) }}",    "mdi:gauge", "measurement"},
    {"gas",         "Room Air Quality", "",            "kΩ",
     "{{ value_json.gas | round(0) }}",         "mdi:air-filter", "measurement"},
  };

  for (const auto& s : sensors) {
    StaticJsonDocument<512> doc;
    doc["name"]                = s.name;
    if (s.device_class[0] != '\0') doc["device_class"] = s.device_class;
    doc["state_class"]         = s.state_class;
    doc["state_topic"]         = MQTT_STATE_TOPIC;
    doc["unit_of_measurement"] = s.unit;
    doc["value_template"]      = s.value_template;
    doc["unique_id"]           = String("smartcurtain_") + s.id;
    doc["expire_after"]        = 120;
    doc["icon"]                = s.icon;

    JsonObject device = doc.createNestedObject("device");
    device["identifiers"][0]   = "smartcurtain_esp32";
    device["name"]             = "Smart Curtain Room";
    device["model"]            = "ESP32 + BME680";
    device["manufacturer"]     = "DIY";

    String payload;
    serializeJson(doc, payload);

    String topic = String("homeassistant/sensor/smartcurtain_") + s.id + "/config";
    mqtt.publish(topic.c_str(), payload.c_str(), true);  // retain = true
    delay(50);
  }
  // Cover entity — gives HA the position slider
  {
    StaticJsonDocument<800> doc;
    doc["name"]               = "Bedroom Curtain";
    doc["device_class"]       = "curtain";
    doc["command_topic"]      = MQTT_COVER_CMD_TOPIC;
    doc["payload_open"]       = "OPEN";
    doc["payload_close"]      = "CLOSE";
    doc["payload_stop"]       = "STOP";
    doc["state_topic"]        = MQTT_COVER_STATE_TOPIC;
    doc["state_open"]         = "open";
    doc["state_closed"]       = "closed";
    doc["state_opening"]      = "opening";
    doc["state_closing"]      = "closing";
    doc["position_topic"]     = MQTT_COVER_POS_TOPIC;
    doc["set_position_topic"] = MQTT_COVER_SET_TOPIC;
    doc["position_open"]      = 100;
    doc["position_closed"]    = 0;
    doc["optimistic"]         = false;
    doc["unique_id"]          = "smartcurtain_cover";

    JsonObject device = doc.createNestedObject("device");
    device["identifiers"][0]  = "smartcurtain_esp32";
    device["name"]            = "Smart Curtain Room";
    device["model"]           = "ESP32 + BME680";
    device["manufacturer"]    = "DIY";

    String payload;
    serializeJson(doc, payload);
    mqtt.publish("homeassistant/cover/smartcurtain_cover/config", payload.c_str(), true);
    delay(50);
  }

  Serial.println("MQTT: auto-discovery published");
}

void mqttConnect() {
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(90);  // longer than max motor run time so connection survives long moves

  int retries = 0;
  while (!mqtt.connected() && retries < 5) {
    Serial.print("MQTT connecting... ");
    bool ok = (strlen(MQTT_USER) > 0)
      ? mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS)
      : mqtt.connect(MQTT_CLIENT_ID);
    if (ok) {
      Serial.println("connected!");
      mqtt.subscribe(MQTT_COVER_CMD_TOPIC);
      mqtt.subscribe(MQTT_COVER_SET_TOPIC);
      publishDiscovery();
      publishCoverState();  // let HA know current position on reconnect
    } else {
      Serial.print("failed, rc=");
      Serial.println(mqtt.state());
      delay(2000);
      retries++;
    }
  }
}

void publishSensorData() {
  if (!bme.performReading()) {
    Serial.println("BME680: reading failed");
    return;
  }

  StaticJsonDocument<128> doc;
  doc["temperature"] = round(bme.temperature * 10.0f) / 10.0f;
  doc["humidity"]    = round(bme.humidity    * 10.0f) / 10.0f;
  doc["pressure"]    = round((bme.pressure / 100.0f) * 10.0f) / 10.0f;  // Pa → hPa
  doc["gas"]         = round(bme.gas_resistance / 1000.0f);              // Ω → kΩ

  char payload[128];
  serializeJson(doc, payload);

  if (mqtt.publish(MQTT_STATE_TOPIC, payload)) {
    Serial.printf("MQTT: T=%.1f°C  H=%.1f%%  P=%.1fhPa  Gas=%.0fkΩ\n",
      bme.temperature, bme.humidity,
      bme.pressure / 100.0f, bme.gas_resistance / 1000.0f);
  } else {
    Serial.println("MQTT: publish failed");
  }
}

void publishCoverState() {
  if (!mqtt.connected() || closedPosition == 0) return;

  int haPos = 100 - (int)round((float)currentPosition / closedPosition * 100.0f);
  haPos = constrain(haPos, 0, 100);

  const char* state;
  if (haPos >= 99)     state = "open";
  else if (haPos <= 1) state = "closed";
  else                 state = "stopped";

  mqtt.publish(MQTT_COVER_POS_TOPIC,   String(haPos).c_str(), true);
  mqtt.publish(MQTT_COVER_STATE_TOPIC, state,                  true);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String t(topic);
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  if (t == MQTT_COVER_CMD_TOPIC) {
    if (msg == "STOP") {
      // STOP is handled immediately — directly kills the motor loop
      motorRunning    = false;
      positionLost    = true;
      pendingCoverCmd = CMD_NONE;
      EEPROM.put(EEPROM_CURR_POS_ADDRESS, currentPosition);
      EEPROM.commit();
    } else if (msg == "OPEN") {
      pendingCoverCmd = CMD_OPEN;
    } else if (msg == "CLOSE") {
      pendingCoverCmd = CMD_CLOSE;
    }
  } else if (t == MQTT_COVER_SET_TOPIC) {
    int targetPct   = constrain(msg.toInt(), 0, 100);
    // HA: 100 = open (steps=0), 0 = closed (steps=closedPosition)
    pendingCoverPos = (int)round((100 - targetPct) / 100.0f * closedPosition);
    pendingCoverCmd = CMD_SET_POS;
  }
}

// ===== HTTP handlers =====

void handleSiriUp() {
  Serial.println("*** HTTP: OPEN ***");
  if (motorRunning) {
    server.send(200, "text/plain", "Motor busy");
    return;
  }
  server.send(200, "text/plain", "Opening");  // RESPOND IMMEDIATELY
  autoHome();
  positionLost = false;
}

void handleSiriDown() {
  Serial.println("*** HTTP: CLOSE ***");
  if (closedPosition == 0) {
    server.send(400, "text/plain", "No closed position set");
    return;
  }
  if (motorRunning) {
    server.send(200, "text/plain", "Motor busy");
    return;
  }
  server.send(200, "text/plain", "Closing");  // RESPOND IMMEDIATELY
  if (positionLost) {
    autoHome();
    delay(500);
  }
  moveToPosition(closedPosition);
}

void handlePsuOn() {
  Serial.println("*** PSU: ON ***");
  pinMode(PSU_PIN, OUTPUT);
  digitalWrite(PSU_PIN, LOW);  // Ground green wire = PSU ON
  server.send(200, "text/plain", "PSU ON");
}

void handlePsuOff() {
  Serial.println("*** PSU: OFF ***");
  pinMode(PSU_PIN, INPUT);  // Floating = PSU OFF
  server.send(200, "text/plain", "PSU OFF");
}

void handleCeilingLampToggle() {
  Serial.println("*** CEILING LAMP: TOGGLE ***");
  server.send(200, "text/plain", "Toggling");  // Respond immediately
  
  pinMode(CEILING_LAMP_PIN, OUTPUT);
  digitalWrite(CEILING_LAMP_PIN, LOW);  // Ground for toggle
  delay(400);  // Brief pulse - 400ms
  pinMode(CEILING_LAMP_PIN, INPUT);  // Back to floating
  
  Serial.println("Lamp toggled");
}

void handleStatus() {
  String state;
  if (closedPosition > 0) {
    state = (currentPosition < (closedPosition * 0.1)) ? "open" : "closed";
  } else {
    state = "unknown";
  }
  
  String json = "{";
  json += "\"state\":\"" + state + "\",";
  json += "\"position\":" + String(currentPosition) + ",";
  json += "\"closedPosition\":" + String(closedPosition) + ",";
  json += "\"isHomed\":" + String(isHomed ? "true" : "false") + ",";
  json += "\"motorRunning\":" + String(motorRunning ? "true" : "false") + ",";
  json += "\"positionLost\":" + String(positionLost ? "true" : "false");
  json += "}";
  
  server.send(200, "application/json", json);
}

void handleRoot() {
  String html = String(HTML_PAGE);
  
  String systemStatus = isHomed ? "Ready" : "Not Homed";
  
  String curtainPosition;
  if (closedPosition > 0) {
    curtainPosition = (currentPosition < (closedPosition * 0.1)) ? "Open" : "Closed";
  } else {
    curtainPosition = "Not Calibrated";
  }

  String connectionStatus = positionLost ? "Position Lost" : "Connected";

  html.replace("{SYSTEM_STATUS}", systemStatus);
  html.replace("{CURTAIN_POSITION}", curtainPosition);
  html.replace("{CONNECTION_STATUS}", connectionStatus);

  server.send(200, "text/html", html);
}

void handleControl() {
  if (!server.hasArg("action")) {
    server.send(400, "text/plain", "No action");
    return;
  }

  String action = server.arg("action");
  Serial.println("Command: " + action);

  if (action == "stop") {
    motorRunning = false;
    positionLost = true;
    Serial.println("STOP");
  }
  else if (action == "home") {
    if (motorRunning) {
      motorRunning = false;
      delay(200);
    }
    server.send(200, "text/plain", "Homing");
    autoHome();
    positionLost = false;
    return;
  }
  else if (action == "manual" && server.hasArg("direction")) {
    if (motorRunning) {
      server.send(200, "text/plain", "Busy");
      return;
    }

    String direction = server.arg("direction");
    int speedDelay = NORMAL_SPEED_DELAY;

    if (currentPosition >= (closedPosition * 0.8) && direction == "up") {
      speedDelay = HEAVY_LOAD_DELAY;
    }

    server.send(200, "text/plain", "Moving");
    if (direction == "up") {
      digitalWrite(DIR_PIN, HIGH);
      runMotorSteps(MANUAL_STEP_SIZE, speedDelay, true, true);
      currentPosition = max(0, currentPosition - MANUAL_STEP_SIZE);
    } else if (direction == "down") {
      digitalWrite(DIR_PIN, LOW);
      runMotorSteps(MANUAL_STEP_SIZE, NORMAL_SPEED_DELAY, false, false);
      currentPosition += MANUAL_STEP_SIZE;
    }
    EEPROM.put(EEPROM_CURR_POS_ADDRESS, currentPosition);
    EEPROM.commit();
    publishCoverState();
    return;
  }
  else if (action == "save") {
    if (currentPosition > 50) {
      closedPosition = currentPosition;
      EEPROM.put(EEPROM_ADDRESS, closedPosition);
      EEPROM.commit();
      Serial.println("Saved: " + String(closedPosition));
    } else {
      server.send(400, "text/plain", "Too close to home");
      return;
    }
  }
  else if (action == "open") {
    if (motorRunning) {
      server.send(200, "text/plain", "Busy");
      return;
    }
    server.send(200, "text/plain", "Opening");
    autoHome();
    positionLost = false;
    return;
  }
  else if (action == "close" && closedPosition > 0) {
    if (motorRunning) {
      server.send(200, "text/plain", "Busy");
      return;
    }
    server.send(200, "text/plain", "Closing");
    if (positionLost) {
      autoHome();
      delay(500);
    }
    moveToPosition(closedPosition);
    return;
  }

  server.send(200, "text/plain", "OK");
}

void autoHome() {
  Serial.println("=== HOMING ===");
  
  motorRunning = true;
  delay(500);

  bool initialSensor = digitalRead(HOME_SENSOR_PIN);
  
  if (initialSensor == HIGH) {
    Serial.println("Already home");
    currentPosition = 0;
    isHomed = true;
    positionLost = false;
    motorRunning = false;
    EEPROM.put(EEPROM_CURR_POS_ADDRESS, currentPosition);
    EEPROM.commit();
    publishCoverState();
    return;
  }

  Serial.println("Finding home...");
  digitalWrite(DIR_PIN, HIGH);

  unsigned long startTime = millis();
  int stepCount = 0;
  int consecutiveHighReadings = 0;
  const int SENSOR_CONFIRM_COUNT = 5;
  int estimatedTotal = (currentPosition > 0) ? currentPosition : closedPosition;

  while (true) {
    if (millis() - startTime > HOMING_TIMEOUT_MS) {
      Serial.println("TIMEOUT");
      positionLost = true;
      motorRunning = false;
      // Error beep pattern - three short bursts
      for (int i = 0; i < 3; i++) {
        motorBeep(800, 200);
        delay(150);
      }
      return;
    }

    if (stepCount % 50 == 0) {
      server.handleClient();
      if (mqtt.connected()) mqtt.loop();
      if (!motorRunning) {
        Serial.println("Homing stopped");
        positionLost = true;
        return;
      }
    }

    // Dynamic speed: slow (0-40%), fast (40-85%), slow approach (85-100%)
    int dynamicDelay = HOMING_SPEED_DELAY;
    if (estimatedTotal > 0) {
      float progress = (float)stepCount / estimatedTotal;
      if (progress > 0.85f) {
        dynamicDelay = HOMING_SPEED_DELAY;  // slow approach to sensor
      } else if (progress > 0.4f) {
        dynamicDelay = NORMAL_SPEED_DELAY;  // fast middle section
      }
    }

    digitalWrite(STEP_PIN, HIGH);
    delayMicroseconds(50);
    digitalWrite(STEP_PIN, LOW);
    delayMicroseconds(dynamicDelay);

    stepCount++;

    bool sensorReading = digitalRead(HOME_SENSOR_PIN);

    if (sensorReading == HIGH) {
      consecutiveHighReadings++;
      if (consecutiveHighReadings >= SENSOR_CONFIRM_COUNT) {
        Serial.println("HOME FOUND");
        currentPosition = 0;
        isHomed = true;
        positionLost = false;
        motorRunning = false;
        EEPROM.put(EEPROM_CURR_POS_ADDRESS, currentPosition);
        EEPROM.commit();
        publishCoverState();
        return;
      }
    } else {
      consecutiveHighReadings = 0;
    }

    if (stepCount % 500 == 0) {
      Serial.print(".");
    }
  }
}

void moveToPosition(int targetPosition) {
  if (targetPosition == currentPosition) {
    return;
  }

  int stepsToMove = abs(targetPosition - currentPosition);
  bool moveUp = (targetPosition < currentPosition);

  Serial.println("Moving to " + String(targetPosition));

  int speedDelay = NORMAL_SPEED_DELAY;
  if (currentPosition >= (closedPosition * 0.8) && moveUp) {
    speedDelay = HEAVY_LOAD_DELAY;
  }

  digitalWrite(DIR_PIN, moveUp ? HIGH : LOW);
  runMotorSteps(stepsToMove, speedDelay, moveUp, moveUp);

  currentPosition = targetPosition;
  Serial.println("Done - Position: " + String(currentPosition));
  EEPROM.put(EEPROM_CURR_POS_ADDRESS, currentPosition);
  EEPROM.commit();
  publishCoverState();
}

void runMotorSteps(int steps, int delayMicros, bool direction, bool checkSensor) {
  digitalWrite(DIR_PIN, direction ? HIGH : LOW);
  motorRunning = true;

  int consecutiveHighReadings = 0;
  const int SENSOR_CONFIRM_COUNT = 3;

  for(int i = 0; i < steps && motorRunning; i++) {
    if (i % 10 == 0) {
      server.handleClient();
      if (mqtt.connected()) mqtt.loop();
      if (!motorRunning) {
        Serial.println("Stopped");
        break;
      }
    }

    if (checkSensor && direction) {
      bool sensorReading = digitalRead(HOME_SENSOR_PIN);

      if (sensorReading == HIGH) {
        consecutiveHighReadings++;
        if (consecutiveHighReadings >= SENSOR_CONFIRM_COUNT) {
          Serial.println("Home sensor hit");
          currentPosition = 0;
          positionLost = false;
          break;
        }
      } else {
        consecutiveHighReadings = 0;
      }
    }

    digitalWrite(STEP_PIN, HIGH);
    delayMicroseconds(50);
    digitalWrite(STEP_PIN, LOW);
    delayMicroseconds(delayMicros);
  }

  motorRunning = false;
}

// ===== MOSFET device handlers =====

void handleDeskOn() {
  server.send(200, "text/plain", "Desk fading on");
  fadeDesk(100);
}

void handleDeskOff() {
  server.send(200, "text/plain", "Desk fading off");
  fadeDesk(0);
}

void handleDeskBrightness() {
  if (!server.hasArg("level")) {
    server.send(400, "text/plain", "Missing level (0-100)");
    return;
  }
  int level = constrain(server.arg("level").toInt(), 0, 100);
  server.send(200, "text/plain", "Desk fading to " + String(level) + "%");
  fadeDesk(level);
}

void handleChargerOn() {
  chargerOn = true;
  digitalWrite(CHARGER_PIN, HIGH);
  server.send(200, "text/plain", "Charger ON");
}

void handleChargerOff() {
  chargerOn = false;
  digitalWrite(CHARGER_PIN, LOW);
  server.send(200, "text/plain", "Charger OFF");
}

void handleCorridorOn() {
  corridorOn = true;
  digitalWrite(CORRIDOR_PIN, HIGH);
  server.send(200, "text/plain", "Corridor ON");
}

void handleCorridorOff() {
  corridorOn = false;
  digitalWrite(CORRIDOR_PIN, LOW);
  server.send(200, "text/plain", "Corridor OFF");
}

void handleDesk2On() {
  server.send(200, "text/plain", "Desk2 fading on");
  fadeDesk2(100);
}

void handleDesk2Off() {
  server.send(200, "text/plain", "Desk2 fading off");
  fadeDesk2(0);
}

void handleDesk2Brightness() {
  if (!server.hasArg("level")) {
    server.send(400, "text/plain", "Missing level (0-100)");
    return;
  }
  int level = constrain(server.arg("level").toInt(), 0, 100);
  server.send(200, "text/plain", "Desk2 fading to " + String(level) + "%");
  fadeDesk2(level);
}


// Fades desk lamp smoothly from current duty to target percent (0-100)
// Full range fade (0→100 or 100→0) takes ~500ms
void fadeDesk(int targetPercent) {
  int targetDuty = map(targetPercent, 0, 100, 0, 255);
  if (deskCurrentDuty == targetDuty) return;
  int direction = (targetDuty > deskCurrentDuty) ? 1 : -1;
  while (deskCurrentDuty != targetDuty) {
    deskCurrentDuty += direction * 3;
    if ((direction > 0 && deskCurrentDuty > targetDuty) ||
        (direction < 0 && deskCurrentDuty < targetDuty)) {
      deskCurrentDuty = targetDuty;
    }
    ledcWrite(DESK_LAMP_LEDC_CHANNEL, deskCurrentDuty);
    delay(6);
  }
  deskBrightness = targetPercent;
}

void fadeDesk2(int targetPercent) {
  int targetDuty = map(targetPercent, 0, 100, 0, 255);
  if (desk2CurrentDuty == targetDuty) return;
  int direction = (targetDuty > desk2CurrentDuty) ? 1 : -1;
  while (desk2CurrentDuty != targetDuty) {
    desk2CurrentDuty += direction * 3;
    if ((direction > 0 && desk2CurrentDuty > targetDuty) ||
        (direction < 0 && desk2CurrentDuty < targetDuty)) {
      desk2CurrentDuty = targetDuty;
    }
    ledcWrite(DESK_LAMP2_LEDC_CHANNEL, desk2CurrentDuty);
    delay(6);
  }
  desk2Brightness = targetPercent;
}

// Steps motor back and forth at audio frequency to produce a tone with near-zero net movement
void motorBeep(int frequency, int durationMs) {
  int halfPeriodUs = 1000000 / (frequency * 2);
  unsigned long endTime = millis() + durationMs;
  int stepCount = 0;
  while (millis() < endTime) {
    // Alternate direction every 4 microsteps — net movement is negligible
    digitalWrite(DIR_PIN, (stepCount / 4) % 2 == 0 ? HIGH : LOW);
    digitalWrite(STEP_PIN, HIGH);
    delayMicroseconds(50);
    digitalWrite(STEP_PIN, LOW);
    delayMicroseconds(halfPeriodUs - 50);
    stepCount++;
  }
  digitalWrite(DIR_PIN, HIGH);  // restore default direction
}
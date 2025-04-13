//Normal version
#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <AsyncWebSocket.h>
#include <AsyncJson.h>
#include <time.h> // Include time.h for date manipulation

const char* ssid = "VM6159022";
const char* password = "rcgc4yyYdfrt";

const int RELAY_UP = 12;
const int RELAY_DOWN = 14;
const int BUTTON_UP = 32;
const int BUTTON_DOWN = 33;

unsigned long CURTAIN_UP_TIME;
unsigned long CURTAIN_DOWN_TIME;

bool isMoving = false;
unsigned long moveStartTime = 0;
unsigned long moveDuration = 0;

unsigned long lastDebounceTimeUp = 0;
unsigned long lastDebounceTimeDown = 0;
int lastButtonStateUp = HIGH;
int lastButtonStateDown = HIGH;
int buttonStateUp = HIGH;
int buttonStateDown = HIGH;
unsigned long debounceDelay = 50;

AsyncWebServer server(80);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "uk.pool.ntp.org", 0, 60000);

struct Schedule {
  bool enabled;
  int upHour;
  int upMinute;
  int downHour;
  int downMinute;
} weekSchedule[7];

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body {
            background-color: #000000;
            color: #00ff00;
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            margin: 0;
            padding: 20px;
        }
        .container {
            max-width: 800px;
            margin: 0 auto;
            padding: 20px;
            border: 1px solid #00ff00;
            border-radius: 8px;
            background-color: #111111;
        }
        .button {
            background-color: #006600;
            border: none;
            color: #00ff00;
            padding: 15px 32px;
            text-align: center;
            text-decoration: none;
            display: inline-block;
            font-size: 16px;
            margin: 4px 2px;
            cursor: pointer;
            border-radius: 20px;
            transition: all 0.3s;
        }
        .button:hover {
            background-color: #00ff00;
            color: #000000;
        }
        .settings {
            background-color: #222222;
            padding: 20px;
            border-radius: 8px;
            margin-top: 20px;
        }
        .schedule-day {
            margin: 10px 0;
            padding: 10px;
            border: 1px solid #00ff00;
            border-radius: 4px;
        }
        input[type="number"], input[type="time"] {
            background-color: #333333;
            border: 1px solid #00ff00;
            color: #00ff00;
            padding: 5px;
            border-radius: 4px;
            margin: 5px 0;
        }
        .slider {
            -webkit-appearance: none;
            width: 100%;
            height: 15px;
            border-radius: 5px;
            background: #333333;
            outline: none;
            opacity: 0.8;
            transition: opacity .2s;
        }
        .slider::-webkit-slider-thumb {
            -webkit-appearance: none;
            appearance: none;
            width: 25px;
            height: 25px;
            border-radius: 50%;
            background: #00ff00;
            cursor: pointer;
        }
        .slider::-moz-range-thumb {
            width: 25px;
            height: 25px;
            border-radius: 50%;
            background: #00ff00;
            cursor: pointer;
        }
        h1, h2, h3 {
            color: #00cc00;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>Smart Curtain Control</h1>
        
        <div class="manual-control">
            <h2>Manual Control</h2>
            <button class="button" onclick="controlCurtain('up')">UP</button>
            <button class="button" onclick="controlCurtain('down')">DOWN</button>
            <button class="button" onclick="controlCurtain('stop')">STOP</button>
        </div>

        <div class="settings">
            <h2>Motor Settings</h2>
            <label>Up Time (ms): </label>
            <input type="range" id="upTime" class="slider" min="1000" max="10000" step="100" value="5000" oninput="updateUpTimeLabel(this.value)" onchange="applySettings()">
            <span id="upTimeLabel">5000</span> ms
            <br><br>
            <label>Down Time (ms): </label>
            <input type="range" id="downTime" class="slider" min="1000" max="10000" step="100" value="5000" oninput="updateDownTimeLabel(this.value)" onchange="applySettings()">
            <span id="downTimeLabel">5000</span> ms
        </div>

        <div class="settings">
            <h2>Schedule</h2>
            <div id="scheduleContainer"></div>
        </div>
    </div>

    <script>
        const days = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'];
        const scheduleContainer = document.getElementById('scheduleContainer');
        
        days.forEach((day, index) => {
            const dayDiv = document.createElement('div');
            dayDiv.className = 'schedule-day';
            dayDiv.innerHTML = `
                <h3>${day}</h3>
                <input type="checkbox" id="enable${index}" onchange="saveSchedule(${index})">
                <label>Enable</label><br>
                <label>Up Time: </label>
                <input type="time" id="upTime${index}" onchange="saveSchedule(${index})"><br>
                <label>Down Time: </label>
                <input type="time" id="downTime${index}" onchange="saveSchedule(${index})">
            `;
            scheduleContainer.appendChild(dayDiv);
        });

        function updateUpTimeLabel(value) {
            document.getElementById('upTimeLabel').innerText = value;
        }

        function updateDownTimeLabel(value) {
            document.getElementById('downTimeLabel').innerText = value;
        }

        function applySettings() {
            const upTime = document.getElementById('upTime').value;
            const downTime = document.getElementById('downTime').value;
            
            fetch('/settings', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify({upTime: Number(upTime), downTime: Number(downTime)})
            })
            .then(response => {
                if (response.ok) {
                    return response.json();
                }
                throw new Error('Network response was not ok.');
            })
            .then(data => {
                console.log('Settings updated:', data);
            })
            .catch(error => {
                console.error('Error:', error);
            });
        }

        function controlCurtain(action) {
            fetch(`/curtain/${action}`);
        }

        function saveSchedule(dayIndex) {
            const enabled = document.getElementById(`enable${dayIndex}`).checked;
            const upTime = document.getElementById(`upTime${dayIndex}`).value;
            const downTime = document.getElementById(`downTime${dayIndex}`).value;
            fetch('/schedule', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({dayIndex, enabled, upTime, downTime})
            })
            .then(response => {
                if (response.ok) {
                    return response.json();
                }
                throw new Error('Network response was not ok.');
            })
            .then(data => {
                console.log('Schedule updated:', data);
            })
            .catch(error => {
                console.error('Error:', error);
            });
        }

        fetch('/settings')
            .then(response => response.json())
            .then(data => {
                document.getElementById('upTime').value = data.upTime;
                document.getElementById('downTime').value = data.downTime;
                updateUpTimeLabel(data.upTime);
                updateDownTimeLabel(data.downTime);
            });

        fetch('/schedule')
            .then(response => response.json())
            .then(data => {
                data.forEach((schedule, index) => {
                    document.getElementById(`enable${index}`).checked = schedule.enabled;
                    document.getElementById(`upTime${index}`).value = 
                        schedule.upHour.toString().padStart(2, '0') + ':' + schedule.upMinute.toString().padStart(2, '0');
                    document.getElementById(`downTime${index}`).value = 
                        schedule.downHour.toString().padStart(2, '0') + ':' + schedule.downMinute.toString().padStart(2, '0');
                });
            });
    </script>
</body>
</html>
)rawliteral";

int calculateBSTOffset(int day, int month, int weekday) {
  // BST starts on the last Sunday of March
  if (month == 3) {
    int lastSunday = 31 - ((31 - day + weekday) % 7);
    if (day >= lastSunday) return 3600; // BST active
  }
  // BST ends on the last Sunday of October
  else if (month == 10) {
    int lastSunday = 31 - ((31 - day + weekday) % 7);
    if (day < lastSunday) return 3600; // BST active
  }
  // BST active between April and September
  else if (month > 3 && month < 10) {
    return 3600; // BST active
  }
  return 0; // No BST
}

void updateTimeOffset() {
  timeClient.update();
  time_t rawTime = timeClient.getEpochTime(); // Get current time as Unix timestamp
  struct tm* timeInfo = gmtime(&rawTime);     // Convert to UTC time structure

  int day = timeInfo->tm_mday;                // Extract day
  int month = timeInfo->tm_mon + 1;           // Extract month (tm_mon is 0-based)
  int weekday = timeInfo->tm_wday;            // Extract weekday (0 = Sunday)

  int bstOffset = calculateBSTOffset(day, month, weekday);
  timeClient.setTimeOffset(0 + bstOffset);    // UTC + BST offset
}

void stopMotor() {
  digitalWrite(RELAY_UP, LOW);
  digitalWrite(RELAY_DOWN, LOW);
  isMoving = false;
}

void moveCurtainUp() {
  if (isMoving) {
    stopMotor();
    delay(500);
  }
  
  digitalWrite(RELAY_DOWN, LOW);
  digitalWrite(RELAY_UP, HIGH);
  isMoving = true;
  moveStartTime = millis();
  moveDuration = CURTAIN_UP_TIME;
}

void moveCurtainDown() {
  if (isMoving) {
    stopMotor();
    delay(500);
  }
  
  digitalWrite(RELAY_UP, LOW);
  digitalWrite(RELAY_DOWN, HIGH);
  isMoving = true;
  moveStartTime = millis();
  moveDuration = CURTAIN_DOWN_TIME;
}

void saveSettingsToSPIFFS() {
  File file = SPIFFS.open("/settings.json", "w");
  if (!file) return;

  StaticJsonDocument<1024> doc;
  doc["upTime"] = CURTAIN_UP_TIME;
  doc["downTime"] = CURTAIN_DOWN_TIME;
  
  JsonArray scheduleArray = doc.createNestedArray("schedule");
  for (int i = 0; i < 7; i++) {
    JsonObject scheduleObj = scheduleArray.createNestedObject();
    scheduleObj["enabled"] = weekSchedule[i].enabled;
    scheduleObj["upHour"] = weekSchedule[i].upHour;
    scheduleObj["upMinute"] = weekSchedule[i].upMinute;
    scheduleObj["downHour"] = weekSchedule[i].downHour;
    scheduleObj["downMinute"] = weekSchedule[i].downMinute;
  }

  serializeJson(doc, file);
  file.close();
}

void loadSettingsFromSPIFFS() {
  if (!SPIFFS.exists("/settings.json")) {
    saveSettingsToSPIFFS();
    return;
  }

  File file = SPIFFS.open("/settings.json", "r");
  if (!file) return;

  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) return;

  CURTAIN_UP_TIME = doc["upTime"];
  CURTAIN_DOWN_TIME = doc["downTime"];

  JsonArray scheduleArray = doc["schedule"];
  for (int i = 0; i < 7 && i < scheduleArray.size(); i++) {
    weekSchedule[i].enabled = scheduleArray[i]["enabled"] | false;
    weekSchedule[i].upHour = scheduleArray[i]["upHour"] | 7;
    weekSchedule[i].upMinute = scheduleArray[i]["upMinute"] | 0;
    weekSchedule[i].downHour = scheduleArray[i]["downHour"] | 22;
    weekSchedule[i].downMinute = scheduleArray[i]["downMinute"] | 0;
  }
}

void checkSchedule() {
  int currentDay = timeClient.getDay();
  int currentHour = timeClient.getHours();
  int currentMinute = timeClient.getMinutes();
  
  if (weekSchedule[currentDay].enabled) {
    if (currentHour == weekSchedule[currentDay].upHour && 
        currentMinute == weekSchedule[currentDay].upMinute) {
      moveCurtainUp();
    }
    else if (currentHour == weekSchedule[currentDay].downHour && 
             currentMinute == weekSchedule[currentDay].downMinute) {
      moveCurtainDown();
    }
  }
}

void setup() {
  Serial.begin(115200);
  
  if (!SPIFFS.begin(true)) return;

  pinMode(RELAY_UP, OUTPUT);
  pinMode(RELAY_DOWN, OUTPUT);
  pinMode(BUTTON_UP, INPUT_PULLUP);
  pinMode(BUTTON_DOWN, INPUT_PULLUP);
  digitalWrite(RELAY_UP, LOW);
  digitalWrite(RELAY_DOWN, LOW);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }
  Serial.println("Connected to WiFi");

  timeClient.begin();
  updateTimeOffset(); // Set initial time offset

  // Add this debug print
  Serial.print("Current time: ");
  Serial.print(timeClient.getHours());
  Serial.print(":");
  Serial.print(timeClient.getMinutes());
  Serial.print(":");
  Serial.println(timeClient.getSeconds());

  loadSettingsFromSPIFFS();

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", index_html);
  });

  server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request){
    StaticJsonDocument<200> doc;
    doc["upTime"] = CURTAIN_UP_TIME;
    doc["downTime"] = CURTAIN_DOWN_TIME;
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  server.on("/schedule", HTTP_GET, [](AsyncWebServerRequest *request){
    StaticJsonDocument<1024> doc;
    JsonArray array = doc.to<JsonArray>();
    for (int i = 0; i < 7; i++) {
      JsonObject scheduleObj = array.createNestedObject();
      scheduleObj["enabled"] = weekSchedule[i].enabled;
      scheduleObj["upHour"] = weekSchedule[i].upHour;
      scheduleObj["upMinute"] = weekSchedule[i].upMinute;
      scheduleObj["downHour"] = weekSchedule[i].downHour;
      scheduleObj["downMinute"] = weekSchedule[i].downMinute;
    }
    String response;
    serializeJson(array, response);
    request->send(200, "application/json", response);
  });

  server.on("/curtain/up", HTTP_GET, [](AsyncWebServerRequest *request){
    moveCurtainUp();
    request->send(200, "application/json", "{\"status\":\"OK\"}");
  });

  server.on("/curtain/down", HTTP_GET, [](AsyncWebServerRequest *request){
    moveCurtainDown();
    request->send(200, "application/json", "{\"status\":\"OK\"}");
  });

  server.on("/curtain/stop", HTTP_GET, [](AsyncWebServerRequest *request){
    stopMotor();
    request->send(200, "application/json", "{\"status\":\"OK\"}");
  });

  AsyncCallbackJsonWebHandler* settingsHandler = new AsyncCallbackJsonWebHandler("/settings", [](AsyncWebServerRequest *request, JsonVariant json) {
    JsonObject jsonObj = json.as<JsonObject>();
    if (jsonObj.containsKey("upTime") && jsonObj.containsKey("downTime")) {
      CURTAIN_UP_TIME = jsonObj["upTime"].as<unsigned long>();
      CURTAIN_DOWN_TIME = jsonObj["downTime"].as<unsigned long>();
      saveSettingsToSPIFFS();
      request->send(200, "application/json", "{\"status\":\"Settings updated\",\"upTime\":" + String(CURTAIN_UP_TIME) + ",\"downTime\":" + String(CURTAIN_DOWN_TIME) + "}");
    } else {
      request->send(400, "application/json", "{\"error\":\"Missing required fields\"}");
    }
  });
  server.addHandler(settingsHandler);

  AsyncCallbackJsonWebHandler* scheduleHandler = new AsyncCallbackJsonWebHandler("/schedule", [](AsyncWebServerRequest *request, JsonVariant json) {
    JsonObject jsonObj = json.as<JsonObject>();
    int dayIndex = jsonObj["dayIndex"] | 0;
    if (dayIndex >= 0 && dayIndex < 7) {
      weekSchedule[dayIndex].enabled = jsonObj["enabled"] | false;
      String upTime = jsonObj["upTime"] | "07:00";
      String downTime = jsonObj["downTime"] | "22:00";
      weekSchedule[dayIndex].upHour = upTime.substring(0, 2).toInt();
      weekSchedule[dayIndex].upMinute = upTime.substring(3, 5).toInt();
      weekSchedule[dayIndex].downHour = downTime.substring(0, 2).toInt();
      weekSchedule[dayIndex].downMinute = downTime.substring(3, 5).toInt();
      saveSettingsToSPIFFS();
      request->send(200, "application/json", "{\"status\":\"Schedule updated\"}");
    } else {
      request->send(400, "application/json", "{\"error\":\"Missing required fields\"}");
    }
  });
  server.addHandler(scheduleHandler);
  
  server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(404);
  });

  server.begin();
}

void loop() {
  timeClient.update();
  
  // Add this debug print every minute
  static unsigned long lastTimePrint = 0;
  if (millis() - lastTimePrint >= 60000) {
    Serial.print("Time now: ");
    Serial.print(timeClient.getHours());
    Serial.print(":");
    Serial.print(timeClient.getMinutes());
    Serial.print(":");
    Serial.println(timeClient.getSeconds());
    lastTimePrint = millis();
  }
  
  if (isMoving && (millis() - moveStartTime >= moveDuration)) {
    stopMotor();
  }
  
  static unsigned long lastScheduleCheck = 0;
  if (millis() - lastScheduleCheck >= 60000) {
    checkSchedule();
    lastScheduleCheck = millis();
  }

  static unsigned long lastTimeUpdate = 0;
  if (millis() - lastTimeUpdate >= 3600000) { // Update offset every hour
    updateTimeOffset();
    lastTimeUpdate = millis();
  }

  int readingUp = digitalRead(BUTTON_UP);
  int readingDown = digitalRead(BUTTON_DOWN);

  if (readingUp != lastButtonStateUp) {
    lastDebounceTimeUp = millis();
  }

  if ((millis() - lastDebounceTimeUp) > debounceDelay) {
    if (readingUp != buttonStateUp) {
      buttonStateUp = readingUp;
      if (buttonStateUp == LOW) {
        moveCurtainUp();
      }
    }
  }
  lastButtonStateUp = readingUp;

  if (readingDown != lastButtonStateDown) {
    lastDebounceTimeDown = millis();
  }

  if ((millis() - lastDebounceTimeDown) > debounceDelay) {
    if (readingDown != buttonStateDown) {
      buttonStateDown = readingDown;
      if (buttonStateDown == LOW) {
        moveCurtainDown();
      }
    }
  }
  lastButtonStateDown = readingDown;

  delay(10);
}



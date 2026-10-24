#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include "DHT.h"
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include <math.h>

// ================= 硬件引脚 =================
#define PIN_CLK 32
#define PIN_DT  33
#define PIN_SW  25
#define TRIG_PIN 5
#define ECHO_PIN 18
#define BUZZER_PIN 4
#define RELAY_PIN 16
#define DHTPIN 15
#define HEART_PIN 34
#define IR_PIN 14
#define SOUND_PIN 35
#define DHTTYPE DHT11

// ================= 对象与全局变量 =================
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, 22, 21);
DHT dht(DHTPIN, DHTTYPE);
WebServer server(80);

// 菜单与状态
String menuItems[] = {"Radar Sensor", "Temp & Humid", "Relay Control", "Heartbeat", "Sound Meter", "IR Receiver", "WiFi & Time", "System Info"};
int totalItems = 8;
int menuIndex = 0;
int displayStart = 0;
bool inSubMode = false;
int relaySubMenu = 0; // 0: 主状态 1: 设置参数

// 继电器高级状态
enum RelayMode { MANUAL, LOCAL_TIMER, SCHEDULE };
RelayMode relayMode = MANUAL;
bool relayState = false;

// 定时参数
unsigned long pulseDuration = 500;      // 单次脉冲时长 (ms)
unsigned long localOnTime = 2000;       // 离线持续开启时长 (ms)
unsigned long localInterval = 5000;     // 离线间隔时长 (ms)
String scheduleOnTime = "08:00";        // 联网定时开
String scheduleOffTime = "20:00";       // 联网定时关

unsigned long lastRelayAction = 0;
bool isPulsing = false;
bool scheduleTriggeredOn = false;
bool scheduleTriggeredOff = false;

// 传感器缓存
float currentDistance = 0;
float currentTemp = 0, currentHum = 0;
int bpm = 0;
float currentDB = 0;
int soundMax = 0, soundMin = 4095;
String currentTimeStr = "No Net";
String apSSID = "ESP32_Smart_Home";
String apPass = "12345678";
bool apState = true;
bool staConnected = false;

// 心跳波形缓存
#define WAVE_SIZE 100
int heartWave[WAVE_SIZE] = {0};
int waveIndex = 0;

// 非阻塞定时器
unsigned long lastRadarTime = 0;
unsigned long lastDHTTime = 0;
unsigned long lastHeartTime = 0;
unsigned long lastSoundTime = 0;
unsigned long lastStatusTime = 0;

// 编码器
int lastCLKState;
int clickCount = 0;
unsigned long firstClickTime = 0;

// 函数声明
void handleRoot();
void handleData();
void handleWave();
void handleToggleRelay();
void handlePulse();
void handleSetRelay();
void drawMainMenu();
void runRelayMode();
void runRadarMode();
void runTempHumidMode();
void runHeartbeatMode();
void runSoundMode();
void runIRMode();
void runWifiInfoMode();
void runSystemInfoMode();

// ================= 前端网页 =================
const char* htmlPage = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="UTF-8">
<title>ESP32 终极物联中枢</title>
<script src="https://cdn.jsdelivr.net/npm/echarts@5.4.3/dist/echarts.min.js"></script>
<style>
body { font-family: sans-serif; background: #111; color: #fff; text-align: center; margin: 0; padding: 20px; }
.grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); gap: 20px; }
.card { background: #222; padding: 20px; border-radius: 10px; border: 1px solid #333; box-shadow: 0 4px 8px rgba(0,0,0,0.5); text-align: left;}
.btn { padding: 10px 15px; font-size: 16px; margin: 5px 0; cursor: pointer; border: none; border-radius: 5px; background: #007BFF; color: white; width: 100%;}
.btn-on { background: #4CAF50; }
.btn-off { background: #f44336; }
.data { font-size: 24px; color: #00BFFF; font-weight: bold; }
input, select { padding: 8px; margin: 5px 0; border-radius: 5px; border: 1px solid #444; background: #333; color: white; width: 90%;}
.row { display: flex; justify-content: space-between; gap: 10px; align-items: center;}
#chart { width: 100%; height: 300px; }
</style></head><body>
<h1>ESP32 终极物联中枢</h1>
<div class="grid">
  <div class="card">
    <h2>状态监控</h2>
    <p>时间: <span class="data" id="time">--:--:--</span></p>
    <p>温度: <span class="data" id="temp">--</span> C | 湿度: <span class="data" id="hum">--</span> %</p>
    <p>距离: <span class="data" id="dist">--</span> cm | 声音: <span class="data" id="db">--</span> dB</p>
    <p>心率: <span class="data" id="bpm">--</span> BPM</p>
    <p>内存: <span class="data" id="ram">--</span> KB</p>
  </div>
  
  <div class="card">
    <h2>继电器高级控制</h2>
    <p>当前状态: <span class="data" id="relayState">OFF</span> | 模式: <span id="relayMode">MANUAL</span></p>
    <div class="row">
        <button class="btn btn-on" onclick="fetch('/toggleRelay')">手动开启</button>
        <button class="btn btn-off" onclick="fetch('/toggleRelay')">手动关闭</button>
    </div>
    <hr>
    <label>单次脉冲时长 (ms): </label><input type="number" id="pulseMs" value="%PULSE%">
    <button class="btn" style="background:#ff9800;" onclick="fetch('/pulse?ms='+document.getElementById('pulseMs').value)">触发单次脉冲</button>
    <hr>
    <label>模式选择:</label>
    <select id="modeSel" onchange="updateMode()">
        <option value="0" %M0%>Manual 手动</option>
        <option value="1" %M1%>Local Timer 离线循环</option>
        <option value="2" %M2%>Schedule 联网定时</option>
    </select>
    <div id="timerPanel" style="display:%TIMER_DISP%">
        <label>间隔 (ms): </label><input type="number" id="localInt" value="%LINT%">
        <label>开启时长 (ms): </label><input type="number" id="localOn" value="%LON%">
    </div>
    <div id="schedPanel" style="display:%SCHED_DISP%">
        <label>开启时间 (HH:MM): </label><input type="text" id="schedOn" value="%SON%">
        <label>关闭时间 (HH:MM): </label><input type="text" id="schedOff" value="%SOFF%">
    </div>
    <button class="btn" style="background:#2196F3; margin-top:15px;" onclick="saveRelaySettings()">保存设置</button>
    <hr>
    <h3>配网 (连接路由器)</h3>
    <input type="text" id="ssid" placeholder="WiFi 账号">
    <input type="password" id="pwd" placeholder="WiFi 密码">
    <button class="btn" onclick="connectWiFi()">连接</button>
    <p id="wifiStatus">%WIFI_STATUS%</p>
  </div>
</div>
<div class="card">
  <h2>心率实时波形</h2>
  <div id="chart"></div>
</div>

<script>
var chart = echarts.init(document.getElementById('chart'), 'dark');
chart.setOption({ backgroundColor: 'transparent', xAxis: { type: 'category', show: false }, yAxis: { type: 'value', show: false, min: 'dataMin', max: 'dataMax' }, series: [{ data: [], type: 'line', smooth: true, lineStyle: { color: '#f44336', width: 3 }, showSymbol: false }] });

function updateData() {
  fetch('/api/data').then(r => r.json()).then(d => {
    document.getElementById('time').innerText = d.time;
    document.getElementById('temp').innerText = d.temp;
    document.getElementById('hum').innerText = d.hum;
    document.getElementById('dist').innerText = d.dist;
    document.getElementById('db').innerText = d.db;
    document.getElementById('bpm').innerText = d.bpm;
    document.getElementById('ram').innerText = d.ram;
    document.getElementById('wifiStatus').innerText = d.staStatus;
    document.getElementById('relayState').innerText = d.relayState ? "ON" : "OFF";
    document.getElementById('relayMode').innerText = d.relayMode;
  }).catch(e => {});
}

function updateWave() {
  fetch('/api/wave').then(r => r.json()).then(d => {
    chart.setOption({ series: [{ data: d }] });
  }).catch(e => {});
}

function updateMode() {
    let mode = document.getElementById('modeSel').value;
    document.getElementById('timerPanel').style.display = (mode == '1') ? 'block' : 'none';
    document.getElementById('schedPanel').style.display = (mode == '2') ? 'block' : 'none';
}

function saveRelaySettings() {
    let mode = document.getElementById('modeSel').value;
    let url = `/api/setRelay?mode=${mode}`;
    if(mode == '1') {
        url += `&lint=${document.getElementById('localInt').value}&lon=${document.getElementById('localOn').value}`;
    } else if(mode == '2') {
        url += `&son=${document.getElementById('schedOn').value}&soff=${document.getElementById('schedOff').value}`;
    }
    fetch(url).then(() => alert('设置已保存！'));
}

function connectWiFi() {
  let s = document.getElementById('ssid').value;
  let p = document.getElementById('pwd').value;
  fetch('/api/connect?ssid=' + s + '&pwd=' + p).then(r => alert('指令已发送'));
}

setInterval(updateData, 2000);
setInterval(updateWave, 100);
</script>
</body></html>
)rawliteral";

// ================= Web 处理 =================
void handleRoot() {
  String html = htmlPage;
  html.replace("%PULSE%", String(pulseDuration));
  html.replace("%LINT%", String(localInterval));
  html.replace("%LON%", String(localOnTime));
  html.replace("%SON%", scheduleOnTime);
  html.replace("%SOFF%", scheduleOffTime);
  
  html.replace("%M0%", relayMode == MANUAL ? "selected" : "");
  html.replace("%M1%", relayMode == LOCAL_TIMER ? "selected" : "");
  html.replace("%M2%", relayMode == SCHEDULE ? "selected" : "");
  
  html.replace("%TIMER_DISP%", relayMode == LOCAL_TIMER ? "block" : "none");
  html.replace("%SCHED_DISP%", relayMode == SCHEDULE ? "block" : "none");
  html.replace("%WIFI_STATUS%", staConnected ? ("已连接: " + WiFi.localIP().toString()) : "未连接路由器");
  server.send(200, "text/html", html);
}

void handleData() {
  String modeStr = relayMode == MANUAL ? "MANUAL" : (relayMode == LOCAL_TIMER ? "TIMER" : "SCHEDULE");
  String json = "{";
  json += "\"time\":\"" + currentTimeStr + "\",";
  json += "\"temp\":" + String(currentTemp, 1) + ",";
  json += "\"hum\":" + String(currentHum, 1) + ",";
  json += "\"dist\":" + String(currentDistance, 1) + ",";
  json += "\"db\":" + String(currentDB, 1) + ",";
  json += "\"bpm\":" + String(bpm) + ",";
  json += "\"ram\":" + String(ESP.getFreeHeap() / 1024) + ",";
  json += "\"relayState\":" + String(relayState ? "true" : "false") + ",";
  json += "\"relayMode\":\"" + modeStr + "\",";
  json += "\"staStatus\":\"" + String(staConnected ? "已连接" : "未连接") + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handleWave() {
  String json = "[";
  for (int i = 0; i < WAVE_SIZE; i++) {
    int idx = (waveIndex + i) % WAVE_SIZE;
    json += String(heartWave[idx]);
    if (i < WAVE_SIZE - 1) json += ",";
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleToggleRelay() {
  if (relayMode == MANUAL) {
    relayState = !relayState;
    digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handlePulse() {
  if (server.hasArg("ms")) pulseDuration = server.arg("ms").toInt();
  digitalWrite(RELAY_PIN, HIGH);
  relayState = true;
  isPulsing = true;
  lastRelayAction = millis();
  server.send(200, "text/plain", "Pulse Triggered");
}

void handleSetRelay() {
  if (server.hasArg("mode")) {
    int m = server.arg("mode").toInt();
    relayMode = (RelayMode)m;
    
    if (relayMode == LOCAL_TIMER) {
      localInterval = server.arg("lint").toInt();
      localOnTime = server.arg("lon").toInt();
      lastRelayAction = millis();
    } else if (relayMode == SCHEDULE) {
      scheduleOnTime = server.arg("son");
      scheduleOffTime = server.arg("soff");
      scheduleTriggeredOn = false;
      scheduleTriggeredOff = false;
    } else {
      digitalWrite(RELAY_PIN, relayState ? HIGH : LOW); // 恢复手动状态
    }
  }
  server.send(200, "text/plain", "OK");
}

// ================= 初始化 =================
void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  
  pinMode(PIN_CLK, INPUT_PULLUP);
  pinMode(PIN_DT, INPUT_PULLUP);
  pinMode(PIN_SW, INPUT_PULLUP);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(IR_PIN, INPUT);
  
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(RELAY_PIN, LOW); 
  analogSetAttenuation(ADC_11db);

  dht.begin();
  u8g2.begin();
  u8g2.setFont(u8g2_font_6x10_tf); 

  WiFi.softAP(apSSID.c_str(), apPass.c_str());
  configTime(8 * 3600, 0, "ntp.aliyun.com", "time.pool.aliyun.com");

  server.on("/", handleRoot);
  server.on("/api/data", handleData);
  server.on("/api/wave", handleWave);
  server.on("/toggleRelay", handleToggleRelay);
  server.on("/pulse", handlePulse);
  server.on("/api/setRelay", handleSetRelay);
  server.begin();

  lastCLKState = digitalRead(PIN_CLK);
  drawMainMenu();
}

// ================= 主循环 =================
void loop() {
  server.handleClient(); 
  
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 10)) {
    char timeStr[6];
    strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo);
    currentTimeStr = String(timeStr);
  }

  // 核心：处理继电器的各种模式
  unsigned long now = millis();
  
  if (isPulsing) {
    if (now - lastRelayAction >= pulseDuration) {
      digitalWrite(RELAY_PIN, LOW);
      relayState = false;
      isPulsing = false;
    }
  } 
  else if (relayMode == LOCAL_TIMER) {
    unsigned long elapsed = now - lastRelayAction;
    if (relayState) {
      if (elapsed >= localOnTime) {
        relayState = false;
        digitalWrite(RELAY_PIN, LOW);
        lastRelayAction = now;
      }
    } else {
      if (elapsed >= localInterval) {
        relayState = true;
        digitalWrite(RELAY_PIN, HIGH);
        lastRelayAction = now;
      }
    }
  } 
  else if (relayMode == SCHEDULE && staConnected) {
    if (currentTimeStr == scheduleOnTime && !scheduleTriggeredOn) {
      relayState = true;
      digitalWrite(RELAY_PIN, HIGH);
      scheduleTriggeredOn = true;
      scheduleTriggeredOff = false;
    } else if (currentTimeStr == scheduleOffTime && !scheduleTriggeredOff) {
      relayState = false;
      digitalWrite(RELAY_PIN, LOW);
      scheduleTriggeredOff = true;
      scheduleTriggeredOn = false;
    }
  }

  // 处理路由器连接状态
  if (WiFi.status() == WL_CONNECTED && !staConnected) staConnected = true;
  else if (WiFi.status() != WL_CONNECTED && staConnected) staConnected = false;

  // 编码器与按键
  int currentCLKState = digitalRead(PIN_CLK);
  if (currentCLKState != lastCLKState && currentCLKState == LOW) {
    if (digitalRead(PIN_DT) != currentCLKState) menuIndex = (menuIndex + 1) % totalItems;
    else menuIndex = (menuIndex - 1 + totalItems) % totalItems;
    if (!inSubMode) drawMainMenu();
  }
  lastCLKState = currentCLKState;

  if (digitalRead(PIN_SW) == LOW) {
    delay(20);
    if (digitalRead(PIN_SW) == LOW) {
      unsigned long clickNow = millis();
      if (clickCount == 0 || (clickNow - firstClickTime > 350)) {
        clickCount = 1; firstClickTime = clickNow;
      } else {
        clickCount = 2;
      }
      while(digitalRead(PIN_SW) == LOW);
    }
  }

  if (clickCount > 0 && (millis() - firstClickTime > 350)) {
    if (clickCount == 1) {
      if (!inSubMode) {
        if (menuIndex == 2) { // 主菜单点击进入继电器高级模式
          inSubMode = true;
          u8g2.clearBuffer();
        } else {
          inSubMode = true;
          u8g2.clearBuffer();
        }
      } else {
        if (menuIndex == 2) { // 在继电器子菜单里点击，切换手动状态
          if (relayMode == MANUAL) {
            relayState = !relayState;
            digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);
          }
        }
      }
    } else if (clickCount == 2) {
      if (inSubMode) {
        inSubMode = false;
        drawMainMenu();
      }
    }
    clickCount = 0; 
  }

  // 子模式执行
  if (inSubMode) {
    if (menuIndex == 0) runRadarMode();
    else if (menuIndex == 1) runTempHumidMode();
    else if (menuIndex == 2) runRelayMode(); // 继电器高级显示
    else if (menuIndex == 3) runHeartbeatMode();
    else if (menuIndex == 4) runSoundMode();
    else if (menuIndex == 5) runIRMode();
    else if (menuIndex == 6) runWifiInfoMode();
    else if (menuIndex == 7) runSystemInfoMode();
  } else {
    if (millis() - lastStatusTime > 1000) {
      lastStatusTime = millis();
      drawMainMenu(); 
    }
  }
}

// ================= UI 绘制 =================
void drawMainMenu() {
  u8g2.clearBuffer();
  u8g2.drawStr(0, 10, "=== System Menu ===");
  u8g2.drawStr(98, 10, currentTimeStr.c_str()); 

  if (menuIndex < displayStart) displayStart = menuIndex;
  if (menuIndex >= displayStart + 5) displayStart = menuIndex - 4;

  for (int i = 0; i < 5; i++) {
    int itemIndex = displayStart + i;
    if (itemIndex >= totalItems) break;
    u8g2.setCursor(0, 22 + (i * 10));
    if (itemIndex == menuIndex) u8g2.print("> ");
    else u8g2.print("  ");
    
    if (itemIndex == 2) { 
      u8g2.print("Relay ["); u8g2.print(relayState ? "ON" : "OFF"); u8g2.print("]");
    } else {
      u8g2.print(menuItems[itemIndex]);
    }
  }
  u8g2.sendBuffer();
}

// 新增：继电器高级状态显示界面
void runRelayMode() {
  u8g2.clearBuffer();
  u8g2.drawStr(0, 10, "--- Relay Advanced ---");
  
  String modeStr = relayMode == MANUAL ? "MANUAL" : (relayMode == LOCAL_TIMER ? "LOCAL TIMER" : "SCHEDULE");
  u8g2.setCursor(0, 25); u8g2.print("Mode: "); u8g2.print(modeStr);
  u8g2.setCursor(0, 35); u8g2.print("State: "); u8g2.print(relayState ? "ON" : "OFF");
  
  if (relayMode == MANUAL) {
    u8g2.setCursor(0, 50); u8g2.print("Click to toggle ON/OFF");
  } else if (relayMode == LOCAL_TIMER) {
    u8g2.setCursor(0, 45); u8g2.print("Interval: "); u8g2.print(localInterval / 1000); u8g2.print("s");
    u8g2.setCursor(0, 55); u8g2.print("Pulse  : "); u8g2.print(localOnTime); u8g2.print("ms");
  } else if (relayMode == SCHEDULE) {
    u8g2.setCursor(0, 45); u8g2.print("ON  At: "); u8g2.print(scheduleOnTime);
    u8g2.setCursor(0, 55); u8g2.print("OFF At: "); u8g2.print(scheduleOffTime);
  }
  u8g2.drawStr(0, 64, "Dbl click to exit");
  u8g2.sendBuffer();
}

// 心率模块：动态滑动阈值 + 波谷检测（终极修复版）
void runHeartbeatMode() {
  if (millis() - lastHeartTime > 30) { 
    lastHeartTime = millis();
    int raw = analogRead(HEART_PIN);
    
    static int lastValidRaw = 2000;
    if (abs(raw - lastValidRaw) > 300) raw = lastValidRaw;
    else lastValidRaw = raw;
    
    static float filteredVal = 2000;
    filteredVal = (filteredVal * 0.6) + (raw * 0.4);

    bool fingerDetected = (filteredVal < 2500 && filteredVal > 200);

    static bool wasDetected = false;
    if (!fingerDetected) {
      bpm = 0;
      if (wasDetected) {
        for (int i = 0; i < WAVE_SIZE; i++) heartWave[i] = 0;
        wasDetected = false;
      }
      
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_ncenB10_tr);
      u8g2.setCursor(0, 25); u8g2.print("Place Finger");
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.drawStr(0, 60, "Dbl click to exit");
      u8g2.sendBuffer();
      return; 
    }
    wasDetected = true;

    heartWave[waveIndex] = (int)filteredVal;
    waveIndex = (waveIndex + 1) % WAVE_SIZE;

    static int buffer[30] = {0};
    static int bufIndex = 0;
    buffer[bufIndex] = filteredVal;
    bufIndex = (bufIndex + 1) % 30;

    int maxV = 0, minV = 4095;
    for (int i = 0; i < 30; i++) {
      if (buffer[i] > maxV) maxV = buffer[i];
      if (buffer[i] < minV && buffer[i] > 0) minV = buffer[i];
    }
    int amplitude = maxV - minV;
    bool signalValid = (amplitude > 30); 

    int valleyThreshold = minV + (amplitude * 0.3); 

    static bool isInValley = false;
    static unsigned long lastBeatTime = 0;
    unsigned long now = millis();

    if (signalValid && filteredVal < valleyThreshold && !isInValley) {
      if (now - lastBeatTime > 400 && now - lastBeatTime < 1500) {
        int instantBPM = 60000 / (now - lastBeatTime);
        if (bpm == 0) bpm = instantBPM;
        else bpm = (bpm * 0.85) + (instantBPM * 0.15); 
        lastBeatTime = now;
      } else if (now - lastBeatTime >= 1500) {
        lastBeatTime = now; 
      }
      isInValley = true;
    }
    
    if (filteredVal > valleyThreshold + 10) {
      isInValley = false;
    }

    if (now - lastBeatTime > 3000) bpm = 0;

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB10_tr);
    u8g2.setCursor(0, 12);
    
    if (!signalValid) {
      u8g2.print("Signal Weak");
    } else if (bpm > 40 && bpm < 150) {
      u8g2.print(bpm); u8g2.print(" BPM");
    } else {
      u8g2.print("Detecting...");
    }

    int dispMax = maxV, dispMin = minV;
    if (dispMax - dispMin < 50) dispMax = dispMin + 50;
    
    int prevX = 0, prevY = 0;
    for (int i = 0; i < WAVE_SIZE; i++) {
      int idx = (waveIndex + i) % WAVE_SIZE;
      int y = map(heartWave[idx], dispMin, dispMax, 60, 20); 
      y = constrain(y, 20, 60); 
      
      if (i > 0) {
        if (abs(y - prevY) < 20) u8g2.drawLine(prevX, prevY, i, y);
      }
      prevX = i;
      prevY = y;
    }
    
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 64, "Dbl click to exit");
    u8g2.sendBuffer();
  }
}

void runRadarMode() {
  if (millis() - lastRadarTime > 100) {
    lastRadarTime = millis();
    digitalWrite(TRIG_PIN, LOW); delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);
    long duration = pulseIn(ECHO_PIN, HIGH, 30000);
    if (duration == 0) currentDistance = 999;
    else currentDistance = duration * 0.034 / 2;

    if (currentDistance < 10.0) {
      if ((millis() / 50) % 2 == 0) digitalWrite(BUZZER_PIN, HIGH); else digitalWrite(BUZZER_PIN, LOW);
    } else if (currentDistance < 30.0) {
      if ((millis() / 200) % 2 == 0) digitalWrite(BUZZER_PIN, HIGH); else digitalWrite(BUZZER_PIN, LOW);
    } else { digitalWrite(BUZZER_PIN, LOW); }

    u8g2.clearBuffer(); u8g2.drawStr(0, 10, "--- Radar ---");
    u8g2.setFont(u8g2_font_ncenB24_tr); u8g2.setCursor(5, 40);
    u8g2.print(currentDistance); u8g2.print(" cm");
    u8g2.setFont(u8g2_font_6x10_tf); u8g2.drawStr(0, 60, "Dbl click to exit");
    u8g2.sendBuffer();
  }
}

void runTempHumidMode() {
  if (millis() - lastDHTTime > 2000) {
    lastDHTTime = millis();
    float t = dht.readTemperature(); float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) { currentTemp = t; currentHum = h; }
    u8g2.clearBuffer(); u8g2.drawStr(0, 10, "--- Temp & Humid ---");
    u8g2.setFont(u8g2_font_ncenB14_tr);
    u8g2.setCursor(0, 30); u8g2.print(currentTemp, 1); u8g2.print(" C");
    u8g2.setCursor(0, 50); u8g2.print(currentHum, 1); u8g2.print(" %");
    u8g2.setFont(u8g2_font_6x10_tf); u8g2.drawStr(0, 60, "Dbl click to exit");
    u8g2.sendBuffer();
  }
}

void runSoundMode() {
  if (millis() - lastSoundTime > 50) {
    lastSoundTime = millis();
    int max_val = 0, min_val = 4095;
    for(int i=0; i<50; i++) {
      int v = analogRead(SOUND_PIN);
      if(v > max_val) max_val = v;
      if(v < min_val) min_val = v;
    }
    soundMax = max_val; soundMin = min_val;
    int vpp = soundMax - soundMin;
    if (vpp > 0) currentDB = 20.0 * log10(vpp) + 30.0; else currentDB = 30.0;
    if (currentDB > 120) currentDB = 120;
    if (currentDB < 30) currentDB = 30;

    u8g2.clearBuffer(); u8g2.drawStr(0, 10, "--- Sound Meter ---");
    u8g2.setFont(u8g2_font_ncenB24_tr); u8g2.setCursor(0, 35);
    u8g2.print(currentDB, 1); u8g2.setFont(u8g2_font_6x10_tf); u8g2.print(" dB");
    u8g2.setCursor(0, 45); u8g2.print("Peak: "); u8g2.print(soundMax);
    u8g2.setCursor(0, 55); u8g2.print("Vpp : "); u8g2.print(vpp);
    u8g2.drawStr(0, 64, "Dbl click to exit"); u8g2.sendBuffer();
  }
}

void runIRMode() {
  if (digitalRead(IR_PIN) == LOW) {
    static unsigned long lastIRSignal = 0;
    lastIRSignal = millis();
    u8g2.clearBuffer(); u8g2.drawStr(0, 10, "--- IR Receiver ---");
    u8g2.setFont(u8g2_font_ncenB10_tr); u8g2.setCursor(0, 30); u8g2.print("Signal Detected!");
    u8g2.setFont(u8g2_font_6x10_tf); u8g2.drawStr(0, 60, "Dbl click to exit");
    u8g2.sendBuffer();
    delay(50); 
  } else {
    static unsigned long lastIRSignal = 0;
    if (millis() - lastIRSignal > 1500) {
      u8g2.clearBuffer(); u8g2.drawStr(0, 10, "--- IR Receiver ---");
      u8g2.setFont(u8g2_font_6x10_tf); u8g2.setCursor(0, 30); u8g2.print("Waiting...");
      u8g2.drawStr(0, 60, "Dbl click to exit"); u8g2.sendBuffer();
    }
    delay(50);
  }
}

void runWifiInfoMode() {
  u8g2.clearBuffer();
  u8g2.drawStr(0, 10, "--- WiFi & Time ---");
  u8g2.setCursor(0, 25); u8g2.print("AP SSID : "); u8g2.print(apSSID);
  u8g2.setCursor(0, 35); u8g2.print("AP PWD  : "); u8g2.print(apPass);
  u8g2.setCursor(0, 45); u8g2.print("STA IP  : "); u8g2.print(staConnected ? WiFi.localIP().toString() : "None");
  u8g2.setCursor(0, 55); u8g2.print("Time    : "); u8g2.print(currentTimeStr);
  u8g2.drawStr(0, 64, "Dbl click to exit");
  u8g2.sendBuffer();
}

void runSystemInfoMode() {
  u8g2.clearBuffer();
  u8g2.drawStr(0, 10, "--- System Info ---");
  u8g2.setCursor(0, 25); u8g2.print("AP Clients: "); u8g2.print(apState ? WiFi.softAPgetStationNum() : 0);
  u8g2.setCursor(0, 35); u8g2.print("Free RAM : "); u8g2.print(ESP.getFreeHeap() / 1024); u8g2.print(" KB");
  u8g2.setCursor(0, 45); u8g2.print("Chip Temp: "); u8g2.print((int)temperatureRead()); u8g2.print(" C");
  u8g2.setCursor(0, 55); u8g2.print("Core Ver : "); u8g2.print(ESP.getSdkVersion());
  u8g2.drawStr(0, 64, "Dbl click to exit");
  u8g2.sendBuffer();
}

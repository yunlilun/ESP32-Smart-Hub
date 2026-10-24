#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include "DHT.h"
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include <math.h>
#include <IRremote.hpp>  // 红外解码库

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

char jsonBuffer[2048]; 

// 菜单与状态
String menuItems[] = {"Radar Sensor", "Temp & Humid", "Relay Control", "Heartbeat", "Sound Meter", "IR Receiver", "WiFi & Time", "System Info", "Factory Reset"};
int totalItems = 9;
int menuIndex = 0;
int displayStart = 0;
bool inSubMode = false;
int resetConfirmStep = 0; 

// 继电器高级状态
enum RelayMode { MANUAL, LOCAL_TIMER, SCHEDULE };
RelayMode relayMode = MANUAL;
bool relayState = false;

// 定时参数
unsigned long pulseDuration = 500;      
unsigned long localOnTime = 2000;       
unsigned long localInterval = 5000;     
String scheduleOnTime = "08:00";        
String scheduleOffTime = "20:00";       

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
bool staConnected = false;

// 红外缓存
String irProtocol = "Unknown";
String irAddress = "0x00";
String irCommand = "0x00";
bool hasIRData = false;
unsigned long lastIRReceiveTime = 0;

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

// 屏幕保护与交互
unsigned long lastInteractionTime = 0;
unsigned long screenTimeout = 300000; 
bool screenOn = true;

// 编码器按键状态
int lastCLKState;
int clickCount = 0;
unsigned long firstClickTime = 0;
unsigned long buttonPressStart = 0;
bool isButtonPressed = false;
bool isLongPressing = false;

// 函数声明
void handleRoot();
void handleData();
void handleWave();
void handleToggleRelay();
void handlePulse();
void handleSetRelay();
void handleConnect();
void handleSetScreen();
void handleReset();
void drawMainMenu();
void runRelayMode();
void runRadarMode();
void runTempHumidMode();
void runHeartbeatMode();
void runSoundMode();
void runIRMode();
void runWifiInfoMode();
void runSystemInfoMode();
void runResetMode();
void updateInteraction();

// ================= 前端网页 =================
const char* htmlPage = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="UTF-8">
<title>ESP32 Smart Hub</title>
<style>
body { font-family: sans-serif; background: #111; color: #fff; text-align: center; margin: 0; padding: 20px; }
.grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); gap: 20px; }
.card { background: #222; padding: 20px; border-radius: 10px; border: 1px solid #333; box-shadow: 0 4px 8px rgba(0,0,0,0.5); text-align: left;}
.btn { padding: 10px 15px; font-size: 16px; margin: 5px 0; cursor: pointer; border: none; border-radius: 5px; background: #007BFF; color: white; width: 100%;}
.btn-on { background: #4CAF50; }
.btn-off { background: #f44336; }
.btn-warn { background: #ff9800; }
.data { font-size: 24px; color: #00BFFF; font-weight: bold; }
input, select { padding: 8px; margin: 5px 0; border-radius: 5px; border: 1px solid #444; background: #333; color: white; width: 90%;}
.row { display: flex; justify-content: space-between; gap: 10px; align-items: center;}
canvas { width: 100%; height: 250px; background: #000; border-radius: 5px; margin-top: 10px; }
</style></head><body>
<h1>ESP32 Smart Hub</h1>
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
    <button class="btn btn-warn" onclick="fetch('/pulse?ms='+document.getElementById('pulseMs').value)">触发单次脉冲</button>
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
    <h3>屏幕与系统</h3>
    <label>熄屏时间 (分钟): </label><input type="number" id="screenMin" value="%SCREEN_MIN%">
    <button class="btn" style="background:#9C27B0;" onclick="saveScreenSettings()">应用屏幕设置</button>
    <button class="btn btn-warn" style="margin-top:10px;" onclick="resetSystem()">一键恢复出厂设置</button>
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
  <canvas id="waveCanvas"></canvas>
</div>

<script>
var canvas = document.getElementById('waveCanvas');
var ctx = canvas.getContext('2d');
var waveData = [];

function resizeCanvas() {
    canvas.width = canvas.clientWidth;
    canvas.height = canvas.clientHeight;
}
window.addEventListener('resize', resizeCanvas);
resizeCanvas();

function drawWave() {
    ctx.fillStyle = '#000';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    if (waveData.length < 2) return;
    let maxV = Math.max(...waveData), minV = Math.min(...waveData);
    if (maxV - minV < 50) maxV = minV + 50;
    ctx.beginPath(); ctx.strokeStyle = '#f44336'; ctx.lineWidth = 2; ctx.lineJoin = 'round';
    for (let i = 0; i < waveData.length; i++) {
        let x = (i / (waveData.length - 1)) * canvas.width;
        let y = canvas.height - ((waveData[i] - minV) / (maxV - minV)) * (canvas.height - 20) - 10;
        if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
    }
    ctx.stroke();
}

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
    if (Array.isArray(d) && d.length > 0) { waveData = d; drawWave(); }
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
    if(mode == '1') url += `&lint=${document.getElementById('localInt').value}&lon=${document.getElementById('localOn').value}`;
    else if(mode == '2') url += `&son=${document.getElementById('schedOn').value}&soff=${document.getElementById('schedOff').value}`;
    fetch(url).then(() => alert('设置已保存！'));
}

function saveScreenSettings() {
    let min = document.getElementById('screenMin').value;
    fetch('/api/setScreen?min=' + min).then(() => alert('屏幕设置已更新！'));
}

function resetSystem() {
    if(confirm("警告：确定要恢复出厂设置吗？\n所有的定时参数、屏幕设置将被清空。")) {
        if(confirm("最后确认：请再次点击确定！\n确认后系统将自动重启并恢复默认设置。")) {
            fetch('/reset').then(() => {
                alert('系统正在重置，请稍后重新连接热点 ESP32_Smart_Home');
            });
        }
    }
}

function connectWiFi() {
  let s = document.getElementById('ssid').value, p = document.getElementById('pwd').value;
  fetch('/api/connect?ssid=' + s + '&pwd=' + p).then(r => alert('指令已发送'));
}

setInterval(updateData, 2000);
setInterval(updateWave, 300);
</script>
</body></html>
)rawliteral";

// ================= Web 处理函数 =================
void handleRoot() {
  updateInteraction();
  String html = htmlPage;
  html.replace("%PULSE%", String(pulseDuration));
  html.replace("%LINT%", String(localInterval));
  html.replace("%LON%", String(localOnTime));
  html.replace("%SON%", scheduleOnTime);
  html.replace("%SOFF%", scheduleOffTime);
  html.replace("%SCREEN_MIN%", String(screenTimeout / 60000));
  
  html.replace("%M0%", relayMode == MANUAL ? "selected" : "");
  html.replace("%M1%", relayMode == LOCAL_TIMER ? "selected" : "");
  html.replace("%M2%", relayMode == SCHEDULE ? "selected" : "");
  
  html.replace("%TIMER_DISP%", relayMode == LOCAL_TIMER ? "block" : "none");
  html.replace("%SCHED_DISP%", relayMode == SCHEDULE ? "block" : "none");
  html.replace("%WIFI_STATUS%", staConnected ? ("已连接: " + WiFi.localIP().toString()) : "未连接路由器");
  server.send(200, "text/html", html);
}

void handleData() {
  updateInteraction();
  snprintf(jsonBuffer, sizeof(jsonBuffer), 
    "{\"time\":\"%s\",\"temp\":%.1f,\"hum\":%.1f,\"dist\":%.1f,\"db\":%.1f,\"bpm\":%d,\"ram\":%d,\"relayState\":%s,\"relayMode\":\"%s\",\"staStatus\":\"%s\"}",
    currentTimeStr.c_str(), currentTemp, currentHum, currentDistance, currentDB, bpm, ESP.getFreeHeap() / 1024, 
    relayState ? "true" : "false", relayMode == MANUAL ? "MANUAL" : (relayMode == LOCAL_TIMER ? "TIMER" : "SCHEDULE"), 
    staConnected ? "已连接" : "未连接"
  );
  server.send(200, "application/json", jsonBuffer);
}

void handleWave() {
  updateInteraction();
  int len = 0;
  len += snprintf(jsonBuffer + len, sizeof(jsonBuffer) - len, "[");
  for (int i = 0; i < WAVE_SIZE; i++) {
    int idx = (waveIndex + i) % WAVE_SIZE;
    len += snprintf(jsonBuffer + len, sizeof(jsonBuffer) - len, "%d", heartWave[idx]);
    if (i < WAVE_SIZE - 1) len += snprintf(jsonBuffer + len, sizeof(jsonBuffer) - len, ",");
    if (len > sizeof(jsonBuffer) - 20) break; 
  }
  len += snprintf(jsonBuffer + len, sizeof(jsonBuffer) - len, "]");
  server.send(200, "application/json", jsonBuffer);
}

void handleToggleRelay() {
  updateInteraction();
  if (relayMode == MANUAL) {
    relayState = !relayState;
    digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handlePulse() {
  updateInteraction();
  if (server.hasArg("ms")) pulseDuration = server.arg("ms").toInt();
  digitalWrite(RELAY_PIN, HIGH);
  relayState = true;
  isPulsing = true;
  lastRelayAction = millis();
  server.send(200, "text/plain", "Pulse Triggered");
}

void handleSetRelay() {
  updateInteraction();
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
    }
    digitalWrite(RELAY_PIN, relayState ? HIGH : LOW); 
  }
  server.send(200, "text/plain", "OK");
}

void handleSetScreen() {
  updateInteraction();
  if (server.hasArg("min")) {
    int m = server.arg("min").toInt();
    if (m < 1) m = 1;
    screenTimeout = m * 60000;
  }
  server.send(200, "text/plain", "OK");
}

void handleConnect() {
  updateInteraction();
  if (server.hasArg("ssid") && server.hasArg("pwd")) {
    String s = server.arg("ssid");
    String p = server.arg("pwd");
    WiFi.begin(s.c_str(), p.c_str());
    server.send(200, "text/plain", "Connecting...");
  } else {
    server.send(400, "text/plain", "Missing args");
  }
}

void handleReset() {
  server.send(200, "text/plain", "Resetting...");
  delay(500);
  digitalWrite(RELAY_PIN, LOW);
  
  u8g2.setPowerSave(0);
  screenOn = true;
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB10_tr);
  u8g2.setCursor(0, 25); u8g2.print("System Resetting...");
  u8g2.sendBuffer();
  
  delay(1000);
  ESP.restart(); 
}

// ================= 交互更新 =================
void updateInteraction() {
  lastInteractionTime = millis();
  if (!screenOn) {
    screenOn = true;
    u8g2.setPowerSave(0); 
    drawMainMenu();
  }
}

// ================= 初始化 =================
void setup() {
  Serial.begin(115200);
  delay(1000); // 防掉电缓冲

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

  delay(500); // 防掉电缓冲

  WiFi.softAP(apSSID.c_str(), apPass.c_str());
  WiFi.setSleep(false); 
  WiFi.setTxPower(WIFI_POWER_8_5dBm); // 降低WiFi功耗防掉电
  configTime(8 * 3600, 0, "ntp.aliyun.com", "time.pool.aliyun.com");

  IrReceiver.begin(IR_PIN, DISABLE_LED_FEEDBACK); // 红外初始化

  server.on("/", handleRoot);
  server.on("/api/data", handleData);
  server.on("/api/wave", handleWave);
  server.on("/toggleRelay", handleToggleRelay);
  server.on("/pulse", handlePulse);
  server.on("/api/setRelay", handleSetRelay);
  server.on("/api/setScreen", handleSetScreen);
  server.on("/api/connect", handleConnect);
  server.on("/reset", handleReset);
  server.begin();

  lastCLKState = digitalRead(PIN_CLK);
  updateInteraction();
}

// ================= 主循环 =================
void loop() {
  server.handleClient(); 
  
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 10)) {
    char timeStr[10];
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
    currentTimeStr = String(timeStr);
  }

  // 红外解码逻辑
  if (IrReceiver.decode()) {
    if ((IrReceiver.decodedIRData.protocol != UNKNOWN) && !(IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT)) {
      irProtocol = String(getProtocolString(IrReceiver.decodedIRData.protocol));
      char buf[10];
      snprintf(buf, sizeof(buf), "0x%02X", IrReceiver.decodedIRData.address);
      irAddress = String(buf);
      snprintf(buf, sizeof(buf), "0x%02X", IrReceiver.decodedIRData.command);
      irCommand = String(buf);
      hasIRData = true;
      lastIRReceiveTime = millis();
    }
    IrReceiver.resume();
  }

  if (hasIRData && (millis() - lastIRReceiveTime > 10000)) {
    hasIRData = false;
  }

  // 屏幕超时休眠检测
  if (screenOn && (millis() - lastInteractionTime > screenTimeout)) {
    u8g2.setPowerSave(1);
    screenOn = false;
  }

  unsigned long now = millis();
  
  // 继电器逻辑
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

  if (WiFi.status() == WL_CONNECTED && !staConnected) staConnected = true;
  else if (WiFi.status() != WL_CONNECTED && staConnected) staConnected = false;

  // 编码器旋转检测
  int currentCLKState = digitalRead(PIN_CLK);
  if (currentCLKState != lastCLKState && currentCLKState == LOW) {
    updateInteraction(); 
    if (digitalRead(PIN_DT) != currentCLKState) menuIndex = (menuIndex + 1) % totalItems;
    else menuIndex = (menuIndex - 1 + totalItems) % totalItems;
    if (!inSubMode && screenOn) drawMainMenu();
  }
  lastCLKState = currentCLKState;

  // 按键逻辑
  if (digitalRead(PIN_SW) == LOW) {
    if (!isButtonPressed) {
      isButtonPressed = true;
      isLongPressing = false;
      buttonPressStart = millis();
      updateInteraction(); 
    } else {
      if (!isLongPressing && (millis() - buttonPressStart > 1500)) {
        isLongPressing = true;
        if (screenOn) {
          u8g2.setPowerSave(1);
          screenOn = false;
        } else {
          updateInteraction();
        }
      }
    }
  } else {
    if (isButtonPressed) {
      isButtonPressed = false;
      if (!isLongPressing) {
        unsigned long now = millis();
        if (clickCount == 0 || (now - firstClickTime > 350)) {
          clickCount = 1; firstClickTime = now;
        } else {
          clickCount = 2;
        }
      }
    }
  }

  if (clickCount > 0 && (millis() - firstClickTime > 350)) {
    if (clickCount == 1) {
      if (!inSubMode) {
        if (menuIndex == 8) { 
          inSubMode = true;
          resetConfirmStep = 0;
          u8g2.clearBuffer();
        } else if (menuIndex == 2) {
          if (relayMode == MANUAL) {
            relayState = !relayState;
            digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);
          }
        } else {
          inSubMode = true;
          u8g2.clearBuffer();
        }
      } else {
        if (menuIndex == 8) { 
          if (resetConfirmStep == 0) resetConfirmStep = 1;
          else if (resetConfirmStep == 1) resetConfirmStep = 2;
        }
        else if (menuIndex == 2 && relayMode == MANUAL) { 
          relayState = !relayState;
          digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);
        }
      }
    } else if (clickCount == 2) {
      if (inSubMode) {
        inSubMode = false;
        resetConfirmStep = 0;
        if (screenOn) drawMainMenu();
      }
    }
    clickCount = 0; 
  }

  // 子模式绘制
  if (inSubMode && screenOn) {
    if (menuIndex == 0) runRadarMode();
    else if (menuIndex == 1) runTempHumidMode();
    else if (menuIndex == 2) runRelayMode(); 
    else if (menuIndex == 3) runHeartbeatMode();
    else if (menuIndex == 4) runSoundMode();
    else if (menuIndex == 5) runIRMode();
    else if (menuIndex == 6) runWifiInfoMode();
    else if (menuIndex == 7) runSystemInfoMode();
    else if (menuIndex == 8) runResetMode();
  } else if (!inSubMode && screenOn) {
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
  u8g2.drawStr(100, 10, currentTimeStr.c_str()); 

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

void runResetMode() {
  u8g2.clearBuffer();
  u8g2.drawStr(0, 10, "=== Factory Reset ===");
  
  if (resetConfirmStep == 0) {
    u8g2.setCursor(0, 30); u8g2.print("Click to start...");
    u8g2.setCursor(0, 50); u8g2.print("Dbl click to exit");
  } 
  else if (resetConfirmStep == 1) {
    u8g2.setCursor(0, 30); u8g2.print("Confirm Reset?");
    u8g2.setCursor(0, 45); u8g2.print("Click AGAIN to confirm");
    u8g2.setCursor(0, 60); u8g2.print("Dbl click to exit");
  }
  else if (resetConfirmStep == 2) {
    u8g2.setCursor(0, 30); u8g2.print("Resetting now...");
    u8g2.sendBuffer();
    delay(1000);
    handleReset(); 
  }
  u8g2.sendBuffer();
}

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

// 心率模块：原版拉伸逻辑保持不动
void runHeartbeatMode() {
  if (millis() - lastHeartTime > 30) { 
    lastHeartTime = millis();
    int raw = analogRead(HEART_PIN);
    
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
        else bpm = (bpm * 0.7) + (instantBPM * 0.3); 
        lastBeatTime = now;
      } else if (now - lastBeatTime >= 1500) {
        lastBeatTime = now; 
      }
      isInValley = true;
    }
    
    if (filteredVal > valleyThreshold + 10) {
      isInValley = false;
    }

    if (now - lastBeatTime > 3000) {
      bpm = 0;
    }

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
    if (dispMax == dispMin) { dispMax += 10; dispMin -= 10; }
    
    int prevX = 0, prevY = 0;
    for (int i = 0; i < WAVE_SIZE; i++) {
      int idx = (waveIndex + i) % WAVE_SIZE;
      int y = map(heartWave[idx], dispMin, dispMax, 60, 20); 
      if (i > 0) u8g2.drawLine(prevX, prevY, i, y);
      prevX = i; prevY = y;
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
  u8g2.clearBuffer();
  u8g2.drawStr(0, 10, "--- IR Decoder ---");
  
  if (hasIRData) {
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.setCursor(0, 25); u8g2.print("Protocol: "); u8g2.print(irProtocol);
    u8g2.setCursor(0, 38); u8g2.print("Address : "); u8g2.print(irAddress);
    u8g2.setCursor(0, 51); u8g2.print("Command : "); u8g2.print(irCommand);
  } else {
    u8g2.setFont(u8g2_font_ncenB10_tr); 
    u8g2.setCursor(0, 35); u8g2.print("Waiting...");
    u8g2.setFont(u8g2_font_6x10_tf);
  }
  
  u8g2.setCursor(0, 64); u8g2.print("Hold 10s | Dbl click to exit");
  u8g2.sendBuffer();
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
  u8g2.setCursor(0, 25); u8g2.print("AP Clients: "); u8g2.print(WiFi.softAPgetStationNum());
  u8g2.setCursor(0, 35); u8g2.print("Free RAM : "); u8g2.print(ESP.getFreeHeap() / 1024); u8g2.print(" KB");
  u8g2.setCursor(0, 45); u8g2.print("Chip Temp: "); u8g2.print((int)temperatureRead()); u8g2.print(" C");
  u8g2.setCursor(0, 55); u8g2.print("Core Ver : "); u8g2.print(ESP.getSdkVersion());
  u8g2.drawStr(0, 64, "Dbl click to exit");
  u8g2.sendBuffer();
}

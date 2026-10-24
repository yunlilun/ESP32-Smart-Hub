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

// 网页 JSON 全局缓冲区，防止内存爆掉
char jsonBuffer[2048]; 

// 菜单与状态
String menuItems[] = {"Radar Sensor", "Temp & Humid", "Relay Control", "Heartbeat", "Sound Meter", "IR Receiver", "WiFi & Time", "System Info"};
int totalItems = 8;
int menuIndex = 0;
int displayStart = 0;
bool inSubMode = false;

bool relayState = false;
bool apState = true; 
String staSSID = "";
String staPass = "";
bool staConnected = false;

// 传感器缓存
float currentDistance = 0;
float currentTemp = 0, currentHum = 0;
int bpm = 0;
float currentDB = 0;
int soundMax = 0, soundMin = 4095;
String irCode = "Waiting...";
String currentTimeStr = "No Net";
String apSSID = "ESP32_Smart_Home";
String apPass = "12345678";

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
unsigned long lastWifiTime = 0;

// 编码器
int lastCLKState;
int clickCount = 0;
unsigned long firstClickTime = 0;

// ================= 函数前向声明 =================
void handleRoot();
void handleData();
void handleWave();
void handleToggleRelay();
void handleToggleAP();
void handleConnect();
void drawMainMenu();
void runRadarMode();
void runTempHumidMode();
void runHeartbeatMode();
void runSoundMode();
void runIRMode();
void runWifiInfoMode();
void runSystemInfoMode();

// ================= 前端网页 (纯原生 Canvas，断网也能秒开) =================
const char* htmlPage = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="UTF-8">
<title>ESP32 终极物联中枢</title>
<style>
body { font-family: sans-serif; background: #111; color: #fff; text-align: center; margin: 0; padding: 20px; }
.grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 20px; }
.card { background: #222; padding: 20px; border-radius: 10px; border: 1px solid #333; box-shadow: 0 4px 8px rgba(0,0,0,0.5); }
.btn { padding: 10px 20px; font-size: 16px; margin: 5px; cursor: pointer; border: none; border-radius: 5px; background: #007BFF; color: white; text-decoration: none; display: inline-block;}
.btn-on { background: #4CAF50; }
.btn-off { background: #f44336; }
.data { font-size: 24px; color: #00BFFF; font-weight: bold; }
input { padding: 8px; margin: 5px; border-radius: 5px; border: 1px solid #444; background: #333; color: white; width: 120px;}
canvas { width: 100%; height: 250px; background: #000; border-radius: 5px; margin-top: 10px; }
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
    <h2>模块控制</h2>
    <a href="/toggleRelay" class="btn %RELAY_CLASS%">继电器: %RELAY_STATUS%</a>
    <a href="/toggleAP" class="btn %AP_CLASS%">热点: %AP_STATUS%</a>
    <br><br>
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

    let maxV = Math.max(...waveData);
    let minV = Math.min(...waveData);
    if (maxV - minV < 50) maxV = minV + 50;

    ctx.beginPath();
    ctx.strokeStyle = '#f44336';
    ctx.lineWidth = 2;
    ctx.lineJoin = 'round';

    for (let i = 0; i < waveData.length; i++) {
        let x = (i / (waveData.length - 1)) * canvas.width;
        let y = canvas.height - ((waveData[i] - minV) / (maxV - minV)) * (canvas.height - 20) - 10;
        
        if (i === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
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
  }).catch(e => {});
}

function updateWave() {
  fetch('/api/wave').then(r => r.json()).then(d => {
    if (Array.isArray(d) && d.length > 0) { 
        waveData = d;
        drawWave();
    }
  }).catch(e => {});
}

function connectWiFi() {
  let s = document.getElementById('ssid').value;
  let p = document.getElementById('pwd').value;
  fetch('/api/connect?ssid=' + s + '&pwd=' + p).then(r => {
    alert('指令已发送，正在连接...');
  });
}

setInterval(updateData, 2000);
setInterval(updateWave, 200); // 降低请求频率到 200ms，防止网络卡死
</script>
</body></html>
)rawliteral";

// ================= Web 处理函数 =================
void handleRoot() {
  String html = htmlPage;
  html.replace("%RELAY_STATUS%", relayState ? "打开中" : "关闭中");
  html.replace("%RELAY_CLASS%", relayState ? "btn-on" : "btn-off");
  html.replace("%AP_STATUS%", apState ? "开启中" : "关闭中");
  html.replace("%AP_CLASS%", apState ? "btn-on" : "btn-off");
  html.replace("%WIFI_STATUS%", staConnected ? ("已连接: " + WiFi.localIP().toString()) : "未连接路由器");
  server.send(200, "text/html", html);
}

void handleData() {
  // 改用安全缓冲区，防止内存碎片导致断网
  snprintf(jsonBuffer, sizeof(jsonBuffer), 
    "{\"time\":\"%s\",\"temp\":%.1f,\"hum\":%.1f,\"dist\":%.1f,\"db\":%.1f,\"bpm\":%d,\"ram\":%d,\"staStatus\":\"%s\"}",
    currentTimeStr.c_str(), currentTemp, currentHum, currentDistance, currentDB, bpm, 
    ESP.getFreeHeap() / 1024, 
    staConnected ? "已连接" : "未连接"
  );
  server.send(200, "application/json", jsonBuffer);
}

void handleWave() {
  // 改用安全缓冲区
  int len = 0;
  len += snprintf(jsonBuffer + len, sizeof(jsonBuffer) - len, "[");
  for (int i = 0; i < WAVE_SIZE; i++) {
    int idx = (waveIndex + i) % WAVE_SIZE;
    len += snprintf(jsonBuffer + len, sizeof(jsonBuffer) - len, "%d", heartWave[idx]);
    if (i < WAVE_SIZE - 1) {
      len += snprintf(jsonBuffer + len, sizeof(jsonBuffer) - len, ",");
    }
    if (len > sizeof(jsonBuffer) - 20) break; 
  }
  len += snprintf(jsonBuffer + len, sizeof(jsonBuffer) - len, "]");
  server.send(200, "application/json", jsonBuffer);
}

void handleToggleRelay() {
  relayState = !relayState;
  digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);
  if(!inSubMode) drawMainMenu();
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleToggleAP() {
  apState = !apState;
  if(apState) {
    WiFi.softAP(apSSID.c_str(), apPass.c_str());
  } else {
    WiFi.softAPdisconnect(true);
  }
  if(!inSubMode) drawMainMenu();
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleConnect() {
  if (server.hasArg("ssid") && server.hasArg("pwd")) {
    staSSID = server.arg("ssid");
    staPass = server.arg("pwd");
    WiFi.begin(staSSID.c_str(), staPass.c_str());
    server.send(200, "text/plain", "Connecting...");
  } else {
    server.send(400, "text/plain", "Missing args");
  }
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
  WiFi.setSleep(false); // ⭐ 强制关闭 WiFi 休眠，保证网页秒开
  configTime(8 * 3600, 0, "ntp.aliyun.com", "time.pool.aliyun.com");

  server.on("/", handleRoot);
  server.on("/api/data", handleData);
  server.on("/api/wave", handleWave);
  server.on("/toggleRelay", handleToggleRelay);
  server.on("/toggleAP", handleToggleAP);
  server.on("/api/connect", handleConnect);
  server.begin();

  lastCLKState = digitalRead(PIN_CLK);
  drawMainMenu();
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

  if (WiFi.status() == WL_CONNECTED && !staConnected) {
    staConnected = true;
  } else if (WiFi.status() != WL_CONNECTED && staConnected) {
    staConnected = false;
  }

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
      unsigned long now = millis();
      if (clickCount == 0 || (now - firstClickTime > 350)) {
        clickCount = 1; firstClickTime = now;
      } else {
        clickCount = 2;
      }
      while(digitalRead(PIN_SW) == LOW);
    }
  }

  if (clickCount > 0 && (millis() - firstClickTime > 350)) {
    if (clickCount == 1) {
      if (!inSubMode) {
        if (menuIndex == 2) {
          relayState = !relayState;
          digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);
          drawMainMenu();
        } else {
          inSubMode = true;
          u8g2.clearBuffer();
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

  if (inSubMode) {
    if (menuIndex == 0) runRadarMode();
    else if (menuIndex == 1) runTempHumidMode();
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

// 心率模块：原封不动
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

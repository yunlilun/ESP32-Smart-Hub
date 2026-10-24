#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include "DHT.h"
#include <WiFi.h>
#include <WebServer.h>
#include <math.h>

// ================= 硬件引脚定义 =================
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
#define SOUND_PIN 35  // 新增声音传感器
#define DHTTYPE DHT11

// ================= 初始化对象 =================
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE, /* clock=*/ 22, /* data=*/ 21);
DHT dht(DHTPIN, DHTTYPE);
WebServer server(80);

// ================= 菜单与状态变量 =================
String menuItems[] = {"Radar Sensor", "Temp & Humid", "Relay Control", "Heartbeat", "Sound Meter", "IR Receiver", "WiFi Hotspot", "System Info"};
int totalItems = 8;
int menuIndex = 0;
int displayStart = 0;

bool inSubMode = false;
bool relayState = false;
bool apState = false; 

int lastCLKState;
int clickCount = 0;
unsigned long firstClickTime = 0;

// 传感器缓存
float currentDistance = 0;
float currentTemp = 0, currentHum = 0;
int bpm = 0;
float currentDB = 0;
int soundMax = 0, soundMin = 4095;
String irCode = "Waiting...";

// 非阻塞定时器
unsigned long lastRadarTime = 0;
unsigned long lastDHTTime = 0;
unsigned long lastStatusTime = 0;
unsigned long lastHeartTime = 0;
unsigned long lastSoundTime = 0;

// ================= 网页前端 (支持自动刷新与继电器/AP控制) =================
const char* htmlPage = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="UTF-8"><meta http-equiv="refresh" content="2">
<title>ESP32 Pro Hub</title>
<style>
body { font-family: Arial; background: #222; color: #fff; text-align: center; margin-top: 30px; }
.card { background: #333; padding: 20px; border-radius: 10px; display: inline-block; width: 350px; }
.btn { padding: 15px 30px; font-size: 18px; margin-top: 15px; cursor: pointer; border: none; border-radius: 5px; text-decoration: none; display: inline-block;}
.on { background: #4CAF50; color: white; }
.off { background: #f44336; color: white; }
.data { font-size: 20px; color: #00BFFF; font-weight: bold; }
</style></head><body>
<div class="card">
  <h2>ESP32 Pro Hub</h2>
  <hr>
  <p>Temp: <span class="data">%TEMP% °C</span> | Hum: <span class="data">%HUM% %</span></p>
  <p>BPM: <span class="data">%BPM%</span> | dB: <span class="data">%DB%</span></p>
  <p>Distance: <span class="data">%DIST% cm</span></p>
  <p>Clients: <span class="data">%DEVICES%</span> | Free RAM: <span class="data">%RAM% KB</span></p>
  <hr>
  <p>Relay Control:</p>
  <a href="/toggleRelay"><button class="btn %RELAY_BTN%">%RELAY_STATUS%</button></a>
</div>
</body></html>
)rawliteral";

void handleRoot() {
  String html = htmlPage;
  html.replace("%TEMP%", String(currentTemp, 1));
  html.replace("%HUM%", String(currentHum, 1));
  html.replace("%BPM%", String(bpm));
  html.replace("%DB%", String(currentDB, 1));
  html.replace("%DIST%", String(currentDistance, 1));
  html.replace("%DEVICES%", String(apState ? WiFi.softAPgetStationNum() : 0));
  html.replace("%RAM%", String(ESP.getFreeHeap() / 1024));
  html.replace("%RELAY_STATUS%", relayState ? "TURN OFF" : "TURN ON");
  html.replace("%RELAY_BTN%", relayState ? "off" : "on");
  server.send(200, "text/html", html);
}

void handleToggleRelay() {
  relayState = !relayState;
  digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);
  server.sendHeader("Location", "/");
  server.send(303); 
}

// ================= 核心：资源管理与功耗控制 =================
// ⭐ 每次切换模式，先调用此函数把不用的东西关掉，防止短路/过热
void manageHardware() {
  // 1. 永远保持关闭的执行器（进子模式时再按需打开）
  digitalWrite(BUZZER_PIN, LOW);
  
  // 2. 如果退出了继电器控制（或者只是平时待机），保持继电器状态不变（这是安全要求）
  // 但是你可以在这里加逻辑：如果不是 Relay 模式，强制拉低电平
  
  // 3. 关闭不需要的引脚
  if (!inSubMode || menuIndex != 0) {
    digitalWrite(TRIG_PIN, LOW); 
    pinMode(ECHO_PIN, INPUT); // 设为高阻态
  }
  if (!inSubMode || menuIndex != 1) {
    // 关闭DHT不需要特殊操作
  }
}

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
  // SOUND_PIN 34, 35 是输入，不需要 pinMode

  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(RELAY_PIN, LOW); 
  
  // 初始化 ADC 衰减（让声音传感器读取范围更大）
  analogSetAttenuation(ADC_11db);

  dht.begin();
  u8g2.begin();
  u8g2.setFont(u8g2_font_6x10_tf); 

  server.on("/", handleRoot);
  server.on("/toggleRelay", handleToggleRelay);

  lastCLKState = digitalRead(PIN_CLK);
  drawMainMenu();
}

void loop() {
  if (apState) server.handleClient();

  // 编码器与按键逻辑
  int currentCLKState = digitalRead(PIN_CLK);
  if (currentCLKState != lastCLKState && currentCLKState == LOW) {
    if (digitalRead(PIN_DT) != currentCLKState) menuIndex = (menuIndex + 1) % totalItems; 
    else menuIndex = (menuIndex - 1 + totalItems) % totalItems; 
    if (!inSubMode) { manageHardware(); drawMainMenu(); }
  }
  lastCLKState = currentCLKState;

  if (digitalRead(PIN_SW) == LOW) {
    delay(20); 
    if (digitalRead(PIN_SW) == LOW) {
      unsigned long now = millis();
      if (clickCount == 0 || (now - firstClickTime > 350)) { clickCount = 1; firstClickTime = now; }
      else { clickCount = 2; }
      while(digitalRead(PIN_SW) == LOW); 
    }
  }

  if (clickCount > 0 && (millis() - firstClickTime > 350)) {
    if (clickCount == 1) {
      if (!inSubMode) {
        if (menuIndex == 2) { // 继电器开关
          relayState = !relayState;
          digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);
          drawMainMenu();
        } 
        else if (menuIndex == 6) { // WiFi 开关
          apState = !apState;
          if(apState) {
            WiFi.softAP("ESP32_Smart_Home", "12345678");
            server.begin();
          } else {
            WiFi.softAPdisconnect(true);
            WiFi.mode(WIFI_OFF); // ⭐ 关闭射频降功耗
          }
          drawMainMenu();
        } 
        else { inSubMode = true; u8g2.clearBuffer(); }
      }
    } 
    else if (clickCount == 2) {
      if (inSubMode) { inSubMode = false; manageHardware(); drawMainMenu(); }
    }
    clickCount = 0; 
  }

  // 子模式运行
  if (inSubMode) {
    if (menuIndex == 0) runRadarMode();
    else if (menuIndex == 1) runTempHumidMode();
    else if (menuIndex == 3) runHeartbeatMode();
    else if (menuIndex == 4) runSoundMode();  // 新增声音
    else if (menuIndex == 5) runIRMode();
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
    } else if (itemIndex == 6) {
      u8g2.print("WiFi ["); u8g2.print(apState ? "ON" : "OFF"); u8g2.print("]");
    } else {
      u8g2.print(menuItems[itemIndex]);
    }
  }
  u8g2.sendBuffer();
}

// ================= 子功能 =================

void runRadarMode() {
  if (millis() - lastRadarTime > 100) {
    lastRadarTime = millis();
    digitalWrite(TRIG_PIN, LOW); delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);
    long duration = pulseIn(ECHO_PIN, HIGH, 30000);
    if (duration == 0) currentDistance = 999; else currentDistance = duration * 0.034 / 2;

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

// ⭐ 新增：专业级声音分贝与参数界面
void runSoundMode() {
  if (millis() - lastSoundTime > 50) {
    lastSoundTime = millis();
    
    int max_val = 0, min_val = 4095;
    // 采集 50 次样本找峰值
    for(int i=0; i<50; i++) {
      int v = analogRead(SOUND_PIN);
      if(v > max_val) max_val = v;
      if(v < min_val) min_val = v;
    }
    
    soundMax = max_val;
    soundMin = min_val;
    
    // 计算峰峰值 (Vpp)
    int vpp = soundMax - soundMin;
    
    // 相对分贝转换公式 (由于没有参考声压，这里计算的是相对变化值)
    // 公式: dB = 20 * log10(Vpp)
    if (vpp > 0) {
      currentDB = 20.0 * log10(vpp) + 30.0; // +30 是经验补偿基数，让数值看起来像环境分贝
    } else {
      currentDB = 30.0;
    }
    if (currentDB > 120) currentDB = 120; // 限制最大值
    if (currentDB < 30) currentDB = 30;

    u8g2.clearBuffer();
    u8g2.drawStr(0, 10, "--- Sound Meter ---");
    
    // 大字显示分贝
    u8g2.setFont(u8g2_font_ncenB24_tr);
    u8g2.setCursor(0, 35);
    u8g2.print(currentDB, 1); 
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.print(" dB");

    // 专业参数：ADC 最大值、最小值、峰峰值
    u8g2.setCursor(0, 45);
    u8g2.print("Peak: "); u8g2.print(soundMax); 
    u8g2.print(" | Min: "); u8g2.print(soundMin);
    u8g2.setCursor(0, 55);
    u8g2.print("Vpp : "); u8g2.print(vpp);
    
    u8g2.drawStr(0, 64, "Dbl click to exit");
    u8g2.sendBuffer();
  }
}

void runHeartbeatMode() {
  if (millis() - lastHeartTime > 20) {
    lastHeartTime = millis();
    int signal = analogRead(HEART_PIN);
    
    static bool isBeating = false;
    static unsigned long lastBeat = 0;

    if (signal > 2000 && !isBeating) {
      isBeating = true;
      unsigned long now = millis();
      if (now - lastBeat > 300 && now - lastBeat < 1500) { bpm = 60000 / (now - lastBeat); }
      lastBeat = now;
    }
    if (signal < 1800 && isBeating) isBeating = false;

    if(millis() % 100 == 0) {
      u8g2.clearBuffer(); u8g2.drawStr(0, 10, "--- Heartbeat ---");
      u8g2.setFont(u8g2_font_ncenB24_tr); u8g2.setCursor(10, 40);
      u8g2.print(bpm); u8g2.print(" BPM");
      u8g2.setFont(u8g2_font_6x10_tf); u8g2.drawStr(0, 60, "Dbl click to exit");
      u8g2.sendBuffer();
    }
  }
}

void runIRMode() {
  if (digitalRead(IR_PIN) == LOW) {
    u8g2.clearBuffer(); u8g2.drawStr(0, 10, "--- IR Receiver ---");
    u8g2.setCursor(0, 30); u8g2.print("Signal Detected!");
    u8g2.setCursor(0, 50); u8g2.print("Check Serial");
    u8g2.drawStr(0, 60, "Dbl click to exit");
    u8g2.sendBuffer();
    Serial.println("红外信号触发！");
    delay(200); 
  } else {
    u8g2.clearBuffer(); u8g2.drawStr(0, 10, "--- IR Receiver ---");
    u8g2.setCursor(0, 30); u8g2.print("Waiting...");
    u8g2.drawStr(0, 60, "Dbl click to exit");
    u8g2.sendBuffer();
    delay(100);
  }
}

void runSystemInfoMode() {
  u8g2.clearBuffer();
  u8g2.drawStr(0, 10, "--- System Info ---");
  u8g2.setCursor(0, 25); u8g2.print("AP Clients: "); u8g2.print(apState ? WiFi.softAPgetStationNum() : 0);
  u8g2.setCursor(0, 35); u8g2.print("Free RAM : "); u8g2.print(ESP.getFreeHeap() / 1024); u8g2.print(" KB");
  u8g2.setCursor(0, 45); u8g2.print("Chip Temp: "); u8g2.print((int)temperatureRead()); u8g2.print(" C");
  u8g2.setCursor(0, 55); u8g2.print("IP Addr  : "); u8g2.print(apState ? WiFi.softAPIP().toString() : "None");
  u8g2.drawStr(0, 64, "Dbl click to exit");
  u8g2.sendBuffer();
}
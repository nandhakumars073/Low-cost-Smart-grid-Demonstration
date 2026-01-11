/********************************************************************
 *  SMART GRID MONITORING SYSTEM — MASTER (ESP8266)
 *  ------------------------------------------------
 *  - Communicates with STM32 via UART
 *  - Communicates with ESP32/ESP8266 slave via ESP-NOW
 *  - Uploads data to ThingSpeak
 *  - Sends WhatsApp/SMS alerts (CallMeBot API)
 *  ------------------------------------------------
 *  Author: Angel Lalu, Shreyas S and ChatGPT
 *  Last Updated: Nov 2025
 ********************************************************************/

#include <ESP8266WiFi.h>
#include <espnow.h>
#include <SoftwareSerial.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

// ----------------------
// STM32 UART Communication
// ----------------------
SoftwareSerial STM32_UART(4, 5);  // RX=D2, TX=D1

// ----------------------
// ESP-NOW (Slave Link)
// ----------------------
uint8_t slaveMAC[] = {0x2C, 0xBC, 0xBB, 0x0A, 0xD1, 0xA0};

typedef struct struct_message {
  float I1;
  float I2;
  float I3;
  char status[10];
} struct_message;

struct_message incomingData;

// ----------------------
// LED Indicator
// ----------------------
#define LED_PIN LED_BUILTIN
bool slaveConnected = false;
bool stmConnected   = false;
unsigned long lastBlink = 0;
bool ledState = false;

// ----------------------
// Line State Tracking (1=ON, 0=OFF)
// ----------------------
bool lineState[3] = { true, true, true };

// ----------------------
// Wi-Fi / Cloud Config
// ----------------------
const char* ssid = "AITTEST"; //Put your shit here man Angel!!!
const char* pass = "12345678"; //Put your shit here man Angel!!!

// ThingSpeak
const char* tsServer   = "api.thingspeak.com";
String tsWriteKey = "7FSZA2BTM8CBSLHQ";

// =====================================================
// SMS / WhatsApp ALERT SETTINGS (CallMeBot API)
// =====================================================
unsigned long lastSMS = 0;
const unsigned long SMS_INTERVAL_MS = 20000;

String phone = "918310958557";
String apikey = "123456";

// ----------------------
// Send WhatsApp/SMS Alert
// ----------------------
void sendSMSAlert(String msg) {
  if (millis() - lastSMS < SMS_INTERVAL_MS) return;
  lastSMS = millis();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️ Wi-Fi not connected — cannot send SMS.");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  msg.replace(" ", "%20");

  String url =
    "https://api.callmebot.com/whatsapp.php?phone=" + phone +
    "&text=" + msg + "&apikey=" + apikey;

  http.setTimeout(4000);
  if (!http.begin(client, url)) {
    Serial.println("❌ HTTP begin failed (SMS)");
    return;
  }
  int httpCode = http.GET();

  if (httpCode == 200) {
    Serial.println("✅ SMS alert sent successfully!");
    digitalWrite(LED_PIN, LOW); delay(100);
    digitalWrite(LED_PIN, HIGH);
  } else {
    Serial.printf("❌ SMS failed! HTTP code: %d\n", httpCode);
  }

  http.end();
}

// ----------------------
// Function Declarations
// ----------------------
void onDataRecv(uint8_t *mac, uint8_t *incoming, uint8_t len);
void onDataSent(uint8_t *mac_addr, uint8_t sendStatus);
void sendCommandToSlave(String cmd);
void sendToSTM32(struct_message data);
void updateLEDStatus();
void setLEDConnected(bool connected);
void flashLEDQuickly();
void handleHandshake(String msg);
void blinkFast();
void blinkSlow();
void sendToThingSpeak();
void updateLocalStateFromStatus(const char* status);
String statusToCSV();

// =====================================================
// SETUP
// =====================================================
void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  Serial.begin(115200);
  STM32_UART.begin(9600);
  Serial.println("\n🔌 ESP8266 MASTER Booting...");

  // Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  Serial.print("Connecting to "); Serial.print(ssid);
  while (WiFi.status() != WL_CONNECTED) { delay(200); Serial.print("."); }
  Serial.println("\n✅ Wi-Fi connected!");
  Serial.print("IP: "); Serial.println(WiFi.localIP());

  // ESP-NOW
  if (esp_now_init() != 0) {
    Serial.println("❌ ESP-NOW init failed!");
    return;
  }
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(onDataRecv);
  esp_now_register_send_cb(onDataSent);
  esp_now_add_peer(slaveMAC, ESP_NOW_ROLE_COMBO, WiFi.channel(), NULL, 0);

  Serial.println("🚀 Ready — waiting for STM32 handshake...");
}

// =====================================================
// LOOP
// =====================================================
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.begin(ssid, pass);
    Serial.println("↻ Wi-Fi reconnecting...");
  }

  updateLEDStatus();

  if (STM32_UART.available()) {
    String msg = STM32_UART.readStringUntil('\n');
    msg.trim();
    if (msg.length() > 0) handleHandshake(msg);
  }

  // Manual ThingSpeak interval
  static unsigned long lastTS = 0;
  if (millis() - lastTS > 15000) {
    lastTS = millis();
    sendToThingSpeak();
  }
}

// =====================================================
// STM32 Handshake & Commands
// =====================================================
void handleHandshake(String msg) {
  Serial.print("STM32 → "); Serial.println(msg);

  if (msg.equalsIgnoreCase("HELLO_ESP")) {
    flashLEDQuickly();
    STM32_UART.println("HELLO_STM");
    stmConnected = true;
    Serial.println("🤝 Handshake complete with STM32.");
  } else {
    sendCommandToSlave(msg);
  }

  if (msg.startsWith("FAULT_")) {
    int line = msg.substring(6, 7).toInt();
    sendSMSAlert("[Grid] ⚡ Fault: Line " + String(line) + " tripped!");
  }
}

// =====================================================
// ESP-NOW Receive
// =====================================================
void onDataRecv(uint8_t *mac, uint8_t *incoming, uint8_t len) {
  memcpy(&incomingData, incoming, sizeof(incomingData));
  sendToSTM32(incomingData);
  setLEDConnected(true);

  updateLocalStateFromStatus(incomingData.status);

  if (String(incomingData.status).indexOf("FAULT") >= 0)
    sendSMSAlert("[Grid] ⚡ Slave Fault: " + String(incomingData.status));
}

void onDataSent(uint8_t *mac_addr, uint8_t sendStatus) {
  setLEDConnected(sendStatus == 0);
}

// =====================================================
// Forward Data to STM32
// =====================================================
void sendToSTM32(struct_message data) {
  char buf[80];
  snprintf(buf, sizeof(buf), "%.2f,%.2f,%.2f,%s\n",
           data.I1, data.I2, data.I3, data.status);
  STM32_UART.print(buf);
  Serial.print("Forwarded → STM32: "); Serial.println(buf);
}

// =====================================================
// Send Command to Slave
// =====================================================
void sendCommandToSlave(String cmd) {
  if (cmd == "OFF_LINE1") lineState[0] = false;
  else if (cmd == "ON_LINE1") lineState[0] = true;
  else if (cmd == "OFF_LINE2") lineState[1] = false;
  else if (cmd == "ON_LINE2") lineState[1] = true;
  else if (cmd == "OFF_LINE3") lineState[2] = false;
  else if (cmd == "ON_LINE3") lineState[2] = true;
  else if (cmd == "OFF_ALL")  lineState[0]=lineState[1]=lineState[2]=false;
  else if (cmd == "ON_ALL")   lineState[0]=lineState[1]=lineState[2]=true;

  uint8_t buf[50];
  cmd.toCharArray((char*)buf, sizeof(buf));
  esp_now_send(slaveMAC, buf, cmd.length());
  Serial.print("📤 Sent → Slave: "); Serial.println(cmd);
}

// =====================================================
// LED Logic
// =====================================================
void updateLEDStatus() {
  if (!stmConnected) blinkSlow();
  else if (!slaveConnected) blinkFast();
  else digitalWrite(LED_PIN, LOW);
}

void setLEDConnected(bool c) { slaveConnected = c; }

void flashLEDQuickly() {
  for (int i=0;i<5;i++){
    digitalWrite(LED_PIN,LOW); delay(100);
    digitalWrite(LED_PIN,HIGH); delay(100);
  }
}

void blinkFast() {
  if (millis() - lastBlink > 200) {
    lastBlink = millis(); ledState=!ledState;
    digitalWrite(LED_PIN, ledState?LOW:HIGH);
  }
}

void blinkSlow() {
  if (millis() - lastBlink > 700) {
    lastBlink = millis(); ledState=!ledState;
    digitalWrite(LED_PIN, ledState?LOW:HIGH);
  }
}

// =====================================================
// ThingSpeak Upload (every 15s)
// =====================================================
void sendToThingSpeak() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClient client;
  HTTPClient http;

  String url = String("http://") + tsServer +
               "/update?api_key=" + tsWriteKey +
               "&field1=" + String(incomingData.I1, 2) +
               "&field2=" + String(incomingData.I2, 2) +
               "&field3=" + String(incomingData.I3, 2) +
               "&field4=" + statusToCSV();

  http.setTimeout(4000);
  if (!http.begin(client, url)) {
    Serial.println("❌ ThingSpeak http.begin failed");
    return;
  }
  int code = http.GET();

  Serial.print("📡 ThingSpeak update → HTTP ");
  Serial.println(code);

  http.end();
}

// =====================================================
// Helpers
// =====================================================
String statusToCSV() {
  return String(lineState[0] ? "1" : "0") + "," +
         String(lineState[1] ? "1" : "0") + "," +
         String(lineState[2] ? "1" : "0");
}

void updateLocalStateFromStatus(const char* status) {
  int f = 0;
  for (int i = 0; status[i] != '\0' && f < 3; i++) {
    if (status[i] == '0' || status[i] == '1') {
      lineState[f] = (status[i] == '1');
      f++;
    }
  }
}

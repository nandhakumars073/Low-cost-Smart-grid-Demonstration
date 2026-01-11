#include <WiFi.h>
#include <esp_now.h>

// ========================== PIN DEFINITIONS ==========================
#define SENSOR1 34
#define SENSOR2 35
#define SENSOR3 32

#define RELAY1 25
#define RELAY2 26
#define RELAY3 27

#define FAULT_SW1 23   // LOW = simulate LG (Line 2 → Ground)
#define FAULT_SW2 22   // LOW = simulate LL or LLG (Lines 2–3)

// ========================== STRUCT DEFINITIONS ======================
typedef struct struct_message {
  float I1;
  float I2;
  float I3;
  char status[10];
} struct_message;

struct_message outgoingData;

// ========================== THRESHOLDS ==============================
float limit1 = 1862.0;
float limit2 = 1860.0;
float limit3 = 1865.0;

// ========================== MASTER MAC =============================
uint8_t masterMAC[] = {0x84, 0x0D, 0x8E, 0xB0, 0x0F, 0xE7}; // change to yours

// ========================== RELAY STATES ============================
bool relay1State = true;
bool relay2State = true;
bool relay3State = true;

// ========================== APPLY STATES ============================
void applyRelayStates() {
  digitalWrite(RELAY1, relay1State ? HIGH : LOW);
  digitalWrite(RELAY2, relay2State ? HIGH : LOW);
  digitalWrite(RELAY3, relay3State ? HIGH : LOW);
}

// ========================== CALLBACKS ===============================
void onDataSent(const esp_now_send_info_t *info, esp_now_send_status_t status) {
  Serial.print("Delivery Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
}

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
  char command[50];
  if (len >= (int)sizeof(command)) len = sizeof(command)-1;
  memcpy(command, incomingData, len);
  command[len] = '\0';
  Serial.print("📩 Received command: ");
  Serial.println(command);

  if (strcmp(command, "RESET") == 0) {
    relay1State = relay2State = relay3State = true;
    Serial.println("✅ All relays RESET (ON)");
  } else if (strcmp(command, "LOAD_1_ON") == 0) relay1State = true;
  else if (strcmp(command, "LOAD_2_ON") == 0) relay2State = true;
  else if (strcmp(command, "LOAD_3_ON") == 0) relay3State = true;
  else if (strcmp(command, "LOAD_1_OFF") == 0 || strcmp(command, "FAULT_1_OFF") == 0) relay1State = false;
  else if (strcmp(command, "LOAD_2_OFF") == 0 || strcmp(command, "FAULT_2_OFF") == 0) relay2State = false;
  else if (strcmp(command, "LOAD_3_OFF") == 0 || strcmp(command, "FAULT_3_OFF") == 0) relay3State = false;

  applyRelayStates();
  Serial.printf("🔘 Relays: 1=%s 2=%s 3=%s\n",
                relay1State ? "ON" : "OFF",
                relay2State ? "ON" : "OFF",
                relay3State ? "ON" : "OFF");
}

// ========================== SETUP ===============================
void setup() {
  Serial.begin(115200);
  Serial.println("ESP32 Slave Starting...");

  pinMode(RELAY1, OUTPUT);
  pinMode(RELAY2, OUTPUT);
  pinMode(RELAY3, OUTPUT);
  pinMode(FAULT_SW1, INPUT_PULLUP);
  pinMode(FAULT_SW2, INPUT_PULLUP);

  relay1State = relay2State = relay3State = true;
  applyRelayStates();

  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ ESP-NOW init failed!");
    while (true) delay(1000);
  }

  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, masterMAC, 6);
  peerInfo.channel = 1;
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);

  Serial.println("✅ ESP-NOW Ready!");
}

// ========================== LOOP ===============================
void loop() {
  float current1 = analogRead(SENSOR1);
  float current2 = analogRead(SENSOR2);
  float current3 = analogRead(SENSOR3);

  bool swFault1 = (digitalRead(FAULT_SW1) == LOW);  // LG
  bool swFault2 = (digitalRead(FAULT_SW2) == LOW);  // LL or LLG

  bool faultDetected = false;

  // --- Normal overcurrent protection ---
  if (current1 > limit1) {
    relay1State = false;
    faultDetected = true;
    Serial.println("⚠ Bus1 Overcurrent -> Relay1 OFF");
  }
  if (current2 > limit2) {
    relay2State = false;
    faultDetected = true;
    Serial.println("⚠ Bus2 Overcurrent -> Relay2 OFF");
  }
  if (current3 > limit3) {
    relay3State = false;
    faultDetected = true;
    Serial.println("⚠ Bus3 Overcurrent -> Relay3 OFF");
  }

  // --- Simulated Faults via Switches ---
  const char *statusLabel = "NORMAL";

  if (swFault1 && !swFault2) {
    // Single-Line-to-Ground (LG)
    relay2State = false;
    faultDetected = true;
    current2 = 9999.0;
    statusLabel = "FAULT_LG";
    Serial.println("⚠ Simulated FAULT_LG (Line-2→Ground)");
  }
  else if (!swFault1 && swFault2) {
    // Line-to-Line (LL)
    relay2State = relay3State = false;
    faultDetected = true;
    current2 = 9999.0;
    current3 = 9999.0;
    statusLabel = "FAULT_LL";
    Serial.println("⚠ Simulated FAULT_LL (Line2–Line3 short)");
  }
  else if (swFault1 && swFault2) {
    // Double-Line-to-Ground (LLG)
    relay2State = relay3State = false;
    faultDetected = true;
    current2 = 9999.0;
    current3 = 9999.0;
    statusLabel = "FAULT_LLG";
    Serial.println("⚠ Simulated FAULT_LLG (Line2 & Line3→Ground)");
  }

  // Apply states to hardware
  applyRelayStates();

  // Prepare message
  outgoingData.I1 = current1;
  outgoingData.I2 = current2;
  outgoingData.I3 = current3;
  strcpy(outgoingData.status, statusLabel);
  outgoingData.status[sizeof(outgoingData.status) - 1] = '\0';  // Safety

  // Send to master
  esp_err_t r = esp_now_send(masterMAC, (uint8_t*)&outgoingData, sizeof(outgoingData));
  if (r == ESP_OK) {
    Serial.printf("📤 I1=%.2f I2=%.2f I3=%.2f Status=%s\n",
                  outgoingData.I1, outgoingData.I2, outgoingData.I3, outgoingData.status);
  }

  delay(2000);
}

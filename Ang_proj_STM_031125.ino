// STM32 master: handshake with ESP-12E, receive CSV current data, 
// automatically isolate faults + allow manual override via Serial Monitor.

HardwareSerial Serial3(PC11, PC10);  // UART to ESP-12E
#define LED_PIN PB13

// UART settings
const unsigned long USB_BAUD  = 115200;
const unsigned long UART_BAUD = 9600;

// Handshake parameters
const int HANDSHAKE_ATTEMPTS = 5;
const unsigned long HANDSHAKE_TIMEOUT_MS = 2000;

// Fault detection threshold
const float FAULT_THRESHOLD = 1900.0;  // mA (example)
bool faultState[3] = {false, false, false};  // tracks faults

// --------------- INIT ----------------
void initializeHardware() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
}

// --------------- PARSE CSV ----------------
bool parseCSV(const String &line, float &i1, float &i2, float &i3, String &status) {
  int idx1 = line.indexOf(',');
  if (idx1 < 0) return false;
  int idx2 = line.indexOf(',', idx1 + 1);
  if (idx2 < 0) return false;
  int idx3 = line.indexOf(',', idx2 + 1);
  String s1 = line.substring(0, idx1);
  String s2 = line.substring(idx1 + 1, idx2);
  String s3;
  if (idx3 >= 0) {
    s3 = line.substring(idx2 + 1, idx3);
    status = line.substring(idx3 + 1);
  } else {
    s3 = line.substring(idx2 + 1);
    status = "";
  }
  s1.trim(); s2.trim(); s3.trim(); status.trim();
  i1 = s1.toFloat();
  i2 = s2.toFloat();
  i3 = s3.toFloat();
  return true;
}

// --------------- HANDSHAKE ----------------
bool handshakeWithESP() {
  for (int attempt = 0; attempt < HANDSHAKE_ATTEMPTS; ++attempt) {
    Serial.print("STM32: Sending HELLO_ESP (attempt ");
    Serial.print(attempt + 1);
    Serial.println(")");
    Serial3.println("HELLO_ESP");
    unsigned long t0 = millis();
    while (millis() - t0 < HANDSHAKE_TIMEOUT_MS) {
      if (Serial3.available()) {
        String r = Serial3.readStringUntil('\n');
        r.trim();
        if (r == "HELLO_STM") {
          Serial.println("Handshake OK ✅");
          digitalWrite(LED_PIN, HIGH);
          while (Serial3.available()) Serial3.read();
          return true;
        }
      }
    }
    digitalWrite(LED_PIN, LOW);
    delay(300);
  }
  Serial.println("Handshake FAILED ❌");
  return false;
}

// --------------- SEND COMMAND ----------------
void sendCommandToESP(const String &cmd) {
  Serial3.println(cmd);
  Serial.print("→ Sent to ESP: ");
  Serial.println(cmd);
}

// --------------- HANDLE DATA ----------------
void handleIncomingLine(const String &line) {
  float I1 = 0, I2 = 0, I3 = 0;
  String status;
  if (!parseCSV(line, I1, I2, I3, status)) {
    Serial.println("⚠️ Invalid CSV format");
    return;
  }

  Serial.print("I1="); Serial.print(I1, 2);
  Serial.print(" I2="); Serial.print(I2, 2);
  Serial.print(" I3="); Serial.print(I3, 2);
  Serial.print(" status="); Serial.println(status);

  float currents[3] = {I1, I2, I3};

  for (int i = 0; i < 3; i++) {
    if (currents[i] > FAULT_THRESHOLD && !faultState[i]) {
      faultState[i] = true;
      sendCommandToESP("FAULT_" + String(i + 1) + "_OFF");
      Serial.printf("⚠️ Line %d fault! Relay OFF\n", i + 1);
    } 
    else if (currents[i] <= FAULT_THRESHOLD && faultState[i]) {
      faultState[i] = false;
      sendCommandToESP("LOAD_" + String(i + 1) + "_ON");
      Serial.printf("✅ Line %d recovered! Relay ON\n", i + 1);
    }
  }

  bool anyFault = faultState[0] || faultState[1] || faultState[2];
  digitalWrite(LED_PIN, anyFault ? (millis() / 200) % 2 : HIGH);
}

// --------------- MANUAL CONTROL ----------------
void handleUserInput() {
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    cmd.toUpperCase();

    if (cmd == "1") sendCommandToESP("FAULT_1_OFF");
    else if (cmd == "2") sendCommandToESP("FAULT_2_OFF");
    else if (cmd == "3") sendCommandToESP("FAULT_3_OFF");
    else if (cmd == "1R") sendCommandToESP("LOAD_1_ON");
    else if (cmd == "2R") sendCommandToESP("LOAD_2_ON");
    else if (cmd == "3R") sendCommandToESP("LOAD_3_ON");
    else if (cmd == "4") {
      for (int i = 1; i <= 3; i++) sendCommandToESP("FAULT_" + String(i) + "_OFF");
    } 
    else if (cmd == "4R") {
      for (int i = 1; i <= 3; i++) sendCommandToESP("LOAD_" + String(i) + "_ON");
    } 
    else {
      Serial.println("Unknown command. Use 1,2,3,4 or add 'R' to restore (e.g. 2R).");
      return;
    }
    Serial.println("✅ Manual command sent.");
  }
}

// --------------- SETUP ----------------
void setup() {
  initializeHardware();
  Serial.begin(USB_BAUD);
  while (!Serial) {}

  Serial3.begin(UART_BAUD);
  delay(300);
  Serial.println("STM32: Booting and attempting handshake...");
  if (!handshakeWithESP()) {
    while (1) {
      digitalWrite(LED_PIN, millis() % 500 < 250 ? HIGH : LOW);
      delay(100);
    }
  }
  Serial.println("STM32: Handshake successful — Listening for data...");
}

// --------------- LOOP ----------------
void loop() {
  // Handle automatic data from ESP
  if (Serial3.available()) {
    String line = Serial3.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) handleIncomingLine(line);
  }

  // Manual input from user
  handleUserInput();
  delay(10);
}

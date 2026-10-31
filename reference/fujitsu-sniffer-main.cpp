#include "FujiHeatPump.h"

// ========== SNIFFER MODE ==========
// Listen-only mode for capturing and analyzing LIN bus traffic
// Does not respond to any frames, purely for protocol reverse-engineering

HardwareSerial *linSerial;

struct SniffStats {
  uint32_t totalFrames = 0;
  uint32_t validFrames = 0;
  uint32_t startTime = 0;
  byte lastSource = 0;
  byte lastDest = 0;
};

SniffStats stats;

// Decode frame for display without modifying it
void printSniffedFrame(const byte buf[8], uint32_t timestamp) {
  Serial.printf("[%08lu ms] RAW: ", timestamp);
  for(int i = 0; i < 8; i++) {
    Serial.printf("%02X ", buf[i]);
  }
  Serial.print("  DECODED: ");

  // XOR decode for analysis
  byte decoded[8];
  for(int i = 0; i < 8; i++) {
    decoded[i] = buf[i] ^ 0xFF;
  }

  for(int i = 0; i < 8; i++) {
    Serial.printf("%02X ", decoded[i]);
  }

  Serial.print("  | Src:");
  switch(decoded[0]) {
    case 0x80: Serial.print("MASTER"); break;
    case 0x40: Serial.print("SLAVE "); break;
    case 0x08: Serial.print("BCAST "); break;
    default: Serial.printf("%02X  ", decoded[0]);
  }

  Serial.printf(" Dst:%02X Type:%d ", decoded[1] & 0x7F, decoded[2] >> 4);

  // Decode byte 3 (temp and fan)
  byte b3 = decoded[3];
  byte tempNib = (b3 >> 4) & 0x0F;
  byte fanNib = b3 & 0x0F;

  int temp = 15 + (tempNib / 2);
  Serial.printf("T:%d°C Fan:%X ", temp, fanNib);

  // Decode byte 4 (power)
  byte b4 = decoded[4];
  Serial.printf("Pwr:%s ", (b4 == 0x80) ? "OFF" : "ON");

  Serial.println();

  stats.validFrames++;
}

void snifferSetup() {
  // Initialize USB serial for output
  Serial.begin(115200);
  while(!Serial) {
    delay(100);
  }

  Serial.println("\n====================================");
  Serial.println("  FUJITSU HEATPUMP LIN BUS SNIFFER");
  Serial.println("====================================");
  Serial.println("MCP2021 LIN Transceiver Configuration\n");

  // Initialize LIN bus serial (500 baud, 8E1)
  linSerial = &Serial2;
  linSerial->begin(500, SERIAL_8E1, 16, 17); // RX=GPIO16, TX=GPIO17
  linSerial->setTimeout(50);

  stats.startTime = millis();

  Serial.printf("Serial2 Config: 500 baud, 8E1\n");
  Serial.printf("MCP2021 PIN1 (RXD) → GPIO16\n");
  Serial.printf("MCP2021 PIN4 (TXD) → GPIO17\n");
  Serial.printf("USB Serial: %lu baud\n", 115200);
  Serial.println("Status: Listening for LIN frames...");
  Serial.println("====================================\n");
}

void snifferLoop() {
  static uint32_t lastDiagTime = 0;
  static uint32_t lastSampleTime = 0;

  if(linSerial->available() > 0) {
    byte b = linSerial->read();
    uint32_t timestamp = millis() - stats.startTime;

    stats.totalFrames++;

    // Log every byte received
    Serial.printf("[%08lu ms] BYTE RX: 0x%02X  | Total bytes: %lu\n",
                  timestamp, b, stats.totalFrames);

  } else {
    // Sample GPIO pins every 100ms and print pattern
    if(millis() - lastSampleTime > 100) {
      lastSampleTime = millis();
      int rxState = digitalRead(16);
      int txState = digitalRead(17);

      uint32_t elapsed = (millis() - stats.startTime) / 1000;
      Serial.printf("[%lu sec] GPIO: RX(16)=%d TX(17)=%d | Serial2: available=%d\n",
                    elapsed, rxState, txState, linSerial->available());
    }

    // Print diagnostic every 10 seconds
    if(millis() - lastDiagTime > 10000) {
      lastDiagTime = millis();
      uint32_t elapsed = (millis() - stats.startTime) / 1000;

      Serial.printf("\n[%lu sec] STATUS: Total bytes: %lu\n\n", elapsed, stats.totalFrames);
    }
  }
}

// ========== MAIN SETUP & LOOP ==========

void setup() {
  snifferSetup();
}

void loop() {
  snifferLoop();
  delay(1); // Small delay to prevent watchdog timeout
}

#include <Arduino.h>
#include <Wire.h>

#define AR1010_I2C_ADDR 0x10

// 1 = crystal    (R0 = 0xFFFB, R11 = 0x4E55)  PDF section 3.6
// 0 = ext. clock (R0 = 0xFF7B, R11 = 0x4F55)  PDF section 3.7
#define AR1010_USE_CRYSTAL 1

#define R1_HMUTE      (1 << 1)   // R1 D1
#define R2_TUNE       (1 << 9)   // R2 D9
#define R2_CHAN_MASK  0x01FF     // R2 D8-D0
#define R3_SEEK       (1 << 14)  // R3 D14

bool initAR1010();
void writeRegister(uint8_t reg, uint16_t value);
uint16_t readRegister(uint8_t reg);
void setFrequency(float MHz);
void dumpRegisters();
void printStatus();
void handleCLI();

void setup() {
  Serial.begin(115200);
  delay(1000);

  Wire.begin(21, 22); // ESP32 default I2C pins
  Wire.setClock(100000);

  Serial.println("\n=== AR1010 FM Radio CLI Diagnostic ===");
  Serial.println("Commands:");
  Serial.println("  dump / d        - Full register dump with descriptive labels");
  Serial.println("  status / s      - Show parsed status (STC, Seek Fail, RSSI)");
  Serial.println("  tune <MHz>      - Tune to frequency in MHz (e.g., tune 99.6)");
  Serial.println("  r <reg>         - Read single register (e.g., r 0x00)");
  Serial.println("  w <reg> <val>   - Write register hex (e.g., w 0x03 0xA010)");
  Serial.println("======================================");

  delay(100); // Allow clock to stabilize

  if (initAR1010()) {
    Serial.println("AR1010 Initialized & Ready!");
    setFrequency(96.9);
  } else {
    Serial.println("Initialization / Calibration Failed or Timed Out!");
  }
  Serial.print("\nAR1010> ");
}

void loop() {
  handleCLI();
}

bool initAR1010() {
  Serial.println("Configuring R1-R17 (PDF Page 16: R1~R17 must be configured before R0)...");

#if AR1010_USE_CRYSTAL
  const uint16_t R0_VAL  = 0xFFFB; // xo_en=1 (internal oscillator drives crystal), ENABLE=1
  const uint16_t R11_VAL = 0x4E55;
#else
  const uint16_t R0_VAL  = 0xFF7B; // xo_en=0 (external clock), ENABLE=1
  const uint16_t R11_VAL = 0x4F55;
#endif

  writeRegister(0x01, 0x5B15);
  writeRegister(0x02, 0xD0B9);
  writeRegister(0x03, 0xA010);
  writeRegister(0x04, 0x0780);
  writeRegister(0x05, 0x28AB);
  writeRegister(0x06, 0x6400);
  writeRegister(0x07, 0x1EE7);
  writeRegister(0x08, 0x7141);
  writeRegister(0x09, 0x007D);
  writeRegister(0x0A, 0x82C6);
  writeRegister(0x0B, R11_VAL);
  writeRegister(0x0C, 0x970C);
  writeRegister(0x0D, 0xB845);
  writeRegister(0x0E, 0xFC2D);
  writeRegister(0x0F, 0x8097);
  writeRegister(0x10, 0x04A1);
  writeRegister(0x11, 0xDF6A);

  writeRegister(0x00, R0_VAL);

  unsigned long startTime = millis();
  while (millis() - startTime < 1500) {
    uint16_t status = readRegister(0x13);
    if (status != 0xFFFF && ((status >> 5) & 0x01)) {
      Serial.println("Calibration complete (STC flag set).");
      return true;
    }
    delay(10);
  }

  Serial.println("Calibration timeout! Check wiring and ensure the board is receiving clean 3.3V.");
  return false;
}

void writeRegister(uint8_t reg, uint16_t value) {
  Wire.beginTransmission(AR1010_I2C_ADDR);
  Wire.write(reg);
  Wire.write((value >> 8) & 0xFF);
  Wire.write(value & 0xFF);
  Wire.endTransmission();
}

uint16_t readRegister(uint8_t reg) {
  Wire.beginTransmission(AR1010_I2C_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return 0xFFFF;
  }

  if (Wire.requestFrom((uint8_t)AR1010_I2C_ADDR, (uint8_t)2) == 2) {
    uint16_t highByte = Wire.read();
    uint16_t lowByte = Wire.read();
    return (highByte << 8) | lowByte;
  }
  return 0xFFFF;
}

void setFrequency(float MHz) {
  if (MHz < 76.0 || MHz > 108.0) {
    Serial.println("Frequency out of supported range (76.0 - 108.0 MHz)");
    return;
  }

  // PDF Page 11: Frequency(MHz) = 69.0 + 0.1 * CHAN
  int channel = (int)((MHz - 69.0) * 10.0 + 0.5);

  uint16_t r1 = readRegister(0x01);
  uint16_t r2 = readRegister(0x02);
  uint16_t r3 = readRegister(0x03);
  if (r1 == 0xFFFF || r2 == 0xFFFF || r3 == 0xFFFF) {
    Serial.println("I2C read error - check wiring.");
    return;
  }

  // --- PDF Page 26 "Tune" sequence ---

  // 1. Set hmute (R1 D1)
  r1 |= R1_HMUTE;
  writeRegister(0x01, r1);

  // 2. Clear TUNE (R2 D9) - also clears STC
  r2 &= ~R2_TUNE;
  writeRegister(0x02, r2);

  // 3. Clear SEEK (R3 D14)
  r3 &= ~R3_SEEK;
  writeRegister(0x03, r3);

  // 4. Set BAND/SPACE/CHAN
  // BAND = R3 D12-D11: 00 = US/EU (87.5-108), 11 = Japan wide (76-108)
  // SPACE = R3 D13 = 1 (100kHz). Preserve VOLUME (D10-D7) and SEEKTH (D6-D0).
  uint8_t band = (MHz < 87.5) ? 0x03 : 0x00;
  r3 = (1 << 15) | (0 << 14) | (1 << 13) | (band << 11) | (r3 & 0x07FF);
  writeRegister(0x03, r3);

  // CHAN = R2 D8-D0
  r2 = (r2 & ~R2_CHAN_MASK) | (channel & R2_CHAN_MASK);
  writeRegister(0x02, r2);

  // 5. Set TUNE (R2 D9) - creates the required 0->1 edge
  r2 |= R2_TUNE;
  writeRegister(0x02, r2);

  // 6. Wait for STC (STATUS D5)
  unsigned long startTime = millis();
  bool tuned = false;
  while (millis() - startTime < 1000) {
    uint16_t status = readRegister(0x13);
    if (status != 0xFFFF && ((status >> 5) & 0x01)) {
      tuned = true;
      break;
    }
    delay(10);
  }

  // 7. Clear hmute
  r1 &= ~R1_HMUTE;
  writeRegister(0x01, r1);

  // Verify the chip actually landed on the requested channel
  uint16_t st = readRegister(0x13);
  int readChan = (st != 0xFFFF) ? ((st >> 7) & 0x01FF) : -1;

  if (tuned && readChan == channel) {
    Serial.print("Successfully tuned to: ");
    Serial.print(MHz);
    Serial.print(" MHz (Channel: ");
    Serial.print(channel);
    Serial.println(")");
  } else if (tuned) {
    Serial.print("STC set but READCHAN = ");
    Serial.print(readChan);
    Serial.print(" (expected ");
    Serial.print(channel);
    Serial.println(") - tune did not take effect.");
  } else {
    Serial.println("Tuning timed out or failed. Check antenna and signal strength.");
  }
}

void dumpRegisters() {
  Serial.println("\n--- AR1010 Detailed Register Dump (PDF Page 28) ---");
  for (uint8_t i = 0x00; i <= 0x1C; i++) {
    uint16_t val = readRegister(i);
    Serial.print("R");
    if (i < 10) Serial.print("0");
    Serial.print(i, DEC);
    Serial.print(" (0x");
    if (i < 0x10) Serial.print("0");
    Serial.print(i, HEX);
    Serial.print("): 0x");
    if (val < 0x1000) Serial.print("0");
    if (val < 0x0100) Serial.print("0");
    if (val < 0x0010) Serial.print("0");
    Serial.print(val, HEX);
    Serial.print(" - ");

    switch(i) {
      case 0x00: Serial.println("R0: xo_en, ENABLE"); break;
      case 0x01: Serial.println("R1: rds_en, rds_int_en, stc_int_en, deemp, mono, smute, hmute"); break;
      case 0x02: Serial.println("R2: TUNE (D9), CHAN<8:0> (D8-D0)"); break;
      case 0x03: Serial.println("R3: SEEKUP, SEEK, SPACE, BAND<1:0>, VOLUME<3:0>, SEEKTH<6:0>"); break;
      case 0x04: Serial.println("R4: (Reserved/Default)"); break;
      case 0x05: Serial.println("R5: (Reserved/Default)"); break;
      case 0x06: Serial.println("R6: (Reserved/Default)"); break;
      case 0x07: Serial.println("R7: (Reserved/Default)"); break;
      case 0x08: Serial.println("R8: (Reserved/Default)"); break;
      case 0x09: Serial.println("R9: (Reserved/Default)"); break;
      case 0x0A: Serial.println("R10: seek wrap"); break;
      case 0x0B: Serial.println("R11: hilo side, hiloctrl_b1, hiloctrl_b2"); break;
      case 0x0C: Serial.println("R12: (Reserved/Default)"); break;
      case 0x0D: Serial.println("R13: GPIO3<1:0>, GPIO2<1:0>, GPIO1<1:0>"); break;
      case 0x0E: Serial.println("R14: VOLUME2<3:0>"); break;
      case 0x0F: Serial.println("R15: rds_sta_en, rds_mecc<1:0>, rds_ctrl"); break;
      case 0x10: Serial.println("R16: (Reserved/Default)"); break;
      case 0x11: Serial.println("R17: (Reserved/Default)"); break;
      case 0x12: Serial.println("RSSI: RSSI<6:0> (D15-D9), IF_CNT<8:0> (D8-D0)"); break;
      case 0x13: Serial.println("STATUS: READCHAN<8:0> (D15-D7), RDSR (D6), STC (D5), SF (D4), ST (D3)"); break;
      case 0x14: Serial.println("RBS: RBS1<1:0>, RBS2<1:0>, RBS3<1:0>, RBS4<1:0>"); break;
      case 0x15: Serial.println("RDS1: RDS1<15:0>"); break;
      case 0x16: Serial.println("RDS2: RDS2<15:0>"); break;
      case 0x17: Serial.println("RDS3: RDS3<15:0>"); break;
      case 0x18: Serial.println("RDS4: RDS4<15:0>"); break;
      case 0x19: Serial.println("RDS5: rds_dsc<15:0>"); break;
      case 0x1A: Serial.println("RDS6: rds_dfc<15:0>"); break;
      case 0x1B: Serial.println("DEVID: VERSION<3:0>, MFID<11:0>"); break;
      case 0x1C: Serial.println("CHIPID: CHIPNO<15:0>"); break;
      default:   Serial.println("Unknown Register"); break;
    }
  }
  Serial.println("-----------------------------------------------");
}

void printStatus() {
  uint16_t r12 = readRegister(0x12);
  uint16_t r13 = readRegister(0x13);

  Serial.println("\n--- AR1010 Status Breakdown ---");

  if (r13 != 0xFFFF && r12 != 0xFFFF) {
    uint8_t rssi = (r12 >> 9) & 0x7F;
    uint16_t if_cnt = r12 & 0x01FF;

    int readChan = (r13 >> 7) & 0x01FF;
    bool stc = (r13 >> 5) & 0x01;
    bool sf  = (r13 >> 4) & 0x01;
    bool st  = (r13 >> 3) & 0x01;

    float currentFreq = 69.0 + (readChan * 0.1);

    Serial.print("  Current Channel       : "); Serial.print(readChan);
    Serial.print(" ("); Serial.print(currentFreq); Serial.println(" MHz)");
    Serial.print("  STC (Tune Complete)   : "); Serial.println(stc ? "YES (Locked)" : "NO (In Progress)");
    Serial.print("  SF  (Seek Fail)       : "); Serial.println(sf ? "FAIL (No Carrier)" : "OK (Valid Signal)");
    Serial.print("  ST  (Stereo Flag)     : "); Serial.println(st ? "STEREO" : "MONO");
    Serial.print("  RSSI (Signal Strength): "); Serial.println(rssi);
    Serial.print("  IF_CNT (IF Counter)   : "); Serial.println(if_cnt);
  } else {
    Serial.println("  Read Error (0xFFFF) - Check wiring and ensure clean 3.3V power.");
  }
  Serial.println("-------------------------------");
}

void handleCLI() {
  if (!Serial.available()) return;

  String input = Serial.readStringUntil('\n');
  input.trim();

  if (input.length() == 0) return;

  Serial.println(input);

  String cmd = input;
  String arg1 = "";
  String arg2 = "";

  int space1 = input.indexOf(' ');
  if (space1 != -1) {
    cmd = input.substring(0, space1);
    int space2 = input.indexOf(' ', space1 + 1);
    if (space2 != -1) {
      arg1 = input.substring(space1 + 1, space2);
      arg2 = input.substring(space2 + 1);
    } else {
      arg1 = input.substring(space1 + 1);
    }
  }

  cmd.toLowerCase();

  if (cmd == "dump" || cmd == "d") {
    dumpRegisters();
  } else if (cmd == "status" || cmd == "s") {
    printStatus();
  } else if (cmd == "tune" || cmd == "t") {
    if (arg1.length() > 0) {
      float freq = arg1.toFloat();
      if (freq >= 76.0 && freq <= 108.0) {
        setFrequency(freq);
      } else {
        Serial.println("Invalid frequency range (76.0 - 108.0 MHz)");
      }
    } else {
      Serial.println("Usage: tune <freq_in_MHz>");
    }
  } else if (cmd == "r") {
    if (arg1.length() > 0) {
      uint8_t reg = (uint8_t)strtol(arg1.c_str(), NULL, 0);
      uint16_t val = readRegister(reg);
      Serial.print("Reg 0x"); Serial.print(reg, HEX);
      Serial.print(" = 0x"); Serial.println(val, HEX);
    } else {
      Serial.println("Usage: r <reg>");
    }
  } else if (cmd == "w") {
    if (arg1.length() > 0 && arg2.length() > 0) {
      uint8_t reg = (uint8_t)strtol(arg1.c_str(), NULL, 0);
      uint16_t val = (uint16_t)strtol(arg2.c_str(), NULL, 0);
      writeRegister(reg, val);
      Serial.print("Wrote Reg 0x"); Serial.print(reg, HEX);
      Serial.print(" = 0x"); Serial.println(val, HEX);
    } else {
      Serial.println("Usage: w <reg> <val>");
    }
  } else {
    Serial.println("Unknown command. Valid: dump, status, tune <freq>, r <reg>, w <reg> <val>");
  }

  Serial.print("\nAR1010> ");
}

#include <Arduino.h>
#include <Wire.h>

#define AR1010_I2C_ADDR 0x10

// 1 = crystal    (R0 = 0xFFFB, R11 = 0x4E55)  PDF section 3.6
// 0 = ext. clock (R0 = 0xFF7B, R11 = 0x4F55)  PDF section 3.7
#define AR1010_USE_CRYSTAL 1

#define R1_HMUTE (1 << 1)   // R1 D1
#define R2_TUNE (1 << 9)    // R2 D9
#define R2_CHAN_MASK 0x01FF // R2 D8-D0
#define R3_SEEK (1 << 14)   // R3 D14

#define BASELINE 2048 // Digital midpoint for 1.65V bias

bool initAR1010();
void writeRegister(uint8_t reg, uint16_t value);
uint16_t readRegister(uint8_t reg);
void setFrequency(float MHz);
void dumpRegisters();
void printStatus();
void handleCLI();
void runADCTest(uint8_t pin);

void setup()
{
  Serial.begin(115200);
  delay(1000);

  Wire.begin(21, 22); // ESP32 default I2C pins
  Wire.setClock(100000);

  Serial.println("\n=== AR1010 FM Radio & ADC Text Diagnostic CLI ===");
  Serial.println("Commands:");
  Serial.println("  dump / d        - Full register dump with descriptive labels");
  Serial.println("  status / s      - Show parsed status (STC, Seek Fail, RSSI)");
  Serial.println("  tune <MHz>      - Tune to frequency in MHz (e.g., tune 96.9)");
  Serial.println("  r <reg>         - Read single register (e.g., r 0x00)");
  Serial.println("  w <reg> <val>   - Write register hex (e.g., w 0x03 0xA010)");
  Serial.println("  adc <pin>       - Stream clean text stats from an ADC1 pin (e.g., adc 34)");
  Serial.println("================================================================");

  // Configure ADC attenuation globally for 0V - 3.3V range
  analogSetAttenuation(ADC_11db);

  delay(100);

  if (initAR1010())
  {
    Serial.println("AR1010 Initialized & Ready!");
    setFrequency(96.9);
  }
  else
  {
    Serial.println("Initialization / Calibration Failed or Timed Out!");
  }
  Serial.print("\nAR1010> ");
}

void loop()
{
  handleCLI();
}

bool initAR1010()
{
  Serial.println("Configuring R1-R17...");

#if AR1010_USE_CRYSTAL
  const uint16_t R0_VAL = 0xFFFB;
  const uint16_t R11_VAL = 0x4E55;
#else
  const uint16_t R0_VAL = 0xFF7B;
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
  while (millis() - startTime < 1500)
  {
    uint16_t status = readRegister(0x13);
    if (status != 0xFFFF && ((status >> 5) & 0x01))
    {
      Serial.println("Calibration complete (STC flag set).");
      return true;
    }
    delay(10);
  }

  Serial.println("Calibration timeout!");
  return false;
}

void writeRegister(uint8_t reg, uint16_t value)
{
  Wire.beginTransmission(AR1010_I2C_ADDR);
  Wire.write(reg);
  Wire.write((value >> 8) & 0xFF);
  Wire.write(value & 0xFF);
  Wire.endTransmission();
}

uint16_t readRegister(uint8_t reg)
{
  Wire.beginTransmission(AR1010_I2C_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0)
  {
    return 0xFFFF;
  }

  if (Wire.requestFrom((uint8_t)AR1010_I2C_ADDR, (uint8_t)2) == 2)
  {
    uint16_t highByte = Wire.read();
    uint16_t lowByte = Wire.read();
    return (highByte << 8) | lowByte;
  }
  return 0xFFFF;
}

void setFrequency(float MHz)
{
  if (MHz < 76.0 || MHz > 108.0)
  {
    Serial.println("Frequency out of supported range (76.0 - 108.0 MHz)");
    return;
  }

  int channel = (int)((MHz - 69.0) * 10.0 + 0.5);

  uint16_t r1 = readRegister(0x01);
  uint16_t r2 = readRegister(0x02);
  uint16_t r3 = readRegister(0x03);
  if (r1 == 0xFFFF || r2 == 0xFFFF || r3 == 0xFFFF)
  {
    Serial.println("I2C read error - check wiring.");
    return;
  }

  r1 |= R1_HMUTE;
  writeRegister(0x01, r1);

  r2 &= ~R2_TUNE;
  writeRegister(0x02, r2);

  r3 &= ~R3_SEEK;
  writeRegister(0x03, r3);

  uint8_t band = (MHz < 87.5) ? 0x03 : 0x00;
  r3 = (1 << 15) | (0 << 14) | (1 << 13) | (band << 11) | (r3 & 0x07FF);
  writeRegister(0x03, r3);

  r2 = (r2 & ~R2_CHAN_MASK) | (channel & R2_CHAN_MASK);
  writeRegister(0x02, r2);

  r2 |= R2_TUNE;
  writeRegister(0x02, r2);

  unsigned long startTime = millis();
  bool tuned = false;
  while (millis() - startTime < 1000)
  {
    uint16_t status = readRegister(0x13);
    if (status != 0xFFFF && ((status >> 5) & 0x01))
    {
      tuned = true;
      break;
    }
    delay(10);
  }

  r1 &= ~R1_HMUTE;
  writeRegister(0x01, r1);

  uint16_t st = readRegister(0x13);
  int readChan = (st != 0xFFFF) ? ((st >> 7) & 0x01FF) : -1;

  if (tuned && readChan == channel)
  {
    Serial.print("Successfully tuned to: ");
    Serial.print(MHz);
    Serial.println(" MHz");
  }
  else
  {
    Serial.println("Tuning failed.");
  }
}

void dumpRegisters()
{
  Serial.println("\n--- AR1010 Register Dump ---");
  for (uint8_t i = 0x00; i <= 0x1C; i++)
  {
    uint16_t val = readRegister(i);
    Serial.print("R");
    if (i < 10)
      Serial.print("0");
    Serial.print(i, DEC);
    Serial.print(": 0x");
    if (val < 0x1000)
      Serial.print("0");
    if (val < 0x0100)
      Serial.print("0");
    if (val < 0x0010)
      Serial.print("0");
    Serial.println(val, HEX);
  }
  Serial.println("-----------------------------");
}

void printStatus()
{
  uint16_t r12 = readRegister(0x12);
  uint16_t r13 = readRegister(0x13);

  Serial.println("\n--- AR1010 Status ---");
  if (r13 != 0xFFFF && r12 != 0xFFFF)
  {
    uint8_t rssi = (r12 >> 9) & 0x7F;
    int readChan = (r13 >> 7) & 0x01FF;
    bool stc = (r13 >> 5) & 0x01;
    bool st = (r13 >> 3) & 0x01;
    float currentFreq = 69.0 + (readChan * 0.1);

    Serial.print("  Freq   : ");
    Serial.print(currentFreq);
    Serial.println(" MHz");
    Serial.print("  Locked : ");
    Serial.println(stc ? "YES" : "NO");
    Serial.print("  Stereo : ");
    Serial.println(st ? "YES" : "NO");
    Serial.print("  RSSI   : ");
    Serial.println(rssi);
  }
  else
  {
    Serial.println("  Read Error (0xFFFF)");
  }
  Serial.println("---------------------");
}

// --- TIMED BUFFER ADC ANALYSIS & ROBUST CLASSIFICATION ---
// --- TIMED BUFFER ADC ANALYSIS & ROBUST CLASSIFICATION ---
void runADCTest(uint8_t pin)
{
  pinMode(pin, INPUT);
  Serial.printf("\nStreaming 2-second window stats from ADC Pin %d. Send any key/enter to exit.\n", pin);

  while (!Serial.available())
  {
    const int sampleCount = 512;
    int samples[sampleCount];
    uint16_t minVal = 4095;
    uint16_t maxVal = 0;
    uint32_t sum = 0;
    uint32_t absoluteDeltaSum = 0;

    for (int i = 0; i < sampleCount; i++)
    {
      uint16_t raw = analogRead(pin);
      samples[i] = raw;
      if (raw < minVal)
        minVal = raw;
      if (raw > maxVal)
        maxVal = raw;
      sum += raw;

      if (i > 0)
      {
        absoluteDeltaSum += abs(samples[i] - samples[i - 1]);
      }

      delayMicroseconds(3000);
    }

    uint16_t avg = sum / sampleCount;
    int peakToPeak = maxVal - minVal;
    uint32_t avgDelta = absoluteDeltaSum / (sampleCount - 1);

    // Read status registers from AR1010
    uint16_t r12 = readRegister(0x12);
    uint16_t r13 = readRegister(0x13);
    uint8_t rssi = (r12 != 0xFFFF) ? ((r12 >> 9) & 0x7F) : 0;
    bool isStereo = (r13 != 0xFFFF) && ((r13 >> 3) & 0x01);

    // Classification Logic using RSSI, Stereo Flag, and Waveform Delta
    String audioStatus = "[SILENCE              ]";

    if (rssi < 15 || peakToPeak < 250)
    {
      audioStatus = "[NO SIGNAL / STATIC   ]";
    }
    else if (isStereo && avgDelta > 90)
    {
      // Real broadcast stations trigger the hardware stereo pilot and have high audio dynamics
      audioStatus = "[ACTIVE BROADCAST AUDIO]";
    }
    else if (peakToPeak >= 550 && avgDelta > 110)
    {
      // Strong signal even if stereo flag is flickering
      audioStatus = "[ACTIVE BROADCAST AUDIO]";
    }
    else
    {
      // High RSSI or P2P, but lacks the clean dynamics of a broadcast (Static)
      audioStatus = "[RF NOISE / STATIC    ]";
    }

    Serial.printf("Pin: %2d | Avg: %4d | P2P: %4d | RSSI: %2d | Delta: %3d | Stereo: %3s | Status: %s\n",
                  pin, avg, peakToPeak, rssi, avgDelta, isStereo ? "YES" : "NO", audioStatus.c_str());
  }

  while (Serial.available())
    Serial.read();
  Serial.println("\nExited ADC Test.");
}

void handleCLI()
{
  if (!Serial.available())
    return;

  String input = Serial.readStringUntil('\n');
  input.trim();
  if (input.length() == 0)
    return;

  Serial.println(input);

  String cmd = input;
  String arg1 = "";
  String arg2 = "";

  int space1 = input.indexOf(' ');
  if (space1 != -1)
  {
    cmd = input.substring(0, space1);
    int space2 = input.indexOf(' ', space1 + 1);
    if (space2 != -1)
    {
      arg1 = input.substring(space1 + 1, space2);
      arg2 = input.substring(space2 + 1);
    }
    else
    {
      arg1 = input.substring(space1 + 1);
    }
  }

  cmd.toLowerCase();

  if (cmd == "dump" || cmd == "d")
  {
    dumpRegisters();
  }
  else if (cmd == "status" || cmd == "s")
  {
    printStatus();
  }
  else if (cmd == "tune" || cmd == "t")
  {
    if (arg1.length() > 0)
    {
      setFrequency(arg1.toFloat());
    }
    else
    {
      Serial.println("Usage: tune <freq_in_MHz>");
    }
  }
  else if (cmd == "r")
  {
    if (arg1.length() > 0)
    {
      uint8_t reg = (uint8_t)strtol(arg1.c_str(), NULL, 0);
      uint16_t val = readRegister(reg);
      Serial.print("Reg 0x");
      Serial.print(reg, HEX);
      Serial.print(" = 0x");
      Serial.println(val, HEX);
    }
    else
    {
      Serial.println("Usage: r <reg>");
    }
  }
  else if (cmd == "w")
  {
    if (arg1.length() > 0 && arg2.length() > 0)
    {
      uint8_t reg = (uint8_t)strtol(arg1.c_str(), NULL, 0);
      uint16_t val = (uint16_t)strtol(arg2.c_str(), NULL, 0);
      writeRegister(reg, val);
      Serial.print("Wrote Reg 0x");
      Serial.print(reg, HEX);
      Serial.print(" = 0x");
      Serial.println(val, HEX);
    }
    else
    {
      Serial.println("Usage: w <reg> <val>");
    }
  }
  else if (cmd == "adc")
  {
    if (arg1.length() > 0)
    {
      uint8_t pin = arg1.toInt();
      runADCTest(pin);
    }
    else
    {
      Serial.println("Usage: adc <pin> (e.g., adc 34)");
    }
  }
  else
  {
    Serial.println("Unknown command. Valid: dump, status, tune <freq>, r <reg>, w <reg> <val>, adc <pin>");
  }

  Serial.print("\nAR1010> ");
}
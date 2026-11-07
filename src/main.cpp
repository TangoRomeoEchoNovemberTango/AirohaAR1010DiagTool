#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "secrets.h"
#include <esp_wifi.h> // Required for esp_wifi_set_max_tx_power()
#include <math.h>     // REQUIRED for fabsf()
// Include ESP-IDF ADC Continuous Driver Headers
#include "esp_adc/adc_continuous.h"
#include "driver/adc.h"

#define AR1010_I2C_ADDR 0x10
#define AR1010_USE_CRYSTAL 1
#define R1_HMUTE (1 << 1)
#define R2_TUNE (1 << 9)
#define R2_CHAN_MASK 0x01FF
#define R3_SEEK (1 << 14)

const char *ssid = WIFI_SSID;
const char *password = WIFI_PASSWORD;

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

int currentAdcPin = 32;
TaskHandle_t AudioTaskHandle = NULL;

// --- AUDIO CONFIGURATION ---
#define TARGET_SAMPLE_RATE 20000                                   // Hz
#define FRAME_MS 20                                                // ms per packet
#define SAMPLES_PER_FRAME ((TARGET_SAMPLE_RATE * FRAME_MS) / 1000) // 400 samples
#define BYTES_PER_SAMPLE 2
#define UDP_PAYLOAD_BYTES (SAMPLES_PER_FRAME * BYTES_PER_SAMPLE) // 800 bytes

// Helper: Map GPIO to ADC Channel Unit/ID for ESP32 Classic
struct AdcMap
{
  uint8_t unit;
  uint8_t channel;
};

AdcMap getAdcConfig(int gpio)
{
  if (gpio == 32)
    return {ADC_UNIT_1, ADC_CHANNEL_4};
  if (gpio == 33)
    return {ADC_UNIT_1, ADC_CHANNEL_5};
  if (gpio == 34)
    return {ADC_UNIT_1, ADC_CHANNEL_6};
  if (gpio == 35)
    return {ADC_UNIT_1, ADC_CHANNEL_7};
  if (gpio == 36)
    return {ADC_UNIT_1, ADC_CHANNEL_0}; // VP
  if (gpio == 39)
    return {ADC_UNIT_1, ADC_CHANNEL_1}; // VN
  return {ADC_UNIT_1, ADC_CHANNEL_4};
}

// Global Handle for ADC Continuous Mode
static adc_continuous_handle_t g_adc_handle = NULL;
static bool g_adc_initialized = false;

// --- HIGH-QUALITY FLOATING POINT BIQUAD FILTER ---
// Target: ~14kHz Cutoff @ 20kHz Sample Rate (Preserves max FM detail)
// Using Cascaded Biquads for 4th Order Butterworth Response
struct BiquadCoeffs
{
  float b0, b1, b2;
  float a1, a2;
};

// State variables for two cascaded stages
static float z1_s1 = 0.0f, z2_s1 = 0.0f;
static float z1_s2 = 0.0f, z2_s2 = 0.0f;

// DC Removal State
static float dc_offset = 2048.0f;

// Pre-calculated coefficients for 20kHz fs, ~14kHz fc
// Calculated using standard bilinear transform for Butterworth Q=0.707
static const BiquadCoeffs coeffs_s1 = {
    .b0 = 0.1247f, .b1 = 0.2494f, .b2 = 0.1247f, .a1 = -0.6683f, .a2 = 0.1758f};
static const BiquadCoeffs coeffs_s2 = {
    .b0 = 0.1247f, .b1 = 0.2494f, .b2 = 0.1247f, .a1 = -0.6683f, .a2 = 0.1758f};

// Helper: Apply one Biquad stage
static inline float apply_biquad(float x, float &z1, float &z2, const BiquadCoeffs &c)
{
  float y = c.b0 * x + z1;
  z1 = c.b1 * x - c.a1 * y + z2;
  z2 = c.b2 * x - c.a2 * y;
  return y;
}

static inline int16_t process_audio_sample(uint16_t raw)
{
  // 1. Convert to Float
  float input = (float)raw;

  // 2. DC Offset Removal (High Pass)
  // Very slow tracking to preserve bass, alpha ~ 0.001
  dc_offset += 0.001f * (input - dc_offset);
  float centered = input - dc_offset;

  // 3. Normalize to +/- 1.0 range
  // Max ADC swing is ~2048 counts around center
  float normalized = centered / 2048.0f;

  // Clamp to prevent filter instability
  if (normalized > 1.0f)
    normalized = 1.0f;
  if (normalized < -1.0f)
    normalized = -1.0f;

  // 4. Apply Cascaded Low Pass Filters (Anti-Alias / Smooth)
  float out1 = apply_biquad(normalized, z1_s1, z2_s1, coeffs_s1);
  float out2 = apply_biquad(out1, z1_s2, z2_s2, coeffs_s2);

  // 5. Optional Light Compression/Limiter to prevent clipping pops
  // Soft clip at 0.95 to leave headroom
  if (out2 > 0.95f)
    out2 = 0.95f + (out2 - 0.95f) * 0.1f;
  if (out2 < -0.95f)
    out2 = -0.95f + (out2 + 0.95f) * 0.1f;

  // 6. Convert back to Int16 PCM
  float pcm_float = out2 * 32767.0f;

  // Final Clamp
  if (pcm_float > 32767.0f)
    pcm_float = 32767.0f;
  if (pcm_float < -32768.0f)
    pcm_float = -32768.0f;

  return (int16_t)pcm_float;
}

void initHardwareAdc(int gpio)
{
  if (g_adc_handle != NULL)
  {
    adc_continuous_stop(g_adc_handle);
    g_adc_handle = NULL;
    g_adc_initialized = false;
  }

  AdcMap map = getAdcConfig(gpio);

  adc_continuous_handle_cfg_t handle_cfg = {
      .max_store_buf_size = 8192,
      .conv_frame_size = 2048,
  };

  esp_err_t err = adc_continuous_new_handle(&handle_cfg, &g_adc_handle);
  if (err != ESP_OK)
  {
    Serial.println("ADC Handle Creation Failed");
    return;
  }

  adc_digi_pattern_config_t pattern = {
      .atten = ADC_ATTEN_DB_12,
      .channel = map.channel,
      .unit = map.unit,
      .bit_width = ADC_BITWIDTH_12,
  };

  adc_continuous_config_t adc_cfg = {};
  adc_cfg.sample_freq_hz = TARGET_SAMPLE_RATE;
  adc_cfg.conv_mode = ADC_CONV_SINGLE_UNIT_1;
  adc_cfg.format = ADC_DIGI_OUTPUT_FORMAT_TYPE1;
  adc_cfg.pattern_num = 1;
  adc_cfg.adc_pattern = &pattern;

  err = adc_continuous_config(g_adc_handle, &adc_cfg);
  if (err != ESP_OK)
  {
    Serial.println("ADC Config Failed");
    return;
  }

  err = adc_continuous_start(g_adc_handle);
  if (err != ESP_OK)
  {
    Serial.println("ADC Start Failed");
    return;
  }

  g_adc_initialized = true;
  Serial.printf("[ADC] Hardware Stream Started on GPIO %d\n", gpio);
}

// Embedded HTML & JavaScript Dashboard
// Embedded HTML & JavaScript Dashboard
const char index_html[] PROGMEM = R"raw(
<!DOCTYPE html>
<html>
<head>
<title>AR1010 Live Audio</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body { font-family: Arial, sans-serif; text-align: center; background: #121212; color: #fff; margin: 0; padding: 20px; }
.card { background: #1e1e1e; padding: 20px; border-radius: 10px; display: inline-block; max-width: 600px; width: 100%; box-shadow: 0 4px 6px rgba(0,0,0,0.3); }
button { padding: 10px 15px; font-size: 16px; margin: 10px; border-radius: 5px; border: none; cursor: pointer; background: #00d2d3; color: #121212; font-weight: bold; transition: background 0.2s; }
button:hover { opacity: 0.9; }
button.recording { background: #ff4757; color: #fff; animation: pulse 1.5s infinite; }
@keyframes pulse { 0% { box-shadow: 0 0 0 0 rgba(255, 71, 87, 0.7); } 70% { box-shadow: 0 0 0 10px rgba(255, 71, 87, 0); } 100% { box-shadow: 0 0 0 0 rgba(255, 71, 87, 0); } }
canvas { background: #000; border-radius: 5px; margin-top: 15px; width: 100%; height: 200px; }
.controls { margin-top: 15px; padding-top: 15px; border-top: 1px solid #333; }
.status-text { font-size: 12px; color: #aaa; margin-top: 5px; min-height: 1.2em; }
select { padding: 8px; background: #333; color: white; border: 1px solid #555; border-radius: 4px; }
.info-box { margin-top: 15px; text-align: left; font-size: 12px; color: #888; background: #111; padding: 10px; border-radius: 5px; }
</style>
</head>
<body>
<div class="card">
<h2>AR1010 Live Audio</h2>
<p>Active Pin: <span id="activePin">32</span></p>
<select id="pinSelect" onchange="changePin(this.value)">
<option value="32" selected>GPIO 32</option>
<option value="33">GPIO 33</option>
<option value="34">GPIO 34</option>
<option value="35">GPIO 35</option>
<option value="36">GPIO 36</option>
<option value="39">GPIO 39</option>
</select>
<br>
<button id="audioToggleBtn" onclick="toggleAudio()">Enable Audio</button>
<canvas id="waveformCanvas"></canvas>

<!-- RECORDING CONTROLS -->
<div class="controls">
    <button id="recordBtn" onclick="toggleRecording()" disabled>Start Recording</button>
    <div class="status-text" id="recStatus">Idle</div>
</div>

<div class="info-box">
    <strong>Stream Info:</strong><br>
    Protocol: WebSocket (/ws)<br>
    Format: Raw PCM (Signed 16-bit Little Endian)<br>
    Sample Rate: 20kHz | Channels: Mono<br>
    <em>Note: Recorded files are saved as .wav</em>
</div>
</div>

<script>
const canvas = document.getElementById('waveformCanvas');
const ctx = canvas.getContext('2d');

// Global Audio Context - Created ONCE and kept alive
let audioCtx = null;
let isPlaying = false;
let ws = null;

const PRELOAD_MS = 200;
let nextPlayTime = 0;
let bufferQueue = [];
let totalBufferedDuration = 0;

// --- RECORDING STATE ---
let isRecording = false;
let recordedChunks = []; 
let recordStartTime = 0;

function changePin(pin) {
    document.getElementById('activePin').innerText = pin;
    fetch(`/setpin?pin=${pin}`).then(r => r.text()).then(console.log);
}

function toggleAudio() {
    const btn = document.getElementById('audioToggleBtn');
    
    if (!isPlaying) {
        // START PLAYBACK
        
        // 1. Initialize AudioContext only if it doesn't exist
        if (!audioCtx) {
            audioCtx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: 20000 });
        }
        
        // 2. Resume context (handles suspended state from previous stops)
        audioCtx.resume().then(() => {
            console.log("AudioContext Resumed");
            
            // 3. Reset playback state variables
            bufferQueue = [];
            totalBufferedDuration = 0;
            nextPlayTime = 0;
            
            // 4. Connect WebSocket
            initWebSocket();
            
            // 5. Update UI
            isPlaying = true;
            btn.innerText = "Stop Audio";
            btn.classList.add("muted");
            document.getElementById('recordBtn').disabled = false;
        }).catch(err => {
            console.error("Failed to resume AudioContext:", err);
            alert("Please interact with the page again to allow audio.");
        });
        
    } else {
        // STOP PLAYBACK
        
        // 1. Close WebSocket ONLY
        if (ws) {
            ws.close();
            ws = null;
        }
        
        // 2. Do NOT close audioCtx! Just clear queues.
        bufferQueue = [];
        totalBufferedDuration = 0;
        nextPlayTime = 0;
        
        // 3. Update UI
        isPlaying = false;
        btn.innerText = "Enable Audio";
        btn.classList.remove("muted");
        document.getElementById('recordBtn').disabled = true;
        
        // Stop recording if active
        if(isRecording) toggleRecording(); 
    }
}

function initWebSocket() {
    const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    ws = new WebSocket(`${proto}//${window.location.host}/ws`);
    ws.binaryType = 'arraybuffer';
    
    ws.onmessage = function(event) {
        const int16Data = new Int16Array(event.data);
        if (int16Data.length === 0) return;
        
        // 1. Handle Playback (Visualizer + Speaker)
        const float32Data = new Float32Array(int16Data.length);
        for (let i = 0; i < int16Data.length; i++) {
            float32Data[i] = int16Data[i] / 32768.0;
        }
        
        const audioBuffer = audioCtx.createBuffer(1, float32Data.length, 20000);
        audioBuffer.getChannelData(0).set(float32Data);
        bufferQueue.push(audioBuffer);
        totalBufferedDuration += audioBuffer.duration;
        
        if (!nextPlayTime && totalBufferedDuration >= (PRELOAD_MS / 1000)) {
            nextPlayTime = audioCtx.currentTime + 0.1;
            schedulePlayback();
        }
        drawWaveform(int16Data.slice(-100));

        // 2. Handle Recording (Save Raw Bytes)
        if (isRecording) {
            recordedChunks.push(event.data);
            updateRecordUI();
        }
    };
    
    ws.onerror = (e) => console.error("WS Error", e);
    ws.onclose = () => {
        if(isPlaying) {
             console.warn("WebSocket closed unexpectedly");
             // Optionally auto-restart or notify user
        }
    };
}

function schedulePlayback() {
    if (!isPlaying || !audioCtx || audioCtx.state !== 'running') return;
    
    while (bufferQueue.length > 0) {
        const buf = bufferQueue.shift();
        const source = audioCtx.createBufferSource();
        source.buffer = buf;
        source.connect(audioCtx.destination);
        source.start(nextPlayTime);
        nextPlayTime += buf.duration;
    }
    
    if (nextPlayTime < audioCtx.currentTime) {
        nextPlayTime = audioCtx.currentTime + 0.05;
    }
    
    setTimeout(schedulePlayback, 50);
}

function drawWaveform(data) {
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    ctx.strokeStyle = '#00d2d3';
    ctx.lineWidth = 1;
    ctx.beginPath();
    const sliceWidth = canvas.width / data.length;
    let x = 0;
    for (let i = 0; i < data.length; i++) {
        const v = (data[i] + 32768) / 65536.0;
        const y = canvas.height - (v * canvas.height);
        if (i === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
        x += sliceWidth;
    }
    ctx.stroke();
}

// --- RECORDING LOGIC ---
function toggleRecording() {
    const btn = document.getElementById('recordBtn');
    const status = document.getElementById('recStatus');
    
    if (!isRecording) {
        isRecording = true;
        recordedChunks = []; 
        recordStartTime = Date.now();
        btn.innerText = "Stop & Save";
        btn.classList.add("recording");
        status.innerText = "Recording...";
    } else {
        isRecording = false;
        btn.innerText = "Start Recording";
        btn.classList.remove("recording");
        status.innerText = "Processing...";
        setTimeout(() => { generateAndDownloadWav(); }, 100);
    }
}

function updateRecordUI() {
    const durationSec = Math.floor((Date.now() - recordStartTime) / 1000);
    const mins = Math.floor(durationSec / 60);
    const secs = durationSec % 60;
    document.getElementById('recStatus').innerText = `Recording: ${mins}:${secs.toString().padStart(2, '0')}`;
    if (durationSec > 300) { toggleRecording(); } // Auto-stop after 5 mins
}

function generateAndDownloadWav() {
    if (recordedChunks.length === 0) {
        alert("No audio recorded!");
        document.getElementById('recStatus').innerText = "Error: No data";
        return;
    }

    try {
        let totalLength = 0;
        for (let chunk of recordedChunks) { totalLength += chunk.byteLength; }

        const combinedBuffer = new Uint8Array(totalLength);
        let offset = 0;
        for (let chunk of recordedChunks) {
            combinedBuffer.set(new Uint8Array(chunk), offset);
            offset += chunk.byteLength;
        }

        const sampleRate = 20000;
        const numChannels = 1;
        const bitsPerSample = 16;
        const byteRate = sampleRate * numChannels * (bitsPerSample / 8);
        const blockAlign = numChannels * (bitsPerSample / 8);
        const dataSize = totalLength;
        const fileSize = 36 + dataSize;

        const wavHeader = new ArrayBuffer(44);
        const view = new DataView(wavHeader);

        const writeString = (offset, str) => {
            for (let i = 0; i < str.length; i++) { view.setUint8(offset + i, str.charCodeAt(i)); }
        };

        writeString(0, 'RIFF');
        view.setUint32(4, fileSize, true);
        writeString(8, 'WAVE');
        writeString(12, 'fmt ');
        view.setUint32(16, 16, true);
        view.setUint16(20, 1, true);
        view.setUint16(22, numChannels, true);
        view.setUint32(24, sampleRate, true);
        view.setUint32(28, byteRate, true);
        view.setUint16(32, blockAlign, true);
        view.setUint16(34, bitsPerSample, true);
        writeString(36, 'data');
        view.setUint32(40, dataSize, true);

        const finalBuffer = new Uint8Array(44 + totalLength);
        finalBuffer.set(new Uint8Array(wavHeader), 0);
        finalBuffer.set(combinedBuffer, 44);

        const blob = new Blob([finalBuffer], { type: 'audio/wav' });
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.style.display = 'none';
        a.href = url;
        
        const now = new Date();
        const filename = `ar1010_recording_${now.getFullYear()}${(now.getMonth()+1).toString().padStart(2,'0')}${now.getDate().toString().padStart(2,'0')}_${now.getHours().toString().padStart(2,'0')}${now.getMinutes().toString().padStart(2,'0')}.wav`;
        a.download = filename;
        
        document.body.appendChild(a);
        a.click();
        window.URL.revokeObjectURL(url);
        document.body.removeChild(a);
        
        document.getElementById('recStatus').innerText = "Saved successfully!";
        recordedChunks = []; 
        
    } catch (e) {
        console.error(e);
        document.getElementById('recStatus').innerText = "Error saving file.";
        alert("Failed to generate WAV file: " + e.message);
    }
}
</script>
</body>
</html>
)raw";

// ================= FORWARD DECLARATIONS =================
// These MUST be before any function that uses them
bool initAR1010();
void writeRegister(uint8_t reg, uint16_t value);
uint16_t readRegister(uint8_t reg);
void setFrequency(float MHz);
void dumpRegisters();
void printStatus();
void printWiFiStatus();
void processCommand(String input);
void handleCLI();

// --- HARDWARE ADC STREAMING TASK ---
void audioStreamTask(void *pvParameters)
{
  uint8_t adc_raw_buf[2048];
  uint32_t bytes_read = 0;
  int16_t pcm_frame[SAMPLES_PER_FRAME];
  int frame_fill = 0;

  vTaskDelay(pdMS_TO_TICKS(100));

  while (true)
  {
    if (!g_adc_initialized || ws.count() == 0)
    {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    esp_err_t r = adc_continuous_read(
        g_adc_handle,
        adc_raw_buf,
        sizeof(adc_raw_buf),
        &bytes_read,
        100);

    if (r != ESP_OK || bytes_read == 0)
    {
      continue;
    }

    const adc_digi_output_data_t *out = (const adc_digi_output_data_t *)adc_raw_buf;
    int n_samples = bytes_read / sizeof(adc_digi_output_data_t);

    for (int i = 0; i < n_samples; i++)
    {
      uint16_t raw_val = out[i].type1.data;
      // Apply the high-quality float filter
      pcm_frame[frame_fill++] = process_audio_sample(raw_val);
      if (frame_fill == SAMPLES_PER_FRAME)
      {
        ws.binaryAll((uint8_t *)pcm_frame, sizeof(pcm_frame));
        frame_fill = 0;
      }
    }
  }
}

void setup()
{
  Serial.begin(115200);
  delay(1000);

  Wire.begin(21, 22);
  Wire.setClock(100000);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  Serial.printf("Connecting to Wi-Fi SSID: %s\n", ssid);
  WiFi.begin(ssid, password);

  unsigned long wifiStart = millis();
  bool wifiConnected = false;
  while (millis() - wifiStart < 15000)
  {
    if (WiFi.status() == WL_CONNECTED)
    {
      wifiConnected = true;
      break;
    }
    Serial.print(".");
    delay(500);
  }
  Serial.println();

  if (wifiConnected)
  {
    Serial.println("[WiFi] Connected!");
    // CRITICAL: Reduce Wi-Fi TX power to minimize RF leakage into audio path
    // NOTE: If this crashes your board, comment this line out.
    esp_wifi_set_max_tx_power(40);

    Serial.print("[WiFi] IP: http://");
    Serial.println(WiFi.localIP());

    ws.onEvent([](AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len)
               {
            if (type == WS_EVT_CONNECT) {
                Serial.printf("[WS] Client connected\n");
            } else if (type == WS_EVT_DISCONNECT) {
                Serial.printf("[WS] Client disconnected\n");
            } });
    server.addHandler(&ws);

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
              { request->send(200, "text/html", index_html); });

    server.on("/setpin", HTTP_GET, [](AsyncWebServerRequest *request)
              {
            if (request->hasParam("pin")) {
                int newPin = request->getParam("pin")->value().toInt();
                currentAdcPin = newPin;
                initHardwareAdc(currentAdcPin);
                request->send(200, "text/plain", "Pin updated to " + String(newPin));
            } else {
                request->send(400, "text/plain", "Bad Request");
            } });

    server.begin();
  }
  else
  {
    Serial.println("[ERROR] Wi-Fi failed!");
  }

  initHardwareAdc(currentAdcPin);

  xTaskCreatePinnedToCore(
      audioStreamTask,
      "AudioTask",
      8192,
      NULL,
      configMAX_PRIORITIES - 2,
      &AudioTaskHandle,
      1);

  Serial.println("\n=== AR1010 Ready ===");
  if (initAR1010())
  {
    setFrequency(96.9);
  }
  Serial.print("\nAR1010> ");
}

// --- NON-BLOCKING ROBUST CLI HANDLER ---
void handleCLI()
{
  static String inputString = "";
  static bool lastWasLineEnd = false;
  static char lastLineChar = 0;

  while (Serial.available())
  {
    char inChar = (char)Serial.read();

    if (inChar == '\r' || inChar == '\n')
    {
      if (lastWasLineEnd && inChar != lastLineChar)
      {
        // Second char of a CR+LF or LF+CR pair — absorb it, do nothing
        lastWasLineEnd = false;
        lastLineChar = 0;
      }
      else
      {
        // First (or only) line-ending character — process the command
        lastWasLineEnd = true;
        lastLineChar = inChar;

        inputString.trim();
        if (inputString.length() > 0)
        {
          Serial.println();
          processCommand(inputString);
          inputString = "";
        }
        Serial.print("AR1010> "); // Now prints ONCE per Enter press, guaranteed
      }
    }
    else
    {
      lastWasLineEnd = false;
      lastLineChar = 0;
      Serial.print(inChar);
      inputString += inChar;
    }
  }
}

void loop()
{
  handleCLI();
  vTaskDelay(pdMS_TO_TICKS(1));
}

void processCommand(String input)
{
  String cmd = input;
  String arg1 = "";
  int space1 = input.indexOf(' ');
  if (space1 != -1)
  {
    cmd = input.substring(0, space1);
    arg1 = input.substring(space1 + 1);
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
  else if (cmd == "wifi")
  {
    printWiFiStatus();
  }
  else if (cmd == "tune" || cmd == "t")
  {
    if (arg1.length() > 0)
    {
      setFrequency(arg1.toFloat());
    }
    else
    {
      Serial.println("Usage: tune <MHz>");
    }
  }
  else if (cmd == "help" || cmd == "?")
  {
    Serial.println("--- Commands ---");
    Serial.println("  dump / d       : Register dump");
    Serial.println("  status / s     : Radio status");
    Serial.println("  wifi           : Wi-Fi info");
    Serial.println("  tune <MHz> / t : Tune frequency");
    Serial.println("------------------");
  }
  else
  {
    Serial.println("Unknown command. Type 'help'.");
  }
}

// --- AR1010 FUNCTIONS ---
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
      Serial.println("Calibration complete.");
      return true;
    }
    delay(10);
  }
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
    return 0xFFFF;
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
    Serial.println("Frequency out of range (76.0-108.0)");
    return;
  }
  int channel = (int)((MHz - 69.0) * 10.0 + 0.5);
  uint16_t r1 = readRegister(0x01);
  uint16_t r2 = readRegister(0x02);
  uint16_t r3 = readRegister(0x03);
  if (r1 == 0xFFFF || r2 == 0xFFFF || r3 == 0xFFFF)
  {
    Serial.println("I2C Error");
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
  while (millis() - startTime < 1000)
  {
    uint16_t status = readRegister(0x13);
    if (status != 0xFFFF && ((status >> 5) & 0x01))
      break;
    delay(10);
  }

  r1 &= ~R1_HMUTE;
  writeRegister(0x01, r1);
  Serial.print("Tuned to: ");
  Serial.print(MHz);
  Serial.println(" MHz");
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

void printWiFiStatus()
{
  Serial.println("\n--- Wi-Fi Diagnostics ---");
  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.print("  IP Address  : http://");
    Serial.println(WiFi.localIP());
    Serial.print("  RSSI        : ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  }
  else
  {
    Serial.println("  Not connected.");
  }
  Serial.println("-------------------------");
}
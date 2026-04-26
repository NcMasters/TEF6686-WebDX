// ============================================================
// TEF6686 DX Analyzer Pro — ESP32-C3 SuperMini Edition
// ============================================================
// Adapted from the ESP32-S2 Mini version.
//
// CHANGES FROM ESP32-S2 MINI VERSION:
//   1. I2C pins remapped: SDA=GPIO4, SCL=GPIO5 (C3 defaults,
//      avoids strapping pin GPIO2 and LED pin GPIO8)
//   2. Serial: Uses native USB-Serial/JTAG via CDC.
//      Arduino IDE: Set "USB CDC On Boot" → Enabled.
//      Board: "ESP32C3 Dev Module" or "XIAO_ESP32C3"
//   3. I2C bus speed kept at 50kHz for TEF6686 compatibility.
//   4. pgm_read_byte / PROGMEM: Works as no-ops on ESP32-C3
//      (same as S2), no change needed in DSP_INIT.h.
//   5. 3.3V logic — same as S2, native TEF6686 compatible.
//   6. WiFi + WebSocket server on port 81 for wireless control.
//      Install "WebSockets" library by Markus Sattler (Links2004)
//      from Arduino Library Manager before compiling.
//
// WIRING (ESP32-C3 SuperMini → TEF6686):
//   GPIO4 (SDA) → TEF6686 SDA
//   GPIO5 (SCL) → TEF6686 SCL
//   3V3         → TEF6686 VCC
//   GND         → TEF6686 GND
//   (4.7kΩ pull-ups on SDA/SCL to 3.3V recommended)
// ============================================================

#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include "DSP_INIT.h"

// --- Embedded Web Interface (Compressed) ---
#include "html_hex.h"
#include "db_hex.h"

Preferences preferences;

// --- ESP32-C3 SuperMini I2C Pin Assignment (Defaults) ---
int sdaPin = 4;
int sclPin = 5;
#define TEF_ADDR 0x64

WebServer server(80);
WebSocketsServer webSocket(81);

uint16_t currentFreq = 10490; 
uint8_t currentMode = 1;      // 1 = FM (Module 32), 2 = AM (Module 33)
int16_t volume = -10;
uint16_t bwMode = 0;   
uint16_t bwLimit = 2360; 
bool highCut = true;
bool lowCut = true;
uint16_t deemphMode = 500;
uint16_t amAtten = 0;    // AM Antenna Attenuation (0.1 dB units)
uint16_t amCoChan = 0;   // AM Co-Channel Detector (0=Off, 1=On)
uint16_t amAgcStart = 900; // AM RF AGC Start Level (0.1 dBuV units, 900 = 90dBuV)
uint16_t amAudioNB = 0;   // AM Audio Noise Blanker (0=Off, 1=On)
uint16_t amSoftMute = 0;  // AM Softmute (0=Off, 1=On)

// --- System States ---
uint8_t scanMode = 0; // 0 = Off, 1 = Listen Scan, 2 = Blitz Scan
bool i2cActive = true; 
bool smartI2C = true; 
uint8_t imsMode = 1;   // 0=Off, 1=Normal
bool eqActive = false;
bool nbActive = true;
uint16_t lastStereoBlend = 1000; 
unsigned long lastTuneTime = 0;
bool i2cQuiet = false; // Persistent: true when Smart I2C has entered quiet mode
bool autoDX = false; 
unsigned long lastDXCheck = 0;
bool lastDXState = false;

// --- WiFi + WebSocket ---
String ssid     = "YOUR_WIFI_SSID";
String password = "YOUR_WIFI_PASSWORD";
bool wifiReady = false;

// --- I2C Communication with Retries ---
void tef_send(uint8_t m, uint8_t c, uint8_t index, uint16_t* p, uint8_t ct, bool stop = true) {
  if (!i2cActive) return;
  uint8_t err = 0;
  for (uint8_t retry = 0; retry < 3; retry++) {
    Wire.beginTransmission(TEF_ADDR);
    Wire.write(m); Wire.write(c); Wire.write(index); 
    for(uint8_t i = 0; i < ct; i++) { 
      Wire.write(p[i] >> 8); 
      Wire.write(p[i] & 0xFF); 
    }
    err = Wire.endTransmission(stop);
    if (err == 0) return; // Success
    delay(2); 
  }
}

void dsp_write_data(const uint8_t* data) {
  uint8_t *pa = (uint8_t *)data;
  uint8_t len, first;
  for (;;) {
    len = pgm_read_byte_near(pa++);
    first = pgm_read_byte_near(pa);
    if (!len) break;
    
    if (len == 2 && first == 0xff) { 
      delay(pgm_read_byte_near(++pa)); 
      pa++; 
    } else {
      uint8_t err = 0;
      // Retry block for robust initialization
      for (uint8_t retry = 0; retry < 3; retry++) {
        Wire.beginTransmission(TEF_ADDR);
        for (int i = 0; i < len; i++) {
          Wire.write(pgm_read_byte_near(pa + i));
        }
        err = Wire.endTransmission();
        if (err == 0) break;
        delay(5);
      }
      
      if (err != 0) {
        Serial.printf("[I2C] DSP Init Error: %d\n", err);
      }
      pa += len; // Advance pointer after processing
    }
  }
}

uint8_t getModule() {
  return (currentMode == 1) ? 32 : 33; 
}

void syncDSP() {
  uint8_t mod = getModule();
  
  if (currentMode == 1) { // FM Specific
      // 1. Multipath Suppression (iMS) - CMD 20, Index 1
      uint16_t ims = (imsMode > 0) ? 1 : 0;
      tef_send(mod, 20, 1, &ims, 1);

      uint16_t eq = (eqActive ? 1 : 0);
      tef_send(mod, 22, 1, &eq, 1);
      
      uint16_t deemp = deemphMode;
      tef_send(mod, 31, 1, &deemp, 1);

      // 4. Stereo Improvement - CMD 32, Index 1
      uint16_t stImp = 1;
      tef_send(mod, 32, 1, &stImp, 1);
  } else { // AM/Shortwave Specific
      // 1. Antenna Attenuation - CMD 12, Index 1
      tef_send(mod, 12, 1, &amAtten, 1);
      
      // 2. Co-Channel Detector - CMD 14, Index 1
      tef_send(mod, 14, 1, &amCoChan, 1);

      // 3. RF AGC Start Level - CMD 11, Index 1
      tef_send(mod, 11, 1, &amAgcStart, 1);

      // 4. Audio Noise Blanker - CMD 24, Index 1
      tef_send(mod, 24, 1, &amAudioNB, 1);

      // 5. Softmute - CMD 42, Index 1 (Mode)
      uint16_t smMode = amSoftMute;
      tef_send(mod, 42, 1, &smMode, 1);
  }

  // 5. Noise Blanker - CMD 23, Index 1
  // Manual: mode, sensitivity, reserved
  uint16_t nb[] = { (uint16_t)(nbActive ? 1 : 0), 500, 0 };
  tef_send(mod, 23, 1, nb, 3);

  // 5. Highcut Max (Dynamic Highcut limit) - CMD 55, Index 1
  // Manual: mode, limit
  uint16_t hcLimit = (currentMode == 1) ? 2400 : 1500;
  uint16_t hc_max[] = { (uint16_t)(highCut ? 1 : 0), hcLimit };
  tef_send(mod, 55, 1, hc_max, 2);

  // 6. Lowcut Max (Dynamic Lowcut limit) - CMD 57, Index 1
  // Manual: mode, limit
  uint16_t lcLimit = (currentMode == 1) ? 100 : 300;
  uint16_t lc[] = { (uint16_t)(lowCut ? 1 : 0), lcLimit };
  tef_send(mod, 57, 1, lc, 2);

  Serial.println("[DSP] Perfect Sync Complete.");
}

void setFilters() {
  syncDSP(); // Delegate to central sync
}

void setBW(uint16_t mode, uint16_t limit) {
  bwMode = mode; bwLimit = limit;
  if (currentMode == 1) {
    // FM: mode, bandwidth, control_sensitivity, low_level_sensitivity
    uint16_t p[4] = {mode, limit, 1000, 1000};
    tef_send(32, 10, 1, p, 4); 
  } else {
    // AM: mode, bandwidth
    uint16_t p[2] = {mode, limit};
    tef_send(33, 10, 1, p, 2); 
  }
}

void sendUpdate(bool forced = false); // Forward declaration

// --- Helper: send JSON to both Serial AND WebSocket clients ---
void broadcast(const char* msg) {
  Serial.println(msg);
  if (wifiReady) webSocket.broadcastTXT(msg, strlen(msg));
}

// --- DUAL-MODE TUNING ENGINE ---
void tuneRadio(uint16_t f, uint8_t mode) {
  currentFreq = f;
  currentMode = mode; 
  lastTuneTime = millis(); // Reset smart I2C timer on tune
  uint8_t mod = getModule();
  
  if (scanMode == 2) {
      // BLITZ SCAN: Pure speed, no settling needed
      uint16_t p[2] = {1, currentFreq}; 
      tef_send(mod, 1, 1, p, 2); 
      delay(25);              
      sendUpdate(true);           
      return;                 
  } 
  else if (scanMode == 1) {
      // LISTEN SCAN: Slightly slower for accurate RSSI, stays muted
      uint16_t p[2] = {1, currentFreq}; 
      tef_send(mod, 1, 1, p, 2); 
      delay(80);              
      sendUpdate(true);           
      return;    
  }
  
  // NORMAL HUMAN TUNING
  uint16_t m = 1; tef_send(48, 11, 1, &m, 1); 
  delay(40); 
  
  uint16_t p[2] = {1, currentFreq}; 
  tef_send(mod, 1, 1, p, 2);
  delay(200); 

  m = 0; tef_send(48, 11, 1, &m, 1); 
  syncDSP(); // Re-apply all DSP settings after tuning to ensure persistence
  sendUpdate(true);
}

void pollRDS() {
  if (currentMode == 2 || scanMode > 0) return; 
  if (!i2cActive) return;
  
  tef_send(32, 131, 1, NULL, 0, false); 
  Wire.requestFrom(TEF_ADDR, 12);
  if(Wire.available() >= 12) {
    uint16_t status = (Wire.read() << 8) | Wire.read();
    uint16_t a = (Wire.read() << 8) | Wire.read();
    uint16_t b = (Wire.read() << 8) | Wire.read();
    uint16_t c = (Wire.read() << 8) | Wire.read(); 
    uint16_t d = (Wire.read() << 8) | Wire.read();
    uint16_t err = (Wire.read() << 8) | Wire.read();

    if (status & 0x8000) {
      char buf[64];
      snprintf(buf, sizeof(buf), "{\"rds\":[%u,%u,%u,%u,%u]}", a, b, c, d, err);
      broadcast(buf);
    }
  } else {
    // Flush buffer if incomplete read to avoid desync
    while(Wire.available()) Wire.read();
  }
}

void sendUpdate(bool forced) {
  if(!i2cActive) return;

  uint8_t mod = getModule();
  tef_send(mod, 128, 1, NULL, 0, false);
  Wire.requestFrom(TEF_ADDR, 14); 
  
  int16_t status = 0;
  float rssi = 0, usn = 0, wam = 0, offset = 0, snr = 0, mod_pct = 0, bw = 0;

  if(Wire.available() >= 12) {
    status = (int16_t)((Wire.read()<<8)|Wire.read());        // Word 1: Status
    rssi = (int16_t)((Wire.read()<<8)|Wire.read()) / 10.0;    // Word 2: Level
    
    if (currentMode == 1) { // FM Mode (Module 32)
        usn = (uint16_t)((Wire.read()<<8)|Wire.read()) / 10.0;    // Word 3: USN
        wam = (uint16_t)((Wire.read()<<8)|Wire.read()) / 10.0;    // Word 4: WAM
        offset = (int16_t)((Wire.read()<<8)|Wire.read()) / 10.0; // Word 5: Offset
        bw = (uint16_t)((Wire.read()<<8)|Wire.read()) / 10.0;     // Word 6: Bandwidth
        mod_pct = (uint16_t)((Wire.read()<<8)|Wire.read()) / 10.0; // Word 7: Modulation
    } else { // AM Mode (Module 33)
        snr = (uint16_t)((Wire.read()<<8)|Wire.read()) / 10.0;    // Word 3: Hardware SNR
        offset = (int16_t)((Wire.read()<<8)|Wire.read()) / 10.0; // Word 4: Offset
        bw = (uint16_t)((Wire.read()<<8)|Wire.read()) / 10.0;     // Word 5: Bandwidth
        mod_pct = (uint16_t)((Wire.read()<<8)|Wire.read()) / 10.0; // Word 6: Modulation
        
        // Clean up any remaining bytes in the buffer to prevent desync
        while(Wire.available()) Wire.read();
    }
  } else {
    while(Wire.available()) Wire.read();
  }

  uint16_t sig_status = 0;
  uint16_t stereo_blend = 0;
  if (currentMode == 1) {
    tef_send(32, 133, 1, NULL, 0, false);
    Wire.requestFrom(TEF_ADDR, 2);
    if(Wire.available() >= 2) {
      sig_status = (Wire.read() << 8) | Wire.read();
    } else {
      while(Wire.available()) Wire.read();
    }
    
    tef_send(32, 134, 1, NULL, 0, false);
    Wire.requestFrom(TEF_ADDR, 8);
    if(Wire.available() >= 8) {
      Wire.read(); Wire.read();
      Wire.read(); Wire.read();
      Wire.read(); Wire.read();
      stereo_blend = (Wire.read() << 8) | Wire.read();
    } else {
      while(Wire.available()) Wire.read();
    }
  }

  // Build JSON and broadcast
  char buf[256];
  int n = snprintf(buf, sizeof(buf),
    "{\"f\":%d,\"m\":%d,\"v\":%d,\"s\":%.1f,",
    currentFreq, currentMode, volume, rssi);
  
  if (currentMode == 1) {
    n += snprintf(buf+n, sizeof(buf)-n, "\"usn\":%.1f,\"wam\":%.1f,", usn, wam);
  } else {
    n += snprintf(buf+n, sizeof(buf)-n, "\"snr\":%.1f,", snr);
  }
  
  n += snprintf(buf+n, sizeof(buf)-n, "\"o\":%.1f,\"bw\":%.1f,\"mod\":%.1f,\"st\":%d,\"sb\":%u,\"i2cSleep\":%d}", 
    offset, bw, mod_pct, (sig_status & 0x8000) ? 1 : 0, stereo_blend, i2cQuiet ? 1 : 0);
  
  broadcast(buf);
}

void applyStartupCommands() {
  preferences.begin("tef6686", true);
  String cmds = preferences.getString("startupCmds", "");
  preferences.end();
  if (cmds.length() == 0) return;

  // Format: "module,cmd,val1,val2...;module,cmd,val..."
  char* str = strdup(cmds.c_str());
  char* saveptr1;
  char* token1 = strtok_r(str, ";", &saveptr1);
  while (token1 != NULL) {
    char* saveptr2;
    char* token2 = strtok_r(token1, ",", &saveptr2);
    if (token2) {
      uint8_t mod = atoi(token2);
      token2 = strtok_r(NULL, ",", &saveptr2);
      if (token2) {
        uint8_t cmd = atoi(token2);
        uint16_t vals[8];
        uint8_t len = 0;
        while ((token2 = strtok_r(NULL, ",", &saveptr2)) != NULL && len < 8) {
          vals[len++] = atoi(token2);
        }
        if (len > 0) {
          tef_send(mod, cmd, 1, vals, len);
        }
      }
    }
    token1 = strtok_r(NULL, ";", &saveptr1);
  }
  free(str);
}

void saveDSP() {
  preferences.begin("tef6686", false);
  preferences.putInt("ims", imsMode);
  preferences.putInt("eq", eqActive ? 1 : 0);
  preferences.putInt("nb", nbActive ? 1 : 0);
  preferences.putInt("hc", highCut ? 1 : 0);
  preferences.putInt("lc", lowCut ? 1 : 0);
  preferences.putInt("deemph", deemphMode);
  preferences.putInt("amAtten", amAtten);
  preferences.putInt("amCoChan", amCoChan);
  preferences.putInt("amAgcStart", amAgcStart);
  preferences.putInt("amAudioNB", amAudioNB);
  preferences.putInt("amSoftMute", amSoftMute);
  preferences.end();
  Serial.println("[System] Settings saved to Flash memory.");
}

// --- COMMAND PROCESSOR (shared by Serial and WebSocket) ---
// Optimized: Uses char array to avoid String fragmentation
void processCommand(const char* input) {
  if (!input || input[0] == '\0') return;
  
  // Skip leading whitespace
  while (*input == ' ' || *input == '\r' || *input == '\n') input++;
  if (*input == '\0') return;

  char cmd = input[0];
  
  if (cmd == 'F') { int f = atoi(input + 1); tuneRadio(f, 1); }
  else if (cmd == 'A') { int f = atoi(input + 1); tuneRadio(f, 2); }
  else if (cmd == 'v') { volume += 2; uint16_t v[] = {(uint16_t)(volume * 10)}; tef_send(48, 10, 1, v, 1); }
  else if (cmd == 'b') { volume -= 2; uint16_t v[] = {(uint16_t)(volume * 10)}; tef_send(48, 10, 1, v, 1); }
  else if (cmd == 'a') setBW(1, currentMode == 1 ? 2360 : 60);  // AUTO Mode (mode=1)
  else if (cmd == 'n') setBW(0, currentMode == 1 ? 560 : 30);   // FIXED NARROW (mode=0)
  else if (cmd == 'B') { uint16_t b = (uint16_t)atoi(input + 1); setBW(0, b); } // FIXED CUSTOM (mode=0)
  else if (cmd == 'G') { amAgcStart = (uint16_t)atoi(input + 1); syncDSP(); saveDSP(); }
  else if (cmd == 'N') { amAudioNB = (uint16_t)atoi(input + 1); syncDSP(); saveDSP(); }
  else if (cmd == 'T') { amSoftMute = (uint16_t)atoi(input + 1); syncDSP(); saveDSP(); }
  else if (cmd == 'h') { highCut = !highCut; syncDSP(); saveDSP(); }
  else if (cmd == 'l') { lowCut = !lowCut; syncDSP(); saveDSP(); }
  else if (cmd == 'I') { i2cActive = !i2cActive; }
  else if (cmd == 'm') { uint16_t hardMute[] = {(uint16_t)(-60 * 10)}; tef_send(48, 10, 1, hardMute, 1); }
  else if (cmd == 'u') { uint16_t v[] = {(uint16_t)(volume * 10)}; tef_send(48, 10, 1, v, 1); }
  else if (cmd == 'O') { uint16_t p[] = {2, 200}; tef_send(32, 66, 1, p, 2); }
  else if (cmd == 'P') { uint16_t p[] = {0, 200}; tef_send(32, 66, 1, p, 2); }
  // --- SCAN COMMANDS ---
  else if (cmd == 'S') { 
      scanMode = 1; // Listen Scan
      uint16_t hardMute[] = {(uint16_t)(-60 * 10)};
      tef_send(48, 10, 1, hardMute, 1); 
  }  
  else if (cmd == 'Q') { 
      scanMode = 2; // Blitz Scan
      uint16_t hardMute[] = {(uint16_t)(-60 * 10)};
      tef_send(48, 10, 1, hardMute, 1); 
  }
  else if (cmd == 'X') { 
      scanMode = 0; // Off / Unmute
      lastTuneTime = millis(); // Reset timer on scan exit
      uint16_t v[] = {(uint16_t)(volume * 10)}; tef_send(48, 10, 1, v, 1); 
  }
  else if (cmd == 'W') {
      // Format: "Wmod,cmd,idx,v1,v2..."
      char* token = strtok((char*)input + 1, ",");
      if (token) {
        uint8_t mod = atoi(token);
        token = strtok(NULL, ",");
        if (token) {
          uint8_t command = atoi(token);
          token = strtok(NULL, ",");
          if (token) {
            uint8_t idx = atoi(token);
            uint16_t vals[16];
            uint8_t len = 0;
            while ((token = strtok(NULL, ",")) != NULL && len < 16) {
              vals[len++] = atoi(token);
            }
            if (len > 0) tef_send(mod, command, idx, vals, len);
          }
        }
      }
  }
  else if (cmd == 'C') {
      // Format: "Csda,scl"
      int sda, scl;
      if (sscanf(input + 1, "%d,%d", &sda, &scl) == 2) {
          preferences.begin("tef6686", false);
          preferences.putInt("sda", sda);
          preferences.putInt("scl", scl);
          preferences.end();
          Serial.printf("[System] I2C Pins updated to SDA=%d, SCL=%d. Restarting...\n", sda, scl);
          delay(500);
          ESP.restart();
      }
  }
  else if (cmd == 'J') {
      preferences.begin("tef6686", false);
      preferences.putString("uiState", input + 1);
      preferences.end();
  }
  else if (cmd == 'Z') {
      smartI2C = (input[1] == '1');
      preferences.begin("tef6686", false);
      preferences.putBool("smartI2C", smartI2C);
      preferences.end();
  }
  else if (cmd == 'L') { lowCut = (atoi(input + 1) == 1); syncDSP(); saveDSP(); }
  else if (cmd == 'D') { deemphMode = (uint16_t)atoi(input + 1); syncDSP(); saveDSP(); }
  else if (cmd == 'U') { 
      autoDX = (atoi(input + 1) == 1); 
      preferences.begin("tef6686", false);
      preferences.putBool("autoDX", autoDX);
      preferences.end();
  }
  else if (cmd == 'Y') { amAtten = (uint16_t)atoi(input + 1); syncDSP(); saveDSP(); }
  else if (cmd == 'K') { amCoChan = (uint16_t)atoi(input + 1); syncDSP(); saveDSP(); }
  else if (cmd == 'R') {
      preferences.begin("tef6686", true);
      String cmds = preferences.getString("startupCmds", "");
      String uiState = preferences.getString("uiState", "{}");
      int savedSda = preferences.getInt("sda", 4);
      int savedScl = preferences.getInt("scl", 5);
      preferences.end();
      char buf[2048];
      snprintf(buf, sizeof(buf), "{\"initCmds\":\"%s\",\"uiState\":%s,\"sda\":%d,\"scl\":%d,\"smartI2C\":%d,\"ims\":%d,\"eq\":%d,\"nb\":%d,\"hc\":%d,\"lc\":%d,\"deemph\":%d,\"autoDX\":%d,\"am_atten\":%d,\"am_cochan\":%d,\"am_agc\":%d,\"am_audio_nb\":%d,\"am_softmute\":%d,\"ssid\":\"%s\"}", 
               cmds.c_str(), uiState.c_str(), savedSda, savedScl, smartI2C ? 1 : 0, imsMode, eqActive ? 1 : 0, nbActive ? 1 : 0, highCut ? 1 : 0, lowCut ? 1 : 0, deemphMode, autoDX ? 1 : 0, amAtten, amCoChan, amAgcStart, amAudioNB, amSoftMute, ssid.c_str());
      broadcast(buf);
  }
  else if (cmd == 'V') {
      // Format: "Vssid,password"
      char* comma = strchr((char*)input + 1, ',');
      if (comma) {
          *comma = '\0';
          char* newSsid = (char*)input + 1;
          char* newPass = comma + 1;
          preferences.begin("tef6686", false);
          preferences.putString("ssid", newSsid);
          preferences.putString("pass", newPass);
          preferences.end();
          Serial.printf("[WiFi] Credentials updated. SSID: %s. Restarting...\n", newSsid);
          delay(500);
          ESP.restart();
      }
  }
}

// --- WebSocket Event Handler ---
void wsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.printf("[WS] Client #%u disconnected\n", num);
      break;
    case WStype_CONNECTED:
      Serial.printf("[WS] Client #%u connected\n", num);
      sendUpdate(true); // Send current state immediately to new client
      break;
    case WStype_TEXT: {
      // Process commands from WebSocket safely, avoiding String allocation
      if (length > 0 && length < 512) {
        char wsCmd[512];
        memcpy(wsCmd, payload, length);
        wsCmd[length] = '\0';
        processCommand(wsCmd);
      }
      break;
    }
    default:
      break;
  }
}

void setup() {
  Serial.begin(115200);
  
  // ESP32-C3 SuperMini: Wait briefly for USB CDC to enumerate
  delay(1500);

  preferences.begin("tef6686", true);
  sdaPin = preferences.getInt("sda", 4);
  sclPin = preferences.getInt("scl", 5);
  smartI2C = preferences.getBool("smartI2C", true);
  imsMode = preferences.getInt("ims", 1);
  eqActive = preferences.getInt("eq", 1) == 1;
  nbActive = preferences.getInt("nb", 1) == 1;
  highCut = preferences.getInt("hc", 1) == 1;
  lowCut = preferences.getInt("lc", 1) == 1;
  autoDX = preferences.getBool("autoDX", true);
  amAtten = preferences.getInt("amAtten", 0);
  amCoChan = preferences.getInt("amCoChan", 0);
  amAgcStart = preferences.getInt("amAgcStart", 900);
  amAudioNB = preferences.getInt("amAudioNB", 0);
  amSoftMute = preferences.getInt("amSoftMute", 0);
  preferences.end();
  
  Wire.begin(sdaPin, sclPin, 50000); 
  Wire.setTimeOut(100); // Prevent hangs if I2C bus is stuck

  Serial.printf("[System] Settings loaded. I2C Started on SDA:%d, SCL:%d\n", sdaPin, sclPin);
  Serial.printf("[System] Smart I2C: %s\n", smartI2C ? "ON" : "OFF");

  // Check if TEF is already initialized (e.g. after ESP32 reset)
  Wire.beginTransmission(TEF_ADDR);
  if (Wire.endTransmission() == 0) {
    bool needsInit = true;
    
    // Try to read multiple times to ensure stability
    for(int i=0; i<3; i++) {
        tef_send(64, 128, 1, NULL, 0, false); // APPL_Get_Operation_Status with Repeated Start and Index 1
        Wire.requestFrom(TEF_ADDR, 2);
        if (Wire.available() >= 2) {
          uint16_t state = (Wire.read() << 8) | Wire.read();
          // state 1 = Booting, 2 = Idle, 3 = Active
          if (state >= 1 && state <= 4) {
              needsInit = false; 
              break;
          }
        }
        delay(10);
    }

    if (needsInit) {
      Serial.println("[TEF6686] Uploading firmware...");
      dsp_write_data(DSP_INIT);
      uint16_t act = 0; tef_send(64, 1, 1, &act, 1);
    } else {
      Serial.println("[TEF6686] Firmware already active, skipping upload.");
    }

    // Module 48: 20=Set_Ana_Out (128=DAC, 1=Enabled)
    uint16_t dac[] = {128, 1}; tef_send(48, 20, 1, dac, 2);
    // Module 48: 13=Set_Output_Source (128=DAC, 224=Audio Processor)
    uint16_t route[] = {128, 224}; tef_send(48, 13, 1, route, 2); 
    
    // Module 32: 81=Set_RDS (1=Decoder Mode, 2=Auto Restart, 0=No Pin)
    uint16_t rds[] = {1, 2, 0}; tef_send(32, 81, 1, rds, 3);
    
    uint16_t vol[] = {(uint16_t)(volume * 10)}; tef_send(48, 10, 1, vol, 1);
    tuneRadio(currentFreq, 1);
    setFilters();
    applyStartupCommands();
    lastTuneTime = millis(); // Initialize smart I2C timer after setup
  } else {
    Serial.println("[System] TEF6686 NOT FOUND at 0x64! Check wiring/power.");
  }

  // --- WiFi Setup (non-blocking: runs in background) ---
  ssid = preferences.getString("ssid", "YOUR_WIFI_SSID");
  password = preferences.getString("pass", "YOUR_WIFI_PASSWORD");
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  Serial.print("[WiFi] Connecting");

  // Wait up to 8 seconds for WiFi
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 8000) {
    delay(250);
    Serial.print(".");
  }

    if (WiFi.status() == WL_CONNECTED) {
    wifiReady = true;
    WiFi.setSleep(false); // CRITICAL: Prevent ESP32 from dropping mDNS broadcast packets
    Serial.println();
    Serial.print("[WiFi] Connected! IP: ");
    Serial.println(WiFi.localIP());

    // Explicitly start MDNS so it's ready before OTA binds to it
    if (MDNS.begin("tef6686")) {
      Serial.println("[WiFi] mDNS started: tef6686.local");
    }

    // --- OTA Setup ---
    ArduinoOTA.setHostname("tef6686");
    
    ArduinoOTA.onStart([]() {
      Serial.println("[OTA] Start updating");
      // CRITICAL: Disable telemetry and RDS polling so the ESP32 has enough resources to flash
      i2cActive = false; 
      scanMode = 0;
    });
    ArduinoOTA.onEnd([]() {
      Serial.println("\n[OTA] End");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("[OTA] Progress: %u%%\r", (progress / (total / 100)));
    });
    ArduinoOTA.onError([](ota_error_t error) {
      Serial.printf("[OTA] Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });

    ArduinoOTA.begin();
    Serial.println("[WiFi] OTA server started (mDNS: tef6686.local)");

    // Add our custom services to the mDNS instance started by ArduinoOTA
    MDNS.addService("ws", "tcp", 81);

    server.on("/", []() {
      server.sendHeader("Content-Encoding", "gzip");
      server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
      server.send_P(200, "text/html; charset=UTF-8", (const char*)WEB_HTML, WEB_HTML_LEN);
    });

    server.on("/stations_db.js", []() {
      server.sendHeader("Content-Encoding", "gzip");
      server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
      server.send_P(200, "application/javascript; charset=UTF-8", (const char*)WEB_DB, WEB_DB_LEN);
    });

    server.begin();
    Serial.println("[WiFi] HTTP server on port 80");

    webSocket.begin();
    webSocket.onEvent(wsEvent);
    Serial.println("[WiFi] WebSocket server on port 81");
  } else {
    Serial.println();
    Serial.println("[WiFi] Connection failed — running USB-only mode");
  }
}

void loop() {
  // Handle Serial commands efficiently without String allocation
  static char serialBuf[64];
  static size_t serialIdx = 0;

  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialIdx > 0) {
        serialBuf[serialIdx] = '\0';
        processCommand(serialBuf);
        serialIdx = 0;
      }
    } else if (serialIdx < sizeof(serialBuf) - 1) {
      serialBuf[serialIdx++] = c;
    }
  }

  // Handle HTTP requests
  if (wifiReady) {
    server.handleClient();
    webSocket.loop();
    ArduinoOTA.handle();
  }

  // Reconnect WiFi if it drops
  static unsigned long lastWifiCheck = 0;
  if (wifiReady && millis() - lastWifiCheck > 10000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] Lost connection, reconnecting...");
      WiFi.reconnect();
    }
  }

  // --- Smart I2C Quiet Mode ---
  // Determine if we should be in quiet mode this iteration.
  // Quiet = smartI2C enabled + weak signal + 10s since last tune
  bool shouldBeQuiet = false;
  if (smartI2C && scanMode == 0 && i2cActive) {
      // stereo_blend: 0 = full stereo (strong), 1000 = full mono (weak)
      // >800 means under 20% stereo = weak signal
      bool weakSignal = (currentMode != 1) || (lastStereoBlend > 800);
      bool timedOut = (millis() - lastTuneTime > 10000);
      shouldBeQuiet = weakSignal && timedOut;
  }
  i2cQuiet = shouldBeQuiet;

  // Periodic telemetry updates
  static unsigned long last = 0;
  unsigned long telemetryInterval = i2cQuiet ? 5000 : 500;

  if (millis() - last > telemetryInterval) { 
    last = millis(); 
    if(scanMode == 0) sendUpdate();
  }

  // RDS polling (skip entirely in quiet mode)
  static unsigned long lastRDS = 0;
  if (!i2cQuiet && millis() - lastRDS > 40) {
    lastRDS = millis();
    if(scanMode == 0) pollRDS();
  }

  // --- Auto DX DSP Logic ---
  if (autoDX && currentMode == 1 && scanMode == 0 && !i2cQuiet) {
      if (millis() - lastDXCheck > 1000) {
          lastDXCheck = millis();
          // lastStereoBlend: 0 = full stereo, 1000 = mono
          // Threshold 500 = 50% stereo
          bool shouldBeOn = (lastStereoBlend > 500); 
          if (shouldBeOn != lastDXState) {
              lastDXState = shouldBeOn;
              eqActive = shouldBeOn;
              nbActive = shouldBeOn;
              highCut = shouldBeOn;
              lowCut = shouldBeOn;
              syncDSP();
              Serial.printf("[AutoDX] Signal quality change. DSP is now %s\n", shouldBeOn ? "ENABLED" : "BYPASSED");
          }
      }
  }
}

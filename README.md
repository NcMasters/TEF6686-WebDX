# TEF6686 DX Analyzer Pro (ESP32-C3 SuperMini)

A highly responsive, headless Web-GUI spectrum analyzer for the NXP TEF6686 radio DSP, designed specifically for the low-cost ESP32-C3 SuperMini.

This project transforms your TEF6686 module into a professional-grade DXing tool. There is no need for physical LCDs or rotary encoders—everything is controlled via a sleek, interactive web interface running right in your browser over USB Serial or WiFi.

## ✨ Features

*   **Headless Web GUI**: Control the radio via a browser. Features drag-to-pan, scroll-to-zoom, and click-to-tune functionality on an interactive spectrum analyzer.
*   **Dual Connection Modes**: Connect via USB Serial (noise-free, high speed via Web Serial API) or WiFi WebSockets (wireless).
*   **Blitz Scan**: Ultra-fast, high-resolution spectrum sweeping across FM, MW, and SW bands.
*   **Ultra-RDS DX Mode**: Maximizes RDS sensitivity for weak signals using forced mono, fixed 56kHz bandwidth, and statistical decoding to eliminate ghost characters.
*   **Live Audio Web Monitor**: Stream the radio audio directly through your browser using your PC's soundcard/line-in.
*   **Advanced DSP Control**: Granular control over Channel EQ, Multipath Mitigation (iMS), Audio Noise Blanker, and dynamic High/Low-cut filters.

## 🔌 Hardware & Wiring

This firmware is optimized specifically for the ESP32-C3 SuperMini.

| ESP32-C3 Pin | TEF6686 Pin | Note |
| :--- | :--- | :--- |
| **GPIO 4** | **SDA** | I2C Data (4.7kΩ pull-up to 3.3V recommended) |
| **GPIO 5** | **SCL** | I2C Clock (4.7kΩ pull-up to 3.3V recommended) |
| **3.3V** | **VCC** | Power |
| **GND** | **GND** | Ground |

## ⚠️ Required Files (Bring Your Own)

For copyright and legal compliance, this repository does not include the NXP proprietary DSP patch or any third-party station databases. You must provide these two files in your project folder before compiling or running the GUI.

### 1. `DSP_INIT.h` (The NXP Firmware Patch)
The TEF6686 requires a firmware initialization array to function correctly. This patch is the proprietary intellectual property of NXP Semiconductors.

**For copyright compliance, this repository does not include this file.**

To compile this project, you must legally acquire the `DSP_INIT.h` file yourself and place it directly in the same folder as this project's `.ino` file. The file must contain the initialization array formatted for Arduino (e.g., `const uint8_t DSP_INIT[] PROGMEM = {...}`). The software will not compile without it.

### 2. `stations_db.js` (The Offline Station Database)
The Web GUI supports an offline database for station identification, but it relies on a local JavaScript file that you must create.

In the same folder as your HTML/GUI files, create a new file named `stations_db.js`. Inside that file, define a constant named `defaultStationsData` containing your local stations in CSV format. The CSV format must include a header row containing the words `freq` (or `mhz`) and `name` (or `program`). You can optionally include a `genre` or `pty` column.

**Example `stations_db.js` format:**
```javascript
const defaultStationsData = `Freq, Name, Genre
87.60, DR P4, Pop
93.90, DR P3, News
100.00, Nova FM, Pop
106.20, Radio 100, Rock`;
```

## 🛠️ Installation & Setup

### Arduino IDE Configuration
1.  Install the ESP32 board manager in the Arduino IDE.
2.  Select **ESP32C3 Dev Module** (or XIAO_ESP32C3).
3.  **CRITICAL**: Set **USB CDC On Boot** to **Enabled**. (This is required for the native USB serial communication to work on the C3).
4.  Open the Library Manager and install the **WebSockets** library by Markus Sattler (Links2004).

### Uploading
1.  Ensure `DSP_INIT.h` is placed in the sketch folder.
2.  Flash the firmware to your ESP32-C3 SuperMini.

## 📻 Usage

1.  Plug the ESP32-C3 into your computer via USB.
2.  Open the `radio.html` file in any modern web browser (Chrome, Edge, or Chromium-based browsers are required for Web Serial support).
3.  Click the **CONNECT** button and select the serial port for your ESP32-C3.
4.  Click **BLITZ SWEEP** to populate the spectrum graph and start DXing!

*(Note: If you configure your WiFi credentials in the .ino file, you can also navigate to http://tef6686.local on your network to use the interface wirelessly).*

## ⚖️ Legal & Acknowledgments

*   **License**: MIT License
*   **Disclaimer**: This open-source project is provided as-is. The developer is not affiliated with NXP Semiconductors.
*   **Thanks**: 
    *   WebSocket communication powered by **WebSockets** by Markus Sattler.
    *   Station identification architecture is designed to be compatible with community-driven databases like radio-browser.info.

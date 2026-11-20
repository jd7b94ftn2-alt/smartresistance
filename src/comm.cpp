#include "comm.h"

#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <NimBLEDevice.h>
#include <PubSubClient.h>
#include <USB.h>
#include <USBCDC.h>
#include <USBHIDConsumerControl.h>
#include <USBHIDKeyboard.h>
#include <USBHIDMouse.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <string>

#include "ancs.h"
#include "app.h"
#include "ble_hid.h"
#include "board_pins.h"
#include "desk.h"
#include "pbank.h"
#include "share.h"
#include "web_index.h"

namespace comm {
namespace {

// ---------------------------------------------------------------- USB (device mode)
// The USB port is started by begin() only in PC mode (ARDUINO_USB_CDC_ON_BOOT=0), because in
// power-bank mode the same controller runs as a USB host. `Serial` is therefore UART0 (J_DBG).
USBCDC usbSerial(0);
HardwareSerial& bridgeUart = Serial;
bool usbDevice = false;
USBHIDConsumerControl consumerControl;
USBHIDMouse hidMouse;
USBHIDKeyboard hidKeyboard;

// ---------------------------------------------------------------- line readers
struct LineBuffer {
    char data[200];
    std::size_t length = 0;

    // Returns true when a full line is ready in `out`.
    bool feed(char c, String& out) {
        if (c == '\r') {
            return false;
        }
        if (c == '\n') {
            data[length] = '\0';
            out = data;
            length = 0;
            return true;
        }
        if (length + 1 < sizeof(data)) {
            data[length++] = c;
        }
        return false;
    }
};

LineBuffer usbLine;
LineBuffer tcpLine;

// ---------------------------------------------------------------- Wi-Fi
WebServer webServer(80);
WiFiServer commandServer(kCommandPort);
WiFiServer bridgeServer(kBridgePort);
WiFiClient commandClient;
WiFiClient bridgeClient;

String deviceName;
bool apActive = false;
bool staRequested = false;
bool servicesStarted = false;
bool ntpStarted = false;
std::uint32_t staStartMs = 0;

String weatherText;
bool weatherWanted = false;
std::uint32_t lastWeatherMs = 0;
bool weatherFetched = false;

constexpr const char* kApPassword = "12345678";
constexpr std::uint32_t kStaFallbackMs = 15000;
constexpr std::uint32_t kWeatherPeriodMs = 15UL * 60UL * 1000UL;

// The phone web page lives in docs/index.html (embedded as web_index.h by tools/embed_web.py).

int hidHostCount();

String buildStateJson() {
    const auto& st = app::state();
    char raw[20];
    snprintf(raw, sizeof(raw), "%010llx", static_cast<unsigned long long>(st.relayPlan.shiftWord));
    String json = "{\"r\":" + String(st.targetOhms);
    json += ",\"est\":" + String(st.estimatedOhms, 3);
    json += String(",\"out\":") + (st.outputEnabled ? "true" : "false");
    json += String(",\"fault\":") + (st.fault ? "true" : "false");
    json += String(",\"mode\":\"") + (app::rawMode() ? "RAW" : (app::ntcActive() ? "NTC" : "R")) + "\"";
    json += ",\"t\":" + String(app::ntcTemperature(), 2);
    json += ",\"ntc\":" + String(app::settings().ntcPreset);
    json += ",\"r25\":" + String(app::ntcR25(), 1);
    json += ",\"beta\":" + String(app::ntcBeta(), 1);
    json += ",\"board\":";
    json += app::tempValid() ? String(app::boardTempC(), 2) : String("null");
    json += String(",\"raw\":\"") + raw + "\"";
    json += String(",\"lock\":") + (st.locked ? "true" : "false");
    json += String(",\"v\":\"") + app::kFirmwareVersion + "\"";
    json += String(",\"name\":\"") + deviceName + "\"";
    json += String(",\"wifi\":\"") + (WiFi.status() == WL_CONNECTED ? "STA" : (apActive ? "AP" : "OFF")) + "\"";
    json += String(",\"ip\":\"") + ipAddress() + "\"";
    json += String(",\"usb\":\"") + (usbDevice ? "PC" : "PBANK") + "\"";
    json += String(",\"hid\":") + hidHostCount();
    json += ",\"types\":[";
    for (int i = 0; i < app::ntcTypeCount(); ++i) {
        json += String(i ? ",\"" : "\"") + app::ntcName(i) + "\"";
    }
    json += "]";
    if (pbank::active()) {
        const pbank::Data d = pbank::snapshot();
        json += String(",\"pb\":{\"link\":\"") + pbank::linkName(pbank::link()) + "\"";
        if (d.valid) {
            json += ",\"age\":" + String(d.ageMs / 1000.0f, 1) + ",\"soc\":" + String(d.soc) + ",\"soh\":" + String(d.soh);
            json += ",\"mv\":" + String(d.cells >= 2 ? d.cellMv[0] + d.cellMv[1] : d.cellMv[0]);
            json += ",\"ma\":" + String(pbank::batteryCurrentMa(d)) + ",\"mw\":" + String(d.powerMw);
            json += ",\"temp\":" + (d.tempValid ? String(d.tempC, 1) : String("null"));
            json += ",\"cyc\":" + (d.cyclesValid ? String(d.cycles) : String("null"));
        }
        json += "}";
    }
    json += "}";
    return json;
}

void startNetworkServices() {
    if (servicesStarted) {
        return;
    }
    webServer.on("/", []() {
        webServer.sendHeader("Content-Encoding", "gzip");
        webServer.sendHeader("Cache-Control", "no-cache");
        webServer.send_P(200, "text/html", reinterpret_cast<const char*>(kWebIndexGz), kWebIndexGzSize);
    });
    webServer.on("/api/state", []() { webServer.send(200, "application/json", buildStateJson()); });
    webServer.on("/api/cmd", []() {
        webServer.send(200, "text/plain", app::handleCommand(webServer.arg("c")));
    });
    // Shared picture / text: POST /api/upload?kind=IMG|TXT&size=<bytes>&name=<name>, multipart file field.
    static String uploadError;
    static int uploadId = -1;
    static bool uploadFailed = false;
    webServer.on("/api/upload", HTTP_POST, []() {
        if (uploadId >= 0) {
            webServer.send(200, "text/plain", String("OK ID=") + uploadId);
        } else {
            webServer.send(400, "text/plain", String("ERR ") + (uploadError.length() ? uploadError : String("no file")));
        }
    }, []() {
        HTTPUpload& up = webServer.upload();
        if (up.status == UPLOAD_FILE_START) {
            uploadId = -1;
            uploadError = "";
            const share::Kind kind = webServer.arg("kind").equalsIgnoreCase("TXT") ? share::Kind::Text : share::Kind::Image;
            uploadFailed = !share::beginUpload(kind, static_cast<std::uint32_t>(webServer.arg("size").toInt()),
                webServer.arg("name"), uploadError);
        } else if (up.status == UPLOAD_FILE_WRITE) {
            if (!uploadFailed && !share::writeUpload(up.buf, up.currentSize)) {
                uploadFailed = true;
                uploadError = "more data than size";
                share::abortUpload();
            }
        } else if (up.status == UPLOAD_FILE_END) {
            if (!uploadFailed) {
                uploadId = share::finishUpload(uploadError);
            }
        } else if (up.status == UPLOAD_FILE_ABORTED) {
            share::abortUpload();
            uploadError = "aborted";
        }
    });
    webServer.begin();
    commandServer.begin();
    commandServer.setNoDelay(true);
    bridgeServer.begin();
    bridgeServer.setNoDelay(true);
    MDNS.begin("smartr");
    MDNS.addService("http", "tcp", 80);
    servicesStarted = true;
}

void startAccessPoint() {
    if (apActive) {
        return;
    }
    WiFi.mode(staRequested ? WIFI_AP_STA : WIFI_AP);
    WiFi.softAP(deviceName.c_str(), kApPassword);
    apActive = true;
}

void fetchWeather() {
    HTTPClient http;
    http.setConnectTimeout(3000);
    http.setTimeout(3000);
    if (http.begin("http://wttr.in/?format=%l:+%t+%C")) {
        if (http.GET() == HTTP_CODE_OK) {
            String body = http.getString();
            body.trim();
            // The TFT fonts are ASCII only: drop the degree sign and any other non-ASCII byte.
            String ascii;
            for (std::size_t i = 0; i < body.length() && ascii.length() < 48; ++i) {
                const char c = body[i];
                if (c >= 32 && c < 127) {
                    ascii += c;
                }
            }
            if (ascii.length() > 0) {
                weatherText = ascii;
                weatherFetched = true;
            }
        }
        http.end();
    }
}

// ---------------------------------------------------------------- MQTT (internet remote control)
// The board connects out to an MQTT broker, so it needs no public IP or port forwarding.
//   <base>/cmd     subscribe: one command per line, same text protocol as USB / TCP
//   <base>/reply   publish:   reply to each command
//   <base>/state   publish:   status JSON (retained), on change and at least every 30 s
//   <base>/online  publish:   "1" (retained); the broker sets "0" when the link drops (last will)
// <base> = smartr/<12 hex digits of the MAC>, unique per board.
WiFiClient mqttPlain;
WiFiClientSecure mqttSecure;
PubSubClient mqtt;
String mqttBase;
String mqttClientId;
String mqttHostCopy;          // PubSubClient keeps the pointer, so the string must stay put
String mqttStatusText = "off";
bool mqttConfigured = false;
std::uint32_t mqttLastAttemptMs = 0;
std::uint32_t mqttRetryMs = 5000;
std::uint32_t mqttLastCheckMs = 0;
std::uint32_t mqttLastPublishMs = 0;
String mqttLastState;
constexpr int kMqttQueueSize = 6;
String mqttQueue[kMqttQueueSize];
int mqttQueued = 0;
constexpr std::uint32_t kMqttMaxRetryMs = 60000;
constexpr std::uint32_t kMqttStatePeriodMs = 30000;

// Runs inside mqtt.loop(): only queue, because a command such as WIFI or MQTT tears the link down.
void onMqttMessage(char* topic, std::uint8_t* payload, unsigned int length) {
    (void)topic;
    String text;
    text.reserve(length + 1);
    for (unsigned int i = 0; i < length; ++i) {
        text += static_cast<char>(payload[i]);
    }
    int start = 0;
    while (start < static_cast<int>(text.length())) {
        int end = text.indexOf('\n', start);
        if (end < 0) {
            end = text.length();
        }
        String line = text.substring(start, end);
        line.trim();
        if (line.length() > 0 && mqttQueued < kMqttQueueSize) {
            mqttQueue[mqttQueued++] = line;
        }
        start = end + 1;
    }
}

void applyMqttSettings() {
    const app::Settings& config = app::settings();
    if (mqtt.connected()) {
        mqtt.publish((mqttBase + "/online").c_str(), "0", true);
        mqtt.disconnect();
    }
    mqttConfigured = config.mqttEnabled && config.mqttHost.length() > 0;
    if (!mqttConfigured) {
        mqttStatusText = "off";
        return;
    }
    if (config.mqttPort == 1883) {
        mqtt.setClient(mqttPlain);
    } else {
        // TLS encrypts the link; the server certificate is not checked (no CA bundle on board).
        mqttSecure.setInsecure();
        mqttSecure.setHandshakeTimeout(8);
        mqtt.setClient(mqttSecure);
    }
    mqttHostCopy = config.mqttHost;
    mqtt.setServer(mqttHostCopy.c_str(), config.mqttPort);
    mqtt.setCallback(onMqttMessage);
    mqtt.setBufferSize(2048);
    mqtt.setKeepAlive(30);
    mqtt.setSocketTimeout(5);
    mqttRetryMs = 5000;
    mqttLastAttemptMs = millis() - mqttRetryMs;
    mqttStatusText = "connecting";
}

void serviceMqtt(bool wifiUp, std::uint32_t now) {
    if (!mqttConfigured) {
        return;
    }
    if (!wifiUp) {
        mqttStatusText = "no wifi";
        return;
    }
    if (!mqtt.connected()) {
        if (now - mqttLastAttemptMs < mqttRetryMs) {
            return;
        }
        mqttLastAttemptMs = now;
        const app::Settings& config = app::settings();
        const String will = mqttBase + "/online";
        const bool ok = config.mqttUser.length() > 0
            ? mqtt.connect(mqttClientId.c_str(), config.mqttUser.c_str(), config.mqttPass.c_str(),
                  will.c_str(), 1, true, "0")
            : mqtt.connect(mqttClientId.c_str(), will.c_str(), 1, true, "0");
        if (!ok) {
            mqttStatusText = "error " + String(mqtt.state());
            mqttRetryMs = std::min(kMqttMaxRetryMs, mqttRetryMs * 2);   // back off: connects block the UI
            return;
        }
        mqttRetryMs = 5000;
        mqtt.publish(will.c_str(), "1", true);
        mqtt.subscribe((mqttBase + "/cmd").c_str(), 1);
        mqttStatusText = "connected";
        mqttLastState = "";
    }
    mqtt.loop();

    while (mqttQueued > 0) {
        const String line = mqttQueue[0];
        for (int i = 1; i < mqttQueued; ++i) {
            mqttQueue[i - 1] = mqttQueue[i];
        }
        --mqttQueued;
        const String reply = app::handleCommand(line);
        if (mqtt.connected() && reply.length() > 0) {
            mqtt.publish((mqttBase + "/reply").c_str(), reply.c_str(), false);
        }
    }

    if (mqtt.connected() && now - mqttLastCheckMs >= 1000) {
        mqttLastCheckMs = now;
        const String state = buildStateJson();
        if (state != mqttLastState || now - mqttLastPublishMs >= kMqttStatePeriodMs) {
            if (mqtt.publish((mqttBase + "/state").c_str(), state.c_str(), true)) {
                mqttLastState = state;
                mqttLastPublishMs = now;
            }
        }
    }
}

// ---------------------------------------------------------------- BLE (Nordic UART service)
constexpr const char* kNusService = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr const char* kNusRx = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr const char* kNusTx = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

NimBLEServer* bleServer = nullptr;
NimBLECharacteristic* bleTx = nullptr;
volatile int bleLinks = 0;            // connected centrals (PC host app, phone, ...), up to 3
volatile bool bleLinked = false;

// ---------------------------------------------------------------- BLE HID (keyboard + media keys)
// The board is also a Bluetooth keyboard (ble_hid.cpp): phones and PCs pair with it once and then
// receive key / media-key reports.
int hidHostCount() {
    return blehid::hosts();
}

void bleKey(std::uint8_t modifiers, std::uint8_t usage) {
    blehid::key(modifiers, usage);
}

void bleConsumer(std::uint16_t usage) {
    blehid::consumer(usage);
}

// HID usage ids (keyboard page 0x07, consumer page 0x0C) used by the BLE paths.
constexpr std::uint8_t kModCtrl = 0x01;
constexpr std::uint8_t kModShift = 0x02;
constexpr std::uint8_t kModAlt = 0x04;
constexpr std::uint8_t kModGui = 0x08;
constexpr std::uint8_t kKeyC = 0x06, kKeyD = 0x07, kKeyL = 0x0F, kKeyS = 0x16, kKeyV = 0x19, kKeyZ = 0x1D;
constexpr std::uint8_t kKeyEnter = 0x28, kKeyEsc = 0x29, kKeyTab = 0x2B;
constexpr std::uint8_t kKeyDown = 0x51, kKeyUp = 0x52;
constexpr std::uint16_t kUsePlayPause = 0xCD, kUseNext = 0xB5, kUsePrev = 0xB6, kUseMute = 0xE2;
constexpr std::uint16_t kUseVolUp = 0xE9, kUseVolDown = 0xEA, kUseHome = 0x0223;

// USB HID when the board is plugged into a PC that enumerated it, otherwise the BLE keyboard.
bool usbHidReady();
volatile bool bleOn = true;
volatile bool bleRawMode = false;     // true: BLE <-> J_DBG UART bridge instead of commands
volatile std::uint16_t blePayload = 20;
volatile std::uint16_t bleConnHandle = 0xFFFF;
char blePeerAddress[20] = "";
QueueHandle_t bleQueue = nullptr;

struct BleLine {
    std::uint16_t length;
    bool raw;
    char text[182];
};

class BleServerCallbacks final : public NimBLEServerCallbacks {
    // Several centrals at once (PC host app + phone keyboard link): keep advertising until full.
    void onConnect(NimBLEServer* server, ble_gap_conn_desc* desc) override {
        (void)server;
        bleLinks = bleLinks + 1;
        bleLinked = true;
        if (bleLinks == 1) {
            blePayload = 20;          // until the MTU exchange; later links keep the current size
        }
        bleConnHandle = desc->conn_handle;
        const std::string address = NimBLEAddress(desc->peer_ota_addr).toString();
        snprintf(blePeerAddress, sizeof(blePeerAddress), "%s", address.c_str());
        if (bleOn && bleLinks < CONFIG_BT_NIMBLE_MAX_CONNECTIONS) {
            NimBLEDevice::startAdvertising();
        }
    }

    void onDisconnect(NimBLEServer* server, ble_gap_conn_desc* desc) override {
        (void)server;
        blehid::connectionClosed(desc->conn_handle);
        bleLinks = bleLinks > 0 ? bleLinks - 1 : 0;
        bleLinked = bleLinks > 0;
        if (desc->conn_handle == bleConnHandle) {
            bleConnHandle = 0xFFFF;
            blePeerAddress[0] = '\0';
        }
        if (bleOn) {
            NimBLEDevice::startAdvertising();
        }
    }

    void onMTUChange(std::uint16_t mtu, ble_gap_conn_desc* desc) override {
        (void)desc;
        const std::uint16_t payload = mtu > 23 ? mtu - 3 : 20;
        const std::uint16_t current = blePayload;     // volatile: copy before std::min
        blePayload = bleLinks <= 1 ? payload : std::min(current, payload);
    }
};

class BleRxCallbacks final : public NimBLECharacteristicCallbacks {
    // Runs on the NimBLE host task: only queue the text, the loop task executes it.
    void onWrite(NimBLECharacteristic* characteristic) override {
        const std::string value = characteristic->getValue();
        if (bleRawMode) {
            // Bridge mode: forward the bytes unchanged, the loop task writes them to the UART.
            for (std::size_t offset = 0; offset < value.size(); offset += sizeof(BleLine::text)) {
                BleLine chunk{};
                chunk.raw = true;
                chunk.length = static_cast<std::uint16_t>(std::min(value.size() - offset, sizeof(chunk.text)));
                memcpy(chunk.text, value.data() + offset, chunk.length);
                if (bleQueue != nullptr) {
                    xQueueSend(bleQueue, &chunk, 0);
                }
            }
            return;
        }
        std::size_t start = 0;
        while (start < value.size()) {
            std::size_t end = value.find('\n', start);
            if (end == std::string::npos) {
                end = value.size();
            }
            BleLine line{};
            const std::size_t length = std::min(end - start, sizeof(line.text) - 1);
            memcpy(line.text, value.data() + start, length);
            line.text[length] = '\0';
            line.length = static_cast<std::uint16_t>(length);
            if (length > 0 && bleQueue != nullptr) {
                xQueueSend(bleQueue, &line, 0);
            }
            start = end + 1;
        }
    }
};

BleServerCallbacks bleServerCallbacks;
BleRxCallbacks bleRxCallbacks;

void bleSend(const String& text) {
    if (!bleLinked || bleTx == nullptr) {
        return;
    }
    const String framed = text + "\n";
    const std::size_t chunk = blePayload;
    for (std::size_t offset = 0; offset < framed.length(); offset += chunk) {
        const std::size_t length = std::min(chunk, static_cast<std::size_t>(framed.length()) - offset);
        bleTx->setValue(reinterpret_cast<const std::uint8_t*>(framed.c_str()) + offset, length);
        bleTx->notify();
        delay(8);
    }
}

void bleSendRaw(const std::uint8_t* data, std::size_t size) {
    if (!bleLinked || bleTx == nullptr) {
        return;
    }
    const std::size_t chunk = blePayload;
    for (std::size_t offset = 0; offset < size; offset += chunk) {
        const std::size_t length = std::min(chunk, size - offset);
        bleTx->setValue(data + offset, length);
        bleTx->notify();
        delay(4);
    }
}

void startBle() {
    bleQueue = xQueueCreate(32, sizeof(BleLine));   // 32 lines: room for a burst of FILEDAT chunks
    NimBLEDevice::init(deviceName.c_str());
    NimBLEDevice::setMTU(247);
    // Keyboards must bond: "Just Works" pairing (no PIN), LE secure connections. The Nordic UART
    // characteristics stay unencrypted, so the PC host app still connects without pairing.
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    NimBLEServer* server = NimBLEDevice::createServer();
    bleServer = server;
    server->setCallbacks(&bleServerCallbacks);
    NimBLEService* service = server->createService(kNusService);
    bleTx = service->createCharacteristic(kNusTx, NIMBLE_PROPERTY::NOTIFY);
    NimBLECharacteristic* rx = service->createCharacteristic(
        kNusRx, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    rx->setCallbacks(&bleRxCallbacks);
    service->start();

    blehid::begin(server);
    ancs::begin();                                 // iPhone notifications on the encrypted link

    // Advertising: flags + keyboard appearance + HID service + name (24 of 31 bytes);
    // scan response: the Nordic UART service the host app and the web page look for.
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    NimBLEAdvertisementData adv;
    adv.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    adv.setAppearance(0x03C1);                     // keyboard
    adv.setCompleteServices16({NimBLEUUID(static_cast<std::uint16_t>(0x1812))});
    adv.setName(deviceName.c_str());
    NimBLEAdvertisementData scan;
    scan.setCompleteServices(NimBLEUUID(kNusService));
    advertising->setAdvertisementData(adv);
    advertising->setScanResponseData(scan);
    if (bleOn) {
        advertising->start();
    }
}

// ---------------------------------------------------------------- UART bridge
char bridgeLines[kBridgeLineCount][kBridgeLineLength + 1];
int bridgeColumn = 0;
std::uint32_t bridgeRev = 0;
bool bridgeHold = false;

void bridgeNewLine() {
    memmove(bridgeLines[0], bridgeLines[1], sizeof(bridgeLines[0]) * (kBridgeLineCount - 1));
    bridgeLines[kBridgeLineCount - 1][0] = '\0';
    bridgeColumn = 0;
}

void bridgePutChar(char c) {
    if (c == '\r') {
        return;
    }
    if (c == '\n') {
        bridgeNewLine();
        return;
    }
    if (c == '\t') {
        c = ' ';
    }
    if (c < 32 || c > 126) {
        c = '.';
    }
    if (bridgeColumn >= kBridgeLineLength) {
        bridgeNewLine();
    }
    char* row = bridgeLines[kBridgeLineCount - 1];
    row[bridgeColumn++] = c;
    row[bridgeColumn] = '\0';
}

void serviceBridge() {
    if (bridgeServer.hasClient()) {
        if (bridgeClient) {
            bridgeClient.stop();
        }
        bridgeClient = bridgeServer.available();
    }

    std::uint8_t buffer[128];
    int available = bridgeUart.available();
    while (available > 0) {
        const int length = bridgeUart.read(buffer, std::min(available, static_cast<int>(sizeof(buffer))));
        if (length <= 0) {
            break;
        }
        if (bridgeClient && bridgeClient.connected()) {
            bridgeClient.write(buffer, length);
        }
        if (bleRawMode) {
            bleSendRaw(buffer, static_cast<std::size_t>(length));
        }
        if (!bridgeHold) {
            for (int i = 0; i < length; ++i) {
                bridgePutChar(static_cast<char>(buffer[i]));
            }
            ++bridgeRev;
        }
        available = bridgeUart.available();
    }

    if (bridgeClient && bridgeClient.connected()) {
        int pending = bridgeClient.available();
        while (pending > 0) {
            const int length = bridgeClient.read(buffer, std::min(pending, static_cast<int>(sizeof(buffer))));
            if (length <= 0) {
                break;
            }
            bridgeUart.write(buffer, length);
            pending = bridgeClient.available();
        }
    }
}

void serviceCommandClient() {
    if (commandServer.hasClient()) {
        if (commandClient) {
            commandClient.stop();
        }
        commandClient = commandServer.available();
        tcpLine.length = 0;
    }
    if (!commandClient || !commandClient.connected()) {
        return;
    }
    String line;
    int budget = 256;
    while (commandClient.available() > 0 && budget-- > 0) {
        if (tcpLine.feed(static_cast<char>(commandClient.read()), line)) {
            const String reply = app::handleCommand(line);
            if (reply.length() > 0) {
                commandClient.println(reply);
            }
        }
    }
}

}  // namespace

void begin() {
    const std::uint64_t mac = ESP.getEfuseMac();
    char suffix[8];
    snprintf(suffix, sizeof(suffix), "%02X%02X",
        static_cast<unsigned>((mac >> 32) & 0xFF), static_cast<unsigned>((mac >> 40) & 0xFF));
    deviceName = String("SmartR-") + suffix;
    char macHex[16];
    snprintf(macHex, sizeof(macHex), "%012llx", static_cast<unsigned long long>(mac & 0xFFFFFFFFFFFFULL));
    mqttBase = String("smartr/") + macHex;
    mqttClientId = String("smartr-") + macHex;

    if (app::settings().usbHost) {
        pbank::begin();              // USB host for the power bank; commands over BLE / Wi-Fi
    } else {
        USB.productName("SmartResistance");
        USB.manufacturerName("SmartR");
        consumerControl.begin();
        hidMouse.begin();
        hidKeyboard.begin();
        usbSerial.setRxBufferSize(4096);   // file uploads (FILEDAT) stream faster than one loop pass
        usbSerial.begin(115200);
        usbSerial.setTimeout(5);
        USB.begin();
        usbDevice = true;
    }

    bridgeUart.setRxBufferSize(2048);   // must precede begin()
    bridgeUart.begin(app::settings().bridgeBaud, SERIAL_8N1, board::kUartRx, board::kUartTx);

    bleOn = app::settings().bleEnabled;
    bleRawMode = app::settings().bleBridge;
    startBle();
    restartWifi();
    applyMqttSettings();
}

void loop() {
    // USB serial commands.
    String line;
    int budget = 256;
    while (usbDevice && usbSerial.available() > 0 && budget-- > 0) {
        if (usbLine.feed(static_cast<char>(usbSerial.read()), line)) {
            const String reply = app::handleCommand(line);
            if (reply.length() > 0) {
                usbSerial.println(reply);
            }
        }
    }

    ancs::loop();

    // BLE commands queued by the NimBLE task.
    BleLine bleLine;
    while (bleQueue != nullptr && xQueueReceive(bleQueue, &bleLine, 0) == pdTRUE) {
        if (bleLine.raw) {
            bridgeUart.write(reinterpret_cast<const std::uint8_t*>(bleLine.text), bleLine.length);
            continue;
        }
        const String reply = app::handleCommand(bleLine.text);
        if (reply.length() > 0) {
            bleSend(reply);
        }
    }

    const std::uint32_t now = millis();
    const bool connected = WiFi.status() == WL_CONNECTED;
    if (staRequested && !connected && !apActive && now - staStartMs > kStaFallbackMs) {
        startAccessPoint();   // router unreachable: stay reachable through the device's own AP
    }
    if ((connected || apActive) && !servicesStarted) {
        startNetworkServices();
    }
    if (connected && !ntpStarted) {
        configTime(desk::utcOffsetMinutes() * 60L, 0, "ntp.aliyun.com", "pool.ntp.org");
        ntpStarted = true;
    }

    if (servicesStarted) {
        webServer.handleClient();
        serviceCommandClient();
    }
    serviceMqtt(connected, now);
    serviceBridge();

    // While the clock page is shown: every 15 min, a failed try again after 1 min (each try blocks up to a few s).
    if (connected && weatherWanted
        && (lastWeatherMs == 0 || now - lastWeatherMs > (weatherFetched ? kWeatherPeriodMs : 60000UL))) {
        lastWeatherMs = now == 0 ? 1 : now;
        weatherWanted = false;
        if (desk::location().valid) {
            weatherFetched = desk::fetchWeather();   // open-meteo for the saved location (WXLOC)
        } else {
            fetchWeather();                          // no location: wttr.in guesses it from the IP
        }
    }
}

void restartWifi() {
    const app::Settings& config = app::settings();
    WiFi.persistent(false);
    WiFi.disconnect(true);
    WiFi.softAPdisconnect(true);
    apActive = false;
    ntpStarted = false;
    WiFi.setHostname("smartr");

    staRequested = config.wifiSsid.length() > 0;
    if (staRequested) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(config.wifiSsid.c_str(), config.wifiPass.c_str());
        staStartMs = millis();
    } else {
        startAccessPoint();
    }
}

bool wifiConnected() {
    return WiFi.status() == WL_CONNECTED;
}

bool accessPointActive() {
    return apActive;
}

String ipAddress() {
    if (wifiConnected()) {
        return WiFi.localIP().toString();
    }
    return apActive ? WiFi.softAPIP().toString() : String("-");
}

String accessPointName() {
    return deviceName;
}

bool timeValid() {
    return time(nullptr) > 1700000000;
}

String weatherLine() {
    return weatherText;
}

void requestWeather() {
    weatherWanted = true;
}

void requestWeatherNow() {
    weatherFetched = false;
    lastWeatherMs = 0;
}

bool bleConnected() {
    return bleLinked;
}

void setBleEnabled(bool enabled) {
    bleOn = enabled;
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    if (enabled) {
        if (!bleLinked) {
            advertising->start();
        }
        return;
    }
    advertising->stop();
    if (bleServer != nullptr) {
        for (const std::uint16_t handle : bleServer->getPeerDevices()) {
            bleServer->disconnect(handle);
        }
    }
}

bool bleEnabled() {
    return bleOn;
}

void setBleBridge(bool bridge) {
    bleRawMode = bridge;
}

bool bleBridge() {
    return bleRawMode;
}

void restartMqtt() {
    applyMqttSettings();
}

bool mqttConnected() {
    return mqtt.connected();
}

String mqttStatus() {
    return mqttStatusText;
}

String mqttTopicBase() {
    return mqttBase;
}

String blePeer() {
    return String(blePeerAddress);
}

String bleName() {
    return deviceName;
}

void setBridgeBaud(std::uint32_t baud) {
    bridgeUart.updateBaudRate(baud);
}

String stateJson() {
    return buildStateJson();
}

bool usbDeviceActive() {
    return usbDevice;
}

namespace {
bool usbHidReady() {
    return usbDevice && static_cast<bool>(USB);   // started and enumerated by a PC
}
}  // namespace

bool usbHidConnected() {
    return usbHidReady();
}

std::uint32_t bridgeRevision() {
    return bridgeRev;
}

const char* bridgeLine(int row) {
    if (row < 0 || row >= kBridgeLineCount) {
        return "";
    }
    return bridgeLines[row];
}

void setBridgePaused(bool paused) {
    bridgeHold = paused;
}

bool bridgePaused() {
    return bridgeHold;
}

void clearBridgeLog() {
    memset(bridgeLines, 0, sizeof(bridgeLines));
    bridgeColumn = 0;
    ++bridgeRev;
}

const char* knobModeName(KnobMode mode) {
    switch (mode) {
        case KnobMode::Volume: return "Volume";
        case KnobMode::Scroll: return "Scroll";
        case KnobMode::Media: return "Media";
        case KnobMode::Arrows: return "Arrows";
        default: return "?";
    }
}

void knobRotate(KnobMode mode, int steps) {
    const int count = steps < 0 ? -steps : steps;
    if (!usbHidReady()) {
        // Bluetooth keyboard: no mouse report, so "scroll" uses the arrow keys.
        for (int i = 0; i < count; ++i) {
            switch (mode) {
                case KnobMode::Volume: bleConsumer(steps > 0 ? kUseVolUp : kUseVolDown); break;
                case KnobMode::Media: bleConsumer(steps > 0 ? kUseNext : kUsePrev); break;
                default: bleKey(0, steps > 0 ? kKeyDown : kKeyUp); break;
            }
        }
        return;
    }
    for (int i = 0; i < count; ++i) {
        switch (mode) {
            case KnobMode::Volume:
                consumerControl.press(steps > 0
                    ? CONSUMER_CONTROL_VOLUME_INCREMENT : CONSUMER_CONTROL_VOLUME_DECREMENT);
                consumerControl.release();
                break;
            case KnobMode::Scroll:
                hidMouse.move(0, 0, steps > 0 ? 1 : -1);
                break;
            case KnobMode::Media:
                consumerControl.press(steps > 0
                    ? CONSUMER_CONTROL_SCAN_NEXT : CONSUMER_CONTROL_SCAN_PREVIOUS);
                consumerControl.release();
                break;
            case KnobMode::Arrows:
                hidKeyboard.write(steps > 0 ? KEY_DOWN_ARROW : KEY_UP_ARROW);
                break;
            default:
                break;
        }
    }
}

void knobClick(KnobMode mode) {
    if (!usbHidReady()) {
        switch (mode) {
            case KnobMode::Volume: bleConsumer(kUseMute); break;
            case KnobMode::Media: bleConsumer(kUsePlayPause); break;
            default: bleKey(0, kKeyEnter); break;
        }
        return;
    }
    switch (mode) {
        case KnobMode::Volume:
            consumerControl.press(CONSUMER_CONTROL_MUTE);
            consumerControl.release();
            break;
        case KnobMode::Scroll:
            hidMouse.click(MOUSE_MIDDLE);
            break;
        case KnobMode::Media:
            consumerControl.press(CONSUMER_CONTROL_PLAY_PAUSE);
            consumerControl.release();
            break;
        case KnobMode::Arrows:
            hidKeyboard.write(KEY_RETURN);
            break;
        default:
            break;
    }
}

namespace {

void chord(std::uint8_t m1, std::uint8_t m2, std::uint8_t key) {
    if (m1 != 0) {
        hidKeyboard.press(m1);
    }
    if (m2 != 0) {
        hidKeyboard.press(m2);
    }
    hidKeyboard.press(key);
    delay(15);
    hidKeyboard.releaseAll();
}

void consumerKey(std::uint16_t usage) {
    consumerControl.press(usage);
    consumerControl.release();
}

// Same shortcuts through the Bluetooth keyboard (HID usage ids instead of Arduino key codes).
void sendActionBle(Action action) {
    switch (action) {
        case Action::Screenshot: bleKey(kModGui | kModShift, kKeyS); break;
        case Action::Lock: bleKey(kModGui, kKeyL); break;
        case Action::ShowDesktop: bleKey(kModGui, kKeyD); break;
        case Action::TaskManager: bleKey(kModCtrl | kModShift, kKeyEsc); break;
        case Action::Copy: bleKey(kModCtrl, kKeyC); break;
        case Action::Paste: bleKey(kModCtrl, kKeyV); break;
        case Action::Undo: bleKey(kModCtrl, kKeyZ); break;
        case Action::SwitchWindow: bleKey(kModAlt, kKeyTab); break;
        case Action::Mute: bleConsumer(kUseMute); break;
        case Action::PlayPause: bleConsumer(kUsePlayPause); break;
        case Action::PrevTrack: bleConsumer(kUsePrev); break;
        case Action::NextTrack: bleConsumer(kUseNext); break;
        case Action::FeishuMic: bleKey(kModCtrl | kModShift, kKeyD); break;
        case Action::FeishuCamera: bleKey(kModCtrl | kModShift, kKeyV); break;
        case Action::VolumeUp: bleConsumer(kUseVolUp); break;
        case Action::VolumeDown: bleConsumer(kUseVolDown); break;
        default: break;
    }
}

}  // namespace

bool bleKeyboardActive() {
    return hidHostCount() > 0;
}

int bleKeyboardHosts() {
    return hidHostCount();
}

int bleBondCount() {
    return NimBLEDevice::getNumBonds();
}

void clearBlePairings() {
    if (bleServer != nullptr) {
        for (const std::uint16_t handle : bleServer->getPeerDevices()) {
            bleServer->disconnect(handle);
        }
    }
    NimBLEDevice::deleteAllBonds();
}

const char* remoteName(Remote remote) {
    switch (remote) {
        case Remote::VolumeUp: return "VOLUP";
        case Remote::VolumeDown: return "VOLDOWN";
        case Remote::PlayPause: return "PLAY";
        case Remote::Next: return "NEXT";
        case Remote::Prev: return "PREV";
        case Remote::Shutter: return "SHUTTER";
        case Remote::Mute: return "MUTE";
        case Remote::PageUp: return "UP";
        case Remote::PageDown: return "DOWN";
        case Remote::Home: return "HOME";
        default: return "?";
    }
}

// Phone remote: always the Bluetooth keyboard. iOS camera: volume up = shutter.
void sendRemote(Remote remote) {
    switch (remote) {
        case Remote::VolumeUp: bleConsumer(kUseVolUp); break;
        case Remote::VolumeDown: bleConsumer(kUseVolDown); break;
        case Remote::PlayPause: bleConsumer(kUsePlayPause); break;
        case Remote::Next: bleConsumer(kUseNext); break;
        case Remote::Prev: bleConsumer(kUsePrev); break;
        case Remote::Shutter: bleConsumer(kUseVolUp); break;
        case Remote::Mute: bleConsumer(kUseMute); break;
        case Remote::PageUp: bleKey(0, kKeyUp); break;
        case Remote::PageDown: bleKey(0, kKeyDown); break;
        case Remote::Home: bleConsumer(kUseHome); break;
        default: break;
    }
}

void sendAction(Action action) {
    if (!usbHidReady()) {
        sendActionBle(action);    // not on a PC's USB: the Bluetooth keyboard (if a host is paired)
        return;
    }
    switch (action) {
        case Action::Screenshot: chord(KEY_LEFT_GUI, KEY_LEFT_SHIFT, 's'); break;
        case Action::Lock: chord(KEY_LEFT_GUI, 0, 'l'); break;
        case Action::ShowDesktop: chord(KEY_LEFT_GUI, 0, 'd'); break;
        case Action::TaskManager: chord(KEY_LEFT_CTRL, KEY_LEFT_SHIFT, KEY_ESC); break;
        case Action::Copy: chord(KEY_LEFT_CTRL, 0, 'c'); break;
        case Action::Paste: chord(KEY_LEFT_CTRL, 0, 'v'); break;
        case Action::Undo: chord(KEY_LEFT_CTRL, 0, 'z'); break;
        case Action::SwitchWindow: chord(KEY_LEFT_ALT, 0, KEY_TAB); break;
        case Action::Mute: consumerKey(CONSUMER_CONTROL_MUTE); break;
        case Action::PlayPause: consumerKey(CONSUMER_CONTROL_PLAY_PAUSE); break;
        case Action::PrevTrack: consumerKey(CONSUMER_CONTROL_SCAN_PREVIOUS); break;
        case Action::NextTrack: consumerKey(CONSUMER_CONTROL_SCAN_NEXT); break;
        case Action::FeishuMic: chord(KEY_LEFT_CTRL, KEY_LEFT_SHIFT, 'd'); break;     // Feishu meeting: mic on/off
        case Action::FeishuCamera: chord(KEY_LEFT_CTRL, KEY_LEFT_SHIFT, 'v'); break;  // Feishu meeting: camera
        case Action::VolumeUp: consumerKey(CONSUMER_CONTROL_VOLUME_INCREMENT); break;
        case Action::VolumeDown: consumerKey(CONSUMER_CONTROL_VOLUME_DECREMENT); break;
        default: break;
    }
}

}  // namespace comm

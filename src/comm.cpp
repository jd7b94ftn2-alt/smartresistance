#include "comm.h"

#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <NimBLEDevice.h>
#include <PubSubClient.h>
#include <USB.h>
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

#include "app.h"
#include "board_pins.h"

namespace comm {
namespace {

// ---------------------------------------------------------------- USB HID
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

const char kIndexHtml[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>SmartResistance</title>
<style>
body{font-family:system-ui,sans-serif;margin:0;padding:12px;background:#12161c;color:#e8edf2}
h1{font-size:18px;margin:0 0 10px}.card{background:#1d242e;border-radius:8px;padding:12px;margin-bottom:10px}
.big{font-size:34px;font-weight:600}.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin-top:8px}
input,select,button{font-size:16px;padding:8px;border-radius:6px;border:1px solid #3a4656;background:#0f1319;color:#e8edf2}
button{background:#2b6cb0;border:0;cursor:pointer}button.on{background:#2f855a}button.off{background:#9b2c2c}
.grid{display:grid;grid-template-columns:repeat(8,1fr);gap:4px;margin-top:8px}
.grid button{padding:6px 0;font-size:12px;background:#2d3748}.grid button.act{background:#d69e2e;color:#111}
pre{background:#0f1319;padding:8px;border-radius:6px;white-space:pre-wrap;min-height:24px}small{color:#93a1b3}
</style></head><body><h1>SmartResistance</h1>
<div class="card"><div class="big" id="r">-</div><small id="s">-</small>
<div class="row"><input id="rv" placeholder="ohms, e.g. 4.7k" size="12"><button onclick="cmd('R '+v('rv'))">Set R</button>
<button id="ob" onclick="cmd('OUT '+(st.out?0:1))">OUT</button></div></div>
<div class="card"><b>NTC</b><div class="row"><select id="nt" onchange="cmd('NTCSEL '+this.value)"></select>
<input id="tv" placeholder="temp C" size="7"><button onclick="cmd('NTC '+v('tv'))">Set temp</button></div>
<div class="row"><input id="s0" placeholder="from" size="5"><input id="s1" placeholder="to" size="5">
<input id="s2" placeholder="step" size="5"><input id="s3" placeholder="dwell ms" size="8">
<button onclick="cmd('SWEEP '+v('s0')+' '+v('s1')+' '+v('s2')+' '+v('s3'))">Sweep</button>
<button class="off" onclick="cmd('SWEEP STOP')">Stop</button></div></div>
<div class="card"><b>Relays</b> <small>manual test, 40 = K_OUT</small><div class="grid" id="g"></div>
<div class="row"><button onclick="cmd('NORMAL')">Back to resistor mode</button></div></div>
<div class="card"><b>Command</b><div class="row"><input id="c" size="24" placeholder="HELP">
<button onclick="cmd(v('c'))">Send</button></div><pre id="o"></pre></div>
<script>
let st={};const v=i=>document.getElementById(i).value,e=i=>document.getElementById(i);
async function cmd(c){const r=await fetch('/api/cmd?c='+encodeURIComponent(c));e('o').textContent=await r.text();poll()}
const g=e('g');for(let i=0;i<41;i++){const b=document.createElement('button');b.textContent=i;b.id='k'+i;
b.onclick=()=>cmd('RELAY '+i+' '+(b.classList.contains('act')?0:1));g.appendChild(b)}
async function poll(){try{st=await (await fetch('/api/state')).json();e('r').textContent=st.r+' Ω';
e('s').textContent='mode '+st.mode+' | est '+st.est+' Ω'+(st.mode=='NTC'?' | '+st.t+' °C':'')+
' | board '+(st.board===null?'-':st.board+' °C');
const ob=e('ob');ob.textContent=st.out?'OUT ON':'OUT OFF';ob.className=st.out?'on':'off';
const nt=e('nt');if(!nt.options.length)st.types.forEach((n,i)=>nt.add(new Option(n,i)));nt.value=st.ntc;
const w=BigInt('0x'+st.raw);for(let i=0;i<40;i++)e('k'+i).classList.toggle('act',((w>>BigInt(i))&1n)==1n);
e('k40').classList.toggle('act',st.out)}catch(x){}}
setInterval(poll,1000);poll();
</script></body></html>)HTML";

String stateJson() {
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
    json += ",\"types\":[";
    for (int i = 0; i < app::ntcTypeCount(); ++i) {
        json += String(i ? ",\"" : "\"") + app::ntcName(i) + "\"";
    }
    json += "]}";
    return json;
}

void startNetworkServices() {
    if (servicesStarted) {
        return;
    }
    webServer.on("/", []() { webServer.send_P(200, "text/html", kIndexHtml); });
    webServer.on("/api/state", []() { webServer.send(200, "application/json", stateJson()); });
    webServer.on("/api/cmd", []() {
        webServer.send(200, "text/plain", app::handleCommand(webServer.arg("c")));
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
        const String state = stateJson();
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
volatile bool bleLinked = false;
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
    void onConnect(NimBLEServer* server) override {
        (void)server;
        bleLinked = true;
        blePayload = 20;
    }

    void onConnect(NimBLEServer* server, ble_gap_conn_desc* desc) override {
        (void)server;
        bleConnHandle = desc->conn_handle;
        const std::string address = NimBLEAddress(desc->peer_ota_addr).toString();
        snprintf(blePeerAddress, sizeof(blePeerAddress), "%s", address.c_str());
    }

    void onDisconnect(NimBLEServer* server) override {
        (void)server;
        bleLinked = false;
        bleConnHandle = 0xFFFF;
        blePeerAddress[0] = '\0';
        if (bleOn) {
            NimBLEDevice::startAdvertising();
        }
    }

    void onMTUChange(std::uint16_t mtu, ble_gap_conn_desc* desc) override {
        (void)desc;
        blePayload = mtu > 23 ? mtu - 3 : 20;
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
    bleQueue = xQueueCreate(12, sizeof(BleLine));
    NimBLEDevice::init(deviceName.c_str());
    NimBLEDevice::setMTU(247);
    NimBLEServer* server = NimBLEDevice::createServer();
    bleServer = server;
    server->setCallbacks(&bleServerCallbacks);
    NimBLEService* service = server->createService(kNusService);
    bleTx = service->createCharacteristic(kNusTx, NIMBLE_PROPERTY::NOTIFY);
    NimBLECharacteristic* rx = service->createCharacteristic(
        kNusRx, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    rx->setCallbacks(&bleRxCallbacks);
    service->start();
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    advertising->addServiceUUID(kNusService);
    advertising->setScanResponse(true);
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
    int available = Serial0.available();
    while (available > 0) {
        const int length = Serial0.read(buffer, std::min(available, static_cast<int>(sizeof(buffer))));
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
        available = Serial0.available();
    }

    if (bridgeClient && bridgeClient.connected()) {
        int pending = bridgeClient.available();
        while (pending > 0) {
            const int length = bridgeClient.read(buffer, std::min(pending, static_cast<int>(sizeof(buffer))));
            if (length <= 0) {
                break;
            }
            Serial0.write(buffer, length);
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

    USB.productName("SmartResistance");
    USB.manufacturerName("SmartR");
    consumerControl.begin();
    hidMouse.begin();
    hidKeyboard.begin();
    USB.begin();
    Serial.begin(115200);
    Serial.setTimeout(5);

    Serial0.setRxBufferSize(2048);   // must precede begin()
    Serial0.begin(app::settings().bridgeBaud, SERIAL_8N1, board::kUartRx, board::kUartTx);

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
    while (Serial.available() > 0 && budget-- > 0) {
        if (usbLine.feed(static_cast<char>(Serial.read()), line)) {
            const String reply = app::handleCommand(line);
            if (reply.length() > 0) {
                Serial.println(reply);
            }
        }
    }

    // BLE commands queued by the NimBLE task.
    BleLine bleLine;
    while (bleQueue != nullptr && xQueueReceive(bleQueue, &bleLine, 0) == pdTRUE) {
        if (bleLine.raw) {
            Serial0.write(reinterpret_cast<const std::uint8_t*>(bleLine.text), bleLine.length);
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
        configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");
        ntpStarted = true;
    }

    if (servicesStarted) {
        webServer.handleClient();
        serviceCommandClient();
    }
    serviceMqtt(connected, now);
    serviceBridge();

    if (connected && weatherWanted
        && (!weatherFetched || now - lastWeatherMs > kWeatherPeriodMs)) {
        lastWeatherMs = now;
        weatherWanted = false;
        fetchWeather();
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
    if (bleLinked && bleServer != nullptr && bleConnHandle != 0xFFFF) {
        bleServer->disconnect(bleConnHandle);
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
    Serial0.updateBaudRate(baud);
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

}  // namespace comm

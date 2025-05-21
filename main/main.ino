#include <U8g2lib.h>
#include <Wire.h>
#include <driver/i2s.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>
#include <ArduinoWebsockets.h>  //https://github.com/gilmaimon/ArduinoWebsockets
#include <NTPClient.h>
#include <WiFiUdp.h>


using namespace websockets;


// 麦克风 INMP441 (I2S 输入)
#define I2S_MIC_BCLK 15
#define I2S_MIC_WS 16
#define I2S_MIC_SD 17

// 扬声器
#define I2S_SPK_BCLK 12
#define I2S_SPK_LRC 11
#define I2S_SPK_DIN 13

// 麦克风采样参数
#define SAMPLE_RATE 16000
#define I2S_SAMPLE_BITS 16
#define RECORD_SECONDS 5  // 可设为 30
#define RECORD_BUFFER_SIZE (SAMPLE_RATE * RECORD_SECONDS)

// 按钮
#define BUTTON_PIN_1 18
bool buttonLastState1 = HIGH;

// AP账号和密码
const char* ssid = "AIWifi";
const char* password = "";

// Wifi配置
WebServer server(8080);
const char* configFile = "/config.json";

// 科大讯飞（语音转文字）API相关
const char* speechHost = "iat-api.xfyun.cn";
const char* speechPath = "/v2/iat";
WebsocketsClient wsSpeech;  // 用于语音转文字
String speechText;

// 时间ntp
WiFiUDP udp;
NTPClient timeClient(udp, "pool.ntp.org", 0, 60000);


// 首页的网页
const char indexHtml[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang='en'>
<head>
    <meta charset='UTF-8'>
    <meta name='viewport' content='width=device-width, initial-scale=1.0'>
    <title>AI Config</title>
    <style>

        body {
            margin: 0;
            padding: 0;
            font-family: Arial, sans-serif;
        }

        .container {
            max-width: 800px;
            margin: 0 auto;
            padding: 20px;
            text-align: center;
        }

        h1 {
            text-align: center;
        }

        .button {
            display: inline-block;
            height: 30px;
            width: 300px;
            margin-top: 20px;

            padding: 10px 20px;
            background-color: deepskyblue;
            color: #fff;
            border: none;
            border-radius: 20px; /* 添加圆角 */
            text-decoration: none;
            line-height: 2; /* 通过调整line-height的值来调整文字的垂直位置 */
            text-align: center; /* 文字居中 */
            box-shadow: 2px 2px 5px rgba(0, 0, 0, 0.2); /* 添加立体感 */
            transition: all 0.3s ease; /* 添加过渡效果 */
        }

        .button:hover {
            background-color: skyblue; /* 鼠标悬停时的背景颜色 */
            transform: translateY(2px); /* 点击效果 */
            box-shadow: 2px 2px 8px rgba(0, 0, 0, 0.3); /* 添加更多立体感 */
        }
        .search-box {
            margin-top: 20px;
            display: inline-block;
            height: 30px;
            width: 300px;
            padding: 5px 10px;
            background-color: #f0f0f0;
            border: 1px solid #ccc;
            border-radius: 20px;
            text-align: center; /* 文字居中 */
        }
        .hidden {
            display: none; /* 初始隐藏 */
        }
    </style>

</head>
<body>
<form action='/config' method='POST'>
    <div class='container'>
        <h1>设备配置页</h1>
        <input type='text' name='ssid' placeholder='输入WIFI名称' class='search-box'>
        <input type='text' name='pass' placeholder='输入WIFI密码' class='search-box'>
        <input type='text' name='appid' placeholder='输入讯飞Appid' class='search-box'>
        <input type='text' name='apikey' placeholder='输入讯飞ApiKey' class='search-box'>
        <input type='text' name='apisecret' placeholder='输入讯飞ApiSecret' class='search-box'>
        <input type='text' name='ttsapikey' placeholder='输入万码云apikey' class='search-box'>
        <input type='submit'  style="height: 50px;width: 320px"  class='button'  value="保存">
    </div>
</form>
</body>
</html>
)rawliteral";


typedef struct {
  String ssid;
  String pass;
  String appid;
  String apikey;
  String apisecret;
  String ttsapikey;
} SystemConfig;
// 全局变量声明
SystemConfig globalConfig;

// OLED屏幕
U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C u8g2(U8G2_R0, /* reset=*/U8X8_PIN_NONE, /* SCL=*/5, /* SDA=*/4);



// 计时变量（音频发送）
unsigned long startTime = 0;
unsigned long lastSendTime = 0;
unsigned long globalEpochTime = 0;
// 状态标记及结果存储
volatile bool isRecording = false;     // 当前是否正在录音
volatile bool speechFinished = false;  // 语音识别结果是否返回（结束帧



// 从 SPIFFS 加载配置
bool loadConfig() {
  if (!SPIFFS.begin(true)) {
    sendMsg("", "SPIFFS 初始化失败");
    return false;
  }
  File file = SPIFFS.open(configFile, "r");
  if (!file) {
    sendMsg("系统文件不存在", "http://192.168.4.1:8080/");
    return false;
  }
  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, file);
  if (error) {
    sendMsg("系统文件未设置", "http://192.168.4.1:8080/");
    file.close();
    return false;
  }
  // 读取数据，并且转换成 SystemConfig 结构体
  globalConfig.ssid = doc["ssid"].as<String>();
  globalConfig.pass = doc["pass"].as<String>();
  globalConfig.appid = doc["appid"].as<String>();
  globalConfig.apikey = doc["apikey"].as<String>();
  globalConfig.apisecret = doc["apisecret"].as<String>();
  globalConfig.ttsapikey = doc["ttsapikey"].as<String>();
  // 空数据的时候
  if (globalConfig.ssid == "" || globalConfig.pass == "") {
    sendMsg("请先链接热点配置系统", "http://192.168.4.1:8080/");
    file.close();
    return false;
  }
  Serial.printf("ssid=%s,pass=%s", globalConfig.ssid, globalConfig.pass);
  Serial.printf("appid=%s,apikey=%s,apisecret=%s,ttsapikey=%s", globalConfig.appid, globalConfig.apikey, globalConfig.apisecret, globalConfig.ttsapikey);

  // 关闭文件
  file.close();
  return true;
}

// 初始化按钮
void initButton() {
  // 使用内部上拉
  pinMode(BUTTON_PIN_1, INPUT_PULLUP);
  buttonLastState1 = HIGH;
  Serial.println("initButton finished");
}
// 初始化 I2S 麦克风
void initI2SMic() {
  i2s_config_t i2s_config = {
    .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S_MSB,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 512
  };

  const i2s_pin_config_t inmp441_pin_config = {
    .bck_io_num = I2S_MIC_BCLK,
    .ws_io_num = I2S_MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_MIC_SD
  };
  i2s_driver_install(I2S_NUM_1, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_1, &inmp441_pin_config);
  i2s_zero_dma_buffer(I2S_NUM_1);
}
// 初始化 I2S 扬声器
void initI2SSpeaker() {
  i2s_config_t i2s_config = {
    .mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 1024,
    .use_apll = false
  };
  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SPK_BCLK,
    .ws_io_num = I2S_SPK_LRC,
    .data_out_num = I2S_SPK_DIN,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);

  Serial.println("initI2SSpeaker finished");
}
// 初始化wifi
void initWifi() {
  if (globalConfig.ssid.length() > 0 && globalConfig.pass.length() > 0) {
    WiFi.begin(globalConfig.ssid.c_str(), globalConfig.pass.c_str());
    sendMsg("正在连接WIFI", globalConfig.ssid);
    // 等待WiFi连接，最多等待10秒
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      sendMsg("WIFI已连接", "IP: " + WiFi.localIP().toString());
    } else {
      sendMsg("WIFI连接失败", "请检查配置");
    }
  }
  Serial.println("initWifi finished");
}

void initNtp() {
  timeClient.begin();
  timeClient.update();  // 获取初始时间
  Serial.println("initNtp finished");
}

void initServer() {
  server.on("/config", HTTP_POST, []() {
    // 获取表单数据
    String ssid = server.arg("ssid");
    String pass = server.arg("pass");
    String appid = server.arg("appid");
    String apikey = server.arg("apikey");
    String apisecret = server.arg("apisecret");
    String ttsapikey = server.arg("ttsapikey");
    // 创建JSON文档
    DynamicJsonDocument doc(2048);
    doc["ssid"] = ssid;
    doc["pass"] = pass;
    doc["appid"] = appid;
    doc["apikey"] = apikey;
    doc["apisecret"] = apisecret;
    doc["ttsapikey"] = ttsapikey;
    // 保存到SPIFFS
    File file = SPIFFS.open(configFile, "w");
    if (!file) {
      server.send(500, "text/plain", "Save fail");
      return;
    }
    // 序列化JSON到文件
    if (serializeJson(doc, file)) {
      server.send(200, "text/plain", "Save success, Rebot system now!");
      file.close();
      delay(1000);
      ESP.restart();
    } else {
      server.send(500, "text/plain", "保存失败");
    }
    Serial.println("config save done");
  });
  server.on("/", HTTP_GET, []() {
    String html((__FlashStringHelper*)indexHtml);
    server.send(200, "text/html", html);
    Serial.println("index loaded");
  });
  server.begin();
  Serial.println("HTTP 服务器已启动，监听端口 8080");
  Serial.println("initServer finished");
}



void listenButtonEvent(uint8_t pin, bool& lastState, void (*onPress)(), void (*onRelease)()) {
  bool currentState = digitalRead(pin);
  // Serial.println("currentState=" + currentState);
  // 检测按下事件（HIGH -> LOW）
  if (lastState == HIGH && currentState == LOW) {
    if (onPress) onPress();
  }
  // 检测释放事件（LOW -> HIGH）
  else if (lastState == LOW && currentState == HIGH) {
    if (onRelease) onRelease();
  }
  // 更新状态
  lastState = currentState;
}


String getDate() {
  time_t epochTime = timeClient.getEpochTime();
  struct tm* ptm = gmtime(&epochTime);  // 转换为 GMT 时间
  char timeString[40];
  strftime(timeString, sizeof(timeString), "%a, %d %b %Y %H:%M:%S GMT", ptm);
  return String(timeString);
}

String createAuthUrl() {
  String date = getDate();
  if (date == "")
    return "";
  String tmp = "host: " + String(speechHost) + "\n";
  tmp += "date: " + date + "\n";
  tmp += "GET " + String(speechPath) + " HTTP/1.1";
  String signature = hmacSHA256(globalConfig.apisecret, tmp);
  String authOrigin = "api_key=\"" + String(globalConfig.apikey) + "\", algorithm=\"hmac-sha256\", headers=\"host date request-line\", signature=\"" + signature + "\"";
  unsigned char authBase64[256] = { 0 };
  size_t authLen = 0;
  int ret = mbedtls_base64_encode(authBase64, sizeof(authBase64) - 1, &authLen, (const unsigned char*)authOrigin.c_str(), authOrigin.length());
  if (ret != 0)
    return "";
  String authorization = String((char*)authBase64);
  String encodedDate = "";
  for (int i = 0; i < date.length(); i++) {
    char c = date.charAt(i);
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encodedDate += c;
    } else if (c == ' ') {
      encodedDate += "+";
    } else if (c == ',') {
      encodedDate += "%2C";
    } else if (c == ':') {
      encodedDate += "%3A";
    } else {
      encodedDate += "%" + String(c, HEX);
    }
  }
  String url = "ws://" + String(speechHost) + String(speechPath) + "?authorization=" + authorization + "&date=" + encodedDate + "&host=" + speechHost;
  Serial.println(url);
  return url;
}


void connectToIFLY() {
  String wsUrl = createAuthUrl();
  wsSpeech.connect(wsUrl);
  wsSpeech.onMessage([](WebsocketsMessage message) {
    Serial.println("返回内容: " + message.data());
    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, message.data());
    if (err.c_str() != "Ok") {
      Serial.print("语音JSON解析错误：");
      Serial.println(err.c_str());
      sendMsg("语音解析错误", err.c_str());
      return;
    }
    //拿出数据

    String tempText = "";
    if (doc.containsKey("data") && doc["data"].containsKey("result") && doc["data"]["result"].containsKey("ws")) {
      for (JsonObject wsObj : doc["data"]["result"]["ws"].as<JsonArray>()) {
        for (JsonObject cwObj : wsObj["cw"].as<JsonArray>()) {
          tempText += cwObj["w"].as<String>() + " ";
        }
      }
      tempText.trim();
      speechText = tempText;
      Serial.println("识别结果：" + speechText);
    }
  });

  // 等待 WebSocket 连接建立
  unsigned long startTime = millis();
  while (!wsSpeech.available() && millis() - startTime < 1000) {
    delay(10);
  }

  // 发个hi
  DynamicJsonDocument jsonDoc(2048);
  jsonDoc["common"]["app_id"] = globalConfig.appid;
  jsonDoc["business"]["language"] = "zh_cn";
  jsonDoc["business"]["domain"] = "iat";       //iat：日常用语
  jsonDoc["business"]["accent"] = "mandarin";  //mandarin：中文普通话、其他语种
  jsonDoc["business"]["vad_eos"] = 3000;
  jsonDoc["data"]["status"] = 0;
  jsonDoc["data"]["format"] = "audio/L16;rate=16000";
  jsonDoc["data"]["encoding"] = "raw";
  char buf[512];
  serializeJson(jsonDoc, buf);
  wsSpeech.send(buf);
}

void sendAudioData(bool firstFrame = false) {
  Serial.println("sendAudioData");
  const int FRAME_SIZE = 1280;        // 16-bit PCM，每帧 1280B 对应 40ms
  static uint8_t buffer[FRAME_SIZE];  // 音频数据缓冲区
  size_t bytesRead = 0;
  static unsigned long lastSendTime = 0;

  unsigned long currentMillis = millis();

  // 每40ms发送一次音频
  if (currentMillis - lastSendTime < 40) {
    return;  // 如果间隔不到40ms，不发送数据
  }

  lastSendTime = currentMillis;  // 更新发送时间

  // 读取 I2S 音频数据
  esp_err_t result = i2s_read(I2S_NUM_1, buffer, FRAME_SIZE, &bytesRead, portMAX_DELAY);
  if (result != ESP_OK || bytesRead == 0) {
    Serial.println("I2S Read Failed or No Data!");
    return;
  }

  // Base64 编码
  String base64Audio = base64Encode(buffer, bytesRead);
  if (base64Audio.length() == 0) {
    Serial.println("Base64 Encoding Failed!");
    return;
  }

  // 发送 JSON 数据
  DynamicJsonDocument jsonDoc(2048);
  jsonDoc["data"]["status"] = firstFrame ? 0 : 1;  // 第一帧 status = 0，其他帧 status = 1
  jsonDoc["data"]["format"] = "audio/L16;rate=16000";
  jsonDoc["data"]["encoding"] = "raw";
  jsonDoc["data"]["audio"] = base64Audio;  // 确保 Base64 编码成功

  char jsonBuffer[2048];
  serializeJson(jsonDoc, jsonBuffer);
  Serial.printf("jsonBuffer %s", jsonBuffer);

  wsSpeech.send(jsonBuffer);  // 发送音频数据
  Serial.printf("Sent %d bytes, status: %d\n", bytesRead, firstFrame ? 0 : 1);
}

void startRecording() {
  Serial.println("startRecording");

  isRecording = true;
  startTime = millis();
  sendAudioData(true);
}

void stopRecording() {
  Serial.println("stopRecording");

  isRecording = false;
  // 发个bye
  DynamicJsonDocument jsonDoc(2048);
  jsonDoc["data"]["status"] = 2;  // 结束传输
  char buf[128];
  serializeJson(jsonDoc, buf);
  if (!wsSpeech.send(buf)) {
    // 失败逻辑
    Serial.println("发送语音失败");
  }
  Serial.println("录音结束，已发送结束信号");
}

void handlePress1() {
  sendMsg("", "开始语音识别:" + isRecording);
  if (!isRecording) {
    connectToIFLY();
    startRecording();
  }
}

void handleRelease1() {
  if (isRecording) {
    stopRecording();
  }
  sendMsg("", "按钮1松开:" + isRecording);
}

void initButtonListener() {
  listenButtonEvent(BUTTON_PIN_1, buttonLastState1, handlePress1, handleRelease1);
}

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);  // 等待 USB CDC 就绪（ESP32-S3 特有）


  delay(1000);  // 等待串口稳定
  WiFi.mode(WIFI_OFF);
  delay(100);
  WiFi.mode(WIFI_AP);

  u8g2.begin();
  u8g2.enableUTF8Print();  // 启用 UTF-8 支持
  delay(100);

  sendMsg("系统", "启动中");
  // 设置wifi热点
  bool result = WiFi.softAP(ssid);
  if (result) {
    sendMsg("", "热点创建成功");
  } else {
    sendMsg("", "热点创建失败");
  }

  initButton();
  initI2SMic();
  initI2SSpeaker();
  initServer();
  // 读取配置
  bool status = loadConfig();
  if (status) {
    initWifi();
    initNtp();
    Serial.println("系统启动");
  } else {
    Serial.println("系统尚未完成");
  }
}

void loop() {
  server.handleClient();
  initButtonListener();
  wsSpeech.poll();  //持续消息接受
}

String lastMsg1 = "";
String lastMsg2 = "";

void sendMsg(String msg1, String msg2) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);

  // 清除每行内容（避免残影）
  u8g2.setDrawColor(0);
  u8g2.drawBox(0, 0, u8g2.getDisplayWidth(), 16);   // 清第1行
  u8g2.drawBox(0, 20, u8g2.getDisplayWidth(), 16);  // 清第2行
  u8g2.setDrawColor(1);

  // 显示第1行
  msg1 = msg1.length() > 0 ? msg1 : lastMsg1;
  u8g2.setCursor(0, 14);
  u8g2.print(msg1);
  lastMsg1 = msg1;

  // 显示第2行
  msg2 = msg2.length() > 0 ? msg2 : lastMsg2;
  u8g2.setCursor(0, 30);
  u8g2.print(msg2);
  lastMsg2 = msg2;

  u8g2.sendBuffer();
}


////////////////////////////
// 工具函数：Base64、HMAC、时间格式转换等
////////////////////////////
String base64Encode(const uint8_t* data, size_t len) {
  if (len == 0 || data == nullptr) {
    Serial.println("Base64编码错误：无数据");
    return "";
  }
  size_t outputLen = 0;
  size_t bufSize = ((len + 2) / 3) * 4 + 1;
  char* buf = (char*)malloc(bufSize);
  if (!buf)
    return "";
  int ret = mbedtls_base64_encode((unsigned char*)buf, bufSize, &outputLen, data, len);
  if (ret != 0) {
    free(buf);
    return "";
  }
  String encoded = String(buf);
  free(buf);
  return encoded;
}

String hmacSHA256(const String& key, const String& data) {
  unsigned char hmacResult[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char*)key.c_str(), key.length());
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)data.c_str(), data.length());
  mbedtls_md_hmac_finish(&ctx, hmacResult);
  mbedtls_md_free(&ctx);
  size_t outLen;
  unsigned char base64Result[64];
  mbedtls_base64_encode(base64Result, sizeof(base64Result), &outLen, hmacResult, sizeof(hmacResult));
  return String((char*)base64Result);
}

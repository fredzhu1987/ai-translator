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
#include <Base64_Arturo.h>

using namespace websockets;


// 麦克风 INMP441 (I2S 输入)
#define I2S_MIC_SCK 15
#define I2S_MIC_WS 16
#define I2S_MIC_SD 17
#define SAMPLE_RATE 16000
#define CHANNELS 1
#define BIT_DEPTH 16
#define FRAME_SIZE 1280  // 16-bit PCM，每帧 1280B 对应 40ms
#define CHUNK_SIZE 1024


// 扬声器
#define I2S_SPK_LRC 11
#define I2S_SPK_BCLK 12
#define I2S_SPK_DIN 13


// oled
#define OLED_SCL 5
#define OLED_SDA 4


// 按钮
#define BUTTON_PIN_1 18
#define BUTTON_PIN_2 8
bool buttonLastState1 = HIGH;
bool buttonLastState2 = HIGH;

// AP账号和密码
const char* ssid = "AIWifi";
const char* password = "";

// Wifi配置
WebServer server(8080);
const char* configFile = "/config.json";

WebsocketsClient wsSpeech;  // 用于语音转文字
WebsocketsClient wsChat;    // 用于大模型对话
WebsocketsClient wsTTS;     // 用于tts转语音

// 科大讯飞（语音转文字）API相关
const char* speechHost = "iat-api.xfyun.cn";
const char* speechPath = "/v2/iat";
// 讯飞大模型
const char* chatHost = "spark-api.xf-yun.com";
const char* chatPath = "/v4.0/chat";
// 讯飞语音合成
const char* ttsHost = "tts-api.xfyun.cn";
const char* ttsPath = "/v2/tts";

String speechText;                  //tts返回的用户输入语音
String chatAggregated;              //累计大模型返回的文字
unsigned long lastChatMsgTime = 0;  //最后大模型的时间，计算下间隔以后在播放
String lastAudioUrl;
String ttsAudioBase64;

// 计时变量（音频发送）
unsigned long startTime = 0;
unsigned long lastSendTime = 0;
// 状态标记及结果存储
volatile bool isRecording = false;     // 当前是否正在录音
volatile bool speechFinished = false;  // 语音识别结果是否返回（结束帧


int16_t audioData[2560];
int16_t* pcm_data;  //录音缓存区
uint recordingSize = 0;
#define I2S_PORT_0 I2S_NUM_0
#define SAMPLE_RATE 16000
#define RECORD_TIME_SECONDS 60
#define BUFFER_SIZE (SAMPLE_RATE * RECORD_TIME_SECONDS)

// 时间ntp
WiFiUDP udp;
NTPClient timeClient(udp, "cn.pool.ntp.org", 0, 60000);


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
U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C u8g2(U8G2_R0, /* reset=*/U8X8_PIN_NONE, /* SCL=*/OLED_SCL, /* SDA=*/OLED_SDA);



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
  // 空数据的时候
  if (globalConfig.ssid == "" || globalConfig.pass == "") {
    sendMsg("请先链接热点配置系统", "http://192.168.4.1:8080/");
    file.close();
    return false;
  }
  Serial.printf("ssid=%s,pass=%s", globalConfig.ssid.c_str(), globalConfig.pass.c_str());
  Serial.printf("appid=%s,apikey=%s,apisecret=%s", globalConfig.appid.c_str(), globalConfig.apikey.c_str(), globalConfig.apisecret.c_str());
  Serial.println("");

  // 关闭文件
  file.close();
  return true;
}

// 初始化按钮
void initButton() {
  // 使用内部上拉
  pinMode(BUTTON_PIN_1, INPUT_PULLUP);
  pinMode(BUTTON_PIN_2, INPUT_PULLUP);
  buttonLastState1 = HIGH;
  buttonLastState2 = HIGH;
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
    .dma_buf_len = 512,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  const i2s_pin_config_t inmp441_pin_config = {
    .bck_io_num = I2S_MIC_SCK,
    .ws_io_num = I2S_MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_MIC_SD
  };
  esp_err_t err;
  err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.print("i2s_driver_install I2S_NUM_0 failed with error: ");
    Serial.println(err);  // 打印出整数错误码
  }
  err = i2s_set_pin(I2S_NUM_0, &inmp441_pin_config);
  if (err != ESP_OK) {
    Serial.print("i2s_set_pin I2S_NUM_0 failed with error: ");
    Serial.println(err);  // 打印出整数错误码
  }
  err = i2s_zero_dma_buffer(I2S_NUM_0);
  if (err != ESP_OK) {
    Serial.print("i2s_zero_dma_buffer I2S_NUM_0 failed with error: ");
    Serial.println(err);  // 打印出整数错误码
  }

  Serial.println("initI2SMic finished");
}
// 初始化 I2S 扬声器
void initI2SSpeaker() {
  i2s_config_t i2s_config = {
    .mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S_MSB,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 512,
    .use_apll = false
  };
  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SPK_BCLK,
    .ws_io_num = I2S_SPK_LRC,
    .data_out_num = I2S_SPK_DIN,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  esp_err_t err;
  err = i2s_driver_install(I2S_NUM_1, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.print("i2s_driver_install I2S_NUM_1 failed with error: ");
    Serial.println(err);
  }
  err = i2s_set_pin(I2S_NUM_1, &pin_config);
  if (err != ESP_OK) {
    Serial.print("i2s_set_pin I2S_NUM_1 failed with error: ");
    Serial.println(err);
  }
  err = i2s_zero_dma_buffer(I2S_NUM_1);
  if (err != ESP_OK) {
    Serial.print("i2s_zero_dma_buffer I2S_NUM_1 failed with error: ");
    Serial.println(err);
  }

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
    Serial.println("initServer ssid = " + ssid);
    Serial.println("initServer pass = " + pass);
    Serial.println("initServer appid = " + appid);
    Serial.println("initServer apikey = " + apikey);
    Serial.println("initServer apisecret = " + apisecret);
    // 创建JSON文档
    DynamicJsonDocument doc(2048);
    doc["ssid"] = ssid;
    doc["pass"] = pass;
    doc["appid"] = appid;
    doc["apikey"] = apikey;
    doc["apisecret"] = apisecret;
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


void connectToIFLY() {
  String wsUrl = generateXunFeiAuthURL(speechHost, speechPath);
  if (wsSpeech.available()) {
    wsSpeech.close();
  }
  bool connected = wsSpeech.connect(wsUrl);
  wsSpeech.onMessage([](WebsocketsMessage message) {
    Serial.println("[tts2text]返回内容: " + message.data());
    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, message.data());
    if (err.c_str() != "Ok") {
      Serial.print("[tts2text]SON解析错误：");
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
      Serial.println("[tts2text]识别结果 speechText：" + speechText);
      wsSpeech.close();  //关闭连接
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
  if (!connected) {
    Serial.println("[tts2text]Not Connected!");
  } else {
    Serial.println("[tts2text]连接成功");
    Serial.printf("[tts2text]first jsonBuffer %s\n", buf);
    wsSpeech.send(buf);
  }
}

void sendAudioData(bool firstFrame = false) {
  Serial.println("[tts2text]sendAudioData");
  static int16_t buffer[FRAME_SIZE];  // 音频数据缓冲区
  size_t bytesRead = 0;
  static unsigned long lastSendTime = 0;

  unsigned long currentMillis = millis();
  // 每40ms发送一次音频
  if (currentMillis - lastSendTime < 40) {
    Serial.println("[tts2text]不足40ms不发送数据");
    return;  // 如果间隔不到40ms，不发送数据
  }
  lastSendTime = currentMillis;  // 更新发送时间

  // 读取 I2S 音频数据
  // esp_err_t result = i2s_read(I2S_NUM_0, buffer, sizeof(buffer), &bytesRead, 100 / portTICK_PERIOD_MS);
  esp_err_t result = i2s_read(I2S_NUM_0, buffer, sizeof(buffer), &bytesRead, portMAX_DELAY);
  if (result != ESP_OK || bytesRead == 0) {
    Serial.println("[tts2text]I2S Read Failed or No Data!");
    return;
  }

  String base64Audio = base64Encode((uint8_t*)buffer, bytesRead);
  // String base64Audio = base64Encode(reinterpret_cast<const uint8_t*>(buffer), bytesRead);
  // String base64Audio = base64Encode(buffer, bytesRead);
  if (base64Audio.length() == 0) {
    Serial.println("[tts2text]Base64 Encoding Failed!");
    return;
  }

  // 发送 JSON 数据
  DynamicJsonDocument jsonDoc(2048);
  jsonDoc["data"]["status"] = firstFrame ? 0 : 1;  // 第一帧 status = 0，其他帧 status = 1
  jsonDoc["data"]["format"] = "audio/L16;rate=16000";
  jsonDoc["data"]["encoding"] = "raw";
  jsonDoc["data"]["audio"] = base64Audio;  // 确保 Base64 编码成功

  String jsonBufferStr;
  serializeJson(jsonDoc, jsonBufferStr);
  Serial.printf("[tts2text]jsonBufferStr %s\n", jsonBufferStr.c_str());


  if (!wsSpeech.send(jsonBufferStr.c_str())) {
    Serial.println("[tts2text]数据发送失败");
  } else {
    Serial.println("[tts2text]数据发送成功");
  }
}

void sendTTSRequest(const String& text) {
  Serial.println("[TTS]sendTTSRequest text:" + text);
  sendMsg("", text);

  DynamicJsonDocument doc(2048);
  JsonObject common = doc.createNestedObject("common");
  common["app_id"] = globalConfig.appid;
  JsonObject business = doc.createNestedObject("business");
  business["aue"] = "raw";
  business["vcn"] = "xiaoyan";
  business["pitch"] = 50;
  business["speed"] = 50;
  JsonObject data = doc.createNestedObject("data");
  data["status"] = 2;

  const char* utf8Text = text.c_str();
  size_t utf8Len = strlen(utf8Text);

  size_t b64Len = 0;
  mbedtls_base64_encode(NULL, 0, &b64Len, (const unsigned char*)utf8Text, utf8Len);  // 先获取需要长度

  unsigned char* b64 = (unsigned char*)malloc(b64Len);
  if (!b64) {
    Serial.println("[TTS] malloc失败");
    return;
  }
  memset(b64, 0, b64Len);

  size_t actualLen = 0;
  int ret = mbedtls_base64_encode(b64, b64Len, &actualLen, (const unsigned char*)utf8Text, utf8Len);
  if (ret == 0) {
    data["text"] = String((char*)b64, actualLen);
  } else {
    Serial.printf("[TTS] base64编码失败，错误码: -0x%04X\n", -ret);
    free(b64);
    return;
  }
  free(b64);

  String output;
  serializeJson(doc, output);
  Serial.println("[TTS]数据发送,output:" + output);
  if (!wsTTS.send(output.c_str())) {
    Serial.println("[TTS]数据发送失败");
  } else {
    Serial.println("[TTS]数据发送成功");
  }
}

void playAudio(String base64Str) {
  Serial.println("[audio]playAudio");

  base64Str.replace("\n", "");
  base64Str.replace("\r", "");
  base64Str.replace(" ", "");

  if (base64Str.length() % 4 != 0) {
    Serial.println("⚠️ base64 长度非法");
    return;
  }

  size_t inputLen = base64Str.length();
  size_t maxChunkInputLen = 768;  // 每次处理 768 个字符，输出约 576 字节
  uint8_t* decodeBuf = (uint8_t*)malloc(576);
  if (!decodeBuf) {
    Serial.println("❌ 内存分配失败");
    return;
  }

  i2s_start(I2S_NUM_1);

  for (size_t offset = 0; offset < inputLen; offset += maxChunkInputLen) {
    size_t chunkLen = (inputLen - offset) > maxChunkInputLen ? maxChunkInputLen : (inputLen - offset);
    size_t outputLen = 0;
    int ret = mbedtls_base64_decode(
      decodeBuf, 576, &outputLen,
      (const unsigned char*)base64Str.substring(offset, offset + chunkLen).c_str(), chunkLen);

    if (ret != 0) {
      Serial.printf("❌ 解码失败 %d\n", ret);
      break;
    }

    size_t written = 0;
    i2s_write(I2S_NUM_1, decodeBuf, outputLen, &written, portMAX_DELAY);
  }

  free(decodeBuf);
  delay(100);
  i2s_zero_dma_buffer(I2S_NUM_1);
  i2s_stop(I2S_NUM_1);
}


void txt2TTS(String text) {
  if (wsTTS.available()) {
    wsTTS.close();
  }
  if (!wsTTS.available()) {
    String ttsURL = generateXunFeiAuthURL(ttsHost, ttsPath);
    Serial.println("[TTS]连接 URL：" + ttsURL);
    wsTTS.onMessage([](WebsocketsMessage message) {
      Serial.println("[TTS]返回内容: " + message.data());
      DynamicJsonDocument doc(4096);
      DeserializationError err = deserializeJson(doc, message.data());
      if (err.c_str() != "Ok") {
        Serial.print("[TTS]大模型JSON解析错误:");
        Serial.println(err.c_str());
        return;
      }
      int code = doc["code"];
      if (code == 0) {
        // 提取当前回复内容和序号
        if (doc["data"].isNull()) {
          return;
        }
        int status = doc["data"]["status"];
        ttsAudioBase64 = doc["data"]["audio"].as<String>();
        playAudio(ttsAudioBase64);
        if (status == 2) {
          wsTTS.close();  //关闭连接
        }
      } else {
        Serial.println("[TTS]请求失败，错误码：" + String(code));
      }
    });
    bool connected = wsTTS.connect(ttsURL);
    unsigned long startTime = millis();
    while (!wsTTS.available() && millis() - startTime < 1000) {
      delay(10);
    }
    if (!connected) {
      Serial.println("[TTS]Not Connected!");
    } else {
      Serial.println("[TTS]连接成功");
      sendTTSRequest(text);
    }
  } else {
    sendTTSRequest(text);
  }
}

void sendChatRequest(const String& userInput) {
  DynamicJsonDocument doc(2048);
  JsonObject header = doc.createNestedObject("header");
  header["app_id"] = globalConfig.appid;
  JsonObject parameter = doc.createNestedObject("parameter");
  JsonObject chat = parameter.createNestedObject("chat");
  chat["domain"] = "4.0Ultra";
  chat["temperature"] = 0.5;
  chat["max_tokens"] = 1024;
  JsonObject payload = doc.createNestedObject("payload");
  JsonObject message = payload.createNestedObject("message");
  JsonArray textArr = message.createNestedArray("text");
  // 系统提示（可根据需要修改）
  JsonObject systemMsg = textArr.createNestedObject();
  systemMsg["role"] = "system";
  systemMsg["content"] = "你是个翻译员,给一个单词或者短句,你返回对应的英语";
  // 用户输入，使用语音识别的结果
  JsonObject userMsg = textArr.createNestedObject();
  userMsg["role"] = "user";
  userMsg["content"] = userInput;
  String output;
  serializeJson(doc, output);
  Serial.println("[chat]发送内容 output:" + output);
  if (!wsChat.send(output.c_str())) {
    Serial.println("[chat]数据发送失败");
  } else {
    Serial.println("[chat]数据发送成功");
  }
}

void processChatResult() {
}

void processSpeechResult() {
  // 没有录音，并且有结果的时候处理逻辑
  if (speechText == "" || isRecording) {
    return;
  }
  Serial.println("[chat]speechText:" + speechText);
  chatAggregated = "";
  if (wsChat.available()) {
    wsChat.close();
  }
  if (!wsChat.available()) {
    String chatURL = generateXunFeiAuthURL(chatHost, chatPath);
    Serial.println("[chat]大模型对话WS URL:" + chatURL);
    wsChat.onMessage([](WebsocketsMessage message) {
      Serial.println("[chat]大模型返回内容:" + message.data());
      DynamicJsonDocument doc(2048);
      DeserializationError err = deserializeJson(doc, message.data());
      if (err.c_str() != "Ok") {
        Serial.print("[chat]大模型JSON解析错误:");
        Serial.println(err.c_str());
        return;
      }
      int code = doc["header"]["code"];
      int status = doc["header"]["status"];
      if (code == 0) {
        String content = doc["payload"]["choices"]["text"][0]["content"].as<String>();
        chatAggregated += content;
      } else {
        Serial.println("[chat]请求失败，错误码：" + String(code));
      }
      if (status == 2) {
        Serial.println("[chat]当前累计回复：" + chatAggregated);
        wsChat.close();  //关闭连接
        txt2TTS(chatAggregated);
      }
    });
    bool connected = wsChat.connect(chatURL);
    unsigned long startTime = millis();
    while (!wsChat.available() && millis() - startTime < 1000) {
      delay(10);
    }
    if (!connected) {
      Serial.println("[chat]Not Connected!");
    } else {
      Serial.println("[chat]连接成功");
      sendChatRequest(speechText);
    }
  } else {
    sendChatRequest(speechText);
  }
  speechText = "";
}

void startRecording() {
  Serial.println("[btn]startRecording");

  isRecording = true;
  startTime = millis();
  sendAudioData(false);
}


void stopRecording() {
  Serial.println("[btn]stopRecording");

  // 发个bye
  DynamicJsonDocument jsonDoc(2048);
  jsonDoc["data"]["status"] = 2;  // 结束传输
  char buf[128];
  serializeJson(jsonDoc, buf);
  Serial.printf("[tts2text]jsonBuffer %s\n", buf);

  if (!wsSpeech.send(buf)) {  // 失败逻辑
    Serial.println("[tts2text]发送语音失败,结束指令");
  } else {
    Serial.println("[tts2text]发送语音成功,结束指令");
  }
  isRecording = false;
}

void handlePress1() {
  sendMsg("", "听取中...");
  connectToIFLY();
  if (!isRecording) {
    startRecording();
  }
}

void handleRelease1() {
  if (isRecording) {
    sendMsg("", "处理中...");
    stopRecording();
  }
}


void handlePress2() {
  // sendMsg("", "[btn]重播");
  // playAudio(ttsAudioBase64);
}

void handleRelease2() {
  playAudio(ttsAudioBase64);
  // sendMsg("", "[btn]按钮2松开");
}

void initButtonListener() {
  listenButtonEvent(BUTTON_PIN_1, buttonLastState1, handlePress1, handleRelease1);
  listenButtonEvent(BUTTON_PIN_2, buttonLastState2, handlePress2, handleRelease2);
}

void displayTask(void* parameter) {
  // 你的任务逻辑
  for (;;) {
    timeClient.update();
    // Serial.println("[task]displayTask");
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
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
  // 1秒刷新下的任务。和loop不一样
  xTaskCreatePinnedToCore(displayTask, "DisplayTask", 10000, NULL, 1, NULL, 1);
  // 读取配置
  bool status = loadConfig();
  if (status) {
    initWifi();
    initNtp();
    Serial.println("系统启动");
  } else {
    Serial.println("系统尚未完成");
  }
  // txt2TTS("How are you？");
}

void loop() {
  server.handleClient();
  initButtonListener();
  wsSpeech.poll();  //持续消息接受
  wsChat.poll();
  wsTTS.poll();

  // 持续发送音频数据
  if (isRecording) {
    sendAudioData(false);
  }

  processSpeechResult();
  processChatResult();
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


String generateXunFeiAuthURL(String host, String path) {
  String date = getDate();
  if (date == "")
    return "";
  String tmp = "host: " + String(host) + "\n";
  tmp += "date: " + date + "\n";
  tmp += "GET " + String(path) + " HTTP/1.1";
  String signature = hmacSHA256(globalConfig.apisecret.c_str(), tmp);
  String authOrigin = "api_key=\"" + String(globalConfig.apikey.c_str()) + "\", algorithm=\"hmac-sha256\", headers=\"host date request-line\", signature=\"" + signature + "\"";
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
  String url = "ws://" + String(host) + String(path) + "?authorization=" + authorization + "&date=" + encodedDate + "&host=" + host;
  return url;
}

////////////////////////////
// 工具函数：Base64、HMAC、时间格式转换等
////////////////////////////
String getDate() {
  time_t epochTime = timeClient.getEpochTime();
  struct tm* ptm = gmtime(&epochTime);  // 转换为 GMT 时间
  char timeString[40];
  strftime(timeString, sizeof(timeString), "%a, %d %b %Y %H:%M:%S GMT", ptm);
  Serial.println("[ntp]timeString:" + String(timeString));
  return String(timeString);
}


String base64Encode(const void* data, size_t len) {
  if (!data || len == 0) return "";

  size_t outputLen = 0;
  size_t bufSize = ((len + 2) / 3) * 4 + 1;
  char* buf = (char*)malloc(bufSize);
  if (!buf) return "";

  int ret = mbedtls_base64_encode((unsigned char*)buf, bufSize, &outputLen, (const unsigned char*)data, len);
  if (ret != 0) {
    Serial.printf("Base64 编码失败，错误码：%d\n", ret);
    free(buf);
    return "";
  }
  String result(buf);
  free(buf);
  return result;
}
// String base64Encode(const uint8_t* data, size_t len) {
//   if (len == 0 || data == nullptr) {
//     Serial.println("Base64编码错误：无数据");
//     return "";
//   }
//   size_t outputLen = 0;
//   size_t bufSize = ((len + 2) / 3) * 4 + 1;
//   char* buf = (char*)malloc(bufSize);
//   if (!buf)
//     return "";
//   int ret = mbedtls_base64_encode((unsigned char*)buf, bufSize, &outputLen, data, len);
//   if (ret != 0) {
//     free(buf);
//     return "";
//   }
//   String encoded = String(buf);
//   free(buf);
//   return encoded;
// }

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

const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

String base64_encode(uint8_t const* buf, unsigned int bufLen) {
  String result;
  int i = 0;
  uint8_t array3[3];
  uint8_t array4[4];

  while (bufLen--) {
    array3[i++] = *(buf++);
    if (i == 3) {
      array4[0] = (array3[0] & 0xfc) >> 2;
      array4[1] = ((array3[0] & 0x03) << 4) + ((array3[1] & 0xf0) >> 4);
      array4[2] = ((array3[1] & 0x0f) << 2) + ((array3[2] & 0xc0) >> 6);
      array4[3] = array3[2] & 0x3f;

      for (i = 0; i < 4; i++) {
        result += base64_chars[array4[i]];
      }
      i = 0;
    }
  }

  if (i) {
    for (int j = i; j < 3; j++) array3[j] = '\0';

    array4[0] = (array3[0] & 0xfc) >> 2;
    array4[1] = ((array3[0] & 0x03) << 4) + ((array3[1] & 0xf0) >> 4);
    array4[2] = ((array3[1] & 0x0f) << 2) + ((array3[2] & 0xc0) >> 6);
    array4[3] = array3[2] & 0x3f;

    for (int j = 0; j < i + 1; j++) result += base64_chars[array4[j]];
    while ((i++ < 3)) result += '=';
  }

  return result;
}


int base64CharToValue(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return 0;
}

// int decode_base64(const char* input, uint8_t* output) {
//   size_t output_len = 0;
//   int ret = mbedtls_base64_decode(output, 8192, &output_len, (const unsigned char*)input, strlen(input));
//   if (ret != 0) {
//     Serial.printf("Base64 解码失败，错误码 %d\n", ret);
//     return 0;
//   }
//   return output_len;
// }

// int decode_base64(const char* input, uint8_t* output) {
//   int len = strlen(input);
//   int i = 0, j = 0;
//   uint32_t buf = 0;
//   int valb = -8;

//   while (i < len) {
//     char c = input[i++];
//     if (c == '=') break;
//     int val = base64CharToValue(c);
//     buf = (buf << 6) | val;
//     valb += 6;
//     if (valb >= 0) {
//       output[j++] = (buf >> valb) & 0xFF;
//       valb -= 8;
//     }
//   }
//   return j;  // 实际解码的字节数
// }

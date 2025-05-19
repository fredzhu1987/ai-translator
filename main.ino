#include <U8g2lib.h>
#include <Wire.h>
#include <driver/i2s.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>


const char* ssid = "AIWifi";
const char* password = "";

// wifi账号密码还有http本地服务器
// const char* ssid = "zls";             // <-- 修改为你的 WiFi 名称
// const char* password = "kuci@biluo";  // <-- 修改为你的 WiFi 密码
WebServer server(8080);
const char* configFile = "/config.json";

// 读取配置
String appKey = "";


// 使用 I2C 初始化，适配你的 OLED 屏幕
// U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C  u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C u8g2(U8G2_R0, /* reset=*/U8X8_PIN_NONE, /* SCL=*/5, /* SDA=*/4);


// 录音按钮D3
const int BUTTON_PIN = 18;
bool lastButtonState = HIGH;
bool currentButtonState = HIGH;


// 麦克风 INMP441 (I2S 输入)
#define I2S_MIC_BCLK 15
#define I2S_MIC_WS 16
#define I2S_MIC_SD 17

#define SAMPLE_RATE 16000
#define I2S_SAMPLE_BITS 16
#define RECORD_SECONDS 5  // 可设为 30
#define RECORD_BUFFER_SIZE (SAMPLE_RATE * RECORD_SECONDS)


// 扬声器
#define I2S_SPK_BCLK 12
#define I2S_SPK_WS 11
#define I2S_SPK_DIN 13

// 收音的部分参数
int16_t* recordedSamples = nullptr;
size_t totalBytesRecorded = 0;
bool isRecording = false;
unsigned long recordStartTime = 0;
size_t totalRead = 0;  // 全局，记录当前录音数据字节数



// 从 SPIFFS 加载配置
void loadConfig() {
  if (!SPIFFS.begin(true)) {
    sendMsg("", "SPIFFS 初始化失败");
    return;
  }
  File file = SPIFFS.open(configFile, "r");
  if (!file) {
    sendMsg("", "请先通过热点配置文件");
    return;
  }
  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, file);
  if (error) {
    sendMsg("", "配置文件异常!");
    file.close();
    return;
  }

  // String ssid = doc["ssid"] | "";
  // String pass = doc["pass"] | "";
  // appId = doc["appid"] | "";
  // apiKey = doc["apikey"] | "";
  // apiSecret = doc["apisecret"] | "";
  // ttsApiKey = doc["ttsapikey"] | "";
  // cityname = doc["city"] | "";
  // weatherapi = doc["api"] | "";

  // file.close();

  // if (ssid == "" || pass == "") {
  //   Serial.println("SSID 或密码为空，进入配网模式");
  //   handleWiFiConfig();
  //   return;
  // }

  // WiFi.begin(ssid.c_str(), pass.c_str());
  // Serial.print("正在连接WiFi");

  // unsigned long startAttemptTime = millis();
  // while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
  //   delay(500);
  //   Serial.print(".");
  // }

  // if (WiFi.status() == WL_CONNECTED) {
  //   Serial.println("\nWiFi连接成功，IP地址: " + WiFi.localIP().toString());
  // } else {
  //   Serial.println("\nWiFi连接失败，进入配网模式");
  //   handleWiFiConfig();
  // }
}

// 初始化按钮
void initButton() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);  // 使用内部上拉
  Serial.println("initButton finished");
}
// 初始化 I2S 麦克风
void initI2SMic() {
  i2s_config_t i2s_config = {
    .mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 512,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_MIC_BCLK,
    .ws_io_num = I2S_MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_MIC_SD
  };
  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);
  Serial.println("initI2SMic finished");
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
    .ws_io_num = I2S_SPK_WS,
    .data_out_num = I2S_SPK_DIN,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  i2s_driver_install(I2S_NUM_1, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_1, &pin_config);
  Serial.println("initI2SSpeaker finished");
}
// 初始化wifi
void initWifi() {
  server.on("/config", HTTP_POST, []() {
    String appkey = server.arg("appkey");
    Serial.println("handleConfig appkey:" + appkey);
    server.send(200, "text/plain", "OK");
    Serial.println("config loaded");
  });
  server.on("/", HTTP_GET, []() {
    if (SPIFFS.exists("/index.html")) {
      fs::File file = SPIFFS.open("/index.html", "r");
      if (file) {
        size_t fileSize = file.size();
        String fileContent;
        while (file.available()) {
          fileContent += (char)file.read();
        }
        file.close();
        server.send(200, "text/html", fileContent);
        Serial.println("index loaded");
        return;
      }
    }
    Serial.println("index.html file not exist");
    server.send(404, "text/plain", "File Not Found");
  });
  server.begin();
  Serial.println("HTTP 服务器已启动，监听端口 8080");
  Serial.println("initWifi finished");
}


void setup() {
  Serial.begin(115200);
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
  // 读取配置
  loadConfig();

  initWifi();
  initButton();
  initI2SMic();
  initI2SSpeaker();


  Serial.println("系统启动");
}






void playAudio() {

  // sendMsg("播放完成");
}

void initListener() {

  bool btnState = digitalRead(BUTTON_PIN) == LOW;  // 是否按下

  // 按下。并且之前没有开始录音的时候
  if (btnState && !isRecording) {
    // isRecording = true;
    // memset(recordedSamples, 0, RECORD_BUFFER_SIZE * sizeof(int16_t));
    // totalRead = 0;
    // totalBytesRecorded = 0;
    // sendMsg("录音中...");
    // Serial.println("录音开始");
  }

  // // 持续采样
  // if (isRecording) {

  //   size_t bytesRead = 0;

  //   while (digitalRead(BUTTON_PIN) == LOW && totalRead < RECORD_BUFFER_SIZE * 2) {
  //     i2s_read(I2S_NUM_0, recordedSamples + totalRead / 2, 1024, &bytesRead, portMAX_DELAY);
  //     totalRead += bytesRead;
  //   }
  //   isRecording = false;
  //   totalBytesRecorded = totalRead;  // 保存有效录音字节数
  //   sendMsg("录音完成，正在播放...");
  //   playAudio();
  // }
}

void loop() {
  server.handleClient();  // 必须调用以处理HTTP请求

  // initListener();
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

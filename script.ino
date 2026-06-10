#if defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <ESP8266HTTPClient.h>
#elif defined(ESP32)
  #include <WiFi.h>
  #include <HTTPClient.h>
  #include <WiFiClientSecure.h>
#endif

#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <string.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>

#include "DHT.h"
#include "images.h"

#define DHT_PIN 23
#define BUTTON_PIN 15
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C
#define LOGO_HEIGHT 64 
#define LOGO_WIDTH 128 

DHT dht(DHT_PIN, DHT22);
Adafruit_SSD1306 OLED(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Preferences pref;

const int MAX_BUFFER = 100;
const uint64_t TIME_TO_SLEEP = 300; // เวลาหลับ (หน่วยเป็นวินาที)

//เกี่ยวกับ wifi
const char* ssid = "...";
const char* password = "...";

//เกี่ยวกับเวลา
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 25200; // เวลาประเทศไทยคือ GMT+7
const int   daylightOffset_sec = 0;

//ip ของ computer 
const char* serverUrl = "...";
const char* roomID = "...";

// สร้างตัวแปรเก็บเวลาสำรองไว้บนหน่วยความจำสลบ (RTC Fast Memory)
// กำหนดค่าเริ่มต้นเป็น 315532800 (คือวันที่ 1 มกราคม 1980 เวลา 00:00:00 UTC) เป็นเลขที่เด่นชัดว่าเน็ตยังไม่เคยต่อติด
RTC_DATA_ATTR unsigned long last_known_time = 315532800; 

// ฟังก์ชันดึงเวลาปัจจุบันออกมาเป็นตัวเลขวินาที (Epoch Time)
unsigned long getEpochTime() {
  time_t now;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return 0; // ดึงเวลาไม่สำเร็จ
  }
  time(&now);
  return (unsigned long)(now + gmtOffset_sec);
}

bool postJson(float t, float h, int btn, unsigned long timestamp);
void saveToFlash(float t, float h, int btn, unsigned long timestamp, int currentCount);
void show(float tempC, float humi);
void finish_screen(int Is_success);

void setup() {
  Serial.begin(115200);
  delay(100); 

  pinMode(SCREEN_ADDRESS, INPUT_PULLUP); 
  delay(50); 

  if(!OLED.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed"));
    for(;;);
  }
  OLED.clearDisplay();
  OLED.setTextColor(WHITE);
  OLED.display();
  
  dht.begin();

  pref.begin("iot_store", false); 
  int bufferCount = pref.getInt("count", 0); 
  Serial.print("--- Boot Up. Current Stash in Flash: ");
  Serial.println(bufferCount);

  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  int isButtonPressed = (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) ? 1 : 0;

  // อ่านค่าเซนเซอร์ปัจจุบัน
  float current_t = dht.readTemperature();
  float current_h = dht.readHumidity();

  if (!isnan(current_h) && !isnan(current_t)) {
    if (isButtonPressed == 1) {
      show(current_t, current_h); 
    }
  }

  // เชื่อมต่อ Wi-Fi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  int connectionCounter = 0;
  while (WiFi.status() != WL_CONNECTED && connectionCounter < 20) {
    delay(500);
    Serial.print(".");
    connectionCounter++;
  }
  Serial.println("");
  
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  struct tm timeinfo;
  int retryTime = 0;
  while(!getLocalTime(&timeinfo) && retryTime < 10) {
    delay(500);
    Serial.print(".");
    retryTime++;
  }
  Serial.println("");

  // 🟢 บริหารจัดการลอจิกเวลาระบบสำรอง (Software RTC Fallback)
  unsigned long current_time = getEpochTime();
  
  if (current_time == 0) {
    // ❌ กรณีเชื่อมต่อเน็ตเพื่อดึงเวลาไม่ได้
    Serial.println("[!] NTP Sync Failed. Checking Backup Time...");
    
    // ถ้า last_known_time ยังคงเป็นค่าเริ่มต้น (315532800 = ปี 1980) แสดงว่าตั้งแต่เปิดเครื่องมายังไม่เคยเกาะเน็ตได้เลย
    if (last_known_time == 315532800) {
      Serial.println("[!] First boot and no internet. Setting time to 0.");
      current_time = 0; // ให้เป็น 0 ตามที่คุณต้องการ เพื่อประทับตราว่า "ไม่มีสัญญาเวลา"
    } else {
      // ถ้าเคยเกาะเน็ตได้มาบ้างแล้วในอดีต ให้ใช้ลอจิกจำลองเวลาเดิม
      current_time = last_known_time + TIME_TO_SLEEP;
      last_known_time = current_time; // อัปเดตตัวแปรในความจำ RTC
      Serial.print(">>> Simulated Time used: "); Serial.println(current_time);
    }
  } else {
    //  กรณีต่อเน็ตได้เวลาจริงสำเร็จ
    Serial.println("[+] NTP Sync Successful!");
    last_known_time = current_time; // อัปเดตเวลาจริงล็อกเข้าความจำ RTC
    Serial.print(">>> Real Time used: "); Serial.println(current_time);
  }

  if (!isnan(current_h) && !isnan(current_t)) {
    // ถ้าเน็ตต่อติด แต่อยู่ในสถานะเวลาเท่ากับ 0 ให้ข้ามการส่งไปเลยเพื่อเซฟระบบหลังบ้าน
    if (WiFi.status() == WL_CONNECTED && current_time != 0) {
      bool success = postJson(current_t, current_h, isButtonPressed, current_time);
      
      if (success) {
        if (isButtonPressed == 1) {
          finish_screen(1); 
        }
        
        if (bufferCount > 0) {
          Serial.println("--- Found data in Flash. Flushing now... ---");
          bool loopBroken = false;
          int i = 0;
          
          for (i = 0; i < bufferCount; i++) {
            String t_key = "t_" + String(i);
            String h_key = "h_" + String(i);
            String b_key = "b_" + String(i);

            float old_t = pref.getFloat(t_key.c_str(), 0.0);
            float old_h = pref.getFloat(h_key.c_str(), 0.0);
            int old_b = pref.getInt(b_key.c_str(), 0);
            unsigned long old_time = pref.getULong(("time_" + String(i)).c_str(), 0);

            Serial.print("Flushing Flash index ["); Serial.print(i); Serial.println("]");
            bool bufSuccess = postJson(old_t, old_h, old_b, old_time);
            
            if (!bufSuccess) {
              Serial.println("\n[!] Connection lost during flush. Keeping remaining data.");
              loopBroken = true;
              break; 
            }
            delay(100);
          }
          
          if (loopBroken) {
            int remaining = bufferCount - i;
            for (int j = 0; j < remaining; j++) {
              pref.putFloat(("t_" + String(j)).c_str(), pref.getFloat(("t_" + String(i + j)).c_str()));
              pref.putFloat(("h_" + String(j)).c_str(), pref.getFloat(("h_" + String(i + j)).c_str()));
              pref.putInt(("b_" + String(j)).c_str(), pref.getInt(("b_" + String(i + j)).c_str()));
              pref.putULong(("time_" + String(j)).c_str(), pref.getULong(("time_" + String(i + j)).c_str(), 0));
            }
            for (int k = remaining; k < bufferCount; k++) {
              pref.remove(("t_" + String(k)).c_str());
              pref.remove(("h_" + String(k)).c_str());
              pref.remove(("b_" + String(k)).c_str());
              pref.remove(("time_" + String(k)).c_str());
            }
            pref.putInt("count", remaining);
            bufferCount = remaining;
          } else {
            pref.clear(); 
            pref.putInt("count", 0); 
            bufferCount = 0;
            Serial.println("--- All Flash data flushed and cleared! ---");
          }
        }
      } else {
        saveToFlash(current_t, current_h, isButtonPressed, current_time, bufferCount);
        if (isButtonPressed == 1){ finish_screen(0); }
      }
    } else {
      // ถ้า Wi-Fi หลุดแต่แรก หรือต่อได้แต่ดันได้เวลาเป็น 0 ระบบจะตกมาอยู่ที่นี่ทั้งหมด
      saveToFlash(current_t, current_h, isButtonPressed, current_time, bufferCount);
      if (isButtonPressed == 1){ finish_screen(0); }
    }
  }

  pref.end(); 
  Serial.println("Going to deep sleep...");
  OLED.clearDisplay(); OLED.display();
  WiFi.disconnect(true);
  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * 1000000ULL);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, 0); 
  esp_deep_sleep_start();
}

void loop() {}

bool postJson(float t, float h, int btn, unsigned long timestamp) {
  WiFiClientSecure client;
  client.setInsecure(); 
  HTTPClient http;
  
  http.begin(client, serverUrl);
  http.addHeader("Content-Type", "application/json");

  http.setTimeout(5000);

  StaticJsonDocument<256> doc;
  doc["room_id"] = roomID;
  doc["temp"] = t;
  doc["humi"] = h;
  doc["button"] = btn;
  doc["timestamp"] = timestamp;

  String jsonString;
  serializeJson(doc, jsonString);

  int httpResponseCode = http.POST(jsonString);
  http.end();

  if (httpResponseCode == 200) {
    Serial.println(" -> Sent successfully! Response: 200");
    return true; 
  } else {
    Serial.print(" -> Send failed! Error: ");
    Serial.println(httpResponseCode);
    return false; 
  }
}

void saveToFlash(float t, float h, int btn, unsigned long timestamp, int currentCount) {
  if (timestamp == 0) {
    Serial.println("[!] Timestamp is 0. Skipping Flash storage to prevent corrupted data.");
    return; 
  }

  if (currentCount < MAX_BUFFER) {
    String t_key = "t_" + String(currentCount);
    String h_key = "h_" + String(currentCount);
    String b_key = "b_" + String(currentCount);
    String time_key = "time_" + String(currentCount);

    pref.putFloat(t_key.c_str(), t);
    pref.putFloat(h_key.c_str(), h);
    pref.putInt(b_key.c_str(), btn);
    pref.putULong(time_key.c_str(), timestamp);
    
    pref.putInt("count", currentCount + 1);
    Serial.println(">>> Saved to Flash memory with Timestamp!");
  } else {
    Serial.println("!!! Flash Buffer Full!");
  }
}

void show(float tempC, float humi){
  OLED.clearDisplay();
  OLED.drawBitmap(
    (OLED.width() - LOGO_WIDTH )/2,
    (OLED.height() - LOGO_HEIGHT)/2,
    upload_img, LOGO_WIDTH, LOGO_HEIGHT, 1);
  OLED.display();
  delay(1000);

  for(int i = 0; i < 3; i++){ 
    OLED.clearDisplay();
    OLED.setTextSize(1);
    OLED.drawBitmap(
      (OLED.width() - LOGO_WIDTH )/2,
      (OLED.height() - LOGO_HEIGHT)/2,
      data_img, LOGO_WIDTH, LOGO_HEIGHT, 1);
    OLED.setCursor(13, 35);
    OLED.print(tempC, 1);
    OLED.print(" C");

    OLED.setCursor(71, 35);
    OLED.print(humi, 1);
    OLED.print(" %");

    OLED.setCursor(25, 50);
    OLED.print("SENDING");
    OLED.display();
    delay(1000);
    for(int j = 0 ; j < 3; j++){
      delay(300);
      OLED.print(".");
      OLED.display();
    }
    delay(500);
  }
}

void finish_screen(int Is_success){ 
  OLED.clearDisplay();
  if(Is_success){
    OLED.drawBitmap(
      (OLED.width() - LOGO_WIDTH )/2,
      (OLED.height() - LOGO_HEIGHT)/2,
      correct_img, LOGO_WIDTH, LOGO_HEIGHT, 1);
    OLED.setCursor(10, 50);
    OLED.print("Sent Successfully");
  }
  else{
    OLED.drawBitmap(
      (OLED.width() - LOGO_WIDTH )/2,
      (OLED.height() - LOGO_HEIGHT)/2,
      incorrect_img, LOGO_WIDTH, LOGO_HEIGHT, 1);
    OLED.setCursor(20, 50);
    OLED.print("Failed to Send");
  }
  OLED.display();
  delay(1500);
  
  OLED.clearDisplay();
  OLED.display();
}
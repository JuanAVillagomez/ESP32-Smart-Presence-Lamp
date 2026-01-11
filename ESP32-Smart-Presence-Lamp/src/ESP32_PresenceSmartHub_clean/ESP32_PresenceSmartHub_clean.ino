/*
 * Project: IoT Smart Presence & Notification Lamp
 * Platform: ESP32 (Dual Core)
 * Author: Juan A. Villagomez
 * Date: 1/10/2026
 * * Description: 
 * A WiFi-enabled ambient lamp that synchronizes with mobile geofencing, 
 * real-time weather APIs, and special date events. Uses MQTT for 
 * cloud messaging and WebSockets for low-latency local control.
 */

#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h> 
#include <AsyncTCP.h>          
#include <time.h>
#include <ArduinoOTA.h>
#include <Adafruit_MQTT.h>
#include <Adafruit_MQTT_Client.h>
#include <ArduinoJson.h> 
#include <HTTPClient.h>
#include "FS.h" 
#include "LittleFS.h" 
#include "secrets.h" // Credentials file

// --- Hardware Configuration ---
#define LED_PIN     18
#define NUM_LEDS    19
#define BUTTON_PIN  4

// --- Networking Configuration ---
#define AIO_SERVER      "io.adafruit.com"
#define AIO_SERVERPORT  1883 

// --- Constants ---
const unsigned long WEATHER_CHECK_INTERVAL = 900000; // 15 minutes
const unsigned long DATE_CHECK_INTERVAL = 60000;     // 1 minute
const int DAY_START_HOUR = 7;
const int NIGHT_START_HOUR = 21;

// --- State Enums ---
enum Effect {
  EFFECT_BREATH,
  EFFECT_FAVORITE,
  EFFECT_NIGHT,
  EFFECT_WEATHER,
  EFFECT_STATIC,
  EFFECT_PRIVATE_PULSE
};

enum GeoState {
  GEO_NONE, 
  GEO_ARRIVED,
  GEO_SETTLED,
  GEO_LEAVE,
  GEO_DRIVING
};

// --- Global Objects ---
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
WiFiClient client;
Adafruit_MQTT_Client mqtt(&client, AIO_SERVER, AIO_SERVERPORT, AIO_USERNAME, AIO_KEY);
Adafruit_MQTT_Subscribe geofenceFeed = Adafruit_MQTT_Subscribe(&mqtt, AIO_USERNAME "/feeds/geofence");
AsyncWebServer server(80); 
AsyncWebSocket ws("/ws"); 

// --- State Variables ---
Effect currentEffect = EFFECT_BREATH;
GeoState currentGeoState = GEO_NONE;
String currentWeatherCondition = "Unknown";
float currentTemperature = 0.0;

// --- Timing & Animation Variables ---
unsigned long lastWeatherCheck = 0;
unsigned long lastDateCheck = 0;
unsigned long lastAnimMillis = 0; 
unsigned long currentStateStart = 0;
unsigned long lastColorUpdate = 0;
unsigned long lastFavColorUpdate = 0;
unsigned long lastNightModeUpdate = 0;
unsigned long lastPulseUpdate = 0;
unsigned long lastArriveUpdate = 0;
unsigned long lastLeaveUpdate = 0;
unsigned long lastDrivingUpdate = 0;
unsigned long lastWeatherAnimUpdate = 0;

int animPos = 0; 
int animBrightness = 0; 
int animStep = 8;
int pulseStep = 0;
int brightnessStep = 5;
int brightness = 50; 
int stateDuration = 0;
int lastButtonState = HIGH;
int effectDelay = 30;
uint8_t staticColorBrightness = 255; 

// --- Special Dates Configuration ---
struct SpecialEvent {
  int month; 
  int day; 
  const char* name;
};

SpecialEvent dates[] = {
  {1, 1,   "NewYear"},
  {2, 14,  "Valentines"},
  {2, 22,  "HerBirthday"},
  {5, 3,   "HisBirthday"},
  {8, 7,   "Anniversary"},
  {12, 25, "Christmas"},
};
const int numDates = sizeof(dates)/sizeof(dates[0]);
int lastPlayedDay[numDates]; 
bool dateActive = false;
int activeEventIndex = -1;

// ==========================================
//           HELPER FUNCTIONS
// ==========================================

// --- Time Helper ---
bool getLocalTimeChecked(struct tm &timeinfo, unsigned long timeoutMs = 3000) { 
  unsigned long start = millis(); 
  while (!getLocalTime(&timeinfo)){ 
    if (millis() - start > timeoutMs) return false; 
    delay(50); 
  } 
  return true; 
}

int getCurrentHour() {
  struct tm timeinfo;
  if (getLocalTimeChecked(timeinfo)) {
    return timeinfo.tm_hour; 
  }
  return -1; 
}

bool isDayTime() {
  int hour = getCurrentHour();
  if (hour == -1) return true; 
  return (hour >= DAY_START_HOUR && hour < NIGHT_START_HOUR);
}

// --- JSON State Helper ---
String getCurrentStateJson() {
  StaticJsonDocument<200> stateDoc;
  String modeName = "Unknown";
  
  switch (currentEffect) {
    case EFFECT_BREATH:       modeName = "breath"; break;
    case EFFECT_FAVORITE:     modeName = "favorite"; break;
    case EFFECT_NIGHT:        modeName = "night"; break;
    case EFFECT_WEATHER:      modeName = "weather"; break;
    case EFFECT_STATIC:       modeName = "static"; break; 
    case EFFECT_PRIVATE_PULSE:modeName = "pulse"; break;
  }
  stateDoc["mode"] = modeName;

  if (currentEffect == EFFECT_BREATH) stateDoc["brightness"] = brightness;
  else if (currentEffect == EFFECT_STATIC) stateDoc["brightness"] = staticColorBrightness;
  else stateDoc["brightness"] = strip.getBrightness();

  if (currentEffect == EFFECT_STATIC) {
    uint32_t color = strip.getPixelColor(0); 
    char hexColor[8];
    snprintf(hexColor, sizeof(hexColor), "#%06X", (color & 0x00FFFFFF)); 
    stateDoc["color"] = hexColor;
  }

  if (currentEffect == EFFECT_WEATHER) {
    stateDoc["weather"] = currentWeatherCondition;
  }

  String output;
  serializeJson(stateDoc, output);
  return output;
}

void broadcastState() {
  String stateJson = getCurrentStateJson();
  // Serial.print("Broadcasting: "); Serial.println(stateJson); // Debug
  ws.textAll(stateJson);
}

// ==========================================
//           NETWORK HANDLERS
// ==========================================

void MQTT_connect() {
  int8_t ret;
  if (mqtt.connected()) return;

  // Serial.print("Connecting to MQTT... ");
  uint8_t retries = 3;
  while ((ret = mqtt.connect()) != 0) { 
       mqtt.disconnect();
       delay(5000);  
       retries--;
       if (retries == 0) while (1); // Halt and wait for WDT
  }
  // Serial.println("MQTT Connected!");
}

void onWsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len) {
  if(type == WS_EVT_CONNECT){
    client->text(getCurrentStateJson());
  } else if(type == WS_EVT_DATA){
    AwsFrameInfo * info = (AwsFrameInfo*)arg;
    if(info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT){
      data[len] = 0; 
      DynamicJsonDocument doc(256); 
      DeserializationError error = deserializeJson(doc, (char*)data);
      if (error) return;

      const char* action = doc["action"]; 
      if (action) {
         String actionStr = String(action);
         actionStr.toLowerCase(); 

         if (actionStr == "setcolor") {
             const char* colorVal = doc["value"]; 
             if (colorVal && strlen(colorVal) == 7 && colorVal[0] == '#') {
                  long number = strtol(&colorVal[1], NULL, 16); 
                  int r = (number >> 16) & 0xFF; 
                  int g = (number >> 8) & 0xFF;  
                  int b = number & 0xFF;         
                  currentEffect = EFFECT_STATIC; 
                  strip.fill(strip.Color(r, g, b));
                  strip.show();
                  broadcastState();
             } else if (colorVal && strcmp(colorVal, "off") == 0) {
                  currentEffect = EFFECT_STATIC; 
                  strip.clear();
                  strip.show();
                  broadcastState();
             }
         } else if (actionStr == "setbrightness") {
             if (doc.containsKey("value") && doc["value"].is<int>()) {
                 int brightnessVal = constrain(doc["value"].as<int>(), 0, 255);
                 staticColorBrightness = (uint8_t)brightnessVal;
                 if (currentEffect == EFFECT_STATIC) {
                    uint32_t currentColor = strip.getPixelColor(0); 
                    strip.setBrightness(staticColorBrightness);
                    strip.fill(currentColor); 
                    strip.show();
                    strip.setBrightness(255); // Reset global
                 }
                broadcastState();
             }
         } else if (actionStr == "setmode") {
             const char* modeVal = doc["value"]; 
             if (modeVal) {
                 String modeStr = String(modeVal);
                 modeStr.toLowerCase(); 
                 Effect newEffect = currentEffect; 
                 if (modeStr == "breath") newEffect = EFFECT_BREATH;
                 else if (modeStr == "favorite") newEffect = EFFECT_FAVORITE;
                 else if (modeStr == "night") newEffect = EFFECT_NIGHT;
                 else if (modeStr == "weather") newEffect = EFFECT_WEATHER;
                 
                 if (newEffect != currentEffect) {
                     currentEffect = newEffect;
                     if (currentEffect == EFFECT_BREATH) {
                         brightness = 0;
                         brightnessStep = 5;
                     }
                     strip.clear(); 
                     strip.show();
                     broadcastState();
                 }
             }
         } else if (actionStr == "getstate") {
             client->text(getCurrentStateJson());
         } else if (actionStr == "privatepulse"){
             const char* secretVal = doc["secret"]; 
             if (secretVal && strcmp(secretVal, PRIVATE_PULSE_CODE) == 0) {
                currentEffect = EFFECT_PRIVATE_PULSE;
                pulseStep = 0; 
                broadcastState();
             } 
         }
      }
    }
  }
}

// ==========================================
//           CORE LOGIC
// ==========================================

void updateWeather() {
  unsigned long now = millis();
  if (now - lastWeatherCheck >= WEATHER_CHECK_INTERVAL || lastWeatherCheck == 0) {
    lastWeatherCheck = now; 

    if (WiFi.status() == WL_CONNECTED) {
      HTTPClient http;
      String cityEncoded = WEATHER_CITY;
      cityEncoded.replace(" ", "%20");
      String serverPath = "http://api.openweathermap.org/data/2.5/weather?q=" + cityEncoded + "," + WEATHER_COUNTRY + "&APPID=" + WEATHER_API_KEY + "&units=imperial";
      
      http.begin(serverPath.c_str());
      http.addHeader("User-Agent", "ESP32_IoT_Lamp");
      int httpResponseCode = http.GET();

      if (httpResponseCode > 0) {
        String payload = http.getString();
        DynamicJsonDocument jsonDoc(1024);
        DeserializationError error = deserializeJson(jsonDoc, payload);
        
        if(!error) {
            const char* mainCondition = jsonDoc["weather"][0]["main"]; 
            if (mainCondition) currentWeatherCondition = String(mainCondition);
            if (jsonDoc["main"].containsKey("temp")) currentTemperature = jsonDoc["main"]["temp"].as<float>();
        } else {
            currentWeatherCondition = "Error";
        }
      } else {
        currentWeatherCondition = "Error";
      }
      http.end();
    } else {
      currentWeatherCondition = "Offline"; 
    }
  }
}

void startSpecialEvent(int index){ 
  if (index < 0 || index >= numDates) return; 
  dateActive = true; 
  activeEventIndex = index; 
  lastAnimMillis = millis(); 
  animPos = 0; 
  animBrightness = 0; 
  animStep = 8; 
  broadcastState();
}

void checkStartDate() {
  struct tm timeinfo;
  if (!getLocalTimeChecked(timeinfo)) return;

  int currentMonth = timeinfo.tm_mon + 1;
  int currentDay = timeinfo.tm_mday;
  bool matchingDayFound = false;
  int matchingIndex = -1;

  for (int i = 0; i < numDates; i++) {
    if (currentMonth == dates[i].month && currentDay == dates[i].day) {
      matchingDayFound = true;
      matchingIndex = i;
      break; 
    }
  }

  if (matchingDayFound) {
    if (dateActive && activeEventIndex == matchingIndex) return;
    else if (lastPlayedDay[matchingIndex] != currentDay) {
      startSpecialEvent(matchingIndex);
      lastPlayedDay[matchingIndex] = currentDay;
    }
  } else {
    if (dateActive) {
      dateActive = false;
      activeEventIndex = -1;
      strip.clear();
      strip.show();
      broadcastState();    
      }
  }
}

// ==========================================
//           ANIMATION EFFECTS
// ==========================================

void fadeStripToBlackBy(Adafruit_NeoPixel &stripRef, byte fadeAmount) {
  for (int i = 0; i < stripRef.numPixels(); i++) {
    uint32_t prevColor = stripRef.getPixelColor(i);
    uint8_t r = (prevColor >> 16) & 0xFF;
    uint8_t g = (prevColor >> 8) & 0xFF;
    uint8_t b = prevColor & 0xFF;
    r = (r * (256 - fadeAmount)) >> 8; 
    g = (g * (256 - fadeAmount)) >> 8;
    b = (b * (256 - fadeAmount)) >> 8;
    stripRef.setPixelColor(i, stripRef.Color(r, g, b));
  }
}

void setTempIndicator(float temperature) {
  uint32_t color;
  if (temperature <= 40.0) color = strip.Color(0, 0, 150);
  else if (temperature <= 70.0) color = strip.Color(0, 150, 150);
  else if (temperature <= 85.0) color = strip.Color(150, 150, 0);
  else color = strip.Color(200, 50, 0);
  for (int i = NUM_LEDS - 4; i < NUM_LEDS; i++) strip.setPixelColor(i, color);
}

void runWeatherMode() {
  unsigned long now = millis();
  if (now - lastWeatherAnimUpdate < 100) return;
  lastWeatherAnimUpdate = now;

  bool isDay = isDayTime(); 
  currentWeatherCondition.toLowerCase(); 
  const int ANIMATION_LEDS = NUM_LEDS - 4; 

  if (currentWeatherCondition == "clear") {
    uint32_t baseColor = isDay ? strip.Color(0, 50, 200) : strip.Color(0, 0, 80); 
    uint32_t sparkleColor = isDay ? strip.Color(255, 200, 0) : strip.Color(100, 100, 0); 
    for (int i = 0; i < ANIMATION_LEDS; i++) strip.setPixelColor(i, baseColor);
    
    if (random(5) == 0) {
      int pos = random(ANIMATION_LEDS);
      strip.setPixelColor(pos, sparkleColor);
      if (pos > 0) strip.setPixelColor(pos - 1, strip.Color(30, 30, 30));
    }
    
    // Decay Logic
    for (int i = 0; i < ANIMATION_LEDS; i++) {
      uint32_t c = strip.getPixelColor(i);
      uint8_t r = (c >> 16) & 0xFF; uint8_t g = (c >> 8) & 0xFF; uint8_t b = c & 0xFF;
      r = (r * 245) >> 8; g = (g * 245) >> 8; b = (b * 240) >> 8; 
      strip.setPixelColor(i, strip.Color(r, g, b));
    }
    strip.setBrightness(isDay ? 255 : 150);

  } else if (currentWeatherCondition == "clouds") {
    fadeStripToBlackBy(strip, 5); 
    for(int k=0; k<3; k++) {
      int pos = random(ANIMATION_LEDS); 
      uint32_t current = strip.getPixelColor(pos);
      uint8_t r = (current >> 16) & 0xFF; uint8_t g = (current >> 8) & 0xFF; uint8_t b = current & 0xFF;
      int addR = isDay ? 20 : 5; int addG = isDay ? 20 : 5; int addB = isDay ? 20 : 10;
      r = (r + addR > 255) ? 255 : r + addR; g = (g + addG > 255) ? 255 : g + addG; b = (b + addB > 255) ? 255 : b + addB;
      strip.setPixelColor(pos, strip.Color(r, g, b));
    }
    uint32_t baseColor = isDay ? strip.Color(50, 50, 50) : strip.Color(10, 10, 20);
    for(int i=0; i<ANIMATION_LEDS; i++) {
      if (strip.getPixelColor(i) == 0) strip.setPixelColor(i, baseColor);
    }
    strip.setBrightness(isDay ? 200 : 120);

  } else if (currentWeatherCondition == "rain" || currentWeatherCondition == "drizzle") {
    uint8_t fadeAmount = isDay ? 40 : 20; 
    uint32_t rainColor = isDay ? strip.Color(0, 50, 200) : strip.Color(0, 0, 100); 
    fadeStripToBlackBy(strip, fadeAmount);
    int pos = random(ANIMATION_LEDS); 
    strip.setPixelColor(pos, rainColor); 
    strip.setBrightness(isDay ? 200 : 120);

  } else if (currentWeatherCondition == "thunderstorm") {
    uint32_t bgColor = isDay ? strip.Color(20, 20, 40) : strip.Color(0, 0, 10);
    if (random(20) == 0) { 
        strip.fill(strip.Color(255, 255, 255), 0, ANIMATION_LEDS); 
        strip.setBrightness(255); strip.show(); delay(50); 
        strip.fill(bgColor, 0, ANIMATION_LEDS); 
    } else {
        strip.fill(bgColor, 0, ANIMATION_LEDS); 
        strip.setBrightness(isDay ? 150 : 80);
    }
  } else if (currentWeatherCondition == "snow") {
    uint8_t fadeAmount = isDay ? 30 : 15;
    uint32_t snowColor = isDay ? strip.Color(200, 200, 200) : strip.Color(80, 80, 100);
    fadeStripToBlackBy(strip, fadeAmount); 
    int pos = random(ANIMATION_LEDS);
    strip.setPixelColor(pos, snowColor); 
    strip.setBrightness(isDay ? 200 : 150);
  } else { 
    uint32_t defaultColor = isDay ? strip.Color(50, 50, 50) : strip.Color(10, 10, 10); 
    strip.fill(defaultColor, 0, ANIMATION_LEDS); 
    strip.setBrightness(isDay ? 100 : 30);
  }
  setTempIndicator(currentTemperature);
  strip.show();
}

void favColor() {
  strip.setBrightness(255);
  unsigned long currentMillis = millis();
  const int flowDelay = 30; 
  const int pulseSpeed = 10; 

  if (currentMillis - lastColorUpdate >= pulseSpeed) {
    lastColorUpdate = currentMillis;
    brightness += brightnessStep;
    if (brightness >= 255) { brightness = 255; brightnessStep = -abs(brightnessStep); }
    else if (brightness <= 50) { brightness = 50; brightnessStep = abs(brightnessStep); }
  }

  if (currentMillis - lastFavColorUpdate >= flowDelay) {
    lastFavColorUpdate = currentMillis;
    const uint16_t HUE_MIN = 30000; 
    const uint16_t HUE_MAX = 60000; 
    const uint16_t HUE_RANGE = HUE_MAX - HUE_MIN; 
    
    for (int i = 0; i < NUM_LEDS; i++) {
      uint32_t totalOffset = (animPos * 700) + (i * 1000);
      uint16_t restrictedOffset = (totalOffset % 65536) * HUE_RANGE / 65536;
      uint16_t hue = HUE_MIN + restrictedOffset; 
      strip.setPixelColor(i, strip.ColorHSV(hue, 155, (uint8_t)brightness));
    }
    strip.show();
    animPos++; 
    strip.setBrightness(255); 
  }
}

void privatePulse() {
  if (millis() - lastPulseUpdate > 100) {
    lastPulseUpdate = millis();
    int phase = pulseStep % (NUM_LEDS * 2);
    if (phase < NUM_LEDS) strip.setPixelColor(phase, strip.Color(255, 105, 180));
    else strip.setPixelColor((NUM_LEDS * 2 - 1) - phase, strip.Color(255, 0, 0));
    strip.show();
    pulseStep++;
  }
}

void runArrive() {
  unsigned long currentMillis = millis();
  if (currentMillis - lastArriveUpdate >= effectDelay) {
    lastArriveUpdate = currentMillis;
    brightness += brightnessStep;
    if (brightness <= 0 || brightness >= 255) brightnessStep = -brightnessStep;
    uint32_t color = strip.Color(0, brightness, 0);
    for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, color);
    strip.show();
  }
}
void runSettled() {
  uint32_t warmWhite = strip.Color(127, 100, 64);
  for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, warmWhite);
  strip.show();
}
void runLeave() {
  unsigned long currentMillis = millis();
  if (currentMillis - lastLeaveUpdate >= effectDelay) {
    lastLeaveUpdate = currentMillis;
    brightness += brightnessStep;
    if (brightness <= 0 || brightness >= 255) brightnessStep = -brightnessStep;
    uint32_t color = strip.Color(0, 0, brightness);
    for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, color);
    strip.show();
  }
}
void runDriving() {
  unsigned long currentMillis = millis();
  if (currentMillis - lastDrivingUpdate >= 600) { 
    lastDrivingUpdate = currentMillis;
    static bool on = false; on = !on;
    uint32_t color = on ? strip.Color(255, 255, 0) : strip.Color(0, 0, 0);
    for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, color);
    strip.show();
  }
}
void runGenerativeSparkle() {
  unsigned long currentMillis = millis();
  int effectDelay = 40; 
  if (currentMillis - lastFavColorUpdate < effectDelay) return;
  lastFavColorUpdate = currentMillis;
  fadeStripToBlackBy(strip, 20);
  if (random(10) == 0) {
    int pos = random(NUM_LEDS);
    uint16_t randomHue = random(65535);
    strip.setPixelColor(pos, strip.ColorHSV(randomHue, 255, 255));
  }
  strip.show();
}
void runNightMode(){
  unsigned long currentMillis = millis();
  if (currentMillis - lastNightModeUpdate >= effectDelay) {
    lastNightModeUpdate = currentMillis;
    strip.fill(strip.Color(100,70,20),0,NUM_LEDS);
    strip.setBrightness(30);
    strip.show();
  }
}

// --- Date Events ---
void animChristmas() { 
  unsigned long now = millis(); 
  if (now - lastAnimMillis < 120) return; 
  lastAnimMillis = now;
  for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, strip.Color(160, 0, 0)); 
  strip.setPixelColor(animPos % NUM_LEDS, strip.Color(0, 160, 0)); 
  strip.show(); animPos++; 
} 
void animAnni() { 
  unsigned long now = millis(); 
  if (now - lastAnimMillis < 40) return; 
  lastAnimMillis = now; 
  animBrightness += animStep; 
  if (animBrightness <= 0 || animBrightness >= 255) animStep = -animStep; 
  uint8_t b = (uint8_t)animBrightness; 
  for (int i = 0; i < NUM_LEDS; i++) { 
    uint8_t r = (uint8_t)((255 * b) / 255); 
    uint8_t g = (uint8_t)((80 * b) / 255); 
    uint8_t bl= (uint8_t)((150 * b) / 255); 
    strip.setPixelColor(i, strip.Color(r, g, bl)); 
  } strip.show(); 
} 
void animValentines() { 
  unsigned long now = millis(); 
  if (now - lastAnimMillis < 100) return; 
  lastAnimMillis = now;
  int p = animPos % NUM_LEDS; 
  strip.clear(); 
  strip.setPixelColor(p, strip.Color(255, 105, 180)); 
  for (int i = 0; i < NUM_LEDS; i++) if (i != p) strip.setPixelColor(i, strip.Color(255, 0, 0)); 
  strip.show(); animPos++; 
} 
void animIndependence() { 
  unsigned long now = millis(); 
  if (now - lastAnimMillis < 200) return; 
  lastAnimMillis = now; 
  int cycle = animPos % 3; 
  if (cycle == 0) strip.fill(strip.Color(255,0,0), 0, NUM_LEDS); 
  else if (cycle == 1) strip.fill(strip.Color(255,255,255), 0, NUM_LEDS); 
  else strip.fill(strip.Color(0,0,255), 0, NUM_LEDS); 
  strip.show(); animPos++; 
} 
void animHerBirthday() { 
  unsigned long now = millis(); 
  if (now - lastAnimMillis < 100) return; 
  lastAnimMillis = now;
  int p = animPos % NUM_LEDS; 
  strip.clear(); 
  strip.setPixelColor(p, strip.Color(255, 105, 180)); 
  for (int i = 0; i < NUM_LEDS; i++) if (i != p) strip.setPixelColor(i, strip.Color(40, 0, 80)); 
  strip.show(); animPos++; 
} 
void animHisBirthday() { 
  unsigned long now = millis(); 
  if (now - lastAnimMillis < 100) return; 
  lastAnimMillis = now;
  int p = animPos % NUM_LEDS; 
  strip.clear(); 
  strip.setPixelColor(p, strip.Color(150, 150, 150)); 
  for (int i = 0; i < NUM_LEDS; i++) if (i != p) strip.setPixelColor(i, strip.Color(0, 0, 255)); 
  strip.show(); animPos++; 
} 
void runActiveSpecialAnimation() { 
  if (!dateActive || activeEventIndex < 0) return; 
  const char* name = dates[activeEventIndex].name; 
  if (strcmp(name, "Christmas") == 0) animChristmas(); 
  else if (strcmp(name, "Valentines") == 0) animValentines(); 
  else if (strcmp(name, "NewYear") == 0) animIndependence(); 
  else if (strcmp(name, "HerBirthday") == 0) animHerBirthday(); 
  else if (strcmp(name, "HisBirthday") == 0) animHisBirthday(); 
  else if (strcmp(name, "Anniversary") == 0) animAnni(); 
  else animChristmas(); 
}

// ==========================================
//           SETUP & LOOP
// ==========================================

void setup() {
  strip.begin();
  strip.show();
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Serial.begin(115200);
  delay(100);

  if(!LittleFS.begin(true)){ 
    Serial.println("Mount Fail");
    return;
  }

  for (int i = 0; i < numDates; i++) lastPlayedDay[i] = -1;

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while(WiFi.status() != WL_CONNECTED){ delay(250); } 
  
  if(WiFi.status() == WL_CONNECTED){
    ws.onEvent(onWsEvent); 
    server.addHandler(&ws); 
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
       request->send(LittleFS, "/index.html", "text/html");
    });
    server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request){
       request->send(LittleFS, "/style.css", "text/css");
    });
    server.on("/script.js", HTTP_GET, [](AsyncWebServerRequest *request){
       request->send(LittleFS, "/script.js", "text/javascript"); 
    });
    server.onNotFound([](AsyncWebServerRequest *request){
       request->send(404, "text/plain", "Not found");
    });
    server.begin(); 
  }

  ArduinoOTA.setHostname("ESP32_LightController");
  ArduinoOTA.setPassword("esp");
  ArduinoOTA.begin();

  setenv("TZ", "CST6CDT", 1); 
  tzset();  
  configTime(0, 0, "pool.ntp.org"); 

  mqtt.subscribe(&geofenceFeed);
  checkStartDate();
}

void loop() {
  MQTT_connect(); 

  // --- WiFi Watchdog ---
  static unsigned long lastWifiCheck = 0;
  if (millis() - lastWifiCheck > 30000) { 
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.disconnect();
      WiFi.reconnect();
    }
  }

  // --- MQTT Incoming ---
  Adafruit_MQTT_Subscribe *subscription;
  while ((subscription = mqtt.readSubscription(10))) { 
    if (subscription == &geofenceFeed) {
      String message = String((char *)geofenceFeed.lastread);
      message.toLowerCase(); 

      if (message == "arrive") {
        currentGeoState = GEO_ARRIVED;
        currentStateStart = millis();
        stateDuration = 15000; 
        brightness = 0; brightnessStep = 5;
      } else if (message == "settled") {
        currentGeoState = GEO_SETTLED;
        currentStateStart = millis();
        stateDuration = 10000; 
      } else if (message == "leave") {
        currentGeoState = GEO_LEAVE;
        currentStateStart = millis();
        stateDuration = 8000; 
        brightness = 0; brightnessStep = 5;
      } else if (message == "driving") {
        currentGeoState = GEO_DRIVING;
        currentStateStart = millis();
        stateDuration = 10000; 
       } else if (message == "pulse" || message == "missyou"){
          currentEffect = EFFECT_PRIVATE_PULSE;
          pulseStep = 0;
          broadcastState();
       }
      broadcastState(); 
    }
  }

  ws.cleanupClients();
  ArduinoOTA.handle();   

  // --- Button Logic ---
  int currentButtonState = digitalRead(BUTTON_PIN);
  if (lastButtonState == HIGH && currentButtonState == LOW) {
    if(dateActive){
      if (activeEventIndex >= 0 && activeEventIndex < numDates) {
        lastPlayedDay[activeEventIndex] = dates[activeEventIndex].day; 
      }
      dateActive = false;
      activeEventIndex = -1;
      strip.clear();
      strip.setBrightness(255); 
      strip.show();
      broadcastState();
    } 
    else if (currentEffect == EFFECT_PRIVATE_PULSE) {
      currentEffect = EFFECT_BREATH;
      brightness = 60; brightnessStep = 5;
      strip.setBrightness(255);
      strip.clear(); strip.show();
      broadcastState();
    } else {
      currentEffect = static_cast<Effect>((currentEffect + 1) % 4);
      if (currentEffect == EFFECT_BREATH) { 
        brightness = 60; brightnessStep = 5;
        strip.setBrightness(255);
      } 
      else if (currentEffect == EFFECT_WEATHER) updateWeather();
      strip.clear(); strip.show();
      broadcastState();
    }
  }
  lastButtonState = currentButtonState;

  if(millis() - lastDateCheck >= DATE_CHECK_INTERVAL){
    lastDateCheck = millis();
    checkStartDate();
    updateWeather();
  }
  
  if (dateActive) { runActiveSpecialAnimation(); return; }

  // --- Geo Override ---
  if(currentGeoState != GEO_NONE){
    switch (currentGeoState) {
      case GEO_ARRIVED: runArrive(); break;
      case GEO_SETTLED: runSettled(); break;
      case GEO_LEAVE:   runLeave(); break;
      case GEO_DRIVING: runDriving(); break;
    }
    if (millis() - currentStateStart >= stateDuration) {
      currentGeoState = GEO_NONE;
      strip.clear(); strip.show();
      broadcastState();
    }
    return; 
  }

  if (currentEffect == EFFECT_PRIVATE_PULSE) { privatePulse(); return; }

  // --- Main Effects Loop ---
  switch (currentEffect) {
    case EFFECT_BREATH:   favColor(); break;
    case EFFECT_FAVORITE: runGenerativeSparkle(); break;
    case EFFECT_NIGHT:    runNightMode(); break;
    case EFFECT_WEATHER:  runWeatherMode(); break;
  }
  delayMicroseconds(500);
}
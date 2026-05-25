/*
  ESP32 SMART DOOR LOCK v4.1 -- AEGIS DASHBOARD EDITION (with FP enrollment animation)
  ================================================================================
  ... (all original feature descriptions remain the same)
  FIXED: Fingerprint enrollment conflicts caused by concurrent sensor access.
*/

#include <WiFi.h>
#include <WiFiAP.h>
#include <WiFiUdp.h>
#include <time.h>
#include <WebServer.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Adafruit_Fingerprint.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <ESP32Time.h>
#include <ArduinoOTA.h>

// ==================== CONFIGURATION ====================
const char* MY_SSID    = "P-TECH";
const char* MY_PASS    = "wirewizard";
const char* ADMIN_USER = "admin";
const char* ADMIN_PASS = "admin";

const char* AP_SSID    = "AEGIS-LOCK";
const char* AP_PASS    = "aegis1234";
const char* OTA_PASS   = "aegisota";

#define RELAY_ACTIVE_STATE HIGH
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT  64
#define OLED_ADDR      0x3C
#define SS_PIN         5
#define RST_PIN        27
#define FP_RX_PIN      16
#define FP_TX_PIN      17
#define FP_WAKEUP_PIN  34
#define FP_USE_WAKEUP  false
#define FP_BAUD        57600
#define RELAY_PIN      26
#define BUZZER_PIN     25
#define LED_GREEN      12
#define LED_RED        14
#define RELAY_UNLOCK   RELAY_ACTIVE_STATE
#define RELAY_LOCK     (RELAY_ACTIVE_STATE == HIGH ? LOW : HIGH)
#define UNLOCK_MS      2000UL
#define COOLDOWN_MS    6000UL
#define FP_POLL_MS     200UL
#define WIFI_CHECK_MS  30000UL
#define WIFI_RETRY_MS  60000UL
#define BUZZER_RES     8
#define BUTTON_PIN     32
#define BUTTON_DEBOUNCE_MS 50
#define LOG_STORE_SIZE 50
#define SESSION_TIMEOUT_MS 3600000UL
#define RFID_WEB_SCAN_TIMEOUT 15000
#define RFID_FAIL_LIMIT 5

// ==================== MUSICAL NOTES ====================
#define NOTE_C4  262
#define NOTE_D4  294
#define NOTE_E4  330
#define NOTE_F4  349
#define NOTE_G4  392
#define NOTE_A4  440
#define NOTE_AS4 466
#define NOTE_B4  494
#define NOTE_C5  523
#define NOTE_D5  587
#define NOTE_E5  659
#define NOTE_F5  698
#define NOTE_G5  784
#define NOTE_A5  880
#define NOTE_B5  988
#define NOTE_C6  1047
#define NOTE_REST 0

// ==================== GLOBAL OBJECTS ====================
Adafruit_SH1106G     display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
MFRC522              rfid(SS_PIN, RST_PIN);
HardwareSerial       fpSerial(2);
Adafruit_Fingerprint finger(&fpSerial);
WebServer            server(80);
Preferences          prefs;
ESP32Time            rtc(0);

// ==================== NETWORK STATE ====================
int           wifiMode       = 0;
unsigned long lastWifiCheck  = 0;
unsigned long lastApRetry    = 0;

// ==================== SYSTEM STATE ====================
String        lastCardUID        = "";
unsigned long lastCardTime       = 0;
unsigned long lastFpCheck        = 0;
bool          doorLocked         = true;
bool          fpAvailable        = false;
int           lastFpCount        = 0;          // cached fingerprint count
bool          enrollPending      = false;
int           enrollStage        = 0;
unsigned long enrollStageTime    = 0;
unsigned long enrollCompletionTime = 0;
String        enrollResult       = "";
unsigned long accessToday        = 0;
unsigned long failsToday         = 0;
String        enrollName         = "";
String        currentToken       = "";
unsigned long sessionTimeout     = 0;
bool          rfidWebScanActive  = false;
String        rfidWebScanUID     = "";
unsigned long rfidWebScanTimeout = 0;
unsigned long lastStatsFlush     = 0;
int           rfidFailCount      = 0;
bool          lastButtonReading  = HIGH;
bool          lastButtonState    = HIGH;
unsigned long lastButtonDebounce = 0;

// ==================== ANIMATION STATE ====================
unsigned long lastAnimSwitch  = 0;
int           animFrame       = 0;
unsigned long lastFrameUpdate = 0;
bool          showFPAnim      = true;
#define ANIM_SWITCH_MS  3000UL
#define ANIM_FRAME_MS   150UL
#define MATRIX_COLS 21
int  matrixColY[MATRIX_COLS];
bool matrixReady = false;

// ==================== BUZZER ====================
struct BuzzerTone     { int frequency; int duration; };
struct BuzzerSequence {
  const BuzzerTone* tones; int length, currentIndex;
  unsigned long lastToneTime; bool active, loop;
};
BuzzerSequence currentSequence = {nullptr, 0, 0, 0, false, false};

const BuzzerTone accessGrantedMelody[]  = {{NOTE_C5,80},{NOTE_E5,80},{NOTE_G5,80},{NOTE_REST,40},{NOTE_C6,120},{NOTE_REST,40},{NOTE_G5,80},{NOTE_C6,250}};
const BuzzerTone accessDeniedMelody[]   = {{NOTE_A4,180},{NOTE_REST,80},{NOTE_G4,180},{NOTE_REST,80},{NOTE_E4,180},{NOTE_REST,80},{NOTE_C4,350}};
const BuzzerTone remoteUnlockMelody[]   = {{NOTE_E5,70},{NOTE_G5,70},{NOTE_C6,70},{NOTE_REST,40},{NOTE_C6,180}};
const BuzzerTone unlockMelody[]         = {{NOTE_G4,90},{NOTE_C5,90},{NOTE_E5,180}};
const BuzzerTone lockEngagedMelody[]    = {{NOTE_C4,180},{NOTE_REST,30},{NOTE_C4,90}};
const BuzzerTone enrollStepMelody[]     = {{NOTE_C5,70},{NOTE_REST,35},{NOTE_E5,70}};
const BuzzerTone enrollCompleteMelody[] = {{NOTE_C4,80},{NOTE_E4,80},{NOTE_G4,80},{NOTE_C5,180},{NOTE_REST,80},{NOTE_C5,80},{NOTE_E5,80},{NOTE_G5,80},{NOTE_C6,350}};
const BuzzerTone cardDetectedMelody[]   = {{NOTE_C5,45},{NOTE_REST,25},{NOTE_E5,45},{NOTE_REST,25},{NOTE_C5,45}};
const BuzzerTone warningMelody[]        = {{NOTE_G5,130},{NOTE_REST,45},{NOTE_G5,130}};
const BuzzerTone countdownTickMelody[]  = {{NOTE_C6,45}};
const BuzzerTone errorMelody[]          = {{NOTE_C5,90},{NOTE_B4,90},{NOTE_AS4,90},{NOTE_A4,90}};
const BuzzerTone tamperAlertMelody[]    = {{NOTE_C6,100},{NOTE_REST,50},{NOTE_C6,100},{NOTE_REST,50},{NOTE_A5,100},{NOTE_REST,50},{NOTE_A5,100},{NOTE_REST,200}};
const BuzzerTone wifiConnectedMelody[]  = {{NOTE_C5,55},{NOTE_REST,18},{NOTE_E5,55},{NOTE_REST,18},{NOTE_G5,55},{NOTE_REST,18},{NOTE_C6,180}};
const BuzzerTone systemReadyMelody[]    = {{NOTE_C5,45},{NOTE_REST,25},{NOTE_E5,45},{NOTE_REST,25},{NOTE_G5,90}};
const BuzzerTone powerUpMelody[]        = {{NOTE_C4,90},{NOTE_REST,25},{NOTE_C4,90},{NOTE_REST,25},{NOTE_E4,90},{NOTE_REST,25},{NOTE_G4,90},{NOTE_REST,25},{NOTE_C5,180},{NOTE_REST,45},{NOTE_E5,180},{NOTE_REST,45},{NOTE_G5,350}};
const BuzzerTone apMelody[]             = {{NOTE_C5,90},{NOTE_REST,40},{NOTE_G4,90},{NOTE_REST,40},{NOTE_C5,180}};

void buzzerInit() { ledcAttach(BUZZER_PIN, 2000, BUZZER_RES); ledcWriteTone(BUZZER_PIN, 0); }
void playBuzzerSequence(const BuzzerTone* t, int len, bool loop = false) {
  currentSequence = {t, len, 0, millis(), true, loop};
  if (len > 0) ledcWriteTone(BUZZER_PIN, t[0].frequency);
}
void stopBuzzer() { ledcWriteTone(BUZZER_PIN, 0); currentSequence.active = false; }
void updateBuzzer() {
  if (!currentSequence.active || !currentSequence.tones) return;
  unsigned long now = millis();
  const BuzzerTone& tone = currentSequence.tones[currentSequence.currentIndex];
  if (now - currentSequence.lastToneTime >= (unsigned long)tone.duration) {
    currentSequence.currentIndex++;
    if (currentSequence.currentIndex >= currentSequence.length) {
      if (currentSequence.loop) currentSequence.currentIndex = 0;
      else { ledcWriteTone(BUZZER_PIN, 0); currentSequence.active = false; return; }
    }
    ledcWriteTone(BUZZER_PIN, currentSequence.tones[currentSequence.currentIndex].frequency);
    currentSequence.lastToneTime = now;
  }
}
#define MELODY(n)      playBuzzerSequence(n, sizeof(n)/sizeof(BuzzerTone))
#define MELODY_LOOP(n) playBuzzerSequence(n, sizeof(n)/sizeof(BuzzerTone), true)
void beepOK()             { MELODY(accessGrantedMelody); }
void beepFail()           { MELODY(accessDeniedMelody); }
void beepRemoteUnlock()   { MELODY(remoteUnlockMelody); }
void beepUnlock()         { MELODY(unlockMelody); }
void beepLockEngaged()    { MELODY(lockEngagedMelody); }
void beepShort()          { MELODY(cardDetectedMelody); }
void beepEnrollStep()     { MELODY(enrollStepMelody); }
void beepEnrollComplete() { MELODY(enrollCompleteMelody); }
void beepWarning()        { MELODY(warningMelody); }
void beepCountdown()      { MELODY(countdownTickMelody); }
void beepError()          { MELODY(errorMelody); }
void beepTamper()         { MELODY_LOOP(tamperAlertMelody); }
void beepWiFiConnected()  { MELODY(wifiConnectedMelody); }
void beepSystemReady()    { MELODY(systemReadyMelody); }
void beepPowerUp()        { MELODY(powerUpMelody); }
void beepAP()             { MELODY(apMelody); }

// ==================== LOGGING ====================
#define LOG_SIZE 50
struct LogEntry { String timestamp, type, method, user, detail; };
LogEntry sysLogs[LOG_SIZE];
int logHead = 0, logCount = 0;

void addLog(String type, String method, String user, String detail) {
  sysLogs[logHead] = {rtc.getTime("%Y-%m-%d %H:%M:%S"), type, method, user, detail};
  String key   = "log_" + String(logHead);
  String entry = sysLogs[logHead].timestamp + "|" + type + "|" + method + "|" + user + "|" + detail;
  prefs.putString(key.c_str(), entry);
  logHead = (logHead + 1) % LOG_SIZE;
  if (logCount < LOG_SIZE) logCount++;
  if (type == "GRANTED") accessToday++;
  if (type == "DENIED")  failsToday++;
}
void loadPersistentLogs() {
  logCount = logHead = 0;
  for (int i = 0; i < LOG_STORE_SIZE; i++) {
    String key   = "log_" + String(i);
    String entry = prefs.getString(key.c_str(), "");
    if (entry.length() == 0) break;
    int i1 = entry.indexOf('|'), i2 = entry.indexOf('|', i1+1);
    int i3 = entry.indexOf('|', i2+1), i4 = entry.indexOf('|', i3+1);
    if (i1 > 0 && i2 > 0 && i3 > 0 && i4 > 0) {
      sysLogs[logHead] = {
        entry.substring(0,i1), entry.substring(i1+1,i2),
        entry.substring(i2+1,i3), entry.substring(i3+1,i4),
        entry.substring(i4+1)
      };
      logHead = (logHead+1) % LOG_STORE_SIZE;
      logCount++;
    }
  }
}
void saveStats() { prefs.putUInt("access_today", accessToday); prefs.putUInt("fails_today", failsToday); }

// ==================== TIME-BASED ACCESS ====================
bool isWithinSchedule(const String& json, int dow, int minutesToday) {
  if (json.length() == 0 || json == "[]") return true;
  JsonDocument doc;
  if (deserializeJson(doc, json)) return true;
  for (JsonObject o : doc.as<JsonArray>()) {
    if ((o["day"] | -1) != dow) continue;
    if (minutesToday >= (o["start"] | 0) && minutesToday < (o["end"] | 1440)) return true;
  }
  return false;
}
bool checkAccessAllowed() {
  String tm = rtc.getTime("%H:%M:%S");
  int minutes = tm.substring(0,2).toInt() * 60 + tm.substring(3,5).toInt();
  return isWithinSchedule(prefs.getString("schedule_global","[]"), rtc.getDayofWeek(), minutes);
}

// ==================== FORWARD DECLARATIONS ====================
void showIdleAnimation();
void showAccessGranted(const String& method, const String& user);
void showAccessDenied();
void showLockAnimation();
void showBootSequence();
void showWiFiConnectingAnimation(const String& ssid, int frame);
void showAPModeScreen();
void showOTAScreen(int progress);
void unlockDoor();
void lockDoor();
void rfidInit();
String dashboardPage();
void otaSetCallbacks();
void otaBegin();
void networkInit();
void showEnrollmentAnimation();

// ==================== OLED ====================
void displayInit() {
  Wire.begin(21, 22);
  if (!display.begin(OLED_ADDR, true)) { Serial.println("[OLED] Not found"); return; }
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE); display.display();
}
void drawWifiBar(int x, int y, int bars) {
  for (int i = 0; i < 4; i++) {
    int bh = 2+i*2, bx = x+i*5;
    if (i < bars) display.fillRect(bx, y-bh, 3, bh, SH110X_WHITE);
    else          display.drawRect(bx, y-bh, 3, bh, SH110X_WHITE);
  }
}
void showBootSequence() {
  const int FRAMES = 70;
  for (int f = 0; f < FRAMES; f++) {
    display.clearDisplay();
    if (f > 3)  { display.setTextSize(2); display.setCursor(24,3);  display.print("AEGIS"); }
    if (f > 15) { display.setTextSize(1); display.setCursor(14,24); display.print("SMART DOOR v4.1"); }
    if (f > 20)   display.drawLine(10,32,118,32,SH110X_WHITE);
    display.drawRect(10,37,108,8,SH110X_WHITE);
    display.fillRect(11,38,map(f,0,FRAMES-1,0,106),6,SH110X_WHITE);
    display.setTextSize(1); display.setCursor(8,52);
    if (f < FRAMES-8) { String d=""; for(int i=0;i<(f/8)%4;i++) d+="."; display.print("INITIALIZING"+d); }
    else display.print("SYSTEM READY !");
    display.display(); delay(40); updateBuzzer(); ArduinoOTA.handle();
  }
}
void showWiFiConnectingAnimation(const String& ssid, int frame) {
  display.clearDisplay(); display.setTextSize(1);
  display.setCursor((128-6*19)/2,5); display.print("Connecting to WiFi...");
  String s = ssid.length()>21 ? ssid.substring(0,18)+"..." : ssid;
  display.setCursor((128-6*s.length())/2,18); display.print(s);
  int cx=64,cy=42,base=cy+6,active=(frame/2)%5;
  for (int i=0;i<4;i++) {
    int bh=4+i*3,by=base-bh;
    if(i<active) display.fillRect(cx-10+i*7,by,5,bh,SH110X_WHITE);
    else         display.drawRect(cx-10+i*7,by,5,bh,SH110X_WHITE);
  }
  int dp=(frame/3)%4; display.setCursor((128-8*6)/2,54); display.print("Connecting");
  for(int d=0;d<dp;d++) display.print("."); for(int d=dp;d<3;d++) display.print(" ");
  display.display();
  ArduinoOTA.handle();
}
void showAPModeScreen() {
  display.clearDisplay(); display.setTextSize(1);
  display.setCursor(20,2);  display.print("** AP MODE **");
  display.drawLine(0,12,128,12,SH110X_WHITE);
  display.setCursor(0,16); display.print("SSID:");
  display.setCursor(0,26); display.print(AP_SSID);
  display.setCursor(0,38); display.print("Pass: "); display.print(AP_PASS);
  display.setCursor(0,50); display.print("http://192.168.4.1");
  display.display();
}
void showOTAScreen(int progress) {
  display.clearDisplay(); display.setTextSize(1);
  display.setCursor(20,5);  display.print("OTA UPDATE...");
  display.drawLine(0,16,128,16,SH110X_WHITE);
  display.setCursor(0,24); display.print("Flashing firmware");
  display.drawRect(10,36,108,10,SH110X_WHITE);
  display.fillRect(11,37,map(progress,0,100,0,106),8,SH110X_WHITE);
  display.setCursor(50,50); display.print(String(progress)+"%");
  display.display();
}
void showIdleAnimation() {
  if (!matrixReady) { for(int i=0;i<MATRIX_COLS;i++) matrixColY[i]=random(-60,12); matrixReady=true; }
  display.clearDisplay();
  const char* CHARS="01AEGIS10"; const int CLEN=9;
  display.setTextSize(1);
  for (int col=0;col<MATRIX_COLS;col++) {
    int x=col*6+1,hy=matrixColY[col];
    if(hy>=14&&hy<52){display.setCursor(x,hy);display.print(CHARS[(col*2+animFrame)%CLEN]);}
    for(int t=1;t<=4;t++){int ty=hy-t*7;if(ty>=14&&ty<52&&((col+t+animFrame)%3!=0)){display.setCursor(x,ty);display.print(CHARS[(col+t+animFrame)%CLEN]);}}
  }
  display.fillRect(0,0,128,12,SH110X_BLACK);
  display.setTextSize(1); display.setCursor(0,2); display.print("AEGIS v4.1");
  if (wifiMode == 2) {
    display.fillRect(98,1,28,10,SH110X_WHITE);
    display.setTextColor(SH110X_BLACK); display.setCursor(101,2); display.print("AP");
    display.setTextColor(SH110X_WHITE);
  } else if (wifiMode == 1) {
    int rssi=WiFi.RSSI();
    drawWifiBar(99,10,(rssi>-50)?4:(rssi>-60)?3:(rssi>-70)?2:(rssi>-80)?1:0);
  } else { display.setCursor(104,2); display.print("--"); }
  display.drawLine(0,12,128,12,SH110X_WHITE);
  int iX=2,iY=15,iS=37,icx=iX+iS/2,icy=iY+iS/2;
  display.fillRect(iX,iY,iS,iS,SH110X_BLACK);
  if (showFPAnim) {
    display.drawCircle(icx,icy,iS/2-1,SH110X_WHITE);
    for(int i=1;i<=3;i++){int r=iS/2-3-i*4;if(r>2)display.drawCircle(icx,icy,r,SH110X_WHITE);}
    int sY=iY+((animFrame*3)%iS);
    display.drawLine(iX+3,sY,iX+iS-3,sY,SH110X_WHITE); display.fillCircle(icx,sY,2,SH110X_WHITE);
  } else {
    display.drawRect(icx-10,icy-7,20,13,SH110X_WHITE); display.fillRect(icx-7,icy-4,5,7,SH110X_WHITE);
    for(int w=0;w<3;w++){int ph=(animFrame*2+w*8)%24,r=5+ph;if(ph<16&&r<22){for(int a=100;a<=260;a+=15){float rad=a*PI/180.0;int px=icx-10+(int)(r*cos(rad)),py=icy+(int)(r*sin(rad));if(px>0&&px<128&&py>14&&py<53)display.drawPixel(px,py,SH110X_WHITE);}}}
  }
  display.fillRect(0,54,128,10,SH110X_BLACK); display.drawLine(0,53,128,53,SH110X_WHITE);
  display.setTextSize(1);
  display.setCursor(44,56); display.print(showFPAnim?"Place Finger...":"Scan Card...");
  display.setCursor(90,56); display.print(doorLocked?"LOCKED":"OPEN");
  display.display();
  unsigned long now=millis();
  if(now-lastFrameUpdate>ANIM_FRAME_MS){
    lastFrameUpdate=now; animFrame++;
    for(int c=0;c<MATRIX_COLS;c++){matrixColY[c]+=3;if(matrixColY[c]>56)matrixColY[c]=12-random(10,55);}
    if(now-lastAnimSwitch>ANIM_SWITCH_MS){lastAnimSwitch=now;showFPAnim=!showFPAnim;}
  }
}
void showAccessGranted(const String& method, const String& user) {
  for(int f=0;f<35;f++){
    display.clearDisplay(); int cx=22,cy=28;
    for(int r=0;r<3;r++){int p=f-r*7;if(p>0){int rd=p*2;if(rd<26)display.drawCircle(cx,cy,rd,SH110X_WHITE);}}
    display.fillCircle(cx,cy,13,SH110X_WHITE);
    display.drawLine(cx-7,cy,cx-2,cy+5,SH110X_BLACK);display.drawLine(cx-6,cy+1,cx-1,cy+6,SH110X_BLACK);
    display.drawLine(cx-2,cy+5,cx+7,cy-5,SH110X_BLACK);display.drawLine(cx-1,cy+6,cx+8,cy-4,SH110X_BLACK);
    display.setTextSize(1);
    display.setCursor(44,14);display.print(method);
    display.setCursor(44,28);display.print(user.length()>10?user.substring(0,10):user);
    display.setCursor(44,48);display.print("Unlocking...");
    display.display();delay(70);server.handleClient();updateBuzzer();ArduinoOTA.handle();
  }
}
void showAccessDenied() {
  for(int f=0;f<22;f++){
    display.clearDisplay(); int cx=22,cy=28;
    if((f/3)%2==0){display.drawRect(0,0,128,64,SH110X_WHITE);display.drawRect(2,2,124,60,SH110X_WHITE);}
    display.fillCircle(cx,cy,13,SH110X_WHITE);
    display.drawLine(cx-7,cy-7,cx+7,cy+7,SH110X_BLACK);display.drawLine(cx-6,cy-7,cx+8,cy+7,SH110X_BLACK);
    display.drawLine(cx+7,cy-7,cx-7,cy+7,SH110X_BLACK);display.drawLine(cx+8,cy-7,cx-6,cy+7,SH110X_BLACK);
    int shk=((f%6)<3)?1:-1; display.setTextSize(1);
    display.setCursor(44+shk,16);display.print("ACCESS");
    display.setCursor(44+shk,30);display.print("DENIED");
    display.setCursor(44,48);display.print("Unauthorized");
    display.display();delay(90);server.handleClient();updateBuzzer();ArduinoOTA.handle();
  }
}
void showLockAnimation() {
  for(int f=0;f<30;f++){
    display.clearDisplay(); int cx=64,cy=30;
    if(f>18){int pr=(f-18)*2;if(pr<20)display.drawCircle(cx,cy+5,pr,SH110X_WHITE);}
    display.fillRect(cx-11,cy,22,18,SH110X_WHITE); display.fillRect(cx-8,cy+3,16,12,SH110X_BLACK);
    display.fillCircle(cx,cy+8,3,SH110X_WHITE); display.fillRect(cx-1,cy+11,3,5,SH110X_WHITE);
    int openOffset=max(0,(15-f)),topY=cy-16-openOffset;
    display.drawLine(cx-7,cy,cx-7,topY+6,SH110X_WHITE); display.drawLine(cx+7,cy,cx+7,topY+6,SH110X_WHITE);
    for(int a=0;a<=180;a+=15){float r=a*PI/180.0;display.drawPixel(cx+(int)(7.5*cos(r)),topY+6-(int)(6.0*sin(r)),SH110X_WHITE);}
    display.setTextSize(1);display.setCursor(cx-18,56);display.print(f<18?"LOCKING...":"  LOCKED  ");
    display.display();delay(50);updateBuzzer();ArduinoOTA.handle();
  }
}

void showEnrollmentAnimation() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0,0);
  display.print("Enroll Fingerprint");
  display.drawLine(0,10,128,10,SH110X_WHITE);

  const int centerX = 64;
  const int centerY = 36;

  switch(enrollStage) {
    case 1:
      display.setCursor(10,20); display.print("Place finger");
      display.setCursor(10,30); display.print("on sensor...");
      if((millis()/500)%2) display.fillCircle(centerX, centerY, 10, SH110X_WHITE);
      else display.drawCircle(centerX, centerY, 10, SH110X_WHITE);
      break;
    case 2:
      display.setCursor(10,20); display.print("Lift finger");
      display.setCursor(10,30); display.print("and wait...");
      display.drawCircle(centerX, centerY, 10, SH110X_WHITE);
      display.drawLine(centerX, centerY-15, centerX, centerY-5, SH110X_WHITE);
      display.drawLine(centerX-4, centerY-10, centerX, centerY-15, SH110X_WHITE);
      display.drawLine(centerX+4, centerY-10, centerX, centerY-15, SH110X_WHITE);
      break;
    case 3:
      display.setCursor(10,20); display.print("Place same");
      display.setCursor(10,30); display.print("finger again...");
      if((millis()/500)%2) display.fillCircle(centerX, centerY, 10, SH110X_WHITE);
      else display.drawCircle(centerX, centerY, 10, SH110X_WHITE);
      display.drawLine(centerX, centerY+5, centerX, centerY+15, SH110X_WHITE);
      display.drawLine(centerX-4, centerY+10, centerX, centerY+15, SH110X_WHITE);
      display.drawLine(centerX+4, centerY+10, centerX, centerY+15, SH110X_WHITE);
      break;
    case 4:
      display.setCursor(10,20); display.print("Enrollment");
      display.setCursor(10,30); display.print("Complete!");
      display.setCursor(10,40); display.print("ID: " + enrollResult.substring(3));
      display.fillCircle(centerX, centerY, 10, SH110X_WHITE);
      display.drawCircle(centerX, centerY, 14, SH110X_WHITE);
      break;
    case 99:
      display.setCursor(10,20); display.print("Enrollment");
      display.setCursor(10,30); display.print("FAILED");
      display.setCursor(10,40); display.print(enrollResult);
      display.drawCircle(centerX, centerY, 10, SH110X_WHITE);
      display.drawLine(centerX-7, centerY-7, centerX+7, centerY+7, SH110X_WHITE);
      display.drawLine(centerX+7, centerY-7, centerX-7, centerY+7, SH110X_WHITE);
      break;
  }
  display.display();
}

// ==================== DOOR CONTROL ====================
void unlockDoor() {
  Serial.println("[DOOR] Unlocking...");
  digitalWrite(RELAY_PIN,RELAY_UNLOCK); digitalWrite(LED_GREEN,HIGH); digitalWrite(LED_RED,LOW);
  doorLocked=false;
  showAccessGranted("ACCESS","GRANTED"); beepUnlock();
  addLog("GRANTED","DOOR","System","Door Unlocked");
  unsigned long t0=millis(); bool w5=false,w3=false,w1=false;
  while(millis()-t0<UNLOCK_MS){
    server.handleClient(); updateBuzzer(); ArduinoOTA.handle();
    unsigned long rem=UNLOCK_MS-(millis()-t0);
    if(rem<5000&&!w5){beepWarning();w5=true;}
    if(rem<3000&&!w3){beepWarning();w3=true;}
    if(rem<1000&&!w1){beepCountdown();w1=true;}
    delay(50);
  }
  digitalWrite(RELAY_PIN,RELAY_LOCK); digitalWrite(LED_GREEN,LOW);
  doorLocked=true;
  addLog("GRANTED","DOOR","System","Door Locked (auto)");
  showLockAnimation();
  Serial.println("[RFID] Re-initialising after unlock...");
  rfidInit();
  showIdleAnimation();
}
void lockDoor() {
  digitalWrite(RELAY_PIN,RELAY_LOCK); digitalWrite(LED_GREEN,LOW); digitalWrite(LED_RED,LOW);
  doorLocked=true; beepLockEngaged();
  addLog("REMOTE","WEB","Admin","Door Locked (manual)");
  showLockAnimation(); showIdleAnimation();
}

// ==================== UTILITIES ====================
String cleanUID(String uid) {
  uid.trim(); uid.toUpperCase(); String out="";
  for(unsigned int i=0;i<uid.length();i++){char c=uid.charAt(i);if(c!=':'&&c!=' '&&c!='-')out+=c;}
  return out;
}
String generateToken() { String t=""; for(int i=0;i<32;i++) t+=String((uint32_t)esp_random()%16,HEX); return t; }
bool isAuthorized() {
  String tok=server.hasHeader("X-Auth-Token")?server.header("X-Auth-Token"):server.arg("token");
  return tok.length()>0&&tok==currentToken&&millis()-sessionTimeout<SESSION_TIMEOUT_MS;
}
void sendUnauthorized() { server.send(401,"application/json","{\"error\":\"Unauthorized\"}"); }

// ==================== RFID ====================
void rfidInit() {
  rfid.PCD_Init(); delay(50); rfid.PCD_SetAntennaGain(rfid.RxGain_max);
  byte ver=rfid.PCD_ReadRegister(rfid.VersionReg);
  if(ver==0x00||ver==0xFF) Serial.println("[RFID] WARNING: MFRC522 not responding");
  else { Serial.printf("[RFID] MFRC522 ready firmware=0x%02X\n",ver); rfidFailCount=0; }
}
String readRFID() {
  String uid="";
  for(byte i=0;i<rfid.uid.size;i++){if(rfid.uid.uidByte[i]<0x10)uid+="0";uid+=String(rfid.uid.uidByte[i],HEX);}
  uid.toUpperCase(); return uid;
}
bool verifyRFID(const String& uid) {
  int n=prefs.getInt("card_count",0);
  for(int i=0;i<n;i++) if(prefs.getString(("card_"+String(i)).c_str(),"")==uid) return true;
  return false;
}
String getRFIDName(const String& uid) {
  int n=prefs.getInt("card_count",0);
  for(int i=0;i<n;i++) if(prefs.getString(("card_"+String(i)).c_str(),"")==uid)
    return prefs.getString(("card_name_"+String(i)).c_str(),uid);
  return uid;
}
bool addCard(String uid, String name) {
  uid=cleanUID(uid);
  if(uid.length()<4||verifyRFID(uid)) return false;
  int n=prefs.getInt("card_count",0);
  prefs.putString(("card_"+String(n)).c_str(),uid);
  prefs.putString(("card_name_"+String(n)).c_str(),name);
  prefs.putString(("card_added_"+String(n)).c_str(),rtc.getTime("%Y-%m-%d"));
  prefs.putInt("card_count",n+1);
  addLog("ENROLL","RFID",name,"Card Added: "+uid); return true;
}
bool removeCard(String uid) {
  uid=cleanUID(uid);
  int n=prefs.getInt("card_count",0);
  String* us=new String[n],*ns=new String[n],*as=new String[n];
  int nc=0; bool found=false;
  for(int i=0;i<n;i++){
    String c=prefs.getString(("card_"+String(i)).c_str(),"");
    if(c==uid){found=true;}
    else{us[nc]=c;ns[nc]=prefs.getString(("card_name_"+String(i)).c_str(),"");as[nc]=prefs.getString(("card_added_"+String(i)).c_str(),"");nc++;}
  }
  if(!found){delete[]us;delete[]ns;delete[]as;return false;}
  for(int i=0;i<n;i++){prefs.remove(("card_"+String(i)).c_str());prefs.remove(("card_name_"+String(i)).c_str());prefs.remove(("card_added_"+String(i)).c_str());}
  for(int i=0;i<nc;i++){prefs.putString(("card_"+String(i)).c_str(),us[i]);prefs.putString(("card_name_"+String(i)).c_str(),ns[i]);prefs.putString(("card_added_"+String(i)).c_str(),as[i]);}
  prefs.putInt("card_count",nc);
  delete[]us;delete[]ns;delete[]as;
  addLog("ENROLL","WEB","Admin","Card Removed: "+uid); return true;
}

// ==================== FINGERPRINT ====================
void fpInit() {
  if(FP_USE_WAKEUP) pinMode(FP_WAKEUP_PIN,INPUT_PULLUP);
  fpSerial.begin(FP_BAUD,SERIAL_8N1,FP_RX_PIN,FP_TX_PIN);
  finger.begin(FP_BAUD); delay(1500);
  for(int a=1;a<=3;a++){
    if(finger.verifyPassword()){
      finger.getTemplateCount();
      lastFpCount = finger.templateCount;   // cache count
      fpAvailable = true;
      return;
    }
    delay(600);
  }
  fpAvailable = false;
  Serial.println("[FP] R307S NOT FOUND");
}
int scanFP() {
  if(!fpAvailable) return -1;
  if(FP_USE_WAKEUP&&digitalRead(FP_WAKEUP_PIN)==HIGH) return -2;
  if(finger.getImage()!=FINGERPRINT_OK) return -2;
  if(finger.image2Tz()!=FINGERPRINT_OK) return -3;
  int r=finger.fingerFastSearch();
  if(r==FINGERPRINT_NOTFOUND) return -5;
  if(r!=FINGERPRINT_OK)       return -6;
  return finger.fingerID;
}
bool deleteFP(int id) {
  Serial.printf("[FP DELETE] Attempting ID %d\n",id);
  if(!fpAvailable){Serial.println("[FP DELETE] Sensor not available");return false;}
  if(!finger.verifyPassword()){Serial.println("[FP DELETE] Password verify failed -- re-init");fpInit();if(!fpAvailable)return false;}
  bool ok=(finger.deleteModel(id)==FINGERPRINT_OK);
  if(ok){
    prefs.remove(("fp_name_"+String(id)).c_str());
    prefs.remove(("fp_enrolled_"+String(id)).c_str());
    prefs.remove(("fp_last_"+String(id)).c_str());
    lastFpCount--;                           // decrement cache
    Serial.printf("[FP DELETE] Successfully deleted ID %d\n",id);
  } else Serial.printf("[FP DELETE] FAILED for ID %d\n",id);
  return ok;
}

// ==================== ENROLLMENT (non‑blocking) ====================
void runEnroll() {
  if(!enrollPending || !fpAvailable) return;
  switch(enrollStage) {
    case 1: {
      int p = finger.getImage();
      if(p == FINGERPRINT_NOFINGER) return;
      if(p != FINGERPRINT_OK || finger.image2Tz(1) != FINGERPRINT_OK) {
        enrollResult = "Image error 1";
        enrollStage = 99;
        enrollCompletionTime = millis() + 2000;
        beepError();
        return;
      }
      beepEnrollStep();
      enrollStage = 2;
      enrollStageTime = millis();
      break;
    }
    case 2: {
      if(finger.getImage() != FINGERPRINT_NOFINGER) return;
      if(millis() - enrollStageTime < 1000) return;
      enrollStage = 3;
      break;
    }
    case 3: {
      int p = finger.getImage();
      if(p == FINGERPRINT_NOFINGER) return;
      if(p != FINGERPRINT_OK || finger.image2Tz(2) != FINGERPRINT_OK) {
        enrollResult = "Image error 2";
        enrollStage = 99;
        enrollCompletionTime = millis() + 2000;
        beepError();
        return;
      }
      if(finger.createModel() != FINGERPRINT_OK) {
        enrollResult = "No match";
        enrollStage = 99;
        enrollCompletionTime = millis() + 2000;
        beepFail();
        return;
      }

      // Find first free ID using Preferences (avoids sensor loadModel interference)
      int newID = -1;
      for (int i = 1; i <= 127; i++) {
        String key = "fp_name_" + String(i);
        if (prefs.getString(key.c_str(), "").length() == 0) {
          newID = i;
          break;
        }
      }
      if(newID == -1) {
        enrollResult = "Sensor full";
        enrollStage = 99;
        enrollCompletionTime = millis() + 2000;
        beepError();
        return;
      }

      Serial.printf("[FP ENROLL] Storing at first free slot: ID %d\n", newID);
      if(finger.storeModel(newID) != FINGERPRINT_OK) {
        enrollResult = "Storage fail";
        enrollStage = 99;
        enrollCompletionTime = millis() + 2000;
        beepFail();
        return;
      }
      prefs.putString(("fp_name_" + String(newID)).c_str(), enrollName);
      prefs.putString(("fp_enrolled_" + String(newID)).c_str(), rtc.getTime("%Y-%m-%d"));
      prefs.putString(("fp_last_" + String(newID)).c_str(), "-");
      lastFpCount++;                              // update cache
      enrollResult = "OK:" + String(newID);
      addLog("ENROLL", "WEB", enrollName, "FP Enrolled ID " + String(newID));
      beepEnrollComplete();
      enrollStage = 4;
      enrollCompletionTime = millis() + 2000;
      break;
    }
  }
}

// ==================== JSON BUILDERS (now safe, no sensor calls) ====================
String getFingerprintsJSON() {
  if(!fpAvailable) return "[]";
  JsonDocument doc; JsonArray arr=doc.to<JsonArray>();
  // Use stored names to determine which IDs are active (no loadModel call)
  for(int i=1; i<=127; i++){
    String name = prefs.getString(("fp_name_"+String(i)).c_str(), "");
    if(name.length() == 0) continue;   // empty slot
    JsonObject o=arr.add<JsonObject>();
    o["id"]=i;
    o["name"]=name;
    String ed=prefs.getString(("fp_enrolled_"+String(i)).c_str(), "");
    if(ed.length()==0){ ed=rtc.getTime("%Y-%m-%d"); prefs.putString(("fp_enrolled_"+String(i)).c_str(),ed); }
    o["enrolled"]=ed;
    o["lastUsed"]=prefs.getString(("fp_last_"+String(i)).c_str(), "-");
    o["active"]=true;
  }
  String j; serializeJson(doc,j); return j;
}
String getRFIDJSON() {
  JsonDocument doc; JsonArray arr=doc.to<JsonArray>();
  int n=prefs.getInt("card_count",0);
  for(int i=0;i<n;i++){
    JsonObject o=arr.add<JsonObject>();
    o["uid"]=prefs.getString(("card_"+String(i)).c_str(),"");
    o["name"]=prefs.getString(("card_name_"+String(i)).c_str(),"");
    o["added"]=prefs.getString(("card_added_"+String(i)).c_str(),"-");
    o["lastUsed"]=prefs.getString(("card_last_"+String(i)).c_str(),"-");
    o["active"]=true;
  }
  String j; serializeJson(doc,j); return j;
}
String getLogsJSON(int limit=50) {
  JsonDocument doc; JsonArray arr=doc.to<JsonArray>();
  int cnt=min(limit,logCount);
  for(int i=0;i<cnt;i++){
    int idx=(logHead-1-i+LOG_SIZE)%LOG_SIZE;
    JsonObject o=arr.add<JsonObject>();
    o["ts"]=sysLogs[idx].timestamp;o["type"]=sysLogs[idx].type;
    o["method"]=sysLogs[idx].method;o["user"]=sysLogs[idx].user;o["detail"]=sysLogs[idx].detail;
  }
  String j; serializeJson(doc,j); return j;
}

// ==================== OTA INIT ====================
bool otaCallbacksRegistered = false;

void otaSetCallbacks() {
  if (otaCallbacksRegistered) return;
  otaCallbacksRegistered = true;

  ArduinoOTA.setHostname("aegis-lock");
  ArduinoOTA.setPassword(OTA_PASS);

  ArduinoOTA.onStart([]() {
    Serial.println("[OTA] Start"); stopBuzzer(); showOTAScreen(0);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\n[OTA] End");
    display.clearDisplay(); display.setTextSize(1);
    display.setCursor(20,28); display.print("Update complete!");
    display.display(); delay(1000);
  });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
    int pct = (p * 100) / t;
    Serial.printf("[OTA] Progress: %u%%\r", pct);
    showOTAScreen(pct);
  });
  ArduinoOTA.onError([](ota_error_t e) {
    Serial.printf("[OTA] Error[%u]\n", e); beepError();
  });
}

void otaBegin() {
  otaSetCallbacks();
  ArduinoOTA.begin();
  Serial.println("[OTA] Advertising on network -- password: " + String(OTA_PASS));
}

// ==================== NETWORK INIT ====================
void networkInit() {
  String ssid = prefs.getString("wifi_ssid", MY_SSID);
  String pass = prefs.getString("wifi_pass", MY_PASS);

  WiFi.disconnect(true); delay(100); WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    showWiFiConnectingAnimation(ssid, tries);
    updateBuzzer(); delay(500);
    Serial.print("."); tries++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    wifiMode = 1;
    Serial.println("[WIFI] STA connected: " + WiFi.localIP().toString());
    beepWiFiConnected();
    configTime(0,0,"pool.ntp.org","time.nist.gov");
    struct tm ti;
    if(getLocalTime(&ti)) rtc.setTime(ti.tm_sec,ti.tm_min,ti.tm_hour,ti.tm_mday,ti.tm_mon+1,ti.tm_year+1900);
    otaBegin();
  } else {
    wifiMode = 2;
    WiFi.mode(WIFI_AP); WiFi.softAP(AP_SSID, AP_PASS); delay(100);
    Serial.println("[WIFI] STA failed -- AP started: " + String(AP_SSID));
    Serial.println("[WIFI] AP IP: " + WiFi.softAPIP().toString());
    beepAP(); showAPModeScreen(); delay(3000);
    lastApRetry = millis();
    otaBegin();
  }
}

// ==================== WEB PAGES (unchanged) ====================
String loginPage() {
  return R"rawliteral(<!DOCTYPE html>
<html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>AEGIS -- Authentication</title>
<style>
:root{--bg:#080c10;--bg2:#0d1117;--bg3:#111820;--border:#1e2d3d;--accent:#00e5ff;--danger:#ff3d5a;--success:#00ff99;--text:#c8dde8;--text2:#7a9bb0}
*{margin:0;padding:0;box-sizing:border-box}
body{background:var(--bg);color:var(--text);font-family:'Segoe UI',Arial,sans-serif;display:flex;align-items:center;justify-content:center;min-height:100vh}
body::before{content:'';position:fixed;inset:0;background-image:linear-gradient(rgba(0,229,255,0.03) 1px,transparent 1px),linear-gradient(90deg,rgba(0,229,255,0.03) 1px,transparent 1px);background-size:40px 40px;pointer-events:none;z-index:0}
.box{position:relative;z-index:1;background:var(--bg2);border:1px solid var(--border);border-radius:14px;padding:40px;max-width:400px;width:90vw}
.logo{font-size:2em;font-weight:800;letter-spacing:.15em;color:var(--accent);text-align:center;margin-bottom:5px}
.sub{font-family:'Courier New',monospace;font-size:.75em;color:var(--text2);text-align:center;margin-bottom:28px}
label{display:block;font-size:.7em;font-weight:700;letter-spacing:.1em;text-transform:uppercase;color:var(--text2);margin-bottom:6px}
input{width:100%;padding:11px;background:var(--bg3);border:1px solid var(--border);border-radius:8px;color:var(--text);font-family:'Courier New',monospace;font-size:.9em;outline:none;transition:all .3s;margin-bottom:18px}
input:focus{border-color:var(--accent);box-shadow:0 0 0 3px rgba(0,229,255,.1)}
.btn{width:100%;padding:12px;background:var(--accent);color:var(--bg);border:none;border-radius:8px;font-size:.9em;font-weight:700;text-transform:uppercase;letter-spacing:.1em;cursor:pointer}
.err{background:rgba(255,61,90,.1);border:1px solid rgba(255,61,90,.3);border-radius:7px;padding:10px;color:var(--danger);font-size:.85em;text-align:center;margin-bottom:16px;display:none}
.err.show{display:block}
.status{display:flex;align-items:center;justify-content:center;gap:8px;margin-top:14px;font-size:.75em;color:var(--text2)}
.dot{width:8px;height:8px;border-radius:50%;background:var(--success);animation:p 2s infinite}
@keyframes p{0%,100%{opacity:1}50%{opacity:.4}}
.hint{font-size:.7em;color:var(--text2);text-align:center;margin-top:10px}
</style></head>
<body><div class="box">
<div class="logo">AEGIS</div>
<div class="sub">// smart door access control</div>
<div class="err" id="err">Invalid credentials</div>
<form onsubmit="login(event)">
<label>Username</label><input type="text" id="u" placeholder="admin" required autofocus>
<label>Password</label><input type="password" id="p" placeholder="password" required>
<button type="submit" class="btn" id="btn">Authenticate &rarr;</button>
</form>
<div class="status"><div class="dot"></div><span>ESP32 v4.0 Door Lock</span></div>
<div class="hint">Default: admin / admin</div>
</div>
<script>
async function login(e){
  e.preventDefault();
  const btn=document.getElementById('btn'),err=document.getElementById('err');
  btn.textContent='Authenticating...';btn.disabled=true;err.classList.remove('show');
  try{
    const r=await fetch('/api/login',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({username:document.getElementById('u').value,password:document.getElementById('p').value})});
    const d=await r.json();
    if(d.success&&d.token){localStorage.setItem('aegis_token',d.token);window.location.href='/dashboard?token='+d.token;}
    else{err.textContent=d.message||'Invalid credentials';err.classList.add('show');btn.textContent='Authenticate \u2192';btn.disabled=false;}
  }catch(x){err.textContent='Connection failed';err.classList.add('show');btn.textContent='Authenticate \u2192';btn.disabled=false;}
}
const t=localStorage.getItem('aegis_token');
if(t)fetch('/api/verify-token',{headers:{'X-Auth-Token':t}}).then(r=>r.json()).then(d=>{if(d.valid)window.location.href='/dashboard?token='+t;}).catch(()=>{});
</script></body></html>)rawliteral";
}

String dashboardPage() {
  return R"rawliteral(<!DOCTYPE html>
<html lang="en"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>AEGIS -- Smart Door Control</title>
<style>
:root{--bg:#080c10;--bg2:#0d1117;--bg3:#111820;--border:#1e2d3d;--accent:#00e5ff;--danger:#ff3d5a;--warn:#ffaa00;--success:#00ff99;--muted:#4a6278;--text:#c8dde8;--text2:#7a9bb0;--radius:10px;--glow:0 0 20px rgba(0,229,255,0.15)}
*{box-sizing:border-box;margin:0;padding:0}body{background:var(--bg);color:var(--text);font-family:'Segoe UI',Arial,sans-serif;min-height:100vh}
.wrap{max-width:1200px;margin:0 auto;padding:20px}
header{background:var(--bg2);border-bottom:1px solid var(--border);padding:14px 20px;display:flex;justify-content:space-between;align-items:center}
.logo{font-size:1.4em;font-weight:700;color:var(--accent);letter-spacing:.1em}.logo span{font-size:.55em;color:var(--text2);display:block}
.hright{display:flex;align-items:center;gap:10px}
.mode-badge{padding:3px 10px;border-radius:5px;font-size:.72em;font-weight:700;letter-spacing:.05em}
.mode-sta{background:rgba(0,255,153,.1);border:1px solid rgba(0,255,153,.3);color:var(--success)}
.mode-ap{background:rgba(255,170,0,.1);border:1px solid rgba(255,170,0,.3);color:var(--warn)}
.dot{width:9px;height:9px;border-radius:50%;background:var(--success);display:inline-block;animation:p 2s infinite;margin-right:5px}
@keyframes p{0%,100%{opacity:1}50%{opacity:.4}}
nav{background:var(--bg2);border-bottom:1px solid var(--border);padding:0 20px;overflow-x:auto;white-space:nowrap}
nav button{background:none;border:none;color:var(--muted);padding:11px 18px;cursor:pointer;font-size:.85em;font-weight:600;text-transform:uppercase;letter-spacing:.05em;transition:color .3s}
nav button.active{color:var(--accent);border-bottom:2px solid var(--accent)}
.panel{display:none;animation:fi .3s ease}.panel.active{display:block}
@keyframes fi{from{opacity:0;transform:translateY(8px)}to{opacity:1;transform:none}}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:18px;margin-top:18px}
.card{background:var(--bg2);border:1px solid var(--border);border-radius:var(--radius);padding:18px}
.ct{font-size:.75em;font-weight:700;text-transform:uppercase;letter-spacing:.1em;color:var(--text2);margin-bottom:12px}
.door-st{text-align:center;font-size:2em;font-weight:800;margin:16px 0}.door-st.locked{color:var(--accent)}.door-st.unlocked{color:var(--success)}
.sn{font-size:2.4em;font-weight:800;color:var(--accent)}.sl{font-size:.8em;color:var(--text2);margin-top:4px}
.btn{display:inline-block;padding:9px 18px;border-radius:7px;border:1px solid var(--accent);color:var(--accent);background:rgba(0,229,255,.05);cursor:pointer;font-weight:700;font-size:.8em;text-transform:uppercase;letter-spacing:.05em;text-decoration:none;transition:all .3s;margin:4px}
.btn:hover{background:rgba(0,229,255,.12);box-shadow:var(--glow)}
.btn.danger{border-color:var(--danger);color:var(--danger);background:rgba(255,61,90,.05)}.btn.danger:hover{background:rgba(255,61,90,.12)}
.btn.warn{border-color:var(--warn);color:var(--warn)}.btn.success{border-color:var(--success);color:var(--success)}
table{width:100%;border-collapse:collapse;font-size:.88em}th,td{padding:9px;text-align:left;border-bottom:1px solid var(--border)}
th{color:var(--text2);font-size:.78em;font-weight:700;text-transform:uppercase;letter-spacing:.05em}tr:hover{background:rgba(255,255,255,.02)}
.badge{display:inline-block;padding:2px 8px;border-radius:4px;font-size:.75em;font-weight:600}
.badge-ok{background:rgba(0,255,153,.1);color:var(--success);border:1px solid rgba(0,255,153,.2)}
.alert{padding:10px;border-radius:7px;margin-bottom:14px;display:none}.alert.show{display:block}
.alert.ok{background:rgba(0,255,153,.06);border:1px solid rgba(0,255,153,.2);color:var(--success)}
.alert.error{background:rgba(255,61,90,.06);border:1px solid rgba(255,61,90,.2);color:var(--danger)}
.alert.warn{background:rgba(255,170,0,.06);border:1px solid rgba(255,170,0,.2);color:var(--warn)}
.le{padding:8px;margin:4px 0;background:var(--bg3);border-radius:5px;font-family:'Courier New',monospace;font-size:.82em}
input[type=text],input[type=password],input[type=time]{width:100%;padding:9px;background:var(--bg3);border:1px solid var(--border);border-radius:6px;color:var(--text);font-size:.88em;margin:5px 0;outline:none;transition:border-color .3s}
input:focus{border-color:var(--accent)}
.modal{display:none;position:fixed;top:0;left:0;right:0;bottom:0;background:rgba(0,0,0,.8);z-index:1000;align-items:center;justify-content:center}
.modal.show{display:flex}
.mc{background:var(--bg2);border:1px solid var(--border);border-radius:14px;padding:28px;max-width:380px;width:90%;text-align:center}
.toggle-wrap{display:flex;align-items:center;justify-content:space-between;margin-bottom:18px}
.toggle-sw{position:relative;width:48px;height:24px}.toggle-sw input{opacity:0;width:0;height:0}
.toggle-sl{position:absolute;cursor:pointer;inset:0;background:var(--border);border-radius:24px;transition:.3s}
.toggle-sl:before{position:absolute;content:"";height:18px;width:18px;left:3px;bottom:3px;background:var(--text2);border-radius:50%;transition:.3s}
input:checked+.toggle-sl{background:var(--success)}input:checked+.toggle-sl:before{transform:translateX(24px);background:#fff}
.day-row{display:flex;align-items:center;justify-content:space-between;padding:8px 0;border-bottom:1px solid rgba(255,255,255,.05)}
.day-row label{display:flex;align-items:center;gap:8px;color:var(--text)}
.day-row input[type=checkbox]{width:16px;height:16px;accent-color:var(--accent)}
.day-row .ti{display:flex;gap:8px;align-items:center}
.day-row input[type=time]{width:108px;font-family:'Courier New',monospace}
</style></head>
<body>
<header>
<div class="logo">AEGIS<span>// smart door control v4.1</span></div>
<div class="hright">
<span id="mode-badge" class="mode-badge mode-sta">ONLINE</span>
<span class="dot"></span><span style="color:var(--text2);font-size:.9em">CONNECTED</span>
<button onclick="logout()" style="background:none;border:1px solid var(--danger);color:var(--danger);padding:5px 10px;border-radius:5px;cursor:pointer;font-size:.8em">Logout</button>
</div>
</header>
<nav>
<button class="active" onclick="showPanel('dashboard',this)">Dashboard</button>
<button onclick="showPanel('fingerprints',this)">Fingerprints</button>
<button onclick="showPanel('rfid',this)">RFID Cards</button>
<button onclick="showPanel('schedule',this)">Schedule</button>
<button onclick="showPanel('logs',this)">Access Logs</button>
<button onclick="showPanel('settings',this)">Settings</button>
</nav>
<div class="wrap">
<div id="ap-banner" class="alert warn" style="margin-top:16px">
  &#9888; Running in AP (offline) mode &mdash; SSID: <strong>AEGIS-LOCK</strong> &bull; IP: 192.168.4.1 &bull; NTP time sync unavailable.
</div>

<!-- Dashboard -->
<div class="panel active" id="panel-dashboard">
<div class="grid">
<div class="card" style="grid-column:1/-1"><div class="ct">Door Status</div>
<div class="door-st locked" id="door-st">LOCKED</div>
<div style="text-align:center">
<button class="btn success" onclick="toggleDoor()">&#128275; Remote Unlock</button>
<button class="btn danger"  onclick="lockDoor()">&#128274; Emergency Lock</button>
</div></div>
<div class="card"><div class="ct">Enrolled Users</div><div class="sn" id="st-users">0</div><div class="sl">Fingerprints + RFID Cards</div></div>
<div class="card"><div class="ct">Access Today</div><div class="sn" style="color:var(--success)" id="st-ok">0</div><div class="sl">Successful entries</div></div>
<div class="card"><div class="ct">Failed Attempts</div><div class="sn" style="color:var(--danger)" id="st-fail">0</div><div class="sl">Denied today</div></div>
<div class="card"><div class="ct">Network</div>
<div style="font-family:'Courier New',monospace;font-size:.95em;color:var(--accent)" id="st-ip">--</div>
<div class="sl" id="st-rssi">--</div>
<div class="sl" id="st-mode" style="margin-top:4px;font-weight:600"></div>
</div>
</div></div>

<!-- Fingerprints -->
<div class="panel" id="panel-fingerprints">
<div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:16px">
<h2 style="font-weight:700">Fingerprint Users</h2><button class="btn" onclick="enrollFP()">+ Enroll New</button></div>
<div id="fp-alert" class="alert"></div>
<div class="card"><table><thead><tr><th>ID</th><th>Name</th><th>Enrolled</th><th>Status</th><th>Actions</th></tr></thead><tbody id="fp-table"></tbody></table></div>
</div>

<!-- RFID -->
<div class="panel" id="panel-rfid">
<div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:16px">
<h2 style="font-weight:700">RFID Cards</h2><button class="btn" onclick="startScan()">+ Scan New Card</button></div>
<div id="rfid-alert" class="alert"></div>
<div class="card"><table><thead><tr><th>UID</th><th>Name</th><th>Added</th><th>Actions</th></tr></thead><tbody id="rfid-table"></tbody></table></div>
</div>

<!-- Schedule -->
<div class="panel" id="panel-schedule">
<h2 style="margin-bottom:16px;font-weight:700">Access Schedule</h2>
<div id="sch-alert" class="alert"></div>
<div class="card">
<div class="toggle-wrap"><span style="font-size:.95em;font-weight:600">Enable Time-Restricted Access</span>
<label class="toggle-sw"><input type="checkbox" id="sch-master" onchange="toggleSch()"><span class="toggle-sl"></span></label></div>
<div id="sch-days" style="display:none"><div id="day-rows"></div>
<button class="btn success" onclick="saveSch()" style="margin-top:16px">Save Schedule</button>
</div></div></div>

<!-- Logs -->
<div class="panel" id="panel-logs">
<div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:16px">
<h2 style="font-weight:700">Access Logs</h2><button class="btn" onclick="loadLogs()">&#8635; Refresh</button></div>
<div class="card" id="log-wrap">Loading...</div></div>

<!-- Settings -->
<div class="panel" id="panel-settings">
<h2 style="margin-bottom:16px;font-weight:700">Settings</h2>
<div class="grid">
<div class="card"><div class="ct">WiFi</div>
<label style="font-size:.78em;color:var(--text2)">SSID</label><input type="text" id="w-ssid" placeholder="Network name">
<label style="font-size:.78em;color:var(--text2)">Password</label><input type="password" id="w-pass" placeholder="Network password">
<button class="btn" style="margin-top:8px" onclick="updateWiFi()">Update WiFi</button></div>
<div class="card"><div class="ct">Admin Password</div>
<label style="font-size:.78em;color:var(--text2)">Current</label><input type="password" id="p-old">
<label style="font-size:.78em;color:var(--text2)">New</label><input type="password" id="p-new">
<button class="btn" style="margin-top:8px" onclick="changePass()">Change Password</button></div>
</div></div>

</div>
<div class="modal" id="scan-modal"><div class="mc">
<div style="font-size:2.5em;margin-bottom:14px">&#128225;</div>
<h3 style="color:var(--accent);margin-bottom:12px">RFID Card Scanner</h3>
<div id="scan-msg" style="color:var(--text2);margin-bottom:16px">Place card on reader...</div>
<div id="scan-form" style="display:none;text-align:left">
<label style="font-size:.78em;color:var(--text2)">Card UID</label>
<input type="text" id="scan-uid" readonly style="color:var(--accent);font-family:'Courier New',monospace">
<label style="font-size:.78em;color:var(--text2)">Card Name</label>
<input type="text" id="scan-name" placeholder="e.g. John's Card">
</div>
<div style="display:flex;gap:10px;justify-content:center;margin-top:16px">
<button class="btn success" id="save-btn" onclick="saveCard()" style="display:none">Save Card</button>
<button class="btn danger" onclick="cancelScan()">Cancel</button>
</div></div></div>

<script>
// Auth bootstrap
const _urlP=new URLSearchParams(window.location.search);
const _urlTok=_urlP.get('token');
const _stored=localStorage.getItem('aegis_token');
const _active=_urlTok||_stored;
if(!_active){window.location.href='/';}
else{if(_urlTok)localStorage.setItem('aegis_token',_urlTok);if(!_urlTok&&_stored)window.history.replaceState({},document.title,'/dashboard?token='+_stored);}

function api(method,path,body){
  const tok=localStorage.getItem('aegis_token');
  if(!tok){logout();return Promise.reject('No token');}
  const hasBody=body!==undefined&&body!==null;
  const headers={'X-Auth-Token':tok};
  if(hasBody)headers['Content-Type']='application/json';
  const opts={method,headers};
  if(hasBody)opts.body=JSON.stringify(body);
  return fetch(path,opts).then(r=>{if(r.status===401){logout();throw new Error('Unauthorized');}return r.json();});
}
function sa(id,type,msg){const e=document.getElementById(id);e.className='alert '+type+' show';e.textContent=msg;setTimeout(()=>e.className='alert',4000);}

let _activePanel='dashboard';
function showPanel(id,btn){
  if(id===_activePanel&&btn)return;
  _activePanel=id;
  document.querySelectorAll('.panel').forEach(p=>p.classList.remove('active'));
  document.querySelectorAll('nav button').forEach(b=>b.classList.remove('active'));
  document.getElementById('panel-'+id).classList.add('active');
  if(btn)btn.classList.add('active');
  if(id==='fingerprints')loadFP();
  if(id==='rfid')loadRFID();
  if(id==='schedule')loadSch();
  if(id==='logs')loadLogs();
}

function updateStatus(){
  api('GET','/api/status').then(d=>{
    const e=document.getElementById('door-st');
    e.textContent=d.locked?'LOCKED':'UNLOCKED';
    e.className='door-st '+(d.locked?'locked':'unlocked');
    document.getElementById('st-users').textContent=(d.fp_count||0)+(d.card_count||0);
    document.getElementById('st-ok').textContent=d.access_today||0;
    document.getElementById('st-fail').textContent=d.fails_today||0;
    document.getElementById('st-ip').textContent=d.ip||'--';
    const isAP=d.wifi_mode===2;
    const badge=document.getElementById('mode-badge');
    const banner=document.getElementById('ap-banner');
    const modeEl=document.getElementById('st-mode');
    if(isAP){
      badge.textContent='AP MODE';badge.className='mode-badge mode-ap';
      banner.style.display='block';
      document.getElementById('st-rssi').textContent='AP clients: '+(d.ap_clients||0);
      modeEl.textContent='Offline hotspot mode';modeEl.style.color='var(--warn)';
    } else {
      badge.textContent='ONLINE';badge.className='mode-badge mode-sta';
      banner.style.display='none';
      document.getElementById('st-rssi').textContent='RSSI: '+(d.rssi||'--')+' dBm';
      modeEl.textContent='Connected to router';modeEl.style.color='var(--success)';
    }
  }).catch(()=>{});
}
const UNLOCK_MS=5000;
function toggleDoor(){api('POST','/api/lock/toggle').then(()=>{const e=document.getElementById('door-st');e.textContent='UNLOCKED';e.className='door-st unlocked';setTimeout(updateStatus,UNLOCK_MS+1500);}).catch(updateStatus);}
function lockDoor(){api('POST','/api/lock').then(updateStatus).catch(updateStatus);}

function loadFP(){
  api('GET','/api/fingerprints').then(d=>{
    const tb=document.getElementById('fp-table');
    if(!d||!d.length){tb.innerHTML='<tr><td colspan=5 style="text-align:center;color:var(--text2);padding:20px">No fingerprints enrolled</td></tr>';return;}
    tb.innerHTML=d.map(f=>`<tr><td style="color:var(--accent)">#${f.id}</td><td>${f.name}</td><td>${f.enrolled}</td><td><span class="badge badge-ok">ACTIVE</span></td><td><button class="btn warn" style="font-size:.72em;padding:4px 10px" onclick="delFP(${f.id})">Delete</button></td></tr>`).join('');
  });
}
function enrollFP(){
  const n=prompt('Enter name for new fingerprint:');if(!n)return;
  api('POST','/api/fingerprint/enroll',{name:n}).then(r=>{
    if(r.status==='started'){
      sa('fp-alert','ok','Place finger on sensor — lift and place again when prompted...');
      let attempts=0;
      const rows=document.querySelectorAll('#fp-table tr');
      let lastCount=rows.length===1&&rows[0].querySelector('td[colspan]')?0:rows.length;
      const poll=()=>{
        attempts++;if(attempts>60){sa('fp-alert','error','Enrollment timed out');return;}
        api('GET','/api/fingerprints').then(fpList=>{
          if((fpList&&fpList.length||0)>lastCount){sa('fp-alert','ok','Fingerprint enrolled!');loadFP();}
          else setTimeout(poll,1000);
        }).catch(()=>setTimeout(poll,1000));
      };
      setTimeout(poll,3000);
    } else sa('fp-alert','error',r.error||'Failed');
  });
}
function delFP(id){
  if(!confirm('Delete fingerprint #'+id+'?'))return;
  api('DELETE','/api/fingerprint/'+id).then(r=>{
    if(r.status==='deleted'){sa('fp-alert','ok','Deleted #'+id);loadFP();}
    else sa('fp-alert','error',r.error||'Failed');
  });
}
function loadRFID(){
  api('GET','/api/rfid/cards').then(d=>{
    const tb=document.getElementById('rfid-table');
    if(!d||!d.length){tb.innerHTML='<tr><td colspan=4 style="text-align:center;color:var(--text2);padding:20px">No RFID cards registered</td></tr>';return;}
    tb.innerHTML=d.map(c=>`<tr><td style="color:var(--warn);font-family:'Courier New',monospace">${c.uid}</td><td>${c.name}</td><td>${c.added}</td><td><button class="btn warn" style="font-size:.72em;padding:4px 10px" onclick="delCard('${c.uid}')">Remove</button></td></tr>`).join('');
  });
}
function startScan(){
  api('POST','/api/rfid/scan/start').then(r=>{
    if(r.status!=='scanning')return;
    document.getElementById('scan-modal').classList.add('show');
    document.getElementById('scan-msg').textContent='Place card on reader...';
    document.getElementById('scan-form').style.display='none';
    document.getElementById('save-btn').style.display='none';
    const poll=()=>{
      api('GET','/api/rfid/scan/status').then(s=>{
        if(s.status==='detected'){
          document.getElementById('scan-msg').textContent='Card detected!';
          document.getElementById('scan-form').style.display='block';
          document.getElementById('scan-uid').value=s.uid;
          document.getElementById('scan-name').value='New Card';
          document.getElementById('save-btn').style.display='inline-block';
        }else if(s.status==='scanning')setTimeout(poll,600);
        else document.getElementById('scan-msg').textContent='Timeout -- try again';
      });
    };
    setTimeout(poll,800);
  });
}
function saveCard(){
  const uid=document.getElementById('scan-uid').value;
  const name=(document.getElementById('scan-name').value.trim())||'Card';
  api('POST','/api/rfid/add',{uid,name}).then(r=>{
    if(r.status==='success'){cancelScan();loadRFID();sa('rfid-alert','ok','Card saved!');}
    else sa('rfid-alert','error',r.message||'Failed');
  });
}
function cancelScan(){document.getElementById('scan-modal').classList.remove('show');api('POST','/api/rfid/scan/cancel');}
function delCard(uid){if(!confirm('Remove '+uid+'?'))return;api('POST','/api/rfid/delete',{uid}).then(loadRFID);}
function loadLogs(){
  api('GET','/api/logs?limit=50').then(d=>{
    const c=document.getElementById('log-wrap');
    if(!d||!d.length){c.innerHTML='<div style="text-align:center;color:var(--text2);padding:20px">No logs yet</div>';return;}
    c.innerHTML=d.map(l=>`<div class="le"><span style="color:var(--text2);margin-right:8px">${l.ts}</span><span style="font-weight:700;margin-right:8px;color:${l.type==='GRANTED'?'var(--success)':l.type==='DENIED'?'var(--danger)':'var(--warn)'}">${l.type}</span><span style="color:var(--accent);margin-right:8px">${l.user}</span><span style="color:var(--text2)">${l.detail}</span></div>`).join('');
  });
}
const DAYS=['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];
let schData=[];
function loadSch(){
  api('GET','/api/schedule').then(d=>{
    schData=Array.isArray(d)?d:[];
    const master=document.getElementById('sch-master'),div=document.getElementById('sch-days');
    master.checked=schData.length>0;div.style.display=schData.length>0?'block':'none';renderDays();
  }).catch(()=>{});
}
function toggleSch(){
  const active=document.getElementById('sch-master').checked;
  document.getElementById('sch-days').style.display=active?'block':'none';
  if(active&&schData.length===0)schData=DAYS.map((_,i)=>({day:i,start:480,end:1020}));
  if(!active)schData=[];renderDays();
}
function renderDays(){
  const c=document.getElementById('day-rows');c.innerHTML='';
  DAYS.forEach((name,idx)=>{
    const obj=schData.find(d=>d.day===idx)||null;
    const checked=obj?'checked':'';const s=obj?m2t(obj.start):'08:00';const e=obj?m2t(obj.end):'17:00';
    c.innerHTML+=`<div class="day-row"><label><input type="checkbox" data-day="${idx}" ${checked} onchange="hdc(this,${idx})"> ${name}</label><div class="ti"><input type="time" id="s-${idx}" value="${s}" onchange="htc(${idx})"><span style="color:var(--text2)">to</span><input type="time" id="e-${idx}" value="${e}" onchange="htc(${idx})"></div></div>`;
  });
}
function hdc(cb,day){
  if(cb.checked){let o=schData.find(d=>d.day===day);if(!o){o={day,start:480,end:1020};schData.push(o);}o.start=t2m(document.getElementById('s-'+day).value);o.end=t2m(document.getElementById('e-'+day).value);}
  else schData=schData.filter(d=>d.day!==day);
}
function htc(day){const o=schData.find(d=>d.day===day);if(o){o.start=t2m(document.getElementById('s-'+day).value);o.end=t2m(document.getElementById('e-'+day).value);}}
function saveSch(){
  if(!document.getElementById('sch-master').checked)schData=[];
  api('POST','/api/schedule',schData).then(()=>sa('sch-alert','ok','Schedule saved!')).catch(()=>sa('sch-alert','error','Save failed'));
}
function t2m(t){if(!t)return 0;const p=t.split(':');return parseInt(p[0])*60+parseInt(p[1]);}
function m2t(m){return String(Math.floor(m/60)).padStart(2,'0')+':'+String(m%60).padStart(2,'0');}
function updateWiFi(){const s=document.getElementById('w-ssid').value,p=document.getElementById('w-pass').value;if(!s){alert('Enter SSID');return;}api('POST','/api/wifi',{ssid:s,password:p}).then(()=>alert('WiFi updated -- reconnecting...'));}
function changePass(){const o=document.getElementById('p-old').value,n=document.getElementById('p-new').value;if(!o||!n){alert('Fill all fields');return;}api('POST','/api/admin/password',{oldPassword:o,newPassword:n}).then(r=>{if(r.status==='changed')alert('Password changed!');else alert('Wrong current password');});}
function logout(){localStorage.removeItem('aegis_token');window.location.href='/';}
updateStatus();setInterval(updateStatus,4000);
</script></body></html>)rawliteral";
}

// ==================== WEB SERVER ====================
void webInit() {
  const char* hkeys[]={"X-Auth-Token"};
  server.collectHeaders(hkeys,1);
  server.on("/",[](){server.send(200,"text/html",loginPage());});
  server.on("/dashboard",[](){
    String tok=server.hasHeader("X-Auth-Token")?server.header("X-Auth-Token"):server.arg("token");
    if(tok.length()>0&&tok==currentToken&&millis()-sessionTimeout<SESSION_TIMEOUT_MS){sessionTimeout=millis();server.send(200,"text/html",dashboardPage());}
    else{server.sendHeader("Location","/");server.send(302,"text/plain","Redirect");}
  });
  server.on("/api/login",HTTP_POST,[](){
    JsonDocument doc;deserializeJson(doc,server.arg("plain"));
    String user=doc["username"]|"",pass=doc["password"]|"";
    if(user==prefs.getString("admin_user",ADMIN_USER)&&pass==prefs.getString("admin_pass",ADMIN_PASS)){
      currentToken=generateToken();sessionTimeout=millis();
      JsonDocument r;r["success"]=true;r["token"]=currentToken;
      String j;serializeJson(r,j);server.send(200,"application/json",j);
      addLog("REMOTE","WEB",user,"Admin logged in");
    } else {server.send(401,"application/json","{\"success\":false,\"message\":\"Invalid credentials\"}");addLog("DENIED","WEB","Unknown","Failed login");}
  });
  server.on("/api/verify-token",HTTP_GET,[](){server.send(200,"application/json",String("{\"valid\":")+( isAuthorized()?"true":"false")+"}");});
  server.on("/api/status",HTTP_GET,[](){
    if(!isAuthorized()){sendUnauthorized();return;}
    // Only poll sensor if not enrolling to avoid conflicts
    if (!enrollPending && fpAvailable) {
      lastFpCount = finger.templateCount;
    }
    JsonDocument doc;
    doc["locked"]=doorLocked;
    doc["rssi"]=(wifiMode==1)?WiFi.RSSI():0;
    doc["uptime"]=millis()/1000;
    doc["fp_count"]=fpAvailable ? lastFpCount : 0;
    doc["card_count"]=prefs.getInt("card_count",0);
    doc["ip"]=(wifiMode==1)?WiFi.localIP().toString():WiFi.softAPIP().toString();
    doc["access_today"]=accessToday;doc["fails_today"]=failsToday;
    doc["wifi_mode"]=wifiMode;
    doc["ap_clients"]=(wifiMode==2)?WiFi.softAPgetStationNum():0;
    String j;serializeJson(doc,j);server.send(200,"application/json",j);
  });
  server.on("/api/lock/toggle",HTTP_POST,[](){
    if(!isAuthorized()){sendUnauthorized();return;}
    server.send(200,"application/json","{\"status\":\"ok\"}");
    if(doorLocked){beepRemoteUnlock();unlockDoor();}else lockDoor();
  });
  server.on("/api/lock",HTTP_POST,[](){
    if(!isAuthorized()){sendUnauthorized();return;}
    server.send(200,"application/json","{\"status\":\"ok\"}");lockDoor();
  });
  server.on("/api/fingerprints",HTTP_GET,[](){if(!isAuthorized()){sendUnauthorized();return;}server.send(200,"application/json",getFingerprintsJSON());});
  server.on("/api/fingerprint/enroll",HTTP_POST,[](){
    if(!isAuthorized()){sendUnauthorized();return;}
    if(!fpAvailable){server.send(503,"application/json","{\"error\":\"Sensor unavailable\"}");return;}
    if(enrollPending){server.send(409,"application/json","{\"error\":\"Already enrolling\"}");return;}
    JsonDocument doc;deserializeJson(doc,server.arg("plain"));
    enrollName=doc["name"]|"User";enrollPending=true;enrollStage=1;enrollResult="";
    server.send(200,"application/json","{\"status\":\"started\"}");
  });
  server.on("/api/rfid/cards",HTTP_GET,[](){if(!isAuthorized()){sendUnauthorized();return;}server.send(200,"application/json",getRFIDJSON());});
  server.on("/api/rfid/scan/start",HTTP_POST,[](){
    if(!isAuthorized()){sendUnauthorized();return;}
    rfidWebScanActive=true;rfidWebScanUID="";rfidWebScanTimeout=millis()+RFID_WEB_SCAN_TIMEOUT;
    server.send(200,"application/json","{\"status\":\"scanning\",\"timeout\":"+String(RFID_WEB_SCAN_TIMEOUT)+"}");
  });
  server.on("/api/rfid/scan/status",HTTP_GET,[](){
    if(!isAuthorized()){sendUnauthorized();return;}
    JsonDocument doc;
    if(!rfidWebScanActive&&rfidWebScanUID.length()>0){doc["status"]="detected";doc["uid"]=rfidWebScanUID;rfidWebScanUID="";}
    else if(rfidWebScanActive&&millis()>rfidWebScanTimeout){doc["status"]="timeout";rfidWebScanActive=false;}
    else if(rfidWebScanActive)doc["status"]="scanning";
    else doc["status"]="idle";
    String j;serializeJson(doc,j);server.send(200,"application/json",j);
  });
  server.on("/api/rfid/scan/cancel",HTTP_POST,[](){rfidWebScanActive=false;server.send(200,"application/json","{\"status\":\"cancelled\"}");});
  server.on("/api/rfid/add",HTTP_POST,[](){
    if(!isAuthorized()){sendUnauthorized();return;}
    JsonDocument doc;deserializeJson(doc,server.arg("plain"));
    addCard(cleanUID(doc["uid"]|""),doc["name"]|"Card")
      ?server.send(200,"application/json","{\"status\":\"success\"}")
      :server.send(409,"application/json","{\"status\":\"error\",\"message\":\"Already exists\"}");
  });
  server.on("/api/rfid/delete",HTTP_POST,[](){
    if(!isAuthorized()){sendUnauthorized();return;}
    JsonDocument doc;deserializeJson(doc,server.arg("plain"));
    removeCard(doc["uid"]|"")
      ?server.send(200,"application/json","{\"status\":\"deleted\"}")
      :server.send(404,"application/json","{\"error\":\"Card not found\"}");
  });
  server.on("/api/logs",HTTP_GET,[](){if(!isAuthorized()){sendUnauthorized();return;}server.send(200,"application/json",getLogsJSON(50));});
  server.on("/api/wifi",HTTP_POST,[](){
    if(!isAuthorized()){sendUnauthorized();return;}
    JsonDocument doc;deserializeJson(doc,server.arg("plain"));
    prefs.putString("wifi_ssid",doc["ssid"]|"");prefs.putString("wifi_pass",doc["password"]|"");
    server.send(200,"application/json","{\"status\":\"saved\"}");
    delay(1500);networkInit();
  });
  server.on("/api/admin/password",HTTP_POST,[](){
    if(!isAuthorized()){sendUnauthorized();return;}
    JsonDocument doc;deserializeJson(doc,server.arg("plain"));
    if(String(doc["oldPassword"]|"")==prefs.getString("admin_pass",ADMIN_PASS)){
      prefs.putString("admin_pass",doc["newPassword"]|"");server.send(200,"application/json","{\"status\":\"changed\"}");
    } else server.send(403,"application/json","{\"error\":\"Wrong password\"}");
  });
  server.on("/api/schedule",HTTP_GET,[](){if(!isAuthorized()){sendUnauthorized();return;}server.send(200,"application/json",prefs.getString("schedule_global","[]"));});
  server.on("/api/schedule",HTTP_POST,[](){
    if(!isAuthorized()){sendUnauthorized();return;}
    JsonDocument doc;deserializeJson(doc,server.arg("plain"));
    String s;serializeJson(doc,s);prefs.putString("schedule_global",s);server.send(200,"application/json","{\"status\":\"saved\"}");
  });
  server.onNotFound([](){
    String uri=server.uri();HTTPMethod meth=server.method();
    if(meth==HTTP_DELETE&&uri.startsWith("/api/fingerprint/")){
      if(!isAuthorized()){sendUnauthorized();return;}
      int id=uri.substring(uri.lastIndexOf('/')+1).toInt();
      if(id<1||id>127){server.send(400,"application/json","{\"error\":\"Invalid ID\"}");return;}
      deleteFP(id)?server.send(200,"application/json","{\"status\":\"deleted\"}"):server.send(404,"application/json","{\"error\":\"Not found\"}");
      return;
    }
    server.send(404,"application/json","{\"error\":\"Not found\"}");
  });
  server.begin();
  Serial.println("[WEB] Server started");
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200); delay(500);
  Serial.println("\n=== ESP32 SMART DOOR LOCK v4.1 ===");
  pinMode(RELAY_PIN,OUTPUT);pinMode(LED_GREEN,OUTPUT);pinMode(LED_RED,OUTPUT);
  digitalWrite(RELAY_PIN,RELAY_LOCK);digitalWrite(LED_GREEN,LOW);digitalWrite(LED_RED,LOW);
  pinMode(BUTTON_PIN,INPUT_PULLUP);

  buzzerInit(); beepPowerUp();
  displayInit();

  otaSetCallbacks();
  showBootSequence();
  SPI.begin(); rfidInit();
  prefs.begin("aegis",false);
  loadPersistentLogs();
  accessToday=prefs.getUInt("access_today",0);failsToday=prefs.getUInt("fails_today",0);
  if(prefs.getInt("first_boot",0)==0){
    prefs.putInt("first_boot",1);prefs.putString("admin_user",ADMIN_USER);prefs.putString("admin_pass",ADMIN_PASS);
    addCard("4DC33F06","Default Card");Serial.println("[SETUP] First boot defaults written");
  }
  fpInit();
  networkInit();
  webInit();
  beepSystemReady();
  Serial.println("[SYSTEM] AEGIS v4.1 Ready  mode=" + String(wifiMode==1?"STA":"AP"));
}

// ==================== LOOP ====================
void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  updateBuzzer();

  // Interior button
  bool br=digitalRead(BUTTON_PIN);
  if(br!=lastButtonReading){lastButtonDebounce=millis();lastButtonReading=br;}
  if((millis()-lastButtonDebounce)>BUTTON_DEBOUNCE_MS){
    if(br!=lastButtonState){
      lastButtonState=br;
      if(lastButtonState==LOW&&doorLocked){
        Serial.println("[BUTTON] Interior press -- unlocking");
        beepRemoteUnlock();addLog("GRANTED","BUTTON","Interior","Manual unlock");unlockDoor();
      }
    }
  }

  // Non‑blocking fingerprint enrollment with animation
  if(enrollPending) {
    runEnroll();
    showEnrollmentAnimation();
    server.handleClient();
    ArduinoOTA.handle();
    updateBuzzer();

    if((enrollStage == 4 || enrollStage == 99) && millis() > enrollCompletionTime) {
      enrollPending = false;
      enrollStage = 0;
      showIdleAnimation();
    }
    delay(10);
    return;
  }

  // WiFi maintenance
  unsigned long now=millis();
  if(wifiMode==1){
    if(now-lastWifiCheck>WIFI_CHECK_MS){
      lastWifiCheck=now;
      if(WiFi.status()!=WL_CONNECTED){
        Serial.println("[WIFI] STA lost -- reconnecting");WiFi.reconnect();delay(3000);
        if(WiFi.status()!=WL_CONNECTED){Serial.println("[WIFI] Reconnect failed -- switching to AP");networkInit();}
      }
    }
  } else if(wifiMode==2){
    if(now-lastApRetry>WIFI_RETRY_MS){
      lastApRetry=now;
      Serial.println("[WIFI] AP mode -- retrying STA...");
      String ssid=prefs.getString("wifi_ssid",MY_SSID);
      String pass=prefs.getString("wifi_pass",MY_PASS);
      WiFi.mode(WIFI_STA);WiFi.begin(ssid.c_str(),pass.c_str());
      int t=0;while(WiFi.status()!=WL_CONNECTED&&t<20){delay(500);t++;}
      if(WiFi.status()==WL_CONNECTED){
        Serial.println("[WIFI] STA reconnected: "+WiFi.localIP().toString());
        wifiMode=1;beepWiFiConnected();
        configTime(0,0,"pool.ntp.org","time.nist.gov");
        struct tm ti;if(getLocalTime(&ti))rtc.setTime(ti.tm_sec,ti.tm_min,ti.tm_hour,ti.tm_mday,ti.tm_mon+1,ti.tm_year+1900);
        otaBegin();
      } else {
        WiFi.mode(WIFI_AP);WiFi.softAP(AP_SSID,AP_PASS);
        Serial.println("[WIFI] STA retry failed -- staying in AP mode");
      }
    }
  }

  if(now-lastStatsFlush>60000){lastStatsFlush=now;saveStats();}

  // RFID health check
  byte ver=rfid.PCD_ReadRegister(rfid.VersionReg);
  if(ver==0x00||ver==0xFF){if(++rfidFailCount>=RFID_FAIL_LIMIT)rfidInit();}
  else rfidFailCount=0;

  // RFID card scan
  if(rfid.PICC_IsNewCardPresent()&&rfid.PICC_ReadCardSerial()){
    beepShort();String uid=readRFID();
    rfid.PICC_HaltA();rfid.PCD_StopCrypto1();
    if(rfidWebScanActive){rfidWebScanUID=uid;rfidWebScanActive=false;Serial.println("[RFID] Web-scan: "+uid);}
    else {
      bool inCooldown=(uid==lastCardUID)&&(millis()-lastCardTime<COOLDOWN_MS);
      if(!inCooldown){
        lastCardUID=uid;lastCardTime=millis();
        if(verifyRFID(uid)){
          if(!checkAccessAllowed()){addLog("DENIED","RFID",getRFIDName(uid),"Outside allowed hours");beepFail();showAccessDenied();digitalWrite(LED_RED,HIGH);delay(2000);digitalWrite(LED_RED,LOW);}
          else{addLog("GRANTED","RFID",getRFIDName(uid),uid);beepOK();unlockDoor();}
        } else {addLog("DENIED","RFID","Unknown",uid);beepFail();showAccessDenied();digitalWrite(LED_RED,HIGH);delay(2000);digitalWrite(LED_RED,LOW);}
      }
    }
  }

  // Fingerprint scan
  if(fpAvailable&&!rfidWebScanActive&&millis()-lastFpCheck>FP_POLL_MS){
    lastFpCheck=millis();int id=scanFP();
    if(id>=1){
      String name=prefs.getString(("fp_name_"+String(id)).c_str(),"User #"+String(id));
      if(!checkAccessAllowed()){addLog("DENIED","FP",name,"Outside allowed hours");beepFail();showAccessDenied();}
      else{addLog("GRANTED","FP",name,"ID "+String(id));beepOK();unlockDoor();}
    } else if(id==-5){addLog("DENIED","FP","Unknown","No match");beepFail();showAccessDenied();delay(1000);}
  }

  if(rfidWebScanActive&&millis()>rfidWebScanTimeout)rfidWebScanActive=false;

  showIdleAnimation();
  delay(10);
}

#include <SoftwareSerial.h>
#include <DHT.h>              // https://github.com/adafruit/DHT-sensor-library
#include <ESP8266WiFi.h>      // ESP8266 Core WiFi Library (you most likely already have this in your sketch)
#include <DNSServer.h>        // Local DNS Server used for redirecting all requests to the configuration portal
#include <ESP8266WebServer.h> // Local WebServer used to serve the configuration portal
#include <WiFiManager.h>      // https://github.com/tzapu/WiFiManager WiFi Configuration Magic
#include "base64.h"           // For auth in InfluxDb
#include "WiFiUtils.h"        // Class implements RSSI and SSID methods for WiFi
#include "config.h"           // Our config. Must exist before uploading. You can look to config_example.h for reference

#define INTERVAL 5000            // Poll sensors and send metrics every 5 sec
#define DHTPIN D5                // Pin where DHT 22 data connected
#define DHTTYPE DHT22            // DHT 22 (AM2302), AM2321
#define MH_Z19_TX D7             // Pin where MH-Z19 TX connected (sensor will return -1 if this pins messed)
#define MH_Z19_RX D6             // Pin where MH-Z19 RX connected (sensor will return -1 if this pins messed)
#define WIFI_MAX_ATTEMPTS_INIT 3 // set to 0 for unlimited, do not use more then 65535
#define WIFI_MAX_ATTEMPTS_SEND 1 // set to 0 for unlimited, do not use more then 65535
#define MAX_DATA_ERRORS 12       // max of errors, reset after them
#define USE_GOOGLE_DNS true

char ssid[] = "11111111";
char pass[] = "11111111";
long previousMillis = 0;
int errorCount = 0;
WiFiUtils wifiUtils;
DHT dht(DHTPIN, DHTTYPE);
SoftwareSerial co2Serial(MH_Z19_TX, MH_Z19_RX);
String auth = base64::encode(String(DATA_USER) + ":" + String(DATA_PASS));

void(* resetFunc) (void) = 0;

int readCO2() {
  byte cmd[9] = {0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79};
  char response[9];
  co2Serial.write(cmd, 9);

  co2Serial.readBytes(response, 9);
  if (response[0] != 0xFF)
  {
    Serial.println("wrong starting byte from co2 sensor!");
    return -1;
  }

  if (response[1] != 0x86)
  {
    Serial.println("wrong command from co2 sensor!");
    return -1;
  }

  int responseHigh = (int) response[2];
  int responseLow = (int) response[3];
  int ppm = (256 * responseHigh) + responseLow;
  return ppm;
}

void sendData(String data, int dataSize) {
  Serial.println("start send");
  Serial.print("data: ");
  Serial.println(data);

  Serial.print("server: ");
  Serial.println(String(DATA_SERVER) + " " + String(DATA_PORT));
  BearSSL::WiFiClientSecure client;
  client.setInsecure();

  client.connect(DATA_SERVER, DATA_PORT);
  if (!client.connected()) {
    Serial.println("could not connect to InfluxDB Server");
    return;
  }
  Serial.println("connected!");

  client.println("POST /write?db=" + String(DATA_DB) + " HTTP/1.1");
  client.println("Authorization: Basic " + auth);
  client.println("Host: " + String(DATA_SERVER));
  client.println("User-Agent: ESP8266/1.0");
  client.println("Connection: close");
  client.println("Content-Type: application/x-www-form-urlencoded");
  client.print("Content-Length: ");
  client.println(dataSize);
  client.println();
  client.println(data);

  uint32_t to = millis() + 4000;
  if (client.connected()) {
    do {
      char tmp[32];
      memset(tmp, 0, 32);
      int rlen = client.read((uint8_t*)tmp, sizeof(tmp) - 1);
      yield();
      if (rlen < 0) {
        break;
      }
      // Only print out first line up to \r, then abort connection
      char *nl = strchr(tmp, '\r');
      if (nl) {
        *nl = 0;
        Serial.print(tmp);
        break;
      }
      Serial.print(tmp);
    } while (millis() < to);
  }
  client.stop();

  Serial.println(" | finish send");
}

void setup() {
  Serial.begin(115200);
  Serial.println("setup started");
  
  unsigned long previousMillis = millis();
  co2Serial.begin(9600);
  dht.begin();

  if (WiFi.status() == WL_NO_SHIELD) {
    Serial.println("WiFi shield not present");
    while (true)
      delay(1000);
  }

  unsigned int attempt = 0;
  while ( WiFi.status() != WL_CONNECTED) {
    if (WIFI_MAX_ATTEMPTS_INIT != 0 && attempt > WIFI_MAX_ATTEMPTS_INIT)
      break;
    if (attempt >= 65535)
      attempt = 0;
    attempt++;
    Serial.print("attempting to connect to WPA SSID: ");
    Serial.println(ssid);

    WiFiManager wifiManager;
    wifiManager.autoConnect("OfficeStation", AP_PASS);
  }

  if (USE_GOOGLE_DNS)
    wifiUtils.setGoogleDNS();

  Serial.println("connected to network");
  wifiUtils.printCurrentNet();
  wifiUtils.printWifiData();

  Serial.println("code written by Antony Ryabov http://bit.ly/2nvmJcX");
  Serial.println("waiting for sensors to init");
  while (millis() - previousMillis < 10000)
    delay(1000);

  Serial.println("setup finished");
  Serial.println("");
}

void loop() {
  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis < INTERVAL)
    return;
  previousMillis = currentMillis;
  Serial.println("loop started");

  if (errorCount > MAX_DATA_ERRORS)
  {
    Serial.println("too many errors, resetting");
    delay(2000);
    resetFunc();
  }
  Serial.println("reading data");
  int ppm = readCO2();
  bool dataError = false;
  Serial.println("PPM = " + String(ppm));

  if (ppm < 100 || ppm > 6000)
  {
    Serial.println("PPM not valid");
    dataError = true;
  }
  int mem = ESP.getFreeHeap();
  Serial.println("RAM = " + String(mem));

  float h = dht.readHumidity();
  float t = dht.readTemperature();
  float hic = dht.computeHeatIndex(t, h, false);

  Serial.print("humidity = ");
  Serial.println(h);
  Serial.print("temp = ");
  Serial.println(t);
  Serial.print("heat index = ");
  Serial.println(hic);

  if (t < 5 || t > 80 )
  {
    Serial.println("temperature not valid");
    dataError = true;
  }

  if (h > 100 || h == 0)
  {
    Serial.println("humidity not valid");
    dataError = true;
  }

  if (dataError)
  {
    Serial.println("skipping loop");
    errorCount++;
    return;
  }
  errorCount = 0;

  String buf = "station_metrics";
  buf = String(buf + ",ID=" + DATA_SENSOR_ID + ",MAC=\"" + wifiUtils.macStr() + "\",NAME=\"" + DATA_NAME + "\" " + "TEMP=" + String(t, 2) + ",HUMIDITY=" + String(h, 2) + ",HEATINDEX=" + String(hic, 2) + ",PPM=" + ppm + ",FREERAM=" + mem + ",RSSI=" + WiFi.RSSI() + ",SSID=\"" + WiFi.SSID().c_str()) + "\"";

  sendData(buf, buf.length());

  Serial.println("loop finished");
  Serial.println("");
}

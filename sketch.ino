/*
  DataCenter Andino S.A.C. - Nodo IoT de la sala de servidores
  ESP32 + DHT22 (temperatura/humedad) + LDR (iluminacion) -> MQTT

  Publica cada INTERVALO_MS un JSON en   <TOPIC_BASE>/ambiente
  Estado del nodo (Last Will) en         <TOPIC_BASE>/estado  ("online" / "offline")
*/
#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include <time.h>
#include <sys/time.h>

// ======================= CONFIGURACION =======================
#define WIFI_SSID   "Wokwi-GUEST"
#define WIFI_PASS   ""
#define WIFI_CANAL  6

#define MQTT_HOST   "broker.hivemq.com"
#define MQTT_PORT   1883

// Grupo
#define TOPIC_BASE   "dcandino/grupoWhari/sala1"
#define TOPIC_DATA   TOPIC_BASE "/ambiente"
#define TOPIC_STATUS TOPIC_BASE "/estado"

#define DEVICE_ID   "esp32-sala1"

#define DHT_PIN     15
#define DHT_TYPE    DHT22
#define LDR_PIN     34

const unsigned long INTERVALO_MS = 5000;
const unsigned long REINTENTO_MS = 5000;

// Calibracion del LDR (ADC 12 bits: 0..4095).
// En Wokwi: oscuro -> raw alto (0 %), luminoso -> raw bajo (100 %).
// Si te sale al reves, intercambia estos dos valores.
const int LDR_RAW_0PCT   = 4095;
const int LDR_RAW_100PCT = 0;
const int LDR_MUESTRAS   = 8;

// ======================= OBJETOS Y ESTADO =======================
DHT dht(DHT_PIN, DHT_TYPE);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

unsigned long ultimoEnvio = 0;
uint32_t secuencia = 0;

// ======================= UTILIDADES =======================
float redondear1(float v) { return roundf(v * 10.0f) / 10.0f; }

int leerLdrRaw() {
  long suma = 0;
  for (int i = 0; i < LDR_MUESTRAS; i++) {
    suma += analogRead(LDR_PIN);
    delay(2);
  }
  return (int)(suma / LDR_MUESTRAS);
}

int ldrAPorcentaje(int raw) {
  long pct = map(raw, LDR_RAW_0PCT, LDR_RAW_100PCT, 0, 100);
  return (int)constrain(pct, 0, 100);
}

// ======================= CONEXIONES =======================
void conectarWiFiInicial() {
  Serial.print("[WiFi] Conectando");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CANAL);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.printf(" OK  IP=%s\n", WiFi.localIP().toString().c_str());
}

void sincronizarHora() {
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  Serial.print("[NTP] Sincronizando");
  unsigned long t0 = millis();
  while (time(nullptr) < 1700000000L && millis() - t0 < 8000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println(time(nullptr) >= 1700000000L ? " OK" : " sin hora (se omite ts)");
}

bool asegurarWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  static unsigned long ultimo = 0;
  if (millis() - ultimo >= REINTENTO_MS) {
    ultimo = millis();
    Serial.println("[WiFi] Conexion perdida, reintentando...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CANAL);
  }
  return false;
}

bool asegurarMQTT() {
  if (mqtt.connected()) return true;
  static unsigned long ultimo = 0;
  if (ultimo != 0 && millis() - ultimo < REINTENTO_MS) return false;
  ultimo = millis();

  String clientId = String(DEVICE_ID) + "-" + String((uint32_t)esp_random(), HEX);
  Serial.printf("[MQTT] Conectando a %s:%d como %s ... ", MQTT_HOST, MQTT_PORT, clientId.c_str());

  // Last Will: si el ESP32 cae sin avisar, el broker publica "offline"
  if (mqtt.connect(clientId.c_str(), NULL, NULL, TOPIC_STATUS, 1, true, "offline")) {
    Serial.println("OK");
    mqtt.publish(TOPIC_STATUS, "online", true);
    return true;
  }
  Serial.printf("fallo rc=%d\n", mqtt.state());
  return false;
}

// ======================= PUBLICACION =======================
void publicarLectura() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  int raw = leerLdrRaw();
  int pct = ldrAPorcentaje(raw);

  JsonDocument doc;
  doc["device_id"] = DEVICE_ID;
  doc["seq"] = ++secuencia;

  struct timeval tv;
  gettimeofday(&tv, nullptr);
  if (tv.tv_sec > 1700000000L) {
    doc["ts"]    = (uint32_t)tv.tv_sec;
    doc["ts_ms"] = (uint16_t)(tv.tv_usec / 1000);
  }

  if (isnan(t)) doc["temp"] = nullptr; else doc["temp"] = redondear1(t);
  if (isnan(h)) doc["hum"]  = nullptr; else doc["hum"]  = redondear1(h);

  doc["luz_raw"] = raw;
  doc["luz_pct"] = pct;
  doc["rssi"]    = WiFi.RSSI();

  char payload[320];
  serializeJson(doc, payload, sizeof(payload));
  bool ok = mqtt.publish(TOPIC_DATA, payload);
  Serial.printf("[%s] %s -> %s\n", ok ? "PUB" : "ERR", TOPIC_DATA, payload);
}

// ======================= ARDUINO =======================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Nodo IoT DataCenter Andino ===");
  dht.begin();
  analogReadResolution(12);
  pinMode(LDR_PIN, INPUT);

  conectarWiFiInicial();
  sincronizarHora();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(15);
}

void loop() {
  if (!asegurarWiFi()) return;
  if (!asegurarMQTT()) return;
  mqtt.loop();

  if (millis() - ultimoEnvio >= INTERVALO_MS) {
    ultimoEnvio = millis();
    publicarLectura();
  }
  delay(10);
}
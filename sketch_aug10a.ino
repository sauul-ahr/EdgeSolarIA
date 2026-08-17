#include <WiFi.h>
#include <HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <LiquidCrystal.h>

// ======================================================
// 1. CONFIGURACIÓN DE RED Y API
// ======================================================
const char* ssid = "iPhone de Saul"; 
const char* password = "123456789"; 

const char* server = "http://api.thingspeak.com/update";
String apiKey = "P45LF4JJ74JPT5JO"; 

// ======================================================
// 2. CONFIGURACIÓN PANTALLA LCD PARALELA (SIN I2C)
// ======================================================
const int rs = 13, en = 12, d4 = 14, d5 = 27, d6 = 26, d7 = 25;
LiquidCrystal lcd(rs, en, d4, d5, d6, d7);

// Carácter personalizado '°'
byte grado[8] = {
  B00110,
  B01001,
  B01001,
  B00110,
  B00000,
  B00000,
  B00000,
  B00000
};

// ======================================================
// 3. CONFIGURACIÓN DEL SENSOR Y RELAY
// ======================================================
const int PIN_DS18B20 = 4; // GPIO 4
OneWire oneWire(PIN_DS18B20);
DallasTemperature sensors(&oneWire);

const int PIN_RELAY = 5;   // GPIO 5

// ======================================================
// 4. PARÁMETROS IA Y HISTÉRESIS
// ======================================================
const float TEMP_ACTIVACION_BASE = 35.0; 
const float TEMP_DESACTIVACION = 30.0;   

const int BUFFER_SIZE = 5; 
float historialTemp[BUFFER_SIZE];
unsigned long historialTiempo[BUFFER_SIZE];
int indiceBuffer = 0;
bool bufferLleno = false;

bool ventiladorEncendido = false;

// Variables compartidas entre tareas
float tempPanelGlobal = 0.0;
float tempPredichaGlobal = 0.0;

// Variables de control de tiempo con millis()
unsigned long ultimoMuestreoSensor = 0;
unsigned long ultimoEnvioThingSpeak = 0; 

const unsigned long INTERVALO_SENSOR = 2000;       // Control/Lectura local cada 2s
const unsigned long INTERVALO_THINGSPEAK = 15000; // Envío a la nube cada 15s

// ======================================================
// FUNCIONES DE LA IA EDGE (REGRESIÓN LINEAL)
// ======================================================
float calcularPendienteCalentamiento() {
  int n = bufferLleno ? BUFFER_SIZE : indiceBuffer;
  if (n < 2) return 0.0;

  float sumX = 0, sumY = 0, sumXY = 0, sumX2 = 0;
  unsigned long t0 = historialTiempo[0];

  for (int i = 0; i < n; i++) {
    float x = (historialTiempo[i] - t0) / 1000.0; // segundos
    float y = historialTemp[i];

    sumX += x;
    sumY += y;
    sumXY += x * y;
    sumX2 += x * x;
  }

  float denominador = (n * sumX2 - sumX * sumX);
  if (denominador == 0) return 0.0;

  return (n * sumXY - sumX * sumY) / denominador;
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n=========================================");
  Serial.println("  SISTEMA DE ENFRIAMIENTO SOLAR EDGE IA  ");
  Serial.println("=========================================");

  // Configuración e inicialización del relé (ACTIVE LOW)
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, HIGH); // HIGH apaga el relé
  ventiladorEncendido = false;

  // Inicialización Pantalla LCD (16x2)
  lcd.begin(16, 2);
  lcd.createChar(0, grado);
  
  lcd.setCursor(0, 0);
  lcd.print("  EDGE SOLAR IA ");
  lcd.setCursor(0, 1);
  lcd.print("Iniciando...");
  delay(1500);

  sensors.begin();

  // Conexión Wi-Fi
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Conectando WiFi");
  Serial.print("Conectando a red WiFi: ");
  Serial.println(ssid);
  
  WiFi.begin(ssid, password);
  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 20) {
    delay(500);
    lcd.setCursor(intentos % 16, 1);
    lcd.print(".");
    Serial.print(".");
    intentos++;
  }  
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi Conectado!");
    Serial.println("WiFi Conectado exitosamente.");
    Serial.print("Direccion IP asignada: ");
    Serial.println(WiFi.localIP());
    delay(1000);
  } else {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Sin WiFi!");
    lcd.setCursor(0, 1);
    lcd.print("Modo Offline");
    Serial.println("No se pudo conectar a WiFi. Operando en MODO OFFLINE.");
    delay(1500);
  }

  lcd.clear();
  
  // Establecer tiempos de inicio de temporizadores
  unsigned long tInicial = millis();
  ultimoMuestreoSensor = tInicial;
  ultimoEnvioThingSpeak = tInicial;

  Serial.println("=========================================\n");
}

void loop() {
  unsigned long tiempoActual = millis();

  // ======================================================
  // TAREA 1: MONITOREO Y CONTROL EN TIEMPO REAL (Cada 2 s)
  // ======================================================
  if (tiempoActual - ultimoMuestreoSensor >= INTERVALO_SENSOR) {
    ultimoMuestreoSensor = tiempoActual;

    sensors.requestTemperatures(); 
    float tempPanel = sensors.getTempCByIndex(0);

    if (tempPanel != DEVICE_DISCONNECTED_C && tempPanel > -55.0) {
      
      // Guardar en buffer circular para IA Edge
      historialTemp[indiceBuffer] = tempPanel;
      historialTiempo[indiceBuffer] = tiempoActual;
      indiceBuffer = (indiceBuffer + 1) % BUFFER_SIZE;
      if (indiceBuffer == 0) bufferLleno = true;

      float pendiente = calcularPendienteCalentamiento();
      float tempPredicha1Min = tempPanel + (pendiente * 60.0);

      // Guardar en variables globales compartidas con la tarea HTTP
      tempPanelGlobal = tempPanel;
      tempPredichaGlobal = tempPredicha1Min;

      // Evaluación del Umbral Adaptativo mediante IA
      float umbralActivacionAdaptativo = TEMP_ACTIVACION_BASE;
      if (pendiente >= 0.02) {
        umbralActivacionAdaptativo = 32.5;
      } else if (pendiente >= 0.01) {
        umbralActivacionAdaptativo = 33.5;
      }

      // --- CONTROL DEL RELÉ (LÓGICA INVERTIDA ACTIVE-LOW) ---
      if (tempPanel <= TEMP_DESACTIVACION) {
        if (ventiladorEncendido) {
          digitalWrite(PIN_RELAY, HIGH); // HIGH APAGA el relé
          ventiladorEncendido = false;
          Serial.println("\n[EVENTO RELE] >>> VENTILADOR APAGADO <<<");
        }
      } 
      else if ((tempPanel >= umbralActivacionAdaptativo || tempPredicha1Min >= TEMP_ACTIVACION_BASE) && !ventiladorEncendido) {
        digitalWrite(PIN_RELAY, LOW); // LOW ENCIENDE el relé
        ventiladorEncendido = true;
        Serial.println("\n[EVENTO RELE] >>> VENTILADOR ENCENDIDO <<<");
      }

      // --- IMPRESIÓN EN SERIAL MONITOR ---
      Serial.print("[MONITOREO] Temp Actual: ");
      Serial.print(tempPanel, 2);
      Serial.print(" °C | Pendiente: ");
      Serial.print(pendiente, 4);
      Serial.print(" °C/s | Temp Predicha (1m): ");
      Serial.print(tempPredicha1Min, 2);
      Serial.print(" °C | Umbral Activo: ");
      Serial.print(umbralActivacionAdaptativo, 1);
      Serial.print(" °C | Rele: ");
      Serial.println(ventiladorEncendido ? "ON" : "OFF");

      // --- ACTUALIZACIÓN PANTALLA LCD ---
      lcd.setCursor(0, 0);
      lcd.print("T:");
      lcd.print(tempPanel, 1);
      lcd.write((uint8_t) 0);
      lcd.print("C   ");
      
      lcd.setCursor(10, 0);
      lcd.print(ventiladorEncendido ? "[FAN:1]" : "[FAN:0]");

      lcd.setCursor(0, 1);
      lcd.print("IA:");
      lcd.print(tempPredicha1Min, 1);
      lcd.write((uint8_t) 0);
      lcd.print("C (1m) ");

    } else {
      Serial.println("[ERROR] No se pudo leer el sensor DS18B20. Verifique la conexion.");
      lcd.setCursor(0, 0);
      lcd.print("Error Sensor!   ");
      lcd.setCursor(0, 1);
      lcd.print("Verifique DS18B20");
    }
  }

  // ======================================================
  // TAREA 2: TELEMETRÍA EN LA NUBE - THINGSPEAK (Cada 15 s)
  // ======================================================
  if (tiempoActual - ultimoEnvioThingSpeak >= INTERVALO_THINGSPEAK) {
    ultimoEnvioThingSpeak = tiempoActual;
    
    if (WiFi.status() == WL_CONNECTED) {
      // Usa las últimas variables calculadas sin detener la ejecución
      if (tempPanelGlobal != DEVICE_DISCONNECTED_C && tempPanelGlobal > -55.0) {
        Serial.println("\n--- ENVIANDO DATOS A THINGSPEAK ---");
        HTTPClient http;
        http.begin(server);
        http.addHeader("Content-Type", "application/x-www-form-urlencoded");

        String httpRequestData = "api_key=" + apiKey + 
                                 "&field1=" + String(tempPanelGlobal, 1) + 
                                 "&field2=" + String(tempPredichaGlobal, 1) +
                                 "&field3=" + String(ventiladorEncendido ? 1 : 0);

        int httpResponseCode = http.POST(httpRequestData);
        Serial.print("Codigo de respuesta HTTP ThingSpeak: ");
        Serial.println(httpResponseCode);
        
        if (httpResponseCode > 0) {
          Serial.println("Telemetria enviada con exito.");
        } else {
          Serial.print("Error al enviar datos HTTP: ");
          Serial.println(http.errorToString(httpResponseCode).c_str());
        }
        
        http.end();
        Serial.println("-----------------------------------\n");
      }
    } else {
      Serial.println("[WIFI] Intentando reconectar a la red...");
      WiFi.reconnect();
    }
  }
}
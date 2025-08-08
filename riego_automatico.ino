#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <EEPROM.h>

// ==================== CONFIGURACIÓN ====================
// Pines
#define RELAY_PIN       32
#define SENSOR_PIN      36
#define LED_WIFI        26
#define BUTTON_RESET    14
#define SDA_PIN         21
#define SCL_PIN         22
#define FLOAT_SENSOR_PIN 27

// Constantes del sistema
const int VALOR_HUMEDAD_ALTA = 1950;  // Suelo muy húmedo (mínimo ADC)
const int VALOR_HUMEDAD_BAJA = 4095;  // Suelo muy seco (máximo ADC)
const int HUMEDAD_MINIMA = 40;        // Umbral para activar riego
const int EEPROM_SIZE = 512;
const int EEPROM_INTERVALO_ADDR = 0;

// Intervalos de tiempo (ms)
const unsigned long INTERVALO_TELEGRAM = 3000;
const unsigned long INTERVALO_LCD = 2000;
const unsigned long INTERVALO_INTERNET = 30000;
const unsigned long INTERVALO_RECONEXION = 3000;
const unsigned long INTERVALO_LED_RAPIDO = 200;
const unsigned long INTERVALO_LED_LENTO = 1000;
const unsigned long INTERVALO_DEBOUNCE = 1000;
const unsigned long INTERVALO_LOOP = 50;

// Telegram
const char* BOT_TOKEN = "7741720891:AAE8vE2viQaHLuyBQRxkQJQCQ___Gh3uxK4";
const String CHAT_ID_USUARIO = "8093122816";
const String CHAT_ID_GRUPO = "-1002618602733";

// ==================== OBJETOS GLOBALES ====================
WiFiManager wifiManager;
WebServer server(80);
LiquidCrystal_I2C lcd(0x27, 16, 2);
WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);
WiFiClient testClient;

// ==================== VARIABLES DE ESTADO ====================
struct SystemState {
  unsigned long lastTelegramCheck = 0;
  unsigned long lastTelegramReport = 0;
  unsigned long lastLCDUpdate = 0;
  unsigned long lastInternetCheck = 0;
  unsigned long lastLedToggle = 0;
  unsigned long lastReconnectAttempt = 0;
  
  bool ledState = false;
  bool internetOK = false;
  bool aguaDisponible = true;
  bool mensajeEnviado = false;
  bool botActivo = true;
  bool enModoAP = false;
  bool intentandoConectar = false;
  
  int humedadActual = 0;
  unsigned long intervaloReporte = 300000; // 5 minutos por defecto
};

SystemState state;

// ==================== ESTRUCTURA DE INTERVALOS ====================
struct IntervaloReporte {
  unsigned long tiempo;
  const char* nombre;
  const char* emoji;
  const char* descripcion;
};

const IntervaloReporte INTERVALOS_DISPONIBLES[] PROGMEM = {
  {30000, "30 segundos", "⚡", "Pruebas rápidas"},
  {60000, "1 minuto", "⏱", "Monitoreo intensivo"},
  {120000, "2 minutos", "⏱", "Seguimiento rápido"},
  {300000, "5 minutos", "⏱", "Control frecuente"},
  {600000, "10 minutos", "⏱", "Supervisión regular"},
  {900000, "15 minutos", "⏰", "Chequeo moderado"},
  {1800000, "30 minutos", "⏰", "Control espaciado"},
  {3600000, "1 hora", "🕐", "Supervisión normal"},
  {7200000, "2 horas", "🕐", "Chequeo extendido"},
  {10800000, "3 horas", "🕐", "Control amplio"},
  {21600000, "6 horas", "🕰", "Reportes espaciados"},
  {43200000, "12 horas", "🕰", "Dos veces al día"},
  {86400000, "24 horas", "📅", "Reporte diario"},
  {0, "Desactivado", "❌", "Sin reportes automáticos"}
};

const int NUM_INTERVALOS = sizeof(INTERVALOS_DISPONIBLES) / sizeof(INTERVALOS_DISPONIBLES[0]);

// ==================== MAPAS DE COMANDOS ====================
struct ComandoMap {
  const char* texto;
  const char* comando;
};

// Comandos principales optimizados con PROGMEM
const ComandoMap COMANDOS_PRINCIPALES[] PROGMEM = {
  {"📊 estado", "/estado"},
  {"💧 encender", "/encender"},
  {"🛑 apagar", "/apagar"},
  {"⏰ intervalo", "/intervalo"},
  {"🤖 activar bot", "/activar_bot"},
  {"🔕 desactivar bot", "/desactivar_bot"},
  {"🔙 volver al menú", "/volver"}
};

const ComandoMap COMANDOS_INTERVALO[] PROGMEM = {
  {"⚡ 30seg", "/int_0"},
  {"⏱ 1min", "/int_1"},
  {"⏱ 2min", "/int_2"},
  {"⏱ 5min", "/int_3"},
  {"⏱ 10min", "/int_4"},
  {"⏰ 15min", "/int_5"},
  {"⏰ 30min", "/int_6"},
  {"🕐 1h", "/int_7"},
  {"🕐 2h", "/int_8"},
  {"🕐 3h", "/int_9"},
  {"🕰 6h", "/int_10"},
  {"🕰 12h", "/int_11"},
  {"📅 24h", "/int_12"},
  {"❌ off", "/int_13"}
};

const int NUM_COMANDOS_PRINCIPALES = sizeof(COMANDOS_PRINCIPALES) / sizeof(COMANDOS_PRINCIPALES[0]);
const int NUM_COMANDOS_INTERVALO = sizeof(COMANDOS_INTERVALO) / sizeof(COMANDOS_INTERVALO[0]);

// ==================== FUNCIONES DE UTILIDAD ====================
bool tiempoTranscurrido(unsigned long &ultimaVez, unsigned long intervalo) {
  unsigned long ahora = millis();
  if (ahora - ultimaVez >= intervalo) {
    ultimaVez = ahora;
    return true;
  }
  return false;
}

String mapearComando(const String &texto, const ComandoMap comandos[], int numComandos) {
  String textoLower = texto;
  textoLower.toLowerCase();
  
  for (int i = 0; i < numComandos; i++) {
    String comandoTexto = String(comandos[i].texto);
    comandoTexto.toLowerCase();
    if (textoLower == comandoTexto) {
      return String(comandos[i].comando);
    }
  }
  return texto; // Retornar original si no se encuentra mapeo
}

String formatearTiempo(unsigned long milisegundos) {
  if (milisegundos == 0) return "0s";
  
  unsigned long segundos = milisegundos / 1000;
  unsigned long minutos = segundos / 60;
  unsigned long horas = minutos / 60;
  
  if (horas > 0) {
    return String(horas) + "h " + String(minutos % 60) + "m";
  } else if (minutos > 0) {
    return String(minutos) + "m " + String(segundos % 60) + "s";
  }
  return String(segundos) + "s";
}

String obtenerNombreIntervalo(unsigned long intervalo) {
  for (int i = 0; i < NUM_INTERVALOS; i++) {
    if (INTERVALOS_DISPONIBLES[i].tiempo == intervalo) {
      return String(INTERVALOS_DISPONIBLES[i].nombre);
    }
  }
  return "Personalizado";
}

// ==================== FUNCIONES DE HARDWARE ====================
int leerHumedad() {
  int lectura = analogRead(SENSOR_PIN);
  int humedad = map(lectura, VALOR_HUMEDAD_ALTA, VALOR_HUMEDAD_BAJA, 100, 0);
  return constrain(humedad, 0, 100);
}

void controlarRele() {
  if (state.aguaDisponible && state.humedadActual < HUMEDAD_MINIMA) {
    digitalWrite(RELAY_PIN, LOW);  // Activar riego
  } else {
    digitalWrite(RELAY_PIN, HIGH); // Desactivar riego
  }
}

void actualizarLCD() {
  if (state.enModoAP) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Modo AP activo");
    lcd.setCursor(0, 1);
    lcd.print("Configura WiFi");
  } else {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Humedad: ");
    lcd.print(state.humedadActual);
    lcd.print("%     ");
    lcd.setCursor(0, 1);
    lcd.print(state.humedadActual < HUMEDAD_MINIMA ? "! Tierra seca  " : "* Tierra humeda ");
  }
}

void controlarLedWiFi() {
  if (state.enModoAP) {
    digitalWrite(LED_WIFI, LOW);
  } else if (WiFi.status() != WL_CONNECTED) {
    if (tiempoTranscurrido(state.lastLedToggle, INTERVALO_LED_RAPIDO)) {
      state.ledState = !state.ledState;
      digitalWrite(LED_WIFI, state.ledState);
    }
  } else {
    if (state.internetOK) {
      digitalWrite(LED_WIFI, HIGH);
    } else {
      if (tiempoTranscurrido(state.lastLedToggle, INTERVALO_LED_LENTO)) {
        state.ledState = !state.ledState;
        digitalWrite(LED_WIFI, state.ledState);
      }
    }
  }
}

bool hayInternet() {
  bool conectado = testClient.connect("google.com", 80);
  testClient.stop();
  return conectado;
}

// ==================== FUNCIONES EEPROM ====================
void guardarIntervaloEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(EEPROM_INTERVALO_ADDR, state.intervaloReporte);
  EEPROM.commit();
  Serial.println("Intervalo guardado: " + String(state.intervaloReporte));
}

void cargarIntervaloEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  unsigned long intervaloGuardado;
  EEPROM.get(EEPROM_INTERVALO_ADDR, intervaloGuardado);
  
  if (intervaloGuardado == 0xFFFFFFFF || intervaloGuardado > 86400000) {
    state.intervaloReporte = 300000; // 5 minutos por defecto
    guardarIntervaloEEPROM();
  } else {
    state.intervaloReporte = intervaloGuardado;
  }
  
  Serial.println("Intervalo cargado: " + String(state.intervaloReporte));
}

void borrarCredenciales() {
  Serial.println("Borrando credenciales WiFi...");
  wifiManager.resetSettings();
  EEPROM.begin(EEPROM_SIZE);
  for (int i = 0; i < EEPROM_SIZE; i++) EEPROM.write(i, 0);
  EEPROM.commit();
  digitalWrite(LED_WIFI, LOW);
  delay(100);
  ESP.restart();
}

// ==================== FUNCIONES TELEGRAM ====================
bool chatAutorizado(const String &chat_id) {
  return (chat_id == CHAT_ID_USUARIO) || (chat_id == CHAT_ID_GRUPO);
}

void enviarMensajeAmbosChats(const String &mensaje, const String &modo = "") {
  if (WiFi.status() == WL_CONNECTED && state.botActivo) {
    bot.sendMessage(CHAT_ID_USUARIO, mensaje, modo);
    bot.sendMessage(CHAT_ID_GRUPO, mensaje, modo);
  }
}

void mostrarBotonesTelegram(const String &chat_id) {
  const String keyboardJson =
    "[[{\"text\":\"📊 Estado\"}, {\"text\":\"💧 Encender\"}],"
    "[{\"text\":\"🛑 Apagar\"}, {\"text\":\"⏰ Intervalo\"}],"
    "[{\"text\":\"🤖 Activar Bot\"}, {\"text\":\"🔕 Desactivar Bot\"}]]";

  const String mensaje = "📋 Menú de Control del Sistema de Riego\n\n🌱 Usa los botones para controlar el sistema:";
  bot.sendMessageWithReplyKeyboard(chat_id, mensaje, "", keyboardJson, true);
}

void mostrarBotonesIntervalo(const String &chat_id) {
  const String keyboardJson = 
    "[" 
    "[{\"text\":\"⚡ 30seg\"}, {\"text\":\"⏱ 1min\"}, {\"text\":\"⏱ 2min\"}],"
    "[{\"text\":\"⏱ 5min\"}, {\"text\":\"⏱ 10min\"}, {\"text\":\"⏰ 15min\"}],"
    "[{\"text\":\"⏰ 30min\"}, {\"text\":\"🕐 1h\"}, {\"text\":\"🕐 2h\"}],"
    "[{\"text\":\"🕐 3h\"}, {\"text\":\"🕰 6h\"}, {\"text\":\"🕰 12h\"}],"
    "[{\"text\":\"📅 24h\"}, {\"text\":\"❌ OFF\"}],"
    "[{\"text\":\"🔙 Volver al menú\"}]"
    "]";

  String mensaje = "⏰ Configurar Intervalo de Reportes\n\n";
  mensaje += "📊 Actual: " + obtenerNombreIntervalo(state.intervaloReporte) + "\n";
  mensaje += "🔸 Estado: " + String(state.botActivo ? "✅ Activo" : "🔕 Inactivo");
  
  if (state.intervaloReporte > 0) {
    unsigned long tiempoTranscurrido = millis() - state.lastTelegramReport;
    unsigned long tiempoRestante = (tiempoTranscurrido < state.intervaloReporte) ? 
                                  (state.intervaloReporte - tiempoTranscurrido) : 0;
    mensaje += "\n🕐 Próximo: " + formatearTiempo(tiempoRestante);
  }

  bot.sendMessageWithReplyKeyboard(chat_id, mensaje, "Markdown", keyboardJson, true);
}

void procesarComandoTelegram(const String &chat_id, String comando) {
  if (!chatAutorizado(chat_id)) return;

  // Mapear comandos de botones
  comando = mapearComando(comando, COMANDOS_PRINCIPALES, NUM_COMANDOS_PRINCIPALES);
  comando = mapearComando(comando, COMANDOS_INTERVALO, NUM_COMANDOS_INTERVALO);
  
  comando.toLowerCase();
  Serial.println("Comando: '" + comando + "'");

  // Procesar comandos
  if (comando == "/activar_bot") {
    state.botActivo = true;
    bot.sendMessage(chat_id, "🤖 Bot activado.", "");
    
  } else if (comando == "/desactivar_bot") {
    state.botActivo = false;
    bot.sendMessage(chat_id, "🔕 Bot desactivado.", "");
    
  } else if (!state.botActivo && comando != "/activar_bot") {
    bot.sendMessage(chat_id, "⚠ Bot inactivo. Usa /activar_bot para activarlo.", "");
    
  } else if (comando == "/estado") {
    String estado = (state.humedadActual < HUMEDAD_MINIMA) ? "⚠ Tierra seca" : "💧 Tierra húmeda";
    String mensaje = "🌱 Estado del Sistema\n\n";
    mensaje += "💧 Humedad: " + String(state.humedadActual) + "%\n";
    mensaje += "🌿 Estado: " + estado + "\n";
    mensaje += "⏰ Reportes: " + obtenerNombreIntervalo(state.intervaloReporte) + "\n";
    mensaje += "🚰 Agua: " + String(state.aguaDisponible ? "✅ Disponible" : "❌ Sin agua");
    
    if (state.intervaloReporte > 0 && state.botActivo) {
      unsigned long tiempoTranscurrido = millis() - state.lastTelegramReport;
      unsigned long tiempoRestante = (tiempoTranscurrido < state.intervaloReporte) ? 
                                    (state.intervaloReporte - tiempoTranscurrido) : 0;
      mensaje += "\n🕐 Próximo: " + formatearTiempo(tiempoRestante);
    }
    
    bot.sendMessage(chat_id, mensaje, "Markdown");
    
  } else if (comando == "/encender") {
    if (state.humedadActual >= HUMEDAD_MINIMA) {
      bot.sendMessage(chat_id, "💧 Humedad " + String(state.humedadActual) + "%. No necesario regar.", "");
    } else if (!state.aguaDisponible) {
      bot.sendMessage(chat_id, "🚱 Sin agua disponible.", "");
    } else {
      digitalWrite(RELAY_PIN, LOW);
      bot.sendMessage(chat_id, "✅ Riego encendido manualmente.", "");
    }
    
  } else if (comando == "/apagar") {
    digitalWrite(RELAY_PIN, HIGH);
    bot.sendMessage(chat_id, "🛑 Riego apagado manualmente.", "");
    
  } else if (comando == "/intervalo") {
    mostrarBotonesIntervalo(chat_id);
    
  } else if (comando == "/volver") {
    mostrarBotonesTelegram(chat_id);
    
  } else if (comando.startsWith("/int_")) {
    int indice = comando.substring(5).toInt();
    if (indice >= 0 && indice < NUM_INTERVALOS) {
      unsigned long intervaloAnterior = state.intervaloReporte;
      state.intervaloReporte = INTERVALOS_DISPONIBLES[indice].tiempo;
      guardarIntervaloEEPROM();
      
      String mensaje = "✅ Intervalo Actualizado\n\n";
      mensaje += "• Anterior: " + obtenerNombreIntervalo(intervaloAnterior) + "\n";
      mensaje += "• Nuevo: " + String(INTERVALOS_DISPONIBLES[indice].nombre) + "\n";
      mensaje += "• " + String(INTERVALOS_DISPONIBLES[indice].descripcion);
      
      if (state.intervaloReporte == 0) {
        mensaje += "\n\n🔕 Reportes DESACTIVADOS";
      } else {
        state.lastTelegramReport = millis();
        mensaje += "\n\n⏰ Próximo reporte en: " + String(INTERVALOS_DISPONIBLES[indice].nombre);
      }
      
      bot.sendMessage(chat_id, mensaje, "Markdown");
      delay(1000);
      mostrarBotonesTelegram(chat_id);
    } else {
      bot.sendMessage(chat_id, "❌ Error: Índice no válido.", "");
    }
    
  } else if (comando == "/comandos") {
    mostrarBotonesTelegram(chat_id);
    
  } else {
    bot.sendMessage(chat_id, "❌ Comando no válido. Usa /comandos para ver opciones.", "");
  }
}

void manejarTelegram() {
  if (WiFi.status() != WL_CONNECTED) return;

  int numMessages = bot.getUpdates(bot.last_message_received + 1);
  for (int i = 0; i < numMessages; i++) {
    procesarComandoTelegram(bot.messages[i].chat_id, bot.messages[i].text);
  }
}

// ==================== FUNCIONES DE MONITOREO ====================
void monitorearSensorAgua() {
  bool aguaAnterior = state.aguaDisponible;
  state.aguaDisponible = (digitalRead(FLOAT_SENSOR_PIN) == LOW);
  
  if (!state.aguaDisponible && aguaAnterior) {
    // Agua se agotó
    digitalWrite(RELAY_PIN, HIGH);
    if (!state.mensajeEnviado) {
      enviarMensajeAmbosChats("🚱 Advertencia: Depósito vacío. Riego pausado.", "Markdown");
      state.mensajeEnviado = true;
    }
  } else if (state.aguaDisponible && !aguaAnterior) {
    // Agua restaurada
    enviarMensajeAmbosChats("💧 Recuperado: Nivel restaurado. Riego reactivado.", "Markdown");
    state.mensajeEnviado = false;
  }
}

void enviarReporteAutomatico() {
  if (WiFi.status() != WL_CONNECTED || state.intervaloReporte == 0 || !state.botActivo) return;
  
  if (tiempoTranscurrido(state.lastTelegramReport, state.intervaloReporte)) {
    String estado = (state.humedadActual < HUMEDAD_MINIMA) ? "⚠ Tierra seca" : "💧 Tierra húmeda";
    String mensaje = "🌱 Reporte Automático\n\n";
    mensaje += "💧 Humedad: " + String(state.humedadActual) + "%\n";
    mensaje += "🌿 Estado: " + estado + "\n";
    mensaje += "🚰 Agua: " + String(state.aguaDisponible ? "✅ Disponible" : "❌ Sin agua");
    mensaje += "\n⏰ Próximo: " + obtenerNombreIntervalo(state.intervaloReporte);
    
    enviarMensajeAmbosChats(mensaje, "Markdown");
    Serial.println("Reporte automático enviado");
  }
}

// ==================== EVENTOS WIFI ====================
void onWiFiEvent(WiFiEvent_t event) {
  switch(event) {
    case WIFI_EVENT_STA_CONNECTED:
      Serial.println("WiFi conectado.");
      state.intentandoConectar = false;
      state.enModoAP = false;
      digitalWrite(LED_WIFI, HIGH);
      break;
    case WIFI_EVENT_STA_DISCONNECTED:
      Serial.println("WiFi desconectado.");
      state.intentandoConectar = true;
      digitalWrite(LED_WIFI, LOW);
      break;
    case WIFI_EVENT_AP_START:
      Serial.println("Modo AP iniciado.");
      state.enModoAP = true;
      digitalWrite(LED_WIFI, LOW);
      break;
    case WIFI_EVENT_AP_STOP:
      Serial.println("Modo AP detenido.");
      state.enModoAP = false;
      break;
  }
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);

  // Configurar pines
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  pinMode(LED_WIFI, OUTPUT);
  pinMode(BUTTON_RESET, INPUT_PULLUP);
  pinMode(FLOAT_SENSOR_PIN, INPUT_PULLUP);

  // Inicializar componentes
  Wire.begin(SDA_PIN, SCL_PIN);
  lcd.init();
  lcd.backlight();
  secured_client.setInsecure();

  // Cargar configuración
  cargarIntervaloEEPROM();

  // Configurar WiFi
  WiFi.onEvent(onWiFiEvent);
  wifiManager.setTimeout(120);

  if (wifiManager.getWiFiIsSaved()) {
    Serial.println("Conectando a WiFi guardada...");
    WiFi.begin();
    state.intentandoConectar = true;
  } else {
    Serial.println("Iniciando AP de configuración...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("ESP32_Riego_AP", "Spiderman");
    state.enModoAP = true;
    
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Modo AP activo");
    lcd.setCursor(0, 1);
    lcd.print("Configura WiFi");

    wifiManager.startConfigPortal("ESP32_Riego_AP", "Spiderman");
    Serial.println("Conectado: " + WiFi.SSID());
    state.enModoAP = false;
  }

  server.begin();
  Serial.println("Sistema iniciado - Intervalo: " + obtenerNombreIntervalo(state.intervaloReporte));
}

// ==================== LOOP PRINCIPAL ====================
void loop() {
  // Procesar servicios básicos
  wifiManager.process();
  server.handleClient();

  // Botón de reset
  if (digitalRead(BUTTON_RESET) == LOW) {
    static unsigned long lastButtonPress = 0;
    if (tiempoTranscurrido(lastButtonPress, INTERVALO_DEBOUNCE)) {
      Serial.println("Borrando credenciales WiFi...");
      borrarCredenciales();
    }
  }

  // Reconexión WiFi
  if (WiFi.status() != WL_CONNECTED && 
      tiempoTranscurrido(state.lastReconnectAttempt, INTERVALO_RECONEXION)) {
    Serial.println("Reintentando conexión WiFi...");
    WiFi.begin();
    state.intentandoConectar = true;
  }

  // Chequeo de Internet
  if (WiFi.status() == WL_CONNECTED && 
      tiempoTranscurrido(state.lastInternetCheck, INTERVALO_INTERNET)) {
    state.internetOK = hayInternet();
    Serial.println(state.internetOK ? "Internet OK" : "Sin Internet");
  }

  // Control de LED WiFi
  controlarLedWiFi();

  // Monitoreo de sensores y control
  monitorearSensorAgua();
  state.humedadActual = leerHumedad();
  controlarRele();

  // Actualizar LCD
  if (tiempoTranscurrido(state.lastLCDUpdate, INTERVALO_LCD)) {
    actualizarLCD();
  }

  // Telegram
  if (WiFi.status() == WL_CONNECTED && 
      tiempoTranscurrido(state.lastTelegramCheck, INTERVALO_TELEGRAM)) {
    manejarTelegram();
  }

  // Reporte automático
  enviarReporteAutomatico();

  delay(INTERVALO_LOOP);
}
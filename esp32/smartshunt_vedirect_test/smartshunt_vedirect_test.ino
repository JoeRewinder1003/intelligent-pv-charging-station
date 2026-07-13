/*
  smartshunt_vedirect_test.ino

  Prueba aislada de lectura del Victron SmartShunt por VE.Direct.
  No usa Wi-Fi, MQTT, AWS, OTA, relés ni actuadores.

  Conexión de señales para esta prueba:
    VE.Direct TX  -> entrada RX de la ESP32 (GPIO16), usando la
                     interfaz/adaptación eléctrica ya empleada.
    VE.Direct GND -> GND de la ESP32.
    VE.Direct RX  -> no conectado.
    VE.Direct Power -> no conectado.

  Configuración VE.Direct:
    19200 baud, 8 data bits, no parity, 1 stop bit.

  Campos procesados:
    V   -> voltage_v    [mV -> V]
    I   -> current_a    [mA -> A]
    P   -> power_w      [W]
    SOC -> soc_percent  [0.1 % -> %]
    CE  -> consumed_ah  [mAh -> Ah]

  Nota:
    Esta prueba usa la línea Checksum como fin de bloque, pero todavía
    no valida matemáticamente el checksum del bloque.
*/

#include <Arduino.h>

namespace {

constexpr uint32_t USB_SERIAL_BAUD = 115200;
constexpr uint32_t VEDIRECT_BAUD = 19200;

constexpr int SMARTSHUNT_RX_PIN = 16;
constexpr int SMARTSHUNT_TX_PIN = -1;  // Prueba solo de recepción.

constexpr size_t LINE_BUFFER_SIZE = 128;
constexpr uint32_t NO_DATA_WARNING_MS = 5000;

HardwareSerial smartShuntSerial(1);

struct SmartShuntData {
  float voltageV = 0.0f;
  float currentA = 0.0f;
  float powerW = 0.0f;
  float socPercent = 0.0f;
  float consumedAh = 0.0f;

  bool hasVoltage = false;
  bool hasCurrent = false;
  bool hasPower = false;
  bool hasSoc = false;
  bool hasConsumedAh = false;

  uint32_t validBlocks = 0;
  uint32_t invalidNumericFields = 0;
  uint32_t bufferOverflows = 0;
  uint32_t lastByteMs = 0;
};

SmartShuntData shunt;

char lineBuffer[LINE_BUFFER_SIZE];
size_t lineLength = 0;
uint32_t lastNoDataWarningMs = 0;

bool parseLongValue(const char* text, long& result) {
  if (text == nullptr || text[0] == '\0') {
    return false;
  }

  // El SmartShunt puede enviar "---" cuando un valor no está disponible
  // o todavía no es significativo.
  if (strcmp(text, "---") == 0) {
    return false;
  }

  char* endPointer = nullptr;
  result = strtol(text, &endPointer, 10);

  return endPointer != text && *endPointer == '\0';
}

void printValueOrUnavailable(
  const char* label,
  bool available,
  float value,
  uint8_t decimals,
  const char* unit
) {
  Serial.print(label);
  Serial.print(": ");

  if (!available) {
    Serial.println("no disponible");
    return;
  }

  Serial.print(value, decimals);

  if (unit != nullptr && unit[0] != '\0') {
    Serial.print(' ');
    Serial.print(unit);
  }

  Serial.println();
}

void printSnapshot() {
  Serial.println();
  Serial.println("===== SmartShunt VE.Direct =====");

  printValueOrUnavailable(
    "Voltaje",
    shunt.hasVoltage,
    shunt.voltageV,
    3,
    "V"
  );

  printValueOrUnavailable(
    "Corriente",
    shunt.hasCurrent,
    shunt.currentA,
    3,
    "A"
  );

  printValueOrUnavailable(
    "Potencia",
    shunt.hasPower,
    shunt.powerW,
    1,
    "W"
  );

  printValueOrUnavailable(
    "SOC",
    shunt.hasSoc,
    shunt.socPercent,
    1,
    "%"
  );

  printValueOrUnavailable(
    "Ah consumidos (CE)",
    shunt.hasConsumedAh,
    shunt.consumedAh,
    3,
    "Ah"
  );

  Serial.print("Bloques recibidos: ");
  Serial.println(shunt.validBlocks);

  Serial.print("Campos numéricos inválidos/no disponibles: ");
  Serial.println(shunt.invalidNumericFields);

  Serial.print("Desbordamientos de línea: ");
  Serial.println(shunt.bufferOverflows);

  Serial.println("================================");
}

void processLine(char* line) {
  if (line == nullptr || line[0] == '\0') {
    return;
  }

  char* separator = strchr(line, '\t');

  if (separator == nullptr) {
    return;
  }

  *separator = '\0';

  const char* key = line;
  const char* valueText = separator + 1;

  // Checksum marca el final del bloque de texto VE.Direct.
  // La validación matemática del checksum queda para una etapa posterior.
  if (strcmp(key, "Checksum") == 0) {
    shunt.validBlocks++;
    printSnapshot();
    return;
  }

  long rawValue = 0;

  if (
    strcmp(key, "V") != 0 &&
    strcmp(key, "I") != 0 &&
    strcmp(key, "P") != 0 &&
    strcmp(key, "SOC") != 0 &&
    strcmp(key, "CE") != 0
  ) {
    return;
  }

  if (!parseLongValue(valueText, rawValue)) {
    shunt.invalidNumericFields++;
    return;
  }

  if (strcmp(key, "V") == 0) {
    shunt.voltageV = static_cast<float>(rawValue) / 1000.0f;
    shunt.hasVoltage = true;
    return;
  }

  if (strcmp(key, "I") == 0) {
    shunt.currentA = static_cast<float>(rawValue) / 1000.0f;
    shunt.hasCurrent = true;
    return;
  }

  if (strcmp(key, "P") == 0) {
    shunt.powerW = static_cast<float>(rawValue);
    shunt.hasPower = true;
    return;
  }

  if (strcmp(key, "SOC") == 0) {
    shunt.socPercent = static_cast<float>(rawValue) / 10.0f;
    shunt.hasSoc = true;
    return;
  }

  if (strcmp(key, "CE") == 0) {
    shunt.consumedAh = static_cast<float>(rawValue) / 1000.0f;
    shunt.hasConsumedAh = true;
  }
}

void readSmartShunt() {
  while (smartShuntSerial.available() > 0) {
    const char receivedByte =
      static_cast<char>(smartShuntSerial.read());

    shunt.lastByteMs = millis();

    if (receivedByte == '\n') {
      lineBuffer[lineLength] = '\0';

      if (lineLength > 0) {
        processLine(lineBuffer);
      }

      lineLength = 0;
      continue;
    }

    if (receivedByte == '\r') {
      continue;
    }

    if (lineLength < LINE_BUFFER_SIZE - 1) {
      lineBuffer[lineLength++] = receivedByte;
    } else {
      lineLength = 0;
      shunt.bufferOverflows++;
    }
  }
}

void warnIfNoData() {
  const uint32_t now = millis();

  if (now - shunt.lastByteMs < NO_DATA_WARNING_MS) {
    return;
  }

  if (now - lastNoDataWarningMs < NO_DATA_WARNING_MS) {
    return;
  }

  lastNoDataWarningMs = now;

  Serial.println();
  Serial.println(
    "ADVERTENCIA: no se reciben datos VE.Direct."
  );
  Serial.println(
    "Revisa GND, la señal TX del SmartShunt, GPIO16 y 19200 baud."
  );
}

}  // namespace

void setup() {
  Serial.begin(USB_SERIAL_BAUD);
  delay(1000);

  Serial.println();
  Serial.println("Prueba aislada SmartShunt VE.Direct");
  Serial.println("ESP32 RX: GPIO16");
  Serial.println("VE.Direct: 19200 baud, 8N1");
  Serial.println("Esperando bloques de datos...");

  smartShuntSerial.begin(
    VEDIRECT_BAUD,
    SERIAL_8N1,
    SMARTSHUNT_RX_PIN,
    SMARTSHUNT_TX_PIN
  );

  shunt.lastByteMs = millis();
}

void loop() {
  readSmartShunt();
  warnIfNoData();
  delay(1);
}

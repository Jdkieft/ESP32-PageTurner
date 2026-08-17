/*
  XIAO ESP32C6 - Bluetooth (BLE) e-reader afstandsbediening
  ============================================================
  D7  knop   -> volgende pagina  (stuurt PIJL-RECHTS)
  D3  touch  -> vorige pagina    (stuurt PIJL-LINKS)
  D6  LED    -> knippert 1x bij opstarten, knippert continu bij lage batterij
  A0  ADC    -> batterijspanning via spanningsdeler
                (zie: forum.seeedstudio.com/t/battery-voltage-monitor-and-ad-conversion-for-xiao-esp32c/267535)

  BENODIGDE LIBRARY (NIET de originele T-vK library, die compileert niet op de C6):
    ESP32-NIMBLE-Keyboard (fork met NimBLE-Arduino 2.x support)
    https://github.com/webmonkey/ESP32-NIMBLE-Keyboard
    -> Download als ZIP -> Arduino IDE: Sketch > Include Library > Add .ZIP Library
    -> Zorg dat de "NimBLE-Arduino" library (by h2zero) ook geinstalleerd is via Library Manager
*/

#include <NimBleKeyboard.h>

// ---------------- Pin configuratie ----------------
const int PIN_NEXT = D7;  // knop  -> volgende pagina
const int PIN_PREV = D3;  // touch -> vorige pagina
const int PIN_LED  = D6;  // status LED
const int PIN_BATT = A0;  // batterijspanning

// ---------------- Instellingen knoppen ----------------
// Pas aan op basis van hoe jouw sensoren daadwerkelijk bedraad zijn:
const bool NEXT_ACTIVE_LOW = true;   // knop naar GND -> LOW bij indrukken
const bool PREV_ACTIVE_LOW = false;  // touchmodule -> HIGH bij aanraken
const unsigned long DEBOUNCE_MS = 40;

// ---------------- Instellingen batterij ----------------
// DIVIDER_RATIO aanpassen aan jouw weerstanden: Vbat = Vadc * DIVIDER_RATIO
// Bij de Seeed-forumpost is dit 2x 200k (1:1 verdeling) -> ratio 2.0
const float DIVIDER_RATIO   = 2.0;     // AANPASSEN aan jouw schakeling!
const int   BATT_SAMPLES    = 16;      // aantal metingen om te middelen (ruisonderdrukking)
// Standaard cutoff/max voor een 3.7V Li-ion/LiPo cel. Heeft jouw accu een
// protectie-PCB met een andere afsnijspanning, pas BATT_EMPTY_VOLT daarop aan.
const float BATT_EMPTY_VOLT = 3.20;    // komt overeen met 0% (typische onderspanningsgrens)
const float BATT_FULL_VOLT  = 4.20;    // komt overeen met 100% (max. laadspanning)
const int   LOW_BATTERY_PCT = 20;      // grens waarop LED gaat waarschuwen

// Zuinige "heartbeat"-puls i.p.v. continu knipperen bij lage batterij
const unsigned long LOW_BATT_PULSE_ON_MS     = 60;    // hoe kort de LED oplicht
const unsigned long LOW_BATT_PULSE_PERIOD_MS = 4000;  // hoe vaak (elke 4s een korte flits)

const unsigned long BATT_CHECK_INTERVAL = 30000; // batterij elke 30s checken

// ---------------- BLE ----------------
// Let op: deze library kapt namen af op 15 tekens, dus kort en duidelijk houden.
BleKeyboard bleKeyboard("Ereader Remote", "DIY", 100);

// ---------------- Statusvariabelen ----------------
bool lastNextState = false;
bool lastPrevState = false;
unsigned long lastNextChange = 0;
unsigned long lastPrevChange = 0;
unsigned long lastBattCheck = 0;
bool lowBattery = false;

void setup() {
  Serial.begin(115200);

  pinMode(PIN_NEXT, NEXT_ACTIVE_LOW ? INPUT_PULLUP : INPUT_PULLDOWN);
  pinMode(PIN_PREV, PREV_ACTIVE_LOW ? INPUT_PULLUP : INPUT_PULLDOWN);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  pinMode(PIN_BATT, INPUT);

  bleKeyboard.begin();

  // 1x knipperen bij opstarten, zoals gevraagd
  blinkLed(1, 150);

  Serial.println("Setup klaar, wachten op BLE-verbinding...");
}

void loop() {
  handleButton(PIN_NEXT, NEXT_ACTIVE_LOW, lastNextState, lastNextChange,
               KEY_RIGHT_ARROW, "volgende pagina");
  handleButton(PIN_PREV, PREV_ACTIVE_LOW, lastPrevState, lastPrevChange,
               KEY_LEFT_ARROW, "vorige pagina");

  if (millis() - lastBattCheck > BATT_CHECK_INTERVAL) {
    lastBattCheck = millis();
    checkBattery();
  }

  updateLowBatteryLed();
}

// ---------------- Knop/touch afhandeling met debounce ----------------
void handleButton(int pin, bool activeLow, bool &lastState, unsigned long &lastChange,
                   uint8_t key, const char* label) {
  bool raw = digitalRead(pin);
  bool pressed = activeLow ? (raw == LOW) : (raw == HIGH);

  if (pressed != lastState && (millis() - lastChange) > DEBOUNCE_MS) {
    lastChange = millis();
    lastState = pressed;

    if (pressed && bleKeyboard.isConnected()) {
      bleKeyboard.write(key);
      Serial.println(label);
    }
  }
}

// ---------------- Batterij ----------------
void checkBattery() {
  // analogReadMilliVolts() gebruikt de fabriekscalibratie (eFuse) van deze
  // specifieke chip, wat veel nauwkeuriger is dan analogRead() met een
  // vaste 3.3V-aanname (chip-tot-chip variatie kan tot ±10% zijn).
  uint32_t sumMv = 0;
  for (int i = 0; i < BATT_SAMPLES; i++) {
    sumMv += analogReadMilliVolts(PIN_BATT);
  }
  float vAdc = (sumMv / (float)BATT_SAMPLES) / 1000.0; // mV -> V
  float vBatt = vAdc * DIVIDER_RATIO;

  int pct = (int)((vBatt - BATT_EMPTY_VOLT) / (BATT_FULL_VOLT - BATT_EMPTY_VOLT) * 100.0);
  pct = constrain(pct, 0, 100);

  Serial.printf("Batterij: %.2f V (%d%%)\n", vBatt, pct);

  lowBattery = (pct <= LOW_BATTERY_PCT);

  if (bleKeyboard.isConnected()) {
    // Laat percentage zien in bv. bluetooth-instellingen van Windows/iOS
    bleKeyboard.setBatteryLevel(pct);
  }
}

// ---------------- LED ----------------
void blinkLed(int times, int onMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(PIN_LED, HIGH);
    delay(onMs);
    digitalWrite(PIN_LED, LOW);
    delay(onMs);
  }
}

void updateLowBatteryLed() {
  static unsigned long cycleStart = 0;

  if (!lowBattery) {
    digitalWrite(PIN_LED, LOW);
    return;
  }

  unsigned long elapsed = (millis() - cycleStart) % LOW_BATT_PULSE_PERIOD_MS;
  bool ledOn = elapsed < LOW_BATT_PULSE_ON_MS;
  digitalWrite(PIN_LED, ledOn ? HIGH : LOW);
}

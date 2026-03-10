/*
Thinkerbell Labs – Embedded Software Engineer Task
Final Complete Version (All Requirements Implemented)
Name: Yuvabharathi Varudharaj
Board: ESP32-S3
Framework: Arduino + FreeRTOS
*/

#include <Arduino.h>

#define BTN1 21
#define BTN2 48
#define BTN3 47

#define SR_DATA  4
#define SR_CLK   7
#define SR_LATCH 6
#define SR_OE    5
#define SR_MR    15

#define POLL_MS 5
#define DEBOUNCE_MS 20
#define SINGLE_MAX 300
#define DOUBLE_WINDOW 500
#define TRIPLE_WINDOW 700
#define LONG_PRESS_MS 1500
#define AUTO_PERIOD 2000

QueueHandle_t button_event_queue;
QueueHandle_t display_pattern_queue;
SemaphoreHandle_t shiftreg_mutex;

typedef enum { EVENT_SINGLE, EVENT_DOUBLE, EVENT_TRIPLE, EVENT_LONG } event_type_t;

typedef struct {
  uint8_t button;
  event_type_t type;
  uint32_t timestamp;
} button_event_t;

typedef enum {
  MENU_MAIN,
  MENU_BRIGHTNESS,
  MENU_MODE,
  MENU_MANUAL,
  MENU_AUTO,
  MENU_INFO,
  MENU_RESET,
  MENU_POWER_OFF
} menu_state_t;

menu_state_t currentMenu = MENU_MAIN;

uint8_t brightness = 5;
bool modeManual = true;
uint8_t patternIndex = 0;
uint8_t mainIndex = 0;
uint8_t infoScreen = 0;
uint32_t autoTimer = 0;

const uint16_t patterns[] = {0xAAAA, 0x5555, 0xF0F0, 0x0F0F};
const uint16_t infoPatterns[] = {0x1234, 0x5678};

uint16_t applyBrightness(uint16_t pattern) {
  if (brightness == 0) return 0x0000;
  if (brightness >= 10) return pattern;
  uint16_t scaled = 0;
  for (int i = 0; i < 16; i++) {
    if (pattern & (1 << i)) {
      if ((i % 10) < brightness)
        scaled |= (1 << i);
    }
  }
  return scaled;
}

void writeShift(uint16_t value) {
  xSemaphoreTake(shiftreg_mutex, portMAX_DELAY);
  digitalWrite(SR_LATCH, LOW);
  shiftOut(SR_DATA, SR_CLK, MSBFIRST, highByte(value));
  shiftOut(SR_DATA, SR_CLK, MSBFIRST, lowByte(value));
  digitalWrite(SR_LATCH, HIGH);
  xSemaphoreGive(shiftreg_mutex);
}

void ButtonInput_Task(void *pv) {
  const int pins[3] = {BTN1, BTN2, BTN3};
  bool lastStable[3] = {HIGH, HIGH, HIGH};
  bool lastReading[3] = {HIGH, HIGH, HIGH};
  uint32_t debounceTimer[3] = {0};
  uint32_t pressStart[3] = {0};
  uint8_t pressCount[3] = {0};
  uint32_t lastRelease[3] = {0};

  while (1) {
    uint32_t now = millis();
    for (int i = 0; i < 3; i++) {
      bool reading = digitalRead(pins[i]);

      if (reading != lastReading[i])
        debounceTimer[i] = now;

      if ((now - debounceTimer[i]) > DEBOUNCE_MS) {
        if (reading != lastStable[i]) {
          lastStable[i] = reading;

          if (reading == LOW)
            pressStart[i] = now;

          if (reading == HIGH) {
            uint32_t duration = now - pressStart[i];

            if (duration >= LONG_PRESS_MS) {
              button_event_t ev = {i, EVENT_LONG, now};
              xQueueSend(button_event_queue, &ev, 0);
              pressCount[i] = 0;
            } else {
              pressCount[i]++;
              lastRelease[i] = now;
            }
          }
        }
      }

      lastReading[i] = reading;

      if (pressCount[i] > 0) {
        uint32_t diff = now - lastRelease[i];
        if ((pressCount[i] == 1 && diff > SINGLE_MAX) ||
            (pressCount[i] == 2 && diff > DOUBLE_WINDOW) ||
            (pressCount[i] >= 3 && diff > TRIPLE_WINDOW)) {

          button_event_t ev;
          ev.button = i;
          ev.timestamp = now;
          if (pressCount[i] == 1) ev.type = EVENT_SINGLE;
          else if (pressCount[i] == 2) ev.type = EVENT_DOUBLE;
          else ev.type = EVENT_TRIPLE;

          xQueueSend(button_event_queue, &ev, 0);
          pressCount[i] = 0;
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(POLL_MS));
  }
}

void MenuLogic_Task(void *pv) {
  uint16_t ledPattern = 0;

  while (1) {
    button_event_t ev;

    if (xQueueReceive(button_event_queue, &ev, pdMS_TO_TICKS(50))) {

      if (currentMenu == MENU_POWER_OFF) {
        if (ev.button == 0 && ev.type == EVENT_LONG)
          currentMenu = MENU_MAIN;
        continue;
      }

      switch (currentMenu) {

        case MENU_MAIN:
          if (ev.button == 0 && ev.type == EVENT_SINGLE)
            mainIndex = (mainIndex + 1) % 4;
          if (ev.button == 2 && ev.type == EVENT_SINGLE)
            mainIndex = (mainIndex + 3) % 4;
          if (ev.button == 1 && ev.type == EVENT_SINGLE)
            currentMenu = (menu_state_t)(mainIndex + 1);
          if (ev.button == 2 && ev.type == EVENT_LONG)
            currentMenu = MENU_POWER_OFF;
          ledPattern = (1 << mainIndex);
          break;

        case MENU_BRIGHTNESS:
          if (ev.button == 1 && ev.type == EVENT_SINGLE && brightness < 10)
            brightness++;
          if (ev.button == 2 && ev.type == EVENT_SINGLE && brightness > 0)
            brightness--;
          if (ev.button == 0 && ev.type == EVENT_SINGLE)
            currentMenu = MENU_MAIN;
          ledPattern = (1 << brightness);
          break;

        case MENU_MODE:
          if (ev.button == 0 && ev.type == EVENT_SINGLE)
            modeManual = !modeManual;
          if (ev.button == 1 && ev.type == EVENT_SINGLE)
            currentMenu = modeManual ? MENU_MANUAL : MENU_AUTO;
          if (ev.button == 2 && ev.type == EVENT_SINGLE)
            currentMenu = MENU_MAIN;
          ledPattern = modeManual ? 0x000F : 0x00F0;
          break;

        case MENU_MANUAL:
          if (ev.button == 0 && ev.type == EVENT_SINGLE)
            patternIndex = (patternIndex + 1) % 4;
          if (ev.button == 1 && ev.type == EVENT_SINGLE)
            currentMenu = MENU_MAIN;
          if (ev.button == 2 && ev.type == EVENT_SINGLE)
            currentMenu = MENU_MAIN;
          ledPattern = patterns[patternIndex];
          break;

        case MENU_AUTO:
          if (millis() - autoTimer > AUTO_PERIOD) {
            patternIndex = (patternIndex + 1) % 4;
            autoTimer = millis();
          }
          if (ev.button == 0 && ev.type == EVENT_SINGLE)
            currentMenu = MENU_MAIN;
          ledPattern = patterns[patternIndex];
          break;

        case MENU_INFO:
          if (ev.button == 0 && ev.type == EVENT_SINGLE)
            infoScreen = (infoScreen + 1) % 2;
          if (ev.button == 2 && ev.type == EVENT_SINGLE)
            currentMenu = MENU_MAIN;
          ledPattern = infoPatterns[infoScreen];
          break;

        case MENU_RESET:
          if (ev.button == 1 && ev.type == EVENT_DOUBLE) {
            brightness = 5;
            modeManual = true;
            patternIndex = 0;
            currentMenu = MENU_MAIN;
          }
          if (ev.button == 2 && ev.type == EVENT_SINGLE)
            currentMenu = MENU_MAIN;
          ledPattern = 0xFFFF;
          break;

        default:
          break;
      }

      xQueueSend(display_pattern_queue, &ledPattern, 0);
    }
  }
}

void DisplayManager_Task(void *pv) {
  uint16_t pattern;

  while (1) {
    if (xQueueReceive(display_pattern_queue, &pattern, portMAX_DELAY)) {
      if (currentMenu == MENU_POWER_OFF)
        writeShift(0x0000);
      else
        writeShift(applyBrightness(pattern));
    }
  }
}

void setup() {
  pinMode(BTN1, INPUT_PULLUP);
  pinMode(BTN2, INPUT_PULLUP);
  pinMode(BTN3, INPUT_PULLUP);

  pinMode(SR_DATA, OUTPUT);
  pinMode(SR_CLK, OUTPUT);
  pinMode(SR_LATCH, OUTPUT);
  pinMode(SR_OE, OUTPUT);
  pinMode(SR_MR, OUTPUT);

  digitalWrite(SR_OE, LOW);
  digitalWrite(SR_MR, LOW);
  delay(5);
  digitalWrite(SR_MR, HIGH);

  button_event_queue = xQueueCreate(10, sizeof(button_event_t));
  display_pattern_queue = xQueueCreate(5, sizeof(uint16_t));
  shiftreg_mutex = xSemaphoreCreateMutex();

  xTaskCreate(ButtonInput_Task, "ButtonInput", 4096, NULL, 2, NULL);
  xTaskCreate(MenuLogic_Task, "MenuLogic", 4096, NULL, 1, NULL);
  xTaskCreate(DisplayManager_Task, "DisplayMgr", 4096, NULL, 1, NULL);
}

void loop() {}

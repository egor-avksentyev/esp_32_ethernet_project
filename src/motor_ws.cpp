#include "motor_ws.h"
#include "config.h"
#include "mega_link.h"
#include <WebSocketsServer.h>

static WebSocketsServer motorWsServer(MOTOR_WS_PORT);

// '\0' — сейчас никто не держит кнопку. Иначе 'U'/'D' — что слать на Mega на каждом тике
static char motorHoldLetter = '\0';
static uint8_t motorHoldClientNum = 0;
static unsigned long lastMotorHoldSend = 0;

static void handleMotorWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_TEXT: {
      // payload не гарантированно завершён нулём библиотекой — строим String по length,
      // не полагаясь на это
      String msg;
      msg.reserve(length);
      for (size_t i = 0; i < length; i++) {
        msg += (char)payload[i];
      }
      if (msg == "up") {
        motorHoldLetter = 'U';
        motorHoldClientNum = num;
        // millis(), не 0 — startHold() в PAGE_HTML уже шлёт одну команду немедленно через
        // cmd() ДО того, как это WS-соединение вообще успевает открыться; если тут тоже
        // форсировать немедленную отправку на следующем motorWsPoll(), короткое одиночное
        // нажатие (внутри Dimmer/Source/EQ/Info, где Up/Down — это ОДИН шаг списка, а не
        // непрерывное вращение мотора) успевало дать целых 2 шага вместо одного: один от
        // cmd(), второй — от этого форсированного немедленного повтора. millis() заставляет
        // подождать полный MOTOR_WS_REPEAT_MS перед первым РЕАЛЬНЫМ повтором — короткий тап
        // (быстрее MOTOR_WS_REPEAT_MS) успевает отпуститься (stopHold()) раньше, чем повтор
        // вообще случится, и даёт ровно один шаг
        lastMotorHoldSend = millis();
      } else if (msg == "down") {
        motorHoldLetter = 'D';
        motorHoldClientNum = num;
        lastMotorHoldSend = millis();
      } else if (msg == "stop" && motorHoldClientNum == num) {
        motorHoldLetter = '\0';
      }
      break;
    }
    case WStype_DISCONNECTED:
      // Соединение оборвалось не штатным "stop" (страницу закрыли, Wi-Fi моргнул) — прекращаем
      // повтор немедленно, не дожидаясь SLIDER_MOTOR_IDLE_TIMEOUT на стороне Mega (тот всё
      // равно отдельно подстрахует, если это сообщение почему-то не дошло)
      if (motorHoldClientNum == num) {
        motorHoldLetter = '\0';
      }
      break;
    default:
      break;
  }
}

void motorWsBegin() {
  motorWsServer.begin();
  motorWsServer.onEvent(handleMotorWsEvent);
}

void motorWsPoll() {
  motorWsServer.loop();
  if (motorHoldLetter == '\0') {
    return;
  }
  if (millis() - lastMotorHoldSend < MOTOR_WS_REPEAT_MS) {
    return;
  }
  lastMotorHoldSend = millis();
  megaLinkSendCommand(motorHoldLetter);
}

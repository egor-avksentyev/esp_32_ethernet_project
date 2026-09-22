#include "motor_ws.h"
#include "config.h"
#include "mega_link.h"
#include <WebSocketsServer.h>

static WebSocketsServer motorWsServer(MOTOR_WS_PORT);

// '\0' — сейчас никто не держит кнопку. Иначе 'U'/'D' — что слать на Mega на каждом тике
static char motorHoldLetter = '\0';
static uint8_t motorHoldClientNum = 0;
static unsigned long lastMotorHoldSend = 0;
// true — ещё ни разу не повторяли в ЭТОМ сеансе удержания (см. MOTOR_WS_FIRST_REPEAT_DELAY_MS)
static bool firstRepeatPending = false;

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
        lastMotorHoldSend = millis();
        firstRepeatPending = true; // см. MOTOR_WS_FIRST_REPEAT_DELAY_MS (config.h) за причиной
      } else if (msg == "down") {
        motorHoldLetter = 'D';
        motorHoldClientNum = num;
        lastMotorHoldSend = millis();
        firstRepeatPending = true;
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
  // Первый повтор в сеансе ждёт дольше (MOTOR_WS_FIRST_REPEAT_DELAY_MS) — отличает короткий тап
  // (уже успевший отпуститься, motorHoldLetter к этому моменту уже '\0') от настоящего
  // удержания; все следующие повторы — уже с обычным, коротким MOTOR_WS_REPEAT_MS для
  // плавного непрерывного вращения мотора
  unsigned long threshold = firstRepeatPending ? MOTOR_WS_FIRST_REPEAT_DELAY_MS : MOTOR_WS_REPEAT_MS;
  if (millis() - lastMotorHoldSend < threshold) {
    return;
  }
  lastMotorHoldSend = millis();
  firstRepeatPending = false;
  megaLinkSendCommand(motorHoldLetter);
}

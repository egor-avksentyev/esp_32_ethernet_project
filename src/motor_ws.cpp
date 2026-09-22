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
      // "up"/"down" — ЕДИНСТВЕННЫЙ источник команды на это нажатие (раньше startHold() в
      // PAGE_HTML ДОПОЛНИТЕЛЬНО слала одну команду сразу через cmd(), а этот обработчик потом
      // ещё и планировал первый повтор — при коротком нажатии внутри Dimmer/Source/EQ/Info
      // (где Up/Down это ОДИН шаг списка, а не непрерывное вращение мотора) обе стороны почти
      // всегда успевали сработать до отпускания, вместо одного шага получалось два. Ни
      // подстройка задержки первого повтора (MOTOR_WS_FIRST_REPEAT_DELAY_MS — было 80мс, потом
      // 300мс), ни что-либо ещё в этом духе не помогало стабильно, потому что сама схема с
      // ДВУМЯ независимыми источниками команды в принципе гоняется наперегонки. Теперь
      // единственный путь — отправляем СРАЗУ здесь (это и есть "один шаг" на одиночное
      // нажатие), а motorWsPoll() ниже только повторяет, если соединение всё ещё держится
      // дольше MOTOR_WS_REPEAT_MS — для непрерывного вращения мотора при настоящем удержании
      if (msg == "up" || msg == "down") {
        char letter = (msg == "up") ? 'U' : 'D';
        motorHoldLetter = letter;
        motorHoldClientNum = num;
        megaLinkSendCommand(letter);
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
  // Первая команда уже отправлена в handleMotorWsEvent() — здесь только повторы, пока
  // соединение всё ещё держится (настоящее удержание, не одиночный тап)
  if (millis() - lastMotorHoldSend < MOTOR_WS_REPEAT_MS) {
    return;
  }
  lastMotorHoldSend = millis();
  megaLinkSendCommand(motorHoldLetter);
}

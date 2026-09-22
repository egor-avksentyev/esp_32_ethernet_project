#include "frame_mirror.h"
#include "config.h"
#include "web_control.h"

// 1024 байта буфера + 1 байт ID иконки (см. FRAME_MIRROR_ICON_* в Mega-репозитории,
// frame_mirror.h) — оба идут дальше клиенту одним куском, чексумма — отдельно, не входит
// в то, что реально рассылается на веб-страницу
#define FRAME_MIRROR_PAYLOAD_LEN 1025

static HardwareSerial FrameMirrorSerial(1);

enum FrameMirrorState {
  WAIT_SYNC1,
  WAIT_SYNC2,
  READING_PAYLOAD,
  READING_CHECKSUM,
};

static FrameMirrorState state = WAIT_SYNC1;
static uint8_t frameBuf[FRAME_MIRROR_PAYLOAD_LEN];
static uint16_t bufPos = 0;

void frameMirrorBegin() {
  // 256 байт по умолчанию у HardwareSerial (arduino-esp32) — меньше одного кадра (1027 байт,
  // ~20мс на 500000 бод). Если loop() хоть немного задержится между вызовами frameMirrorPoll()
  // (конкурирует за время с WebServer/WebSocket-серверами), буфер переполняется и часть байт
  // теряется — кадр рассыпается на чексумме КАЖДЫЙ раз, кадры не проходят вообще никогда,
  // хотя провод и Mega исправны. Явно увеличиваем — с запасом на пару кадров, ДО begin()
  // (после begin() уже не действует)
  FrameMirrorSerial.setRxBufferSize(FRAME_MIRROR_PAYLOAD_LEN * 2);
  // TX не нужен (канал строго Mega->ESP32) — -1 оставляет его непривязанным
  FrameMirrorSerial.begin(FRAME_MIRROR_BAUD, SERIAL_8N1, FRAME_MIRROR_RX_PIN, -1);
}

// Простой синк-байт-ориентированный разбор — см. Mega-репозиторий, frame_mirror.h за полным
// форматом. Не line-based (в отличие от mega_link.cpp) — этот канал бинарный и ничем больше
// не пользуется, конфликтов с '\n' внутри пиксельных байт нет и быть не может
void frameMirrorPoll() {
  while (FrameMirrorSerial.available()) {
    uint8_t b = (uint8_t)FrameMirrorSerial.read();
    switch (state) {
      case WAIT_SYNC1:
        if (b == 0xAA) {
          state = WAIT_SYNC2;
        }
        break;
      case WAIT_SYNC2:
        if (b == 0x55) {
          state = READING_PAYLOAD;
          bufPos = 0;
        } else if (b != 0xAA) {
          state = WAIT_SYNC1;
        }
        // b == 0xAA: остаёмся в WAIT_SYNC2 — вдруг это начало настоящего 0xAA 0x55
        break;
      case READING_PAYLOAD:
        frameBuf[bufPos++] = b;
        if (bufPos >= FRAME_MIRROR_PAYLOAD_LEN) {
          state = READING_CHECKSUM;
        }
        break;
      case READING_CHECKSUM: {
        uint8_t checksum = 0;
        for (uint16_t i = 0; i < FRAME_MIRROR_PAYLOAD_LEN; i++) {
          checksum ^= frameBuf[i];
        }
        if (checksum == b) {
          // Совпало — кадр цел (1024 байта буфера + 1 байт ID иконки), рассылаем как есть.
          // Не совпало — просто отбрасываем целиком (не показываем визуально "битый" кадр),
          // следующий 0xAA 0x55 подхватит разбор заново
          webControlBroadcastFrame(frameBuf, FRAME_MIRROR_PAYLOAD_LEN);
        }
        state = WAIT_SYNC1;
        break;
      }
    }
  }
}

// SPDX-License-Identifier: MIT
// The OneiOS bridge channel on a byte stream: the messages of oneios_bridge_proto.h where the machine has no
// "oneios,bridge" virtio device but has a serial port, which is crosvm's VM of the native path (its second ns16550,
// the phone's end a Unix socket: docs/native.md "The bridge in the crosvm VM"). One message is one frame:
//   0x4f 0x42 ("OB"), struct ob_header (8 bytes), `length` payload bytes, CRC-16/CCITT-FALSE of header and payload (2 bytes)
// All values little-endian. A frame with a length above OB_MAX_PAYLOAD or a wrong checksum is dropped and the reader
// looks for the next 0x4f 0x42. Freestanding C, included unchanged by the kernel side (drivers/bridge/OneiOSBridge.c),
// by the phone's end (runtime/bridges/devices/bridge_stream.c) and by their host tests.
#ifndef ONEIOS_BRIDGE_STREAM_H
#define ONEIOS_BRIDGE_STREAM_H

#include <stddef.h>
#include <stdint.h>
#include "oneios_bridge_proto.h"

#define OBS_SOF0      0x4fu
#define OBS_SOF1      0x42u
#define OBS_OVERHEAD  4u                                 // the two start bytes and the checksum
#define OBS_MAX_FRAME (OB_MAX_MESSAGE + OBS_OVERHEAD)

static inline uint16_t obs_crc16(uint16_t crc, const uint8_t *data, size_t length)
{
  for (size_t i = 0; i < length; i++) {
    crc ^= (uint16_t)((uint16_t)data[i] << 8);
    for (int bit = 0; bit < 8; bit++) crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
  }
  return crc;
}

// One message as a frame in out (OBS_MAX_FRAME bytes at least). The frame's length, or 0 for a payload that is too long.
static inline size_t obs_encode(uint8_t *out, uint16_t channel, uint16_t type, const void *payload, uint32_t length)
{
  if (length > OB_MAX_PAYLOAD) return 0;
  out[0] = OBS_SOF0;
  out[1] = OBS_SOF1;
  out[2] = (uint8_t)channel; out[3] = (uint8_t)(channel >> 8);
  out[4] = (uint8_t)type;    out[5] = (uint8_t)(type >> 8);
  out[6] = (uint8_t)length;  out[7] = (uint8_t)(length >> 8); out[8] = (uint8_t)(length >> 16); out[9] = (uint8_t)(length >> 24);
  const uint8_t *from = (const uint8_t *)payload;
  for (uint32_t i = 0; i < length; i++) out[10 + i] = from[i];
  const uint16_t crc = obs_crc16(0xffffu, out + 2, 8u + length);
  out[10 + length] = (uint8_t)crc;
  out[11 + length] = (uint8_t)(crc >> 8);
  return 12u + length;
}

// The reader's state. message holds struct ob_header and the payload as oneios_bridge_proto.h has them (the host's
// byte order is the wire's: both ends are little-endian arm64, and the tests' x86-64).
struct obs_parser {
  uint32_t state;               // 0 wants 0x4f, 1 wants 0x42, 2 in the header, 3 in the payload, 4 and 5 the checksum
  uint32_t have;                // bytes of message so far
  uint32_t need;                // the header's size and the payload's
  uint16_t crc;                 // the frame's, as it arrives
  uint64_t frames, dropped;     // whole messages; frames given up (too long, wrong checksum)
  uint8_t message[OB_MAX_MESSAGE];
};

// Takes one byte. 1 when message now holds one whole, checked message of `have` bytes (valid until the next call).
static inline int obs_push(struct obs_parser *p, uint8_t byte)
{
  switch (p->state) {
  case 0:
    if (byte == OBS_SOF0) p->state = 1;
    return 0;
  case 1:
    if (byte == OBS_SOF1) { p->state = 2; p->have = 0; p->need = 8; }
    else p->state = byte == OBS_SOF0 ? 1 : 0;
    return 0;
  case 2:
    p->message[p->have++] = byte;
    if (p->have < 8) return 0;
    {
      const uint32_t length = (uint32_t)p->message[4] | (uint32_t)p->message[5] << 8 | (uint32_t)p->message[6] << 16 | (uint32_t)p->message[7] << 24;
      if (length > OB_MAX_PAYLOAD) { p->dropped++; p->state = 0; return 0; }
      p->need = 8 + length;
      p->state = length ? 3 : 4;
    }
    return 0;
  case 3:
    p->message[p->have++] = byte;
    if (p->have == p->need) p->state = 4;
    return 0;
  case 4:
    p->crc = byte;
    p->state = 5;
    return 0;
  default:
    p->crc |= (uint16_t)((uint16_t)byte << 8);
    p->state = 0;
    if (p->crc != obs_crc16(0xffffu, p->message, p->have)) { p->dropped++; return 0; }
    p->frames++;
    return 1;
  }
}
#endif

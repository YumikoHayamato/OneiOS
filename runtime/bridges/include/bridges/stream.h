// SPDX-License-Identifier: MIT
// The bridge channel's phone end on a byte stream (devices/bridge_stream.c): what devices/bridge_dev.c is for
// xnuhost's "oneios,bridge" virtio device, for a guest that has only a serial port to the phone. That is the native
// path's VM of crosvm (docs/native.md "The bridge in the crosvm VM"): the kernel's bridge driver writes the messages of
// drivers/bridge/oneios_bridge_proto.h as frames (drivers/bridge/oneios_bridge_stream.h) to crosvm's second ns16550,
// crosvm carries the port's bytes on a Unix socket, and `bridgectl serve SOCKET` is this code on its other end.
// Served: control (the hello), haptics and the battery. No xnuhost in it.
#ifndef ONEIOS_BRIDGE_STREAM_SERVE_H
#define ONEIOS_BRIDGE_STREAM_SERVE_H

#include "bridges/bridges.h"
#include "../../battery/battery.h"

typedef struct {
  br_haptic *haptic;                      // NULL: the haptic channel is not served
  br_battery *battery;                    // NULL: the battery channel is not served
} br_stream_config;

// What went over the stream, for the log and the tests.
typedef struct {
  uint64_t bytes_in, bytes_out;
  uint64_t frames_in, frames_bad;         // whole messages from the guest; frames given up (length, checksum)
  uint64_t hellos, haptic, battery_queries, battery_states, unknown;
} br_stream_stats;

// Serves fd (a connected stream socket, or any descriptor poll() and read() take) until the other end closes it
// (returns 0), *stop is set (0; looked at every 200 ms) or a read or write fails (-1). At its end the motor is
// stopped and the battery is no longer read. stats may be NULL.
int br_stream_serve(int fd, const br_stream_config *config, const volatile int *stop, br_stream_stats *stats);
#endif

// SPDX-License-Identifier: MIT
// The display helper's portable core: xnuhost's display.sock protocol (runtime/xnuhost/src/display.c,
// docs/booter.md 2.8), the letterbox fit, and the row copy. No Android here, so the host tests run it.
#ifndef ONEIOS_XHD_H
#define ONEIOS_XHD_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "../../xnuhost/include/xnuhost/pixels.h"

// The hello's format: what the frame's pixels hold (xnuhost's pixels.h says it in full).
#define XHD_FORMAT_BGRX    XH_PIXELS_BGRX8      // bytes B,G,R,X, 8-bit sRGB: iOS's 'BGRA' framebuffer
#define XHD_FORMAT_RGBA16F XH_PIXELS_RGBA16F    // R,G,B,A half floats, linear extended sRGB: iOS's 'RGhA'
#define XHD_MAX_SIDE 16384u

// The layer's buffer layouts the copy writes (Android's AHARDWAREBUFFER_FORMAT_* values).
#define XHD_BUFFER_RGBX_8888 2                   // bytes R,G,B,X
#define XHD_BUFFER_BGRA_8888 5                   // bytes B,G,R,A
#define XHD_BUFFER_RGBA_FP16 22                  // R,G,B,A half floats

// Android's dataspaces (ADataSpace) the helper tags the buffers with.
#define XHD_DATASPACE_UNKNOWN      0
#define XHD_DATASPACE_SRGB         142671872     // STANDARD_BT709 | TRANSFER_SRGB | RANGE_FULL
#define XHD_DATASPACE_DISPLAY_P3   143261696     // STANDARD_DCI_P3 | TRANSFER_SRGB | RANGE_FULL
#define XHD_DATASPACE_SCRGB        411107328     // STANDARD_BT709 | TRANSFER_SRGB | RANGE_EXTENDED
#define XHD_DATASPACE_SCRGB_LINEAR 406913024     // STANDARD_BT709 | TRANSFER_LINEAR | RANGE_EXTENDED
// The buffer and dataspace for a frame format: B,G,R,A with sRGB for 8-bit frames (the colours iOS composited them
// in), RGBA_FP16 with SCRGB_LINEAR for half-float ones (the same numbers, the same meaning). 0 for an unknown format.
int xhd_buffer_for(uint32_t format, int *dataspace);
const char *xhd_dataspace_name(int dataspace);
// --dataspace's names: auto (-1), unknown, srgb, p3, scrgb, scrgb-linear. -2 for a name it does not know.
int xhd_dataspace_parse(const char *name);

// One SOCK_SEQPACKET message, little-endian:
//   hello  { "XHD1", width, height, stride, format } + the frame memfd (SCM_RIGHTS)
//   flush  { "XHDF", x, y, w, h }
// and from the helper, straight after connecting (xnuhost waits for it before it sizes the frame in a session):
//   panel  { "XHDP", width, height, hz, 0 }
// The frame handshake (2026-11-19, S25 Ultra): a sender that will not write the frame while the helper copies it asks
// for acks with  want { "XHDW", 1, 0, 0, 0 }  after the hello; the helper then answers each post with
//   ack    { "XHDA", posts, x, y, w }   (posts counted from 1; x, y, w of the copied rectangle, informative)
// and the sender writes the frame again only after the ack (or a timeout, for a helper without acks). With no
// "XHDW" (xnuhost today) the helper sends nothing: nothing changes. Unknown messages are ignored both ways.
struct xhd_message { char magic[4]; uint32_t a, b, c, d; };

struct xhd_rect { uint32_t x, y, w, h; };            // w == 0: empty

struct xhd_frame {
  uint32_t width, height, stride, format;
  int memfd;
  const uint8_t *pixels;                              // read-only map of the memfd
  size_t bytes;
};

// The session's folder is writable by the guest, so nothing from the socket is trusted:
// 0 when the hello describes a frame that fits in memfd_size bytes, else -EPROTO/-EINVAL.
int xhd_check_hello(const struct xhd_message *m, size_t length, off_t memfd_size);
// Connect to the socket at path, waiting up to wait_ms for it to appear; *stop != 0 ends the wait.
// Refuses anything that is not a socket (a link included). Returns the fd or -errno.
int xhd_connect(const char *path, int wait_ms, const volatile int *stop);
// Tell xnuhost the panel's mode. 0 or -errno.
int xhd_send_panel(int fd, uint32_t width, uint32_t height, uint32_t hz);
// Receive the hello and map the frame. 0 or -errno.
int xhd_recv_hello(int fd, struct xhd_frame *frame, int timeout_ms);
void xhd_frame_close(struct xhd_frame *frame);
// Read every pending flush without blocking, adding each (clipped to the frame) to *damage.
// 0 the socket is open, -1 it closed.
int xhd_drain(int fd, const struct xhd_frame *frame, struct xhd_rect *damage);
// Whether the sender asked for acks ("XHDW" seen by xhd_drain on this process's connection).
int xhd_acks_wanted(void);
// The sender's side: ask for acks; the helper's side: one ack (non-blocking: a full socket drops it). 0 or -errno.
int xhd_send_want_acks(int fd);
int xhd_send_ack(int fd, uint32_t posts, const struct xhd_rect *copied);
// The sender's side: wait up to timeout_ms for the next ack, reading (and dropping) anything else. 1 an ack, 0 the
// timeout, -1 the socket closed or failed.
int xhd_wait_ack(int fd, int timeout_ms);

// The first post waits for the guest (2026-11-15): the layer is above the OneOS boot screen and opaque, so a posted
// frame covers it, and until session/ready the frame is black (the empty memfd, then backboardd's first frames) or the
// kernel console. 1 while `ready` (SESSION/ready, which xnuhost makes at the first frame of the guest's own with
// content, or ready-content-wait seconds after its first) does not exist; 0 when it does, or with no session (NULL:
// --socket, displayctl). Any error but ENOENT counts as there, so a fault here never keeps the picture away.
#define XHD_HOLD_POLL_MS 250                           // how often a held helper looks for `ready` between flushes
int xhd_hold_first(const char *ready);

int xhd_rect_clip(struct xhd_rect *r, uint32_t width, uint32_t height);   // 0 when nothing is left
void xhd_rect_union(struct xhd_rect *into, const struct xhd_rect *r);

// The largest rectangle with the source's aspect inside dw x dh, centred.
struct xhd_fit { uint32_t x, y, w, h; };
void xhd_fit(uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh, struct xhd_fit *fit);

// Copy rectangle r of a frame in `format` (XHD_FORMAT_*) into a buffer of the same size laid out as `buffer`
// (XHD_BUFFER_*; dst_stride in bytes). A frame into a buffer of its own layout is copied as it is (B,G,R,X into
// B,G,R,A, half floats into RGBA_FP16); B,G,R,X into R,G,B,X swaps (alpha 0xff); half floats into an 8-bit buffer
// are encoded to sRGB and clamped (what is outside sRGB is lost); 8-bit into RGBA_FP16 is decoded to linear.
// 0, or -EINVAL for a format or buffer it does not know.
int xhd_copy(uint8_t *dst, size_t dst_stride, int buffer, const uint8_t *src, size_t src_stride, uint32_t format, struct xhd_rect r);

// The serving side (xnuhost's), for displayctl and the tests.
int xhd_listen(const char *path);                                        // fd or -errno
int xhd_send_hello(int fd, uint32_t width, uint32_t height, uint32_t stride, uint32_t format, int memfd);
int xhd_send_flush(int fd, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
int xhd_memfd(const char *name, size_t bytes);                           // fd or -errno

// The bridge of a VM of Android's crosvm (displayctl pool): the native kernel's boot video in crosvm's restricted DMA pool.
// crosvm maps the pool as a mapping of its own of the guest's memfd: in the text of its /proc/PID/maps, the line of a
// shared, readable mapping of "/memfd:crosvm_guest" at file offset `offset`, `bytes` long. 0 and its start, end and
// inode, or -ENOENT. `shared_ram`: Droid-VM's shared-RAM VM (xnuloader-crosvm --shared-ram), whose RAM is the host's to
// read as well: the devices' region is then part of a longer mapping of the memfd, and any shared, readable mapping
// that holds [offset, offset + bytes) counts. Never for a protected VM, whose other mappings are its lent RAM.
int xhd_pool_mapping(const char *maps, uint64_t offset, uint64_t bytes, int shared_ram, uint64_t *start, uint64_t *end,
                     uint64_t *inode);
// The guest's frame (src: rows of width * 4 bytes, `stride` apart) into the helper's (dst, the same layout): a row is
// read only when every page_size page it touches is resident (resident[i] & 1 for page i of src, as mincore says; no
// read makes the host allocate guest memory), and written only when it differs. `invalidate`: clean and invalidate the
// row's cache lines first (arm64; the guest's console writes the boot video uncached). The rows written, and in
// *changed the band they span (w == 0: none).
uint32_t xhd_pool_copy(uint8_t *dst, const uint8_t *src, uint32_t width, uint32_t height, uint32_t stride,
                       const unsigned char *resident, size_t page_size, int invalidate, struct xhd_rect *changed);
// The guest's frame count (OneiOSDisplay.cpp oneios_display_frame_done) in its page: 1 and *count when the page has
// "OFSQ" version 1, else 0.
#define XHD_FRAME_SEQ_MAGIC 0x000000015153464full
int xhd_frame_seq(const uint8_t *page, uint64_t *count);

// The touch of a VM of crosvm (displayctl touch): the phone's touchscreen to crosvm's multi-touch device, which reads
// virtio_input_event's from its socket (type, code, value: 8 bytes, little-endian). 1 and *out for the events a
// multi-touch frame is made of (EV_SYN SYN_REPORT, BTN_TOUCH, ABS_X, ABS_Y, the MT slot, tracking id, positions and
// contact size), passed as they are; 0 for the rest (the touchscreen's other keys and switches, EV_MSC, SYN_DROPPED).
struct xhd_vinput { uint16_t type, code; uint32_t value; };
int xhd_touch_event(uint16_t type, uint16_t code, int32_t value, struct xhd_vinput *out);
// The side keys of a takeover (displayctl touch --keys: One UI is down, so the phone's keys are the VM's to read, never to
// grab): each press and release of power, volume up and volume down goes to the guest as it comes, an EV_KEY and a
// SYN_REPORT (its virtio-input driver hands them to iOS's HID buttons: lock and wake, the volume); the autorepeat of a held
// key is not sent (iOS repeats a held volume key itself). A click of power (released within XHD_POWER_CLICK_S of its press)
// also asks for the panel to be switched (*toggle = 1). *power_down: when power went down (0: up), kept by the caller.
// Returns the events written to out, 0 for anything else.
#define XHD_POWER_CLICK_S 2.0
// Whether a click of power (xhd_side_key's toggle) switches the panel too: only with displayctl touch --power-blanks. By default
// the key goes to the VM alone and the panel stays as it is (take14: the panel went black by itself on a short click, and
// iOS, which owns the screen in a takeover, did not ask for it).
int xhd_power_blanks_panel(int toggle, int power_blanks);

unsigned xhd_side_key(uint16_t type, uint16_t code, int32_t value, double now, double *power_down, int *toggle,
                      struct xhd_vinput out[2]);
#endif

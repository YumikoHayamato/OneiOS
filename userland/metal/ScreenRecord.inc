// SPDX-License-Identifier: MIT
//
// OneiOS: screen recording. A CPU stand-in for the scaler (IOSurfaceAccelerator) inside backboardd. Appended to the Metal
// plug-in's NativeAdrenoMetalPlugin.mm by userland/metal/build.sh (stage), as DisplayWide.inc is: the plug-in is the
// OneiOS code that runs inside backboardd (the render server).
//
// Why [read in 24A446; screenrec README "Root cause" has the addresses]:
//   The Control Center button makes replayd ask backboardd for a virtual display (FigVirtualFramebuffer). backboardd plugs
//   it, clones Display 1 onto it, and on every frame CA::WindowServer::IOMFBDisplay::clone_update copies the composed
//   frame into replayd's writeback buffer with IOMFBDisplay::copy_surface. copy_surface has two ways: the scaler
//   (Surface::copy_iosurface -> IOSurfaceAccelerator) when CA::ScalerUtil::get_io_service() found AppleM2ScalerCSCDriver or
//   AppleM2ScalerParavirtDriver, else the GPU (MetalContext::get_surface for both surfaces, then MetalContext::copy_surface).
//   This guest has no scaler service, so the GPU way is taken, and it fails on every frame ("failed to copy main layer",
//   926 in take18's attempt; replayd: 0 source frames out of 927 writeback requests, -5833 "start timeout").
//   (The one "get_wireless_surface_options returned error E00002D5" is the first writeback request, answered kIOReturnBusy
//   by replayd before its writeback pool existed; CA logs each failed request and there is only one.)
//   This file makes the scaler way exist in backboardd, for the time a display is a clone.
//
// What this does, in backboardd only, when the guest has no scaler service: the entries of IOSurfaceAcceleratorCreate,
// ...GetRunLoopSource and ...TransformSurface (the three QuartzCore's IOSAManager and copy_iosurface call) jump to the
// functions below; the framework's other entries that take an accelerator answer kIOReturnUnsupported (or 0), so nothing
// hands our object to Apple's code. TransformSurface copies the source surface's rectangle into the destination's with
// the CPU: 'BGRA' or 'RGhA' (half floats, oneios-display-wide=1) in; 'BGRA', '420v', '420f', 'x420' or 'xf20' out
// (BT.709, 2x2 box filter when the size halves, nearest otherwise). It honours the shared events QuartzCore passes
// (kIOSurfaceAcceleratorSharedEvent{Src,Dst}{Ref,Wait,Signal}) and delivers the asynchronous completion on the run loop
// QuartzCore added our run loop source to, as the real framework's notification port does.
// The entries are patched as NativeMetal's patchFramebufferBranch patches QuartzCore (vm_protect VM_PROT_COPY, vm_write):
// this process's copy of the pages only. No shared cache address is written here: dlsym finds the entries.
//
// Switch: OPT-IN, boot argument oneios-screenrec=1. Without it nothing is patched (recording stops at -5833 as before).
// With a real AppleM2Scaler* service (not this guest) it also does nothing.
// EXPERIMENT: the entries are rewritten in this process's copy of a shared-cache text page. Whether this guest's code
// signing lets backboardd execute that page is not known (AMFI runs with Apple's trust caches); if not, backboardd is
// killed at the first call into IOSurfaceAccelerator, i.e. when a recording starts and the gate below opens.
//
// What it logs (iOS's log, process backboardd):
//   "OneiOS screenrec: CPU scaler installed (N entries)"      at start, or "... not installed: <why>"
//   "OneiOS screenrec: first copy SRC WxH 'FMT' -> DST WxH 'FMT' (async A, events S/D)"
//   "OneiOS screenrec: N copies, last M us, average A us, failed F"   every 600 copies
//   "OneiOS screenrec: cannot copy 'FMT' -> 'FMT' (...)"              once per pair of formats it does not do

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------------------------------------------------
// The pixel core: no Apple API, so tests/screenrec-core-test.cc can build it on the PC (ONEIOS_SCREENREC_CORE_ONLY).
// ---------------------------------------------------------------------------------------------------------------------
namespace oneios_screenrec {

static constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return (uint32_t)(uint8_t)a << 24 | (uint32_t)(uint8_t)b << 16 | (uint32_t)(uint8_t)c << 8 | (uint32_t)(uint8_t)d;
}
static constexpr uint32_t kBGRA = fourcc('B', 'G', 'R', 'A');
static constexpr uint32_t kRGhA = fourcc('R', 'G', 'h', 'A');
static constexpr uint32_t k420v = fourcc('4', '2', '0', 'v');
static constexpr uint32_t k420f = fourcc('4', '2', '0', 'f');
static constexpr uint32_t kx420 = fourcc('x', '4', '2', '0');
static constexpr uint32_t kxf20 = fourcc('x', 'f', '2', '0');

struct Plane { uint8_t *base; size_t stride; size_t width, height; };
struct Image { uint32_t format; unsigned planes; Plane plane[2]; };
struct Rect { size_t x, y, w, h; };

static bool srcSupported(uint32_t f) { return f == kBGRA || f == kRGhA; }
static bool dstSupported(uint32_t f) { return f == kBGRA || f == k420v || f == k420f || f == kx420 || f == kxf20; }

static inline float halfToFloat(uint16_t h) {
    const uint32_t sign = (uint32_t)(h & 0x8000) << 16, exp = (h >> 10) & 0x1f, man = h & 0x3ff;
    uint32_t bits;
    if (exp == 0) {
        if (!man) bits = sign;
        else {  // subnormal
            int e = -1;
            uint32_t m = man;
            do { m <<= 1; e++; } while (!(m & 0x400));
            bits = sign | (uint32_t)(127 - 15 - e) << 23 | (m & 0x3ff) << 13;
        }
    } else if (exp == 31) bits = sign | 0x7f800000 | man << 13;
    else bits = sign | (exp + 127 - 15) << 23 | man << 13;
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}
static inline float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
// linear extended sRGB (the 'RGhA' framebuffer) -> sRGB-encoded 0..255 (values past 1, EDR, are clipped)
static inline uint8_t linearToSrgb8(float v) {
    v = clamp01(v);
    // the sRGB curve from a 4097-entry table built at the first use
    static uint8_t table[4097];
    static bool built;
    if (!built) {
        for (int i = 0; i <= 4096; i++) {
            const double l = i / 4096.0;
            double s = l <= 0.0031308 ? 12.92 * l : 1.055 * __builtin_pow(l, 1.0 / 2.4) - 0.055;
            table[i] = (uint8_t)(s * 255.0 + 0.5);
        }
        built = true;
    }
    return table[(int)(v * 4096.f + 0.5f)];
}

// One destination row of the copy as 8-bit B, G, R, A (sRGB-encoded): source row(s) for it, a 2x2 box average when the
// source is exactly twice as large in both directions, the nearest pixel otherwise (xmap: source column per column).
struct RowFetcher {
    const Image &src; Rect s, d; bool box; size_t *xmap;
    RowFetcher(const Image &src_, Rect s_, Rect d_, size_t *xmap_) : src(src_), s(s_), d(d_), box(s_.w == 2 * d_.w && s_.h == 2 * d_.h), xmap(xmap_) {
        for (size_t dx = 0; dx < d.w; dx++) {
            size_t x = s.x + (dx * s.w + s.w / 2) / d.w;  // centre of the destination pixel, in source columns
            xmap[dx] = x < s.x + s.w ? x : s.x + s.w - 1;
        }
    }
    inline const uint8_t *srcRow(size_t y) const { return src.plane[0].base + y * src.plane[0].stride; }
    void fetch(size_t dy, uint8_t *out) const {
        if (src.format == kBGRA && box) {
            const uint8_t *r0 = srcRow(s.y + 2 * dy) + s.x * 4, *r1 = srcRow(s.y + 2 * dy + 1) + s.x * 4;
            for (size_t dx = 0; dx < d.w; dx++, r0 += 8, r1 += 8, out += 4) {
                out[0] = (uint8_t)((r0[0] + r0[4] + r1[0] + r1[4] + 2) >> 2);
                out[1] = (uint8_t)((r0[1] + r0[5] + r1[1] + r1[5] + 2) >> 2);
                out[2] = (uint8_t)((r0[2] + r0[6] + r1[2] + r1[6] + 2) >> 2);
                out[3] = 0xff;
            }
            return;
        }
        size_t y = s.y + (dy * s.h + s.h / 2) / d.h;
        if (y >= s.y + s.h) y = s.y + s.h - 1;
        const uint8_t *row = srcRow(y);
        if (src.format == kBGRA) {
            if (s.w == d.w) { memcpy(out, row + s.x * 4, d.w * 4); for (size_t dx = 0; dx < d.w; dx++) out[4 * dx + 3] = 0xff; return; }
            for (size_t dx = 0; dx < d.w; dx++, out += 4) {
                const uint8_t *q = row + xmap[dx] * 4;
                out[0] = q[0]; out[1] = q[1]; out[2] = q[2]; out[3] = 0xff;
            }
            return;
        }
        // kRGhA: R, G, B, A half floats, linear extended sRGB; nearest (box filtering half floats is not worth it here)
        for (size_t dx = 0; dx < d.w; dx++, out += 4) {
            const uint16_t *q = (const uint16_t *)(row + xmap[dx] * 8);
            out[2] = linearToSrgb8(halfToFloat(q[0]));
            out[1] = linearToSrgb8(halfToFloat(q[1]));
            out[0] = linearToSrgb8(halfToFloat(q[2]));
            out[3] = 0xff;
        }
    }
};

// BT.709 in 16.16 fixed point; video range: Y 16..235, C 16..240; full range: 0..255.
static inline int lumaOf(int r, int g, int b, bool full) {
    return full ? (13933 * r + 46871 * g + 4732 * b + 32768) >> 16                  // 0.2126 0.7152 0.0722
                : ((11966 * r + 40254 * g + 4064 * b + 32768) >> 16) + 16;          // *219/255
}
static inline void chromaOf(int r, int g, int b, bool full, int &cb, int &cr) {    // r, g, b: sums of 4 pixels
    if (full) {
        cb = ((-7509 * r - 25259 * g + 32768 * b + 131072) >> 18) + 128;            // /1.8556
        cr = ((32768 * r - 29763 * g - 3005 * b + 131072) >> 18) + 128;             // /1.5748
    } else {
        cb = ((-6596 * r - 22189 * g + 28785 * b + 131072) >> 18) + 128;            // *224/255
        cr = ((28785 * r - 26145 * g - 2640 * b + 131072) >> 18) + 128;
    }
    cb = cb < 0 ? 0 : (cb > 255 ? 255 : cb);
    cr = cr < 0 ? 0 : (cr > 255 ? 255 : cr);
}
static inline uint16_t to10(int v, bool full) { return (uint16_t)((full ? (v << 2 | v >> 6) : v << 2) << 6); }

// Copies src's rectangle s into dst's rectangle d (both already clipped to their surfaces). false: a format it does not do
// or no scratch memory. scratch: 2 * 4 * d.w bytes plus d.w size_t (scratchBytes).
static size_t scratchBytes(Rect d) { return 8 * d.w + sizeof(size_t) * d.w; }
// Rows [y0, y1) of the destination rectangle only (y0 even), so that bands can run on several threads.
static bool copyRect(const Image &src, Rect s, const Image &dst, Rect d, uint8_t *scratch, size_t y0 = 0, size_t y1 = SIZE_MAX) {
    if (!srcSupported(src.format) || !dstSupported(dst.format) || !s.w || !s.h || !d.w || !d.h || (y0 & 1)) return false;
    if (y1 > d.h) y1 = d.h;
    uint8_t *row0 = scratch, *row1 = scratch + 4 * d.w;
    const RowFetcher fetch(src, s, d, (size_t *)(scratch + 8 * d.w));
    if (dst.format == kBGRA) {
        const Plane &p = dst.plane[0];
        for (size_t dy = y0; dy < y1; dy++) fetch.fetch(dy, p.base + (d.y + dy) * p.stride + d.x * 4);
        return true;
    }
    if (dst.planes < 2) return false;
    const bool full = dst.format == k420f || dst.format == kxf20, wide = dst.format == kx420 || dst.format == kxf20;
    const Plane &py = dst.plane[0], &pc = dst.plane[1];
    for (size_t dy = y0; dy < y1; dy += 2) {
        const bool two = dy + 1 < y1;
        fetch.fetch(dy, row0);
        if (two) fetch.fetch(dy + 1, row1); else memcpy(row1, row0, 4 * d.w);
        for (int j = 0; j < (two ? 2 : 1); j++) {
            const uint8_t *in = j ? row1 : row0;
            const size_t Y = d.y + dy + j;
            if (wide) {
                uint16_t *out = (uint16_t *)(py.base + Y * py.stride) + d.x;
                for (size_t dx = 0; dx < d.w; dx++, in += 4) out[dx] = to10(lumaOf(in[2], in[1], in[0], full), full);
            } else {
                uint8_t *out = py.base + Y * py.stride + d.x;
                for (size_t dx = 0; dx < d.w; dx++, in += 4) out[dx] = (uint8_t)lumaOf(in[2], in[1], in[0], full);
            }
        }
        const size_t cy = (d.y + dy) / 2;
        for (size_t dx = 0; dx < d.w; dx += 2) {
            const size_t i = 4 * dx, k = dx + 1 < d.w ? i + 4 : i;  // an odd last column: its own pixel twice
            const int b = row0[i] + row0[k] + row1[i] + row1[k], g = row0[i + 1] + row0[k + 1] + row1[i + 1] + row1[k + 1],
                      r = row0[i + 2] + row0[k + 2] + row1[i + 2] + row1[k + 2];
            int cb, cr;
            chromaOf(r, g, b, full, cb, cr);
            const size_t cx = (d.x + dx) / 2;
            if (wide) {
                uint16_t *c = (uint16_t *)(pc.base + cy * pc.stride) + 2 * cx;
                c[0] = to10(cb, full); c[1] = to10(cr, full);
            } else {
                uint8_t *c = pc.base + cy * pc.stride + 2 * cx;
                c[0] = (uint8_t)cb; c[1] = (uint8_t)cr;
            }
        }
    }
    return true;
}

// The 48-byte rectangle argument of IOSurfaceAcceleratorTransformSurface as QuartzCore's copy_iosurface fills it
// (24A446 0x1845ca388..0x1845ca3a4; IOSurfaceAccelerator's prepareTransformRectangles copies it whole): the source
// rectangle in 48.16 fixed point, then the destination rectangle in whole pixels.
struct TransformRects {
    uint64_t srcX, srcY, srcW, srcH;  // << 16
    uint32_t dstX, dstY, dstW, dstH;
};
static_assert(sizeof(TransformRects) == 48, "the framework copies 48 bytes");

// The rectangles to copy, clipped to the surfaces. With no argument: whole source to whole destination.
static bool rectsFor(const TransformRects *t, size_t sw, size_t sh, size_t dw, size_t dh, Rect &s, Rect &d) {
    if (!t) { s = {0, 0, sw, sh}; d = {0, 0, dw, dh}; return sw && sh && dw && dh; }
    s = {(size_t)(t->srcX >> 16), (size_t)(t->srcY >> 16), (size_t)((t->srcW + 0x8000) >> 16), (size_t)((t->srcH + 0x8000) >> 16)};
    d = {t->dstX, t->dstY, t->dstW, t->dstH};
    if (!s.w && !s.h) s = {0, 0, sw, sh};
    if (!d.w && !d.h) d = {0, 0, dw, dh};
    if (s.x >= sw || s.y >= sh || d.x >= dw || d.y >= dh) return false;
    if (s.x + s.w > sw) s.w = sw - s.x;
    if (s.y + s.h > sh) s.h = sh - s.y;
    if (d.x + d.w > dw) d.w = dw - d.x;
    if (d.y + d.h > dh) d.h = dh - d.y;
    return s.w && s.h && d.w && d.h;
}

}  // namespace oneios_screenrec

#ifndef ONEIOS_SCREENREC_CORE_ONLY
// ---------------------------------------------------------------------------------------------------------------------
// backboardd: the accelerator. Everything Apple's is reached with dlsym (IOKit has no headers in the iOS SDK).
// ---------------------------------------------------------------------------------------------------------------------
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <os/log.h>
#include <initializer_list>
#include <new>
#include <pthread.h>
#include <ptrauth.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <unistd.h>

namespace oneios_screenrec {

typedef struct __IOSurface *SurfaceRef;
struct Completion { void (*func)(void *refcon, int32_t status, void *info); void *refcon; void *info; };

static const size_t kBands = 4;
static const int32_t kIOReturnSuccess_ = 0;
static const int32_t kIOReturnBadArgument_ = (int32_t)0xE00002C2;
static const int32_t kIOReturnUnsupported_ = (int32_t)0xE00002C7;
static const int32_t kIOReturnNoResources_ = (int32_t)0xE00002BE;

// IOSurface (public C API; reached with dlsym so the plug-in's link list does not change)
static struct {
    uint32_t (*pixelFormat)(SurfaceRef);
    size_t (*width)(SurfaceRef), (*height)(SurfaceRef), (*planeCount)(SurfaceRef);
    size_t (*widthOfPlane)(SurfaceRef, size_t), (*heightOfPlane)(SurfaceRef, size_t), (*bytesPerRowOfPlane)(SurfaceRef, size_t);
    size_t (*bytesPerRow)(SurfaceRef);
    void *(*baseAddress)(SurfaceRef), *(*baseAddressOfPlane)(SurfaceRef, size_t);
    int32_t (*lock)(SurfaceRef, uint32_t, uint32_t *), (*unlock)(SurfaceRef, uint32_t, uint32_t *);
} S;

struct Pending { Completion c; int32_t status; };
struct Accel {
    CFRunLoopSourceRef source = nullptr;
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER, copying = PTHREAD_MUTEX_INITIALIZER;
    CFRunLoopRef loops[4] = {};
    unsigned loopCount = 0;
    Pending *pending = nullptr;
    size_t count = 0, capacity = 0;
    uint8_t *scratch = nullptr;
    size_t scratchSize = 0;
};

static CFStringRef keySrcRef, keySrcWait, keySrcSignal, keyDstRef, keyDstWait, keyDstSignal, keySkipAsync;
static uint64_t gCopies, gFailed, gTotalUs, gLastUs;
static bool gFirstLogged;
static uint32_t gRefused[8][2];
static unsigned gRefusedCount;
static mach_timebase_info_data_t gTimebase;

static void say(const char *what) { os_log(OS_LOG_DEFAULT, "OneiOS screenrec: %{public}s", what); }
static void fourccText(uint32_t f, char out[5]) {
    for (int i = 0; i < 4; i++) { char c = (char)(f >> (24 - 8 * i)); out[i] = c >= 32 && c < 127 ? c : '?'; }
    out[4] = 0;
}

// The accelerator object QuartzCore keeps and CFReleases: a CFData whose bytes hold an Accel. Its deallocator runs ours.
static pthread_mutex_t gAccelsLock = PTHREAD_MUTEX_INITIALIZER;
static struct { const void *ref; Accel *accel; } gAccels[16];
static void accelDealloc(void *ptr, void *) {
    Accel *a = (Accel *)ptr;
    pthread_mutex_lock(&gAccelsLock);
    for (auto &x : gAccels) if (x.accel == a) x = {nullptr, nullptr};
    pthread_mutex_unlock(&gAccelsLock);
    if (a->source) { CFRunLoopSourceInvalidate(a->source); CFRelease(a->source); }
    for (unsigned i = 0; i < a->loopCount; i++) CFRelease(a->loops[i]);
    free(a->pending);
    free(a->scratch);
    a->~Accel();
    free(a);
}
static Accel *accelOf(const void *ref) {
    if (!ref) return nullptr;
    pthread_mutex_lock(&gAccelsLock);
    Accel *found = nullptr;
    for (const auto &x : gAccels) if (x.ref == ref) found = x.accel;
    pthread_mutex_unlock(&gAccelsLock);
    return found;
}
// The run loop source's callbacks: QuartzCore adds the source to its run loop (create_new_iosa_manager); completions are
// queued by TransformSurface and run here, on that run loop, as the framework's notification port would run them.
static void sourceSchedule(void *info, CFRunLoopRef loop, CFStringRef) {
    Accel *a = (Accel *)info;
    pthread_mutex_lock(&a->lock);
    if (a->loopCount < 4) { CFRetain(loop); a->loops[a->loopCount++] = loop; }
    pthread_mutex_unlock(&a->lock);
}
static void sourcePerform(void *info) {
    Accel *a = (Accel *)info;
    for (;;) {
        pthread_mutex_lock(&a->lock);
        if (!a->count) { pthread_mutex_unlock(&a->lock); return; }
        const Pending p = a->pending[0];
        memmove(a->pending, a->pending + 1, --a->count * sizeof *a->pending);
        pthread_mutex_unlock(&a->lock);
        p.c.func(p.c.refcon, p.status, p.c.info);
    }
}
// false: no run loop has the source yet (nothing queued; the caller then calls back at once).
static bool queueCompletion(Accel *a, const Completion &c, int32_t status) {
    pthread_mutex_lock(&a->lock);
    const unsigned n = a->loopCount;
    if (!n) { pthread_mutex_unlock(&a->lock); return false; }
    if (a->count == a->capacity) {
        const size_t want = a->capacity ? 2 * a->capacity : 8;
        Pending *grown = (Pending *)realloc(a->pending, want * sizeof *grown);
        if (!grown) { pthread_mutex_unlock(&a->lock); return false; }
        a->pending = grown; a->capacity = want;
    }
    a->pending[a->count++] = {c, status};
    CFRunLoopRef loops[4];
    for (unsigned i = 0; i < n; i++) loops[i] = a->loops[i];
    pthread_mutex_unlock(&a->lock);
    CFRunLoopSourceSignal(a->source);
    for (unsigned i = 0; i < n; i++) CFRunLoopWakeUp(loops[i]);
    return true;
}

// int32_t IOSurfaceAcceleratorCreate(CFAllocatorRef, CFDictionaryRef properties, IOSurfaceAcceleratorRef *out)
static int32_t create(CFAllocatorRef, CFDictionaryRef, const void **out) {
    if (!out) return kIOReturnBadArgument_;
    *out = nullptr;
    void *mem = calloc(1, sizeof(Accel));
    if (!mem) return kIOReturnNoResources_;
    Accel *a = new (mem) Accel();
    CFRunLoopSourceContext context = {};
    context.info = a;
    context.schedule = sourceSchedule;
    context.perform = sourcePerform;
    a->source = CFRunLoopSourceCreate(kCFAllocatorDefault, 0, &context);
    CFAllocatorContext deallocContext = {};
    deallocContext.allocate = [](CFIndex size, CFOptionFlags, void *) -> void * { return malloc((size_t)size); };
    deallocContext.deallocate = accelDealloc;
    // A CFData with no copy of the bytes, freed by accelDealloc: CFRelease of the handle tears the accelerator down.
    CFAllocatorRef deallocator = CFAllocatorCreate(kCFAllocatorDefault, &deallocContext);
    CFDataRef ref = deallocator ? CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, (const UInt8 *)a, sizeof(Accel), deallocator) : nullptr;
    if (deallocator) CFRelease(deallocator);
    if (!ref || !a->source) {
        if (ref) CFRelease(ref); else accelDealloc(a, nullptr);
        return kIOReturnNoResources_;
    }
    pthread_mutex_lock(&gAccelsLock);
    bool kept = false;
    for (auto &x : gAccels) if (!kept && !x.ref) { x = {ref, a}; kept = true; }
    pthread_mutex_unlock(&gAccelsLock);
    if (!kept) { CFRelease(ref); return kIOReturnNoResources_; }  // 16 live accelerators: QuartzCore keeps one a display
    *out = ref;
    return kIOReturnSuccess_;
}

// CFRunLoopSourceRef IOSurfaceAcceleratorGetRunLoopSource(IOSurfaceAcceleratorRef)
static CFRunLoopSourceRef getRunLoopSource(const void *ref) {
    Accel *a = accelOf(ref);
    return a ? a->source : nullptr;
}

static bool numberU64(CFDictionaryRef d, CFStringRef key, uint64_t &out) {
    if (!d || !key) return false;
    CFTypeRef v = CFDictionaryGetValue(d, key);
    if (!v || CFGetTypeID(v) != CFNumberGetTypeID()) return false;
    long long n = 0;
    if (!CFNumberGetValue((CFNumberRef)v, kCFNumberLongLongType, &n)) return false;
    out = (uint64_t)n;
    return true;
}
static bool flag(CFDictionaryRef d, CFStringRef key) {
    if (!d || !key) return false;
    CFTypeRef v = CFDictionaryGetValue(d, key);
    if (!v) return false;
    if (CFGetTypeID(v) == CFBooleanGetTypeID()) return CFBooleanGetValue((CFBooleanRef)v);
    uint64_t n = 0;
    return numberU64(d, key, n) && n;
}
// The shared events: the Ref values are IOSurfaceSharedEvent objects (IOSurfaceAccelerator passes them to
// IOSurfaceSharedEventGetMachPort), so -waitUntilSignaledValue:timeoutMS: and -setSignaledValue: reach them here.
// (IOSurfaceSharedEventGetMachPort is a selector stub: [ref eventPort].) An object that does not answer a selector (the
// Metal plug-in's own event class, say) is left alone and logged once.
static bool answers(id object, SEL sel) {
    static SEL respondsTo = sel_registerName("respondsToSelector:");
    return object && ((BOOL(*)(id, SEL, SEL))objc_msgSend)(object, respondsTo, sel);
}
static void eventWait(uint64_t ref, uint64_t value) {
    if (!ref || !value) return;
    static SEL sel = sel_registerName("waitUntilSignaledValue:timeoutMS:");
    id event = (__bridge id)(void *)ref;
    if (answers(event, sel)) ((BOOL(*)(id, SEL, uint64_t, uint64_t))objc_msgSend)(event, sel, value, 100);
    else { static bool said; if (!said) { said = true; say("a shared event without waitUntilSignaledValue:timeoutMS: (not waited)"); } }
}
static void eventSignal(uint64_t ref, uint64_t value) {
    if (!ref || !value) return;
    static SEL sel = sel_registerName("setSignaledValue:");
    id event = (__bridge id)(void *)ref;
    if (answers(event, sel)) ((void (*)(id, SEL, uint64_t))objc_msgSend)(event, sel, value);
    else { static bool said; if (!said) { said = true; say("a shared event without setSignaledValue: (not signalled)"); } }
}

static Image imageOf(SurfaceRef s, void *const base[2]) {
    Image im = {};
    im.format = S.pixelFormat(s);
    const size_t planes = S.planeCount(s);
    if (!planes) {
        im.planes = 1;
        im.plane[0] = {(uint8_t *)base[0], S.bytesPerRow(s), S.width(s), S.height(s)};
    } else {
        im.planes = planes > 2 ? 2 : (unsigned)planes;
        for (unsigned i = 0; i < im.planes; i++)
            im.plane[i] = {(uint8_t *)base[i], S.bytesPerRowOfPlane(s, i), S.widthOfPlane(s, i), S.heightOfPlane(s, i)};
    }
    return im;
}

static void refusedOnce(uint32_t sf, uint32_t df, const char *why) {
    for (unsigned i = 0; i < gRefusedCount; i++) if (gRefused[i][0] == sf && gRefused[i][1] == df) return;
    if (gRefusedCount < 8) { gRefused[gRefusedCount][0] = sf; gRefused[gRefusedCount][1] = df; gRefusedCount++; }
    char a[5], b[5];
    fourccText(sf, a); fourccText(df, b);
    os_log(OS_LOG_DEFAULT, "OneiOS screenrec: cannot copy '%{public}s' -> '%{public}s' (%{public}s)", a, b, why);
}

// int32_t IOSurfaceAcceleratorTransformSurface(IOSurfaceAcceleratorRef, IOSurfaceRef src, IOSurfaceRef dst,
//     CFDictionaryRef options, const TransformRects *rects, const Completion *completion, void *, void *)
static int32_t transformSurface(const void *ref, SurfaceRef src, SurfaceRef dst, CFDictionaryRef options,
                                const TransformRects *rects, const Completion *completion, void *, void *) {
    Accel *a = accelOf(ref);
    if (!a || !src || !dst) return kIOReturnBadArgument_;
    const uint64_t t0 = mach_absolute_time();
    uint64_t srcRef = 0, srcWait = 0, srcSignal = 0, dstRef = 0, dstWait = 0, dstSignal = 0;
    numberU64(options, keySrcRef, srcRef); numberU64(options, keySrcWait, srcWait); numberU64(options, keySrcSignal, srcSignal);
    numberU64(options, keyDstRef, dstRef); numberU64(options, keyDstWait, dstWait); numberU64(options, keyDstSignal, dstSignal);
    eventWait(srcRef, srcWait);  // the writer of the source (the GPU's frame) is done
    eventWait(dstRef, dstWait);  // the destination's last reader is done
    const uint32_t sf = S.pixelFormat(src), df = S.pixelFormat(dst);
    int32_t status = kIOReturnUnsupported_;
    Rect rs, rd;
    if (!srcSupported(sf) || !dstSupported(df)) refusedOnce(sf, df, "format");
    else if (!rectsFor(rects, S.width(src), S.height(src), S.width(dst), S.height(dst), rs, rd)) {
        refusedOnce(sf, df, "rectangle outside the surfaces");
        status = kIOReturnBadArgument_;
    } else if (S.lock(src, 1 /* kIOSurfaceLockReadOnly */, nullptr) == 0) {
        if (S.lock(dst, 0, nullptr) == 0) {
            void *sb[2] = {S.baseAddress(src), nullptr}, *db[2] = {S.baseAddress(dst), nullptr};
            if (S.planeCount(src)) sb[0] = S.baseAddressOfPlane(src, 0);
            if (S.planeCount(dst)) for (size_t i = 0; i < 2 && i < S.planeCount(dst); i++) db[i] = S.baseAddressOfPlane(dst, i);
            const Image is = imageOf(src, sb), id_ = imageOf(dst, db);
            const bool planar = df != kBGRA;
            if (!is.plane[0].base || !id_.plane[0].base || (planar && (id_.planes < 2 || !id_.plane[1].base)))
                refusedOnce(sf, df, "no CPU address (a compressed or unmapped surface)");
            else {
                // Bands of even rows on up to kBands threads (one would hold QuartzCore's render thread ~10 ms a frame
                // at 1440x3120); one copy at a time per accelerator, as the scratch rows are shared.
                pthread_mutex_lock(&a->copying);
                const size_t one = (scratchBytes(rd) + 63) & ~(size_t)63, need = one * kBands;
                if (need > a->scratchSize) {
                    uint8_t *grown = (uint8_t *)realloc(a->scratch, need);
                    if (grown) { a->scratch = grown; a->scratchSize = need; }
                }
                if (need <= a->scratchSize) {
                    const size_t bands = rd.h >= 64 ? kBands : 1, rows = ((rd.h + bands - 1) / bands + 1) & ~(size_t)1;
                    __block bool ok = true;
                    uint8_t *scratch = a->scratch;
                    const Image *ps = &is, *pd = &id_;
                    const Rect s0 = rs, d0 = rd;
                    dispatch_apply(bands, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^(size_t i) {
                        if (i * rows < d0.h && !copyRect(*ps, s0, *pd, d0, scratch + i * one, i * rows, (i + 1) * rows)) ok = false;
                    });
                    if (ok) status = kIOReturnSuccess_;
                }
                pthread_mutex_unlock(&a->copying);
            }
            S.unlock(dst, 0, nullptr);
        }
        S.unlock(src, 1, nullptr);
    }
    eventSignal(srcRef, srcSignal);  // done reading the source
    eventSignal(dstRef, dstSignal);  // the destination holds the frame (also on failure: no one waits forever)
    const uint64_t us = (mach_absolute_time() - t0) * gTimebase.numer / gTimebase.denom / 1000;
    gCopies++; gLastUs = us; gTotalUs += us;
    if (status) gFailed++;
    if (!gFirstLogged && status == kIOReturnSuccess_) {
        gFirstLogged = true;
        char a1[5], b1[5];
        fourccText(sf, a1); fourccText(df, b1);
        os_log(OS_LOG_DEFAULT, "OneiOS screenrec: first copy %zux%zu '%{public}s' -> %zux%zu '%{public}s' (async %d, events %d/%d, %llu us)",
               rs.w, rs.h, a1, rd.w, rd.h, b1, completion && completion->func ? 1 : 0, srcRef ? 1 : 0, dstRef ? 1 : 0,
               (unsigned long long)us);
    }
    if (gCopies % 600 == 0)
        os_log(OS_LOG_DEFAULT, "OneiOS screenrec: %llu copies, last %llu us, average %llu us, failed %llu",
               (unsigned long long)gCopies, (unsigned long long)gLastUs, (unsigned long long)(gTotalUs / gCopies),
               (unsigned long long)gFailed);
    // The framework answers an asynchronous request at once and calls back later; a refused one it answers with the error
    // and no callback. QuartzCore calls the callback itself for a synchronous one (completion is then null).
    if (status == kIOReturnSuccess_ && completion && completion->func && !flag(options, keySkipAsync))
        if (!queueCompletion(a, *completion, status)) completion->func(completion->refcon, status, completion->info);
    return status;
}

// The framework's other entries that take an accelerator: never Apple's code with our object.
static int32_t unsupported() { return kIOReturnUnsupported_; }
static uintptr_t zero() { return 0; }

// The 16-byte jump written at an entry: ldr x16, #8; br x16; .quad target.
static bool redirect(void *entry, void *target, const char *name, char *why, size_t whySize) {
    const uintptr_t at = (uintptr_t)ptrauth_strip(entry, ptrauth_key_function_pointer);
    const uintptr_t to = (uintptr_t)ptrauth_strip(target, ptrauth_key_function_pointer);
    uint32_t head[4] = {};
    vm_size_t got = 0;
    if (vm_read_overwrite(mach_task_self(), at, sizeof head, (vm_address_t)head, &got) != KERN_SUCCESS || got != sizeof head) {
        snprintf(why, whySize, "%s unreadable", name); return false;
    }
    if (head[0] == 0x58000050 && head[1] == 0xd61f0200) return true;  // ours already (a second load of the plug-in)
    // The jump takes 16 bytes: an entry whose body ends sooner (ret, retab, or a tail branch in the first three
    // instructions) would lose the next function's first bytes. 24A446's entries patched here are 24 bytes or more.
    for (int i = 0; i < 3; i++) {
        const uint32_t w = head[i];
        if (w == 0xd65f03c0 || w == 0xd65f0fff || (w & 0xfc000000) == 0x14000000) {
            snprintf(why, whySize, "%s is shorter than the jump", name); return false;
        }
    }
    const uint32_t code[4] = {0x58000050, 0xd61f0200, (uint32_t)to, (uint32_t)(to >> 32)};
    const vm_address_t page = at & ~(uintptr_t)(vm_page_size - 1);
    const vm_size_t span = ((at + sizeof code - 1) & ~(uintptr_t)(vm_page_size - 1)) - page + vm_page_size;
    if (vm_protect(mach_task_self(), page, span, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
        snprintf(why, whySize, "%s: vm_protect", name); return false;
    }
    const bool wrote = vm_write(mach_task_self(), at, (vm_offset_t)code, sizeof code) == KERN_SUCCESS;
    const bool restored = vm_protect(mach_task_self(), page, span, FALSE, VM_PROT_READ | VM_PROT_EXECUTE) == KERN_SUCCESS;
    sys_icache_invalidate((void *)at, sizeof code);
    if (!wrote || !restored) { snprintf(why, whySize, "%s: %s", name, wrote ? "vm_protect back" : "vm_write"); return false; }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// The gate: QuartzCore takes the scaler path only when CA::ScalerUtil::get_io_service() found a scaler service.
// IOMFBDisplay::copy_surface (0x1845c92ec: ldr w8, [get_io_service's value]; csel w8, wzr, w26, eq) and clone_update
// (0x1847b2f24: no service -> copy flag 0x800, "use the GPU") read the value that get_io_service's dispatch_once block
// (0x18454894c, IOServiceGetMatchingService of AppleM2ScalerCSCDriver, then ...ParavirtDriver) stores at 0x1e7020ae8,
// token 0x1e7020ae0. With no service the clone goes to the GPU copy (the renderer's surface textures, vtable +0x448),
// which returns no texture for replayd's buffer here: "failed to copy main layer". The value is set to the registry root
// (a valid entry without scaler properties: ScalerLimits keeps its built-in limits, has_capability says no) only while a
// display has a clone master (a recording, or AirPlay mirroring), and back to 0 when none has, so the compositor's other
// copies and layer decisions are as before outside a recording. Re-evaluated on QuartzCore's own Darwin notification
// "com.apple.CoreAnimation.CAWindowServer.DisplayChanged" (posted after "Display 1 add clone 2").
// The addresses are 24A446's, slid by CARenderServerStart's as DisplayWide.inc does; each is checked against the code
// that reads or writes it before anything is written.
static const uintptr_t kAnchor = 0x1846dce88;           // CARenderServerStart
static const uintptr_t kServiceToken = 0x1e7020ae0;     // get_io_service's dispatch_once token
static const uintptr_t kServiceValue = 0x1e7020ae8;     // its io_service_t (uint32)
static const struct { uintptr_t at; uint32_t word; } kServiceChecks[] = {
    {0x184548980, 0x903156d4}, {0x184548984, 0xb90aea80},  // the block: adrp x20, 0x1e7020000; str w0, [x20, #0xae8]
    {0x1845c92ec, 0xf03152a8}, {0x1845c92f0, 0xb94ae908},  // copy_surface: adrp x8, 0x1e7020000; ldr w8, [x8, #0xae8]
    {0x1847b2f24, 0xd0314368}, {0x1847b2f28, 0xb94ae908},  // clone_update: the same
};
static uintptr_t gSlide;
static uint32_t gRootEntry, gServiceShown;
static bool gGateReady;

static bool readWord(uintptr_t at, void *out, size_t size) {
    vm_size_t got = 0;
    return vm_read_overwrite(mach_task_self(), at, size, (vm_address_t)out, &got) == KERN_SUCCESS && got == size;
}
static bool writeService(uint32_t value) {
    if (value == gServiceShown) return true;
    if (vm_write(mach_task_self(), kServiceValue + gSlide, (vm_offset_t)&value, sizeof value) != KERN_SUCCESS) return false;
    gServiceShown = value;
    return true;
}
// Is any display a clone (screen recording's virtual display, AirPlay)? Asked on the notification's queue.
static unsigned clonesNow() {
    Class server = objc_getClass("CAWindowServer");
    if (!server) return 0;
    id s = ((id (*)(Class, SEL))objc_msgSend)(server, sel_registerName("serverIfRunning"));
    if (!s) return 0;
    id displays = ((id (*)(id, SEL))objc_msgSend)(s, sel_registerName("displays"));
    const unsigned long n = displays ? ((unsigned long (*)(id, SEL))objc_msgSend)(displays, sel_registerName("count")) : 0;
    unsigned clones = 0;
    for (unsigned long i = 0; i < n; i++) {
        id d = ((id (*)(id, SEL, unsigned long))objc_msgSend)(displays, sel_registerName("objectAtIndex:"), i);
        if (d && ((id (*)(id, SEL))objc_msgSend)(d, sel_registerName("cloneMaster"))) clones++;
    }
    return clones;
}
static void evaluateGate() {
    if (!gGateReady) return;
    const unsigned clones = clonesNow();
    const uint32_t want = clones ? gRootEntry : 0;
    if (want == gServiceShown) return;
    const bool ok = writeService(want);
    os_log(OS_LOG_DEFAULT, "OneiOS screenrec: scaler path %{public}s (%u clone display%{public}s)%{public}s", want ? "on" : "off",
           clones, clones == 1 ? "" : "s", ok ? "" : ": the write was refused");
}
// After the accelerator's entries are ours (a scaler path that reaches Apple's IOSurfaceAcceleratorCreate without a
// service aborts backboardd: create_new_iosa_manager calls abort() when Create fails).
static bool installGate(char *why, size_t whySize) {
    void *start = dlsym(RTLD_DEFAULT, "CARenderServerStart");
    if (!start) { snprintf(why, whySize, "no CARenderServerStart"); return false; }
    gSlide = (uintptr_t)ptrauth_strip(start, ptrauth_key_function_pointer) - kAnchor;
    for (const auto &c : kServiceChecks) {
        uint32_t w = 0;
        if (!readWord(c.at + gSlide, &w, sizeof w) || w != c.word) {
            snprintf(why, whySize, "QuartzCore is not 24A446's (0x%lx holds 0x%08x, not 0x%08x)", (unsigned long)c.at, w, c.word);
            return false;
        }
    }
    uint64_t token = 0;
    uint32_t value = 0;
    if (!readWord(kServiceToken + gSlide, &token, sizeof token) || !readWord(kServiceValue + gSlide, &value, sizeof value)) {
        snprintf(why, whySize, "get_io_service's value unreadable"); return false;
    }
    if (value) { snprintf(why, whySize, "get_io_service already found a scaler service (0x%x)", value); return false; }
    if (token != 0 && token != ~0ull) { snprintf(why, whySize, "get_io_service's block is running (token 0x%llx)", (unsigned long long)token); return false; }
    if (!token) {  // not asked yet: mark it done with no service, so that the block never overwrites the value later
        const uint64_t done = ~0ull;
        if (vm_write(mach_task_self(), kServiceToken + gSlide, (vm_offset_t)&done, sizeof done) != KERN_SUCCESS) {
            snprintf(why, whySize, "get_io_service's token not writable"); return false;
        }
    }
    void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY);
    auto rootEntry = (uint32_t (*)(uint32_t))(iokit ? dlsym(iokit, "IORegistryGetRootEntry") : nullptr);
    gRootEntry = rootEntry ? rootEntry(0 /* kIOMainPortDefault */) : 0;
    if (!gRootEntry) { snprintf(why, whySize, "no registry root entry"); return false; }
    auto notifyRegister = (uint32_t (*)(const char *, int *, dispatch_queue_t, void (^)(int)))dlsym(RTLD_DEFAULT, "notify_register_dispatch");
    if (!notifyRegister) { snprintf(why, whySize, "no notify_register_dispatch"); return false; }
    static dispatch_queue_t queue = dispatch_queue_create("oneios.screenrec.gate", DISPATCH_QUEUE_SERIAL);
    static int notifyToken;
    if (notifyRegister("com.apple.CoreAnimation.CAWindowServer.DisplayChanged", &notifyToken, queue, ^(int) { evaluateGate(); })) {
        snprintf(why, whySize, "notify_register_dispatch failed"); return false;
    }
    gGateReady = true;
    dispatch_async(queue, ^{ evaluateGate(); });
    return true;
}

static bool bootArg(const char *key) {
    char args[1100] = {};
    size_t size = sizeof args - 1;
    return sysctlbyname("kern.bootargs", args, &size, nullptr, 0) == 0 && strstr(args, key);
}

static bool scalerServicePresent() {
    void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY | RTLD_NOLOAD);
    if (!iokit) iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY);
    auto matching = (CFMutableDictionaryRef (*)(const char *))(iokit ? dlsym(iokit, "IOServiceMatching") : nullptr);
    auto getService = (unsigned (*)(unsigned, CFDictionaryRef))(iokit ? dlsym(iokit, "IOServiceGetMatchingService") : nullptr);
    auto release = (int (*)(unsigned))(iokit ? dlsym(iokit, "IOObjectRelease") : nullptr);
    if (!matching || !getService || !release) return false;
    for (const char *name : {"AppleM2ScalerParavirtDriver", "AppleM2ScalerCSCDriver"}) {
        CFMutableDictionaryRef m = matching(name);
        if (!m) continue;
        const unsigned service = getService(0 /* kIOMainPortDefault */, m);  // consumes m
        if (service) { release(service); return true; }
    }
    return false;
}

static void install() {
    char why[160] = "";
    // Opt-in until a phone boot shows that backboardd may run a patched text page (code signing could kill it on its
    // first call into IOSurfaceAccelerator, at the start of a recording): oneios-screenrec=1 turns it on.
    if (!bootArg("oneios-screenrec=1")) return;
    if (scalerServicePresent()) { say("not installed: a scaler service matches (the real IOSurfaceAccelerator works)"); return; }
    void *iosa = dlopen("/System/Library/PrivateFrameworks/IOSurfaceAccelerator.framework/IOSurfaceAccelerator", RTLD_LAZY);
    void *surf = dlopen("/System/Library/Frameworks/IOSurface.framework/IOSurface", RTLD_LAZY);
    if (!iosa || !surf) { say("not installed: IOSurfaceAccelerator or IOSurface did not load"); return; }
#define SYM(field, name) S.field = (decltype(S.field))dlsym(surf, name)
    SYM(pixelFormat, "IOSurfaceGetPixelFormat"); SYM(width, "IOSurfaceGetWidth"); SYM(height, "IOSurfaceGetHeight");
    SYM(planeCount, "IOSurfaceGetPlaneCount"); SYM(widthOfPlane, "IOSurfaceGetWidthOfPlane");
    SYM(heightOfPlane, "IOSurfaceGetHeightOfPlane"); SYM(bytesPerRowOfPlane, "IOSurfaceGetBytesPerRowOfPlane");
    SYM(bytesPerRow, "IOSurfaceGetBytesPerRow"); SYM(baseAddress, "IOSurfaceGetBaseAddress");
    SYM(baseAddressOfPlane, "IOSurfaceGetBaseAddressOfPlane"); SYM(lock, "IOSurfaceLock"); SYM(unlock, "IOSurfaceUnlock");
#undef SYM
    if (!S.pixelFormat || !S.width || !S.height || !S.planeCount || !S.widthOfPlane || !S.heightOfPlane ||
        !S.bytesPerRowOfPlane || !S.bytesPerRow || !S.baseAddress || !S.baseAddressOfPlane || !S.lock || !S.unlock) {
        say("not installed: an IOSurface function is missing"); return;
    }
    auto key = [&](const char *name) -> CFStringRef { void *p = dlsym(iosa, name); return p ? *(CFStringRef *)p : nullptr; };
    keySrcRef = key("kIOSurfaceAcceleratorSharedEventSrcRef"); keySrcWait = key("kIOSurfaceAcceleratorSharedEventSrcWait");
    keySrcSignal = key("kIOSurfaceAcceleratorSharedEventSrcSignal"); keyDstRef = key("kIOSurfaceAcceleratorSharedEventDstRef");
    keyDstWait = key("kIOSurfaceAcceleratorSharedEventDstWait"); keyDstSignal = key("kIOSurfaceAcceleratorSharedEventDstSignal");
    keySkipAsync = key("kIOSurfaceAcceleratorSkipAsyncCallback");
    mach_timebase_info(&gTimebase);
    struct Entry { const char *name; void *target; };
    const Entry entries[] = {
        {"IOSurfaceAcceleratorCreate", (void *)create},
        {"IOSurfaceAcceleratorGetRunLoopSource", (void *)getRunLoopSource},
        {"IOSurfaceAcceleratorTransformSurface", (void *)transformSurface},
        {"IOSurfaceAcceleratorTransferSurface", (void *)unsupported},
        {"IOSurfaceAcceleratorConditionalTransferSurfaceWithSwap", (void *)unsupported},
        {"IOSurfaceAcceleratorBlitSurface", (void *)unsupported},
        {"IOSurfaceAcceleratorCaptureSurface", (void *)unsupported},
        {"IOSurfaceAcceleratorAbortTransfers", (void *)unsupported},
        {"IOSurfaceAcceleratorAbortCaptures", (void *)unsupported},
        {"IOSurfaceAcceleratorSetProperty", (void *)unsupported},
        {"IOSurfaceAcceleratorSetCustomFilter", (void *)unsupported},
        {"IOSurfaceAcceleratorSetFilterCoefficients", (void *)unsupported},
        {"IOSurfaceAcceleratorGetHistogram", (void *)unsupported},
        {"IOSurfaceAcceleratorGetDiag", (void *)unsupported},
        {"IOSurfaceAcceleratorGetTransformEstimation", (void *)unsupported},
        {"IOSurfaceAcceleratorParavirtProcessGuestData", (void *)unsupported},
        {"IOSurfaceAcceleratorKernelTests", (void *)unsupported},
        {"IOSurfaceAcceleratorGetID", (void *)zero},
        {"IOSurfaceAcceleratorGetServiceObject", (void *)zero},
    };
    // All three that matter must be there before any entry is touched; the rest are patched where present.
    for (const char *must : {"IOSurfaceAcceleratorCreate", "IOSurfaceAcceleratorGetRunLoopSource", "IOSurfaceAcceleratorTransformSurface"})
        if (!dlsym(iosa, must)) { os_log(OS_LOG_DEFAULT, "OneiOS screenrec: not installed: no %{public}s", must); return; }
    unsigned done = 0;
    // The guards first, Create last: until Create is ours no object of ours exists, so a half-done install is harmless.
    for (size_t i = sizeof entries / sizeof *entries; i-- > 0;) {
        void *entry = dlsym(iosa, entries[i].name);
        if (!entry) continue;
        if (!redirect(entry, entries[i].target, entries[i].name, why, sizeof why)) {
            // Create is redirected last, so until then no object of ours exists and Apple's entries still run as before
            // for every caller; a refused guard entry is only a warning.
            if (i >= 3) { os_log(OS_LOG_DEFAULT, "OneiOS screenrec: %{public}s left as it is", why); continue; }
            os_log(OS_LOG_DEFAULT, "OneiOS screenrec: not installed: %{public}s (%u guard entries redirected; Create is not)",
                   why, done);
            return;
        }
        done++;
    }
    if (!installGate(why, sizeof why)) {
        // The entries stay ours (harmless: QuartzCore never reaches them without the gate, and no object of ours exists).
        os_log(OS_LOG_DEFAULT, "OneiOS screenrec: not installed: %{public}s (%u entries redirected, the gate is not)", why, done);
        return;
    }
    os_log(OS_LOG_DEFAULT, "OneiOS screenrec: CPU scaler installed (%u entries, slide 0x%lx)", done, (unsigned long)gSlide);
}

}  // namespace oneios_screenrec

__attribute__((constructor)) static void oneiosScreenRecord() {
    if (strcmp(getprogname(), "backboardd")) return;
    oneios_screenrec::install();
}
#endif  // ONEIOS_SCREENREC_CORE_ONLY

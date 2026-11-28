// SPDX-License-Identifier: MIT
// OneiOS Bench: where is the VM weak? A diagnostic app for the iOS 27 guest (Gunyah VM, Adreno through Metal on
// Vulkan through virtio-gpu). UIKit, Metal (MSL compiled at run time), Core Animation, ImageIO; nothing else.
//
// A live overlay on top of everything: FPS, a frame-time graph (the last 240 frames, lines at 8.3 and 16.7 ms),
// frame-time p50/p95/p99 and the frames dropped at 120 Hz, CPU time per frame (the app's own work in the frame:
// encode, layout), GPU time per command buffer (MTLCommandBuffer GPUStartTime..GPUEndTime; "n/a" where the driver
// gives none), submit-to-present latency, NSProcessInfo.thermalState and the memory footprint (phys_footprint).
//
// The tests run about 15 s each. Each result goes on screen and to iOS's unified log as ONE line:
//   oneios-bench: <test> <metrics...> | fps=.. p50=.. p95=.. p99=.. drop=.. cpu=..ms gpu=..ms thermal=N foot=MiB
// plus "oneios-bench: begin ..." and "oneios-bench: end ..." around a run, and "oneios-bench: mem-step ..." while the
// memory test allocates (it may be killed by jetsam before it can say more; it runs last).
// The lines are logged as public text (a "%@" argument is <private> in the stored log) and appended to a file too (the
// launch line names it). "FPS" is the app's display-link callbacks; "screen" is the frames that reached the display
// (the guest kernel's count, sysctl kern.oneios_frames, or the render server's counter): layers the render server
// cannot draw in time lower the second and leave the first at 120.
#import <UIKit/UIKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <ImageIO/ImageIO.h>
#import <simd/simd.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <os/proc.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define TEST_SECONDS 15.0
#define GRAPH_FRAMES 240
#define REFRESH_MS (1000.0 / 120.0)

// ---- small helpers ------------------------------------------------------------------------------------------------
static double now_s(void) { return CACurrentMediaTime(); }
static double now_ms(void) { return CACurrentMediaTime() * 1000.0; }
static double thread_cpu_ms(void) { return clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) / 1e6; }
static double footprint_mib(void) {
  task_vm_info_data_t info;
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) != KERN_SUCCESS) return -1;
  return info.phys_footprint / 1048576.0;
}
// os_proc_available_memory: a process with no memory limit gets a huge number; -1 here says "no limit"
static double avail_mib(void) {
  double a = os_proc_available_memory() / 1048576.0;
  return a > 1048576.0 ? -1 : a;
}
// One line: public in the unified log, and in the results file (the first folder that takes it).
static int g_logFd = -2;
static const char *g_logPath = "none";
// The results file, opened once (before the launch line, which names it).
static const char *blog_open(void) {
  if (g_logFd == -2) {
    static const char *paths[] = {"/private/var/oneios/log/bench.log", "/private/var/mobile/Library/Logs/oneios-bench.log",
                                  "/private/var/mobile/Documents/oneios-bench.log", "/private/var/tmp/oneios-bench.log"};
    g_logFd = -1;
    for (size_t i = 0; i < sizeof paths / sizeof paths[0] && g_logFd < 0; i++) {
      g_logFd = open(paths[i], O_WRONLY | O_CREAT | O_APPEND, 0644);
      if (g_logFd >= 0) g_logPath = paths[i];
    }
  }
  return g_logPath;
}
static void BLog(NSString *format, ...) NS_FORMAT_FUNCTION(1, 2);
static void BLog(NSString *format, ...) {
  va_list ap;
  va_start(ap, format);
  NSString *s = [[NSString alloc] initWithFormat:format arguments:ap];
  va_end(ap);
  os_log(OS_LOG_DEFAULT, "%{public}s", s.UTF8String);
  blog_open();
  if (g_logFd >= 0) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    NSData *d = [[NSString stringWithFormat:@"%ld.%03d %d %@\n", (long)tv.tv_sec, (int)(tv.tv_usec / 1000), getpid(), s]
        dataUsingEncoding:NSUTF8StringEncoding];
    if (write(g_logFd, d.bytes, d.length) < 0) { close(g_logFd); g_logFd = -1; g_logPath = "none (write failed)"; }
  }
}
// The frames that reached the screen (every display update, whoever asked for it). 0 = nobody tells. First the guest
// kernel's own count of the frames its display wrote (sysctl kern.oneios_frames, when OneiOS's kernel gives it), then
// the render server's counter where QuartzCore still has it (looked up at run time).
static uint32_t (*g_frameCounter)(mach_port_t);
static int g_screenSource = -1;  // 0 none, 1 the kernel's sysctl, 2 the render server's counter
static uint32_t screen_frames(void) {
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    uint64_t n = 0;
    size_t len = sizeof n;
    if (sysctlbyname("kern.oneios_frames", &n, &len, NULL, 0) == 0 && len == sizeof n) { g_screenSource = 1; return; }
    g_frameCounter = (uint32_t (*)(mach_port_t))dlsym(RTLD_DEFAULT, "CARenderServerGetFrameCounter");
    g_screenSource = g_frameCounter ? 2 : 0;
  });
  if (g_screenSource == 1) {
    uint64_t n = 0;
    size_t len = sizeof n;
    return sysctlbyname("kern.oneios_frames", &n, &len, NULL, 0) == 0 ? (uint32_t)n : 0;
  }
  return g_frameCounter ? g_frameCounter(MACH_PORT_NULL) : 0;
}
static const char *screen_source(void) {
  uint32_t n = screen_frames();
  return g_screenSource == 1 ? "kernel" : g_screenSource == 2 ? (n ? "render-server" : "render-server-zero") : "none";
}
static NSString *thermal_name(NSProcessInfoThermalState s) {
  switch (s) {
    case NSProcessInfoThermalStateNominal: return @"nominal";
    case NSProcessInfoThermalStateFair: return @"fair";
    case NSProcessInfoThermalStateSerious: return @"serious";
    case NSProcessInfoThermalStateCritical: return @"critical";
  }
  return @"?";
}
static int cmp_double(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}
static volatile uint64_t g_sink;
static NSArray<UIImage *> *g_listImages;  // the scroll test's row images

// A growable list of samples (ms), thread safe.
@interface Samples : NSObject
- (void)add:(double)v;
- (void)reset;
- (NSUInteger)count;
- (double)mean;
- (double)pct:(double)p;  // 0..100; NAN when empty
- (double)max;
- (NSUInteger)countAbove:(double)v;
@end
@implementation Samples {
  double *_v;
  NSUInteger _n, _cap;
}
- (void)dealloc { free(_v); }
- (void)add:(double)v {
  @synchronized(self) {
    if (_n == _cap) {
      _cap = _cap ? _cap * 2 : 1024;
      _v = realloc(_v, _cap * sizeof(double));
    }
    _v[_n++] = v;
  }
}
- (void)reset { @synchronized(self) { _n = 0; } }
- (NSUInteger)count { @synchronized(self) { return _n; } }
- (double)mean {
  @synchronized(self) {
    if (!_n) return NAN;
    double s = 0;
    for (NSUInteger i = 0; i < _n; i++) s += _v[i];
    return s / _n;
  }
}
- (double)pct:(double)p {
  @synchronized(self) {
    if (!_n) return NAN;
    double *c = malloc(_n * sizeof(double));
    memcpy(c, _v, _n * sizeof(double));
    qsort(c, _n, sizeof(double), cmp_double);
    NSUInteger i = (NSUInteger)(p / 100.0 * (_n - 1) + 0.5);
    double r = c[MIN(i, _n - 1)];
    free(c);
    return r;
  }
}
- (NSUInteger)countAbove:(double)v {
  @synchronized(self) {
    NSUInteger k = 0;
    for (NSUInteger i = 0; i < _n; i++)
      if (_v[i] > v) k++;
    return k;
  }
}
- (double)max {
  @synchronized(self) {
    double m = NAN;
    for (NSUInteger i = 0; i < _n; i++)
      if (isnan(m) || _v[i] > m) m = _v[i];
    return m;
  }
}
@end

static NSString *fmt_ms(double v) { return isnan(v) ? @"n/a" : [NSString stringWithFormat:@"%.2f", v]; }

// ---- Metal --------------------------------------------------------------------------------------------------------
typedef struct {
  simd_float4x4 mvp, model;
  simd_float4 color, params;
} Uni;  // 160 bytes, as the MSL struct

typedef struct {
  float p[3], n[3];
} Vtx;

static NSString *const kShaders = @"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct Uni { float4x4 mvp; float4x4 model; float4 color; float4 params; };\n"
"struct VIn { packed_float3 p; packed_float3 n; };\n"
"struct VOut { float4 pos [[position]]; float3 n; float2 uv; float4 col; float4 prm; };\n"
"vertex VOut v_lit(uint vid [[vertex_id]], uint iid [[instance_id]], const device VIn *vb [[buffer(0)]],\n"
"                  const device Uni *u [[buffer(1)]]) {\n"
"  Uni U = u[iid]; VOut o; float3 p = float3(vb[vid].p); float3 n = float3(vb[vid].n);\n"
"  o.pos = U.mvp * float4(p, 1.0); o.n = (U.model * float4(n, 0.0)).xyz; o.uv = float2(0.0); o.col = U.color; o.prm = U.params;\n"
"  return o; }\n"
"fragment float4 f_lit(VOut i [[stage_in]]) {\n"
"  float3 L = normalize(float3(0.4, 0.8, 0.5)); float3 N = normalize(i.n); float d = max(dot(N, L), 0.0);\n"
"  float3 h = normalize(L + float3(0.0, 0.0, 1.0)); float s = pow(max(dot(N, h), 0.0), 32.0);\n"
"  return float4(i.col.rgb * (0.15 + 0.85 * d) + s * 0.4, 1.0); }\n"
"vertex VOut v_quad(uint vid [[vertex_id]], constant Uni &u [[buffer(1)]]) {\n"
"  float2 c = float2((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0); VOut o;\n"
"  o.pos = float4(c, 0.0, 1.0); o.n = float3(0.0, 0.0, 1.0); o.uv = float2(c.x * 0.5 + 0.5, 0.5 - c.y * 0.5);\n"
"  o.col = u.color; o.prm = u.params; return o; }\n"
"fragment float4 f_flat(VOut i [[stage_in]]) { return i.col; }\n"
"fragment float4 f_alu(VOut i [[stage_in]]) {\n"
"  float a = i.uv.x * 8.0, b = i.uv.y * 8.0; int n = int(i.prm.x);\n"
"  for (int k = 0; k < n; k++) { a = fma(sin(a), 1.0001, cos(b)); b = fma(cos(a), 0.9999, sin(b) + 0.001); }\n"
"  return float4(fract(a), fract(b), fract(a * b), 1.0); }\n"
"fragment float4 f_tex(VOut i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
"  int taps = int(i.prm.x); float scale = i.prm.y; float4 acc = float4(0.0);\n"
"  for (int k = 0; k < taps; k++) { float2 o = float2(float(k) * 0.0137, float(k) * 0.0071); acc += t.sample(s, i.uv * scale + o); }\n"
"  return acc / float(max(taps, 1)); }\n"
"kernel void k_fma(device float4 *buf [[buffer(0)]], constant uint &iters [[buffer(1)]], uint gid [[thread_position_in_grid]]) {\n"
"  float4 a = buf[gid]; float4 b = float4(1.0001, 0.9999, 1.0002, 0.9998); float4 c = float4(0.0001);\n"
"  for (uint k = 0; k < iters; k++) { a = fma(a, b, c); b = fma(b, a, c); a = fma(a, b, c); b = fma(b, a, c); }\n"
"  buf[gid] = a + b; }\n";

static simd_float4x4 m_persp(float fovy, float aspect, float n, float f) {
  float ys = 1.0f / tanf(fovy * 0.5f), xs = ys / aspect, zs = f / (n - f);
  return simd_matrix(simd_make_float4(xs, 0, 0, 0), simd_make_float4(0, ys, 0, 0), simd_make_float4(0, 0, zs, -1),
                     simd_make_float4(0, 0, zs * n, 0));
}
static simd_float4x4 m_rot(float a, simd_float3 v) {
  v = simd_normalize(v);
  float c = cosf(a), s = sinf(a), t = 1 - c, x = v.x, y = v.y, z = v.z;
  return simd_matrix(simd_make_float4(t * x * x + c, t * x * y + z * s, t * x * z - y * s, 0),
                     simd_make_float4(t * x * y - z * s, t * y * y + c, t * y * z + x * s, 0),
                     simd_make_float4(t * x * z + y * s, t * y * z - x * s, t * z * z + c, 0), simd_make_float4(0, 0, 0, 1));
}
static simd_float4x4 m_trans(float x, float y, float z) {
  return simd_matrix(simd_make_float4(1, 0, 0, 0), simd_make_float4(0, 1, 0, 0), simd_make_float4(0, 0, 1, 0), simd_make_float4(x, y, z, 1));
}
static simd_float4x4 m_scale(float s) {
  return simd_matrix(simd_make_float4(s, 0, 0, 0), simd_make_float4(0, s, 0, 0), simd_make_float4(0, 0, s, 0), simd_make_float4(0, 0, 0, 1));
}

static id<MTLBuffer> make_cube(id<MTLDevice> dev) {
  Vtx v[36];
  int k = 0;
  for (int face = 0; face < 6; face++) {
    int axis = face / 2;
    float sign = face % 2 ? -1.0f : 1.0f;
    float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    int order[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; i++) {
      float a = corners[order[i]][0], b = corners[order[i]][1];
      float p[3];
      p[axis] = sign;
      p[(axis + 1) % 3] = a;
      p[(axis + 2) % 3] = b;
      for (int j = 0; j < 3; j++) { v[k].p[j] = p[j]; v[k].n[j] = j == axis ? sign : 0; }
      k++;
    }
  }
  return [dev newBufferWithBytes:v length:sizeof v options:MTLResourceStorageModeShared];
}

// ---- the overlay ----------------------------------------------------------------------------------------------------
@interface Overlay : UIView
@property (nonatomic, strong) UILabel *text;
@property (nonatomic, strong) CAShapeLayer *graph, *lines;
@end
@implementation Overlay
- (instancetype)initWithFrame:(CGRect)frame {
  if ((self = [super initWithFrame:frame])) {
    self.userInteractionEnabled = NO;
    self.backgroundColor = [UIColor colorWithWhite:0 alpha:0.62];
    self.layer.cornerRadius = 8;
    _text = [[UILabel alloc] init];
    _text.numberOfLines = 0;
    _text.font = [UIFont monospacedSystemFontOfSize:10.5 weight:UIFontWeightMedium];
    _text.textColor = UIColor.whiteColor;
    [self addSubview:_text];
    _lines = [CAShapeLayer layer];
    _lines.strokeColor = [UIColor colorWithRed:1 green:0.8 blue:0.2 alpha:0.6].CGColor;
    _lines.lineWidth = 0.5;
    _lines.fillColor = nil;
    [self.layer addSublayer:_lines];
    _graph = [CAShapeLayer layer];
    _graph.strokeColor = [UIColor colorWithRed:0.3 green:1 blue:0.5 alpha:1].CGColor;
    _graph.lineWidth = 1;
    _graph.fillColor = nil;
    [self.layer addSublayer:_graph];
  }
  return self;
}
- (CGRect)graphRect {
  return CGRectMake(6, self.bounds.size.height - 66, self.bounds.size.width - 12, 60);
}
- (void)layoutSubviews {
  [super layoutSubviews];
  _text.frame = CGRectMake(6, 4, self.bounds.size.width - 12, self.bounds.size.height - 74);
  CGRect g = [self graphRect];
  UIBezierPath *p = [UIBezierPath bezierPath];
  for (int i = 1; i <= 2; i++) {  // 8.3 ms and 16.7 ms at a 33.3 ms scale
    CGFloat y = CGRectGetMaxY(g) - g.size.height * (i * REFRESH_MS) / 33.3;
    [p moveToPoint:CGPointMake(g.origin.x, y)];
    [p addLineToPoint:CGPointMake(CGRectGetMaxX(g), y)];
  }
  _lines.path = p.CGPath;
}
- (void)plot:(const double *)ms count:(int)n head:(int)head {
  CGRect g = [self graphRect];
  UIBezierPath *p = [UIBezierPath bezierPath];
  for (int i = 0; i < n; i++) {
    double v = ms[(head + i) % GRAPH_FRAMES];
    CGFloat x = g.origin.x + g.size.width * i / (GRAPH_FRAMES - 1);
    CGFloat y = CGRectGetMaxY(g) - g.size.height * MIN(v, 33.3) / 33.3;
    if (i == 0) [p moveToPoint:CGPointMake(x, y)];
    else [p addLineToPoint:CGPointMake(x, y)];
  }
  _graph.path = p.CGPath;
}
@end

@interface MetalView : UIView
@end
@implementation MetalView
+ (Class)layerClass { return [CAMetalLayer class]; }
@end

// ---- a test -----------------------------------------------------------------------------------------------------------
typedef NS_ENUM(NSInteger, Cat) { CatCPU = 1, CatGPU, CatCA, CatDecode, CatMem };
@class Bench;
@interface BenchTest : NSObject
@property (nonatomic, copy) NSString *name;
@property (nonatomic) Cat cat;
@property (nonatomic) double seconds;
@property (nonatomic, copy) NSString *result;
// a test run on a background thread: returns its metrics
@property (nonatomic, copy) NSString *(^work)(Bench *b);
// a test driven by the display link on the main thread
@property (nonatomic, copy) void (^setup)(Bench *b, UIView *stage);
@property (nonatomic, copy) void (^frame)(Bench *b, double elapsed);
@property (nonatomic, copy) NSString *(^finish)(Bench *b);
@end
@implementation BenchTest
@end

static BenchTest *T(NSString *name, Cat cat) {
  BenchTest *t = [[BenchTest alloc] init];
  t.name = name;
  t.cat = cat;
  t.seconds = TEST_SECONDS;
  return t;
}

// ---- the bench: meter, runner, tests ---------------------------------------------------------------------------------
@interface Bench : UIViewController <UITableViewDataSource, UITableViewDelegate>
@property (nonatomic, readonly) BOOL stopRequested;
@end

@implementation Bench {
  CADisplayLink *_link;
  Overlay *_overlay;
  UIView *_stage;
  MetalView *_metalView;
  UITableView *_table;
  UISegmentedControl *_filter;
  UITextView *_summary;
  UIButton *_runButton, *_stopButton;
  NSArray<BenchTest *> *_tests;
  NSMutableArray<BenchTest *> *_queue;
  BenchTest *_current;
  double _testStart;
  BOOL _stop, _bgRunning;
  NSMutableString *_runLog;
  int _runNumber;

  // meter
  double _last;
  double _graphMs[GRAPH_FRAMES];
  int _graphHead, _graphCount;
  Samples *_frameMs, *_cpuMs, *_gpuMs, *_presentMs, *_doneMs, *_drawableWaitMs;
  Samples *_oFrame, *_oCpu, *_oGpu, *_oPresent;  // overlay: the last half second
  int _drops, _gpuNA;
  double _overlayAt;
  uint32_t _screenAtTest, _screenAtStep, _screenAtOverlay;  // the render server's frame counter at those moments
  double _overlayScreenAt;
  double _lastCpu;

  // metal
  id<MTLDevice> _dev;
  id<MTLCommandQueue> _queueMTL;
  id<MTLLibrary> _lib;
  NSString *_metalError;
  double _libMs;
  id<MTLRenderPipelineState> _pLit, _pBlend, _pAlu, _pTex, _pOff, _pFlat;
  id<MTLComputePipelineState> _pFma;
  id<MTLDepthStencilState> _depthOn;
  id<MTLTexture> _depthTex, _bigTex;
  id<MTLSamplerState> _sampler;
  id<MTLBuffer> _cube, _uni, _mesh, _meshIdx, _fmaBuf, _uploadShared, _uploadPrivate;
  NSUInteger _meshIndexCount;
  NSMutableArray<id<MTLTexture>> *_targets;
  dispatch_semaphore_t _inflight;
  uint64_t _frameNo;
  // per-test state
  int _step, _param;
  double _stepStart;
  NSMutableString *_stepLog;
  Samples *_aux;
  double _auxSum;
  void *_hostBuf;
  NSMutableArray *_views;
  NSMutableArray<NSValue *> *_chunks;
  BOOL _memWarning;
  double _memWarnAt, _memAvailAtWarn;
}

- (BOOL)stopRequested { return _stop; }

- (instancetype)init {
  if ((self = [super init])) {
    _frameMs = [Samples new]; _cpuMs = [Samples new]; _gpuMs = [Samples new]; _presentMs = [Samples new];
    _doneMs = [Samples new]; _drawableWaitMs = [Samples new]; _aux = [Samples new];
    _oFrame = [Samples new]; _oCpu = [Samples new]; _oGpu = [Samples new]; _oPresent = [Samples new];
    _inflight = dispatch_semaphore_create(3);
    _runLog = [NSMutableString string];
    _tests = [self makeTests];
  }
  return self;
}

// ---------------------------------------------------------------------------------------------------------------------
- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = UIColor.systemBackgroundColor;
  _filter = [[UISegmentedControl alloc] initWithItems:@[ @"All", @"CPU", @"GPU", @"CA", @"Decode", @"Mem" ]];
  _filter.selectedSegmentIndex = 0;
  [_filter addTarget:self action:@selector(filterChanged) forControlEvents:UIControlEventValueChanged];
  [self.view addSubview:_filter];
  _runButton = [UIButton buttonWithType:UIButtonTypeSystem];
  [_runButton setTitle:@"Run all" forState:UIControlStateNormal];
  _runButton.titleLabel.font = [UIFont boldSystemFontOfSize:18];
  [_runButton addTarget:self action:@selector(runFiltered) forControlEvents:UIControlEventTouchUpInside];
  [self.view addSubview:_runButton];
  _stopButton = [UIButton buttonWithType:UIButtonTypeSystem];
  [_stopButton setTitle:@"Stop" forState:UIControlStateNormal];
  [_stopButton addTarget:self action:@selector(stopRun) forControlEvents:UIControlEventTouchUpInside];
  [self.view addSubview:_stopButton];
  _table = [[UITableView alloc] initWithFrame:CGRectZero style:UITableViewStylePlain];
  _table.dataSource = self;
  _table.delegate = self;
  _table.rowHeight = 52;
  [self.view addSubview:_table];
  _summary = [[UITextView alloc] init];
  _summary.editable = NO;
  _summary.font = [UIFont monospacedSystemFontOfSize:10 weight:UIFontWeightRegular];
  _summary.backgroundColor = UIColor.secondarySystemBackgroundColor;
  _summary.text = @"Tap a test to run it, or Run all (the filter above picks CPU only, GPU only, ...).\nResults also go to the unified log as \"oneios-bench:\" lines.";
  [self.view addSubview:_summary];

  _stage = [[UIView alloc] init];
  _stage.backgroundColor = UIColor.blackColor;
  _stage.hidden = YES;
  [self.view addSubview:_stage];
  _overlay = [[Overlay alloc] init];
  [self.view addSubview:_overlay];

  [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(memoryWarning)
                                               name:UIApplicationDidReceiveMemoryWarningNotification object:nil];
  _link = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick:)];
  _link.preferredFrameRateRange = CAFrameRateRangeMake(80, 120, 120);
  [_link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];

  [self setupMetal];
  [self reportLastMemoryRun];
  char model[64] = "?";
  size_t len = sizeof model;
  sysctlbyname("hw.machine", model, &len, NULL, 0);
  BLog(@"oneios-bench: launch model=%s os=%@ cpus=%lu page=%lu ram=%.0fMiB metal=%@ lib=%.1fms foot=%.1fMiB avail=%.0fMiB screen-counter=%s file=%s",
        model, [UIDevice currentDevice].systemVersion, (unsigned long)[NSProcessInfo processInfo].activeProcessorCount,
        (unsigned long)vm_page_size, [NSProcessInfo processInfo].physicalMemory / 1048576.0,
        _dev ? _dev.name : (_metalError ?: @"none"), _libMs, footprint_mib(), avail_mib(),
        screen_source(), blog_open());
}

- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  CGRect s = UIEdgeInsetsInsetRect(self.view.bounds, self.view.safeAreaInsets);
  CGFloat w = s.size.width, x = s.origin.x, y = s.origin.y;
  _overlay.frame = CGRectMake(x + 6, y + 2, w - 12, 150);
  CGFloat top = y + 158;
  _filter.frame = CGRectMake(x + 8, top, w - 16, 32);
  _runButton.frame = CGRectMake(x + 8, top + 38, w * 0.6, 40);
  _stopButton.frame = CGRectMake(x + w * 0.65, top + 38, w * 0.33, 40);
  CGFloat rest = CGRectGetMaxY(s) - (top + 84);
  _table.frame = CGRectMake(x, top + 84, w, rest * 0.58);
  _summary.frame = CGRectMake(x + 4, CGRectGetMaxY(_table.frame) + 4, w - 8, rest * 0.42 - 8);
  _stage.frame = CGRectMake(x, top, w, CGRectGetMaxY(s) - top);
}

- (BOOL)prefersStatusBarHidden { return YES; }

// ---- the table ------------------------------------------------------------------------------------------------------
- (NSArray<BenchTest *> *)filtered {
  NSInteger f = _filter.selectedSegmentIndex;
  if (f <= 0) return _tests;
  NSMutableArray *a = [NSMutableArray array];
  for (BenchTest *t in _tests)
    if (t.cat == (Cat)f) [a addObject:t];
  return a;
}
- (void)filterChanged {
  NSInteger f = _filter.selectedSegmentIndex;
  NSArray *names = @[ @"Run all", @"CPU only", @"GPU only", @"CA only", @"Decode only", @"Memory" ];
  [_runButton setTitle:names[f] forState:UIControlStateNormal];
  [_table reloadData];
}
- (NSInteger)tableView:(UITableView *)tv numberOfRowsInSection:(NSInteger)section {
  return tv.tag == 77 ? 3000 : (NSInteger)[self filtered].count;
}
- (UITableViewCell *)tableView:(UITableView *)tv cellForRowAtIndexPath:(NSIndexPath *)ip {
  if (tv.tag == 77) return [self listCell:tv row:ip.row];
  UITableViewCell *c = [tv dequeueReusableCellWithIdentifier:@"t"];
  if (!c) c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"t"];
  BenchTest *t = [self filtered][ip.row];
  NSArray *cats = @[ @"", @"CPU", @"GPU", @"CA", @"Decode", @"Mem" ];
  c.textLabel.text = [NSString stringWithFormat:@"%@ · %@", cats[t.cat], t.name];
  c.detailTextLabel.text = t == _current ? @"running..." : (t.result ?: @"not run");
  c.detailTextLabel.numberOfLines = 2;
  c.detailTextLabel.font = [UIFont monospacedSystemFontOfSize:9.5 weight:UIFontWeightRegular];
  return c;
}
- (UITableViewCell *)listCell:(UITableView *)tv row:(NSInteger)row {
  UITableViewCell *c = [tv dequeueReusableCellWithIdentifier:@"l"];
  if (!c) {
    c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"l"];
    UILabel *badge = [[UILabel alloc] initWithFrame:CGRectMake(0, 0, 44, 24)];
    badge.textAlignment = NSTextAlignmentCenter;
    badge.backgroundColor = UIColor.systemRedColor;
    badge.textColor = UIColor.whiteColor;
    badge.layer.cornerRadius = 12;
    badge.layer.shadowOpacity = 0.4;
    badge.layer.shadowRadius = 3;
    badge.layer.shadowOffset = CGSizeMake(0, 2);
    c.accessoryView = badge;
  }
  c.imageView.image = g_listImages.count ? g_listImages[row % g_listImages.count] : nil;
  c.textLabel.text = [NSString stringWithFormat:@"Item %ld: a row with an image", (long)row];
  c.detailTextLabel.text = [NSString stringWithFormat:@"subtitle %ld, some more text to lay out", (long)(row * 7919 % 1000)];
  ((UILabel *)c.accessoryView).text = [NSString stringWithFormat:@"%ld", (long)(row % 99)];
  return c;
}
- (void)tableView:(UITableView *)tv didSelectRowAtIndexPath:(NSIndexPath *)ip {
  [tv deselectRowAtIndexPath:ip animated:YES];
  if (tv.tag == 77) return;
  if (_current || _queue.count) return;
  [self startRun:@[ [self filtered][ip.row] ] label:@"one"];
}

// ---- the runner -----------------------------------------------------------------------------------------------------
- (void)runFiltered {
  if (_current || _queue.count) return;
  NSArray *f = [self filtered];
  // the memory test last: jetsam may end the app
  NSMutableArray *order = [NSMutableArray array];
  for (BenchTest *t in f)
    if (t.cat != CatMem) [order addObject:t];
  for (BenchTest *t in f)
    if (t.cat == CatMem) [order addObject:t];
  [self startRun:order label:[_runButton titleForState:UIControlStateNormal]];
}
- (void)stopRun {
  _stop = YES;
  [_queue removeAllObjects];
}
- (void)startRun:(NSArray *)tests label:(NSString *)label {
  _stop = NO;
  _runNumber++;
  _queue = [tests mutableCopy];
  [_runLog setString:@""];
  _summary.text = @"";
  [UIApplication sharedApplication].idleTimerDisabled = YES;
  BLog(@"oneios-bench: begin run=%d set=\"%@\" tests=%lu thermal=%ld foot=%.1fMiB avail=%.0fMiB", _runNumber, label,
        (unsigned long)tests.count, (long)[NSProcessInfo processInfo].thermalState, footprint_mib(), avail_mib());
  [self next];
}
- (void)next {
  if (_stop || !_queue.count) {
    _current = nil;
    [UIApplication sharedApplication].idleTimerDisabled = NO;
    BLog(@"oneios-bench: end run=%d %@ thermal=%ld foot=%.1fMiB", _runNumber, _stop ? @"stopped" : @"done",
          (long)[NSProcessInfo processInfo].thermalState, footprint_mib());
    [_table reloadData];
    // the footprint at rest, after what the tests freed has gone back: the number to compare between runs (a leak grows it)
    int run = _runNumber;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
      if (!self->_current) BLog(@"oneios-bench: rest run=%d foot=%.1fMiB (5 s after its end)", run, footprint_mib());
    });
    return;
  }
  _current = _queue.firstObject;
  [_queue removeObjectAtIndex:0];
  [_table reloadData];
  [self resetWindow];
  _testStart = now_s();
  _screenAtTest = _screenAtStep = screen_frames();
  BenchTest *t = _current;
  if (t.work) {
    _bgRunning = YES;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      NSString *r = t.work(self);
      dispatch_async(dispatch_get_main_queue(), ^{
        self->_bgRunning = NO;
        [self done:t metrics:r];
      });
    });
  } else {
    _stage.hidden = NO;
    [self.view bringSubviewToFront:_stage];
    [self.view bringSubviewToFront:_overlay];
    _step = 0; _param = 0; _stepStart = now_s(); _stepLog = [NSMutableString string]; _auxSum = 0;
    [_aux reset];
    t.setup(self, _stage);
  }
}
- (void)resetWindow {
  [_frameMs reset]; [_cpuMs reset]; [_gpuMs reset]; [_presentMs reset]; [_doneMs reset]; [_drawableWaitMs reset];
  _drops = 0;
  _gpuNA = 0;
}
- (void)done:(BenchTest *)t metrics:(NSString *)metrics {
  double took = MAX(now_s() - _testStart, 0.001);
  uint32_t drawn = screen_frames();
  NSString *screen = drawn ? [NSString stringWithFormat:@"%.1f", (uint32_t)(drawn - _screenAtTest) / took] : @"n/a";
  NSString *frames = [NSString stringWithFormat:@"fps=%.1f screen=%@ p50=%@ p95=%@ p99=%@ drop=%d cpu=%@ms gpu=%@ms%@ thermal=%ld foot=%.0fMiB",
                      _frameMs.count / took, screen, fmt_ms([_frameMs pct:50]), fmt_ms([_frameMs pct:95]),
                      fmt_ms([_frameMs pct:99]), _drops, fmt_ms([_cpuMs mean]), fmt_ms([_gpuMs mean]),
                      _gpuNA && !_gpuMs.count ? @"(none)" : @"", (long)[NSProcessInfo processInfo].thermalState, footprint_mib()];
  NSString *slug = [[t.name lowercaseString] stringByReplacingOccurrencesOfString:@" " withString:@"-"];
  NSString *line = [NSString stringWithFormat:@"%@ %@ | %@", slug, metrics ?: @"", frames];
  BLog(@"oneios-bench: %@", line);
  t.result = [NSString stringWithFormat:@"%@\n%@", metrics ?: @"", frames];
  [_runLog appendFormat:@"%@\n", line];
  _summary.text = _runLog;
  [_summary scrollRangeToVisible:NSMakeRange(_runLog.length, 0)];
  [self next];
}
- (void)finishFrameTest {
  BenchTest *t = _current;
  NSString *r = t.finish ? t.finish(self) : @"";
  for (UIView *v in [_stage.subviews copy]) [v removeFromSuperview];
  for (CALayer *l in [_stage.layer.sublayers copy]) [l removeFromSuperlayer];
  [_views removeAllObjects];
  _stage.hidden = YES;
  _metalView = nil;
  [self done:t metrics:r];
}

// ---- the display link: meter, overlay, frame tests -------------------------------------------------------------------
- (void)tick:(CADisplayLink *)link {
  double t = link.timestamp;
  if (_last > 0) {
    double ms = (t - _last) * 1000.0;
    _graphMs[(_graphHead + _graphCount) % GRAPH_FRAMES] = ms;
    if (_graphCount < GRAPH_FRAMES) _graphCount++;
    else _graphHead = (_graphHead + 1) % GRAPH_FRAMES;
    [_oFrame add:ms];
    if (_current) {
      [_frameMs add:ms];
      int missed = (int)floor(ms / REFRESH_MS + 0.5) - 1;
      if (missed > 0) _drops += missed;
    }
  }
  _last = t;
  if (_current && !_current.work && _current.frame) {
    double c0 = now_ms();
    double elapsed = now_s() - _testStart;
    if (elapsed >= _current.seconds || _stop) {
      [self finishFrameTest];
    } else {
      _current.frame(self, elapsed);
      double c = now_ms() - c0;
      [_cpuMs add:c];
      [_oCpu add:c];
    }
  }
  if (t - _overlayAt >= 0.25) {
    _overlayAt = t;
    [self updateOverlay];
  }
}
- (void)updateOverlay {
  double fps = _oFrame.count ? 1000.0 / [_oFrame mean] : 0;
  uint32_t drawn = screen_frames();
  double nowS = now_s();
  NSString *screen = @"n/a";
  if (drawn && _overlayScreenAt > 0 && nowS > _overlayScreenAt)
    screen = [NSString stringWithFormat:@"%.0f", (uint32_t)(drawn - _screenAtOverlay) / (nowS - _overlayScreenAt)];
  _screenAtOverlay = drawn;
  _overlayScreenAt = nowS;
  NSString *test = _current ? [NSString stringWithFormat:@"%@ %.0f/%.0f s", _current.name, now_s() - _testStart, _current.seconds] : @"idle";
  _overlay.text.text = [NSString stringWithFormat:
      @"app FPS %.1f  SCREEN %@   frame p50 %@ p95 %@ p99 %@ max %@ ms\n"
      @"test window: p50 %@ p95 %@ p99 %@ ms, dropped@120Hz %d\n"
      @"CPU/frame %@ ms   GPU/cb %@ ms%@   present %@ ms\n"
      @"thermal %@   footprint %.1f MiB   avail %.0f MiB\n"
      @"%@",
      fps, screen, fmt_ms([_oFrame pct:50]), fmt_ms([_oFrame pct:95]), fmt_ms([_oFrame pct:99]), fmt_ms([_oFrame max]),
      fmt_ms([_frameMs pct:50]), fmt_ms([_frameMs pct:95]), fmt_ms([_frameMs pct:99]), _drops,
      fmt_ms([_oCpu mean]), fmt_ms([_oGpu mean]), _gpuNA && !_oGpu.count ? @"(none)" : @"", fmt_ms([_oPresent mean]),
      thermal_name([NSProcessInfo processInfo].thermalState), footprint_mib(), avail_mib(), test];
  [_overlay plot:_graphMs count:_graphCount head:_graphHead];
  [_oFrame reset]; [_oCpu reset]; [_oGpu reset]; [_oPresent reset];
}

// ---- Metal setup ----------------------------------------------------------------------------------------------------
- (id<MTLRenderPipelineState>)pipeline:(NSString *)vs fs:(NSString *)fs format:(MTLPixelFormat)format depth:(BOOL)depth blend:(BOOL)blend
                                   lib:(id<MTLLibrary>)lib {
  MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
  d.vertexFunction = [lib newFunctionWithName:vs];
  d.fragmentFunction = [lib newFunctionWithName:fs];
  d.colorAttachments[0].pixelFormat = format;
  if (blend) {
    d.colorAttachments[0].blendingEnabled = YES;
    d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
  }
  if (depth) d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
  NSError *e = nil;
  id<MTLRenderPipelineState> p = [_dev newRenderPipelineStateWithDescriptor:d error:&e];
  if (!p) BLog(@"oneios-bench: pipeline %@/%@ failed: %@", vs, fs, e.localizedDescription);
  return p;
}
- (void)setupMetal {
  _dev = MTLCreateSystemDefaultDevice();
  if (!_dev) { _metalError = @"MTLCreateSystemDefaultDevice=nil"; return; }
  _queueMTL = [_dev newCommandQueue];
  NSError *e = nil;
  double t0 = now_ms();
  _lib = [_dev newLibraryWithSource:kShaders options:nil error:&e];
  _libMs = now_ms() - t0;
  if (!_lib) {
    _metalError = [NSString stringWithFormat:@"newLibraryWithSource: %@", e.localizedDescription];
    BLog(@"oneios-bench: %@", _metalError);
    _dev = nil;
    return;
  }
  MTLPixelFormat bgra = MTLPixelFormatBGRA8Unorm;
  _pLit = [self pipeline:@"v_lit" fs:@"f_lit" format:bgra depth:YES blend:NO lib:_lib];
  _pBlend = [self pipeline:@"v_quad" fs:@"f_flat" format:bgra depth:NO blend:YES lib:_lib];
  _pFlat = [self pipeline:@"v_quad" fs:@"f_flat" format:bgra depth:NO blend:NO lib:_lib];
  _pAlu = [self pipeline:@"v_quad" fs:@"f_alu" format:bgra depth:NO blend:NO lib:_lib];
  _pTex = [self pipeline:@"v_quad" fs:@"f_tex" format:bgra depth:NO blend:NO lib:_lib];
  _pOff = [self pipeline:@"v_quad" fs:@"f_flat" format:MTLPixelFormatRGBA8Unorm depth:NO blend:NO lib:_lib];
  _pFma = [_dev newComputePipelineStateWithFunction:[_lib newFunctionWithName:@"k_fma"] error:&e];
  MTLDepthStencilDescriptor *ds = [MTLDepthStencilDescriptor new];
  ds.depthCompareFunction = MTLCompareFunctionLess;
  ds.depthWriteEnabled = YES;
  _depthOn = [_dev newDepthStencilStateWithDescriptor:ds];
  MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
  sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
  sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeRepeat;
  _sampler = [_dev newSamplerStateWithDescriptor:sd];
  _cube = make_cube(_dev);
  _uni = [_dev newBufferWithLength:3 * 10000 * sizeof(Uni) options:MTLResourceStorageModeShared];
}

- (void)metalStage:(UIView *)stage {
  _metalView = [[MetalView alloc] initWithFrame:stage.bounds];
  _metalView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  [stage addSubview:_metalView];
  CAMetalLayer *l = (CAMetalLayer *)_metalView.layer;
  l.device = _dev;
  l.pixelFormat = MTLPixelFormatBGRA8Unorm;
  l.framebufferOnly = YES;
  l.maximumDrawableCount = 3;
  CGFloat scale = stage.traitCollection.displayScale > 0 ? stage.traitCollection.displayScale : 3.0;
  l.contentsScale = scale;
  l.drawableSize = CGSizeMake(stage.bounds.size.width * scale, stage.bounds.size.height * scale);
  if (!_depthTex || _depthTex.width != (NSUInteger)l.drawableSize.width || _depthTex.height != (NSUInteger)l.drawableSize.height) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                  width:(NSUInteger)l.drawableSize.width
                                                                                 height:(NSUInteger)l.drawableSize.height mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModePrivate;
    _depthTex = [_dev newTextureWithDescriptor:td];
  }
}
- (CGSize)drawableSize { return ((CAMetalLayer *)_metalView.layer).drawableSize; }

// One frame on the GPU: drawable, command buffer, the test's encoding, present; the measurements on the way.
- (void)gpuFrame:(BOOL)depth encode:(void (^)(id<MTLCommandBuffer> cb, MTLRenderPassDescriptor *rp))encode {
  if (!_metalView) return;
  if (dispatch_semaphore_wait(_inflight, dispatch_time(DISPATCH_TIME_NOW, 200 * NSEC_PER_MSEC))) return;
  CAMetalLayer *l = (CAMetalLayer *)_metalView.layer;
  double w0 = now_ms();
  id<CAMetalDrawable> d = [l nextDrawable];
  [_drawableWaitMs add:now_ms() - w0];
  if (!d) { dispatch_semaphore_signal(_inflight); return; }
  id<MTLCommandBuffer> cb = [_queueMTL commandBuffer];
  MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
  rp.colorAttachments[0].texture = d.texture;
  rp.colorAttachments[0].loadAction = MTLLoadActionClear;
  rp.colorAttachments[0].storeAction = MTLStoreActionStore;
  rp.colorAttachments[0].clearColor = MTLClearColorMake(0.05, 0.06, 0.09, 1);
  if (depth) {
    rp.depthAttachment.texture = _depthTex;
    rp.depthAttachment.loadAction = MTLLoadActionClear;
    rp.depthAttachment.storeAction = MTLStoreActionDontCare;
    rp.depthAttachment.clearDepth = 1.0;
  }
  encode(cb, rp);
  double submit = now_s();
  __weak Bench *weakSelf = self;
  dispatch_semaphore_t sem = _inflight;
  Samples *gpu = _gpuMs, *ogpu = _oGpu, *doneS = _doneMs, *pres = _presentMs, *opres = _oPresent;
  [d addPresentedHandler:^(id<MTLDrawable> drawable) {
    CFTimeInterval p = drawable.presentedTime;
    if (p > 0) { [pres add:(p - submit) * 1000.0]; [opres add:(p - submit) * 1000.0]; }
  }];
  [cb presentDrawable:d];
  [cb addCompletedHandler:^(id<MTLCommandBuffer> b) {
    [doneS add:(now_s() - submit) * 1000.0];
    double g = (b.GPUEndTime - b.GPUStartTime) * 1000.0;
    if (b.GPUStartTime > 0 && g > 0) { [gpu add:g]; [ogpu add:g]; }
    else { Bench *s = weakSelf; if (s) s->_gpuNA++; }
    if (b.error) BLog(@"oneios-bench: command buffer error: %@", b.error.localizedDescription);
    dispatch_semaphore_signal(sem);
  }];
  [cb commit];
  _frameNo++;
}
- (Uni *)uniSlot {
  return (Uni *)_uni.contents + (_frameNo % 3) * 10000;
}
- (NSUInteger)uniOffset {
  return (_frameNo % 3) * 10000 * sizeof(Uni);
}
- (NSString *)gpuCommon {
  return [NSString stringWithFormat:@"gpu_p95=%@ submit2done_p50=%@ present_p50=%@ present_p95=%@ drawable_wait_p95=%@",
          fmt_ms([_gpuMs pct:95]), fmt_ms([_doneMs pct:50]), fmt_ms([_presentMs pct:50]), fmt_ms([_presentMs pct:95]),
          fmt_ms([_drawableWaitMs pct:95])];
}
// A step of a stepped test: every `each` seconds the next parameter; the figures of the step just ended go in _stepLog.
- (BOOL)nextStepAt:(double)elapsed each:(double)each {
  int step = (int)(elapsed / each);
  if (step == _step) return NO;
  _step = step;
  return YES;
}

static BenchTest *GPU(NSString *name) { return T(name, CatGPU); }

// ---- the GPU tests ----------------------------------------------------------------------------------------------------
- (void)addGPUTests:(NSMutableArray *)a {
  BenchTest *t;

  // 1. one spinning lit cube with depth
  t = GPU(@"Lit cube");
  t.setup = ^(Bench *b, UIView *stage) { [b metalStage:stage]; };
  t.frame = ^(Bench *b, double el) { [b drawCubes:1 elapsed:el]; };
  t.finish = ^NSString *(Bench *b) { return [b gpuCommon]; };
  [a addObject:t];

  // 2. draw-call scaling: 100 .. 10000 cubes, one draw each
  t = GPU(@"Draw calls");
  t.setup = ^(Bench *b, UIView *stage) { [b metalStage:stage]; b->_param = 0; [b->_aux reset]; b->_stepStart = now_s(); };
  t.frame = ^(Bench *b, double el) {
    static const int counts[5] = {100, 300, 1000, 3000, 10000};
    int step = MIN((int)(el / 3.0), 4);
    if (step != b->_param) {
      [b->_stepLog appendFormat:@"n%d=%.1ffps/%.2fms ", counts[b->_param], b->_aux.count / (now_s() - b->_stepStart), [b->_aux mean]];
      [b->_aux reset];
      b->_param = step;
      b->_stepStart = now_s();
    }
    double c0 = now_ms();
    [b drawCubes:counts[step] elapsed:el];
    [b->_aux add:now_ms() - c0];
  };
  t.finish = ^NSString *(Bench *b) {
    [b->_stepLog appendFormat:@"n10000=%.1ffps/%.2fms", b->_aux.count / (now_s() - b->_stepStart), [b->_aux mean]];
    return [NSString stringWithFormat:@"%@ (fps/encode) %@", b->_stepLog, [b gpuCommon]];
  };
  [a addObject:t];

  // 3. fill rate / overdraw: K blended full-screen quads
  t = GPU(@"Fill rate");
  t.setup = ^(Bench *b, UIView *stage) { [b metalStage:stage]; b->_param = 0; b->_stepStart = now_s(); [b->_aux reset]; };
  t.frame = ^(Bench *b, double el) {
    static const int layers[4] = {4, 8, 16, 32};
    int step = MIN((int)(el / 3.75), 3);
    CGSize sz = [b drawableSize];
    if (step != b->_param) {
      double fps = b->_aux.count / (now_s() - b->_stepStart);
      [b->_stepLog appendFormat:@"x%d=%.1ffps/%.2fGpix/s ", layers[b->_param], fps, fps * layers[b->_param] * sz.width * sz.height / 1e9];
      [b->_aux reset];
      b->_param = step;
      b->_stepStart = now_s();
    }
    [b->_aux add:1];
    int k = layers[step];
    [b gpuFrame:NO encode:^(id<MTLCommandBuffer> cb, MTLRenderPassDescriptor *rp) {
      id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
      [e setRenderPipelineState:b->_pBlend];
      Uni *u = [b uniSlot];
      for (int i = 0; i < k; i++) {
        u[i].color = simd_make_float4(0.5f + 0.5f * sinf(el + i), 0.5f + 0.5f * cosf(el * 1.3f + i), 0.6f, 0.08f);
        [e setVertexBuffer:b->_uni offset:[b uniOffset] + i * sizeof(Uni) atIndex:1];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
      }
      [e endEncoding];
    }];
  };
  t.finish = ^NSString *(Bench *b) {
    CGSize sz = [b drawableSize];
    double fps = b->_aux.count / (now_s() - b->_stepStart);
    [b->_stepLog appendFormat:@"x32=%.1ffps/%.2fGpix/s", fps, fps * 32 * sz.width * sz.height / 1e9];
    return [NSString stringWithFormat:@"%.0fx%.0f %@ %@", sz.width, sz.height, b->_stepLog, [b gpuCommon]];
  };
  [a addObject:t];

  // 4. ALU-heavy fragment shader
  t = GPU(@"Fragment ALU");
  t.setup = ^(Bench *b, UIView *stage) { [b metalStage:stage]; b->_param = 0; b->_stepStart = now_s(); [b->_aux reset]; };
  t.frame = ^(Bench *b, double el) {
    static const int iters[3] = {16, 64, 256};
    int step = MIN((int)(el / 5.0), 2);
    if (step != b->_param) {
      [b->_stepLog appendFormat:@"it%d=%.1ffps/gpu%@ ", iters[b->_param], b->_aux.count / (now_s() - b->_stepStart), fmt_ms([b->_gpuMs pct:50])];
      [b->_aux reset];
      [b->_gpuMs reset];
      b->_param = step;
      b->_stepStart = now_s();
    }
    [b->_aux add:1];
    int n = iters[step];
    [b gpuFrame:NO encode:^(id<MTLCommandBuffer> cb, MTLRenderPassDescriptor *rp) {
      id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
      [e setRenderPipelineState:b->_pAlu];
      Uni *u = [b uniSlot];
      u[0].params = simd_make_float4(n, 0, 0, 0);
      [e setVertexBuffer:b->_uni offset:[b uniOffset] atIndex:1];
      [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
      [e endEncoding];
    }];
  };
  t.finish = ^NSString *(Bench *b) {
    [b->_stepLog appendFormat:@"it256=%.1ffps/gpu%@", b->_aux.count / (now_s() - b->_stepStart), fmt_ms([b->_gpuMs pct:50])];
    return [NSString stringWithFormat:@"%@ (sin+cos+fma x2 per iteration per pixel) %@", b->_stepLog, [b gpuCommon]];
  };
  [a addObject:t];

  // 5. texture bandwidth: a 4096x4096 RGBA8 texture (64 MiB), many taps per pixel
  t = GPU(@"Texture bandwidth");
  t.setup = ^(Bench *b, UIView *stage) {
    [b metalStage:stage];
    double t0 = now_ms();
    if (!b->_bigTex) {
      MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4096 height:4096 mipmapped:NO];
      td.usage = MTLTextureUsageShaderRead;
      b->_bigTex = [b->_dev newTextureWithDescriptor:td];
      uint32_t *rows = malloc(4096 * 4 * 64);
      for (int y0 = 0; y0 < 4096; y0 += 64) {
        for (int y = 0; y < 64; y++)
          for (int x = 0; x < 4096; x++) rows[y * 4096 + x] = (uint32_t)((x ^ (y0 + y)) * 2654435761u) | 0xff000000u;
        [b->_bigTex replaceRegion:MTLRegionMake2D(0, y0, 4096, 64) mipmapLevel:0 withBytes:rows bytesPerRow:4096 * 4];
      }
      free(rows);
    }
    b->_auxSum = now_ms() - t0;
    b->_param = 0; b->_stepStart = now_s(); [b->_aux reset];
  };
  t.frame = ^(Bench *b, double el) {
    static const int taps[3] = {1, 4, 16};
    int step = MIN((int)(el / 5.0), 2);
    CGSize sz = [b drawableSize];
    if (step != b->_param) {
      double fps = b->_aux.count / (now_s() - b->_stepStart);
      [b->_stepLog appendFormat:@"taps%d=%.1ffps/%.2fGtex/s ", taps[b->_param], fps, fps * taps[b->_param] * sz.width * sz.height / 1e9];
      [b->_aux reset];
      b->_param = step;
      b->_stepStart = now_s();
    }
    [b->_aux add:1];
    int n = taps[step];
    [b gpuFrame:NO encode:^(id<MTLCommandBuffer> cb, MTLRenderPassDescriptor *rp) {
      id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
      [e setRenderPipelineState:b->_pTex];
      Uni *u = [b uniSlot];
      u[0].params = simd_make_float4(n, 2.0f, 0, 0);  // scale 2: the whole texture is crossed twice per screen
      [e setVertexBuffer:b->_uni offset:[b uniOffset] atIndex:1];
      [e setFragmentTexture:b->_bigTex atIndex:0];
      [e setFragmentSamplerState:b->_sampler atIndex:0];
      [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
      [e endEncoding];
    }];
  };
  t.finish = ^NSString *(Bench *b) {
    CGSize sz = [b drawableSize];
    double fps = b->_aux.count / (now_s() - b->_stepStart);
    [b->_stepLog appendFormat:@"taps16=%.1ffps/%.2fGtex/s", fps, fps * 16 * sz.width * sz.height / 1e9];
    return [NSString stringWithFormat:@"upload64MiB=%.0fms %@ %@", b->_auxSum, b->_stepLog, [b gpuCommon]];
  };
  [a addObject:t];

  // 6. vertex-heavy mesh: a 400x400 sphere (160k vertices, 318k triangles), 1..16 instances
  t = GPU(@"Vertex mesh");
  t.setup = ^(Bench *b, UIView *stage) { [b metalStage:stage]; [b makeMesh]; b->_param = 0; b->_stepStart = now_s(); [b->_aux reset]; };
  t.frame = ^(Bench *b, double el) {
    static const int inst[4] = {1, 4, 8, 16};
    int step = MIN((int)(el / 3.75), 3);
    if (step != b->_param) {
      double fps = b->_aux.count / (now_s() - b->_stepStart);
      [b->_stepLog appendFormat:@"x%d=%.1ffps/%.0fMtri/s ", inst[b->_param], fps, fps * inst[b->_param] * b->_meshIndexCount / 3 / 1e6];
      [b->_aux reset];
      b->_param = step;
      b->_stepStart = now_s();
    }
    [b->_aux add:1];
    int k = inst[step];
    CGSize sz = [b drawableSize];
    simd_float4x4 proj = m_persp(1.0f, sz.width / sz.height, 0.1f, 100.0f);
    Uni *u = [b uniSlot];
    for (int i = 0; i < k; i++) {
      float x = (i % 4) - 1.5f, y = (i / 4) - 1.5f;
      simd_float4x4 model = simd_mul(m_trans(x * 1.1f, y * 1.1f, -6.0f), simd_mul(m_rot(el + i, simd_make_float3(0.3f, 1, 0.1f)), m_scale(0.5f)));
      u[i].model = model;
      u[i].mvp = simd_mul(proj, model);
      u[i].color = simd_make_float4(0.4f + 0.1f * i, 0.7f, 0.9f - 0.05f * i, 1);
    }
    [b gpuFrame:YES encode:^(id<MTLCommandBuffer> cb, MTLRenderPassDescriptor *rp) {
      id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
      [e setRenderPipelineState:b->_pLit];
      [e setDepthStencilState:b->_depthOn];
      [e setVertexBuffer:b->_mesh offset:0 atIndex:0];
      [e setVertexBuffer:b->_uni offset:[b uniOffset] atIndex:1];
      [e drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:b->_meshIndexCount indexType:MTLIndexTypeUInt32
                   indexBuffer:b->_meshIdx indexBufferOffset:0 instanceCount:k];
      [e endEncoding];
    }];
  };
  t.finish = ^NSString *(Bench *b) {
    double fps = b->_aux.count / (now_s() - b->_stepStart);
    [b->_stepLog appendFormat:@"x16=%.1ffps/%.0fMtri/s", fps, fps * 16 * b->_meshIndexCount / 3 / 1e6];
    return [NSString stringWithFormat:@"%@ %@", b->_stepLog, [b gpuCommon]];
  };
  [a addObject:t];

  // 7. render-target switches: P passes into 64 small targets per frame, then one composite
  t = GPU(@"Render passes");
  t.setup = ^(Bench *b, UIView *stage) {
    [b metalStage:stage];
    if (!b->_targets) {
      b->_targets = [NSMutableArray array];
      for (int i = 0; i < 64; i++) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:256 height:256 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        [b->_targets addObject:[b->_dev newTextureWithDescriptor:td]];
      }
    }
    b->_param = 0; b->_stepStart = now_s(); [b->_aux reset];
  };
  t.frame = ^(Bench *b, double el) {
    static const int passes[3] = {8, 32, 128};
    int step = MIN((int)(el / 5.0), 2);
    if (step != b->_param) {
      double fps = b->_aux.count / (now_s() - b->_stepStart);
      [b->_stepLog appendFormat:@"p%d=%.1ffps/%.0fpasses/s/cpu%.2fms ", passes[b->_param], fps, fps * passes[b->_param], [b->_cpuMs mean]];
      [b->_aux reset];
      [b->_cpuMs reset];
      b->_param = step;
      b->_stepStart = now_s();
    }
    [b->_aux add:1];
    int n = passes[step];
    [b gpuFrame:NO encode:^(id<MTLCommandBuffer> cb, MTLRenderPassDescriptor *rp) {
      Uni *u = [b uniSlot];
      for (int i = 0; i < n; i++) {
        MTLRenderPassDescriptor *op = [MTLRenderPassDescriptor renderPassDescriptor];
        op.colorAttachments[0].texture = b->_targets[i % 64];
        op.colorAttachments[0].loadAction = MTLLoadActionClear;
        op.colorAttachments[0].storeAction = MTLStoreActionStore;
        op.colorAttachments[0].clearColor = MTLClearColorMake(i / (double)n, 0.2, 0.4, 1);
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:op];
        [e setRenderPipelineState:b->_pOff];
        u[i % 1000].color = simd_make_float4(fmodf(el + i * 0.1f, 1.0f), 0.5f, 0.3f, 1);
        [e setVertexBuffer:b->_uni offset:[b uniOffset] + (i % 1000) * sizeof(Uni) atIndex:1];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [e endEncoding];
      }
      id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
      [e setRenderPipelineState:b->_pTex];
      u[1000].params = simd_make_float4(1, 1, 0, 0);
      [e setVertexBuffer:b->_uni offset:[b uniOffset] + 1000 * sizeof(Uni) atIndex:1];
      [e setFragmentTexture:b->_targets[(n - 1) % 64] atIndex:0];
      [e setFragmentSamplerState:b->_sampler atIndex:0];
      [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
      [e endEncoding];
    }];
  };
  t.finish = ^NSString *(Bench *b) {
    double fps = b->_aux.count / (now_s() - b->_stepStart);
    [b->_stepLog appendFormat:@"p128=%.1ffps/%.0fpasses/s", fps, fps * 128];
    return [NSString stringWithFormat:@"%@ %@", b->_stepLog, [b gpuCommon]];
  };
  [a addObject:t];

  // 8. compute throughput: 1M float4 threads, an FMA loop
  t = GPU(@"Compute");
  t.setup = ^(Bench *b, UIView *stage) {
    [b metalStage:stage];
    if (!b->_fmaBuf) b->_fmaBuf = [b->_dev newBufferWithLength:(1 << 20) * 16 options:MTLResourceStorageModePrivate];
    [b->_aux reset];
  };
  t.frame = ^(Bench *b, double el) {
    [b gpuFrame:NO encode:^(id<MTLCommandBuffer> cb, MTLRenderPassDescriptor *rp) {
      id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
      [c setComputePipelineState:b->_pFma];
      [c setBuffer:b->_fmaBuf offset:0 atIndex:0];
      uint32_t iters = 64;
      [c setBytes:&iters length:4 atIndex:1];
      NSUInteger tg = MIN((NSUInteger)256, b->_pFma.maxTotalThreadsPerThreadgroup);
      for (int k = 0; k < 4; k++) [c dispatchThreads:MTLSizeMake(1 << 20, 1, 1) threadsPerThreadgroup:MTLSizeMake(tg, 1, 1)];
      [c endEncoding];
      id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
      [e endEncoding];
    }];
  };
  t.finish = ^NSString *(Bench *b) {
    // 4 dispatches x 1M threads x 64 iterations x 4 float4 FMAs (8 flops each)
    double flops = 4.0 * (1 << 20) * 64 * 4 * 8;
    double g = [b->_gpuMs pct:50], w = [b->_doneMs pct:50];
    return [NSString stringWithFormat:@"gflops_gputime=%@ gflops_wall=%.1f (%.1f GFLOP per frame) %@",
            isnan(g) ? @"n/a" : [NSString stringWithFormat:@"%.1f", flops / (g / 1000.0) / 1e9], flops / (w / 1000.0) / 1e9, flops / 1e9, [b gpuCommon]];
  };
  [a addObject:t];

  // 9. buffer upload: memcpy into a shared buffer, then a blit into a private one, every frame
  t = GPU(@"Buffer upload");
  t.setup = ^(Bench *b, UIView *stage) {
    [b metalStage:stage];
    b->_uploadShared = [b->_dev newBufferWithLength:64 << 20 options:MTLResourceStorageModeShared];
    b->_uploadPrivate = [b->_dev newBufferWithLength:64 << 20 options:MTLResourceStorageModePrivate];
    b->_hostBuf = malloc(64 << 20);
    memset(b->_hostBuf, 0x5a, 64 << 20);
    b->_param = 0; b->_stepStart = now_s(); [b->_aux reset];
  };
  t.frame = ^(Bench *b, double el) {
    static const int mib[4] = {1, 4, 16, 64};
    int step = MIN((int)(el / 3.75), 3);
    if (step != b->_param) {
      [b->_stepLog appendFormat:@"%dMiB=%.1ffps/memcpy%.2fGB/s/blit%@ms ", mib[b->_param], b->_aux.count / (now_s() - b->_stepStart),
                                 mib[b->_param] * 1.048576 / [b->_aux mean], fmt_ms([b->_gpuMs pct:50])];
      [b->_aux reset];
      [b->_gpuMs reset];
      b->_param = step;
      b->_stepStart = now_s();
    }
    size_t bytes = (size_t)mib[step] << 20;
    double c0 = now_ms();
    memcpy(b->_uploadShared.contents, b->_hostBuf, bytes);
    [b->_aux add:now_ms() - c0];
    [b gpuFrame:NO encode:^(id<MTLCommandBuffer> cb, MTLRenderPassDescriptor *rp) {
      id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
      [bl copyFromBuffer:b->_uploadShared sourceOffset:0 toBuffer:b->_uploadPrivate destinationOffset:0 size:bytes];
      [bl endEncoding];
      id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
      [e endEncoding];
    }];
  };
  t.finish = ^NSString *(Bench *b) {
    [b->_stepLog appendFormat:@"64MiB=%.1ffps/memcpy%.2fGB/s/blit%@ms", b->_aux.count / (now_s() - b->_stepStart), 64 * 1.048576 / [b->_aux mean],
                               fmt_ms([b->_gpuMs pct:50])];
    free(b->_hostBuf);
    b->_hostBuf = NULL;
    b->_uploadShared = nil;
    b->_uploadPrivate = nil;
    return [NSString stringWithFormat:@"%@ %@", b->_stepLog, [b gpuCommon]];
  };
  [a addObject:t];

  // 10. pipeline compile time: a new library and pipeline every 8th frame, compiled on the main thread (a game's stutter)
  t = GPU(@"Pipeline compile");
  t.setup = ^(Bench *b, UIView *stage) { [b metalStage:stage]; [b->_aux reset]; b->_param = 0; };
  t.frame = ^(Bench *b, double el) {
    id<MTLRenderPipelineState> p = b->_pFlat;
    if (b->_frameNo % 8 == 0) {
      double c0 = now_ms();
      NSString *src = [NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\n"
                       "struct VOut { float4 pos [[position]]; float2 uv; };\n"
                       "vertex VOut vq(uint vid [[vertex_id]]) { float2 c = float2((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0);"
                       " VOut o; o.pos = float4(c, 0.0, 1.0); o.uv = c; return o; }\n"
                       "fragment float4 fq(VOut i [[stage_in]]) { float k = %u.0 + %f; float a = i.uv.x * k;"
                       " for (int j = 0; j < %d; j++) a = sin(a) * 1.01 + cos(i.uv.y * float(j));"
                       " return float4(fract(a), fract(k * 0.001), 0.5, 1.0); }\n",
                       arc4random(), el, 4 + (int)(b->_frameNo % 13)];
      NSError *e = nil;
      id<MTLLibrary> lib = [b->_dev newLibraryWithSource:src options:nil error:&e];
      double c1 = now_ms();
      MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
      d.vertexFunction = [lib newFunctionWithName:@"vq"];
      d.fragmentFunction = [lib newFunctionWithName:@"fq"];
      d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
      id<MTLRenderPipelineState> np = lib ? [b->_dev newRenderPipelineStateWithDescriptor:d error:&e] : nil;
      double c2 = now_ms();
      if (np) {
        p = np;
        [b->_aux add:c2 - c0];
        b->_auxSum += c1 - c0;
        b->_param++;
      } else {
        BLog(@"oneios-bench: pipeline compile failed: %@", e.localizedDescription);
      }
    }
    Uni *u = [b uniSlot];
    u[0].color = simd_make_float4(0.2f, 0.4f, 0.8f, 1);
    [b gpuFrame:NO encode:^(id<MTLCommandBuffer> cb, MTLRenderPassDescriptor *rp) {
      id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
      [e setRenderPipelineState:p];
      [e setVertexBuffer:b->_uni offset:[b uniOffset] atIndex:1];
      [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
      [e endEncoding];
    }];
  };
  t.finish = ^NSString *(Bench *b) {
    unsigned long hitches = (unsigned long)[b->_frameMs countAbove:25.0];
    return [NSString stringWithFormat:@"hitches>25ms=%lu compiles=%d compile+pipeline_p50=%@ p95=%@ max=%@ ms library_mean=%.2fms startup_library=%.1fms %@",
            hitches, b->_param, fmt_ms([b->_aux pct:50]), fmt_ms([b->_aux pct:95]), fmt_ms([b->_aux max]), b->_param ? b->_auxSum / b->_param : NAN,
            b->_libMs, [b gpuCommon]];
  };
  [a addObject:t];

  // 11. present latency: a light frame, the time from commit to on screen (drawable presentedTime)
  t = GPU(@"Present latency");
  t.setup = ^(Bench *b, UIView *stage) { [b metalStage:stage]; };
  t.frame = ^(Bench *b, double el) { [b drawCubes:1 elapsed:el]; };
  t.finish = ^NSString *(Bench *b) {
    return [NSString stringWithFormat:@"submit2present_p50=%@ p95=%@ p99=%@ max=%@ presented=%lu submit2done_p50=%@ p95=%@ "
            @"drawable_wait_p50=%@ p95=%@ (presentedTime 0 = not reported by this stack: n/a)",
            fmt_ms([b->_presentMs pct:50]), fmt_ms([b->_presentMs pct:95]), fmt_ms([b->_presentMs pct:99]), fmt_ms([b->_presentMs max]),
            (unsigned long)b->_presentMs.count, fmt_ms([b->_doneMs pct:50]), fmt_ms([b->_doneMs pct:95]),
            fmt_ms([b->_drawableWaitMs pct:50]), fmt_ms([b->_drawableWaitMs pct:95])];
  };
  [a addObject:t];
}

- (void)drawCubes:(int)n elapsed:(double)el {
  CGSize sz = [self drawableSize];
  simd_float4x4 proj = m_persp(1.0f, sz.width / sz.height, 0.1f, 200.0f);
  Uni *u = [self uniSlot];
  int side = (int)ceil(sqrt((double)n));
  float spread = n == 1 ? 0 : 2.6f;
  for (int i = 0; i < n; i++) {
    float x = n == 1 ? 0 : ((i % side) - side / 2.0f) * spread, y = n == 1 ? 0 : ((i / side) - side / 2.0f) * spread;
    float z = n == 1 ? -5.0f : -(side * spread * 0.9f) - 4.0f;
    simd_float4x4 model = simd_mul(m_trans(x, y, z), m_rot((float)el * 1.2f + i * 0.1f, simd_make_float3(0.5f, 1, 0.2f)));
    u[i].model = model;
    u[i].mvp = simd_mul(proj, model);
    u[i].color = simd_make_float4(0.3f + 0.7f * (i % 7) / 7.0f, 0.5f, 1.0f - 0.6f * (i % 5) / 5.0f, 1);
  }
  NSUInteger base = [self uniOffset];
  [self gpuFrame:YES encode:^(id<MTLCommandBuffer> cb, MTLRenderPassDescriptor *rp) {
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:self->_pLit];
    [e setDepthStencilState:self->_depthOn];
    [e setVertexBuffer:self->_cube offset:0 atIndex:0];
    [e setVertexBuffer:self->_uni offset:base atIndex:1];
    for (int i = 0; i < n; i++) {
      [e setVertexBufferOffset:base + i * sizeof(Uni) atIndex:1];
      [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:36];
    }
    [e endEncoding];
  }];
}

- (void)makeMesh {
  if (_mesh) return;
  const int R = 400, S = 400;
  NSUInteger nv = (NSUInteger)(R + 1) * (S + 1);
  Vtx *v = malloc(nv * sizeof(Vtx));
  for (int r = 0; r <= R; r++)
    for (int s = 0; s <= S; s++) {
      float th = (float)M_PI * r / R, ph = 2 * (float)M_PI * s / S;
      float x = sinf(th) * cosf(ph), y = cosf(th), z = sinf(th) * sinf(ph);
      float bump = 1.0f + 0.05f * sinf(ph * 12) * sinf(th * 12);
      Vtx *q = &v[r * (S + 1) + s];
      q->p[0] = x * bump; q->p[1] = y * bump; q->p[2] = z * bump;
      q->n[0] = x; q->n[1] = y; q->n[2] = z;
    }
  _meshIndexCount = (NSUInteger)R * S * 6;
  uint32_t *idx = malloc(_meshIndexCount * 4);
  NSUInteger k = 0;
  for (int r = 0; r < R; r++)
    for (int s = 0; s < S; s++) {
      uint32_t a = r * (S + 1) + s, b = a + S + 1;
      idx[k++] = a; idx[k++] = b; idx[k++] = a + 1;
      idx[k++] = a + 1; idx[k++] = b; idx[k++] = b + 1;
    }
  _mesh = [_dev newBufferWithBytes:v length:nv * sizeof(Vtx) options:MTLResourceStorageModeShared];
  _meshIdx = [_dev newBufferWithBytes:idx length:_meshIndexCount * 4 options:MTLResourceStorageModeShared];
  free(v);
  free(idx);
}

// ---- the CPU tests (a background thread; the overlay keeps drawing) ----------------------------------------------------
static uint64_t int_work(uint64_t n, uint64_t x) {
  uint64_t a = 0;
  for (uint64_t i = 0; i < n; i++) {
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    a += x * 0x9E3779B97F4A7C15ull;
  }
  return a;
}
static double float_work(uint64_t n) {
  double a0 = 1, a1 = 1.1, a2 = 1.2, a3 = 1.3, a4 = 1.4, a5 = 1.5, a6 = 1.6, a7 = 1.7;
  const double m = 0.9999999, c = 1e-7;
  for (uint64_t i = 0; i < n; i++) {
    a0 = a0 * m + c; a1 = a1 * m + c; a2 = a2 * m + c; a3 = a3 * m + c;
    a4 = a4 * m + c; a5 = a5 * m + c; a6 = a6 * m + c; a7 = a7 * m + c;
  }
  return a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7;
}

typedef struct {
  atomic_int *stop;
  uint64_t iters;
  int index;
} Worker;
static void *worker_main(void *arg) {
  Worker *w = arg;
  uint64_t x = 88172645463325252ull + w->index, sink = 0;
  while (!atomic_load_explicit(w->stop, memory_order_relaxed)) {
    sink += int_work(1 << 20, x++);
    w->iters += 1 << 20;
  }
  g_sink += sink;
  return NULL;
}

typedef struct {
  dispatch_semaphore_t ping, pong;
  atomic_int stop;
  double *wake;  // one-way: the time the waiter woke after an idle wait
  int rounds;
  _Atomic double signaled;
} PingPong;
static void *pong_main(void *arg) {
  PingPong *pp = arg;
  for (;;) {
    dispatch_semaphore_wait(pp->ping, DISPATCH_TIME_FOREVER);
    if (atomic_load(&pp->stop)) break;
    dispatch_semaphore_signal(pp->pong);
  }
  return NULL;
}
typedef struct {
  dispatch_semaphore_t go;
  _Atomic double sent;
  double *lat;
  int n, max;
  atomic_int stop;
} IdleWake;
static void *idle_main(void *arg) {
  IdleWake *iw = arg;
  for (;;) {
    dispatch_semaphore_wait(iw->go, DISPATCH_TIME_FOREVER);
    double t = now_ms();
    if (atomic_load(&iw->stop)) break;
    if (iw->n < iw->max) iw->lat[iw->n++] = (t - atomic_load(&iw->sent)) * 1000.0;  // us
  }
  return NULL;
}
static double pct_of(double *v, int n, double p) {
  if (n <= 0) return NAN;
  qsort(v, n, sizeof(double), cmp_double);
  int i = (int)(p / 100.0 * (n - 1) + 0.5);
  return v[MIN(i, n - 1)];
}

- (void)addCPUTests:(NSMutableArray *)a {
  BenchTest *t;

  t = T(@"Single-thread int+float", CatCPU);
  t.work = ^NSString *(Bench *b) {
    uint64_t n = 0, x = 1;
    double t0 = now_s(), c0 = thread_cpu_ms();
    while (now_s() - t0 < 7 && !b.stopRequested) { g_sink += int_work(20000000, x++); n += 20000000; }
    double it = n / (now_s() - t0) / 1e6, cpuShare = (thread_cpu_ms() - c0) / ((now_s() - t0) * 1000.0);
    uint64_t m = 0;
    double f = 0;
    t0 = now_s();
    while (now_s() - t0 < 7 && !b.stopRequested) { f += float_work(10000000); m += 10000000; }
    g_sink += (uint64_t)f;
    double ft = m * 16.0 / (now_s() - t0) / 1e9;  // 8 mul + 8 add per iteration
    return [NSString stringWithFormat:@"int=%.0fMiter/s (xorshift+mul, ~7 ops) float=%.2fGFLOPS (8 dep. chains) cpu_share=%.2f", it, ft, cpuShare];
  };
  [a addObject:t];

  t = T(@"Multi-thread scaling", CatCPU);
  t.work = ^NSString *(Bench *b) {
    NSMutableString *s = [NSMutableString string];
    double one = 0;
    for (int k = 1; k <= 8 && !b.stopRequested; k++) {
      atomic_int stop = 0;
      Worker w[8];
      pthread_t th[8];
      for (int i = 0; i < k; i++) { w[i] = (Worker){&stop, 0, i}; pthread_create(&th[i], NULL, worker_main, &w[i]); }
      double t0 = now_s();
      usleep(1700000);
      atomic_store(&stop, 1);
      uint64_t sum = 0;
      for (int i = 0; i < k; i++) { pthread_join(th[i], NULL); sum += w[i].iters; }
      double rate = sum / (now_s() - t0) / 1e6;
      if (k == 1) one = rate;
      [s appendFormat:@"t%d=%.0f(x%.2f) ", k, rate, one > 0 ? rate / one : 0];
    }
    return [NSString stringWithFormat:@"Miter/s %@cpus=%lu", s, (unsigned long)[NSProcessInfo processInfo].activeProcessorCount];
  };
  [a addObject:t];

  t = T(@"Memory bandwidth", CatCPU);
  t.work = ^NSString *(Bench *b) {
    const size_t sz = 64 << 20;
    uint8_t *src = malloc(sz), *dst = malloc(sz);
    memset(src, 1, sz);
    memset(dst, 2, sz);
    double t0 = now_s();
    size_t moved = 0;
    while (now_s() - t0 < 5 && !b.stopRequested) { memcpy(dst, src, sz); moved += sz; }
    double cp = moved / (now_s() - t0) / 1e9;
    t0 = now_s(); moved = 0;
    while (now_s() - t0 < 4 && !b.stopRequested) { memset(dst, (int)moved, sz); moved += sz; }
    double st = moved / (now_s() - t0) / 1e9;
    t0 = now_s(); moved = 0;
    uint64_t acc = 0;
    while (now_s() - t0 < 4 && !b.stopRequested) {
      const uint64_t *p = (const uint64_t *)src;
      for (size_t i = 0; i < sz / 8; i += 4) acc += p[i] + p[i + 1] + p[i + 2] + p[i + 3];
      moved += sz;
    }
    g_sink += acc;
    double rd = moved / (now_s() - t0) / 1e9;
    free(src);
    free(dst);
    return [NSString stringWithFormat:@"memcpy=%.2fGB/s (counted once) memset=%.2fGB/s read=%.2fGB/s buffer=64MiB", cp, st, rd];
  };
  [a addObject:t];

  t = T(@"Memory latency", CatCPU);
  t.work = ^NSString *(Bench *b) {
    static const size_t sizes[7] = {16 << 10, 128 << 10, 1 << 20, 4 << 20, 16 << 20, 64 << 20, 128 << 20};
    NSMutableString *s = [NSMutableString string];
    for (int k = 0; k < 7 && !b.stopRequested; k++) {
      size_t n = sizes[k] / 64;  // one pointer per cache line
      void **buf = malloc(sizes[k]);
      uint32_t *perm = malloc(n * sizeof(uint32_t));
      for (size_t i = 0; i < n; i++) perm[i] = (uint32_t)i;
      for (size_t i = n - 1; i > 0; i--) { size_t j = arc4random_uniform((uint32_t)i); uint32_t x = perm[i]; perm[i] = perm[j]; perm[j] = x; }
      for (size_t i = 0; i < n; i++) buf[perm[i] * 8] = &buf[perm[(i + 1) % n] * 8];
      free(perm);
      void **p = buf;
      uint64_t loads = 0;
      double t0 = now_s();
      while (now_s() - t0 < 1.8) {
        for (int i = 0; i < 100000; i++) p = (void **)*p;
        loads += 100000;
      }
      double ns = (now_s() - t0) * 1e9 / loads;
      g_sink += (uintptr_t)p;
      free(buf);
      [s appendFormat:@"%zuK=%.1fns ", sizes[k] >> 10, ns];
    }
    return [NSString stringWithFormat:@"pointer chase %@", s];
  };
  [a addObject:t];

  t = T(@"Malloc churn", CatCPU);
  t.work = ^NSString *(Bench *b) {
    enum { SLOTS = 4096 };
    void **slot = calloc(SLOTS, sizeof(void *));
    NSMutableString *s = [NSMutableString string];
    static const size_t lo[3] = {16, 4096, 1 << 20}, hi[3] = {1024, 65536, 1 << 20};
    static const char *names[3] = {"small", "medium", "1MiB"};
    for (int k = 0; k < 3 && !b.stopRequested; k++) {
      uint64_t ops = 0;
      double t0 = now_s();
      while (now_s() - t0 < 4.5) {
        for (int i = 0; i < 1000; i++) {
          uint32_t r = arc4random();
          int j = r % SLOTS;
          free(slot[j]);
          size_t z = lo[k] + (hi[k] > lo[k] ? (r >> 12) % (hi[k] - lo[k]) : 0);
          slot[j] = malloc(z);
          ((volatile char *)slot[j])[0] = 1;
        }
        ops += 1000;
      }
      for (int j = 0; j < SLOTS; j++) { free(slot[j]); slot[j] = NULL; }
      [s appendFormat:@"%s=%.2fMops/s(%.0fns) ", names[k], ops / (now_s() - t0) / 1e6, (now_s() - t0) * 1e9 / ops];
    }
    free(slot);
    return [NSString stringWithFormat:@"malloc+free %@", s];
  };
  [a addObject:t];

  t = T(@"Page faults", CatCPU);
  t.work = ^NSString *(Bench *b) {
    const size_t sz = 64 << 20;
    const size_t page = vm_page_size;
    Samples *first = [Samples new], *again = [Samples new], *k16 = [Samples new];
    double t0 = now_s(), zfMs = 0;
    int rounds = 0;
    while (now_s() - t0 < 12 && !b.stopRequested) {
      volatile uint8_t *m = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
      if (m == MAP_FAILED) break;
      double a0 = now_ms();
      for (size_t i = 0; i < sz; i += page) m[i] = 1;
      double a1 = now_ms();
      for (size_t i = 0; i < sz; i += page) m[i] = 2;
      double a2 = now_ms();
      munmap((void *)m, sz);
      [first add:(a1 - a0) * 1e6 / (sz / page)];
      [again add:(a2 - a1) * 1e6 / (sz / page)];
      zfMs += a1 - a0;
      // the same with a 16 KiB stride: one touch per 16 KiB (iOS's native page)
      m = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
      if (m == MAP_FAILED) break;
      a0 = now_ms();
      for (size_t i = 0; i < sz; i += 16384) m[i] = 1;
      a1 = now_ms();
      munmap((void *)m, sz);
      [k16 add:(a1 - a0) * 1e6 / (sz / 16384)];
      rounds++;
    }
    return [NSString stringWithFormat:@"page=%zu first_touch=%.0fns/page (%.0fMB/s) retouch=%.1fns/page stride16K=%.0fns/touch rounds=%d x64MiB",
            page, [first pct:50], rounds ? rounds * sz / 1e6 / (zfMs / 1000.0) : 0.0, [again pct:50], [k16 pct:50], rounds];
  };
  [a addObject:t];

  t = T(@"Syscalls", CatCPU);
  t.work = ^NSString *(Bench *b) {
    NSMutableString *s = [NSMutableString string];
    for (int k = 0; k < 6 && !b.stopRequested; k++) {
      uint64_t n = 0;
      double t0 = now_s();
      while (now_s() - t0 < 2.4) {
        for (int i = 0; i < 10000; i++) {
          switch (k) {
            case 0: g_sink += getppid(); break;
            case 1: g_sink += mach_absolute_time(); break;
            case 2: { struct timeval tv; gettimeofday(&tv, NULL); g_sink += tv.tv_usec; break; }
            case 3: { mach_port_t p = mach_thread_self(); mach_port_deallocate(mach_task_self(), p); break; }
            case 4: sched_yield(); break;
            case 5: { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); g_sink += ts.tv_nsec; break; }
          }
        }
        n += 10000;
      }
      static const char *names[6] = {"getppid", "mach_absolute_time", "gettimeofday", "mach_thread_self", "sched_yield", "clock_gettime"};
      [s appendFormat:@"%s=%.1fns ", names[k], (now_s() - t0) * 1e9 / n];
    }
    return s;
  };
  [a addObject:t];

  t = T(@"Thread wake", CatCPU);
  t.work = ^NSString *(Bench *b) {
    // 1. ping-pong: a round trip through two semaphores, both threads busy waking each other
    PingPong pp = {dispatch_semaphore_create(0), dispatch_semaphore_create(0), 0, NULL, 0, 0};
    pthread_t th;
    pthread_create(&th, NULL, pong_main, &pp);
    int cap = 400000, n = 0;
    double *rt = malloc(cap * sizeof(double));
    double t0 = now_s();
    while (now_s() - t0 < 5 && n < cap && !b.stopRequested) {
      double a0 = now_ms();
      dispatch_semaphore_signal(pp.ping);
      dispatch_semaphore_wait(pp.pong, DISPATCH_TIME_FOREVER);
      rt[n++] = (now_ms() - a0) * 1000.0;
    }
    atomic_store(&pp.stop, 1);
    dispatch_semaphore_signal(pp.ping);
    pthread_join(th, NULL);
    double rt50 = pct_of(rt, n, 50), rt99 = pct_of(rt, n, 99), rtmax = n ? rt[n - 1] : NAN;
    int rounds = n;
    // 2. idle wake: the waiter has slept 2 ms (its vCPU may have halted) when it is signalled; one-way latency
    IdleWake iw = {dispatch_semaphore_create(0), 0, rt, 0, cap, 0};
    pthread_create(&th, NULL, idle_main, &iw);
    t0 = now_s();
    while (now_s() - t0 < 5 && !b.stopRequested) {
      usleep(2000);
      atomic_store(&iw.sent, now_ms());
      dispatch_semaphore_signal(iw.go);
    }
    usleep(5000);
    atomic_store(&iw.stop, 1);
    dispatch_semaphore_signal(iw.go);
    pthread_join(th, NULL);
    double iw50 = pct_of(rt, iw.n, 50), iw99 = pct_of(rt, iw.n, 99), iwmax = iw.n ? rt[iw.n - 1] : NAN;
    // 3. timer wake: how late usleep(1000) returns
    int m = 0;
    t0 = now_s();
    while (now_s() - t0 < 4 && m < cap && !b.stopRequested) {
      double a0 = now_ms();
      usleep(1000);
      rt[m++] = (now_ms() - a0 - 1.0) * 1000.0;
    }
    double tl50 = pct_of(rt, m, 50), tl99 = pct_of(rt, m, 99);
    free(rt);
    return [NSString stringWithFormat:@"pingpong_rt p50=%.1fus p99=%.1fus max=%.0fus n=%d | idle_wake p50=%.1fus p99=%.1fus max=%.0fus n=%d | "
            @"usleep1ms_late p50=%.0fus p99=%.0fus", rt50, rt99, rtmax, rounds, iw50, iw99, iwmax, iw.n, tl50, tl99];
  };
  [a addObject:t];
}

// ---- Core Animation / UIKit tests ------------------------------------------------------------------------------------
- (void)addMovingBackground:(UIView *)stage count:(int)n {
  for (int i = 0; i < n; i++) {
    UIView *v = [[UIView alloc] initWithFrame:CGRectMake(arc4random_uniform((uint32_t)stage.bounds.size.width),
                                                         arc4random_uniform((uint32_t)stage.bounds.size.height), 70, 70)];
    v.backgroundColor = [UIColor colorWithHue:(i % 12) / 12.0 saturation:0.8 brightness:0.95 alpha:1];
    v.layer.cornerRadius = 14;
    [stage addSubview:v];
    CABasicAnimation *r = [CABasicAnimation animationWithKeyPath:@"transform.rotation.z"];
    r.fromValue = @0;
    r.toValue = @(2 * M_PI);
    r.duration = 2 + (i % 5);
    r.repeatCount = HUGE_VALF;
    [v.layer addAnimation:r forKey:@"spin"];
  }
}

// "/screenN" for the step that ends now: the render server's frames a second since the step before (call before the
// step's start time is set again).
- (NSString *)screenStep {
  uint32_t drawn = screen_frames();
  NSString *r = drawn ? [NSString stringWithFormat:@"/screen%.1f", (uint32_t)(drawn - _screenAtStep) / MAX(now_s() - _stepStart, 0.001)] : @"";
  _screenAtStep = drawn;
  return r;
}

- (void)addCATests:(NSMutableArray *)a {
  BenchTest *t;

  t = T(@"Blur layers", CatCA);
  t.setup = ^(Bench *b, UIView *stage) {
    [b addMovingBackground:stage count:40];
    b->_views = [NSMutableArray array];
    b->_param = -1;
    b->_stepStart = now_s();
  };
  t.frame = ^(Bench *b, double el) {
    static const int counts[3] = {4, 12, 24};
    int step = MIN((int)(el / 5.0), 2);
    if (step != b->_param) {
      if (b->_param >= 0)
        [b->_stepLog appendFormat:@"n%d=%.1ffps%@ ", counts[b->_param], b->_aux.count / (now_s() - b->_stepStart), [b screenStep]];
      [b->_aux reset];
      b->_param = step;
      b->_stepStart = now_s();
      while ((int)b->_views.count < counts[step]) {
        UIVisualEffectView *ev = [[UIVisualEffectView alloc] initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemMaterial]];
        ev.frame = CGRectMake(0, 0, 150, 110);
        ev.layer.cornerRadius = 18;
        ev.clipsToBounds = YES;
        UILabel *l = [[UILabel alloc] initWithFrame:CGRectInset(ev.bounds, 10, 10)];
        l.text = [NSString stringWithFormat:@"blur %lu", (unsigned long)b->_views.count];
        [ev.contentView addSubview:l];
        [b->_stage addSubview:ev];
        [b->_views addObject:ev];
      }
    }
    [b->_aux add:1];
    CGSize sz = b->_stage.bounds.size;
    for (NSUInteger i = 0; i < b->_views.count; i++) {
      UIView *v = b->_views[i];
      v.center = CGPointMake(sz.width * (0.5 + 0.4 * sin(el * 0.9 + i)), sz.height * (0.5 + 0.4 * cos(el * 0.7 + i * 1.3)));
    }
  };
  t.finish = ^NSString *(Bench *b) {
    [b->_stepLog appendFormat:@"n24=%.1ffps%@", b->_aux.count / (now_s() - b->_stepStart), [b screenStep]];
    return [NSString stringWithFormat:@"UIVisualEffectView systemMaterial moving over 40 spinning views: %@", b->_stepLog];
  };
  [a addObject:t];

  t = T(@"Shadows", CatCA);
  t.setup = ^(Bench *b, UIView *stage) {
    b->_views = [NSMutableArray array];
    b->_param = -1;
    b->_stepStart = now_s();
    stage.backgroundColor = UIColor.whiteColor;
  };
  t.frame = ^(Bench *b, double el) {
    static const int counts[3] = {50, 150, 300};
    int step = MIN((int)(el / 5.0), 2);
    if (step != b->_param) {
      if (b->_param >= 0) [b->_stepLog appendFormat:@"n%d=%.1ffps%@ ", counts[b->_param], b->_aux.count / (now_s() - b->_stepStart), [b screenStep]];
      [b->_aux reset];
      b->_param = step;
      b->_stepStart = now_s();
      while ((int)b->_views.count < counts[step]) {
        UIView *v = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 54, 54)];
        v.backgroundColor = [UIColor colorWithHue:(b->_views.count % 10) / 10.0 saturation:0.5 brightness:1 alpha:1];
        v.layer.cornerRadius = 12;
        v.layer.shadowColor = UIColor.blackColor.CGColor;
        v.layer.shadowOpacity = 0.5;
        v.layer.shadowRadius = 8;
        v.layer.shadowOffset = CGSizeMake(0, 4);  // no shadowPath: the shape is found offscreen each frame
        [b->_stage addSubview:v];
        [b->_views addObject:v];
      }
    }
    [b->_aux add:1];
    CGSize sz = b->_stage.bounds.size;
    for (NSUInteger i = 0; i < b->_views.count; i++) {
      UIView *v = b->_views[i];
      v.center = CGPointMake(sz.width * (0.5 + 0.45 * sin(el * 0.8 + i * 0.37)), sz.height * (0.5 + 0.45 * cos(el * 0.6 + i * 0.21)));
    }
  };
  t.finish = ^NSString *(Bench *b) {
    [b->_stepLog appendFormat:@"n300=%.1ffps%@", b->_aux.count / (now_s() - b->_stepStart), [b screenStep]];
    b->_stage.backgroundColor = UIColor.blackColor;
    return [NSString stringWithFormat:@"moving views with shadows, no shadowPath: %@", b->_stepLog];
  };
  [a addObject:t];

  t = T(@"Scroll list", CatCA);
  t.setup = ^(Bench *b, UIView *stage) {
    [b makeListImages];
    UITableView *tv = [[UITableView alloc] initWithFrame:stage.bounds style:UITableViewStylePlain];
    tv.dataSource = (id<UITableViewDataSource>)b;
    tv.tag = 77;
    tv.rowHeight = 72;
    [stage addSubview:tv];
    b->_views = [NSMutableArray arrayWithObject:tv];
  };
  t.frame = ^(Bench *b, double el) {
    UITableView *tv = b->_views.firstObject;
    CGFloat y = fmod(el * 2400.0, MAX(tv.contentSize.height - tv.bounds.size.height, 1));
    tv.contentOffset = CGPointMake(0, y);
  };
  t.finish = ^NSString *(Bench *b) {
    g_listImages = nil;  // 64 images of 240x240: about 15 MiB that stayed in the footprint after the first run
    return @"UITableView 3000 rows, image + 2 labels + shadowed badge each, 2400 pt/s";
  };
  [a addObject:t];

  t = T(@"App transition", CatCA);
  t.setup = ^(Bench *b, UIView *stage) {
    [b addMovingBackground:stage count:20];
    UIView *app = [[UIView alloc] initWithFrame:stage.bounds];
    app.backgroundColor = UIColor.systemBackgroundColor;
    app.layer.cornerRadius = 40;
    app.clipsToBounds = YES;
    UIVisualEffectView *bar = [[UIVisualEffectView alloc] initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemChromeMaterial]];
    bar.frame = CGRectMake(0, 0, stage.bounds.size.width, 90);
    [app addSubview:bar];
    for (int i = 0; i < 30; i++) {
      UILabel *l = [[UILabel alloc] initWithFrame:CGRectMake(16, 100 + i * 36, stage.bounds.size.width - 32, 30)];
      l.text = [NSString stringWithFormat:@"Row %d: some app content with text", i];
      l.backgroundColor = [UIColor colorWithHue:i / 30.0 saturation:0.15 brightness:1 alpha:1];
      [app addSubview:l];
    }
    app.transform = CGAffineTransformMakeScale(0.12, 0.12);
    app.alpha = 0;
    [stage addSubview:app];
    b->_views = [NSMutableArray arrayWithObject:app];
    b->_param = 0;
  };
  t.frame = ^(Bench *b, double el) {
    int cycle = (int)(el / 0.6);
    if (cycle == b->_param) return;
    b->_param = cycle;
    UIView *app = b->_views.firstObject;
    BOOL open = cycle % 2 == 1;
    CGSize sz = b->_stage.bounds.size;
    CGPoint icon = CGPointMake(sz.width * 0.25 + (cycle % 4) * 40, sz.height * 0.75);
    if (open) { app.center = icon; app.transform = CGAffineTransformMakeScale(0.12, 0.12); }
    [UIView animateWithDuration:0.45 delay:0 usingSpringWithDamping:0.86 initialSpringVelocity:0.3 options:0 animations:^{
      app.transform = open ? CGAffineTransformIdentity : CGAffineTransformMakeScale(0.12, 0.12);
      app.center = open ? CGPointMake(sz.width / 2, sz.height / 2) : icon;
      app.alpha = open ? 1 : 0;
    } completion:nil];
  };
  t.finish = ^NSString *(Bench *b) {
    return [NSString stringWithFormat:@"spring zoom open/close every 0.6 s (%d), blurred bar + 30 rows over 20 spinning views", b->_param];
  };
  [a addObject:t];
}

- (void)makeListImages {
  if (g_listImages) return;
  NSMutableArray *a = [NSMutableArray array];
  UIGraphicsImageRenderer *r = [[UIGraphicsImageRenderer alloc] initWithSize:CGSizeMake(120, 120)];
  for (int i = 0; i < 64; i++) {
    UIImage *img = [r imageWithActions:^(UIGraphicsImageRendererContext *ctx) {
      CGContextRef c = ctx.CGContext;
      CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
      CGFloat comps[8] = {(i % 8) / 8.0, 0.4, 1 - (i % 8) / 8.0, 1, 1, (i % 5) / 5.0, 0.3, 1};
      CGGradientRef g = CGGradientCreateWithColorComponents(cs, comps, NULL, 2);
      CGContextDrawLinearGradient(c, g, CGPointZero, CGPointMake(120, 120), 0);
      CGGradientRelease(g);
      CGColorSpaceRelease(cs);
      [[NSString stringWithFormat:@"%d", i] drawAtPoint:CGPointMake(30, 36) withAttributes:@{
        NSFontAttributeName : [UIFont boldSystemFontOfSize:40], NSForegroundColorAttributeName : UIColor.whiteColor}];
    }];
    [a addObject:img];
  }
  g_listImages = a;
}


// ---- image decode ---------------------------------------------------------------------------------------------------------
static CGImageRef make_photo(int w, int h, int seed) {
  CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
  CGContextRef c = CGBitmapContextCreate(NULL, w, h, 8, w * 4, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
  uint32_t *px = CGBitmapContextGetData(c);
  uint32_t s = 2463534242u + seed;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      s ^= s << 13; s ^= s >> 17; s ^= s << 5;
      uint32_t r = (x * 255 / w + (s & 15)) & 255, g = (y * 255 / h + ((s >> 4) & 15)) & 255, bl = ((x + y + seed * 9) & 255);
      px[y * w + x] = 0xff000000u | (bl << 16) | (g << 8) | r;
    }
  CGImageRef img = CGBitmapContextCreateImage(c);
  CGContextRelease(c);
  CGColorSpaceRelease(cs);
  return img;
}
static NSData *encode_images(CFStringRef type, int frames, int w, int h) {
  NSMutableData *d = [NSMutableData data];
  CGImageDestinationRef dst = CGImageDestinationCreateWithData((__bridge CFMutableDataRef)d, type, frames, NULL);
  if (!dst) return nil;
  NSDictionary *props = @{(__bridge NSString *)kCGImageDestinationLossyCompressionQuality : @0.85};
  if (frames > 1)
    CGImageDestinationSetProperties(dst, (__bridge CFDictionaryRef) @{(__bridge NSString *)kCGImagePropertyGIFDictionary :
                                                                           @{(__bridge NSString *)kCGImagePropertyGIFLoopCount : @0}});
  for (int i = 0; i < frames; i++) {
    CGImageRef img = make_photo(w, h, i);
    NSDictionary *fp = frames > 1 ? @{(__bridge NSString *)kCGImagePropertyGIFDictionary : @{(__bridge NSString *)kCGImagePropertyGIFDelayTime : @0.04}} : props;
    CGImageDestinationAddImage(dst, img, (__bridge CFDictionaryRef)fp);
    CGImageRelease(img);
  }
  BOOL ok = CGImageDestinationFinalize(dst);
  CFRelease(dst);
  return ok ? d : nil;
}
// Decodes every frame of `data` again and again for `seconds`; returns ms per frame and the count.
static double decode_loop(NSData *data, double seconds, int *count, size_t *pixels) {
  double t0 = now_s();
  int n = 0;
  size_t px = 0;
  NSDictionary *opt = @{(__bridge NSString *)kCGImageSourceShouldCacheImmediately : @YES, (__bridge NSString *)kCGImageSourceShouldCache : @NO};
  while (now_s() - t0 < seconds) {
    CGImageSourceRef src = CGImageSourceCreateWithData((__bridge CFDataRef)data, NULL);
    size_t frames = CGImageSourceGetCount(src);
    for (size_t i = 0; i < frames; i++) {
      CGImageRef img = CGImageSourceCreateImageAtIndex(src, i, (__bridge CFDictionaryRef)opt);
      if (img) {
        // draw it once into a small bitmap as well: a decode the cache flag skipped still happens here
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef c = CGBitmapContextCreate(NULL, 64, 64, 8, 256, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
        CGContextDrawImage(c, CGRectMake(0, 0, 64, 64), img);
        CGContextRelease(c);
        CGColorSpaceRelease(cs);
        px += CGImageGetWidth(img) * CGImageGetHeight(img);
        CGImageRelease(img);
        n++;
      }
    }
    CFRelease(src);
  }
  *count = n;
  *pixels = px;
  return n ? (now_s() - t0) * 1000.0 / n : NAN;
}

- (void)addDecodeTests:(NSMutableArray *)a {
  BenchTest *t = T(@"Image decode", CatDecode);
  t.work = ^NSString *(Bench *b) {
    double e0 = now_ms();
    NSData *jpg = encode_images(CFSTR("public.jpeg"), 1, 2048, 1536);
    NSData *png = encode_images(CFSTR("public.png"), 1, 2048, 1536);
    NSData *gif = encode_images(CFSTR("com.compuserve.gif"), 24, 480, 270);
    double encMs = now_ms() - e0;
    NSMutableString *s = [NSMutableString stringWithFormat:@"encode_all=%.0fms ", encMs];
    NSArray *names = @[ @"jpeg2048x1536", @"png2048x1536", @"gif480x270x24" ];
    NSArray *datas = @[ jpg ?: [NSData data], png ?: [NSData data], gif ?: [NSData data] ];
    for (int k = 0; k < 3 && !b.stopRequested; k++) {
      NSData *d = datas[k];
      if (!d.length) { [s appendFormat:@"%@=encode-failed ", names[k]]; continue; }
      int n = 0;
      size_t px = 0;
      double t0 = now_s();
      double ms = decode_loop(d, 4.0, &n, &px);
      [s appendFormat:@"%@=%.2fms/frame(%.0fMpix/s,%luKB) ", names[k], ms, px / (now_s() - t0) / 1e6, (unsigned long)(d.length / 1024)];
    }
    return s;
  };
  [a addObject:t];
}

// ---- memory pressure ---------------------------------------------------------------------------------------------------
- (void)memoryWarning {
  if (_memWarning) return;
  _memWarning = YES;
  _memWarnAt = _chunks.count * 32.0;
  _memAvailAtWarn = avail_mib();
  BLog(@"oneios-bench: mem-warning allocated=%.0fMiB foot=%.1fMiB avail=%.0fMiB", _memWarnAt, footprint_mib(), _memAvailAtWarn);
}
- (void)reportLastMemoryRun {
  NSUserDefaults *u = [NSUserDefaults standardUserDefaults];
  NSInteger step = [u integerForKey:@"oneios.memStep"];
  if (step > 0) {
    BLog(@"oneios-bench: mem-previous the last memory test did not finish: it reached %ldMiB (killed, probably by jetsam)", (long)step * 32);
    [u removeObjectForKey:@"oneios.memStep"];
  }
}
- (void)addMemTests:(NSMutableArray *)a {
  BenchTest *t = T(@"Memory pressure", CatMem);
  t.seconds = 40;
  t.setup = ^(Bench *b, UIView *stage) {
    b->_chunks = [NSMutableArray array];
    b->_memWarning = NO;
    b->_memWarnAt = NAN;
    b->_memAvailAtWarn = NAN;
    b->_stepStart = 0;
    b->_auxSum = avail_mib();
    UILabel *l = [[UILabel alloc] initWithFrame:CGRectInset(stage.bounds, 20, 200)];
    l.numberOfLines = 0;
    l.textColor = UIColor.whiteColor;
    l.text = @"Allocating 32 MiB steps (touched) until a memory warning...";
    [stage addSubview:l];
  };
  t.frame = ^(Bench *b, double el) {
    if (b->_memWarning || b->_stepStart < 0) return;
    if (el - b->_stepStart < 0.15) return;
    b->_stepStart = el;
    double avail = avail_mib();
    if (avail >= 0 && avail < 96) {  // stop before jetsam: the warning never came (-1: no limit, the warning ends it)
      b->_stepStart = -1;
      BLog(@"oneios-bench: mem-stop no warning before avail<96MiB allocated=%luMiB", (unsigned long)b->_chunks.count * 32);
      return;
    }
    void *p = malloc(32 << 20);
    if (!p) { b->_stepStart = -1; return; }
    memset(p, (int)b->_chunks.count | 1, 32 << 20);
    [b->_chunks addObject:[NSValue valueWithPointer:p]];
    [[NSUserDefaults standardUserDefaults] setInteger:(NSInteger)b->_chunks.count forKey:@"oneios.memStep"];
    if (b->_chunks.count % 4 == 0)
      BLog(@"oneios-bench: mem-step allocated=%luMiB foot=%.1fMiB avail=%.0fMiB", (unsigned long)b->_chunks.count * 32, footprint_mib(), avail);
  };
  t.finish = ^NSString *(Bench *b) {
    double peak = footprint_mib();
    unsigned long total = (unsigned long)b->_chunks.count * 32;
    for (NSValue *v in b->_chunks) free(v.pointerValue);
    [b->_chunks removeAllObjects];
    [[NSUserDefaults standardUserDefaults] removeObjectForKey:@"oneios.memStep"];
    return [NSString stringWithFormat:@"avail_at_start=%.0fMiB warning_at=%@ avail_at_warning=%@ allocated=%luMiB peak_foot=%.0fMiB",
            b->_auxSum, isnan(b->_memWarnAt) ? @"none" : [NSString stringWithFormat:@"%.0fMiB", b->_memWarnAt],
            isnan(b->_memAvailAtWarn) ? @"n/a" : [NSString stringWithFormat:@"%.0fMiB", b->_memAvailAtWarn], total, peak];
  };
  [a addObject:t];
}

- (NSArray<BenchTest *> *)makeTests {
  NSMutableArray *a = [NSMutableArray array];
  [self addGPUTests:a];
  [self addCPUTests:a];
  [self addCATests:a];
  [self addDecodeTests:a];
  [self addMemTests:a];
  // A GPU test without a Metal device reports so instead of drawing.
  for (BenchTest *t in a) {
    if (t.cat != CatGPU) continue;
    void (^setup)(Bench *, UIView *) = t.setup;
    NSString *(^finish)(Bench *) = t.finish;
    void (^frame)(Bench *, double) = t.frame;
    t.setup = ^(Bench *b, UIView *stage) {
      if (b->_dev) setup(b, stage);
      else b->_current.seconds = 0.5;  // no device: no 15 s of a black stage (it looked like a hang)
    };
    t.frame = ^(Bench *b, double el) { if (b->_dev) frame(b, el); };
    t.finish = ^NSString *(Bench *b) {
      return b->_dev ? finish(b) : [NSString stringWithFormat:@"no-metal-device (%@)", b->_metalError ?: @"?"];
    };
  }
  return a;
}
@end

// ---- app and scene -------------------------------------------------------------------------------------------------------
@interface SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property (nonatomic, strong) UIWindow *window;
@end
@implementation SceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
  self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
  self.window.rootViewController = [[Bench alloc] init];
  [self.window makeKeyAndVisible];
}
@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation AppDelegate
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options { return YES; }
- (UISceneConfiguration *)application:(UIApplication *)application
    configurationForConnectingSceneSession:(UISceneSession *)session
                                   options:(UISceneConnectionOptions *)options {
  UISceneConfiguration *c = [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:session.role];
  c.delegateClass = [SceneDelegate class];
  return c;
}
@end

int main(int argc, char *argv[]) {
  @autoreleasepool {
    return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
  }
}

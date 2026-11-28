// SPDX-License-Identifier: MIT
// 2048 for OneiOS: a UIKit game whose tiles move with Core Animation, with a frame-time overlay (CADisplayLink).
// It is a test of the CPU and Core Animation path of the guest: every move animates up to 16 layers, merges bump
// them, and "Auto" plays a move every 90 ms so the animation never stops.
//
// The overlay: FPS, mean and worst frame time of the last second, frames over 1.5 refresh periods. Every 10 s the same
// figures go to iOS's unified log as one line:
//   oneios-2048: fps=118.9 mean=8.41ms p99=12.10ms max=16.70ms drops=3 moves=110 score=1288 thermal=0
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#include <stdlib.h>

#define N 4

@interface Tile : NSObject
@property (nonatomic) int value;
@property (nonatomic, strong) UILabel *view;
@property (nonatomic) BOOL merged;
@end
@implementation Tile
@end

static UIColor *TileColor(int v) {
  switch (v) {
    case 2: return [UIColor colorWithRed:0.93 green:0.89 blue:0.85 alpha:1];
    case 4: return [UIColor colorWithRed:0.93 green:0.88 blue:0.78 alpha:1];
    case 8: return [UIColor colorWithRed:0.95 green:0.69 blue:0.47 alpha:1];
    case 16: return [UIColor colorWithRed:0.96 green:0.58 blue:0.39 alpha:1];
    case 32: return [UIColor colorWithRed:0.96 green:0.49 blue:0.37 alpha:1];
    case 64: return [UIColor colorWithRed:0.96 green:0.37 blue:0.23 alpha:1];
    case 128: return [UIColor colorWithRed:0.93 green:0.81 blue:0.45 alpha:1];
    case 256: return [UIColor colorWithRed:0.93 green:0.80 blue:0.38 alpha:1];
    case 512: return [UIColor colorWithRed:0.93 green:0.78 blue:0.31 alpha:1];
    case 1024: return [UIColor colorWithRed:0.93 green:0.77 blue:0.25 alpha:1];
    case 2048: return [UIColor colorWithRed:0.93 green:0.76 blue:0.18 alpha:1];
    default: return [UIColor colorWithRed:0.24 green:0.23 blue:0.20 alpha:1];
  }
}

// ---- frame-time overlay ------------------------------------------------------------------------------------------
@interface FrameMeter : NSObject
@property (nonatomic, strong) UILabel *label;
@property (nonatomic, copy) NSString *(^extra)(void);
- (void)start;
@end

@implementation FrameMeter {
  CADisplayLink *_link;
  CFTimeInterval _last, _windowStart, _logStart;
  double _ms[2048];
  int _count;
  double _logMs[4096];
  int _logCount, _logDrops;
}
- (void)start {
  _link = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick:)];
  _link.preferredFrameRateRange = CAFrameRateRangeMake(60, 120, 120);
  [_link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
}
static int cmpd(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}
- (void)tick:(CADisplayLink *)link {
  CFTimeInterval now = link.timestamp;
  if (_last == 0) { _last = now; _windowStart = now; _logStart = now; return; }
  double ms = (now - _last) * 1000.0;
  _last = now;
  double period = (link.targetTimestamp - link.timestamp) * 1000.0;
  if (period <= 0.5) period = 1000.0 / 120.0;
  if (_count < 2048) _ms[_count++] = ms;
  if (_logCount < 4096) _logMs[_logCount++] = ms;
  if (ms > period * 1.5) _logDrops += (int)(ms / period + 0.5) - 1;
  if (now - _windowStart >= 0.5) {
    double sum = 0, worst = 0;
    int slow = 0;
    for (int i = 0; i < _count; i++) { sum += _ms[i]; if (_ms[i] > worst) worst = _ms[i]; if (_ms[i] > period * 1.5) slow++; }
    double fps = _count / (now - _windowStart);
    self.label.text = [NSString stringWithFormat:@"%.1f fps  %.2f ms avg  %.2f max  %d slow  (%.0f Hz)%@", fps, sum / _count,
                       worst, slow, 1000.0 / period, self.extra ? self.extra() : @""];
    _count = 0;
    _windowStart = now;
  }
  if (now - _logStart >= 10.0 && _logCount > 0) {
    double sum = 0;
    for (int i = 0; i < _logCount; i++) sum += _logMs[i];
    qsort(_logMs, _logCount, sizeof(double), cmpd);
    double p99 = _logMs[(int)(_logCount * 0.99)], worst = _logMs[_logCount - 1];
    NSLog(@"oneios-2048: fps=%.1f mean=%.2fms p99=%.2fms max=%.2fms drops=%d %@ thermal=%ld", _logCount / (now - _logStart),
          sum / _logCount, p99, worst, _logDrops, self.extra ? self.extra() : @"", (long)[NSProcessInfo processInfo].thermalState);
    _logCount = 0;
    _logDrops = 0;
    _logStart = now;
  }
}
@end

// ---- the game ---------------------------------------------------------------------------------------------------
@interface GameViewController : UIViewController
@end

@implementation GameViewController {
  Tile *_grid[N][N];
  UIView *_board;
  UILabel *_scoreLabel, *_fpsLabel;
  UIButton *_autoButton, *_newButton;
  FrameMeter *_meter;
  NSTimer *_autoTimer;
  int _score, _moves;
  CGFloat _cell, _gap;
  BOOL _animating;
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = [UIColor colorWithRed:0.98 green:0.97 blue:0.94 alpha:1];
  _board = [[UIView alloc] init];
  _board.backgroundColor = [UIColor colorWithRed:0.73 green:0.68 blue:0.63 alpha:1];
  _board.layer.cornerRadius = 10;
  [self.view addSubview:_board];

  _scoreLabel = [[UILabel alloc] init];
  _scoreLabel.font = [UIFont boldSystemFontOfSize:28];
  _scoreLabel.textColor = [UIColor colorWithRed:0.47 green:0.43 blue:0.40 alpha:1];
  [self.view addSubview:_scoreLabel];

  _fpsLabel = [[UILabel alloc] init];
  _fpsLabel.font = [UIFont monospacedDigitSystemFontOfSize:12 weight:UIFontWeightMedium];
  _fpsLabel.textColor = UIColor.whiteColor;
  _fpsLabel.backgroundColor = [UIColor colorWithWhite:0 alpha:0.65];
  _fpsLabel.numberOfLines = 2;
  _fpsLabel.layer.cornerRadius = 6;
  _fpsLabel.layer.masksToBounds = YES;
  [self.view addSubview:_fpsLabel];

  _newButton = [UIButton buttonWithType:UIButtonTypeSystem];
  [_newButton setTitle:@"New game" forState:UIControlStateNormal];
  [_newButton addTarget:self action:@selector(newGame) forControlEvents:UIControlEventTouchUpInside];
  [self.view addSubview:_newButton];
  _autoButton = [UIButton buttonWithType:UIButtonTypeSystem];
  [_autoButton setTitle:@"Auto: off" forState:UIControlStateNormal];
  [_autoButton addTarget:self action:@selector(toggleAuto) forControlEvents:UIControlEventTouchUpInside];
  [self.view addSubview:_autoButton];

  UISwipeGestureRecognizerDirection dirs[4] = {UISwipeGestureRecognizerDirectionLeft, UISwipeGestureRecognizerDirectionRight,
                                               UISwipeGestureRecognizerDirectionUp, UISwipeGestureRecognizerDirectionDown};
  for (int i = 0; i < 4; i++) {
    UISwipeGestureRecognizer *g = [[UISwipeGestureRecognizer alloc] initWithTarget:self action:@selector(swiped:)];
    g.direction = dirs[i];
    [self.view addGestureRecognizer:g];
  }

  _meter = [[FrameMeter alloc] init];
  _meter.label = _fpsLabel;
  __weak GameViewController *weakSelf = self;
  _meter.extra = ^NSString *{
    GameViewController *s = weakSelf;
    return s ? [NSString stringWithFormat:@"\nmoves=%d score=%d", s->_moves, s->_score] : @"";
  };
  [_meter start];
  NSLog(@"oneios-2048: start %@", [[NSBundle mainBundle] bundlePath]);
}

- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  BOOL first = _cell == 0;
  CGRect safe = UIEdgeInsetsInsetRect(self.view.bounds, self.view.safeAreaInsets);
  CGFloat side = MIN(safe.size.width - 24, safe.size.height - 200);
  _gap = side * 0.03;
  _cell = (side - _gap * (N + 1)) / N;
  _board.frame = CGRectMake(CGRectGetMidX(safe) - side / 2, CGRectGetMidY(safe) - side / 2 + 30, side, side);
  _scoreLabel.frame = CGRectMake(safe.origin.x + 16, CGRectGetMinY(_board.frame) - 56, side, 40);
  _fpsLabel.frame = CGRectMake(safe.origin.x + 8, safe.origin.y + 4, safe.size.width - 16, 36);
  _newButton.frame = CGRectMake(CGRectGetMinX(_board.frame), CGRectGetMaxY(_board.frame) + 16, side / 2, 44);
  _autoButton.frame = CGRectMake(CGRectGetMidX(_board.frame), CGRectGetMaxY(_board.frame) + 16, side / 2, 44);
  if (first) {
    for (int r = 0; r < N; r++)
      for (int c = 0; c < N; c++) {
        UIView *slot = [[UIView alloc] initWithFrame:[self frameAtRow:r col:c]];
        slot.backgroundColor = [UIColor colorWithRed:0.80 green:0.76 blue:0.71 alpha:1];
        slot.layer.cornerRadius = 6;
        [_board addSubview:slot];
      }
    [self newGame];
  }
}

- (CGRect)frameAtRow:(int)r col:(int)c {
  return CGRectMake(_gap + c * (_cell + _gap), _gap + r * (_cell + _gap), _cell, _cell);
}

- (void)styleTile:(Tile *)t {
  t.view.text = [NSString stringWithFormat:@"%d", t.value];
  t.view.backgroundColor = TileColor(t.value);
  t.view.textColor = t.value <= 4 ? [UIColor colorWithRed:0.47 green:0.43 blue:0.40 alpha:1] : UIColor.whiteColor;
  CGFloat size = t.value < 100 ? _cell * 0.45 : t.value < 1000 ? _cell * 0.36 : _cell * 0.28;
  t.view.font = [UIFont boldSystemFontOfSize:size];
}

- (void)newGame {
  for (int r = 0; r < N; r++)
    for (int c = 0; c < N; c++) {
      [_grid[r][c].view removeFromSuperview];
      _grid[r][c] = nil;
    }
  _score = 0;
  _moves = 0;
  [self spawn];
  [self spawn];
  [self updateScore];
}

- (void)updateScore {
  _scoreLabel.text = [NSString stringWithFormat:@"Score %d", _score];
}

- (BOOL)spawn {
  int slots[N * N][2], count = 0;
  for (int r = 0; r < N; r++)
    for (int c = 0; c < N; c++)
      if (!_grid[r][c]) { slots[count][0] = r; slots[count][1] = c; count++; }
  if (!count) return NO;
  int pick = arc4random_uniform(count);
  int r = slots[pick][0], c = slots[pick][1];
  Tile *t = [[Tile alloc] init];
  t.value = arc4random_uniform(10) == 0 ? 4 : 2;
  t.view = [[UILabel alloc] initWithFrame:[self frameAtRow:r col:c]];
  t.view.textAlignment = NSTextAlignmentCenter;
  t.view.layer.cornerRadius = 6;
  t.view.layer.masksToBounds = YES;
  [self styleTile:t];
  [_board addSubview:t.view];
  _grid[r][c] = t;
  t.view.transform = CGAffineTransformMakeScale(0.1, 0.1);
  [UIView animateWithDuration:0.12 delay:0.06 options:0 animations:^{ t.view.transform = CGAffineTransformIdentity; } completion:nil];
  return YES;
}

// Moves the board towards (dr, dc); returns whether anything moved.
- (BOOL)moveRow:(int)dr col:(int)dc {
  BOOL moved = NO;
  for (int r = 0; r < N; r++)
    for (int c = 0; c < N; c++) _grid[r][c].merged = NO;
  NSMutableArray<Tile *> *gone = [NSMutableArray array];
  NSMutableArray<Tile *> *bumped = [NSMutableArray array];
  // Walk the cells from the side the tiles move to.
  for (int i = 0; i < N; i++) {
    for (int k = 0; k < N; k++) {
      int r, c;
      if (dr) { c = i; r = dr > 0 ? N - 1 - k : k; }
      else { r = i; c = dc > 0 ? N - 1 - k : k; }
      Tile *t = _grid[r][c];
      if (!t) continue;
      int nr = r, nc = c;
      while (1) {
        int tr = nr + dr, tc = nc + dc;
        if (tr < 0 || tr >= N || tc < 0 || tc >= N) break;
        Tile *o = _grid[tr][tc];
        if (!o) { nr = tr; nc = tc; continue; }
        if (o.value == t.value && !o.merged) {
          // merge into o
          o.value *= 2;
          o.merged = YES;
          _score += o.value;
          _grid[r][c] = nil;
          [gone addObject:t];
          [bumped addObject:o];
          CGRect target = [self frameAtRow:tr col:tc];
          [UIView animateWithDuration:0.1 animations:^{ t.view.frame = target; }];
          moved = YES;
          t = nil;
        }
        break;
      }
      if (t && (nr != r || nc != c)) {
        _grid[r][c] = nil;
        _grid[nr][nc] = t;
        CGRect target = [self frameAtRow:nr col:nc];
        [UIView animateWithDuration:0.1 animations:^{ t.view.frame = target; }];
        moved = YES;
      }
    }
  }
  if (moved) {
    _animating = YES;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.1 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
      for (Tile *t in gone) [t.view removeFromSuperview];
      for (Tile *o in bumped) {
        [self styleTile:o];
        [o.view.superview bringSubviewToFront:o.view];
        CABasicAnimation *pop = [CABasicAnimation animationWithKeyPath:@"transform.scale"];
        pop.fromValue = @1.0;
        pop.toValue = @1.18;
        pop.duration = 0.07;
        pop.autoreverses = YES;
        [o.view.layer addAnimation:pop forKey:@"pop"];
      }
      [self spawn];
      [self updateScore];
      self->_animating = NO;
      if (![self canMove]) {
        NSLog(@"oneios-2048: game over score=%d moves=%d", self->_score, self->_moves);
        if (self->_autoTimer) [self newGame];
      }
    });
  }
  return moved;
}

- (BOOL)canMove {
  for (int r = 0; r < N; r++)
    for (int c = 0; c < N; c++) {
      if (!_grid[r][c]) return YES;
      if (c + 1 < N && _grid[r][c + 1] && _grid[r][c + 1].value == _grid[r][c].value) return YES;
      if (r + 1 < N && _grid[r + 1][c] && _grid[r + 1][c].value == _grid[r][c].value) return YES;
    }
  return NO;
}

- (void)slide:(UISwipeGestureRecognizerDirection)d {
  if (_animating) return;
  int dr = 0, dc = 0;
  if (d == UISwipeGestureRecognizerDirectionLeft) dc = -1;
  else if (d == UISwipeGestureRecognizerDirectionRight) dc = 1;
  else if (d == UISwipeGestureRecognizerDirectionUp) dr = -1;
  else dr = 1;
  if ([self moveRow:dr col:dc]) _moves++;
}

- (void)swiped:(UISwipeGestureRecognizer *)g {
  [self slide:g.direction];
}

- (void)toggleAuto {
  if (_autoTimer) {
    [_autoTimer invalidate];
    _autoTimer = nil;
    [_autoButton setTitle:@"Auto: off" forState:UIControlStateNormal];
    return;
  }
  [_autoButton setTitle:@"Auto: on" forState:UIControlStateNormal];
  __weak GameViewController *weakSelf = self;
  _autoTimer = [NSTimer scheduledTimerWithTimeInterval:0.09 repeats:YES block:^(NSTimer *timer) {
    GameViewController *s = weakSelf;
    if (!s || s->_animating) return;
    // prefer down/left/right, as a lazy player does; up only when nothing else moves
    UISwipeGestureRecognizerDirection order[4] = {UISwipeGestureRecognizerDirectionDown, UISwipeGestureRecognizerDirectionLeft,
                                                  UISwipeGestureRecognizerDirectionRight, UISwipeGestureRecognizerDirectionUp};
    int first = arc4random_uniform(3);
    UISwipeGestureRecognizerDirection a = order[first], b = order[(first + 1) % 3], c = order[(first + 2) % 3];
    UISwipeGestureRecognizerDirection seq[4] = {a, b, c, order[3]};
    for (int i = 0; i < 4; i++) {
      int before = s->_moves;
      [s slide:seq[i]];
      if (s->_moves != before) break;
    }
  }];
}

- (BOOL)prefersStatusBarHidden {
  return YES;
}
@end

// ---- app and scene ------------------------------------------------------------------------------------------------
@interface SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property (nonatomic, strong) UIWindow *window;
@end
@implementation SceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
  self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
  self.window.rootViewController = [[GameViewController alloc] init];
  [self.window makeKeyAndVisible];
}
@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation AppDelegate
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options {
  return YES;
}
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

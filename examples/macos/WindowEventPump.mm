#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <IOSurface/IOSurface.h>

#include "../PaintInputBridge.hpp"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <dispatch/dispatch.h>

@class PSWindowDelegate;

namespace {

static NSWindow* gWindow = nil;
static PSWindowDelegate* gWindowDelegate = nil;
static std::mutex gFramebufferMutex;
static std::shared_ptr<std::vector<std::uint8_t>> gFramebufferPixels;
static int gFramebufferWidth = 0;
static int gFramebufferHeight = 0;
static int gFramebufferStride = 0;
static IOSurfaceRef gPresentedIOSurface = nullptr;
static int gIOSurfaceWidth = 0;
static int gIOSurfaceHeight = 0;
static int gDesiredWindowWidth = 640;
static int gDesiredWindowHeight = 360;
static std::string gDesiredWindowTitle = "PathSpace Input Test";
static CAMetalLayer* gMetalLayer = nil;
static id<MTLDevice> gMetalDevice = nil;
static id<MTLCommandQueue> gMetalCommandQueue = nil;
static bool gMetalAvailable = false;
static std::atomic<bool> gMetalPresentScheduled{false};
static CGFloat gBackingScale = 1.0;

static void EnsureMetalDevice() {
    if (gMetalDevice && gMetalCommandQueue) {
        return;
    }
    gMetalDevice = MTLCreateSystemDefaultDevice();
    if (!gMetalDevice) {
        gMetalAvailable = false;
        return;
    }
    gMetalCommandQueue = [gMetalDevice newCommandQueue];
    if (!gMetalCommandQueue) {
        gMetalDevice = nil;
        gMetalAvailable = false;
        return;
    }
    gMetalAvailable = true;
}

static bool ComputePixelCoordinates(NSEvent* ev, int& outX, int& outY) {
    NSWindow* window = ev.window ?: gWindow;
    if (!window) return false;
    NSView* view = window.contentView;
    if (!view) return false;

    NSPoint inWindow = ev.locationInWindow;
    NSPoint local = [view convertPoint:inWindow fromView:nil];
    NSRect bounds = view.bounds;

    CGFloat scale = window.backingScaleFactor;
    if (scale < (CGFloat)1.0) {
        scale = 1.0;
    }
    double widthPixels = static_cast<double>(bounds.size.width) * static_cast<double>(scale);
    double heightPixels = static_cast<double>(bounds.size.height) * static_cast<double>(scale);
    if (widthPixels <= 0.0 || heightPixels <= 0.0) {
        return false;
    }

    double px = std::clamp(static_cast<double>(local.x) * static_cast<double>(scale),
                            0.0,
                            widthPixels - 1.0);
    double py = std::clamp(static_cast<double>(local.y) * static_cast<double>(scale),
                            0.0,
                            heightPixels - 1.0);
    double flippedY = (heightPixels - 1.0) - py;

    outX = static_cast<int>(std::llround(px));
    outY = static_cast<int>(std::llround(flippedY));
    return true;
}

static PaintInput::MouseButton ButtonFromEvent(NSEvent* ev) {
    switch (ev.type) {
        case NSEventTypeLeftMouseDown:
        case NSEventTypeLeftMouseUp:
            return PaintInput::MouseButton::Left;
        case NSEventTypeRightMouseDown:
        case NSEventTypeRightMouseUp:
            return PaintInput::MouseButton::Right;
        case NSEventTypeOtherMouseDown:
        case NSEventTypeOtherMouseUp:
        default:
            if (ev.buttonNumber == 2) return PaintInput::MouseButton::Middle;
            if (ev.buttonNumber == 3) return PaintInput::MouseButton::Button4;
            if (ev.buttonNumber == 4) return PaintInput::MouseButton::Button5;
            return PaintInput::MouseButton::Middle;
    }
}

static void ScheduleMetalPresent() {
    if (!gMetalAvailable || !gMetalLayer || !gMetalCommandQueue) {
        if (gWindow) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [[gWindow contentView] setNeedsDisplay:YES];
            });
        }
        return;
    }
    bool expected = false;
    if (!gMetalPresentScheduled.compare_exchange_strong(expected, true)) {
        return;
    }

    dispatch_async(dispatch_get_main_queue(), ^{
        gMetalPresentScheduled.store(false);
        @autoreleasepool {
            CAMetalLayer* layer = gMetalLayer;
            id<MTLCommandQueue> queue = gMetalCommandQueue;
            NSWindow* windowStrong = gWindow;
            if (!layer || !queue || !windowStrong) {
                return;
            }

            std::shared_ptr<std::vector<std::uint8_t>> pixels;
            int width = 0;
            int height = 0;
            int stride = 0;
#if defined(__APPLE__)
            IOSurfaceRef sharedSurface = nullptr;
            int iosWidth = 0;
            int iosHeight = 0;
#endif
            {
                std::lock_guard<std::mutex> lock(gFramebufferMutex);
                pixels = gFramebufferPixels;
                width = gFramebufferWidth;
                height = gFramebufferHeight;
                stride = gFramebufferStride;
#if defined(__APPLE__)
                if (gPresentedIOSurface) {
                    sharedSurface = gPresentedIOSurface;
                    iosWidth = gIOSurfaceWidth;
                    iosHeight = gIOSurfaceHeight;
                    gPresentedIOSurface = nullptr;
                    gIOSurfaceWidth = 0;
                    gIOSurfaceHeight = 0;
                }
#endif
            }

#if defined(__APPLE__)
            if (sharedSurface && iosWidth > 0 && iosHeight > 0 && gMetalDevice) {
                CGFloat scale = windowStrong.backingScaleFactor;
                if (scale < (CGFloat)1.0) {
                    scale = 1.0;
                }
                layer.contentsScale = scale;
                layer.drawableSize = CGSizeMake(iosWidth, iosHeight);

                id<CAMetalDrawable> drawable = [layer nextDrawable];
                if (drawable) {
                    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                                              width:iosWidth
                                                                                                             height:iosHeight
                                                                                                          mipmapped:NO];
                    descriptor.storageMode = MTLStorageModeShared;
                    id<MTLTexture> sourceTexture = [gMetalDevice newTextureWithDescriptor:descriptor
                                                                                      iosurface:sharedSurface
                                                                                           plane:0];
                    if (sourceTexture) {
                        id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
                        id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
                        MTLSize size = MTLSizeMake(iosWidth, iosHeight, 1);
                        [blit copyFromTexture:sourceTexture
                                   sourceSlice:0
                                    sourceLevel:0
                                   sourceOrigin:MTLOriginMake(0, 0, 0)
                                     sourceSize:size
                                   toTexture:drawable.texture
                            destinationSlice:0
                             destinationLevel:0
                            destinationOrigin:MTLOriginMake(0, 0, 0)];
                        [blit endEncoding];
                        [commandBuffer presentDrawable:drawable];
                        [commandBuffer commit];
                        sourceTexture = nil;
                        CFRelease(sharedSurface);
                        return;
                    }
                }
                CFRelease(sharedSurface);
            } else if (sharedSurface) {
                CFRelease(sharedSurface);
            }
#endif

            if (!pixels || pixels->empty() || width <= 0 || height <= 0 || stride <= 0) {
                return;
            }

            CGFloat scale = windowStrong.backingScaleFactor;
            if (scale < (CGFloat)1.0) {
                scale = 1.0;
            }
            layer.contentsScale = scale;
            layer.drawableSize = CGSizeMake(width, height);

            id<CAMetalDrawable> drawable = [layer nextDrawable];
            if (!drawable) {
                return;
            }

            MTLRegion region = MTLRegionMake2D(0, 0, width, height);
            [drawable.texture replaceRegion:region
                                 mipmapLevel:0
                                   withBytes:pixels->data()
                                 bytesPerRow:static_cast<NSUInteger>(stride)];

            id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
            [commandBuffer presentDrawable:drawable];
            [commandBuffer commit];
        }
    });
}

static void UpdateMetalDrawableSize() {
    if (!gMetalAvailable || !gMetalLayer) {
        return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        CAMetalLayer* layer = gMetalLayer;
        NSWindow* windowStrong = gWindow;
        if (!layer || !windowStrong) {
            return;
        }
        CGFloat scale = windowStrong.backingScaleFactor;
        if (scale < (CGFloat)1.0) {
            scale = 1.0;
        }
        gBackingScale = scale;
        CGSize boundsSize = layer.bounds.size;
        layer.contentsScale = scale;
        layer.drawableSize = CGSizeMake(boundsSize.width * scale,
                                        boundsSize.height * scale);
    });
}

} // namespace

@interface PSEventView : NSView
@property(nonatomic, strong) CAMetalLayer* psMetalLayer;
- (void)configureMetalLayerIfPossible;
@end

@interface PSWindowDelegate : NSObject <NSWindowDelegate>
@end

@implementation PSEventView

- (instancetype)initWithFrame:(NSRect)frameRect {
    self = [super initWithFrame:frameRect];
    if (self) {
        [self configureMetalLayerIfPossible];
    }
    return self;
}

- (void)awakeFromNib {
    [super awakeFromNib];
    [self configureMetalLayerIfPossible];
}

- (void)configureMetalLayerIfPossible {
    EnsureMetalDevice();
    if (!gMetalDevice) {
        self.wantsLayer = YES;
        gMetalAvailable = false;
        self.psMetalLayer = nil;
        gMetalLayer = nil;
        return;
    }

    self.wantsLayer = YES;
    CAMetalLayer* layer = self.psMetalLayer;
    if (!layer) {
        layer = [CAMetalLayer layer];
    }
    layer.device = gMetalDevice;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = NO;
    layer.magnificationFilter = kCAFilterNearest;
    layer.minificationFilter = kCAFilterNearest;

    NSWindow* window = self.window ?: gWindow;
    CGFloat scale = window ? window.backingScaleFactor : (CGFloat)1.0;
    if (scale < (CGFloat)1.0) {
        scale = 1.0;
    }
    gBackingScale = scale;
    layer.contentsScale = scale;
    layer.drawableSize = CGSizeMake(self.bounds.size.width * scale,
                                    self.bounds.size.height * scale);

    self.layer = layer;
    self.psMetalLayer = layer;
    gMetalLayer = layer;
    gMetalAvailable = true;
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (self.window) {
        [self.window setAcceptsMouseMovedEvents:YES];
        [self.window makeFirstResponder:self];
    }
    [self configureMetalLayerIfPossible];
    UpdateMetalDrawableSize();
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    UpdateMetalDrawableSize();
}

- (void)enqueueMouseEvent:(PaintInput::MouseEvent)event {
    PaintInput::enqueue_mouse(event);
}

- (void)handleMoveEvent:(NSEvent *)event {
    int xi = 0;
    int yi = 0;
    bool hasCoords = ComputePixelCoordinates(event, xi, yi);

    PaintInput::MouseEvent rel{};
    rel.type = PaintInput::MouseEventType::Move;
    rel.dx = (int)event.deltaX;
    rel.dy = (int)event.deltaY;
    if (hasCoords) {
        rel.x = xi;
        rel.y = yi;
    }
    [self enqueueMouseEvent:rel];

    if (hasCoords) {
        PaintInput::MouseEvent abs{};
        abs.type = PaintInput::MouseEventType::AbsoluteMove;
        abs.x = xi;
        abs.y = yi;
        [self enqueueMouseEvent:abs];
    }
}

- (void)mouseMoved:(NSEvent *)event {
    [self handleMoveEvent:event];
}

- (void)mouseDragged:(NSEvent *)event {
    [self handleMoveEvent:event];
}

- (void)rightMouseDragged:(NSEvent *)event {
    [self handleMoveEvent:event];
}

- (void)otherMouseDragged:(NSEvent *)event {
    [self handleMoveEvent:event];
}

- (void)scrollWheel:(NSEvent *)event {
    CGFloat dy = event.hasPreciseScrollingDeltas ? event.scrollingDeltaY : event.deltaY;
    int ticks = (int)llround(dy);
    if (ticks == 0) {
        return;
    }
    PaintInput::MouseEvent ev{};
    ev.type = PaintInput::MouseEventType::Wheel;
    ev.wheel = ticks;
    int xi = 0, yi = 0;
    if (ComputePixelCoordinates(event, xi, yi)) {
        ev.x = xi;
        ev.y = yi;
    }
    [self enqueueMouseEvent:ev];
}

- (void)mouseDown:(NSEvent *)event {
    PaintInput::MouseEvent ev{};
    ev.type = PaintInput::MouseEventType::ButtonDown;
    ev.button = ButtonFromEvent(event);
    int xi = 0, yi = 0;
    if (ComputePixelCoordinates(event, xi, yi)) {
        ev.x = xi;
        ev.y = yi;
    }
    [self enqueueMouseEvent:ev];
}

- (void)mouseUp:(NSEvent *)event {
    PaintInput::MouseEvent ev{};
    ev.type = PaintInput::MouseEventType::ButtonUp;
    ev.button = ButtonFromEvent(event);
    int xi = 0, yi = 0;
    if (ComputePixelCoordinates(event, xi, yi)) {
        ev.x = xi;
        ev.y = yi;
    }
    [self enqueueMouseEvent:ev];
}

- (void)rightMouseDown:(NSEvent *)event {
    [self mouseDown:event];
}

- (void)rightMouseUp:(NSEvent *)event {
    [self mouseUp:event];
}

- (void)otherMouseDown:(NSEvent *)event {
    [self mouseDown:event];
}

- (void)otherMouseUp:(NSEvent *)event {
    [self mouseUp:event];
}

// No need for additional absolute-move handlers; handled in handleMoveEvent.

- (void)drawRect:(NSRect)dirtyRect {
    [super drawRect:dirtyRect];
    if (gMetalAvailable) {
        return;
    }
    std::shared_ptr<std::vector<std::uint8_t>> pixels;
    int width = 0;
    int height = 0;
    int stride = 0;
    {
        std::lock_guard<std::mutex> lock(gFramebufferMutex);
        pixels = gFramebufferPixels;
        width = gFramebufferWidth;
        height = gFramebufferHeight;
        stride = gFramebufferStride;
    }
    if (!pixels || pixels->empty() || width <= 0 || height <= 0 || stride <= 0) {
        [[NSColor whiteColor] setFill];
        NSRectFill(dirtyRect);
        return;
    }

    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    if (!ctx) {
        return;
    }
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    if (!colorSpace) {
        return;
    }
    CGDataProviderRef provider = CGDataProviderCreateWithData(nullptr,
                                                              pixels->data(),
                                                              pixels->size(),
                                                              nullptr);
    if (!provider) {
        CGColorSpaceRelease(colorSpace);
        return;
    }

    CGBitmapInfo bitmapInfo = kCGImageAlphaPremultipliedLast | kCGBitmapByteOrderDefault;
    CGImageRef image = CGImageCreate(width,
                                     height,
                                     8,
                                     32,
                                     stride,
                                     colorSpace,
                                     bitmapInfo,
                                     provider,
                                     nullptr,
                                     false,
                                     kCGRenderingIntentDefault);
    if (image) {
        CGRect dest = CGRectMake(0, 0, width, height);
        CGContextSaveGState(ctx);
        CGContextTranslateCTM(ctx, 0, height);
        CGContextScaleCTM(ctx, 1.0, -1.0);
        CGContextDrawImage(ctx, dest, image);
        CGContextRestoreGState(ctx);
        CGImageRelease(image);
    }

    CGDataProviderRelease(provider);
    CGColorSpaceRelease(colorSpace);
}

@end

@implementation PSWindowDelegate
- (void)windowWillClose:(NSNotification *)notification {
    (void)notification;
    gMetalAvailable = false;
    gMetalLayer = nil;
    gMetalCommandQueue = nil;
    gMetalDevice = nil;
    gMetalPresentScheduled.store(false);
    PaintInput::clear_mouse();
    {
        std::lock_guard<std::mutex> lock(gFramebufferMutex);
        gFramebufferPixels.reset();
        gFramebufferWidth = 0;
        gFramebufferHeight = 0;
        gFramebufferStride = 0;
    }
    gWindow = nil;
    gWindowDelegate = nil;
}
@end

namespace SP {
class PathIOMouse;
class PathIOKeyboard;

void PSConfigureLocalEventWindow(int width, int height, const char* title) {
    gDesiredWindowWidth = width;
    gDesiredWindowHeight = height;
    if (title) {
        gDesiredWindowTitle = title;
    }
    if (gWindow) {
        dispatch_async(dispatch_get_main_queue(), ^{
            NSRect frame = [gWindow frame];
            frame.size = NSMakeSize(gDesiredWindowWidth, gDesiredWindowHeight);
            [gWindow setFrame:frame display:YES animate:NO];
            gWindow.title = [NSString stringWithUTF8String:gDesiredWindowTitle.c_str()];
            [[gWindow contentView] setFrame:NSMakeRect(0, 0, gDesiredWindowWidth, gDesiredWindowHeight)];
            [[gWindow contentView] setNeedsDisplay:YES];
            UpdateMetalDrawableSize();
        });
    }
}

void PSInitLocalEventWindowWithSize(int width, int height, const char* title);

void PSInitLocalEventWindow() {
    PSInitLocalEventWindowWithSize(gDesiredWindowWidth, gDesiredWindowHeight, gDesiredWindowTitle.c_str());
}

void PSInitLocalEventWindow(PathIOMouse*, PathIOKeyboard*) {
    PSInitLocalEventWindow();
}

void PSInitLocalEventWindowWithSize(int width, int height, const char* title) {
    PSConfigureLocalEventWindow(width, height, title);

    @autoreleasepool {
        EnsureMetalDevice();
        NSApplication* app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        if (!gWindow) {
            NSRect frame = NSMakeRect(200, 200, gDesiredWindowWidth, gDesiredWindowHeight);
            gWindow = [[NSWindow alloc] initWithContentRect:frame
                                                  styleMask:(NSWindowStyleMaskTitled |
                                                             NSWindowStyleMaskClosable |
                                                             NSWindowStyleMaskMiniaturizable |
                                                             NSWindowStyleMaskResizable)
                                                    backing:NSBackingStoreBuffered
                                                      defer:NO];
            gWindow.title = [NSString stringWithUTF8String:gDesiredWindowTitle.c_str()];
            gWindowDelegate = [[PSWindowDelegate alloc] init];
            gWindow.delegate = gWindowDelegate;
            PSEventView* view = [[PSEventView alloc] initWithFrame:NSMakeRect(0, 0, gDesiredWindowWidth, gDesiredWindowHeight)];
            [gWindow setContentView:view];
            [gWindow setAcceptsMouseMovedEvents:YES];
            [gWindow makeFirstResponder:view];
            [gWindow makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
            UpdateMetalDrawableSize();
        } else {
            [[gWindow contentView] setFrame:NSMakeRect(0, 0, gDesiredWindowWidth, gDesiredWindowHeight)];
            gWindow.title = [NSString stringWithUTF8String:gDesiredWindowTitle.c_str()];
            if (!gWindowDelegate) {
                gWindowDelegate = [[PSWindowDelegate alloc] init];
            }
            gWindow.delegate = gWindowDelegate;
            [gWindow makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
            UpdateMetalDrawableSize();
        }
    }
}

void PSPollLocalEventWindow() {
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        while (true) {
            NSEvent* ev = [app nextEventMatchingMask:NSEventMaskAny
                                           untilDate:[NSDate dateWithTimeIntervalSinceNow:0.0]
                                              inMode:NSDefaultRunLoopMode
                                             dequeue:YES];
            if (!ev) break;
            [app sendEvent:ev];
        }
    }
}

void PSUpdateWindowFramebuffer(const std::uint8_t* data,
                               int width,
                               int height,
                               int rowStrideBytes) {
    if (!data || width <= 0 || height <= 0 || rowStrideBytes <= 0) {
        return;
    }
    auto pixels = std::make_shared<std::vector<std::uint8_t>>(
        data,
        data + static_cast<std::size_t>(rowStrideBytes) * static_cast<std::size_t>(height));
    {
        std::lock_guard<std::mutex> lock(gFramebufferMutex);
        if (gPresentedIOSurface) {
            CFRelease(gPresentedIOSurface);
            gPresentedIOSurface = nullptr;
        }
        gFramebufferWidth = width;
        gFramebufferHeight = height;
        gFramebufferStride = rowStrideBytes;
        gFramebufferPixels = std::move(pixels);
    }
    ScheduleMetalPresent();
}

void PSPresentIOSurface(void* surfacePtr,
                        int width,
                        int height,
                        int rowStrideBytes) {
    auto surface = static_cast<IOSurfaceRef>(surfacePtr);
    if (!surface || width <= 0 || height <= 0 || rowStrideBytes <= 0) {
        if (surface) {
            CFRelease(surface);
        }
        return;
    }
    (void)rowStrideBytes;
    {
        std::lock_guard<std::mutex> lock(gFramebufferMutex);
        if (gPresentedIOSurface) {
            CFRelease(gPresentedIOSurface);
            gPresentedIOSurface = nullptr;
        }
        gPresentedIOSurface = surface;
        gIOSurfaceWidth = width;
        gIOSurfaceHeight = height;
        CFRetain(gPresentedIOSurface);
        gFramebufferPixels.reset();
        gFramebufferWidth = 0;
        gFramebufferHeight = 0;
        gFramebufferStride = 0;
    }
    ScheduleMetalPresent();
}

void PSGetLocalWindowContentSize(int* width, int* height) {
    if (!width || !height) {
        return;
    }
    @autoreleasepool {
        if (!gWindow) {
            *width = 0;
            *height = 0;
            return;
        }
        NSView* view = [gWindow contentView];
        if (!view) {
            *width = 0;
            *height = 0;
            return;
        }
        NSWindow* window = gWindow;
        CGFloat scale = window ? window.backingScaleFactor : (CGFloat)1.0;
        if (scale < (CGFloat)1.0) {
            scale = 1.0;
        }
        NSRect bounds = [view bounds];
        *width = static_cast<int>(std::llround(bounds.size.width * scale));
        *height = static_cast<int>(std::llround(bounds.size.height * scale));
    }
}

} // namespace SP

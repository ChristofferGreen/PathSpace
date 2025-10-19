#include <pathspace/ui/LocalWindowBridge.hpp>
#include <pathspace/ui/PathWindowView.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <Cocoa/Cocoa.h>
#include <dispatch/dispatch.h>
#include <Foundation/Foundation.h>
#include <Metal/Metal.h>
#include <QuartzCore/CAMetalLayer.h>
#include <IOSurface/IOSurface.h>
#endif

namespace SP::UI {

#if defined(__APPLE__)
namespace {

struct CallbackState {
    LocalWindowCallbacks callbacks{};
    std::mutex mutex;
};

CallbackState& callback_state() {
    static CallbackState state{};
    return state;
}

void emit_mouse_event(LocalMouseEvent const& ev) {
    auto& state = callback_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.callbacks.mouse_event) {
        state.callbacks.mouse_event(ev, state.callbacks.user_data);
    }
}

void clear_mouse_events() {
    auto& state = callback_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.callbacks.clear_mouse) {
        state.callbacks.clear_mouse(state.callbacks.user_data);
    }
}

struct WindowState {
    NSWindow* window = nil;
    id<MTLDevice> metal_device = nil;
    id<MTLCommandQueue> metal_queue = nil;
    CAMetalLayer* metal_layer = nil;
    std::vector<IOSurfaceRef> iosurface_pool{};
    IOSurfaceRef presented_iosurface = nullptr;
    std::size_t iosurface_pool_index = 0;
    int iosurface_width = 0;
    int iosurface_height = 0;
    std::mutex framebuffer_mutex;
    std::shared_ptr<std::vector<std::uint8_t>> framebuffer_pixels;
    int framebuffer_width = 0;
    int framebuffer_height = 0;
    int framebuffer_stride = 0;
    std::atomic<bool> metal_present_scheduled{false};
    CGFloat backing_scale = 1.0;
    bool metal_available = false;
    int desired_width = 640;
    int desired_height = 360;
    std::string desired_title = "PathSpace Window";
};

WindowState& window_state() {
    static WindowState state{};
    return state;
}

NSWindowDelegate*& global_window_delegate_storage() {
    static NSWindowDelegate* delegate = nil;
    return delegate;
}

NSWindowDelegate* global_window_delegate() {
    return global_window_delegate_storage();
}

void set_global_window_delegate(NSWindowDelegate* delegate) {
    global_window_delegate_storage() = delegate;
}

NSWindowDelegate* get_delegate() {
    return global_window_delegate();
}

void ensure_metal_device(WindowState& state) {
    if (state.metal_device && state.metal_queue) {
        return;
    }
    state.metal_device = MTLCreateSystemDefaultDevice();
    if (!state.metal_device) {
        state.metal_available = false;
        PathWindowView::ResetMetalPresenter();
        return;
    }
    state.metal_queue = [state.metal_device newCommandQueue];
    if (!state.metal_queue) {
        state.metal_device = nil;
        state.metal_available = false;
        PathWindowView::ResetMetalPresenter();
        return;
    }
    state.metal_available = true;
}

bool compute_pixel_coordinates(WindowState& state, NSEvent* ev, int& out_x, int& out_y) {
    NSWindow* window = ev.window ?: state.window;
    if (!window) return false;
    NSView* view = window.contentView;
    if (!view) return false;

    NSPoint in_window = ev.locationInWindow;
    NSPoint local = [view convertPoint:in_window fromView:nil];
    NSRect bounds = view.bounds;

    CGFloat scale = window.backingScaleFactor;
    if (scale < (CGFloat)1.0) {
        scale = 1.0;
    }
    double width_pixels = static_cast<double>(bounds.size.width) * static_cast<double>(scale);
    double height_pixels = static_cast<double>(bounds.size.height) * static_cast<double>(scale);
    if (width_pixels <= 0.0 || height_pixels <= 0.0) {
        return false;
    }

    double px = std::clamp(static_cast<double>(local.x) * static_cast<double>(scale),
                           0.0,
                           width_pixels - 1.0);
    double py = std::clamp(static_cast<double>(local.y) * static_cast<double>(scale),
                           0.0,
                           height_pixels - 1.0);
    double flipped_y = (height_pixels - 1.0) - py;

    out_x = static_cast<int>(std::llround(px));
    out_y = static_cast<int>(std::llround(flipped_y));
    return true;
}

LocalMouseButton button_from_event(NSEvent* ev) {
    switch (ev.type) {
        case NSEventTypeLeftMouseDown:
        case NSEventTypeLeftMouseUp:
            return LocalMouseButton::Left;
        case NSEventTypeRightMouseDown:
        case NSEventTypeRightMouseUp:
            return LocalMouseButton::Right;
        case NSEventTypeOtherMouseDown:
        case NSEventTypeOtherMouseUp:
        default:
            if (ev.buttonNumber == 2) return LocalMouseButton::Middle;
            if (ev.buttonNumber == 3) return LocalMouseButton::Button4;
            if (ev.buttonNumber == 4) return LocalMouseButton::Button5;
            return LocalMouseButton::Middle;
    }
}

void update_presenter_config(WindowState& state) {
    if (!state.metal_available || !state.metal_layer) {
        return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        WindowState& st = window_state();
        CAMetalLayer* layer = st.metal_layer;
        NSWindow* window = st.window;
        if (!layer || !window) {
            return;
        }
        CGFloat scale = window.backingScaleFactor;
        if (scale < (CGFloat)1.0) {
            scale = 1.0;
        }
        st.backing_scale = scale;
        layer.contentsScale = scale;
        CGSize bounds_size = layer.bounds.size;
        layer.drawableSize = CGSizeMake(bounds_size.width * scale,
                                        bounds_size.height * scale);

        PathWindowView::MetalPresenterConfig config{};
        config.layer = (__bridge void*)layer;
        config.device = (__bridge void*)st.metal_device;
        config.command_queue = (__bridge void*)st.metal_queue;
        config.contents_scale = static_cast<double>(scale);
        PathWindowView::ConfigureMetalPresenter(config);
    });
}

void schedule_metal_present(WindowState& state) {
    if (!state.metal_available || !state.metal_layer || !state.metal_queue) {
        if (state.window) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [[state.window contentView] setNeedsDisplay:YES];
            });
        }
        return;
    }
    bool expected = false;
    if (!state.metal_present_scheduled.compare_exchange_strong(expected, true)) {
        return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        WindowState& st = window_state();
        st.metal_present_scheduled.store(false);
        @autoreleasepool {
            CAMetalLayer* layer = st.metal_layer;
            id<MTLCommandQueue> queue = st.metal_queue;
            NSWindow* window = st.window;
            if (!layer || !queue || !window) {
                return;
            }

            std::shared_ptr<std::vector<std::uint8_t>> pixels;
            int width = 0;
            int height = 0;
            int stride = 0;
            IOSurfaceRef shared_surface = nullptr;
            int ios_width = 0;
            int ios_height = 0;
            {
                std::lock_guard<std::mutex> lock(st.framebuffer_mutex);
                pixels = st.framebuffer_pixels;
                width = st.framebuffer_width;
                height = st.framebuffer_height;
                stride = st.framebuffer_stride;
                if (st.presented_iosurface) {
                    shared_surface = st.presented_iosurface;
                    ios_width = st.iosurface_width;
                    ios_height = st.iosurface_height;
                    st.presented_iosurface = nullptr;
                    st.iosurface_width = 0;
                    st.iosurface_height = 0;
                } else {
                    auto const pool_size = st.iosurface_pool.size();
                    if (pool_size > 0) {
                        auto try_count = pool_size;
                        while (try_count--) {
                            auto idx = st.iosurface_pool_index % pool_size;
                            IOSurfaceRef candidate = st.iosurface_pool[idx];
                            st.iosurface_pool_index = (st.iosurface_pool_index + 1) % pool_size;
                            if (!candidate) {
                                continue;
                            }
                            auto candidate_width = IOSurfaceGetWidth(candidate);
                            auto candidate_height = IOSurfaceGetHeight(candidate);
                            if (candidate_width == width && candidate_height == height) {
                                shared_surface = candidate;
                                ios_width = candidate_width;
                                ios_height = candidate_height;
                                st.iosurface_pool[idx] = nullptr;
                                break;
                            }
                        }
                    }
                }
            }

            static NSUInteger present_counter = 0;
            static const NSUInteger recycle_threshold = 120;

            if (shared_surface && ios_width > 0 && ios_height > 0 && st.metal_device) {
                CGFloat scale = window.backingScaleFactor;
                if (scale < (CGFloat)1.0) {
                    scale = 1.0;
                }
                layer.contentsScale = scale;
                layer.drawableSize = CGSizeMake(ios_width, ios_height);

                id<CAMetalDrawable> drawable = [layer nextDrawable];
                if (drawable) {
                    MTLTextureDescriptor* descriptor =
                        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                           width:ios_width
                                                                          height:ios_height
                                                                       mipmapped:NO];
                    descriptor.storageMode = MTLStorageModeShared;
                    id<MTLTexture> source_texture =
                        [st.metal_device newTextureWithDescriptor:descriptor
                                                         iosurface:shared_surface
                                                              plane:0];
                    if (source_texture) {
                        id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
                        id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
                        MTLSize size = MTLSizeMake(ios_width, ios_height, 1);
                        [blit copyFromTexture:source_texture
                                   sourceSlice:0
                                    sourceLevel:0
                                   sourceOrigin:MTLOriginMake(0, 0, 0)
                                     sourceSize:size
                                   toTexture:drawable.texture
                            destinationSlice:0
                             destinationLevel:0
                            destinationOrigin:MTLOriginMake(0, 0, 0)];
                        [blit endEncoding];
                        [command_buffer presentDrawable:drawable];
                        [command_buffer commit];
                        present_counter++;
                        if (present_counter >= recycle_threshold) {
                            CFRelease(shared_surface);
                            present_counter = 0;
                        } else {
                            std::lock_guard<std::mutex> lock(st.framebuffer_mutex);
                            if (st.presented_iosurface) {
                                st.iosurface_pool.push_back(st.presented_iosurface);
                            }
                            st.presented_iosurface = shared_surface;
                            st.iosurface_width = ios_width;
                            st.iosurface_height = ios_height;
                        }
                        source_texture = nil;
                        return;
                    }
                }
                CFRelease(shared_surface);
            } else if (shared_surface) {
                CFRelease(shared_surface);
            }

            if (!pixels || pixels->empty() || width <= 0 || height <= 0 || stride <= 0) {
                return;
            }

            CGFloat scale = window.backingScaleFactor;
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

            id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
            [command_buffer presentDrawable:drawable];
            [command_buffer commit];
        }
    });
}

@interface PSLocalEventView : NSView
@property(nonatomic, strong) CAMetalLayer* psMetalLayer;
- (void)configureMetalLayerIfPossible;
- (void)enqueueMouseEvent:(LocalMouseEvent const&)event;
- (void)handleMoveEvent:(NSEvent *)event;
- (void)pushButtonEvent:(NSEvent *)event type:(LocalMouseEventType)type;
- (void)pushWheelEvent:(NSEvent *)event;
- (void)drawFallback:(NSRect)dirtyRect;
@end

@interface PSLocalWindowDelegate : NSObject <NSWindowDelegate>
@end

@implementation PSLocalEventView

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
    WindowState& state = window_state();
    ensure_metal_device(state);
    if (!state.metal_device) {
        self.wantsLayer = YES;
        state.metal_available = false;
        self.psMetalLayer = nil;
        state.metal_layer = nil;
        PathWindowView::ResetMetalPresenter();
        return;
    }

    self.wantsLayer = YES;
    CAMetalLayer* layer = self.psMetalLayer;
    if (!layer) {
        layer = [CAMetalLayer layer];
    }
    layer.device = state.metal_device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = NO;
    layer.magnificationFilter = kCAFilterNearest;
    layer.minificationFilter = kCAFilterNearest;

    NSWindow* window = self.window ?: state.window;
    CGFloat scale = window ? window.backingScaleFactor : (CGFloat)1.0;
    if (scale < (CGFloat)1.0) {
        scale = 1.0;
    }
    state.backing_scale = scale;
    layer.contentsScale = scale;
    layer.drawableSize = CGSizeMake(self.bounds.size.width * scale,
                                    self.bounds.size.height * scale);

    self.layer = layer;
    self.psMetalLayer = layer;
    state.metal_layer = layer;
    state.metal_available = true;

    PathWindowView::MetalPresenterConfig config{};
    config.layer = (__bridge void*)layer;
    config.device = (__bridge void*)state.metal_device;
    config.command_queue = (__bridge void*)state.metal_queue;
    config.contents_scale = static_cast<double>(scale);
    PathWindowView::ConfigureMetalPresenter(config);
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (self.window) {
        [self.window setAcceptsMouseMovedEvents:YES];
        [self.window makeFirstResponder:self];
    }
    [self configureMetalLayerIfPossible];
    update_presenter_config(window_state());
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    update_presenter_config(window_state());
}

- (void)enqueueMouseEvent:(LocalMouseEvent const&)event {
    emit_mouse_event(event);
}

- (void)handleMoveEvent:(NSEvent *)event {
    WindowState& state = window_state();
    int xi = 0;
    int yi = 0;
    bool has_coords = compute_pixel_coordinates(state, event, xi, yi);

    LocalMouseEvent rel{};
    rel.type = LocalMouseEventType::Move;
    rel.dx = static_cast<int>(event.deltaX);
    rel.dy = static_cast<int>(event.deltaY);
    if (has_coords) {
        rel.x = xi;
        rel.y = yi;
    }
    [self enqueueMouseEvent:rel];

    if (has_coords) {
        LocalMouseEvent abs{};
        abs.type = LocalMouseEventType::AbsoluteMove;
        abs.x = xi;
        abs.y = yi;
        [self enqueueMouseEvent:abs];
    }
}

- (void)mouseMoved:(NSEvent *)event { [self handleMoveEvent:event]; }
- (void)mouseDragged:(NSEvent *)event { [self handleMoveEvent:event]; }
- (void)rightMouseDragged:(NSEvent *)event { [self handleMoveEvent:event]; }
- (void)otherMouseDragged:(NSEvent *)event { [self handleMoveEvent:event]; }

- (void)scrollWheel:(NSEvent *)event {
    [self pushWheelEvent:event];
}

- (void)pushWheelEvent:(NSEvent *)event {
    WindowState& state = window_state();
    CGFloat dy = event.hasPreciseScrollingDeltas ? event.scrollingDeltaY : event.deltaY;
    int ticks = static_cast<int>(std::llround(dy));
    if (ticks == 0) {
        return;
    }
    LocalMouseEvent ev{};
    ev.type = LocalMouseEventType::Wheel;
    ev.wheel = ticks;
    int xi = 0;
    int yi = 0;
    if (compute_pixel_coordinates(state, event, xi, yi)) {
        ev.x = xi;
        ev.y = yi;
    }
    [self enqueueMouseEvent:ev];
}

- (void)mouseDown:(NSEvent *)event { [self pushButtonEvent:event type:LocalMouseEventType::ButtonDown]; }
- (void)mouseUp:(NSEvent *)event { [self pushButtonEvent:event type:LocalMouseEventType::ButtonUp]; }
- (void)rightMouseDown:(NSEvent *)event { [self mouseDown:event]; }
- (void)rightMouseUp:(NSEvent *)event { [self mouseUp:event]; }
- (void)otherMouseDown:(NSEvent *)event { [self mouseDown:event]; }
- (void)otherMouseUp:(NSEvent *)event { [self mouseUp:event]; }

- (void)pushButtonEvent:(NSEvent *)event type:(LocalMouseEventType)type {
    WindowState& state = window_state();
    LocalMouseEvent ev{};
    ev.type = type;
    ev.button = button_from_event(event);
    int xi = 0;
    int yi = 0;
    if (compute_pixel_coordinates(state, event, xi, yi)) {
        ev.x = xi;
        ev.y = yi;
    }
    [self enqueueMouseEvent:ev];
}

- (void)drawRect:(NSRect)dirtyRect {
    [super drawRect:dirtyRect];
    if (window_state().metal_available) {
        return;
    }
    [self drawFallback:dirtyRect];
}

- (void)drawFallback:(NSRect)dirtyRect {
    WindowState& state = window_state();
    std::shared_ptr<std::vector<std::uint8_t>> pixels;
    int width = 0;
    int height = 0;
    int stride = 0;
    {
        std::lock_guard<std::mutex> lock(state.framebuffer_mutex);
        pixels = state.framebuffer_pixels;
        width = state.framebuffer_width;
        height = state.framebuffer_height;
        stride = state.framebuffer_stride;
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

@implementation PSLocalWindowDelegate
- (void)windowWillClose:(NSNotification *)notification {
    (void)notification;
    WindowState& state = window_state();
    state.metal_available = false;
    state.metal_layer = nil;
    state.metal_queue = nil;
    state.metal_device = nil;
    state.metal_present_scheduled.store(false);
    clear_mouse_events();
    {
        std::lock_guard<std::mutex> lock(state.framebuffer_mutex);
        state.framebuffer_pixels.reset();
        state.framebuffer_width = 0;
        state.framebuffer_height = 0;
        state.framebuffer_stride = 0;
    }
    state.window = nil;
    set_global_window_delegate(nil);
}
@end

#endif // defined(__APPLE__)

void SetLocalWindowCallbacks(LocalWindowCallbacks const& callbacks) {
#if defined(__APPLE__)
    auto& state = callback_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.callbacks = callbacks;
#else
    (void)callbacks;
#endif
}

void ConfigureLocalWindow(int width, int height, char const* title) {
#if defined(__APPLE__)
    WindowState& state = window_state();
    state.desired_width = width;
    state.desired_height = height;
    if (title) {
        state.desired_title = title;
    }
    if (state.window) {
        dispatch_async(dispatch_get_main_queue(), ^{
            WindowState& st = window_state();
            NSRect frame = [st.window frame];
            frame.size = NSMakeSize(st.desired_width, st.desired_height);
            [st.window setFrame:frame display:YES animate:NO];
            st.window.title = [NSString stringWithUTF8String:st.desired_title.c_str()];
            [[st.window contentView] setFrame:NSMakeRect(0, 0, st.desired_width, st.desired_height)];
            [[st.window contentView] setNeedsDisplay:YES];
            update_presenter_config(st);
        });
    }
#else
    (void)width;
    (void)height;
    (void)title;
#endif
}

void InitLocalWindow() {
    InitLocalWindowWithSize(0, 0, nullptr);
}

void InitLocalWindowWithSize(int width, int height, char const* title) {
#if defined(__APPLE__)
    WindowState& state = window_state();
    if (width > 0) {
        state.desired_width = width;
    }
    if (height > 0) {
        state.desired_height = height;
    }
    if (title) {
        state.desired_title = title;
    }

    @autoreleasepool {
        ensure_metal_device(state);
        NSApplication* app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        if (!state.window) {
            NSRect frame = NSMakeRect(200, 200, state.desired_width, state.desired_height);
            state.window = [[NSWindow alloc] initWithContentRect:frame
                                                       styleMask:(NSWindowStyleMaskTitled |
                                                                  NSWindowStyleMaskClosable |
                                                                  NSWindowStyleMaskMiniaturizable |
                                                                  NSWindowStyleMaskResizable)
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
            state.window.title = [NSString stringWithUTF8String:state.desired_title.c_str()];
            auto* delegate = [[PSLocalWindowDelegate alloc] init];
            state.window.delegate = delegate;
            set_global_window_delegate(delegate);
            auto* view = [[PSLocalEventView alloc] initWithFrame:NSMakeRect(0, 0, state.desired_width, state.desired_height)];
            [state.window setContentView:view];
            [state.window setAcceptsMouseMovedEvents:YES];
            [state.window makeFirstResponder:view];
            [state.window makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
            update_presenter_config(state);
        } else {
            [[state.window contentView] setFrame:NSMakeRect(0, 0, state.desired_width, state.desired_height)];
            state.window.title = [NSString stringWithUTF8String:state.desired_title.c_str()];
            auto* delegate = get_delegate();
            if (!delegate) {
                delegate = [[PSLocalWindowDelegate alloc] init];
            }
            state.window.delegate = delegate;
            [state.window makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
            update_presenter_config(state);
        }
    }
#else
    (void)width;
    (void)height;
    (void)title;
#endif
}

void PollLocalWindow() {
#if defined(__APPLE__)
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
#endif
}

void PresentLocalWindowFramebuffer(std::uint8_t const* data,
                                   int width,
                                   int height,
                                   int row_stride_bytes) {
#if defined(__APPLE__)
    if (!data || width <= 0 || height <= 0 || row_stride_bytes <= 0) {
        return;
    }
    WindowState& state = window_state();
    auto pixels = std::make_shared<std::vector<std::uint8_t>>(
        data,
        data + static_cast<std::size_t>(row_stride_bytes) * static_cast<std::size_t>(height));
    {
        std::lock_guard<std::mutex> lock(state.framebuffer_mutex);
        if (state.presented_iosurface) {
            state.iosurface_pool.push_back(state.presented_iosurface);
            state.presented_iosurface = nullptr;
        }
        state.framebuffer_width = width;
        state.framebuffer_height = height;
        state.framebuffer_stride = row_stride_bytes;
        state.framebuffer_pixels = std::move(pixels);
    }
    schedule_metal_present(state);
#else
    (void)data;
    (void)width;
    (void)height;
    (void)row_stride_bytes;
#endif
}

void PresentLocalWindowIOSurface(void* surface,
                                 int width,
                                 int height,
                                 int row_stride_bytes) {
#if defined(__APPLE__)
    auto surface_ref = static_cast<IOSurfaceRef>(surface);
    if (!surface_ref || width <= 0 || height <= 0 || row_stride_bytes <= 0) {
        if (surface_ref) {
            CFRelease(surface_ref);
        }
        return;
    }
    (void)row_stride_bytes;
    WindowState& state = window_state();
    {
        std::lock_guard<std::mutex> lock(state.framebuffer_mutex);
        if (state.presented_iosurface) {
            CFRelease(state.presented_iosurface);
            state.presented_iosurface = nullptr;
        }
        state.presented_iosurface = surface_ref;
        state.iosurface_width = width;
        state.iosurface_height = height;
        CFRetain(state.presented_iosurface);
        state.framebuffer_pixels.reset();
        state.framebuffer_width = 0;
        state.framebuffer_height = 0;
        state.framebuffer_stride = 0;
    }
    schedule_metal_present(state);
#else
    (void)surface;
    (void)width;
    (void)height;
    (void)row_stride_bytes;
#endif
}

void GetLocalWindowContentSize(int* width, int* height) {
#if defined(__APPLE__)
    if (!width || !height) {
        return;
    }
    @autoreleasepool {
        WindowState& state = window_state();
        if (!state.window) {
            *width = 0;
            *height = 0;
            return;
        }
        NSView* view = [state.window contentView];
        if (!view) {
            *width = 0;
            *height = 0;
            return;
        }
        CGFloat scale = state.window ? state.window.backingScaleFactor : (CGFloat)1.0;
        if (scale < (CGFloat)1.0) {
            scale = 1.0;
        }
        NSRect bounds = [view bounds];
        *width = static_cast<int>(std::llround(bounds.size.width * scale));
        *height = static_cast<int>(std::llround(bounds.size.height * scale));
    }
#else
    if (width) *width = 0;
    if (height) *height = 0;
#endif
}

} // namespace SP::UI

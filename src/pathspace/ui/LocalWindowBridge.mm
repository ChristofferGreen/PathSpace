#include <pathspace/ui/LocalWindowBridge.hpp>
#include <pathspace/ui/PathWindowView.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <Cocoa/Cocoa.h>
#include <dispatch/dispatch.h>
#include <Foundation/Foundation.h>
#include <IOSurface/IOSurface.h>
#include <Metal/Metal.h>
#include <QuartzCore/CAMetalLayer.h>
#include <ImageIO/ImageIO.h>
#include <CoreServices/CoreServices.h>
#endif

#if defined(__APPLE__)
@class PSLocalWindowDelegate;
using SP::UI::LocalMouseButton;
using SP::UI::LocalMouseEvent;
using SP::UI::LocalMouseEventType;
using SP::UI::LocalWindowCallbacks;
using SP::UI::LocalKeyEvent;
using SP::UI::LocalKeyEventType;
using SP::UI::LocalKeyModifierAlt;
using SP::UI::LocalKeyModifierCapsLock;
using SP::UI::LocalKeyModifierCommand;
using SP::UI::LocalKeyModifierControl;
using SP::UI::LocalKeyModifierFunction;
using SP::UI::LocalKeyModifierShift;
#endif

namespace {

auto quit_flag() -> std::atomic<bool>& {
    static std::atomic<bool> flag{false};
    return flag;
}

#if defined(__APPLE__)

struct CallbackState {
    LocalWindowCallbacks callbacks{};
    std::mutex mutex;
};

auto callback_state() -> CallbackState& {
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

unsigned int modifier_mask_from_flags(NSEventModifierFlags flags) {
    unsigned int mask = 0;
    if (flags & NSEventModifierFlagShift) {
        mask |= LocalKeyModifierShift;
    }
    if (flags & NSEventModifierFlagControl) {
        mask |= LocalKeyModifierControl;
    }
    if (flags & NSEventModifierFlagOption) {
        mask |= LocalKeyModifierAlt;
    }
    if (flags & NSEventModifierFlagCommand) {
        mask |= LocalKeyModifierCommand;
    }
    if (flags & NSEventModifierFlagFunction) {
        mask |= LocalKeyModifierFunction;
    }
    if (flags & NSEventModifierFlagCapsLock) {
        mask |= LocalKeyModifierCapsLock;
    }
    return mask;
}

auto make_local_key_event(NSEvent* event, LocalKeyEventType type) -> LocalKeyEvent {
    LocalKeyEvent ev{};
    ev.type = type;
    ev.keycode = static_cast<unsigned int>(event.keyCode);
    ev.modifiers = modifier_mask_from_flags(event.modifierFlags);
    ev.repeat = event.isARepeat;
    NSString* chars = event.charactersIgnoringModifiers;
    if (chars.length > 0) {
        unichar ch = [chars characterAtIndex:0];
        ev.character = static_cast<char32_t>(ch);
    }
    return ev;
}

auto is_quit_shortcut(LocalKeyEvent const& ev) -> bool {
    if (ev.type != LocalKeyEventType::KeyDown) {
        return false;
    }
    constexpr unsigned int kKeycodeQ = 0x0C;
    constexpr unsigned int kKeycodeF4 = 0x76;
    bool command = (ev.modifiers & LocalKeyModifierCommand) != 0u;
    bool control = (ev.modifiers & LocalKeyModifierControl) != 0u;
    bool alt = (ev.modifiers & LocalKeyModifierAlt) != 0u;
    bool is_q_keycode = ev.keycode == kKeycodeQ;
    bool is_q_character = ev.character == U'Q' || ev.character == U'q';
    bool is_q = is_q_keycode || is_q_character;
    bool command_q = command && is_q;
    bool control_q = control && is_q;
    bool alt_f4 = alt && (ev.keycode == kKeycodeF4);
    return command_q || control_q || alt_f4;
}

void emit_key_event(LocalKeyEvent const& ev) {
    if (is_quit_shortcut(ev)) {
        if (!quit_flag().load(std::memory_order_acquire)) {
            SP::UI::RequestLocalWindowQuit();
        }
    }
    auto& state = callback_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.callbacks.key_event) {
        state.callbacks.key_event(ev, state.callbacks.user_data);
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
    PSLocalWindowDelegate* window_delegate = nil;
    std::atomic<bool> live_resize_active{false};
    std::atomic<bool> pending_present_after_resize{false};
};

auto window_state() -> WindowState& {
    static WindowState state{};
    return state;
}

void ensure_metal_device(WindowState& state) {
    if (state.metal_device && state.metal_queue) {
        return;
    }
    state.metal_device = MTLCreateSystemDefaultDevice();
    if (!state.metal_device) {
        state.metal_available = false;
        SP::UI::PathWindowView::ResetMetalPresenter();
        return;
    }
    state.metal_queue = [state.metal_device newCommandQueue];
    if (!state.metal_queue) {
        state.metal_device = nil;
        state.metal_available = false;
        SP::UI::PathWindowView::ResetMetalPresenter();
        return;
    }
    state.metal_available = true;
}

auto compute_pixel_coordinates(WindowState& state, NSEvent* ev, int& out_x, int& out_y) -> bool {
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

auto button_from_event(NSEvent* ev) -> LocalMouseButton {
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

        SP::UI::PathWindowView::MetalPresenterConfig config{};
        config.layer = (__bridge void*)layer;
        config.device = (__bridge void*)st.metal_device;
        config.command_queue = (__bridge void*)st.metal_queue;
        config.contents_scale = static_cast<double>(scale);
        SP::UI::PathWindowView::ConfigureMetalPresenter(config);
    });
}

void schedule_metal_present(WindowState& state);

void clear_presenter_surfaces(WindowState& state) {
    std::lock_guard<std::mutex> lock(state.framebuffer_mutex);
    for (auto* surface : state.iosurface_pool) {
        if (surface) {
            CFRelease(surface);
        }
    }
    state.iosurface_pool.clear();
    state.iosurface_pool_index = 0;
    if (state.presented_iosurface) {
        CFRelease(state.presented_iosurface);
        state.presented_iosurface = nullptr;
    }
    state.iosurface_width = 0;
    state.iosurface_height = 0;
}

void begin_resize_block(WindowState& state) {
    state.live_resize_active.store(true, std::memory_order_release);
    state.pending_present_after_resize.store(true, std::memory_order_release);
    state.metal_present_scheduled.store(false);
    clear_presenter_surfaces(state);
}

void end_resize_block(WindowState& state) {
    state.live_resize_active.store(false, std::memory_order_release);
    update_presenter_config(state);
    if (state.pending_present_after_resize.exchange(false, std::memory_order_acq_rel)) {
        schedule_metal_present(state);
    }
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
    if (state.live_resize_active.load(std::memory_order_acquire)) {
        state.pending_present_after_resize.store(true, std::memory_order_release);
        state.metal_present_scheduled.store(false);
        return;
    }
    bool expected = false;
    if (!state.metal_present_scheduled.compare_exchange_strong(expected, true)) {
        return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        WindowState& st = window_state();
        st.pending_present_after_resize.store(false, std::memory_order_release);
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

#endif // defined(__APPLE__)

} // namespace

#if defined(__APPLE__)

@interface PSLocalEventView : NSView
@property(nonatomic, strong) CAMetalLayer* psMetalLayer;
- (void)configureMetalLayerIfPossible;
- (void)handleMoveEvent:(NSEvent *)event;
- (void)pushButtonEvent:(NSEvent *)event type:(LocalMouseEventType)type;
- (void)pushWheelEvent:(NSEvent *)event;
- (void)drawFallback:(NSRect)dirtyRect;
@end

@interface PSLocalWindowDelegate : NSObject <NSWindowDelegate>
@end

@implementation PSLocalEventView

- (BOOL)acceptsFirstResponder {
    return YES;
}

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
        SP::UI::PathWindowView::ResetMetalPresenter();
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

    SP::UI::PathWindowView::MetalPresenterConfig config{};
    config.layer = (__bridge void*)layer;
    config.device = (__bridge void*)state.metal_device;
    config.command_queue = (__bridge void*)state.metal_queue;
    config.contents_scale = static_cast<double>(scale);
    SP::UI::PathWindowView::ConfigureMetalPresenter(config);
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
    emit_mouse_event(rel);

    if (has_coords) {
        LocalMouseEvent abs{};
        abs.type = LocalMouseEventType::AbsoluteMove;
        abs.x = xi;
        abs.y = yi;
        emit_mouse_event(abs);
    }
}

- (void)mouseMoved:(NSEvent *)event { [self handleMoveEvent:event]; }
- (void)mouseDragged:(NSEvent *)event { [self handleMoveEvent:event]; }
- (void)rightMouseDragged:(NSEvent *)event { [self handleMoveEvent:event]; }
- (void)otherMouseDragged:(NSEvent *)event { [self handleMoveEvent:event]; }

- (void)scrollWheel:(NSEvent *)event {
    [self pushWheelEvent:event];
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
    emit_mouse_event(ev);
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
    emit_mouse_event(ev);
}

- (void)keyDown:(NSEvent *)event {
    emit_key_event(make_local_key_event(event, LocalKeyEventType::KeyDown));
}

- (void)keyUp:(NSEvent *)event {
    emit_key_event(make_local_key_event(event, LocalKeyEventType::KeyUp));
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
- (void)windowWillStartLiveResize:(NSNotification *)notification {
    (void)notification;
    begin_resize_block(window_state());
}

- (void)windowDidEndLiveResize:(NSNotification *)notification {
    (void)notification;
    end_resize_block(window_state());
}

- (void)windowWillEnterFullScreen:(NSNotification *)notification {
    (void)notification;
    begin_resize_block(window_state());
}

- (void)windowDidEnterFullScreen:(NSNotification *)notification {
    (void)notification;
    end_resize_block(window_state());
}

- (void)windowWillExitFullScreen:(NSNotification *)notification {
    (void)notification;
    begin_resize_block(window_state());
}

- (void)windowDidExitFullScreen:(NSNotification *)notification {
    (void)notification;
    end_resize_block(window_state());
}

- (void)windowWillClose:(NSNotification *)notification {
    (void)notification;
    WindowState& state = window_state();
    clear_presenter_surfaces(state);
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
    state.window_delegate = nil;
    quit_flag().store(true, std::memory_order_release);
}
@end

#endif // defined(__APPLE__)

namespace SP::UI {

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
    quit_flag().store(false, std::memory_order_release);
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
            // Align on-screen window visuals with the app's dark framebuffer theme.
            state.window.styleMask |= NSWindowStyleMaskFullSizeContentView;
            [state.window setTitlebarAppearsTransparent:YES];
            state.window.titleVisibility = NSWindowTitleHidden;
            state.window.backgroundColor = [NSColor colorWithCalibratedRed:(CGFloat)(28.0 / 255.0)
                                                                     green:(CGFloat)(31.0 / 255.0)
                                                                      blue:(CGFloat)(38.0 / 255.0)
                                                                     alpha:1.0];
            state.window.title = [NSString stringWithUTF8String:state.desired_title.c_str()];
            PSLocalWindowDelegate* delegate = [[PSLocalWindowDelegate alloc] init];
            state.window.delegate = delegate;
            state.window_delegate = delegate;
            PSLocalEventView* view = [[PSLocalEventView alloc] initWithFrame:NSMakeRect(0, 0, state.desired_width, state.desired_height)];
            [state.window setContentView:view];
            [state.window setAcceptsMouseMovedEvents:YES];
            [state.window makeFirstResponder:view];
            [state.window makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
            update_presenter_config(state);
        } else {
            [[state.window contentView] setFrame:NSMakeRect(0, 0, state.desired_width, state.desired_height)];
            state.window.title = [NSString stringWithUTF8String:state.desired_title.c_str()];
            PSLocalWindowDelegate* delegate = state.window_delegate;
            if (!delegate) {
                delegate = [[PSLocalWindowDelegate alloc] init];
                state.window_delegate = delegate;
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

auto GetLocalWindowNumber() -> unsigned int {
#if defined(__APPLE__)
    @autoreleasepool {
        WindowState& state = window_state();
        if (state.window) {
            return static_cast<unsigned int>([state.window windowNumber]);
        }
    }
#endif
    return 0U;
}

void RequestLocalWindowQuit() {
    quit_flag().store(true, std::memory_order_release);
#if defined(__APPLE__)
    @autoreleasepool {
        WindowState& state = window_state();
        if (state.window) {
            dispatch_async(dispatch_get_main_queue(), ^{
                WindowState& st = window_state();
                if (st.window) {
                    [st.window performClose:nil];
                }
            });
        }
    }
#endif
}

auto LocalWindowQuitRequested() -> bool {
    return quit_flag().load(std::memory_order_acquire);
}

void ClearLocalWindowQuitRequest() {
    quit_flag().store(false, std::memory_order_release);
}

auto SaveLocalWindowScreenshot(char const* path) -> bool {
#if defined(__APPLE__)
    if (!path || path[0] == '\0') {
        return false;
    }
    @autoreleasepool {
        WindowState& state = window_state();
#if defined(__MAC_OS_X_VERSION_MIN_REQUIRED) && __MAC_OS_X_VERSION_MIN_REQUIRED < 150000
        if (state.window) {
            CGRect bounds = [state.window frame];
            CGImageRef image = CGWindowListCreateImage(bounds,
                                                       kCGWindowListOptionIncludingWindow,
                                                       static_cast<CGWindowID>([state.window windowNumber]),
                                                       kCGWindowImageBoundsIgnoreFraming | kCGWindowImageNominalResolution);
            if (image) {
                CFStringRef type = CFSTR("public.png");
                CFURLRef url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
                                                                       reinterpret_cast<const UInt8*>(path),
                                                                       static_cast<CFIndex>(std::strlen(path)),
                                                                       false);
                if (url) {
                    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, type, 1, nullptr);
                    if (dest) {
                        CGImageDestinationAddImage(dest, image, nullptr);
                        bool ok = CGImageDestinationFinalize(dest);
                        CFRelease(dest);
                        CFRelease(url);
                        CGImageRelease(image);
                        return ok;
                    }
                    CFRelease(url);
                }
                CGImageRelease(image);
            }
        }
#endif
    }
    @autoreleasepool {
        WindowState& state = window_state();
        std::vector<std::uint8_t> raster;
        int width = 0;
        int height = 0;
        int stride = 0;
        IOSurfaceRef surface = nullptr;
        {
            std::lock_guard<std::mutex> lock(state.framebuffer_mutex);
            if (state.framebuffer_pixels && !state.framebuffer_pixels->empty()
                && state.framebuffer_width > 0 && state.framebuffer_height > 0 && state.framebuffer_stride > 0) {
                stride = state.framebuffer_stride;
                width = state.framebuffer_width;
                height = state.framebuffer_height;
                raster.assign(state.framebuffer_pixels->begin(), state.framebuffer_pixels->end());
            } else if (state.presented_iosurface && state.iosurface_width > 0 && state.iosurface_height > 0) {
                surface = state.presented_iosurface;
                CFRetain(surface);
            }
        }

        if (surface) {
            IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr);
            auto* base = static_cast<std::uint8_t*>(IOSurfaceGetBaseAddress(surface));
            size_t row_bytes = IOSurfaceGetBytesPerRow(surface);
            width = static_cast<int>(IOSurfaceGetWidth(surface));
            height = static_cast<int>(IOSurfaceGetHeight(surface));
            stride = static_cast<int>(row_bytes);
            if (base && width > 0 && height > 0 && row_bytes >= static_cast<size_t>(width) * 4) {
                raster.resize(row_bytes * static_cast<size_t>(height));
                std::memcpy(raster.data(), base, raster.size());
            }
            IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
            CFRelease(surface);
        }

        if (raster.empty() || width <= 0 || height <= 0 || stride <= 0) {
            return false;
        }

        CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
        if (!colorSpace) {
            return false;
        }

        CGDataProviderRef provider = CGDataProviderCreateWithData(nullptr,
                                                                  raster.data(),
                                                                  raster.size(),
                                                                  nullptr);
        if (!provider) {
            CGColorSpaceRelease(colorSpace);
            return false;
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
        if (!image) {
            CGDataProviderRelease(provider);
            CGColorSpaceRelease(colorSpace);
            return false;
        }

        CFStringRef path_string = CFStringCreateWithCString(nullptr, path, kCFStringEncodingUTF8);
        if (!path_string) {
            CGImageRelease(image);
            CGDataProviderRelease(provider);
            CGColorSpaceRelease(colorSpace);
            return false;
        }
        CFURLRef url = CFURLCreateWithFileSystemPath(nullptr, path_string, kCFURLPOSIXPathStyle, false);
        CFRelease(path_string);
        if (!url) {
            CGImageRelease(image);
            CGDataProviderRelease(provider);
            CGColorSpaceRelease(colorSpace);
            return false;
        }
        CFStringRef uti = kUTTypePNG;
        CGImageDestinationRef destination = CGImageDestinationCreateWithURL(url, uti, 1, nullptr);
        if (!destination) {
            CFRelease(url);
            CGImageRelease(image);
            CGDataProviderRelease(provider);
            CGColorSpaceRelease(colorSpace);
            return false;
        }

        CGImageDestinationAddImage(destination, image, nullptr);
        bool success = CGImageDestinationFinalize(destination);

        CGImageRelease(image);
        CGDataProviderRelease(provider);
        CGColorSpaceRelease(colorSpace);
        CFRelease(destination);
        CFRelease(url);
        return success;
    }
#else
    (void)path;
    return false;
#endif
}

} // namespace SP::UI

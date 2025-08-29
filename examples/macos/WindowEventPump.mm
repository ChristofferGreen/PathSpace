#import <Cocoa/Cocoa.h>

// Objective-C++ bridge: forward local window events to PathSpace providers.
#include <pathspace/layer/io/PathIOMouse.hpp>
#include <pathspace/layer/io/PathIOKeyboard.hpp>

using SP::PathIOMouse;
using SP::PathIOKeyboard;

// Globals for simple wiring (no threads, polled from user code)
static PathIOMouse*    gMouseProvider    = nullptr;
static PathIOKeyboard* gKeyboardProvider = nullptr;
static NSWindow*       gWindow           = nil;

// Forward declarations
static uint32_t PSModifiersFrom(NSEventModifierFlags flags);
static SP::MouseButton PSButtonFrom(NSEvent* ev);
static void PSEmitAbsoluteIfNeeded(NSEvent* ev);

// NSView subclass to capture mouse/keyboard events
@interface PSEventView : NSView
@end

@implementation PSEventView

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { (void)event; return YES; }
- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (self.window) {
        [self.window setAcceptsMouseMovedEvents:YES];
        [self.window makeFirstResponder:self];
    }
}

- (void)mouseMoved:(NSEvent *)event {
    if (gMouseProvider) {
        SP::PathIOMouse::Event ev{};
        ev.type = SP::MouseEventType::Move;
        ev.dx = (int)event.deltaX;
        ev.dy = (int)event.deltaY;
        gMouseProvider->insert<"/events">(ev);
        PSEmitAbsoluteIfNeeded(event);
    }
}
- (void)mouseDragged:(NSEvent *)event {
    if (gMouseProvider) {
        SP::PathIOMouse::Event ev{};
        ev.type = SP::MouseEventType::Move;
        ev.dx = (int)event.deltaX;
        ev.dy = (int)event.deltaY;
        gMouseProvider->insert<"/events">(ev);
        PSEmitAbsoluteIfNeeded(event);
    }
}
- (void)rightMouseDragged:(NSEvent *)event {
    if (gMouseProvider) {
        SP::PathIOMouse::Event ev{};
        ev.type = SP::MouseEventType::Move;
        ev.dx = (int)event.deltaX;
        ev.dy = (int)event.deltaY;
        gMouseProvider->insert<"/events">(ev);
        PSEmitAbsoluteIfNeeded(event);
    }
}
- (void)otherMouseDragged:(NSEvent *)event {
    if (gMouseProvider) {
        SP::PathIOMouse::Event ev{};
        ev.type = SP::MouseEventType::Move;
        ev.dx = (int)event.deltaX;
        ev.dy = (int)event.deltaY;
        gMouseProvider->insert<"/events">(ev);
        PSEmitAbsoluteIfNeeded(event);
    }
}
- (void)scrollWheel:(NSEvent *)event {
    if (gMouseProvider) {
        // Quantize to integer ticks
        CGFloat dy = event.hasPreciseScrollingDeltas ? event.scrollingDeltaY : event.deltaY;
        int ticks = (int)llround(dy);
        if (ticks != 0) {
            SP::PathIOMouse::Event ev{};
            ev.type = SP::MouseEventType::Wheel;
            ev.wheel = ticks;
            gMouseProvider->insert<"/events">(ev);
        }
    }
}
- (void)mouseDown:(NSEvent *)event {
    if (gMouseProvider) {
        SP::PathIOMouse::Event ev{};
        ev.type = SP::MouseEventType::ButtonDown;
        ev.button = PSButtonFrom(event);
        gMouseProvider->insert<"/events">(ev);
    }
}
- (void)mouseUp:(NSEvent *)event {
    if (gMouseProvider) {
        SP::PathIOMouse::Event ev{};
        ev.type = SP::MouseEventType::ButtonUp;
        ev.button = PSButtonFrom(event);
        gMouseProvider->insert<"/events">(ev);
    }
}
- (void)rightMouseDown:(NSEvent *)event {
    if (gMouseProvider) {
        SP::PathIOMouse::Event ev{};
        ev.type = SP::MouseEventType::ButtonDown;
        ev.button = PSButtonFrom(event);
        gMouseProvider->insert<"/events">(ev);
    }
}
- (void)rightMouseUp:(NSEvent *)event {
    if (gMouseProvider) {
        SP::PathIOMouse::Event ev{};
        ev.type = SP::MouseEventType::ButtonUp;
        ev.button = PSButtonFrom(event);
        gMouseProvider->insert<"/events">(ev);
    }
}
- (void)otherMouseDown:(NSEvent *)event {
    if (gMouseProvider) {
        SP::PathIOMouse::Event ev{};
        ev.type = SP::MouseEventType::ButtonDown;
        ev.button = PSButtonFrom(event);
        gMouseProvider->insert<"/events">(ev);
    }
}
- (void)otherMouseUp:(NSEvent *)event {
    if (gMouseProvider) {
        SP::PathIOMouse::Event ev{};
        ev.type = SP::MouseEventType::ButtonUp;
        ev.button = PSButtonFrom(event);
        gMouseProvider->insert<"/events">(ev);
    }
}

- (void)keyDown:(NSEvent *)event {
    if (gKeyboardProvider) {
        SP::PathIOKeyboard::Event ev{};
        ev.type = SP::KeyEventType::KeyDown;
        ev.keycode = (int)event.keyCode;
        ev.modifiers = PSModifiersFrom(event.modifierFlags);
        gKeyboardProvider->insert<"/events">(ev);
    }
    // Ask Cocoa to produce insertText: for text composition
    [self interpretKeyEvents:@[event]];
}
- (void)keyUp:(NSEvent *)event {
    if (gKeyboardProvider) {
        SP::PathIOKeyboard::Event ev{};
        ev.type = SP::KeyEventType::KeyUp;
        ev.keycode = (int)event.keyCode;
        ev.modifiers = PSModifiersFrom(event.modifierFlags);
        gKeyboardProvider->insert<"/events">(ev);
    }
}
- (void)flagsChanged:(NSEvent *)event {
    // Could emit modifier-only changes if needed; no-op for now.
    (void)event;
}

// Text input (called by interpretKeyEvents:)
- (void)insertText:(id)insertString {
    if (!gKeyboardProvider) return;

    NSString* s = nil;
    if ([insertString isKindOfClass:[NSAttributedString class]]) {
        s = [insertString string];
    } else if ([insertString isKindOfClass:[NSString class]]) {
        s = (NSString*)insertString;
    }
    if (!s) return;

    // Grab current modifiers from the current event if available
    NSEvent* cur = NSApp.currentEvent;
    uint32_t mods = cur ? PSModifiersFrom(cur.modifierFlags) : SP::Mod_None;

    // Forward UTF-8 text
    @autoreleasepool {
        const char* utf8 = [s UTF8String];
        if (utf8) {
            gKeyboardProvider->simulateText(std::string(utf8), mods, /*deviceId=*/0);
        }
    }
}

@end

// Helpers

static uint32_t PSModifiersFrom(NSEventModifierFlags flags) {
    uint32_t m = SP::Mod_None;
    if (flags & NSEventModifierFlagShift)   m |= SP::Mod_Shift;
    if (flags & NSEventModifierFlagControl) m |= SP::Mod_Ctrl;
    if (flags & NSEventModifierFlagOption)  m |= SP::Mod_Alt;
    if (flags & NSEventModifierFlagCommand) m |= SP::Mod_Meta;
    return m;
}

static SP::MouseButton PSButtonFrom(NSEvent* ev) {
    switch (ev.type) {
        case NSEventTypeLeftMouseDown:
        case NSEventTypeLeftMouseUp:   return SP::MouseButton::Left;
        case NSEventTypeRightMouseDown:
        case NSEventTypeRightMouseUp:  return SP::MouseButton::Right;
        case NSEventTypeOtherMouseDown:
        case NSEventTypeOtherMouseUp:
        default:
            // Map common middle button
            if (ev.buttonNumber == 2) return SP::MouseButton::Middle;
            if (ev.buttonNumber == 3) return SP::MouseButton::Button4;
            if (ev.buttonNumber == 4) return SP::MouseButton::Button5;
            return SP::MouseButton::Middle;
    }
}

static void PSEmitAbsoluteIfNeeded(NSEvent* ev) {
    if (!gMouseProvider) return;
    NSWindow* w = ev.window ?: gWindow;
    if (!w) return;
    NSView* v = w.contentView;
    if (!v) return;
    NSPoint pInWin = ev.locationInWindow;
    NSPoint p      = [v convertPoint:pInWin fromView:nil];
    // Flip Y to a more conventional top-left origin if desired; here we keep native view coords.
    int xi = (int)llround(p.x);
    int yi = (int)llround(p.y);
    SP::PathIOMouse::Event mev{};
    mev.type = SP::MouseEventType::AbsoluteMove;
    mev.x = xi;
    mev.y = yi;
    gMouseProvider->insert<"/events">(mev);
}

// Public API (C++)

namespace SP {

// Create a small Cocoa window and wire its events to the given providers.
// - No threads are created; pair with PSPollLocalEventWindow() in your main loop.
// - Calling multiple times reuses the same window and swaps the provider pointers.
void PSInitLocalEventWindow(PathIOMouse* mouse, PathIOKeyboard* keyboard) {
    @autoreleasepool {
        gMouseProvider = mouse;
        gKeyboardProvider = keyboard;

        NSApplication* app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        if (!gWindow) {
            NSRect frame = NSMakeRect(200, 200, 640, 360);
            gWindow = [[NSWindow alloc] initWithContentRect:frame
                                                  styleMask:(NSWindowStyleMaskTitled |
                                                             NSWindowStyleMaskClosable |
                                                             NSWindowStyleMaskMiniaturizable |
                                                             NSWindowStyleMaskResizable)
                                                    backing:NSBackingStoreBuffered
                                                      defer:NO];
            gWindow.title = @"PathSpace Input Test";
            PSEventView* view = [[PSEventView alloc] initWithFrame:((NSRect){.origin={0,0}, .size=frame.size})];
            [gWindow setContentView:view];
            [gWindow setAcceptsMouseMovedEvents:YES];
            [gWindow makeFirstResponder:view];
            [gWindow makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
        } else {
            [gWindow makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
        }
    }
}

// Poll the Cocoa event queue and dispatch pending events; non-blocking.
// Call this regularly (e.g., each loop iteration) to pump window events.
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

} // namespace SP

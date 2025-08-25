#import <Foundation/Foundation.h>
#import <objc/message.h>
#include <pathspace/layer/PathIOGamepad.hpp>


// ObjC++ translation unit providing:
// - A weakly-linked helper for macOS GameController haptics (PSGameControllerApplyRumble).
// - GameController input sourcing (PSInitGameControllerInput) that forwards events to PathIOGamepad.
//
// Strategy:
// - Resolve GCController dynamically via NSClassFromString so this TU stays loadable even on systems
//   without GameController.framework at runtime.
// - Prefer simple, vendor-agnostic selectors; guard all calls via respondsToSelector.
// - For inputs, install valueChangedHandler on the extended profile to forward button/axis changes.
// - Return conservative results when APIs are not present; callers will degrade gracefully.
//
// Note: The example application must be linked with -framework GameController for these selectors to resolve
//       on systems that provide them.

static inline NSArray* PSFetchControllers() {
    Class GCController = NSClassFromString(@"GCController");
    if (!GCController) return nil;
    SEL controllersSel = NSSelectorFromString(@"controllers");
    if (![GCController respondsToSelector:controllersSel]) return nil;

    // NSArray<GCController*>* (+controllers)
    typedef id (*MsgSendClass0)(Class, SEL);
    auto send = reinterpret_cast<MsgSendClass0>(objc_msgSend);
    id arr = send(GCController, controllersSel);
    if (![arr isKindOfClass:[NSArray class]]) return nil;
    return (NSArray*)arr;
}

static inline id PSSelectControllerByIndex(NSArray* ctrls, int deviceId) {
    if (!ctrls || ctrls.count == 0) return nil;
    if (deviceId >= 0 && deviceId < (int)ctrls.count) {
        return ctrls[(NSUInteger)deviceId];
    }
    // Default to first connected controller
    return ctrls.firstObject;
}

extern "C" bool PSGameControllerApplyRumble(int deviceId, float low, float high, uint32_t durationMs) {
    @autoreleasepool {
        NSArray* ctrls = PSFetchControllers();
        id controller = PSSelectControllerByIndex(ctrls, deviceId);
        if (!controller) return false;

        // Option 1: Some controllers expose a simple rumble selector directly on GCController:
        //   - (BOOL)setRumbleWithStrong:(float)strong weak:(float)weak duration:(double)seconds;
        SEL setRumbleSel = NSSelectorFromString(@"setRumbleWithStrong:weak:duration:");
        if ([controller respondsToSelector:setRumbleSel]) {
            typedef BOOL (*RumbleFunc)(id, SEL, float, float, double);
            auto rumble = reinterpret_cast<RumbleFunc>(objc_msgSend);
            double seconds = (double)durationMs / 1000.0;
            BOOL ok = rumble(controller, setRumbleSel, low, high, seconds);
            return ok ? true : false;
        }

        // Option 2: Newer haptics engine path (GameController haptics), accessed dynamically:
        // controller.haptics -> engine = [haptics createEngineWithLocality:error:]
        // [engine startWithError:&err]; // then try a simple continuous player if available
        // We keep this best-effort and very defensive due to API variations.
        SEL hapticsSel = NSSelectorFromString(@"haptics");
        if ([controller respondsToSelector:hapticsSel]) {
            typedef id (*MsgSend0)(id, SEL);
            auto send0 = reinterpret_cast<MsgSend0>(objc_msgSend);
            id haptics = send0(controller, hapticsSel);
            if (haptics) {
                SEL createEngineSel = NSSelectorFromString(@"createEngineWithLocality:error:");
                if ([haptics respondsToSelector:createEngineSel]) {
                    // id engine = [haptics createEngineWithLocality:0 error:nil];
                    typedef id (*CreateEngineFunc)(id, SEL, NSInteger, NSError**);
                    auto createEngine = reinterpret_cast<CreateEngineFunc>(objc_msgSend);
                    id engine = createEngine(haptics, createEngineSel, (NSInteger)0 /* default locality */, nullptr);
                    if (engine) {
                        // BOOL started = [engine startWithError:&err];
                        SEL startSel = NSSelectorFromString(@"startWithError:");
                        if ([engine respondsToSelector:startSel]) {
                            typedef BOOL (*StartFunc)(id, SEL, NSError**);
                            auto start = reinterpret_cast<StartFunc>(objc_msgSend);
                            BOOL started = start(engine, startSel, nullptr);
                            if (started) {
                                // Try a minimal "continuous" player API if present:
                                // id player = [engine createPlayerWithType:error:]; (type 0 as generic)
                                SEL createPlayerSel = NSSelectorFromString(@"createPlayerWithType:error:");
                                if ([engine respondsToSelector:createPlayerSel]) {
                                    typedef id (*CreatePlayerFunc)(id, SEL, NSInteger, NSError**);
                                    auto createPlayer = reinterpret_cast<CreatePlayerFunc>(objc_msgSend);
                                    id player = createPlayer(engine, createPlayerSel, (NSInteger)0, nullptr);
                                    if (player) {
                                        // Configure and start player if selectors exist:
                                        // [player setIntensity:value]; [player setSharpness:value]; [player startAtTime:error:]
                                        SEL setIntensitySel = NSSelectorFromString(@"setIntensity:");
                                        SEL setSharpnessSel = NSSelectorFromString(@"setSharpness:");
                                        if ([player respondsToSelector:setIntensitySel]) {
                                            typedef void (*SetFloatFunc)(id, SEL, float);
                                            auto setF = reinterpret_cast<SetFloatFunc>(objc_msgSend);
                                            float intensity = fmaxf(low, high);
                                            setF(player, setIntensitySel, intensity);
                                        }
                                        if ([player respondsToSelector:setSharpnessSel]) {
                                            typedef void (*SetFloatFunc)(id, SEL, float);
                                            auto setF = reinterpret_cast<SetFloatFunc>(objc_msgSend);
                                            setF(player, setSharpnessSel, 0.5f);
                                        }

                                        SEL startAtTimeSel = NSSelectorFromString(@"startAtTime:error:");
                                        if ([player respondsToSelector:startAtTimeSel]) {
                                            typedef BOOL (*StartAtTimeFunc)(id, SEL, double, NSError**);
                                            auto startAt = reinterpret_cast<StartAtTimeFunc>(objc_msgSend);
                                            BOOL ok = startAt(player, startAtTimeSel, 0.0, nullptr);
                                            if (ok) {
                                                // Schedule stop after duration (if stop selector exists), fire-and-forget.
                                                uint32_t ms = durationMs;
                                                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)ms * NSEC_PER_MSEC),
                                                               dispatch_get_main_queue(), ^{
                                                    SEL stopSel = NSSelectorFromString(@"stopAtTime:error:");
                                                    if ([player respondsToSelector:stopSel]) {
                                                        typedef BOOL (*StopAtTimeFunc)(id, SEL, double, NSError**);
                                                        auto stopAt = reinterpret_cast<StopAtTimeFunc>(objc_msgSend);
                                                        (void)stopAt(player, stopSel, 0.0, nullptr);
                                                    }
                                                });
                                                return true;
                                            }
                                        }
                                    }
                                }

                                // If we started the engine but couldn't create a player, stop it gracefully.
                                SEL stopSel = NSSelectorFromString(@"stopWithCompletionHandler:");
                                if ([engine respondsToSelector:stopSel]) {
                                    typedef void (*StopFunc)(id, SEL, void(^)(void));
                                    auto stop = reinterpret_cast<StopFunc>(objc_msgSend);
                                    stop(engine, stopSel, nil);
                                }
                            }
                        }
                    }
                }
            }
        }

        // No supported haptics API path was available
        return false;
    }
}

// Initialize GameController input sourcing and forward events to the provided PathIOGamepad.
// This installs handlers for currently connected controllers and subscribes to connect/disconnect notifications.
namespace SP {
void PSInitGameControllerInput(PathIOGamepad* pad) {
    @autoreleasepool {
        if (pad == nullptr) return;
        Class GCController = NSClassFromString(@"GCController");
        if (!GCController) return;

        // Helper: install a valueChangedHandler on the extended gamepad profile
        auto installHandlerForController = ^(id controller) {
            if (!controller) return;
            SEL extendedSel = NSSelectorFromString(@"extendedGamepad");
            if (![controller respondsToSelector:extendedSel]) return;
            typedef id (*MsgSend0)(id, SEL);
            auto send0 = reinterpret_cast<MsgSend0>(objc_msgSend);
            id profile = send0(controller, extendedSel);
            if (!profile) return;

            SEL setHandlerSel = NSSelectorFromString(@"setValueChangedHandler:");
            if ([profile respondsToSelector:setHandlerSel]) {
                // Handler signature: ^(id extendedGamepad, id element)
                // Use typed element classes to avoid calling '-[GCControllerElement value]' which can raise.
                Class BtnClass  = NSClassFromString(@"GCControllerButtonInput");
                Class AxisClass = NSClassFromString(@"GCControllerAxisInput");
                Class DPadClass = NSClassFromString(@"GCControllerDirectionPad");

                void (^handler)(id, id) = ^(id /*extendedGamepad*/, id element) {
                    @try {
                        typedef id (*MsgSend0)(id, SEL);
                        typedef BOOL (*MsgSendBool0)(id, SEL);
                        typedef float (*MsgSendFloat0)(id, SEL);
                        auto send0      = reinterpret_cast<MsgSend0>(objc_msgSend);
                        auto sendBool0  = reinterpret_cast<MsgSendBool0>(objc_msgSend);
                        auto sendFloat0 = reinterpret_cast<MsgSendFloat0>(objc_msgSend);

                        // Buttons (with triggers treated as analog axes)
                        if (BtnClass && [element isKindOfClass:BtnClass]) {
                            // Triggers as axes (LT=4, RT=5)
                            SEL leftTriggerSel  = NSSelectorFromString(@"leftTrigger");
                            SEL rightTriggerSel = NSSelectorFromString(@"rightTrigger");
                            id  lt = [profile respondsToSelector:leftTriggerSel]  ? send0(profile, leftTriggerSel)  : nil;
                            id  rt = [profile respondsToSelector:rightTriggerSel] ? send0(profile, rightTriggerSel) : nil;
                            SEL valueSel = NSSelectorFromString(@"value");
                            if (element == lt && [element respondsToSelector:valueSel]) {
                                float v = sendFloat0(element, valueSel);
                                pad->simulateAxisMove(/*axis*/4, v);
                                return;
                            }
                            if (element == rt && [element respondsToSelector:valueSel]) {
                                float v = sendFloat0(element, valueSel);
                                pad->simulateAxisMove(/*axis*/5, v);
                                return;
                            }

                            // Map common digital buttons to stable indices
                            int idx = -1;
                            if ([profile respondsToSelector:NSSelectorFromString(@"buttonA")] &&
                                element == send0(profile, NSSelectorFromString(@"buttonA"))) idx = 0;
                            else if ([profile respondsToSelector:NSSelectorFromString(@"buttonB")] &&
                                     element == send0(profile, NSSelectorFromString(@"buttonB"))) idx = 1;
                            else if ([profile respondsToSelector:NSSelectorFromString(@"buttonX")] &&
                                     element == send0(profile, NSSelectorFromString(@"buttonX"))) idx = 2;
                            else if ([profile respondsToSelector:NSSelectorFromString(@"buttonY")] &&
                                     element == send0(profile, NSSelectorFromString(@"buttonY"))) idx = 3;
                            else if ([profile respondsToSelector:NSSelectorFromString(@"leftShoulder")] &&
                                     element == send0(profile, NSSelectorFromString(@"leftShoulder"))) idx = 4;
                            else if ([profile respondsToSelector:NSSelectorFromString(@"rightShoulder")] &&
                                     element == send0(profile, NSSelectorFromString(@"rightShoulder"))) idx = 5;
                            else if ([profile respondsToSelector:NSSelectorFromString(@"leftThumbstickButton")] &&
                                     element == send0(profile, NSSelectorFromString(@"leftThumbstickButton"))) idx = 8;
                            else if ([profile respondsToSelector:NSSelectorFromString(@"rightThumbstickButton")] &&
                                     element == send0(profile, NSSelectorFromString(@"rightThumbstickButton"))) idx = 9;
                            else idx = 0; // fallback

                            SEL isPressedSel = NSSelectorFromString(@"isPressed");
                            if ([element respondsToSelector:isPressedSel]) {
                                BOOL pressed = sendBool0(element, isPressedSel);
                                if (pressed) {
                                    pad->simulateButtonDown(idx);
                                } else {
                                    pad->simulateButtonUp(idx);
                                }
                            }
                            return;
                        }

                        // Single axis: map to stick/dpad axes if recognized
                        if (AxisClass && [element isKindOfClass:AxisClass]) {
                            int axisIdx = -1;
                            SEL xSel = NSSelectorFromString(@"xAxis");
                            SEL ySel = NSSelectorFromString(@"yAxis");

                            // Left stick: 0=X, 1=Y
                            SEL ltsSel = NSSelectorFromString(@"leftThumbstick");
                            id  lts    = [profile respondsToSelector:ltsSel] ? send0(profile, ltsSel) : nil;
                            if (lts) {
                                id lx = send0(lts, xSel);
                                id ly = send0(lts, ySel);
                                if (element == lx) axisIdx = 0;
                                else if (element == ly) axisIdx = 1;
                            }

                            // Right stick: 2=X, 3=Y
                            if (axisIdx == -1) {
                                SEL rtsSel = NSSelectorFromString(@"rightThumbstick");
                                id  rts    = [profile respondsToSelector:rtsSel] ? send0(profile, rtsSel) : nil;
                                if (rts) {
                                    id rx = send0(rts, xSel);
                                    id ry = send0(rts, ySel);
                                    if (element == rx) axisIdx = 2;
                                    else if (element == ry) axisIdx = 3;
                                }
                            }

                            // DPad axes: 6=X, 7=Y
                            if (axisIdx == -1) {
                                SEL dpadSel = NSSelectorFromString(@"dpad");
                                id  dpad    = [profile respondsToSelector:dpadSel] ? send0(profile, dpadSel) : nil;
                                if (dpad) {
                                    id dx = send0(dpad, xSel);
                                    id dy = send0(dpad, ySel);
                                    if (element == dx) axisIdx = 6;
                                    else if (element == dy) axisIdx = 7;
                                }
                            }

                            if (axisIdx != -1) {
                                SEL valueSel = NSSelectorFromString(@"value");
                                if ([element respondsToSelector:valueSel]) {
                                    float v = sendFloat0(element, valueSel);
                                    pad->simulateAxisMove(axisIdx, v);
                                }
                                return;
                            }
                        }

                        // DPad (x/y axes changed together) — emit both indices (6,7)
                        if (DPadClass && [element isKindOfClass:DPadClass]) {
                            SEL xSel = NSSelectorFromString(@"xAxis");
                            SEL ySel = NSSelectorFromString(@"yAxis");
                            if ([element respondsToSelector:xSel] && [element respondsToSelector:ySel]) {
                                id xAxis = send0(element, xSel);
                                id yAxis = send0(element, ySel);
                                SEL valueSel = NSSelectorFromString(@"value");
                                if (xAxis && [xAxis respondsToSelector:valueSel]) {
                                    float xv = sendFloat0(xAxis, valueSel);
                                    pad->simulateAxisMove(/*axis*/6, xv);
                                }
                                if (yAxis && [yAxis respondsToSelector:valueSel]) {
                                    float yv = sendFloat0(yAxis, valueSel);
                                    pad->simulateAxisMove(/*axis*/7, yv);
                                }
                                return;
                            }
                        }
                    } @catch (NSException* /*ex*/) {
                        // Swallow and continue; some elements may expose selectors but not support direct reads.
                    }
                };
                typedef void (*SetHandler)(id, SEL, id);
                auto setH = reinterpret_cast<SetHandler>(objc_msgSend);
                setH(profile, setHandlerSel, handler);
            }
        };

        // Attach to existing controllers and emit "connected"
        NSArray* ctrls = PSFetchControllers();
        if (ctrls) {
            for (id c in ctrls) {
                pad->simulateConnected();
                installHandlerForController(c);
            }
        }

        // Observe connect/disconnect notifications (using string names to avoid compile-time linkage)
        NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
        NSOperationQueue* queue = [NSOperationQueue mainQueue];

        [center addObserverForName:@"GCControllerDidConnectNotification"
                            object:nil
                             queue:queue
                        usingBlock:^(NSNotification* note) {
            id controller = note.object;
            pad->simulateConnected();
            installHandlerForController(controller);
        }];

        [center addObserverForName:@"GCControllerDidDisconnectNotification"
                            object:nil
                             queue:queue
                        usingBlock:^(NSNotification* /*note*/) {
            pad->simulateDisconnected();
        }];
    }
}
} // namespace SP

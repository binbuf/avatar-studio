#include "platform_power.h"

#import <Foundation/Foundation.h>

namespace {

// App Nap only suspends an app that has no user-initiated activity. Holding
// one for the lifetime of a live consumer is the documented way to opt out.
// The object is retained explicitly because this target is built without ARC.
id g_activity = nil;

} // namespace

namespace avatar::platform {

void disable_app_nap() {
    if (g_activity != nil) {
        return;
    }
    NSProcessInfo* info = [NSProcessInfo processInfo];
    const NSActivityOptions options =
        NSActivityUserInitiatedAllowingIdleSystemSleep | NSActivityLatencyCritical;
#if __has_feature(objc_arc)
    g_activity = [info beginActivityWithOptions:options
                                         reason:@"Avatar Studio live render"];
#else
    g_activity = [[info beginActivityWithOptions:options
                                          reason:@"Avatar Studio live render"] retain];
#endif
}

void enable_app_nap() {
    if (g_activity == nil) {
        return;
    }
    [[NSProcessInfo processInfo] endActivity:g_activity];
#if !__has_feature(objc_arc)
    [g_activity release];
#endif
    g_activity = nil;
}

} // namespace avatar::platform

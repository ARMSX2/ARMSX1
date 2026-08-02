#import "AppDelegate.h"
#import "EmulatorRunner.h"

@interface AppDelegate ()
@property (nonatomic, strong) EmulatorRunner *emulatorRunner;
@property (nonatomic, copy) NSString *launchProtocolURL;
@end

@implementation AppDelegate

- (void)forceLandscapeIfNeeded {
    dispatch_async(dispatch_get_main_queue(), ^{
        UIInterfaceOrientation currentOrientation = UIInterfaceOrientationUnknown;
        if (@available(iOS 13.0, *)) {
            currentOrientation = self.window.windowScene.interfaceOrientation;
        } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            currentOrientation = UIApplication.sharedApplication.statusBarOrientation;
#pragma clang diagnostic pop
        }

        if (UIInterfaceOrientationIsLandscape(currentOrientation)) {
            return;
        }

        NSNumber *value = @(UIInterfaceOrientationLandscapeRight);
        [[UIDevice currentDevice] setValue:value forKey:@"orientation"];
        [UIViewController attemptRotationToDeviceOrientation];
    });
}

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    (void)application;

    self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
    self.window.backgroundColor = [UIColor blackColor];
    [self.window makeKeyAndVisible];
    [self forceLandscapeIfNeeded];

    NSURL *launchURL = launchOptions[UIApplicationLaunchOptionsURLKey];
    if (launchURL.absoluteString.length) {
        self.launchProtocolURL = launchURL.absoluteString;
    }

    self.emulatorRunner = [EmulatorRunner new];
    [self.emulatorRunner startWithArgs:self.launchProtocolURL.length ? @[self.launchProtocolURL] : @[]];

    return YES;
}

- (BOOL)application:(UIApplication *)application openURL:(NSURL *)url options:(NSDictionary<UIApplicationOpenURLOptionsKey, id> *)options {
    (void)application;
    (void)options;

    NSString *argument = url.absoluteString;
    if (argument.length == 0) {
        return NO;
    }

    if (self.launchProtocolURL.length && [self.launchProtocolURL isEqualToString:argument]) {
        self.launchProtocolURL = nil;
        return YES;
    }

    if (!self.emulatorRunner) {
        self.emulatorRunner = [EmulatorRunner new];
        [self.emulatorRunner startWithArgs:@[argument]];
    } else {
        [self.emulatorRunner enqueueLaunchArgument:argument];
    }

    return YES;
}

@end

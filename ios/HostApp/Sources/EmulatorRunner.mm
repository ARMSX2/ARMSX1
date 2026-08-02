#import "EmulatorRunner.h"

#import <SDL2/SDL.h>

#include <string>
#include <vector>

#import "armsx_bridge.h"

@interface EmulatorRunner ()
@property (nonatomic, assign, readwrite, getter=isRunning) BOOL running;
@end

@implementation EmulatorRunner

- (void)startWithArgs:(NSArray<NSString *> *)args {
    if (self.running) {
        for (NSString *argument in args) {
            [self enqueueLaunchArgument:argument];
        }
        return;
    }

    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        NSLog(@"SDL_Init failed: %s", SDL_GetError());
        return;
    }

    self.running = YES;

    dispatch_async(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            NSMutableArray<NSString *> *nativeArgs = [NSMutableArray arrayWithObject:@"armsx"];
            NSFileManager *fm = [NSFileManager defaultManager];

            NSMutableArray<NSString *> *candidatePaths = [NSMutableArray array];
            NSBundle *bundle = [NSBundle mainBundle];

            NSString *bundleRoot = bundle.bundlePath;
            NSString *resourceRoot = bundle.resourcePath;

            if (bundleRoot.length) {
                [candidatePaths addObject:[bundleRoot stringByAppendingPathComponent:@"bios.bin"]];
                [candidatePaths addObject:[bundleRoot stringByAppendingPathComponent:@"Contents/bios.bin"]];
            }
            if (resourceRoot.length) {
                [candidatePaths addObject:[resourceRoot stringByAppendingPathComponent:@"bios.bin"]];
            }

            NSString *biosPath = [bundle pathForResource:@"bios" ofType:@"bin"];
            if (biosPath.length) {
                [candidatePaths insertObject:biosPath atIndex:0];
            }

            biosPath = nil;

            for (NSString *candidate in candidatePaths) {
                if ([fm fileExistsAtPath:candidate]) {
                    biosPath = candidate;
                    break;
                }
            }

            if (!biosPath.length) {
                char *basePathC = SDL_GetBasePath();
                if (basePathC) {
                    NSString *basePath = [NSString stringWithUTF8String:basePathC];
                    SDL_free(basePathC);
                    NSString *fallback = [basePath stringByAppendingPathComponent:@"bios.bin"];
                    if ([fm fileExistsAtPath:fallback]) {
                        biosPath = fallback;
                    }
                }
            }

            if (biosPath.length && [fm fileExistsAtPath:biosPath]) {
                char *prefPathC = SDL_GetPrefPath("nanodata", "armsx");
                NSString *prefBase = prefPathC ? [NSString stringWithUTF8String:prefPathC] : nil;
                if (prefPathC) {
                    SDL_free(prefPathC);
                }

                NSString *writableBios = prefBase ? [prefBase stringByAppendingPathComponent:@"bios.bin"] : nil;
                NSError *copyErr = nil;

                if (writableBios.length && [fm fileExistsAtPath:writableBios]) {
                    [fm removeItemAtPath:writableBios error:nil];
                }

                if (writableBios.length && [fm copyItemAtPath:biosPath toPath:writableBios error:&copyErr]) {
                    biosPath = writableBios;
                    NSLog(@"Using bundled BIOS copied to writable path %@", biosPath);
                } else if (writableBios.length) {
                    NSLog(@"Failed to copy BIOS to writable location (%@). Using bundle path. Error: %@", writableBios, copyErr);
                }
            }

            if (biosPath.length && [fm fileExistsAtPath:biosPath]) {
                [nativeArgs addObject:@"--bios"];
                [nativeArgs addObject:biosPath];
                NSLog(@"Passing BIOS path to libarmsx: %@", biosPath);
            } else {
                NSLog(@"No bundled BIOS found; libarmsx will rely on user-provided settings/CLI.");
            }

            if (args.count) {
                [nativeArgs addObjectsFromArray:args];
            }

            std::vector<std::string> argvStorage;
            std::vector<const char *> argv;

            for (NSString *stringArg in nativeArgs) {
                argvStorage.emplace_back([stringArg UTF8String]);
            }

            for (const auto &value : argvStorage) {
                argv.push_back(value.c_str());
            }

            argv.push_back(nullptr);

            external_main((int)argvStorage.size(), argv.data());
        }
    });
}

- (void)enqueueLaunchArgument:(NSString *)argument {
    if (argument.length == 0) {
        return;
    }

    psxe_enqueue_launch_argument(argument.UTF8String);
}

@end

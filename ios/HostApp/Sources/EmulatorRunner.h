#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface EmulatorRunner : NSObject

@property (nonatomic, assign, readonly, getter=isRunning) BOOL running;

- (void)startWithArgs:(NSArray<NSString *> *)args;
- (void)enqueueLaunchArgument:(NSString *)argument;

@end

NS_ASSUME_NONNULL_END

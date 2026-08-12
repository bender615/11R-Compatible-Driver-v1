/*
See the LICENSE.txt file for this sample’s licensing information.

Abstract:
The Objective-C/Swift bridging header, which manages communications
			 between user clients and the audio device.
*/
#import <IOKit/IOKitLib.h>
#import <Foundation/Foundation.h>

@interface ElevenRackUserClient : NSObject

- (NSString*) open;
- (void) close;
- (double) readCurrentSampleRate;
- (uint32_t) readHardwareSampleRate;
- (NSInteger) readClockSource;
- (NSDictionary<NSString*, NSNumber*>*) readAudioStatus;
- (NSDictionary<NSString*, NSNumber*>*) readCoreAudioStatus;
- (NSString*) setSampleRate:(double)sampleRate;
- (NSString*) followExternalSampleRate:(double)sampleRate;
- (NSString*) setClockSource:(NSInteger)clockSource;

@end

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
- (NSString*) toggleDataSource;
- (NSString*) toggleRate;

@end

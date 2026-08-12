/*
See the LICENSE.txt file for this sample’s licensing information.

Abstract:
The implementation of the Objective-C bridging class, which manages
			 communications between user clients and the audio device.
*/

#import "ElevenRackUserClient.h"
#import "ElevenRackDriverKeys.h"

@interface ElevenRackUserClient()
@property io_object_t ioObject;
@property io_connect_t ioConnection;
@end

@implementation ElevenRackUserClient

#if TARGET_OS_OSX
#import <CoreAudio/CoreAudio.h>
#import <CoreAudio/AudioServerPlugIn.h>
#import <vector>

- (AudioObjectID)audioDeviceID:(OSStatus*)outError
{
	AudioObjectPropertyAddress address = {
		kAudioHardwarePropertyTranslateUIDToDevice,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain
	};
	auto deviceUID = CFSTR(kElevenRackDriverDeviceUID);
	AudioObjectID deviceID = kAudioObjectUnknown;
	UInt32 size = sizeof(deviceID);
	OSStatus error = AudioObjectGetPropertyData(kAudioObjectSystemObject,
		&address, sizeof(deviceUID), &deviceUID, &size, &deviceID);
	if (outError) *outError = error;
	return error == noErr ? deviceID : kAudioObjectUnknown;
}

// The CoreAudio framework and custom property API is only available in macOS.
// Validate the device's custom properties by checking the data types, selector,
// qualifier, and data value.
- (OSStatus)checkDeviceCustomProperties
{
	OSStatus err = kAudioHardwareNoError;
	try
	{
		AudioObjectPropertyAddress prop_addr = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};

		prop_addr = {kAudioHardwarePropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
		auto device_uid = CFSTR(kElevenRackDriverDeviceUID);
		AudioObjectID device_id = kAudioObjectUnknown;
		UInt32 out_size = sizeof(AudioObjectID);
		err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop_addr, sizeof(CFStringRef), &device_uid, &out_size, &device_id);
		if (err)
		{
			throw std::runtime_error("Failed to get ElevenRackDevice by uid");
		}

		prop_addr = {kAudioObjectPropertyCustomPropertyInfoList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
		err = AudioObjectGetPropertyDataSize(device_id, &prop_addr, 0, nullptr, &out_size);
		if (err)
		{
			throw std::runtime_error("Failed to get custom property list size");
		}

		auto num_items = out_size/sizeof(AudioServerPlugInCustomPropertyInfo);
		std::vector<AudioServerPlugInCustomPropertyInfo> custom_prop_list(num_items);
		err = AudioObjectGetPropertyData(device_id, &prop_addr, 0, nullptr, &out_size, custom_prop_list.data());
		if (err)
		{
			throw std::runtime_error("Failed to get custom property list");
		}
		num_items = out_size / sizeof(AudioServerPlugInCustomPropertyInfo);
		custom_prop_list.resize(num_items);
		if (num_items != 1)
		{
			throw std::runtime_error("Should only have one custom property on the ElevenRackDevice");
		}

		AudioServerPlugInCustomPropertyInfo custom_prop_info = custom_prop_list.front();
		if (custom_prop_info.mSelector != kElevenRackDriverCustomPropertySelector)
		{
			throw std::runtime_error("Custom property selector is incorrect");
		}
		if (custom_prop_info.mQualifierDataType != kAudioServerPlugInCustomPropertyDataTypeCFString)
		{
			throw std::runtime_error("Custom property qualifier type is incorrect");
		}
		if (custom_prop_info.mPropertyDataType != kAudioServerPlugInCustomPropertyDataTypeCFString)
		{
			throw std::runtime_error("Custom property data type is incorrect");
		}

		std::vector<std::pair<CFStringRef, CFStringRef>> custom_prop_qualifier_data_pair = {
			{ CFSTR(kElevenRackDriverCustomPropertyQualifier0), CFSTR(kElevenRackDriverCustomPropertyDataValue0) },
			{ CFSTR(kElevenRackDriverCustomPropertyQualifier1), CFSTR(kElevenRackDriverCustomPropertyDataValue1) },
		};

		prop_addr = { custom_prop_info.mSelector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
		for (const auto &[qualifier, data] : custom_prop_qualifier_data_pair)
		{
			CFStringRef custom_prop_data = nullptr;
			UInt32 the_size = sizeof(CFStringRef);
			err = AudioObjectGetPropertyData(device_id, &prop_addr, sizeof(CFStringRef), &qualifier, &the_size, &custom_prop_data);
			if (err)
			{
				throw std::runtime_error("Error getting custom property value");
			}
			CFComparisonResult compare_result = CFStringCompare(data, custom_prop_data, kCFCompareCaseInsensitive);
			if (compare_result != kCFCompareEqualTo)
			{
				throw std::runtime_error("Custom property data is incorrect");
			}
			CFRelease(custom_prop_data);
		}
	}
	catch(...)
	{
		NSLog(@"Caught exception trying to validate custom properties.");
	}
	return err;
}
#endif

- (double)readCurrentSampleRate
{
#if TARGET_OS_OSX
	OSStatus error = noErr;
	AudioObjectID deviceID = [self audioDeviceID:&error];
	if (error != noErr || deviceID == kAudioObjectUnknown) return 0;
	AudioObjectPropertyAddress address = {
		kAudioDevicePropertyNominalSampleRate,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain
	};
	Float64 rate = 0;
	UInt32 size = sizeof(rate);
	error = AudioObjectGetPropertyData(deviceID, &address, 0, nullptr,
		&size, &rate);
	return error == noErr ? rate : 0;
#else
	return 0;
#endif
}

- (NSString*)setSampleRate:(double)sampleRate
{
#if TARGET_OS_OSX
	uint32_t requested = (uint32_t)sampleRate;
	if (requested != 44100 && requested != 48000 &&
		requested != 88200 && requested != 96000)
		return @"Unsupported sample rate.";
	NSInteger clockSource = [self readClockSource];
	if (clockSource > ElevenRackClockSource_Internal)
		return @"Switch Clock Source to Internal before changing sample rate.";

	OSStatus error = noErr;
	AudioObjectID deviceID = [self audioDeviceID:&error];
	if (error != noErr || deviceID == kAudioObjectUnknown)
		return @"Eleven Rack Core Audio device is unavailable.";
	AudioObjectPropertyAddress address = {
		kAudioDevicePropertyNominalSampleRate,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain
	};
	Float64 rate = sampleRate;
	error = AudioObjectSetPropertyData(deviceID, &address, 0, nullptr,
		sizeof(rate), &rate);
	if (error != noErr)
		return [NSString stringWithFormat:
			@"Failed to set sample rate (Core Audio error %d). Stop audio applications and retry.",
			(int)error];
	return [NSString stringWithFormat:@"Sample rate set to %.1f kHz.",
		sampleRate / 1000.0];
#else
	return @"Sample-rate control is available on macOS only.";
#endif
}

- (NSString*)followExternalSampleRate:(double)sampleRate
{
#if TARGET_OS_OSX
	const uint32_t requested = static_cast<uint32_t>(sampleRate);
	if (requested != 44100 && requested != 48000 &&
		requested != 88200 && requested != 96000)
		return @"External clock is not at a supported sample rate.";
	if ([self readClockSource] == ElevenRackClockSource_Internal)
		return @"External-rate following is only available with AES/EBU or S/PDIF clock.";
	OSStatus error = noErr;
	AudioObjectID deviceID = [self audioDeviceID:&error];
	if (error != noErr || deviceID == kAudioObjectUnknown)
		return @"Eleven Rack Core Audio device is unavailable.";
	AudioObjectPropertyAddress address = {
		kAudioDevicePropertyNominalSampleRate,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain
	};
	Float64 rate = sampleRate;
	error = AudioObjectSetPropertyData(deviceID, &address, 0, nullptr,
		sizeof(rate), &rate);
	return error == noErr ? @"Core Audio followed the external clock rate." :
		[NSString stringWithFormat:@"Could not follow external rate (Core Audio error %d).", (int)error];
#else
	return @"External-rate following is available on macOS only.";
#endif
}

- (NSInteger)readClockSource
{
	return [[self readAudioStatus][@"clockSource"] integerValue];
}

- (uint32_t)readHardwareSampleRate
{
	return [[self readAudioStatus][@"hardwareSampleRate"] unsignedIntValue];
}

- (NSDictionary<NSString*, NSNumber*>*)readAudioStatus
{
	if (_ioConnection == IO_OBJECT_NULL) return @{};
	uint64_t output[4] = { 0, 0, ElevenRackClockValidity_Unknown, 0 };
	uint32_t outputCount = 4;
	kern_return_t error = IOConnectCallScalarMethod(_ioConnection,
		static_cast<uint64_t>(ElevenRackDriverExternalMethod_GetAudioStatus),
		nullptr, 0, output, &outputCount);
	if (error != kIOReturnSuccess || outputCount != 4) return @{};
	return @{
		@"clockSource": @(output[0]),
		@"hardwareSampleRate": @(output[1]),
		@"clockValid": @(output[2]),
		@"streaming": @(output[3])
	};
}

- (NSDictionary<NSString*, NSNumber*>*)readCoreAudioStatus
{
#if TARGET_OS_OSX
	OSStatus error = noErr;
	AudioObjectID deviceID = [self audioDeviceID:&error];
	if (error != noErr || deviceID == kAudioObjectUnknown) return @{};

	auto readUInt32 = ^UInt32(AudioObjectPropertySelector selector,
		AudioObjectPropertyScope scope) {
		AudioObjectPropertyAddress address = {
			selector, scope, kAudioObjectPropertyElementMain
		};
		UInt32 value = 0;
		UInt32 size = sizeof(value);
		return AudioObjectGetPropertyData(deviceID, &address, 0, nullptr,
			&size, &value) == noErr ? value : 0;
	};
	const UInt32 bufferFrames = readUInt32(kAudioDevicePropertyBufferFrameSize,
		kAudioObjectPropertyScopeGlobal);
	const UInt32 inputSafety = readUInt32(kAudioDevicePropertySafetyOffset,
		kAudioObjectPropertyScopeInput);
	const UInt32 outputSafety = readUInt32(kAudioDevicePropertySafetyOffset,
		kAudioObjectPropertyScopeOutput);
	const UInt32 inputLatency = readUInt32(kAudioDevicePropertyLatency,
		kAudioObjectPropertyScopeInput);
	const UInt32 outputLatency = readUInt32(kAudioDevicePropertyLatency,
		kAudioObjectPropertyScopeOutput);
	const double rate = [self readCurrentSampleRate];
	const UInt64 roundTripFrames = static_cast<UInt64>(bufferFrames) * 2 +
		inputSafety + outputSafety + inputLatency + outputLatency;
	const double roundTripMilliseconds = rate > 0 ?
		static_cast<double>(roundTripFrames) * 1000.0 / rate : 0;
	return @{
		@"sampleRate": @(rate),
		@"bufferFrames": @(bufferFrames),
		@"inputSafetyFrames": @(inputSafety),
		@"outputSafetyFrames": @(outputSafety),
		@"inputLatencyFrames": @(inputLatency),
		@"outputLatencyFrames": @(outputLatency),
		@"estimatedRoundTripFrames": @(roundTripFrames),
		@"estimatedRoundTripMilliseconds": @(roundTripMilliseconds)
	};
#else
	return @{};
#endif
}

- (NSString*)setClockSource:(NSInteger)clockSource
{
	if (_ioConnection == IO_OBJECT_NULL)
		return @"Cannot set clock source because the driver user client is not connected.";
	if (clockSource < ElevenRackClockSource_Internal ||
		clockSource > ElevenRackClockSource_SPDIF)
		return @"Unsupported clock source.";
	uint64_t input = (uint64_t)clockSource;
	kern_return_t error = IOConnectCallScalarMethod(_ioConnection,
		static_cast<uint64_t>(ElevenRackDriverExternalMethod_SetClockSource),
		&input, 1, nullptr, nullptr);
	if (error == kIOReturnBusy)
		return @"Clock source cannot change while audio is streaming. Stop audio applications and retry.";
	if (error != kIOReturnSuccess)
		return [NSString stringWithFormat:@"Failed to set clock source (error 0x%08X).", error];
	NSArray<NSString*>* names = @[ @"", @"Internal", @"AES/EBU", @"S/PDIF" ];
	return [NSString stringWithFormat:@"Clock source set to %@.", names[clockSource]];
}

// Open a user client instance, which initiates communication with the driver.
- (NSString*) open
{
	if (_ioObject == IO_OBJECT_NULL && _ioConnection == IO_OBJECT_NULL)
	{
		// Get the IOKit main port.
		mach_port_t theMainPort = MACH_PORT_NULL;
		kern_return_t theKernelError = IOMainPort(bootstrap_port, &theMainPort);
		if (theKernelError != kIOReturnSuccess)
		{
			return @"Failed to get IOMainPort.";
		}

		// Create a matching dictionary for the driver class. Note that classes
		// published by a dext need to be matched by class name rather than
		// other methods. So be sure to use IOServiceNameMatching rather than
		// IOServiceMatching to construct the dictionary.
		CFDictionaryRef theMatchingDictionary = IOServiceNameMatching(kElevenRackDriverClassName);
		io_service_t matchedService = IOServiceGetMatchingService(theMainPort, theMatchingDictionary);
		if (matchedService)
		{
			_ioObject = matchedService;
			theKernelError = IOServiceOpen(_ioObject, mach_task_self(), 0, &_ioConnection);
			if (theKernelError == kIOReturnSuccess)
			{
#if TARGET_OS_OSX
				OSStatus error = [self checkDeviceCustomProperties];
				if (error)
				{
					return @"Connection to user client succeeded, but custom properties could not be validated";
				}
#endif
				return @"Connection to user client succeeded";
			}
			else
			{
				IOObjectRelease(_ioObject);
				_ioObject = IO_OBJECT_NULL;
				_ioConnection = IO_OBJECT_NULL;
				return [NSString stringWithFormat:@"Failed to open user client connection, error:%u.", theKernelError];
			}
		}
		return @"Driver Extension is not running";
	}
	return @"User client is already connected";
}

- (void)close
{
	if (_ioConnection != IO_OBJECT_NULL)
	{
		IOServiceClose(_ioConnection);
		_ioConnection = IO_OBJECT_NULL;
	}
	if (_ioObject != IO_OBJECT_NULL)
	{
		IOObjectRelease(_ioObject);
		_ioObject = IO_OBJECT_NULL;
	}
}

- (void)dealloc
{
	[self close];
}
@end

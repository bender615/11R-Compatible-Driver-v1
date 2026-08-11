/*
See the LICENSE.txt file for this sample’s licensing information.

Abstract:
The implementation of an AudioDriverKit device that generates a
             sine wave.
*/

// Local Includes
#include "ElevenRackDevice.h"
#include "ElevenRackDriver.h"
#include "ElevenRackDriverKeys.h"

// AudioDriverKit Includes
#include <AudioDriverKit/AudioDriverKit.h>

// System Includes
#include <math.h>
#include <cstring>
#include <utility>
#include <DriverKit/DriverKit.h>
#include <USBDriverKit/IOUSBHostDevice.h>
#include <USBDriverKit/IOUSBHostInterface.h>
#include <USBDriverKit/IOUSBHostPipe.h>
#include <USBDriverKit/USBDriverKitDefs.h>

#define kDefaultSampleRate 48000.0
#define kSampleRateCount 4
#define kInputChannels 8
#define kOutputChannels 6
#define kUSBInterfaceOutput 3
#define kUSBInterfaceInput 4
#define kUSBEndpointOutput 0x03
#define kUSBEndpointInput 0x83
#define kUSBAlternateStreaming 1
#define kUSBRequestCount 8
#define kUSBFramesPerRequest 16
#define kUSBMaxPacket 416

static const double kSupportedSampleRates[kSampleRateCount] = {
	44100.0, 48000.0, 88200.0, 96000.0
};

// Core Audio element numbers are one-based.  These names follow the physical
// USB lane order used by the original Eleven Rack driver and appear in clients
// that query kAudioObjectPropertyElementName (Audio MIDI Setup, Logic, etc.).
static const char* kInputChannelNames[kInputChannels] = {
	"Guitar Input", "Mic Input", "Eleven Rig L", "Eleven Rig R",
	"Digital Input L", "Digital Input R", "Line Input L", "Line Input R"
};
static const char* kOutputChannelNames[kOutputChannels] = {
	"Main Output L", "Main Output R", "Re-Amp L", "Re-Amp R",
	"Digital Out L", "Digital Out R"
};

#define ER_FLOAT_FORMAT(rate, channels) { \
	(rate), IOUserAudioFormatID::LinearPCM, \
	static_cast<IOUserAudioFormatFlags>(IOUserAudioFormatFlags::FormatFlagIsFloat | \
		IOUserAudioFormatFlags::FormatFlagsNativeEndian), \
	static_cast<uint32_t>(sizeof(float) * (channels)), 1, \
	static_cast<uint32_t>(sizeof(float) * (channels)), (channels), 32 \
}

struct ERUSBRequest
{
	IOBufferMemoryDescriptor* dataDescriptor;
	IOBufferMemoryDescriptor* frameDescriptor;
	IOMemoryMap* dataMap;
	IOMemoryMap* frameMap;
	OSAction* action;
	bool input;
};

#define kToneGenerationBufferFrameSize 512

#define kNumInputDataSources 3

struct ElevenRackDevice_IVars
{
	OSSharedPtr<IOUserAudioDriver>	m_driver;
	OSSharedPtr<IODispatchQueue>	m_work_queue;

	uint64_t	m_zts_host_ticks_per_buffer;

	IOUserAudioStreamBasicDescription		m_stream_format;

	OSSharedPtr<IOUserAudioStream>			m_output_stream;
	OSSharedPtr<IOMemoryMap>				m_output_memory_map;

	OSSharedPtr<IOUserAudioStream>			m_input_stream;
	OSSharedPtr<IOMemoryMap>				m_input_memory_map;

	OSSharedPtr<IOUserAudioLevelControl>	m_input_volume_control;
	OSSharedPtr<IOUserAudioSelectorControl> m_input_selector_control;
	IOUserAudioSelectorValueDescription 	m_data_sources[kNumInputDataSources];

	OSSharedPtr<IOTimerDispatchSource>		m_zts_timer_event_source;
	OSSharedPtr<OSAction>					m_zts_timer_occurred_action;

	uint64_t	m_tone_sample_index;

	OSSharedPtr<IOUSBHostDevice>		m_usb_device;
	OSSharedPtr<IOUSBHostInterface>	m_usb_input_interface;
	OSSharedPtr<IOUSBHostInterface>	m_usb_output_interface;
	OSSharedPtr<IOUSBHostPipe>		m_usb_input_pipe;
	OSSharedPtr<IOUSBHostPipe>		m_usb_output_pipe;
	ERUSBRequest m_usb_input_requests[kUSBRequestCount];
	ERUSBRequest m_usb_output_requests[kUSBRequestCount];
	uint64_t m_usb_next_input_frame;
	uint64_t m_usb_next_output_frame;
	uint64_t m_usb_input_sample_frame;
	uint64_t m_usb_output_sample_frame;
	double m_usb_output_accumulator;
	bool m_usb_running;
};

static void ERReleaseUSBRequest(ERUSBRequest& request)
{
	OSSafeReleaseNULL(request.action);
	OSSafeReleaseNULL(request.dataMap);
	OSSafeReleaseNULL(request.frameMap);
	OSSafeReleaseNULL(request.dataDescriptor);
	OSSafeReleaseNULL(request.frameDescriptor);
}

void ElevenRackDevice::ConfigureUSBDevice(IOUSBHostDevice* device)
{
	ivars->m_usb_device = OSSharedPtr(device, OSRetain);
}

kern_return_t ElevenRackDevice::SetUSBHardwareRate(uint32_t rate)
{
	if (!ivars->m_usb_device) return kIOReturnNotReady;
	kern_return_t result = kIOReturnSuccess;
	IOBufferMemoryDescriptor* descriptor = nullptr;
	IOMemoryMap* map = nullptr;
	uint16_t transferred = 0;

	// Select the Eleven Rack internal clock (entity 0x80 on interface 1).
	result = ivars->m_usb_device->CreateIOBuffer(kIOMemoryDirectionOut, 4, &descriptor);
	if (result != kIOReturnSuccess) goto Exit;
	result = descriptor->CreateMapping(0, 0, 0, 0, 0, &map);
	if (result != kIOReturnSuccess) goto Exit;
	*reinterpret_cast<uint8_t*>(map->GetAddress() + map->GetOffset()) = 1;
	result = ivars->m_usb_device->DeviceRequest(ivars->m_driver.get(), 0x21, 0x01,
		0x0100, 0x8001, 1, descriptor, &transferred, 1000);
	if (result != kIOReturnSuccess) goto Exit;

	*reinterpret_cast<uint32_t*>(map->GetAddress() + map->GetOffset()) = rate;
	transferred = 0;
	result = ivars->m_usb_device->DeviceRequest(ivars->m_driver.get(), 0x21, 0x01,
		0x0100, 0x8101, 4, descriptor, &transferred, 1000);

Exit:
	OSSafeReleaseNULL(map);
	OSSafeReleaseNULL(descriptor);
	return result;
}

kern_return_t ElevenRackDevice::StartUSBTransport()
{
	if (!ivars->m_usb_device || !ivars->m_input_memory_map || !ivars->m_output_memory_map)
		return kIOReturnNotReady;

	kern_return_t result = kIOReturnSuccess;
	uintptr_t iterator = 0;
	result = ivars->m_usb_device->CreateInterfaceIterator(&iterator);
	if (result != kIOReturnSuccess) return result;

	for (;;)
	{
		IOUSBHostInterface* candidate = nullptr;
		result = ivars->m_usb_device->CopyInterface(iterator, &candidate);
		if (result != kIOReturnSuccess || candidate == nullptr) break;
		const IOUSBConfigurationDescriptor* configuration = candidate->CopyConfigurationDescriptor();
		const IOUSBInterfaceDescriptor* descriptor = configuration ? candidate->GetInterfaceDescriptor(configuration) : nullptr;
		if (descriptor && descriptor->bInterfaceNumber == kUSBInterfaceInput)
			ivars->m_usb_input_interface = OSSharedPtr(candidate, OSNoRetain);
		else if (descriptor && descriptor->bInterfaceNumber == kUSBInterfaceOutput)
			ivars->m_usb_output_interface = OSSharedPtr(candidate, OSNoRetain);
		else
			candidate->release();
		if (configuration) IOFree(const_cast<IOUSBConfigurationDescriptor*>(configuration), configuration->wTotalLength);
	}
	ivars->m_usb_device->DestroyInterfaceIterator(iterator);
	if (!ivars->m_usb_input_interface || !ivars->m_usb_output_interface) return kIOReturnNotFound;

	result = ivars->m_usb_input_interface->Open(ivars->m_driver.get(), 0, nullptr);
	if (result != kIOReturnSuccess) goto Failure;
	result = ivars->m_usb_output_interface->Open(ivars->m_driver.get(), 0, nullptr);
	if (result != kIOReturnSuccess) goto Failure;
	result = ivars->m_usb_input_interface->SelectAlternateSetting(kUSBAlternateStreaming);
	if (result != kIOReturnSuccess) goto Failure;
	result = ivars->m_usb_output_interface->SelectAlternateSetting(kUSBAlternateStreaming);
	if (result != kIOReturnSuccess) goto Failure;

	{
		IOUSBHostPipe* pipe = nullptr;
		result = ivars->m_usb_input_interface->CopyPipe(kUSBEndpointInput, &pipe);
		if (result != kIOReturnSuccess) goto Failure;
		ivars->m_usb_input_pipe = OSSharedPtr(pipe, OSNoRetain);
		pipe = nullptr;
		result = ivars->m_usb_output_interface->CopyPipe(kUSBEndpointOutput, &pipe);
		if (result != kIOReturnSuccess) goto Failure;
		ivars->m_usb_output_pipe = OSSharedPtr(pipe, OSNoRetain);
	}

	for (uint32_t direction = 0; direction < 2; ++direction)
	{
		for (uint32_t index = 0; index < kUSBRequestCount; ++index)
		{
			ERUSBRequest& request = direction == 0 ? ivars->m_usb_input_requests[index] : ivars->m_usb_output_requests[index];
			request.input = direction == 0;
			if (!request.dataDescriptor)
			{
				auto interface = request.input ? ivars->m_usb_input_interface.get() : ivars->m_usb_output_interface.get();
				IOOptionBits memoryDirection = request.input ? kIOMemoryDirectionIn : kIOMemoryDirectionOut;
				result = interface->CreateIOBuffer(memoryDirection, kUSBFramesPerRequest * kUSBMaxPacket, &request.dataDescriptor);
				if (result != kIOReturnSuccess) goto Failure;
				result = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut,
					kUSBFramesPerRequest * sizeof(IOUSBIsochronousFrame), 0, &request.frameDescriptor);
				if (result != kIOReturnSuccess) goto Failure;
				result = request.dataDescriptor->CreateMapping(0, 0, 0, 0, 0, &request.dataMap);
				if (result != kIOReturnSuccess) goto Failure;
				result = request.frameDescriptor->CreateMapping(0, 0, 0, 0, 0, &request.frameMap);
				if (result != kIOReturnSuccess) goto Failure;
				result = request.input ? CreateActionInputIsochComplete(sizeof(ERUSBRequest*), &request.action)
					: CreateActionOutputIsochComplete(sizeof(ERUSBRequest*), &request.action);
				if (result != kIOReturnSuccess) goto Failure;
				*reinterpret_cast<ERUSBRequest**>(request.action->GetReference()) = &request;
			}
		}
	}

	result = SetUSBHardwareRate(static_cast<uint32_t>(GetSampleRate()));
	if (result != kIOReturnSuccess) goto Failure;
	{
		uint64_t frame = 0, timestamp = 0;
		result = ivars->m_usb_device->GetFrameNumber(&frame, &timestamp);
		if (result != kIOReturnSuccess) goto Failure;
		ivars->m_usb_next_input_frame = frame + 25;
		ivars->m_usb_next_output_frame = frame + 25;
	}
	ivars->m_usb_input_sample_frame = 0;
	ivars->m_usb_output_sample_frame = 0;
	ivars->m_usb_output_accumulator = 0.0;
	ivars->m_usb_running = true;

	for (uint32_t index = 0; index < kUSBRequestCount; ++index)
	{
		InputIsochComplete_Impl(ivars->m_usb_input_requests[index].action, kIOReturnSuccess);
		OutputIsochComplete_Impl(ivars->m_usb_output_requests[index].action, kIOReturnSuccess);
	}
	return kIOReturnSuccess;

Failure:
	StopUSBTransport();
	return result;
}

void ElevenRackDevice::StopUSBTransport()
{
	ivars->m_usb_running = false;
	if (ivars->m_usb_input_pipe) ivars->m_usb_input_pipe->Abort(kIOUSBAbortSynchronous, kIOReturnAborted, nullptr);
	if (ivars->m_usb_output_pipe) ivars->m_usb_output_pipe->Abort(kIOUSBAbortSynchronous, kIOReturnAborted, nullptr);
	ivars->m_usb_input_pipe.reset();
	ivars->m_usb_output_pipe.reset();
	if (ivars->m_usb_input_interface)
	{
		ivars->m_usb_input_interface->SelectAlternateSetting(0);
		ivars->m_usb_input_interface->Close(ivars->m_driver.get(), 0);
	}
	if (ivars->m_usb_output_interface)
	{
		ivars->m_usb_output_interface->SelectAlternateSetting(0);
		ivars->m_usb_output_interface->Close(ivars->m_driver.get(), 0);
	}
	ivars->m_usb_input_interface.reset();
	ivars->m_usb_output_interface.reset();
}

void ElevenRackDevice::InputIsochComplete_Impl(OSAction* action, IOReturn status)
{
	auto reference = reinterpret_cast<ERUSBRequest**>(action->GetReference());
	ERUSBRequest* request = reference ? *reference : nullptr;
	if (!request || !ivars->m_usb_running) return;
	if (status != kIOReturnSuccess && status != kIOReturnUnderrun)
	{
		DebugMsg("Eleven Rack capture transfer failed: 0x%x", status);
		ivars->m_usb_running = false;
		return;
	}

	auto frames = reinterpret_cast<IOUSBIsochronousFrame*>(request->frameMap->GetAddress() + request->frameMap->GetOffset());
	auto usb = reinterpret_cast<const uint8_t*>(request->dataMap->GetAddress() + request->dataMap->GetOffset());
	auto audio = reinterpret_cast<float*>(ivars->m_input_memory_map->GetAddress() + ivars->m_input_memory_map->GetOffset());
	const uint64_t audioCapacityFrames = ivars->m_input_memory_map->GetLength() / (sizeof(float) * kInputChannels);

	for (uint32_t packet = 0; packet < kUSBFramesPerRequest; ++packet)
	{
		uint32_t bytes = frames[packet].completeCount;
		const uint8_t* source = usb + static_cast<size_t>(packet) * kUSBMaxPacket;
		uint32_t sampleFrames = bytes / (sizeof(int32_t) * kInputChannels);
		for (uint32_t frame = 0; frame < sampleFrames; ++frame)
		{
			uint64_t destinationFrame = ivars->m_usb_input_sample_frame++ % audioCapacityFrames;
			for (uint32_t channel = 0; channel < kInputChannels; ++channel)
			{
				int32_t sample = 0;
				std::memcpy(&sample, source + (frame * kInputChannels + channel) * sizeof(int32_t), sizeof(sample));
				audio[destinationFrame * kInputChannels + channel] = static_cast<float>(sample / 2147483648.0);
			}
		}
		frames[packet].status = 0;
		frames[packet].requestCount = kUSBMaxPacket;
		frames[packet].completeCount = 0;
		frames[packet].reserved = 0;
		frames[packet].timeStamp = 0;
	}

	status = ivars->m_usb_input_pipe->IsochIO(request->dataDescriptor, request->frameDescriptor,
		ivars->m_usb_next_input_frame, action);
	if (status == kIOReturnSuccess) ivars->m_usb_next_input_frame += kUSBFramesPerRequest / 8;
	else { DebugMsg("Unable to arm Eleven Rack capture: 0x%x", status); ivars->m_usb_running = false; }
}

void ElevenRackDevice::OutputIsochComplete_Impl(OSAction* action, IOReturn status)
{
	auto reference = reinterpret_cast<ERUSBRequest**>(action->GetReference());
	ERUSBRequest* request = reference ? *reference : nullptr;
	if (!request || !ivars->m_usb_running) return;
	if (status != kIOReturnSuccess && status != kIOReturnUnderrun)
	{
		DebugMsg("Eleven Rack playback transfer failed: 0x%x", status);
		ivars->m_usb_running = false;
		return;
	}

	auto frames = reinterpret_cast<IOUSBIsochronousFrame*>(request->frameMap->GetAddress() + request->frameMap->GetOffset());
	auto usb = reinterpret_cast<uint8_t*>(request->dataMap->GetAddress() + request->dataMap->GetOffset());
	auto audio = reinterpret_cast<const float*>(ivars->m_output_memory_map->GetAddress() + ivars->m_output_memory_map->GetOffset());
	const uint64_t audioCapacityFrames = ivars->m_output_memory_map->GetLength() / (sizeof(float) * kOutputChannels);
	const double rate = GetSampleRate();
	size_t byteOffset = 0;

	for (uint32_t packet = 0; packet < kUSBFramesPerRequest; ++packet)
	{
		ivars->m_usb_output_accumulator += rate / 8000.0;
		uint32_t sampleFrames = static_cast<uint32_t>(ivars->m_usb_output_accumulator);
		ivars->m_usb_output_accumulator -= sampleFrames;
		for (uint32_t frame = 0; frame < sampleFrames; ++frame)
		{
			uint64_t sourceFrame = ivars->m_usb_output_sample_frame++ % audioCapacityFrames;
			for (uint32_t channel = 0; channel < kOutputChannels; ++channel)
			{
				float value = audio[sourceFrame * kOutputChannels + channel];
				if (value > 1.0f) value = 1.0f;
				else if (value < -1.0f) value = -1.0f;
				int32_t sample = static_cast<int32_t>(value * 2147483647.0f);
				std::memcpy(usb + byteOffset + (frame * kOutputChannels + channel) * sizeof(int32_t), &sample, sizeof(sample));
			}
		}
		uint32_t bytes = sampleFrames * kOutputChannels * sizeof(int32_t);
		frames[packet].status = 0;
		frames[packet].requestCount = bytes;
		frames[packet].completeCount = 0;
		frames[packet].reserved = 0;
		frames[packet].timeStamp = 0;
		byteOffset += bytes;
	}

	status = ivars->m_usb_output_pipe->IsochIO(request->dataDescriptor, request->frameDescriptor,
		ivars->m_usb_next_output_frame, action);
	if (status == kIOReturnSuccess) ivars->m_usb_next_output_frame += kUSBFramesPerRequest / 8;
	else { DebugMsg("Unable to arm Eleven Rack playback: 0x%x", status); ivars->m_usb_running = false; }
}

bool ElevenRackDevice::init(IOUserAudioDriver* in_driver,
						   bool in_supports_prewarming,
						   OSString* in_device_uid,
						   OSString* in_model_uid,
						   OSString* in_manufacturer_uid,
						   uint32_t in_zero_timestamp_period)
{
	auto success = super::init(in_driver, in_supports_prewarming, in_device_uid, in_model_uid, in_manufacturer_uid, in_zero_timestamp_period);
	if (!success)
	{
		return false;
	}
	ivars = IONewZero(ElevenRackDevice_IVars, 1);
	if (ivars == nullptr)
	{
		return false;
	}

	IOOperationHandler io_operation = nullptr;
	IOReturn error = kIOReturnSuccess;

	ivars->m_driver = OSSharedPtr(in_driver, OSRetain);
	ivars->m_work_queue = GetWorkQueue();

	IOTimerDispatchSource* zts_timer_event_source = nullptr;
	OSAction* zts_timer_occurred_action = nullptr;

	OSSharedPtr<OSString> output_stream_name = OSSharedPtr(OSString::withCString("Eleven Rack Outputs (6 ch)"), OSNoRetain);

	OSSharedPtr<OSString> input_stream_name = OSSharedPtr(OSString::withCString("Eleven Rack Inputs (8 ch)"), OSNoRetain);
	OSSharedPtr<OSString> input_volume_control_name = OSSharedPtr(OSString::withCString("SimpleInputVolumeControl"), OSNoRetain);
	OSSharedPtr<OSString> input_data_source_control = OSSharedPtr(OSString::withCString("Input Tone Frequency Control"), OSNoRetain);

	// Custom property information.
	/// - Tag: CreateCustomProperty
	IOUserAudioObjectPropertyAddress prop_addr = {
		kElevenRackDriverCustomPropertySelector,
		IOUserAudioObjectPropertyScope::Global,
		IOUserAudioObjectPropertyElementMain };
	OSSharedPtr<IOUserAudioCustomProperty> custom_property = nullptr;
	OSSharedPtr<OSString> qualifier = nullptr;
	OSSharedPtr<OSString> data = nullptr;

	// Configure the device and add stream objects.
	auto data_source_0 = OSSharedPtr(OSString::withCString("Sine Tone 440"), OSNoRetain);
	auto data_source_1 = OSSharedPtr(OSString::withCString("Sine Tone 660"), OSNoRetain);
	auto data_source_2 = OSSharedPtr(OSString::withCString("Loopback"), OSNoRetain);
	ivars->m_data_sources[0] = { 440, data_source_0 };
	ivars->m_data_sources[1] = { 660, data_source_1 };
	ivars->m_data_sources[2] = { 0, data_source_2 };

	// Set up stream formats and other stream-related properties.
	/// - Tag: CreateStreamFormats
	SetAvailableSampleRates(kSupportedSampleRates, kSampleRateCount);
	SetSampleRate(kDefaultSampleRate);
	IOUserAudioChannelLabel input_channel_layout[kInputChannels] = {
		IOUserAudioChannelLabel::Left, IOUserAudioChannelLabel::Right,
		IOUserAudioChannelLabel::Left, IOUserAudioChannelLabel::Right,
		IOUserAudioChannelLabel::Left, IOUserAudioChannelLabel::Right,
		IOUserAudioChannelLabel::Left, IOUserAudioChannelLabel::Right
	};
	IOUserAudioChannelLabel output_channel_layout[kOutputChannels] = {
		IOUserAudioChannelLabel::Left, IOUserAudioChannelLabel::Right,
		IOUserAudioChannelLabel::Left, IOUserAudioChannelLabel::Right,
		IOUserAudioChannelLabel::Left, IOUserAudioChannelLabel::Right
	};

	IOUserAudioStreamBasicDescription input_stream_formats[] =
	{
		ER_FLOAT_FORMAT(44100.0, kInputChannels),
		ER_FLOAT_FORMAT(48000.0, kInputChannels),
		ER_FLOAT_FORMAT(88200.0, kInputChannels),
		ER_FLOAT_FORMAT(96000.0, kInputChannels)
	};
	IOUserAudioStreamBasicDescription output_stream_formats[] =
	{
		ER_FLOAT_FORMAT(44100.0, kOutputChannels),
		ER_FLOAT_FORMAT(48000.0, kOutputChannels),
		ER_FLOAT_FORMAT(88200.0, kOutputChannels),
		ER_FLOAT_FORMAT(96000.0, kOutputChannels)
	};

	// Add a custom property for the audio driver.
	/// - Tag: AddCustomProperty
	custom_property = IOUserAudioCustomProperty::Create(in_driver,
														prop_addr,
														true,
														IOUserAudioCustomPropertyDataType::String,
														IOUserAudioCustomPropertyDataType::String);

	// Set the qualifier and data-value pair on the custom property.
	qualifier = OSSharedPtr(OSString::withCString(kElevenRackDriverCustomPropertyQualifier0), OSNoRetain);
	data = OSSharedPtr(OSString::withCString(kElevenRackDriverCustomPropertyDataValue0), OSNoRetain);
	custom_property->SetQualifierAndDataValue(qualifier.get(), data.get());

	// Set another qualifier and data-value pair on the custom property.
	qualifier = OSSharedPtr(OSString::withCString(kElevenRackDriverCustomPropertyQualifier1), OSNoRetain);
	data = OSSharedPtr(OSString::withCString(kElevenRackDriverCustomPropertyDataValue1), OSNoRetain);
	custom_property->SetQualifierAndDataValue(qualifier.get(), data.get());
	AddCustomProperty(custom_property.get());

	// Create the IOBufferMemoryDescriptor ring buffer for the input stream.
	/// - Tag: CreateRingBufferAndMemoryDescriptor
	OSSharedPtr<IOBufferMemoryDescriptor> output_io_ring_buffer;
	OSSharedPtr<IOBufferMemoryDescriptor> input_io_ring_buffer;
	IOBufferMemoryDescriptor* created_output_ring_buffer = nullptr;
	IOBufferMemoryDescriptor* created_input_ring_buffer = nullptr;
	const auto output_buffer_size_bytes = static_cast<uint32_t>(in_zero_timestamp_period * sizeof(float) * kOutputChannels);
	const auto input_buffer_size_bytes = static_cast<uint32_t>(in_zero_timestamp_period * sizeof(float) * kInputChannels);
	error = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, output_buffer_size_bytes, 0, &created_output_ring_buffer);
	output_io_ring_buffer.reset(created_output_ring_buffer, OSRetain);
	OSSafeReleaseNULL(created_output_ring_buffer);
	FailIf(error != kIOReturnSuccess, , Failure, "Failed to create output IOBufferMemoryDescriptor");

	error = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, input_buffer_size_bytes, 0, &created_input_ring_buffer);
	input_io_ring_buffer.reset(created_input_ring_buffer, OSRetain);
	OSSafeReleaseNULL(created_input_ring_buffer);
	FailIf(error != kIOReturnSuccess, , Failure, "Failed to create input IOBufferMemoryDescriptor");

	// Create an output/input stream object and pass in the I/O ring buffer memory descriptor.
	ivars->m_output_stream = IOUserAudioStream::Create(in_driver, IOUserAudioStreamDirection::Output, output_io_ring_buffer.get());
	FailIfNULL(ivars->m_output_stream.get(), error = kIOReturnNoMemory, Failure, "failed to create output stream");

	ivars->m_input_stream = IOUserAudioStream::Create(in_driver, IOUserAudioStreamDirection::Input, input_io_ring_buffer.get());
	FailIfNULL(ivars->m_input_stream.get(), error = kIOReturnNoMemory, Failure, "failed to create input stream");

	//	Configure stream properties: name, available formats, and current format.
	ivars->m_output_stream->SetName(output_stream_name.get());
	ivars->m_output_stream->SetAvailableStreamFormats(output_stream_formats, kSampleRateCount);
	ivars->m_stream_format = output_stream_formats[1];
	ivars->m_output_stream->SetCurrentStreamFormat(&ivars->m_stream_format);

	ivars->m_input_stream->SetName(input_stream_name.get());
	ivars->m_input_stream->SetAvailableStreamFormats(input_stream_formats, kSampleRateCount);
	ivars->m_input_stream->SetCurrentStreamFormat(&input_stream_formats[1]);

	// Publish stable, human-readable names for every physical Core Audio element.
	// Keep these tied to USB lane order; changing labels would silently reroute DAW
	// sessions even though the numeric channel count stayed the same.
	for (uint32_t channel = 0; channel < kInputChannels; ++channel)
	{
		auto name = OSSharedPtr(OSString::withCString(kInputChannelNames[channel]), OSNoRetain);
		error = ivars->m_input_stream->SetElementName(channel + 1,
			IOUserAudioObjectPropertyScope::Input, name.get());
		FailIfError(error, , Failure, "failed to name Eleven Rack input channel");
	}
	for (uint32_t channel = 0; channel < kOutputChannels; ++channel)
	{
		auto name = OSSharedPtr(OSString::withCString(kOutputChannelNames[channel]), OSNoRetain);
		error = ivars->m_output_stream->SetElementName(channel + 1,
			IOUserAudioObjectPropertyScope::Output, name.get());
		FailIfError(error, , Failure, "failed to name Eleven Rack output channel");
	}

	// Add a stream object to the driver.
	error = AddStream(ivars->m_output_stream.get());
	FailIfError(error, , Failure, "failed to add output stream");

	error = AddStream(ivars->m_input_stream.get());
	FailIfError(error, , Failure, "failed to add input stream");

	/// - Tag: AddVolumeControlObject
	// Create the volume control object for the input stream.
	ivars->m_input_volume_control = IOUserAudioLevelControl::Create(in_driver,
																	true,
																	-6.0,
																	{-96.0, 0.0},
																	IOUserAudioObjectPropertyElementMain,
																	IOUserAudioObjectPropertyScope::Input,
																	IOUserAudioClassID::VolumeControl);
	FailIfNULL(ivars->m_input_volume_control.get(), error = kIOReturnNoMemory, Failure, "Failed to create input volume control");
	ivars->m_input_volume_control->SetName(input_volume_control_name.get());

	// Add the volume control to the device object.
	error = AddControl(ivars->m_input_volume_control.get());
	FailIfError(error, , Failure, "failed to add input volume level control");

	// Create the input data source selector control for controlling the sine tone frequency.
	ivars->m_input_selector_control = IOUserAudioSelectorControl::Create(in_driver,
																		 true,
																		 IOUserAudioObjectPropertyElementMain,
																		 IOUserAudioObjectPropertyScope::Input,
																		 IOUserAudioClassID::DataSourceControl);
	FailIfNULL(ivars->m_input_selector_control.get(), error = kIOReturnNoMemory, Failure, "Failed to create input data source control");
	ivars->m_input_selector_control->AddControlValueDescriptions(ivars->m_data_sources, 3);
	// Set the data source selector's current value to tone with a frequency of 440 Hz.
	ivars->m_input_selector_control->SetCurrentSelectedValues(&ivars->m_data_sources[0].m_value, 1);
	ivars->m_input_selector_control->SetName(input_data_source_control.get());

	// Add the data-source selector control to the driver.
	error = AddControl(ivars->m_input_selector_control.get());
	FailIfError(error, , Failure, "failed to add input data source control");

	// Configure device-related information.
	SetPreferredOutputChannelLayout(output_channel_layout, kOutputChannels);
	SetTransportType(IOUserAudioTransportType::USB);

	SetPreferredInputChannelLayout(input_channel_layout, kInputChannels);
	SetTransportType(IOUserAudioTransportType::USB);

	/// - Tag: InitZtsTimer
	// Initialize the timer that stands in for a real interrupt.
	error = IOTimerDispatchSource::Create(ivars->m_work_queue.get(), &zts_timer_event_source);
	FailIfError(error, , Failure, "failed to create the ZTS timer event source");
	ivars->m_zts_timer_event_source = OSSharedPtr(zts_timer_event_source, OSNoRetain);

	// Create a timer action to generate timestamps.
	error = CreateActionZtsTimerOccurred(sizeof(void*), &zts_timer_occurred_action);
	FailIfError(error, , Failure, "failed to create the timer event source action");
	ivars->m_zts_timer_occurred_action = OSSharedPtr(zts_timer_occurred_action, OSNoRetain);
	ivars->m_zts_timer_event_source->SetHandler(ivars->m_zts_timer_occurred_action.get());

	/// - Tag: CreateRealTimeAudioCallback
	io_operation = ^kern_return_t(IOUserAudioObjectID in_device,
								  IOUserAudioIOOperation in_io_operation,
								  uint32_t in_io_buffer_frame_size,
								  uint64_t in_sample_time,
								  uint64_t in_host_time)
	{
		if (in_io_operation == IOUserAudioIOOperationWriteEnd)
		{
			// no-op, Host has written data to the output buffer
		}
		else if (in_io_operation == IOUserAudioIOOperationBeginRead)
		{
			// USB capture writes directly into the AudioDriverKit input ring.
		}

		return kIOReturnSuccess;
	};

	/// - Tag: SetRealTimeAudioCallback
	this->SetIOOperationHandler(io_operation);

	return true;

Failure:
	ivars->m_driver.reset();
	ivars->m_output_stream.reset();
	ivars->m_output_memory_map.reset();
	ivars->m_input_stream.reset();
	ivars->m_input_memory_map.reset();
	ivars->m_input_volume_control.reset();
	ivars->m_zts_timer_event_source.reset();
	ivars->m_zts_timer_occurred_action.reset();
	return false;
}

void ElevenRackDevice::free()
{
	if (ivars != nullptr)
	{
		StopUSBTransport();
		for (uint32_t index = 0; index < kUSBRequestCount; ++index)
		{
			ERReleaseUSBRequest(ivars->m_usb_input_requests[index]);
			ERReleaseUSBRequest(ivars->m_usb_output_requests[index]);
		}
		ivars->m_usb_device.reset();
		ivars->m_driver.reset();
		ivars->m_output_stream.reset();
		ivars->m_output_memory_map.reset();
		ivars->m_input_stream.reset();
		ivars->m_input_memory_map.reset();
		ivars->m_input_volume_control.reset();
		ivars->m_input_selector_control.reset();
		ivars->m_zts_timer_event_source.reset();
		ivars->m_zts_timer_occurred_action.reset();
		ivars->m_work_queue.reset();
	}
	IOSafeDeleteNULL(ivars, ElevenRackDevice_IVars, 1);
	super::free();
}

/// - Tag: StartIOImpl
kern_return_t ElevenRackDevice::StartIO(IOUserAudioStartStopFlags in_flags)
{
	DebugMsg("Start I/O: device %u", GetObjectID());

	__block kern_return_t error = kIOReturnSuccess;

	ivars->m_work_queue->DispatchSync(^(){
		OSSharedPtr<IOMemoryDescriptor> input_iomd;
		OSSharedPtr<IOMemoryDescriptor> output_iomd;
		OSSharedPtr<IOMemoryMap> input_memory_map;
		OSSharedPtr<IOMemoryMap> output_memory_map;
		IOMemoryMap* created_output_memory_map = nullptr;
		IOMemoryMap* created_input_memory_map = nullptr;

		//	Tell IOUserAudioObject base class to start I/O for the device.
		error = super::StartIO(in_flags);
		FailIfError(error, , Failure, "Failed to start I/O");

		output_iomd = ivars->m_output_stream->GetIOMemoryDescriptor();
		FailIfNULL(output_iomd.get(), error = kIOReturnNoMemory, Failure, "Failed to get output stream IOMemoryDescriptor");
		error = output_iomd->CreateMapping(0, 0, 0, 0, 0, &created_output_memory_map);
		output_memory_map.reset(created_output_memory_map, OSRetain);
		OSSafeReleaseNULL(created_output_memory_map);
		FailIf(error != kIOReturnSuccess, , Failure, "Failed to create memory map from output stream IOMemoryDescriptor");

		input_iomd = ivars->m_input_stream->GetIOMemoryDescriptor();
		FailIfNULL(input_iomd.get(), error = kIOReturnNoMemory, Failure, "Failed to get input stream IOMemoryDescriptor");
		error = input_iomd->CreateMapping(0, 0, 0, 0, 0, &created_input_memory_map);
		input_memory_map.reset(created_input_memory_map, OSRetain);
		OSSafeReleaseNULL(created_input_memory_map);
		FailIf(error != kIOReturnSuccess, , Failure, "Failed to create memory map from input stream IOMemoryDescriptor");

		ivars->m_output_memory_map = std::move(output_memory_map);
		ivars->m_input_memory_map = std::move(input_memory_map);

		error = StartUSBTransport();
		FailIfError(error, , Failure, "Failed to start Eleven Rack USB transport");

		// Start the timers to send timestamps and generate sine tone on the stream I/O buffer.
		StartTimers();
		return;

	Failure:
		super::StopIO(in_flags);
		ivars->m_output_memory_map.reset();
		ivars->m_input_memory_map.reset();
		return;
	});

	return error;
}

kern_return_t ElevenRackDevice::StopIO(IOUserAudioStartStopFlags in_flags)
{
	DebugMsg("Stop IO: device %u", GetObjectID());

	// Tell the IOUserAudioObject base class to stop I/O for the device.
	__block kern_return_t error;

	ivars->m_work_queue->DispatchSync(^(){
		// Stop the timers for timestamps and sine tone generator.
		StopUSBTransport();
		StopTimers();

		error = super::StopIO(in_flags);
	});


	if (error != kIOReturnSuccess)
   {
	   DebugMsg("Failed to stop IO, error %d", error);
   }

	return error;
}

/// - Tag: PerformDeviceConfigurationChange
kern_return_t ElevenRackDevice::PerformDeviceConfigurationChange(uint64_t change_action, OSObject* in_change_info)
{
	DebugMsg("change action %llu", change_action);
	kern_return_t ret = kIOReturnSuccess;
	switch (change_action) {
			// Add custom config change handlers.
		case k_custom_config_change_action:
		{
			if (in_change_info)
			{
				auto change_info_string = OSDynamicCast(OSString, in_change_info);
				DebugMsg("%s", change_info_string->getCStringNoCopy());
			}

			// Cycle the control app through every hardware clock rate.
			double current_rate = GetSampleRate();
			double rate_to_set = kSupportedSampleRates[0];
			for (uint32_t i = 0; i < kSampleRateCount; ++i)
			{
				if (static_cast<uint32_t>(current_rate) == static_cast<uint32_t>(kSupportedSampleRates[i]))
				{
					rate_to_set = kSupportedSampleRates[(i + 1) % kSampleRateCount];
					break;
				}
			}
			ret = SetSampleRate(rate_to_set);
			if (ret == kIOReturnSuccess)
			{
				// Update the stream formats with the new rate.
				ret = ivars->m_input_stream->DeviceSampleRateChanged(rate_to_set);
				if (ret == kIOReturnSuccess)
				{
					ret = ivars->m_output_stream->DeviceSampleRateChanged(rate_to_set);
				}
			}
		}
			break;

		default:
			ret = super::PerformDeviceConfigurationChange(change_action, in_change_info);
			break;
	}

	// Update the cached format.
	ivars->m_stream_format = ivars->m_input_stream->GetCurrentStreamFormat();

	return ret;
}

kern_return_t ElevenRackDevice::AbortDeviceConfigurationChange(uint64_t change_action, OSObject* in_change_info)
{
	// Handle aborted configuration changes as necessary.
	return super::AbortDeviceConfigurationChange(change_action, in_change_info);
}

kern_return_t ElevenRackDevice::HandleChangeSampleRate(double in_sample_rate)
{
	bool supported = false;
	for (uint32_t i = 0; i < kSampleRateCount; ++i)
		supported |= static_cast<uint32_t>(in_sample_rate) == static_cast<uint32_t>(kSupportedSampleRates[i]);
	if (!supported) return kIOReturnUnsupported;

	// The USB transport applies the matching UAC SET_CUR clock request while
	// I/O is stopped. Core Audio never runs input and output at different rates.
	auto result = SetUSBHardwareRate(static_cast<uint32_t>(in_sample_rate));
	if (result != kIOReturnSuccess) return result;
	result = SetSampleRate(in_sample_rate);
	if (result == kIOReturnSuccess)
	{
		result = ivars->m_input_stream->DeviceSampleRateChanged(in_sample_rate);
		if (result == kIOReturnSuccess)
		{
			result = ivars->m_output_stream->DeviceSampleRateChanged(in_sample_rate);
		}
		if (result == kIOReturnSuccess)
		{
			ivars->m_stream_format = ivars->m_input_stream->GetCurrentStreamFormat();
			UpdateTimers();
		}
	}
	return result;
}

inline int16_t ElevenRackDevice::FloatToInt16(float in_sample)
{
	if (in_sample > 1.0f)
	{
		in_sample = 1.0f;
	}
	else if (in_sample < -1.0f)
	{
		in_sample = -1.0f;
	}
	return static_cast<int16_t>(in_sample * 0x7fff);
}

kern_return_t ElevenRackDevice::StartTimers()
{
	kern_return_t error = kIOReturnSuccess;

	UpdateTimers();

	if(ivars->m_zts_timer_event_source.get() != nullptr)
	{
		/// - Tag: StartTimers
		// Clear the device's timestamps.
		UpdateCurrentZeroTimestamp(0, 0);
		auto current_time = mach_absolute_time();

		// Start the timer. The first timestamp occurs when the timer goes off.
		ivars->m_zts_timer_event_source->WakeAtTime(kIOTimerClockMachAbsoluteTime, current_time + ivars->m_zts_host_ticks_per_buffer, 0);
		ivars->m_zts_timer_event_source->SetEnable(true);
	}
	else
	{
		error = kIOReturnNoResources;
	}

	return error;
}

void	ElevenRackDevice::StopTimers()
{
	if(ivars->m_zts_timer_event_source.get() != nullptr)
	{
		ivars->m_zts_timer_event_source->SetEnable(false);
	}
}

void	ElevenRackDevice::UpdateTimers()
{
	struct mach_timebase_info timebase_info;
	mach_timebase_info(&timebase_info);

	double sample_rate = ivars->m_stream_format.mSampleRate;
	double host_ticks_per_buffer = static_cast<double>(GetZeroTimestampPeriod() * NSEC_PER_SEC) / sample_rate;
	host_ticks_per_buffer = (host_ticks_per_buffer * static_cast<double>(timebase_info.denom)) / static_cast<double>(timebase_info.numer);
	ivars->m_zts_host_ticks_per_buffer = static_cast<uint64_t>(host_ticks_per_buffer);
}

/// - Tag: ZtsTimerOccurred
void	ElevenRackDevice::ZtsTimerOccurred_Impl(OSAction* action, uint64_t time)
{
	// Get the current time.
	auto current_time = time;

	// Increment the timestamps...
	uint64_t current_sample_time = 0;
	uint64_t current_host_time = 0;
	GetCurrentZeroTimestamp(&current_sample_time, &current_host_time);

	auto host_ticks_per_buffer = ivars->m_zts_host_ticks_per_buffer;

	if(current_host_time != 0)
	{
		current_sample_time += GetZeroTimestampPeriod();
		current_host_time += host_ticks_per_buffer;
	}
	else
	{
		// ...but not if it's the first one.
		current_sample_time = 0;
		current_host_time = current_time;
	}

	// Update the device with the current timestamp.
	UpdateCurrentZeroTimestamp(current_sample_time, current_host_time);

	// Set the timer to go off in one buffer.
	ivars->m_zts_timer_event_source->WakeAtTime(kIOTimerClockMachAbsoluteTime,
												current_host_time + host_ticks_per_buffer, 0);
}

/// - Tag: GenerateToneForInput
void ElevenRackDevice::GenerateToneForInput(double in_tone_freq, size_t in_sample_time, size_t in_frame_size)
{
	// Fill out the input buffer with a sine tone.
	if (ivars->m_input_memory_map)
	{
		// Get the pointer to the I/O buffer and use stream format information
		// to get the buffer length.
		const auto& format = ivars->m_stream_format;
		auto buffer_length = ivars->m_input_memory_map->GetLength() / (format.mBytesPerFrame / format.mChannelsPerFrame);
		auto num_samples = in_frame_size;
		auto buffer = reinterpret_cast<int16_t*>(ivars->m_input_memory_map->GetAddress() + ivars->m_input_memory_map->GetOffset());

		// Get the volume control dB value to apply gain to the tone.
		auto input_volume_level = ivars->m_input_volume_control->GetScalarValue();

		for(size_t i = 0; i < num_samples; i++)
		{
			float float_value = input_volume_level * sin(2.0 * M_PI * in_tone_freq * static_cast<double>(ivars->m_tone_sample_index) / format.mSampleRate);
			int16_t integer_value = FloatToInt16(float_value);
			for (auto channel_index = 0; channel_index < format.mChannelsPerFrame; channel_index++)
			{
				auto buffer_index = (format.mChannelsPerFrame * (in_sample_time + i) + channel_index) % buffer_length;
				buffer[buffer_index] = integer_value;
			}
			ivars->m_tone_sample_index += 1;
		}
	}
}

kern_return_t ElevenRackDevice::ToggleDataSource()
{
	__block kern_return_t ret = kIOReturnSuccess;
	GetWorkQueue()->DispatchSync(^(){
		IOUserAudioSelectorValue current_data_source_value;
		ivars->m_input_selector_control->GetCurrentSelectedValues(&current_data_source_value, 1);


		IOUserAudioSelectorValue data_source_value_to_set = current_data_source_value;
		if (current_data_source_value == ivars->m_data_sources[0].m_value)
		{
			data_source_value_to_set = ivars->m_data_sources[1].m_value;
		}
		else if (current_data_source_value == ivars->m_data_sources[1].m_value)
		{
			data_source_value_to_set = ivars->m_data_sources[2].m_value;
		}
		else
		{
			data_source_value_to_set = ivars->m_data_sources[0].m_value;
		}
		ret = ivars->m_input_selector_control->SetCurrentSelectedValues(&data_source_value_to_set, 1);
	});
	return ret;
}

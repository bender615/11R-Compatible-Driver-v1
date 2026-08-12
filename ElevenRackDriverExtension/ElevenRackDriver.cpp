/*
See the LICENSE.txt file for this sample’s licensing information.

Abstract:
The implementation of the driver, which manages communications
             between user clients and the audio device.
*/

// Self Include
#include "ElevenRackDriver.h"

// Local Include
#include "ElevenRackDevice.h"
#include "ElevenRackDriverUserClient.h"
#include "ElevenRackDriverKeys.h"

// System Include
#include <AudioDriverKit/AudioDriverKit.h>
#include <DriverKit/IOUserServer.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSString.h>
#include <DriverKit/IODispatchQueue.h>
#include <USBDriverKit/IOUSBHostDevice.h>

constexpr uint32_t k_zero_time_stamp_period = 32768;

struct ElevenRackDriver_IVars
{
	OSSharedPtr<IODispatchQueue>	m_work_queue;
	OSSharedPtr<ElevenRackDevice>	m_simple_audio_device;
	OSSharedPtr<IOUSBHostDevice>	m_usb_device;
};

bool ElevenRackDriver::init()
{
	auto answer = super::init();
	if (!answer)
	{
		return false;
	}
	ivars = new ElevenRackDriver_IVars();
	if (ivars == nullptr)
	{
		return false;
	}

	return true;
}

void ElevenRackDriver::free()
{
	if (ivars != nullptr)
	{
		ivars->m_work_queue.reset();
		ivars->m_simple_audio_device.reset();
		ivars->m_usb_device.reset();
	}
	IOSafeDeleteNULL(ivars, ElevenRackDriver_IVars, 1);
	super::free();
}

/// - Tag: StartImpl
kern_return_t ElevenRackDriver::Start_Impl(IOService* in_provider)
{
	bool success = false;
	auto device_uid = OSSharedPtr(OSString::withCString(kElevenRackDriverDeviceUID), OSNoRetain);
	auto model_uid = OSSharedPtr(OSString::withCString("ElevenRackDevice-Model"), OSNoRetain);
	auto manufacturer_uid = OSSharedPtr(OSString::withCString("Avid"), OSNoRetain);
	auto device_name = OSSharedPtr(OSString::withCString("Eleven Rack"), OSNoRetain);
	IOUSBHostDevice* usb_device = nullptr;
	bool usb_device_open = false;

	kern_return_t error = Start(in_provider, SUPERDISPATCH);
	FailIfError(error, , Failure, "Failed to start Super");
	usb_device = OSDynamicCast(IOUSBHostDevice, in_provider);
	FailIfNULL(usb_device, error = kIOReturnUnsupported, Failure,
			   "Eleven Rack driver requires an IOUSBHostDevice provider");
	ivars->m_usb_device = OSSharedPtr(usb_device, OSRetain);
	error = usb_device->Open(this, 0, 0);
	FailIfError(error, , Failure, "Failed to open Eleven Rack USB device");
	usb_device_open = true;

	// Get the service's default dispatch queue from the driver object.
	ivars->m_work_queue = GetWorkQueue();
	FailIfError(ivars->m_work_queue.get() == nullptr, error = kIOReturnInvalid, Failure, "failed to get default work queue");

	// Allocate and configure audio devices as necessary.
	ivars->m_simple_audio_device = OSSharedPtr(OSTypeAlloc(ElevenRackDevice), OSNoRetain);
	FailIfNULL(ivars->m_simple_audio_device.get(), error = kIOReturnNoMemory, Failure, "Failed to allocate ElevenRackDevice");

	success = ivars->m_simple_audio_device->init(this, false, device_uid.get(), model_uid.get(), manufacturer_uid.get(), k_zero_time_stamp_period);
	FailIf(success == false, error = kIOReturnNoMemory, Failure, "Failed to init ElevenRackDevice");
	ivars->m_simple_audio_device->ConfigureUSBDevice(usb_device);

	ivars->m_simple_audio_device->SetName(device_name.get());

	// Add the device object to the driver.
	AddObject(ivars->m_simple_audio_device.get());

	// Register the service.
	error = RegisterService();
	FailIfError(error, , Failure, "failed to register service!");

	return kIOReturnSuccess;

Failure:
	if (usb_device_open && usb_device) usb_device->Close(this, 0);
	ivars->m_simple_audio_device.reset();
	ivars->m_work_queue.reset();
	ivars->m_usb_device.reset();
	return error;
}

kern_return_t	ElevenRackDriver::Stop_Impl(IOService* in_provider)
{
	if (ivars->m_usb_device) ivars->m_usb_device->Close(this, 0);
	auto ret = Stop(in_provider, SUPERDISPATCH);
	ivars->m_work_queue.reset();
	ivars->m_simple_audio_device.reset();
	ivars->m_usb_device.reset();
	return ret;
}


/// - Tag: NewUserClientImpl
kern_return_t ElevenRackDriver::NewUserClient_Impl(uint32_t in_type, IOUserClient** out_user_client)
{
	kern_return_t error = kIOReturnSuccess;
	*out_user_client = nullptr;

	// Have the superclass create the IOUserAudioDriverUserClient object
	// if the type is kIOUserAudioDriverUserClientType.
	if (in_type == kIOUserAudioDriverUserClientType)
	{
		error = super::NewUserClient(in_type, out_user_client, SUPERDISPATCH);
		FailIfError(error, , Failure, "Failed to create user client");
		FailIfNULL(*out_user_client, error = kIOReturnNoMemory, Failure, "Failed to create user client");
	}
	else
	{
		IOService* created_service = nullptr;
		error = Create(this, "ElevenRackDriverUserClientProperties", &created_service);
		// Create returns a +1 reference. Give the shared pointer its own retain,
		// then explicitly balance Create's reference in this scope.
		OSSharedPtr<IOService> user_client_service(created_service, OSRetain);
		OSSafeReleaseNULL(created_service);
		FailIfError(error, , Failure, "failed to create the ElevenRackDriver user client");

		// Create returns a +1 IOService. Retain the typed reference, allow the
		// service wrapper to release its ownership, and transfer the typed +1
		// reference through the NewUserClient output parameter.
		OSSharedPtr<IOUserClient> user_client(
			OSDynamicCast(IOUserClient, user_client_service.get()), OSRetain);
		FailIfNULL(user_client.get(), error = kIOReturnBadArgument, Failure,
			"created service is not an IOUserClient");
		*out_user_client = user_client.detach();
	}

Failure:
	return error;
}

kern_return_t ElevenRackDriver::StartDevice(IOUserAudioObjectID in_object_id, IOUserAudioStartStopFlags in_flags)
{
	if (in_object_id != ivars->m_simple_audio_device->GetObjectID())
	{
		DebugMsg("ElevenRackDriver::StartDevice - unknown object id %u", in_object_id);
		return kIOReturnBadArgument;
	}

	__block kern_return_t ret;
	ivars->m_work_queue->DispatchSync(^(){
		// Tell the superclass to start the device transport and watchdog.
		ret = super::StartDevice(in_object_id, in_flags);
	});
	if (ret == kIOReturnSuccess)
	{
		// Enable any custom driver-related things here.
	}
	return ret;
}

kern_return_t ElevenRackDriver::StopDevice(IOUserAudioObjectID in_object_id, IOUserAudioStartStopFlags in_flags)
{
	if (in_object_id != ivars->m_simple_audio_device->GetObjectID())
	{
		DebugMsg("ElevenRackDriver::StopDevice - unknown object id %u", in_object_id);
		return kIOReturnBadArgument;
	}

	// Tell the superclass to stop the device transport and watchdog.
	__block kern_return_t ret;
	ivars->m_work_queue->DispatchSync(^(){
		ret = super::StopDevice(in_object_id, in_flags);
	});

	if (ret == kIOReturnSuccess)
	{
	// Disable any custom driver-related things here.
	}
	return ret;
}

kern_return_t ElevenRackDriver::HandleToggleDataSource()
{
	// Retain the legacy selector ordinal for user-client ABI compatibility,
	// but do not publish the sample driver's artificial tone selector.
	return kIOReturnUnsupported;
}

/// - Tag: HandleTestConfigChange
kern_return_t ElevenRackDriver::HandleTestConfigChange()
{
	auto change_info = OSSharedPtr(OSString::withCString("Toggle Sample Rate"), OSNoRetain);
	return ivars->m_simple_audio_device->RequestDeviceConfigurationChange(k_custom_config_change_action, change_info.get());
}

kern_return_t ElevenRackDriver::HandleSetClockSource(uint32_t source)
{
	__block kern_return_t ret = kIOReturnSuccess;
	ivars->m_work_queue->DispatchSync(^(){
		ret = ivars->m_simple_audio_device->SetClockSource(source);
	});
	return ret;
}

kern_return_t ElevenRackDriver::HandleGetAudioStatus(uint32_t* source,
	uint32_t* hardware_rate, uint32_t* clock_valid, uint32_t* streaming)
{
	if (!source || !hardware_rate || !clock_valid || !streaming) return kIOReturnBadArgument;
	__block kern_return_t ret = kIOReturnSuccess;
	ivars->m_work_queue->DispatchSync(^(){
		ret = ivars->m_simple_audio_device->GetAudioStatus(source, hardware_rate,
			clock_valid, streaming);
	});
	return ret;
}

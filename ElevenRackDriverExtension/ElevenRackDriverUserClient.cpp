/*
See the LICENSE.txt file for this sample’s licensing information.

Abstract:
The implementation of the user client, which connects to and
            exercises the driver.
*/

//	Local Include
#include "ElevenRackDriverUserClient.h"
#include "ElevenRackDriver.h"
#include "ElevenRackDriverKeys.h"

//	System Includes
#include <DriverKit/DriverKit.h>
#include <DriverKit/OSSharedPtr.h>
#include <AudioDriverKit/AudioDriverKit.h>

struct ElevenRackDriverUserClient_IVars
{
	OSSharedPtr<ElevenRackDriver>	m_provider = nullptr;
};

bool	ElevenRackDriverUserClient::init()
{
	auto theAnswer = super::init();
	if (!theAnswer)
	{
		return false;
	}
	ivars = IONewZero(ElevenRackDriverUserClient_IVars, 1);
	if (ivars == nullptr)
	{
		return false;
	}

	return true;
}

void	ElevenRackDriverUserClient::free()
{
	if (ivars != nullptr)
	{
		ivars->m_provider.reset();
	}
	IOSafeDeleteNULL(ivars, ElevenRackDriverUserClient_IVars, 1);
	super::free();
}

kern_return_t	ElevenRackDriverUserClient::Start_Impl(IOService* in_provider)
{
	kern_return_t ret = kIOReturnSuccess;
	FailIfNULL(in_provider, ret = kIOReturnBadArgument, Failure, "provider is null!");

	ret = Start(in_provider, SUPERDISPATCH);
	FailIfError(ret, , Failure, "Failed to start super!");

	ivars->m_provider = OSSharedPtr(OSDynamicCast(ElevenRackDriver, in_provider), OSRetain);

	return kIOReturnSuccess;

Failure:
	ivars->m_provider.reset();
	return ret;
}

kern_return_t	ElevenRackDriverUserClient::Stop_Impl(IOService* in_provider)
{
	auto result = Stop(in_provider, SUPERDISPATCH);
	ivars->m_provider.reset();
	return result;
}

kern_return_t	ElevenRackDriverUserClient::ExternalMethod(uint64_t in_selector,
															IOUserClientMethodArguments* in_arguments,
															const IOUserClientMethodDispatch* in_dispatch,
															OSObject* in_target,
															void* in_reference)
{
	kern_return_t ret = kIOReturnSuccess;

	if (ivars == nullptr)
	{
		return kIOReturnNoResources;
	}
	if (ivars->m_provider.get() == nullptr)
	{
		return kIOReturnNotAttached;
	}

	switch(static_cast<ElevenRackDriverExternalMethod>(in_selector))
	{
		case ElevenRackDriverExternalMethod_Open:
		{
			ret = kIOReturnSuccess;
			break;
		}

		case ElevenRackDriverExternalMethod_Close:
		{
			ret = kIOReturnSuccess;
			break;
		}

		case ElevenRackDriverExternalMethod_ToggleDataSource:
		{
			ret = ivars->m_provider->HandleToggleDataSource();
			break;
		}

		case ElevenRackDriverExternalMethod_TestConfigChange:
		{
			ret = ivars->m_provider->HandleTestConfigChange();
			break;
		}

		case ElevenRackDriverExternalMethod_SetClockSource:
		{
			if (in_arguments == nullptr || in_arguments->scalarInput == nullptr ||
				in_arguments->scalarInputCount != 1)
				return kIOReturnBadArgument;
			ret = ivars->m_provider->HandleSetClockSource(
				static_cast<uint32_t>(in_arguments->scalarInput[0]));
			break;
		}

		case ElevenRackDriverExternalMethod_GetAudioStatus:
		{
			if (in_arguments == nullptr || in_arguments->scalarOutput == nullptr ||
				in_arguments->scalarOutputCount < 4)
				return kIOReturnNoSpace;
			uint32_t source = 0;
			uint32_t hardware_rate = 0;
			uint32_t clock_valid = ElevenRackClockValidity_Unknown;
			uint32_t streaming = 0;
			ret = ivars->m_provider->HandleGetAudioStatus(&source, &hardware_rate,
				&clock_valid, &streaming);
			if (ret == kIOReturnSuccess)
			{
				in_arguments->scalarOutput[0] = source;
				in_arguments->scalarOutput[1] = hardware_rate;
				in_arguments->scalarOutput[2] = clock_valid;
				in_arguments->scalarOutput[3] = streaming;
				in_arguments->scalarOutputCount = 4;
			}
			break;
		}

		default:
			ret = super::ExternalMethod(in_selector, in_arguments, in_dispatch, in_target, in_reference);
	};

	return ret;
}

/*
See the LICENSE.txt file for this sample’s licensing information.

Abstract:
The constants for identifiers that both the user client and the driver use.
*/

#ifndef ElevenRackDriverKeys_h
#define ElevenRackDriverKeys_h

#define kElevenRackDriverClassName "ElevenRackDriver"
#define kElevenRackDriverDeviceUID "ElevenRackDevice-UID"

#define kElevenRackDriverCustomPropertySelector 'sadc'
#define kElevenRackDriverCustomPropertyQualifier0 "Qualifier-0"
#define kElevenRackDriverCustomPropertyQualifier1 "Qualifier-1"
#define kElevenRackDriverCustomPropertyDataValue0 "Default-0"
#define kElevenRackDriverCustomPropertyDataValue1 "Default-1"

enum ElevenRackDriverExternalMethod
{
    ElevenRackDriverExternalMethod_Open, // No arguments.
    ElevenRackDriverExternalMethod_Close, // No arguments.
    ElevenRackDriverExternalMethod_ToggleDataSource, // No argument. This switches between data source selection.
    ElevenRackDriverExternalMethod_TestConfigChange, // No arguments. This switches between sample rates and exercises the config change mechanism.
    ElevenRackDriverExternalMethod_SetClockSource, // One scalar input: ElevenRackClockSource.
    ElevenRackDriverExternalMethod_GetAudioStatus, // Four outputs: clock source, hardware rate, clock validity, streaming.
};

enum ElevenRackClockValidity
{
    ElevenRackClockValidity_Unlocked = 0,
    ElevenRackClockValidity_Locked = 1,
    ElevenRackClockValidity_Unknown = 2,
};

enum ElevenRackClockSource
{
    ElevenRackClockSource_Internal = 1,
    ElevenRackClockSource_AES_EBU = 2,
    ElevenRackClockSource_SPDIF = 3,
};

#endif /* ElevenRackDriverKeys_h */

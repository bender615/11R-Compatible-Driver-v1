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
    ElevenRackDriverExternalMethod_TestConfigChange // No arguments. This switches between sample rates and exercises the config change mechanism.
};

#endif /* ElevenRackDriverKeys_h */

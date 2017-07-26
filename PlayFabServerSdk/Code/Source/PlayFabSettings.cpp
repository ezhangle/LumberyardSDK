#include "StdAfx.h"
#include "PlayFabSettings.h"

using namespace PlayFabServerSdk;

PlayFabSettings PlayFabSettings::playFabSettings; // Global settings for all Apis

PlayFabSettings::PlayFabSettings() :
    playFabSDKVersion("0.0.170710"),
    playFabVersionString("LumberyardSDK-0.0.170710"),
    useDevelopmentEnvironment(false),
    developmentEnvironmentURL(".playfabsandbox.com"),
    productionEnvironmentURL(".playfabapi.com"),
    titleId(), // You must set this value for PlayFab to work properly (Found in the Game Manager for your title, at the PlayFab Website)
    globalErrorHandler(nullptr),
    developerSecretKey(), // You must set this value for PlayFab to work properly (Found in the Game Manager for your title, at the PlayFab Website)
    serverURL()
{};

// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#pragma once

namespace Lightmass
{

extern FString InstigatorUserName;

/**
 * Handles critical error. The call only performs anything on the first call.
 */
void appHandleCriticalError();

/**
 * Returns the crash reporter URL after appHandleCriticalError() has been called.
 */
const class FString& appGetCrashReporterURL();

}

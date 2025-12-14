/**
* Copyright (C) 2025 | Created by NordVader Inc.
* All rights reserved!
* My Discord Server: https://discord.gg/B8prpf3vzD
*/

#include "PhysXInstancedSubsystem.h"

#define LOCTEXT_NAMESPACE "FPhysXInstancedSubsystemModule"

// -----------------------------------------------------------------------------
// IModuleInterface
// -----------------------------------------------------------------------------

void FPhysXInstancedSubsystem::StartupModule()
{
	// Called when the module is loaded into memory.
}

void FPhysXInstancedSubsystem::ShutdownModule()
{
	// Called during module shutdown before the module is unloaded.
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FPhysXInstancedSubsystem, PhysXInstancedSubsystem)

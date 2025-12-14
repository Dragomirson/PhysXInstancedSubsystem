/**
* Copyright (C) 2025 | Created by NordVader Inc.
* All rights reserved!
* My Discord Server: https://discord.gg/B8prpf3vzD
*/

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

// ============================================================================
// PhysXInstancedSubsystem module interface
// ============================================================================
//
// This module class is the entry point used by Unreal's module loader.
// StartupModule / ShutdownModule are called when the plugin module is loaded
// and unloaded, respectively.
// ============================================================================

class FPhysXInstancedSubsystem : public IModuleInterface
{
public:
	/** Called after the module is loaded into memory. */
	virtual void StartupModule() override;

	/** Called before the module is unloaded from memory. */
	virtual void ShutdownModule() override;
};

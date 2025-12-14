/**
* Copyright (C) 2025 | Created by NordVader Inc.
* All rights reserved!
* My Discord Server: https://discord.gg/B8prpf3vzD
*/

#pragma once

#include "CoreMinimal.h"

class UWorld;

#if PHYSICS_INTERFACE_PHYSX

// Forward declare PhysX types to avoid including heavy PhysX headers in this file.
namespace physx
{
	class PxScene;
}

// ============================================================================
// PhysX scene access
// ============================================================================

/**
 * Return the low-level PhysX scene (physx::PxScene) for the given Unreal world.
 *
 * Returns nullptr if:
 *  - World is null,
 *  - the world has no physics scene,
 *  - or the PhysX backend is not available.
 *
 * This function is only valid when PHYSICS_INTERFACE_PHYSX is enabled.
 */
PHYSXINSTANCEDSUBSYSTEM_API physx::PxScene* GetPhysXSceneFromWorld(UWorld* World);

#endif // PHYSICS_INTERFACE_PHYSX

/**
* Copyright (C) 2025 | Created by NordVader Inc.
* All rights reserved!
* My Discord Server: https://discord.gg/B8prpf3vzD
*/

#include "PhysXSupportCore.h"

#if PHYSICS_INTERFACE_PHYSX

#include "Engine/World.h"

// FPhysScene is declared by PhysicsCore; the include name differs between engine versions.
#if __has_include("PhysicsPublicCore.h")
	#include "PhysicsPublicCore.h"
#else
	#include "PhysicsPublic.h"
#endif

using namespace physx;

/**
 * Returns the low-level PhysX scene (physx::PxScene) for the given UWorld.
 *
 * Flow:
 *  1) Validate the input world pointer.
 *  2) Query the world's physics scene (FPhysScene).
 *  3) Return FPhysScene::GetPxScene() when available.
 */
PxScene* GetPhysXSceneFromWorld(UWorld* World)
{
	if (!World)
	{
		return nullptr;
	}

	FPhysScene* PhysScene = World->GetPhysicsScene();
	if (!PhysScene)
	{
		return nullptr;
	}

	return PhysScene->GetPxScene();
}

#endif // PHYSICS_INTERFACE_PHYSX

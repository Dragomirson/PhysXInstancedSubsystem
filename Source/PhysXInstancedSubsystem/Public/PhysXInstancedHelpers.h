/**
* Copyright (C) 2025 | Created by NordVader Inc.
* All rights reserved!
* My Discord Server: https://discord.gg/B8prpf3vzD
*/

#pragma once

#include "CoreMinimal.h"
#include "PhysXSupportCore.h"

#if PHYSICS_INTERFACE_PHYSX

// Forward declarations for PhysX types to avoid including heavy PhysX headers here.
namespace physx
{
	class PxScene;
	class PxRigidDynamic;
	class PxShape;
	class PxMaterial;
}

class UWorld;
class UInstancedStaticMeshComponent;
struct FBodyInstance;

// ============================================================================
// PhysX instanced helpers
// ============================================================================
//
// This header intentionally stays thin:
//
// - It includes PhysXSupportCore.h which declares the low-level entry point:
//     physx::PxScene* GetPhysXSceneFromWorld(UWorld* World);
//
// - All low-level implementation lives in the corresponding .cpp files:
//     - PhysXSupportCore.cpp   (scene access / world -> PxScene mapping)
//     - PhysXInstancedBody.cpp (PxRigidDynamic creation / destruction)
//
// Any .cpp that includes this header can call GetPhysXSceneFromWorld(World)
// without including heavy PhysX headers directly.
// ============================================================================

#endif // PHYSICS_INTERFACE_PHYSX

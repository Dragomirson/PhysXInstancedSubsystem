/**
* Copyright (C) 2025 | Created by NordVader Inc.
* All rights reserved!
* My Discord Server: https://discord.gg/B8prpf3vzD
*/

#pragma once

#include "CoreMinimal.h"
#include "Types/PhysXInstancedTypes.h"

#if PHYSICS_INTERFACE_PHYSX

class UWorld;

/**
 * Debug helper that draws simple primitives for PhysX instanced bodies.
 *
 * This uses Unreal's DrawDebug* functions and is intended for development/debugging only.
 * Rendering is controlled by console variables:
 *
 *   physx.Instanced.DebugDraw             - Master enable switch (0 = off, 1 = on).
 *   physx.Instanced.DebugDrawMaxDistance  - Max camera distance in centimeters.
 *   physx.Instanced.DebugDrawMaxInstances - Max number of instances drawn per frame.
 *   physx.Instanced.DebugDrawFrameStep    - Draw only every N-th frame.
 *
 * Shape mapping:
 *   - PxBoxGeometry      -> DrawDebugBox.
 *   - PxSphereGeometry   -> DrawDebugSphere.
 *   - PxCapsuleGeometry  -> DrawDebugCapsule.
 *   - Convex/Triangle    -> Proxy box approximation.
 */
class FPhysXInstancedDebugDraw
{
public:
	/** Draw debug primitives for the provided set of instance bodies. */
	static void Draw(
		UWorld* World,
		const TMap<FPhysXInstanceID, FPhysXInstanceData>& Instances);
};

#endif // PHYSICS_INTERFACE_PHYSX

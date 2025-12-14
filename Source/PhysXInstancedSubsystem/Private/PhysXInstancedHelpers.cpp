/**
* Copyright (C) 2025 | Created by NordVader Inc.
* All rights reserved!
* My Discord Server: https://discord.gg/B8prpf3vzD
*/

#include "PhysXInstancedHelpers.h"

#if PHYSICS_INTERFACE_PHYSX

/*
 * This translation unit intentionally provides no definitions.
 *
 * GetPhysXSceneFromWorld(UWorld*) is implemented once in PhysXSupportCore.cpp
 * and exported for external linkage, so any caller that includes
 * PhysXInstancedHelpers.h links against that implementation.
 *
 * Keeping this file avoids duplicate symbol definitions while preserving the
 * existing module file layout.
 */

#endif // PHYSICS_INTERFACE_PHYSX

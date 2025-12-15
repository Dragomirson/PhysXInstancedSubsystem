/**
* Copyright (C) 2025 | Created by NordVader Inc.
* All rights reserved!
* My Discord Server: https://discord.gg/B8prpf3vzD
*/

#pragma once

// ============================================================================
// Common includes for PhysX Instanced subsystem headers.
// ============================================================================

#include "CoreMinimal.h"
#include "Types/PhysXInstancedTypes.h"


#if PHYSICS_INTERFACE_PHYSX

// P2U / U2P conversion helpers (U2PVector, U2PTransform, etc.).
#if __has_include("PhysXPublicCore.h")
	#include "PhysXPublicCore.h"
#else
	#include "PhysXPublic.h"
#endif

// Converts a UU scalar into PhysX units using the existing U2PVector conversion.
static FORCEINLINE float U2PScalar(float ValueUU)
{	return U2PVector(FVector(ValueUU, 0.f, 0.f)).x;	}

#endif // PHYSICS_INTERFACE_PHYSX
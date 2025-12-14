/**
* Copyright (C) 2025 | Created by NordVader Inc.
* All rights reserved!
* My Discord Server: https://discord.gg/B8prpf3vzD
*/

#pragma once

#include "CoreMinimal.h"
#include "PhysXIncludes.h"    // PxPhysicsAPI, PxRigidDynamic, etc.
#include "PhysXPublicCore.h"  // U2PTransform / P2UTransform helpers (if needed)

// ============================================================================
// PxRigidDynamic helpers
// ============================================================================

/**
 * Thin convenience wrapper around PxRigidDynamic.
 *
 * This class does NOT own the actor and does NOT call release().
 * It only provides a small, readable helper API on top of PxRigidDynamic.
 */
class FPhysXRigidDynamicWrapper
{
public:
	FPhysXRigidDynamicWrapper() = default;

	explicit FPhysXRigidDynamicWrapper(physx::PxRigidDynamic* InActor)
		: Actor(InActor)
	{
	}

	/** Return the underlying PxRigidDynamic pointer. */
	physx::PxRigidDynamic* Get() const { return Actor; }

	/** True if the wrapped PxRigidDynamic pointer is non-null. */
	bool IsValid() const { return Actor != nullptr; }

	/** Enable or disable kinematic mode on the actor. */
	void SetKinematic(bool bKinematic);

	/** Set kinematic target using an Unreal FTransform (converted to PxTransform internally). */
	void SetKinematicTarget(const FTransform& TargetTM);

	/** Set linear and angular damping values on the actor. */
	void SetDamping(float LinDamp, float AngDamp);

	/** Set maximum angular velocity on the actor. */
	void SetMaxAngularVelocity(float MaxAngVel);

	/** Lock or unlock translation axes on the actor. */
	void LockPosition(bool bX, bool bY, bool bZ);

	/** Lock or unlock rotation axes on the actor. */
	void LockRotation(bool bX, bool bY, bool bZ);

	/** Set sleep threshold used by PhysX to decide when the body can go to sleep. */
	void SetSleepThreshold(float Threshold);

	/** Set stabilization threshold used by PhysX for solver stabilization heuristics. */
	void SetStabilizationThreshold(float Threshold);

	/** Wake the actor up (if it is sleeping). */
	void WakeUp();

	/** Force the actor to sleep immediately. */
	void PutToSleep();

	/** Set solver iteration counts used by PhysX for contacts/constraints. */
	void SetSolverIterations(uint32 PositionIters, uint32 VelocityIters = 1);

	/** Set the contact report threshold used by PhysX for contact force reports. */
	void SetContactReportThreshold(float Threshold);

private:
	/** Wrapped PxRigidDynamic pointer (non-owning). */
	physx::PxRigidDynamic* Actor = nullptr;
};

// ============================================================================
// Simple rigid dynamic creation helper
// ============================================================================

/**
 * Create a PxRigidDynamic with a single shape, add it to the given PxScene, and return the pointer.
 *
 * This function does not perform addRef/release ownership management.
 *
 * @param Physics   PxPhysics instance used to create the actor (typically GPhysXSDK).
 * @param Scene     PxScene the actor is added to.
 * @param PxPose    Initial world pose of the actor.
 * @param Geometry  Shape geometry (PxBoxGeometry / PxSphereGeometry / ...).
 * @param Material  Shape material.
 * @param Density   Density value used for mass/inertia computation.
 */
physx::PxRigidDynamic* CreateRigidDynamicActor(
	physx::PxPhysics& Physics,
	physx::PxScene& Scene,
	const physx::PxTransform& PxPose,
	const physx::PxGeometry& Geometry,
	physx::PxMaterial& Material,
	float Density
);

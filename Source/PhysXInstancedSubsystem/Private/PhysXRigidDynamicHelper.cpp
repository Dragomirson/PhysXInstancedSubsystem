/**
* Copyright (C) 2025 | Created by NordVader Inc.
* All rights reserved!
* My Discord Server: https://discord.gg/B8prpf3vzD
*/

#include "PhysXRigidDynamicHelper.h"
#include "PhysXPublicCore.h" // U2PTransform / P2UTransform helpers.

using namespace physx;

// -----------------------------------------------------------------------------
// FPhysXRigidDynamicWrapper
// -----------------------------------------------------------------------------

void FPhysXRigidDynamicWrapper::SetKinematic(bool bKinematic)
{
	if (!Actor)
	{
		return;
	}

	PxRigidBodyFlags Flags = Actor->getRigidBodyFlags();

	if (bKinematic)
	{
		Flags |= PxRigidBodyFlag::eKINEMATIC;
	}
	else
	{
		Flags &= ~PxRigidBodyFlag::eKINEMATIC;
	}

	Actor->setRigidBodyFlags(Flags);
}

void FPhysXRigidDynamicWrapper::SetKinematicTarget(const FTransform& TargetTM)
{
	if (!Actor)
	{
		return;
	}

	const PxTransform PxTarget = U2PTransform(TargetTM);
	Actor->setKinematicTarget(PxTarget);
}

void FPhysXRigidDynamicWrapper::SetDamping(float LinDamp, float AngDamp)
{
	if (!Actor)
	{
		return;
	}

	Actor->setLinearDamping(LinDamp);
	Actor->setAngularDamping(AngDamp);
}

void FPhysXRigidDynamicWrapper::SetMaxAngularVelocity(float MaxAngVel)
{
	if (!Actor)
	{
		return;
	}

	Actor->setMaxAngularVelocity(MaxAngVel);
}

void FPhysXRigidDynamicWrapper::LockPosition(bool bX, bool bY, bool bZ)
{
	if (!Actor)
	{
		return;
	}

	PxRigidDynamicLockFlags Flags;

	if (bX) Flags |= PxRigidDynamicLockFlag::eLOCK_LINEAR_X;
	if (bY) Flags |= PxRigidDynamicLockFlag::eLOCK_LINEAR_Y;
	if (bZ) Flags |= PxRigidDynamicLockFlag::eLOCK_LINEAR_Z;

	Actor->setRigidDynamicLockFlags(Flags);
}

void FPhysXRigidDynamicWrapper::LockRotation(bool bX, bool bY, bool bZ)
{
	if (!Actor)
	{
		return;
	}

	PxRigidDynamicLockFlags Flags = Actor->getRigidDynamicLockFlags();

	if (bX) Flags |= PxRigidDynamicLockFlag::eLOCK_ANGULAR_X;
	if (bY) Flags |= PxRigidDynamicLockFlag::eLOCK_ANGULAR_Y;
	if (bZ) Flags |= PxRigidDynamicLockFlag::eLOCK_ANGULAR_Z;

	Actor->setRigidDynamicLockFlags(Flags);
}

void FPhysXRigidDynamicWrapper::SetSleepThreshold(float Threshold)
{
	if (!Actor)
	{
		return;
	}

	Actor->setSleepThreshold(Threshold);
}

void FPhysXRigidDynamicWrapper::SetStabilizationThreshold(float Threshold)
{
	if (!Actor)
	{
		return;
	}

	Actor->setStabilizationThreshold(Threshold);
}

void FPhysXRigidDynamicWrapper::WakeUp()
{
	if (!Actor)
	{
		return;
	}

	Actor->wakeUp();
}

void FPhysXRigidDynamicWrapper::PutToSleep()
{
	if (!Actor)
	{
		return;
	}

	Actor->putToSleep();
}

void FPhysXRigidDynamicWrapper::SetSolverIterations(uint32 PositionIters, uint32 VelocityIters)
{
	if (!Actor)
	{
		return;
	}

	Actor->setSolverIterationCounts(PositionIters, VelocityIters);
}

void FPhysXRigidDynamicWrapper::SetContactReportThreshold(float Threshold)
{
	if (!Actor)
	{
		return;
	}

	Actor->setContactReportThreshold(Threshold);
}

// -----------------------------------------------------------------------------
// CreateRigidDynamicActor
// -----------------------------------------------------------------------------

physx::PxRigidDynamic* CreateRigidDynamicActor(
	physx::PxPhysics& Physics,
	physx::PxScene&   Scene,
	const physx::PxTransform& PxPose,
	const physx::PxGeometry&  Geometry,
	physx::PxMaterial&        Material,
	float                     Density)
{
	// Creates a dynamic rigid body with a single shape using PhysX helper API.
	PxRigidDynamic* Dynamic = PxCreateDynamic(Physics, PxPose, Geometry, Material, Density);
	if (!Dynamic)
	{
		return nullptr;
	}

	// Applies default damping values and an angular velocity limit to the new body.
	Dynamic->setLinearDamping(0.0f);
	Dynamic->setAngularDamping(0.05f);
	Dynamic->setMaxAngularVelocity(7.0f);

	// Adds the new actor to the target PhysX scene.
	Scene.addActor(*Dynamic);

	return Dynamic;
}

/**
* Copyright (C) 2025 | Created by NordVader Inc.
* All rights reserved!
* My Discord Server: https://discord.gg/B8prpf3vzD
*/

#include "PhysXInstancedBody.h"
#include "Debug/PhysXInstancedStats.h"
#include "Actors/PhysXInstancedMeshActor.h"

#if PHYSICS_INTERFACE_PHYSX

#include "PhysXInstancedHelpers.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/BoxElem.h"
#include "PhysicsEngine/SphereElem.h"
#include "PhysicsEngine/SphylElem.h"

// P2U / U2P
#if __has_include("PhysXPublicCore.h")
	#include "PhysXPublicCore.h"
#else
	#include "PhysXPublic.h"
#endif

// Full PhysX API.
#include "PxPhysicsAPI.h"

// Collision filtering helpers (CreateShapeFilterData, FCollisionFilterData, EPDF_*, etc.).
#include "Physics/PhysicsFiltering.h"

using namespace physx;

DEFINE_LOG_CATEGORY_STATIC(LogPhysXInstanced, Log, All);

// -----------------------------------------------------------------------------
// FPhysXInstanceBody lifetime
// -----------------------------------------------------------------------------

void FPhysXInstanceBody::Destroy()
{
	// Removes the actor from its scene (if any) and releases the PhysX object.
	if (!PxBody)
	{
		return;
	}

	PxScene* Scene = PxBody->getScene();
	if (Scene)
	{
		Scene->removeActor(*PxBody);
	}

	PxBody->release();
	PxBody = nullptr;
}

void FPhysXInstanceBody::AddActorToScene(UWorld* World)
{
	if (!PxBody || !World)
	{
		return;
	}

	PxScene* Scene = GetPhysXSceneFromWorld(World);
	if (!Scene)
	{
		return;
	}

	// Already in some scene – nothing to do.
	if (PxBody->getScene())
	{
		return;
	}

	Scene->addActor(*PxBody);
}

// -----------------------------------------------------------------------------
// Local helpers
// -----------------------------------------------------------------------------

namespace
{
	PxTransform MakePxTransformForInstance(UInstancedStaticMeshComponent* InstancedMesh, int32 InstanceIndex)
	{
		const FTransform ComponentTM = InstancedMesh->GetComponentTransform();

		const FTransform InstanceLocalTM = InstancedMesh->PerInstanceSMData.IsValidIndex(InstanceIndex)
			? FTransform(InstancedMesh->PerInstanceSMData[InstanceIndex].Transform)
			: FTransform::Identity;

		const FTransform WorldTM = InstanceLocalTM * ComponentTM;
		return U2PTransform(WorldTM);
	}

	UBodySetup* GetCollisionBodySetup(UInstancedStaticMeshComponent* InstancedMesh, UStaticMesh* OverrideMesh)
	{
		if (OverrideMesh && OverrideMesh->GetBodySetup())
		{
			return OverrideMesh->GetBodySetup();
		}

		if (InstancedMesh && InstancedMesh->GetStaticMesh())
		{
			return InstancedMesh->GetStaticMesh()->GetBodySetup();
		}

		return nullptr;
	}

	// Builds a box geometry from the static mesh bounds and instance/component scale.
	PxBoxGeometry MakeBoxGeometryForInstance(
		UInstancedStaticMeshComponent* InstancedMesh,
		int32 InstanceIndex,
		PxVec3& OutLocalCenter)
	{
		OutLocalCenter = PxVec3(0.f);

		if (!InstancedMesh)
		{
			return PxBoxGeometry(PxVec3(1.f));
		}

		const UStaticMesh* StaticMesh = InstancedMesh->GetStaticMesh();
		if (!StaticMesh)
		{
			return PxBoxGeometry(PxVec3(1.f));
		}

		const FBoxSphereBounds MeshBounds = StaticMesh->GetBounds();

		FVector TotalScale = InstancedMesh->GetComponentTransform().GetScale3D();
		if (InstancedMesh->PerInstanceSMData.IsValidIndex(InstanceIndex))
		{
			const FTransform InstanceLocalTM(InstancedMesh->PerInstanceSMData[InstanceIndex].Transform);
			TotalScale *= InstanceLocalTM.GetScale3D();
		}

		const FVector HalfSizeUU = MeshBounds.BoxExtent * TotalScale;
		const FVector CenterUU   = MeshBounds.Origin   * TotalScale;

		OutLocalCenter = U2PVector(CenterUU);
		return PxBoxGeometry(U2PVector(HalfSizeUU));
	}

	// Builds a sphere geometry from either BodySetup sphere data or the static mesh bounds.
	PxSphereGeometry MakeSphereGeometryForInstance(
		UBodySetup* BodySetup,
		UInstancedStaticMeshComponent* InstancedMesh,
		int32 InstanceIndex,
		PxVec3& OutLocalCenter)
	{
		OutLocalCenter = PxVec3(0.f);

		if (!InstancedMesh)
		{
			return PxSphereGeometry(U2PScalar(50.f));
		}

		const UStaticMesh* StaticMesh = InstancedMesh->GetStaticMesh();
		if (!StaticMesh)
		{
			return PxSphereGeometry(U2PScalar(50.f));
		}

		FVector TotalScale = InstancedMesh->GetComponentTransform().GetScale3D();
		if (InstancedMesh->PerInstanceSMData.IsValidIndex(InstanceIndex))
		{
			const FTransform InstanceLocalTM(InstancedMesh->PerInstanceSMData[InstanceIndex].Transform);
			TotalScale *= InstanceLocalTM.GetScale3D();
		}

		float   RadiusUU = 0.f;
		FVector CenterUU = FVector::ZeroVector;

		if (BodySetup && BodySetup->AggGeom.SphereElems.Num() > 0)
		{
			const FKSphereElem& Sphere = BodySetup->AggGeom.SphereElems[0];
			const float MaxScale = TotalScale.GetAbsMax();

			RadiusUU = Sphere.Radius * MaxScale;
			CenterUU = Sphere.Center * TotalScale;
		}
		else
		{
			const FBoxSphereBounds MeshBounds = StaticMesh->GetBounds();
			const float MaxScale = TotalScale.GetAbsMax();

			RadiusUU = MeshBounds.SphereRadius * MaxScale;
			CenterUU = MeshBounds.Origin * TotalScale;
		}

		if (RadiusUU <= KINDA_SMALL_NUMBER)
		{
			RadiusUU = 50.f;
		}

		OutLocalCenter = U2PVector(CenterUU);
		return PxSphereGeometry(U2PScalar(RadiusUU));
	}

	// Builds a capsule geometry fitted to the mesh bounds or BodySetup sphyl data.
	// Outputs the local center and the selected major axis index (0=X, 1=Y, 2=Z).
	PxCapsuleGeometry MakeCapsuleGeometryForInstance(
		UInstancedStaticMeshComponent* InstancedMesh,
		int32 InstanceIndex,
		PxVec3& OutLocalCenter,
		int32& OutMajorAxisIndex)
	{
		OutLocalCenter    = PxVec3(0.f);
		OutMajorAxisIndex = 2;

		if (!InstancedMesh)
		{
			return PxCapsuleGeometry(PxReal(1.f), PxReal(1.f));
		}

		const UStaticMesh* StaticMesh = InstancedMesh->GetStaticMesh();
		if (!StaticMesh)
		{
			return PxCapsuleGeometry(PxReal(1.f), PxReal(1.f));
		}

		UBodySetup* BodySetup = StaticMesh->GetBodySetup();

		FVector TotalScale = InstancedMesh->GetComponentTransform().GetScale3D();
		if (InstancedMesh->PerInstanceSMData.IsValidIndex(InstanceIndex))
		{
			const FTransform InstanceLocalTM(InstancedMesh->PerInstanceSMData[InstanceIndex].Transform);
			TotalScale *= InstanceLocalTM.GetScale3D();
		}

		// 1) Prefer a simple capsule (sphyl) from the mesh BodySetup.
		if (BodySetup && BodySetup->AggGeom.SphylElems.Num() > 0)
		{
			const FKSphylElem& Sphyl = BodySetup->AggGeom.SphylElems[0];
			const float MaxScale = TotalScale.GetAbsMax();

			float RadiusUU     = FMath::Max(Sphyl.Radius * MaxScale, KINDA_SMALL_NUMBER);
			float HalfHeightUU = 0.5f * Sphyl.Length * MaxScale;

			if (HalfHeightUU <= KINDA_SMALL_NUMBER)
			{
				HalfHeightUU = RadiusUU * 0.5f;
			}

			const FVector CenterUU = Sphyl.Center * TotalScale;
			OutLocalCenter = U2PVector(CenterUU);

			const FVector AxisLocal = Sphyl.Rotation.RotateVector(FVector::XAxisVector).GetSafeNormal();
			const FVector AbsAxis   = AxisLocal.GetAbs();

			if (AbsAxis.X >= AbsAxis.Y && AbsAxis.X >= AbsAxis.Z)
			{
				OutMajorAxisIndex = 0;
			}
			else if (AbsAxis.Y >= AbsAxis.Z)
			{
				OutMajorAxisIndex = 1;
			}
			else
			{
				OutMajorAxisIndex = 2;
			}

			return PxCapsuleGeometry(
				U2PScalar(RadiusUU),
				U2PScalar(HalfHeightUU));
		}

		// 2) Fallback: fit a capsule to the mesh bounds and align along the major axis.
		const FBoxSphereBounds MeshBounds = StaticMesh->GetBounds();

		const FVector ExtentsUU = MeshBounds.BoxExtent * TotalScale;
		const FVector AbsExt    = ExtentsUU.GetAbs();

		float MajorExtent = AbsExt.X;
		OutMajorAxisIndex = 0;

		if (AbsExt.Y > MajorExtent)
		{
			MajorExtent       = AbsExt.Y;
			OutMajorAxisIndex = 1;
		}
		if (AbsExt.Z > MajorExtent)
		{
			MajorExtent       = AbsExt.Z;
			OutMajorAxisIndex = 2;
		}

		float RadExtent = 0.f;
		if (OutMajorAxisIndex == 0)
		{
			RadExtent = FMath::Max(AbsExt.Y, AbsExt.Z);
		}
		else if (OutMajorAxisIndex == 1)
		{
			RadExtent = FMath::Max(AbsExt.X, AbsExt.Z);
		}
		else
		{
			RadExtent = FMath::Max(AbsExt.X, AbsExt.Y);
		}

		float RadiusUU = FMath::Max(RadExtent, KINDA_SMALL_NUMBER);

		// Ensures that (HalfHeight + Radius) approximately matches the major extent.
		float HalfHeightUU = MajorExtent - RadiusUU;
		if (HalfHeightUU <= KINDA_SMALL_NUMBER)
		{
			HalfHeightUU = RadiusUU * 0.5f;
		}

		const FVector CenterUU = MeshBounds.Origin * TotalScale;
		OutLocalCenter = U2PVector(CenterUU);

		return PxCapsuleGeometry(
			U2PScalar(RadiusUU),
			U2PScalar(HalfHeightUU));
	}

	PxConvexMeshGeometry MakeConvexGeometry(UBodySetup* BodySetup, UInstancedStaticMeshComponent* InstancedMesh)
	{
		if (!BodySetup || BodySetup->AggGeom.ConvexElems.Num() == 0)
		{
			return PxConvexMeshGeometry();
		}

		const FKConvexElem& Convex = BodySetup->AggGeom.ConvexElems[0];

		if (!Convex.GetConvexMesh())
		{
			return PxConvexMeshGeometry();
		}

		const FVector ComponentScale = InstancedMesh->GetComponentScale();
		const PxVec3 PxScale = U2PVector(ComponentScale);
		const PxMeshScale MeshScale(PxScale, PxQuat(PxIdentity));

		return PxConvexMeshGeometry(Convex.GetConvexMesh(), MeshScale);
	}

	PxTriangleMeshGeometry MakeTriangleMeshGeometry(UBodySetup* BodySetup, UInstancedStaticMeshComponent* InstancedMesh)
	{
		if (!BodySetup || BodySetup->TriMeshes.Num() == 0)
		{
			return PxTriangleMeshGeometry();
		}

		PxTriangleMesh* TriMesh = BodySetup->TriMeshes[0];
		if (!TriMesh)
		{
			return PxTriangleMeshGeometry();
		}

		const FVector ComponentScale = InstancedMesh->GetComponentScale();
		const PxVec3 PxScale = U2PVector(ComponentScale);
		const PxMeshScale MeshScale(PxScale, PxQuat(PxIdentity));

		return PxTriangleMeshGeometry(TriMesh, MeshScale);
	}
} // anonymous namespace

// -----------------------------------------------------------------------------
// Body creation
// -----------------------------------------------------------------------------

bool FPhysXInstanceBody::CreateFromInstancedStaticMesh(
	UInstancedStaticMeshComponent* InstancedMesh,
	int32 InstanceIndex,
	bool bSimulate,
	PxMaterial* DefaultMaterial,
	EPhysXInstanceShapeType ShapeType,
	UStaticMesh* OverrideCollisionMesh)
{
	// Measure CPU time spent creating a PhysX body for an instance.
	SCOPE_CYCLE_COUNTER(STAT_PhysXInstanced_CreateBody);

	if (PxBody)
	{
		Destroy();
	}

	if (!InstancedMesh || !DefaultMaterial)
	{
		return false;
	}

	// We only need the SDK here, not the scene. Actor will be added later.
	if (!GPhysXSDK)
	{
		return false;
	}

	FBodyInstance* TemplateBodyInstance = InstancedMesh->GetBodyInstance();
	if (!TemplateBodyInstance)
	{
		return false;
	}

	UBodySetup* CollisionBodySetup = GetCollisionBodySetup(InstancedMesh, OverrideCollisionMesh);

	const bool bPhysicsStatic =
		!InstancedMesh->IsSimulatingPhysics() && !bSimulate && InstancedMesh->Mobility != EComponentMobility::Movable;

	const bool bUseTriangleMesh = (ShapeType == EPhysXInstanceShapeType::TriangleMeshStatic);
	bool bSkipMassUpdate = false;

	if (bUseTriangleMesh && bSimulate)
	{
		UE_LOG(LogPhysXInstanced, Warning,
			TEXT("TriangleMesh selected for a simulating instance – forcing kinematic and skipping mass update."));
		bSimulate = false;
		bSkipMassUpdate = true;
	}

	const PxTransform PxTM = MakePxTransformForInstance(InstancedMesh, InstanceIndex);

	// Use global PhysX SDK instead of accessing PxScene here.
	PxPhysics& Physics = *GPhysXSDK;
	PxRigidDynamic* RigidDynamic = Physics.createRigidDynamic(PxTM);
	if (!RigidDynamic)
	{
		return false;
	}

	// ---------------------------------------------------------------------
	// Shape creation
	// ---------------------------------------------------------------------

	PxShape* Shape = nullptr;

	switch (ShapeType)
	{
	case EPhysXInstanceShapeType::Box:
	{
		PxVec3 LocalCenter(0.f);
		PxBoxGeometry Geom = MakeBoxGeometryForInstance(InstancedMesh, InstanceIndex, LocalCenter);
		if (Geom.isValid())
		{
			Shape = RigidDynamic->createShape(Geom, *DefaultMaterial);
			if (Shape)
			{
				// Offsets the shape so the geometry matches the static mesh bounds center.
				Shape->setLocalPose(PxTransform(LocalCenter));
			}
		}
		break;
	}

	case EPhysXInstanceShapeType::Sphere:
	{
		PxVec3 LocalCenter(0.f);
		PxSphereGeometry Geom = MakeSphereGeometryForInstance(CollisionBodySetup, InstancedMesh, InstanceIndex, LocalCenter);
		if (Geom.isValid())
		{
			Shape = RigidDynamic->createShape(Geom, *DefaultMaterial);
			if (Shape)
			{
				// Offsets the sphere so it matches the same bounds center logic as the box.
				Shape->setLocalPose(PxTransform(LocalCenter));
			}
		}
		break;
	}

	case EPhysXInstanceShapeType::Capsule:
	{
		PxVec3  LocalCenter(0.f);
		float   RadiusUU      = 50.f;
		float   HalfHeightUU  = 25.f;
		FQuat   LocalRotation = FQuat::Identity;

		const UStaticMesh* StaticMesh = InstancedMesh ? InstancedMesh->GetStaticMesh() : nullptr;
		UBodySetup* BodySetup = StaticMesh ? StaticMesh->GetBodySetup() : nullptr;

		// 1) If there is a simple capsule collision in BodySetup – use its size and rotation
		if (BodySetup && BodySetup->AggGeom.SphylElems.Num() > 0)
		{
			const FKSphylElem& Sphyl = BodySetup->AggGeom.SphylElems[0];

			FVector TotalScale = InstancedMesh->GetComponentTransform().GetScale3D();
			if (InstancedMesh->PerInstanceSMData.IsValidIndex(InstanceIndex))
			{
				const FTransform InstanceLocalTM(InstancedMesh->PerInstanceSMData[InstanceIndex].Transform);
				TotalScale *= InstanceLocalTM.GetScale3D();
			}

			const float MaxScale = TotalScale.GetAbsMax();

			RadiusUU     = FMath::Max(Sphyl.Radius * MaxScale, KINDA_SMALL_NUMBER);
			HalfHeightUU = FMath::Max(0.5f * Sphyl.Length * MaxScale, RadiusUU * 0.5f);

			const FVector CenterUU = Sphyl.Center * TotalScale;
			LocalCenter = U2PVector(CenterUU);

			// Sphyl.Rotation already describes capsule axis in mesh local space
			LocalRotation = Sphyl.Rotation.Quaternion();
		}
		else
		{
			// 2) Fallback: fit capsule into mesh bounds and align along the major axis
			if (!StaticMesh)
			{
				break;
			}

			const FBoxSphereBounds MeshBounds = StaticMesh->GetBounds();

			FVector TotalScale = InstancedMesh->GetComponentTransform().GetScale3D();
			if (InstancedMesh->PerInstanceSMData.IsValidIndex(InstanceIndex))
			{
				const FTransform InstanceLocalTM(InstancedMesh->PerInstanceSMData[InstanceIndex].Transform);
				TotalScale *= InstanceLocalTM.GetScale3D();
			}

			const FVector ExtentsUU = MeshBounds.BoxExtent * TotalScale;
			const FVector AbsExt    = ExtentsUU.GetAbs();

			// choose major axis X/Y/Z
			EAxis::Type MajorAxis = EAxis::X;
			float MajorExtent = AbsExt.X;
			if (AbsExt.Y > MajorExtent) { MajorAxis = EAxis::Y; MajorExtent = AbsExt.Y; }
			if (AbsExt.Z > MajorExtent) { MajorAxis = EAxis::Z; MajorExtent = AbsExt.Z; }

			float RadExtent = 0.f;
			if (MajorAxis == EAxis::X)       RadExtent = FMath::Max(AbsExt.Y, AbsExt.Z);
			else if (MajorAxis == EAxis::Y)  RadExtent = FMath::Max(AbsExt.X, AbsExt.Z);
			else                             RadExtent = FMath::Max(AbsExt.X, AbsExt.Y);

			RadiusUU     = FMath::Max(RadExtent, KINDA_SMALL_NUMBER);
			HalfHeightUU = MajorExtent - RadiusUU;
			if (HalfHeightUU <= KINDA_SMALL_NUMBER)
			{
				HalfHeightUU = RadiusUU * 0.5f;
			}

			const FVector CenterUU = MeshBounds.Origin * TotalScale;
			LocalCenter = U2PVector(CenterUU);

			const FVector DesiredAxis =
				(MajorAxis == EAxis::X) ? FVector::XAxisVector :
				(MajorAxis == EAxis::Y) ? FVector::YAxisVector :
										  FVector::ZAxisVector;

			LocalRotation = FQuat::FindBetweenNormals(FVector::XAxisVector, DesiredAxis);
		}

		PxCapsuleGeometry Geom(U2PScalar(RadiusUU), U2PScalar(HalfHeightUU));
		if (Geom.isValid())
		{
			Shape = RigidDynamic->createShape(Geom, *DefaultMaterial);
			if (Shape)
			{
				// Applies an optional per-actor local offset for the collision shape.
				FTransform ShapeOffset = FTransform::Identity;
				if (InstancedMesh)
				{
					if (const APhysXInstancedMeshActor* PhysXActor = Cast<APhysXInstancedMeshActor>(InstancedMesh->GetOwner()))
					{
						ShapeOffset = PhysXActor->ShapeCollisionOffset;
					}
				}

				const FQuat   FinalRot   = LocalRotation * ShapeOffset.GetRotation();
				const FVector FinalPosUU = P2UVector(LocalCenter) + ShapeOffset.GetLocation();

				Shape->setLocalPose(PxTransform(
					U2PVector(FinalPosUU),
					U2PQuat(FinalRot)
				));
			}
		}

		break;
	}

	case EPhysXInstanceShapeType::Convex:
	{
		PxConvexMeshGeometry Geom = MakeConvexGeometry(CollisionBodySetup, InstancedMesh);
		if (Geom.isValid())
		{
			Shape = RigidDynamic->createShape(Geom, *DefaultMaterial);
		}
		else
		{
			UE_LOG(LogPhysXInstanced, Warning, TEXT("Convex недоступен, откат к Box."));
			PxVec3 LocalCenter(0.f);
			PxBoxGeometry BoxGeom = MakeBoxGeometryForInstance(InstancedMesh, InstanceIndex, LocalCenter);
			if (BoxGeom.isValid())
			{
				Shape = RigidDynamic->createShape(BoxGeom, *DefaultMaterial);
				if (Shape)
				{
					Shape->setLocalPose(PxTransform(LocalCenter));
				}
			}
		}
		break;
	}

	case EPhysXInstanceShapeType::TriangleMeshStatic:
	{
		PxTriangleMeshGeometry Geom = MakeTriangleMeshGeometry(CollisionBodySetup, InstancedMesh);
		if (Geom.isValid())
		{
			Shape = RigidDynamic->createShape(Geom, *DefaultMaterial);
			bSkipMassUpdate = true;
		}
		else
		{
			UE_LOG(LogPhysXInstanced, Warning, TEXT("TriangleMesh недоступен, откат к Box."));
			PxVec3 LocalCenter(0.f);
			PxBoxGeometry BoxGeom = MakeBoxGeometryForInstance(InstancedMesh, InstanceIndex, LocalCenter);
			if (BoxGeom.isValid())
			{
				Shape = RigidDynamic->createShape(BoxGeom, *DefaultMaterial);
				if (Shape)
				{
					Shape->setLocalPose(PxTransform(LocalCenter));
				}
			}
		}
		break;
	}

	default:
		break;
	}

	if (!Shape)
	{
		// Final fallback: create a small box shape to keep the actor valid.
		PxBoxGeometry BoxGeom(PxVec3(10.f));
		if (!BoxGeom.isValid())
		{
			RigidDynamic->release();
			return false;
		}

		Shape = RigidDynamic->createShape(BoxGeom, *DefaultMaterial);
	}

	Shape->setFlag(PxShapeFlag::eSIMULATION_SHAPE,  true);
	Shape->setFlag(PxShapeFlag::eSCENE_QUERY_SHAPE, true);
	Shape->setFlag(PxShapeFlag::eTRIGGER_SHAPE,     false);

	// ---------------------------------------------------------------------
	// CCD configuration
	// ---------------------------------------------------------------------

	// CCD filter flag used by CreateShapeFilterData to route CCD interactions.
	bool bCCDFilter = false;

	// CCD rigid body flag (PxRigidBodyFlag::eENABLE_CCD) applied on the actor.
	bool bCCDFlag = false;

	bool bHasActorCCDConfig = false;
	EPhysXInstanceCCDMode CCDMode = EPhysXInstanceCCDMode::Off;

	if (InstancedMesh)
	{
		if (AActor* Owner = InstancedMesh->GetOwner())
		{
			if (const APhysXInstancedMeshActor* PhysXActor = Cast<APhysXInstancedMeshActor>(Owner))
			{
				CCDMode = PhysXActor->CCDConfig.Mode;
				bHasActorCCDConfig = true;
			}
		}
	}

	if (bHasActorCCDConfig)
	{
		switch (CCDMode)
		{
		case EPhysXInstanceCCDMode::Off:
			// Leaves CCD disabled.
			break;

		case EPhysXInstanceCCDMode::Simulating:
			if (!bPhysicsStatic && !bUseTriangleMesh && bSimulate)
			{
				bCCDFilter = true;
				bCCDFlag   = true;
			}
			break;

		case EPhysXInstanceCCDMode::All:
			if (!bPhysicsStatic && !bUseTriangleMesh)
			{
				bCCDFilter = true;
				bCCDFlag   = true;
			}
			break;

		case EPhysXInstanceCCDMode::AutoByVelocity:
			// Enables CCD filtering but leaves the rigid body flag disabled initially.
			// The subsystem toggles PxRigidBodyFlag::eENABLE_CCD based on velocity thresholds.
			if (!bPhysicsStatic && !bUseTriangleMesh && bSimulate)
			{
				bCCDFilter = true;
				bCCDFlag   = false;
			}
			break;

		default:
			break;
		}
	}
	else
	{
		// Falls back to the legacy BodyInstance CCD flag when no actor-level config is present.
		if (TemplateBodyInstance->bUseCCD && !bPhysicsStatic && !bUseTriangleMesh && bSimulate)
		{
			bCCDFilter = true;
			bCCDFlag   = true;
		}
	}

	// ---------------------------------------------------------------------
	// Collision filtering
	// ---------------------------------------------------------------------

	{
		AActor* Owner = InstancedMesh->GetOwner();
		const int32  ActorID     = Owner ? Owner->GetUniqueID() : 0;
		const uint32 ComponentID = InstancedMesh->GetUniqueID();
		const uint16 BodyIndex   = (uint16)InstanceIndex;

		const uint8 MyChannel = (uint8)TemplateBodyInstance->GetObjectType();
		const FMaskFilter MaskFilter = TemplateBodyInstance->GetMaskFilter();
		const FCollisionResponseContainer& Responses =
			TemplateBodyInstance->GetResponseToChannels();

		FCollisionFilterData QueryData;
		FCollisionFilterData SimData;

		const bool bEnableCCD = bCCDFilter;

		const bool bEnableContactNotify = TemplateBodyInstance->bNotifyRigidBodyCollision;
		const bool bModifyContacts      = TemplateBodyInstance->bContactModification;

		CreateShapeFilterData(
			MyChannel,
			MaskFilter,
			ActorID,
			Responses,
			ComponentID,
			BodyIndex,
			QueryData,
			SimData,
			bEnableCCD,
			bEnableContactNotify,
			bPhysicsStatic,
			bModifyContacts
		);

		const PxFilterData PxQuery(
			QueryData.Word0, QueryData.Word1, QueryData.Word2, QueryData.Word3);

		const PxFilterData PxSim(
			SimData.Word0, SimData.Word1, SimData.Word2, SimData.Word3);

		Shape->setQueryFilterData(PxQuery);
		Shape->setSimulationFilterData(PxSim);
	}

	// ---------------------------------------------------------------------
	// Rigid body settings
	// ---------------------------------------------------------------------

	{
		bool bEnableGravity = true;
		bool bGotGravityFromActor = false;

		if (InstancedMesh)
		{
			if (AActor* Owner = InstancedMesh->GetOwner())
			{
				if (const APhysXInstancedMeshActor* PhysXActor = Cast<APhysXInstancedMeshActor>(Owner))
				{
					bEnableGravity = PhysXActor->bInstancesUseGravity;
					bGotGravityFromActor = true;
				}
			}
		}

		if (!bGotGravityFromActor && TemplateBodyInstance)
		{
			bEnableGravity = TemplateBodyInstance->bEnableGravity;
		}

		RigidDynamic->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, !bEnableGravity);

		const bool bKinematic = !bSimulate;
		RigidDynamic->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, bKinematic);

		// Applies the initial CCD rigid body flag derived from CCDMode / legacy settings.
		if (bCCDFlag)
		{
			RigidDynamic->setRigidBodyFlag(PxRigidBodyFlag::eENABLE_CCD, true);
		}

		RigidDynamic->setLinearDamping(TemplateBodyInstance->LinearDamping);
		RigidDynamic->setAngularDamping(TemplateBodyInstance->AngularDamping);

		RigidDynamic->setSolverIterationCounts(
			TemplateBodyInstance->PositionSolverIterationCount,
			TemplateBodyInstance->VelocitySolverIterationCount);
	}

	if (!bSkipMassUpdate)
	{
		const float Mass =
			(TemplateBodyInstance->GetBodyMass() > 0.f)
				? TemplateBodyInstance->GetBodyMass()
				: 10.0f;

		PxRigidBodyExt::updateMassAndInertia(*RigidDynamic, Mass);
	}

	//Scene->addActor(*RigidDynamic);
	PxBody = RigidDynamic;
	return true;
}

physx::PxRigidActor* FPhysXInstanceBody::GetPxActor() const
{
	return PxBody;
}

#endif // PHYSICS_INTERFACE_PHYSX

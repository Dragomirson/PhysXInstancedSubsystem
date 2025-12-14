/**
* Copyright (C) 2025 | Created by NordVader Inc.
* All rights reserved!
* My Discord Server: https://discord.gg/B8prpf3vzD
*/

#include "Debug/PhysXInstancedDebugDraw.h"

#if PHYSICS_INTERFACE_PHYSX

#include "Camera/PlayerCameraManager.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"

#include "PhysXIncludes.h"

#if __has_include("PhysXPublicCore.h")
	#include "PhysXPublicCore.h" // P2UVector / P2UQuat helpers
#else
	#include "PhysXPublic.h"     // P2UVector / P2UQuat helpers
#endif

using namespace physx;

// ============================================================================
// Console variables
// ============================================================================

/**
 * Master switch for PhysX instanced debug drawing.
 *
 * Console:
 *   physx.Instanced.DebugDraw 0|1
 */
static TAutoConsoleVariable<int32> CVarPhysXInstancedDebugDraw(
	TEXT("physx.Instanced.DebugDraw"),
	0,
	TEXT("PhysX instanced debug drawing (0 = off, 1 = on)."),
	ECVF_Cheat);

/**
 * Maximum distance from the camera (cm) to draw instances.
 *
 * Console:
 *   physx.Instanced.DebugDrawMaxDistance <float>
 */
static TAutoConsoleVariable<float> CVarPhysXInstancedDebugDrawMaxDistance(
	TEXT("physx.Instanced.DebugDrawMaxDistance"),
	15000.0f,
	TEXT("Max distance from camera for PhysX instanced debug (in cm, <=0 = no limit)."),
	ECVF_Cheat);

/**
 * Maximum number of instances to draw per frame.
 *
 * Console:
 *   physx.Instanced.DebugDrawMaxInstances <int>
 */
static TAutoConsoleVariable<int32> CVarPhysXInstancedDebugDrawMaxInstances(
	TEXT("physx.Instanced.DebugDrawMaxInstances"),
	256,
	TEXT("Max number of PhysX instanced bodies to draw per frame."),
	ECVF_Cheat);

/**
 * Draw only every N-th frame (1 = every frame).
 *
 * Console:
 *   physx.Instanced.DebugDrawFrameStep <int>
 */
static TAutoConsoleVariable<int32> CVarPhysXInstancedDebugDrawFrameStep(
	TEXT("physx.Instanced.DebugDrawFrameStep"),
	1,
	TEXT("Draw PhysX instanced debug only every N-th frame (1 = every frame)."),
	ECVF_Cheat);

// ============================================================================
// Local helpers
// ============================================================================

/** Fetch a camera location used for distance-based culling of debug shapes. */
static bool GetCameraLocation(UWorld* World, FVector& OutLocation)
{
	if (!World)
	{
		return false;
	}

	if (APlayerController* PC = World->GetFirstPlayerController())
	{
		if (PC->PlayerCameraManager)
		{
			OutLocation = PC->PlayerCameraManager->GetCameraLocation();
			return true;
		}

		if (APawn* Pawn = PC->GetPawn())
		{
			OutLocation = Pawn->GetActorLocation();
			return true;
		}
	}

	return false;
}

// ============================================================================
// FPhysXInstancedDebugDraw
// ============================================================================

void FPhysXInstancedDebugDraw::Draw(
	UWorld* World,
	const TMap<FPhysXInstanceID, FPhysXInstanceData>& Instances)
{
	if (!World)
	{
		return;
	}

	// Early out when debug drawing is disabled.
	if (CVarPhysXInstancedDebugDraw.GetValueOnGameThread() == 0)
	{
		return;
	}

	// Apply frame stepping to reduce CPU cost in heavy scenes.
	static int32 FrameCounter = 0;
	++FrameCounter;

	const int32 FrameStep = FMath::Max(1, CVarPhysXInstancedDebugDrawFrameStep.GetValueOnGameThread());
	if ((FrameCounter % FrameStep) != 0)
	{
		return;
	}

	// Prepare distance culling settings.
	const float MaxDistance = CVarPhysXInstancedDebugDrawMaxDistance.GetValueOnGameThread();
	const bool  bUseDistanceCulling = (MaxDistance > 0.0f);
	const float MaxDistanceSq = MaxDistance * MaxDistance;

	FVector CameraLocation = FVector::ZeroVector;
	const bool bHasCamera = GetCameraLocation(World, CameraLocation);

	// Draw for one frame only.
	const float LifeTime = 0.0f;
	const bool  bPersistent = false;
	const float LineThickness = 1.5f;

	const int32 MaxInstances = FMath::Max(0, CVarPhysXInstancedDebugDrawMaxInstances.GetValueOnGameThread());
	int32 NumDrawn = 0;

	for (const TPair<FPhysXInstanceID, FPhysXInstanceData>& Pair : Instances)
	{
		const FPhysXInstanceData& Data = Pair.Value;

		PxRigidActor* RigidActor = Data.Body.GetPxActor();
		if (!RigidActor)
		{
			continue;
		}

		// Enforce the per-frame instance limit.
		if (MaxInstances > 0 && NumDrawn >= MaxInstances)
		{
			break;
		}

		const PxU32 ShapeCount = RigidActor->getNbShapes();
		if (ShapeCount == 0)
		{
			continue;
		}

		TArray<PxShape*> Shapes;
		Shapes.AddUninitialized(ShapeCount);
		RigidActor->getShapes(Shapes.GetData(), ShapeCount);

		PxRigidDynamic* RigidDynamic = RigidActor->is<PxRigidDynamic>();
		const bool bSleeping = (RigidDynamic != nullptr) && RigidDynamic->isSleeping();

		for (PxShape* Shape : Shapes)
		{
			if (!Shape)
			{
				continue;
			}

			const PxGeometryType::Enum GeomType = Shape->getGeometryType();

			// Combine actor global pose with the local shape pose.
			const PxTransform GlobalPose = RigidActor->getGlobalPose() * Shape->getLocalPose();
			const FVector Center = P2UVector(GlobalPose.p);
			const FQuat   Rot = P2UQuat(GlobalPose.q);

			// Skip shapes that are too far from the camera when culling is enabled.
			if (bUseDistanceCulling && bHasCamera)
			{
				const float DistSq = FVector::DistSquared(Center, CameraLocation);
				if (DistSq > MaxDistanceSq)
				{
					continue;
				}
			}

			FColor Color = FColor::White;

			switch (GeomType)
			{
			case PxGeometryType::eBOX:
				{
					PxBoxGeometry BoxGeom;
					if (!Shape->getBoxGeometry(BoxGeom))
					{
						break;
					}

					const FVector ExtentsUU = P2UVector(PxVec3(
						BoxGeom.halfExtents.x,
						BoxGeom.halfExtents.y,
						BoxGeom.halfExtents.z));

					// Box: green, or cyan when the body is sleeping.
					Color = bSleeping ? FColor::Cyan : FColor::Green;

					DrawDebugBox(
						World,
						Center,
						ExtentsUU,
						Rot,
						Color,
						bPersistent,
						LifeTime,
						/*DepthPriority=*/0,
						LineThickness);
					++NumDrawn;
					break;
				}

			case PxGeometryType::eSPHERE:
				{
					PxSphereGeometry SphereGeom;
					if (!Shape->getSphereGeometry(SphereGeom))
					{
						break;
					}

					const float RadiusUU = P2UVector(PxVec3(SphereGeom.radius, 0.0f, 0.0f)).X;

					// Sphere: yellow, or orange-ish when the body is active.
					Color = bSleeping ? FColor::Yellow : FColor(255, 200, 50);

					DrawDebugSphere(
						World,
						Center,
						RadiusUU,
						16,
						Color,
						bPersistent,
						LifeTime,
						/*DepthPriority=*/0,
						LineThickness);
					++NumDrawn;
					break;
				}

			case PxGeometryType::eCAPSULE:
				{
					PxCapsuleGeometry CapsuleGeom;
					if (!Shape->getCapsuleGeometry(CapsuleGeom))
					{
						break;
					}

					// PhysX capsule is aligned along X axis.
					const float RadiusUU = P2UVector(PxVec3(CapsuleGeom.radius, 0.0f, 0.0f)).X;
					const float HalfHeightUU = P2UVector(PxVec3(CapsuleGeom.halfHeight, 0.0f, 0.0f)).X;

					// Unreal debug capsule expects the axis aligned along Z, so rotate accordingly.
					const FQuat AxisAdjust(FVector(0.f, 1.f, 0.f), HALF_PI);
					const FQuat CapsuleRot = Rot * AxisAdjust;

					// Capsule: blue, or darker blue when the body is sleeping.
					Color = bSleeping ? FColor(100, 100, 255) : FColor::Blue;

					DrawDebugCapsule(
						World,
						Center,
						HalfHeightUU,
						RadiusUU,
						CapsuleRot,
						Color,
						bPersistent,
						LifeTime,
						/*DepthPriority=*/0,
						LineThickness);
					++NumDrawn;
					break;
				}

			case PxGeometryType::eCONVEXMESH:
			case PxGeometryType::eTRIANGLEMESH:
				{
					// Complex meshes are drawn as a proxy box derived from the static mesh bounds.
					FVector BoxExtents = FVector::ZeroVector;

					if (const UInstancedStaticMeshComponent* ISMC = Data.InstancedComponent.Get())
					{
						if (UStaticMesh* Mesh = ISMC->GetStaticMesh())
						{
							const FBoxSphereBounds Bounds = Mesh->GetBounds();
							BoxExtents = Bounds.BoxExtent * ISMC->GetComponentScale();
						}
					}

					if (!BoxExtents.IsNearlyZero())
					{
						// Convex: magenta, Triangle: red (darker when sleeping).
						if (GeomType == PxGeometryType::eCONVEXMESH)
						{
							Color = bSleeping ? FColor(200, 0, 200) : FColor(255, 0, 255);
						}
						else
						{
							Color = bSleeping ? FColor(200, 0, 0) : FColor::Red;
						}

						DrawDebugBox(
							World,
							Center,
							BoxExtents,
							Rot,
							Color,
							bPersistent,
							LifeTime,
							/*DepthPriority=*/0,
							LineThickness);
						++NumDrawn;
					}
					break;
				}

			default:
				// Unsupported geometry types are skipped.
				break;
			}

			// Stop immediately when the draw limit is reached.
			if (MaxInstances > 0 && NumDrawn >= MaxInstances)
			{
				return;
			}
		}
	}
}

#endif // PHYSICS_INTERFACE_PHYSX

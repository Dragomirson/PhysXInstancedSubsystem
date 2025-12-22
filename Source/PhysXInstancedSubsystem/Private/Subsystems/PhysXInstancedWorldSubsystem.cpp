/**
* Copyright (C) 2025 | Created by NordVader Inc.
* All rights reserved!
* My Discord Server: https://discord.gg/B8prpf3vzD
*/

#include "Subsystems/PhysXInstancedWorldSubsystem.h"
#include "Actors/PhysXInstancedMeshActor.h"
#include "Components/PhysXInstancedStaticMeshComponent.h"
#include "Debug/PhysXInstancedStats.h"
#include "PhysXInstancedBody.h"
#include "Types/PhysXInstancedTypes.h"

#include "Async/ParallelFor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "DrawDebugHelpers.h"

#include "PhysXInstancedSubsystem/Public/PhysXSupportCore.h"

#if PHYSICS_INTERFACE_PHYSX

#include "Debug/PhysXInstancedDebugDraw.h"
#include "PhysXIncludes.h"

#if __has_include("PhysXPublicCore.h")
	#include "PhysXPublicCore.h"
#else
	#include "PhysXPublic.h"
#endif

using namespace physx;

#if PHYSICS_INTERFACE_PHYSX

struct UPhysXInstancedWorldSubsystem::FPhysXInstanceUserData
{
	static constexpr uint32 MagicValue = 0x50584944; // 'PXID'

	uint32          Magic      = MagicValue;
	FPhysXInstanceID InstanceID;
};

void UPhysXInstancedWorldSubsystem::EnsureInstanceUserData(FPhysXInstanceID ID)
{
	FPhysXInstanceData* Data = Instances.Find(ID);
	if (!Data)
	{
		return;
	}

	physx::PxRigidActor* Actor = Data->Body.GetPxActor();
	if (!Actor)
	{
		return;
	}

	FPhysXInstanceUserData*& Slot = UserDataByID.FindOrAdd(ID);
	if (!Slot)
	{
		Slot = new FPhysXInstanceUserData();
	}

	Slot->InstanceID = ID;
	Actor->userData  = Slot;
}

void UPhysXInstancedWorldSubsystem::ClearInstanceUserData(FPhysXInstanceID ID)
{
	// Detach from the PhysX actor (must happen BEFORE release()).
	if (FPhysXInstanceData* Data = Instances.Find(ID))
	{
		if (physx::PxRigidActor* Actor = Data->Body.GetPxActor())
		{
			if (FPhysXInstanceUserData* UD = UserDataByID.FindRef(ID))
			{
				if (Actor->userData == UD)
				{
					Actor->userData = nullptr;
				}
			}
			else
			{
				// Last-resort safety: do not leave stale pointers on our custom actors.
				Actor->userData = nullptr;
			}
		}
	}

	// Free the allocation owned by the subsystem.
	if (FPhysXInstanceUserData** UDPtr = UserDataByID.Find(ID))
	{
		delete *UDPtr;
		UserDataByID.Remove(ID);
	}
}

FPhysXInstanceID UPhysXInstancedWorldSubsystem::GetInstanceIDFromPxActor(const physx::PxRigidActor* Actor) const
{
	if (!Actor || !Actor->userData)
	{
		return FPhysXInstanceID();
	}

	const FPhysXInstanceUserData* UD =
		reinterpret_cast<const FPhysXInstanceUserData*>(Actor->userData);

	if (UD->Magic != FPhysXInstanceUserData::MagicValue)
	{
		return FPhysXInstanceID();
	}

	return UD->InstanceID;
}

#endif // PHYSICS_INTERFACE_PHYSX

// ============================================================================
// PhysX globals / console variables
// ============================================================================

// Single shared material for all instanced bodies in this world.
static PxMaterial* GInstancedDefaultMaterial = nullptr;

// Toggle for using ParallelFor in AsyncPhysicsStep.
static TAutoConsoleVariable<int32> CVarPhysXInstancedUseParallelStep(
	TEXT("physxinstanced.AsyncStep.Parallel"),
	1,
	TEXT("Use ParallelFor in UPhysXInstancedWorldSubsystem::AsyncPhysicsStep.\n")
	TEXT("0 = run single-threaded on the game thread.\n")
	TEXT("1 = use ParallelFor when Jobs.Num() >= 64."),
	ECVF_Default);

// Hard limit on number of async jobs processed per frame in AsyncPhysicsStep.
static TAutoConsoleVariable<int32> CVarPhysXInstancedMaxJobsPerFrame(
	TEXT("physxinstanced.AsyncStep.MaxJobsPerFrame"),
	0,
	TEXT("Hard limit on number of async jobs processed per frame in UPhysXInstancedWorldSubsystem::AsyncPhysicsStep.\n")
	TEXT("0 = no limit (process all).\n")
	TEXT(">0 = clamp number of jobs to this value per frame."),
	ECVF_Default);

// Toggle for using ParallelFor in RegisterInstancesBatch (batched body creation).
static TAutoConsoleVariable<int32> CVarPhysXInstancedUseParallelRegister(
	TEXT("physxinstanced.Register.Parallel"),
	1,
	TEXT("Use ParallelFor in UPhysXInstancedWorldSubsystem::RegisterInstancesBatch.\n")
	TEXT("0 = create PhysX bodies on the game thread (no ParallelFor).\n")
	TEXT("1 = use ParallelFor when Jobs.Num() >= 32."),
	ECVF_Default);

#endif // PHYSICS_INTERFACE_PHYSX

namespace
{
#if ENABLE_DRAW_DEBUG

	static bool IsDebugEnabled(EPhysXInstancedQueryDebugMode Mode)
	{
		return Mode != EPhysXInstancedQueryDebugMode::None;
	}

	// Duration semantics:
	//  - Duration <= 0 : draw infinitely (persistent)
	//  - Duration > 0  : draw for Duration seconds (non-persistent + lifetime)
	static void MakeDebugDrawParams(float Duration, bool& OutPersistentLines, float& OutLifeTime, float& OutStringDuration)
	{
		if (Duration <= 0.0f)
		{
			OutPersistentLines = true;
			OutLifeTime        = 0.0f;  // persistent forever for primitives
			OutStringDuration  = -1.0f; // negative duration => persistent debug string
		}
		else
		{
			OutPersistentLines = false;
			OutLifeTime        = Duration;
			OutStringDuration  = Duration;
		}
	}

	static void DrawLineSafe(UWorld* World, const FVector& A, const FVector& B, const FColor& Color, float Duration, float Thickness = 1.5f)
	{
		if (!World) return;

		bool  bPersistent = false;
		float LifeTime = 0.0f;
		float StringDuration = 0.0f;
		MakeDebugDrawParams(Duration, bPersistent, LifeTime, StringDuration);

		DrawDebugLine(World, A, B, Color, bPersistent, LifeTime, 0, Thickness);
	}

	static void DrawPointSafe(UWorld* World, const FVector& P, const FColor& Color, float Duration, float Size = 12.0f)
	{
		if (!World) return;

		bool  bPersistent = false;
		float LifeTime = 0.0f;
		float StringDuration = 0.0f;
		MakeDebugDrawParams(Duration, bPersistent, LifeTime, StringDuration);

		DrawDebugPoint(World, P, Size, Color, bPersistent, LifeTime, 0);
	}

	static void DrawSphereSafe(UWorld* World, const FVector& C, float R, const FColor& Color, float Duration, float Thickness = 1.0f)
	{
		if (!World) return;

		bool  bPersistent = false;
		float LifeTime = 0.0f;
		float StringDuration = 0.0f;
		MakeDebugDrawParams(Duration, bPersistent, LifeTime, StringDuration);

		DrawDebugSphere(World, C, R, 16, Color, bPersistent, LifeTime, 0, Thickness);
	}

	static void DrawArrowSafe(UWorld* World, const FVector& From, const FVector& To, const FColor& Color, float Duration, float Thickness = 1.5f)
	{
		if (!World) return;

		bool  bPersistent = false;
		float LifeTime = 0.0f;
		float StringDuration = 0.0f;
		MakeDebugDrawParams(Duration, bPersistent, LifeTime, StringDuration);

		DrawDebugDirectionalArrow(World, From, To, 12.0f, Color, bPersistent, LifeTime, 0, Thickness);
	}

	static void DrawTextSafe(UWorld* World, const FVector& At, const FString& Text, const FColor& Color, float Duration)
	{
		if (!World) return;

		bool  bPersistent = false;
		float LifeTime = 0.0f;
		float StringDuration = 0.0f;
		MakeDebugDrawParams(Duration, bPersistent, LifeTime, StringDuration);

		// Note: DrawDebugString does not use bPersistentLines; it relies on Duration.
		// We treat Duration <= 0 as "infinite" by passing negative duration.
		DrawDebugString(World, At, Text, nullptr, Color, StringDuration, /*bDrawShadow=*/true);
	}

#endif // ENABLE_DRAW_DEBUG
} // namespace

namespace
{
	static bool IsOwnerStorageActor(const UInstancedStaticMeshComponent* ISMC)
	{
		if (!ISMC)
		{
			return false;
		}

		if (const AActor* Owner = ISMC->GetOwner())
		{
			if (const APhysXInstancedMeshActor* PhysXActor = Cast<APhysXInstancedMeshActor>(Owner))
			{
				return (PhysXActor->bIsStorageActor || PhysXActor->bStorageOnly);
			}
		}

		return false;
	}

	static bool GetInstanceWorldLocation_Safe(
		const FPhysXInstanceData& Data,
		FVector& OutLocation)
	{
		OutLocation = FVector::ZeroVector;

		UInstancedStaticMeshComponent* ISMC = Data.InstancedComponent.Get();
		if (!ISMC || !ISMC->IsValidLowLevelFast() || Data.InstanceIndex == INDEX_NONE)
		{
			return false;
		}

#if PHYSICS_INTERFACE_PHYSX
		if (physx::PxRigidActor* RA = Data.Body.GetPxActor())
		{
			const physx::PxTransform PxPose = RA->getGlobalPose();
			OutLocation = P2UVector(PxPose.p);
			return true;
		}
#endif

		FTransform TM;
		if (ISMC->GetInstanceTransform(Data.InstanceIndex, TM, /*bWorldSpace=*/true))
		{
			OutLocation = TM.GetLocation();
			return true;
		}

		return false;
	}
}

// ============================================================================
// Constructor
// ============================================================================

UPhysXInstancedWorldSubsystem::UPhysXInstancedWorldSubsystem()
{
	// All setup happens in Initialize().
	NumBodiesTotal      = 0;
	NumBodiesSimulating = 0;
	NumBodiesSleeping   = 0;
}

// ============================================================================
// UWorldSubsystem interface
// ============================================================================

void UPhysXInstancedWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	CachedWorld = GetWorld();
	
	PendingAddActorsHead = 0;
	PendingAddActors.Reset();

	Instances.Reset();
	NextID = 1;

	Actors.Reset();
	NextActorID = 1;

	NumBodiesLifetimeCreated = 0;

#if PHYSICS_INTERFACE_PHYSX
	// Create one default material for the whole lifetime of this subsystem.
	if (GPhysXSDK && !GInstancedDefaultMaterial)
	{
		const PxReal StaticFriction  = 0.6f;
		const PxReal DynamicFriction = 0.6f;
		const PxReal Restitution     = 0.1f;

		GInstancedDefaultMaterial = GPhysXSDK->createMaterial(
			StaticFriction,
			DynamicFriction,
			Restitution);
	}
#endif // PHYSICS_INTERFACE_PHYSX
}

void UPhysXInstancedWorldSubsystem::Deinitialize()
{
	CachedWorld.Reset();
	
	PendingAddActorsHead = 0;
	PendingAddActors.Reset();

#if PHYSICS_INTERFACE_PHYSX

	for (TPair<FPhysXInstanceID, FPhysXInstanceData>& Pair : Instances)
	{
		ClearInstanceUserData(Pair.Key);
		Pair.Value.Body.Destroy();
	}


	// Release the shared material.
	if (GInstancedDefaultMaterial)
	{
		GInstancedDefaultMaterial->release();
		GInstancedDefaultMaterial = nullptr;
	}
#endif // PHYSICS_INTERFACE_PHYSX

	Instances.Reset();

	Super::Deinitialize();
}

// ============================================================================
// UTickableWorldSubsystem interface
// ============================================================================

void UPhysXInstancedWorldSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

#if PHYSICS_INTERFACE_PHYSX
	ProcessPendingAddActors();
	ProcessInstanceTasks();
#endif

	AsyncPhysicsStep(DeltaTime, DeltaTime);
}

TStatId UPhysXInstancedWorldSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UPhysXInstancedSubsystem, STATGROUP_Tickables);
}

// ============================================================================
// Spawn API
// ============================================================================

FPhysXSpawnInstanceResult UPhysXInstancedWorldSubsystem::SpawnPhysicsInstance(
	const FPhysXSpawnInstanceRequest& Request)
{
	FPhysXSpawnInstanceResult Result;

	UWorld* World = GetWorld();
	if (!World)
	{
		return Result;
	}

	APhysXInstancedMeshActor* TargetActor = nullptr;

	// Local buffer of resolved materials used for actor matching.
	TArray<UMaterialInterface*> DesiredMaterials;

	auto BuildDesiredMaterials = [&DesiredMaterials](const FPhysXSpawnInstanceRequest& InRequest)
	{
		DesiredMaterials.Reset();

		UStaticMesh* StaticMesh = InRequest.StaticMesh;
		if (!StaticMesh)
		{
			return;
		}

		const int32 NumSlots = StaticMesh->GetStaticMaterials().Num();
		DesiredMaterials.Reserve(NumSlots);

		for (int32 SlotIndex = 0; SlotIndex < NumSlots; ++SlotIndex)
		{
			UMaterialInterface* Material = nullptr;

			if (InRequest.bUseOverrideMaterials &&
				InRequest.OverrideMaterials.IsValidIndex(SlotIndex))
			{
				Material = InRequest.OverrideMaterials[SlotIndex];
			}

			if (!Material)
			{
				Material = StaticMesh->GetMaterial(SlotIndex);
			}

			DesiredMaterials.Add(Material);
		}
	};

	// ---------------------------------------------------------------------
	// 1) Resolve / create the owning actor
	// ---------------------------------------------------------------------

	switch (Request.ActorMode)
	{
	case EPhysXInstanceActorMode::UseExplicitActor:
	{
		// Uses the actor provided in the request without modifying it.
		TargetActor = Request.ExplicitActor.Get();
		break;
	}

	case EPhysXInstanceActorMode::AlwaysCreateNew:
	case EPhysXInstanceActorMode::FindOrCreateByMeshAndMats:
	default:
	{
		if (!Request.StaticMesh)
		{
			return Result;
		}

		BuildDesiredMaterials(Request);

		// Try to reuse an existing actor with the same mesh and resolved materials.
		if (Request.ActorMode == EPhysXInstanceActorMode::FindOrCreateByMeshAndMats)
		{
			for (const TPair<FPhysXActorID, FPhysXActorData>& Pair : Actors)
			{
				APhysXInstancedMeshActor* Actor = Pair.Value.Actor.Get();
				if (!Actor || !Actor->IsValidLowLevelFast())
				{
					continue;
				}

				// Storage actors are not used for dynamic instance spawning.
				if (Actor->bIsStorageActor || Actor->bStorageOnly)
				{
					continue;
				}


				UInstancedStaticMeshComponent* ISMC = Actor->InstancedMesh;
				if (!ISMC)
				{
					continue;
				}

				if (ISMC->GetStaticMesh() != Request.StaticMesh)
				{
					continue;
				}

				const int32 NumSlots = DesiredMaterials.Num();
				bool bMaterialsMatch = true;

				for (int32 SlotIndex = 0; SlotIndex < NumSlots; ++SlotIndex)
				{
					if (ISMC->GetMaterial(SlotIndex) != DesiredMaterials[SlotIndex])
					{
						bMaterialsMatch = false;
						break;
					}
				}

				if (!bMaterialsMatch)
				{
					continue;
				}

				TargetActor = Actor;
				break;
			}
		}

		// Create a new actor if no suitable one was found.
		if (!TargetActor)
		{
			FActorSpawnParameters SpawnParams;
			SpawnParams.SpawnCollisionHandlingOverride =
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

			TargetActor = World->SpawnActor<APhysXInstancedMeshActor>(
				APhysXInstancedMeshActor::StaticClass(),
				Request.InstanceWorldTransform,
				SpawnParams);

			if (!TargetActor)
			{
				return Result;
			}

			// Register the actor in the subsystem for tracking.
			const FPhysXActorID NewActorID = RegisterInstancedMeshActor(TargetActor);
			TargetActor->PhysXActorID = NewActorID;

			// Copy mesh/material settings from the request into the actor.
			TargetActor->InstanceStaticMesh         = Request.StaticMesh;
			TargetActor->bOverrideInstanceMaterials = Request.bUseOverrideMaterials;
			TargetActor->InstanceOverrideMaterials  = Request.OverrideMaterials;

			TargetActor->ApplyInstanceMaterials();
		}
		break;
	}
	}

	// Guard: if the actor or its ISM component is missing, abort.
	if (!TargetActor || !TargetActor->InstancedMesh)
	{
		return Result;
	}

	// Ensure the actor is registered (relevant for UseExplicitActor mode).
	if (TargetActor->PhysXActorID.GetUniqueID() == 0u)
	{
		const FPhysXActorID NewActorID = RegisterInstancedMeshActor(TargetActor);
		TargetActor->PhysXActorID = NewActorID;
	}

	// ---------------------------------------------------------------------
	// 2) Add an ISM instance and register its PhysX body
	// ---------------------------------------------------------------------

	const FTransform& WorldTransform = Request.InstanceWorldTransform;

	const int32 NewInstanceIndex =
		TargetActor->InstancedMesh->AddInstanceWorldSpace(WorldTransform);

	if (NewInstanceIndex == INDEX_NONE)
	{
		return Result;
	}

	const bool bSimulate =
		Request.bStartSimulating && TargetActor->bSimulateInstances;

	const FPhysXInstanceID NewInstanceID =
		RegisterInstance(TargetActor->InstancedMesh, NewInstanceIndex, bSimulate);

	if (!NewInstanceID.IsValid())
	{
		// Roll back the ISM instance if PhysX registration failed.
		TargetActor->InstancedMesh->RemoveInstance(NewInstanceIndex);
		return Result;
	}

	// Actor keeps track of the instance handles it owns.
	TargetActor->RegisteredInstanceIDs.Add(NewInstanceID);

	// Initial velocities are applied only to the newly created instance.
	if (!Request.InitialLinearVelocity.IsNearlyZero())
	{
		SetInstanceLinearVelocity(
			NewInstanceID,
			Request.InitialLinearVelocity,
			/*bAutoWake=*/true);
	}

	if (!Request.InitialAngularVelocityRad.IsNearlyZero())
	{
		SetInstanceAngularVelocityInRadians(
			NewInstanceID,
			Request.InitialAngularVelocityRad,
			/*bAutoWake=*/true);
	}

	Result.bSuccess            = true;
	Result.Actor               = TargetActor;
	Result.InstanceIndex       = NewInstanceIndex;
	Result.InstanceID          = NewInstanceID;
	Result.FinalWorldTransform = WorldTransform;

	return Result;
}

// ============================================================================
// Registration API
// ============================================================================

FPhysXInstanceID UPhysXInstancedWorldSubsystem::RegisterInstance(
	UInstancedStaticMeshComponent* InstancedMesh,
	int32 InstanceIndex,
	bool bSimulate)
{
	// Measure CPU time spent registering a new instance in the subsystem.
	SCOPE_CYCLE_COUNTER(STAT_PhysXInstanced_RegisterInstance);

	if (!InstancedMesh || InstanceIndex < 0)
	{
		return FPhysXInstanceID(); // invalid
	}

	FPhysXInstanceID NewID(NextID++);

	FPhysXInstanceData NewData;
	NewData.InstancedComponent = InstancedMesh;
	NewData.InstanceIndex      = InstanceIndex;
	NewData.bSimulating        = bSimulate;
	NewData.SleepTime          = 0.0f;
	NewData.FallTime           = 0.0f;
	NewData.bWasSleeping       = false;

#if !PHYSICS_INTERFACE_PHYSX
	// Without PhysX, only bookkeeping data is stored.
	Instances.Add(NewID, NewData);
	AddSlotMapping(NewID);
	++NumBodiesTotal;
	if (NewData.bSimulating)
	{
		++NumBodiesSimulating;
	}

	return NewID;
#else
	// If PhysX is present but the shared material is missing, only store bookkeeping data.
	if (!GInstancedDefaultMaterial)
	{
		Instances.Add(NewID, NewData);
		AddSlotMapping(NewID);
		// No PxActor exists in this path, so EnsureInstanceUserData() is a no-op.
		EnqueueAddActorToScene(NewID, InstancedMesh);
		
		++NumBodiesTotal;
		if (NewData.bSimulating)
		{
			++NumBodiesSimulating;
		}

		return NewID;
	}

	// Read shape settings from the owning PhysX instanced mesh actor.
	EPhysXInstanceShapeType ShapeType = EPhysXInstanceShapeType::Box;
	UStaticMesh* OverrideMesh         = nullptr;

	if (AActor* Owner = InstancedMesh->GetOwner())
	{
		if (const APhysXInstancedMeshActor* PhysXActor = Cast<APhysXInstancedMeshActor>(Owner))
		{
			ShapeType    = PhysXActor->InstanceShapeType;
			OverrideMesh = PhysXActor->OverrideCollisionMesh;
		}
	}

	// Create a PhysX body via FPhysXInstanceBody helper.
	if (!NewData.Body.CreateFromInstancedStaticMesh(
		InstancedMesh,
		InstanceIndex,
		bSimulate,
		GInstancedDefaultMaterial,
		ShapeType,
		OverrideMesh))
	{
		// Creation failed: do not add to the map, return an invalid ID.
		return FPhysXInstanceID();
	}
	
	// IMPORTANT:
	// userData setup requires the instance record to exist in Instances.
	Instances.Add(NewID, NewData);
	AddSlotMapping(NewID);
	EnsureInstanceUserData(NewID);
	
	++NumBodiesLifetimeCreated;
	++NumBodiesTotal;
	if (NewData.bSimulating)
	{
		++NumBodiesSimulating;
	}

	// Defer adding the PhysX actor to the scene to a separate budgeted phase.
	EnqueueAddActorToScene(NewID, InstancedMesh);

	return NewID;
#endif // PHYSICS_INTERFACE_PHYSX
}

void UPhysXInstancedWorldSubsystem::RegisterInstancesBatch(
	UInstancedStaticMeshComponent* InstancedMesh,
	const TArray<int32>& InstanceIndices,
	bool bSimulate,
	TArray<FPhysXInstanceID>& OutInstanceIDs)
{
	OutInstanceIDs.Reset();

#if !PHYSICS_INTERFACE_PHYSX
	// Without PhysX, registration is performed by calling RegisterInstance per index.
	if (!InstancedMesh || InstanceIndices.Num() == 0)
	{
		return;
	}

	OutInstanceIDs.Reserve(InstanceIndices.Num());

	for (int32 InstanceIndex : InstanceIndices)
	{
		OutInstanceIDs.Add(RegisterInstance(InstancedMesh, InstanceIndex, bSimulate));
	}

	return;

#else // PHYSICS_INTERFACE_PHYSX

	SCOPE_CYCLE_COUNTER(STAT_PhysXInstanced_RegisterInstance);

	if (!InstancedMesh || InstanceIndices.Num() == 0)
	{
		return;
	}

	OutInstanceIDs.Reserve(InstanceIndices.Num());

	// If the default material is missing, fall back to single-instance registration.
	if (!GInstancedDefaultMaterial)
	{
		for (int32 InstanceIndex : InstanceIndices)
		{
			OutInstanceIDs.Add(RegisterInstance(InstancedMesh, InstanceIndex, bSimulate));
		}
		return;
	}

	// ---------------------------------------------------------
	// 0) Read shape settings once from the owning actor
	// ---------------------------------------------------------

	EPhysXInstanceShapeType ShapeType = EPhysXInstanceShapeType::Box;
	UStaticMesh* OverrideMesh         = nullptr;

	if (AActor* Owner = InstancedMesh->GetOwner())
	{
		if (const APhysXInstancedMeshActor* PhysXActor =
			Cast<APhysXInstancedMeshActor>(Owner))
		{
			ShapeType    = PhysXActor->InstanceShapeType;
			OverrideMesh = PhysXActor->OverrideCollisionMesh;
		}
	}

	// ---------------------------------------------------------
	// 1) Create bookkeeping entries and build the job array
	// ---------------------------------------------------------

	struct FPhysXInstanceCreateJob
	{
		FPhysXInstanceID               ID;
		FPhysXInstanceData*            Data = nullptr;
		UInstancedStaticMeshComponent* ISMC = nullptr;
		int32                          InstanceIndex = INDEX_NONE;
		bool                           bSimulate = false;

		bool                           bSuccess = false;
	};

	const int32 NumToRegister = InstanceIndices.Num();

	Instances.Reserve(Instances.Num() + NumToRegister);

	TArray<FPhysXInstanceCreateJob> Jobs;
	Jobs.Reserve(NumToRegister);

	{
		SCOPE_CYCLE_COUNTER(STAT_PhysXInstanced_RegisterPrepareJobs);

		for (int32 InstanceIndex : InstanceIndices)
		{
			if (!ensureMsgf(InstanceIndex >= 0,
				TEXT("RegisterInstancesBatch: got negative InstanceIndex=%d"), InstanceIndex))
			{
				continue;
			}

			const FPhysXInstanceID NewID(NextID++);

			FPhysXInstanceData NewData;
			NewData.InstancedComponent = InstancedMesh;
			NewData.InstanceIndex      = InstanceIndex;
			NewData.bSimulating        = bSimulate;
			NewData.SleepTime          = 0.0f;
			NewData.FallTime           = 0.0f;
			NewData.bWasSleeping       = false;

			FPhysXInstanceData& StoredData = Instances.Add(NewID, NewData);
			AddSlotMapping(NewID);

			FPhysXInstanceCreateJob& Job = Jobs.AddDefaulted_GetRef();
			Job.ID            = NewID;
			Job.Data          = &StoredData;
			Job.ISMC          = InstancedMesh;
			Job.InstanceIndex = InstanceIndex;
			Job.bSimulate     = bSimulate;

			OutInstanceIDs.Add(NewID);
		}
	}

	if (Jobs.Num() == 0)
	{
		return;
	}

	// ---------------------------------------------------------
	// 2) Create PhysX bodies (optionally parallel)
	// ---------------------------------------------------------

	auto DoCreateBodyForJob = [ShapeType, OverrideMesh](FPhysXInstanceCreateJob& Job)
	{
		SCOPE_CYCLE_COUNTER(STAT_PhysXInstanced_RegisterCreateBodyWorker);

		if (!Job.ISMC || !Job.Data)
		{
			return;
		}

		Job.bSuccess = Job.Data->Body.CreateFromInstancedStaticMesh(
			Job.ISMC,
			Job.InstanceIndex,
			Job.bSimulate,
			GInstancedDefaultMaterial,
			ShapeType,
			OverrideMesh);
	};

	const bool bUseParallelRegister =
		(CVarPhysXInstancedUseParallelRegister.GetValueOnGameThread() != 0) &&
		(Jobs.Num() >= 32);

	if (bUseParallelRegister)
	{
		ParallelFor(Jobs.Num(), [&Jobs, &DoCreateBodyForJob](int32 JobIndex)
		{
			DoCreateBodyForJob(Jobs[JobIndex]);
		});
	}
	else
	{
		for (int32 JobIndex = 0; JobIndex < Jobs.Num(); ++JobIndex)
		{
			DoCreateBodyForJob(Jobs[JobIndex]);
		}
	}

	// ---------------------------------------------------------
	// 3) Finalize on the game thread: cleanup and stats
	// ---------------------------------------------------------

	{
		SCOPE_CYCLE_COUNTER(STAT_PhysXInstanced_RegisterFinalize);

		for (int32 JobIndex = 0; JobIndex < Jobs.Num(); ++JobIndex)
		{
			FPhysXInstanceCreateJob& Job = Jobs[JobIndex];

			if (!Job.bSuccess)
			{
				Instances.Remove(Job.ID);
				InstanceIDBySlot.Remove(FPhysXInstanceSlotKey(Job.ISMC, Job.InstanceIndex));

				if (OutInstanceIDs.IsValidIndex(JobIndex))
				{
					OutInstanceIDs[JobIndex] = FPhysXInstanceID(); // invalid
				}

				continue;
			}

			// Queue PhysX actor for scene insertion on the game thread.
			EnsureInstanceUserData(Job.ID);
			EnqueueAddActorToScene(Job.ID, Job.ISMC);

			++NumBodiesLifetimeCreated;
			++NumBodiesTotal;
			
			if (Job.Data && Job.Data->bSimulating)
			{
				++NumBodiesSimulating;
			}
		}
	}

#endif // PHYSICS_INTERFACE_PHYSX
}

void UPhysXInstancedWorldSubsystem::UnregisterInstance(FPhysXInstanceID ID)
{
	if (FPhysXInstanceData* Data = Instances.Find(ID))
	{
#if PHYSICS_INTERFACE_PHYSX
		ClearInstanceUserData(ID);
		Data->Body.Destroy();
#endif

		if (NumBodiesTotal > 0)
		{
			--NumBodiesTotal;
		}

		if (Data->bSimulating && NumBodiesSimulating > 0)
		{
			--NumBodiesSimulating;
		}

		RemoveSlotMapping(ID);
		InvalidatePendingAddEntries(ID);
		Instances.Remove(ID);
	}
#if PHYSICS_INTERFACE_PHYSX
	else
	{
		// If the instance record is already gone, still ensure we don't leak user-data.
		ClearInstanceUserData(ID);
	}
#endif
}

#if PHYSICS_INTERFACE_PHYSX

namespace
{
	struct FPhysXInstanceAsyncStepJob
	{
		// Input data populated on the game thread.
		FPhysXInstanceID               ID;
		FPhysXInstanceData*            Data = nullptr;
		UInstancedStaticMeshComponent* ISMC = nullptr;
		physx::PxRigidDynamic*         RigidDynamic = nullptr;

		// Per-frame config snapshots copied from the owning actor.
		FPhysXInstanceStopConfig       StopConfig;
		FPhysXInstanceCCDConfig        CCDConfig;
		bool                           bUseCustomKillZ = false;
		float                          CustomKillZ = 0.0f;
		EPhysXInstanceStopAction       LostInstanceAction = EPhysXInstanceStopAction::None;

		// Owner actor location used for max-distance checks.
		bool                           bHasOwnerLocation = false;
		FVector                        OwnerLocation = FVector::ZeroVector;

		// Results computed in the worker.
		FTransform                     NewWorldTransform;
		FVector                        NewLocation = FVector::ZeroVector;
		bool                           bSleeping = false;

		// Copy of InstanceData->bWasSleeping from the start of the frame.
		bool                           bWasSleepingInitial = false;

		// Auto-stop decisions.
		bool                           bApplyStopAction = false;
		EPhysXInstanceStopAction       ActionToApply = EPhysXInstanceStopAction::None;

		// CCD decisions.
		bool                           bEnableCCD  = false;
		bool                           bDisableCCD = false;

		// Updated timers.
		float                          NewSleepTime = 0.0f;
		float                          NewFallTime  = 0.0f;
	};

	// Reused every frame to avoid per-tick allocations in AsyncPhysicsStep.
	static TArray<FPhysXInstanceAsyncStepJob> GAsyncStepJobs;
}

#endif // PHYSICS_INTERFACE_PHYSX

// ============================================================================
// Physics update
// ============================================================================

void UPhysXInstancedWorldSubsystem::AsyncPhysicsStep(float DeltaTime, float SimTime)
{
#if !PHYSICS_INTERFACE_PHYSX
	// No PhysX backend: nothing to do.
	return;
#else

	// Early-out only when nothing is registered at all.
	if (Instances.Num() == 0)
	{
		NumBodiesTotal = 0;
		NumBodiesSimulating = 0;
		NumBodiesSleeping = 0;

		SET_DWORD_STAT(STAT_PhysXInstanced_BodiesTotal,      0);
		SET_DWORD_STAT(STAT_PhysXInstanced_BodiesSimulating, 0);
		SET_DWORD_STAT(STAT_PhysXInstanced_BodiesSleeping,   0);
		SET_DWORD_STAT(STAT_PhysXInstanced_JobsPerFrame,     0);
		SET_DWORD_STAT(STAT_PhysXInstanced_InstancesTotal,   0);

		const uint32 LifetimeClamped = (uint32)FMath::Min<uint64>(NumBodiesLifetimeCreated, (uint64)MAX_uint32);
		SET_DWORD_STAT(STAT_PhysXInstanced_BodiesLifetimeCreated, LifetimeClamped);
		return;
	}

	// Total number of registered instances (simulating / sleeping / storage).
	SET_DWORD_STAT(STAT_PhysXInstanced_InstancesTotal, Instances.Num());

	SCOPE_CYCLE_COUNTER(STAT_PhysXInstanced_AsyncPhysicsStep);

	// Local per-frame counters.
	int32 LocalTotal    = 0; // total PhysX bodies with valid PxRigidDynamic
	int32 LocalSleeping = 0; // how many of them are considered sleeping

	// --------------------------------------------------------------------
	// 0) Query active actors from the PhysX scene
	// --------------------------------------------------------------------

	PxScene* PxScenePtr = nullptr;
	if (UWorld* World = GetWorld())
	{
		PxScenePtr = GetPhysXSceneFromWorld(World);
	}

	TSet<PxRigidActor*> ActiveActorsSet;
	int32 NumActiveActorsFromScene = 0;

	if (PxScenePtr)
	{
		PxU32     NumActive    = 0;
		PxActor** ActiveArray  = PxScenePtr->getActiveActors(NumActive);

		if (ActiveArray && NumActive > 0)
		{
			NumActiveActorsFromScene = static_cast<int32>(NumActive);
			ActiveActorsSet.Reserve(NumActiveActorsFromScene);

			for (PxU32 Index = 0; Index < NumActive; ++Index)
			{
				if (PxActor* Actor = ActiveArray[Index])
				{
					if (PxRigidActor* Rigid = Actor->is<PxRigidActor>())
					{
						ActiveActorsSet.Add(Rigid);
					}
				}
			}
		}
	}

#if DO_GUARD_SLOW
	// Debug-only: verify that a single ISM slot is not used by two InstanceIDs.
	{
		TMap<TPair<UInstancedStaticMeshComponent*, int32>, FPhysXInstanceID> SlotOwners;

		for (const TPair<FPhysXInstanceID, FPhysXInstanceData>& Pair : Instances)
		{
			const FPhysXInstanceData& DataCheck = Pair.Value;
			UInstancedStaticMeshComponent* ISMCCheck = DataCheck.InstancedComponent.Get();

			if (!ISMCCheck || DataCheck.InstanceIndex == INDEX_NONE)
			{
				continue;
			}

			const int32 NumInstances = ISMCCheck->GetInstanceCount();
			if (DataCheck.InstanceIndex < 0 || DataCheck.InstanceIndex >= NumInstances)
			{
				UE_LOG(LogTemp, Error,
					TEXT("[PhysXInstanced] Invalid InstanceIndex=%d (Num=%d) for ID=%u, ISMC=%s"),
					DataCheck.InstanceIndex,
					NumInstances,
					Pair.Key.GetUniqueID(),
					*GetNameSafe(ISMCCheck));
				continue;
			}

			TPair<UInstancedStaticMeshComponent*, int32> Key(ISMCCheck, DataCheck.InstanceIndex);
			if (const FPhysXInstanceID* Existing = SlotOwners.Find(Key))
			{
				UE_LOG(LogTemp, Error,
					TEXT("[PhysXInstanced] Duplicate slot (ISMC=%s, Index=%d) for IDs %u and %u"),
					*GetNameSafe(ISMCCheck),
					DataCheck.InstanceIndex,
					Existing->GetUniqueID(),
					Pair.Key.GetUniqueID());
			}
			else
			{
				SlotOwners.Add(Key, Pair.Key);
			}
		}
	}
#endif // DO_GUARD_SLOW

	// --------------------------------------------------------------------
	// 1) Build jobs for simulating bodies that are worth processing
	// --------------------------------------------------------------------

	TArray<FPhysXInstanceAsyncStepJob>& Jobs = GAsyncStepJobs;
	Jobs.Reset();
	Jobs.Reserve(Instances.Num());

	{
		SCOPE_CYCLE_COUNTER(STAT_PhysXInstanced_AsyncBuildJobs);

		struct FActorFrameConfig
		{
			FPhysXInstanceStopConfig  StopConfig;
			FPhysXInstanceCCDConfig   CCDConfig;

			bool                      bUseCustomKillZ = false;
			float                     CustomKillZ = 0.0f;
			EPhysXInstanceStopAction  LostInstanceAction = EPhysXInstanceStopAction::None;

			FVector                   OwnerLocation = FVector::ZeroVector;
		};

		TMap<const APhysXInstancedMeshActor*, FActorFrameConfig> ActorConfigs;
		ActorConfigs.Reserve(Actors.Num());

		const int32 MaxJobsPerFrame =
			CVarPhysXInstancedMaxJobsPerFrame.GetValueOnGameThread();

		int32 NumJobsAdded = 0;

		// Active-actor filtering is used only when the scene provides an active list.
		const bool bUseActiveActorFilter = (ActiveActorsSet.Num() > 0) && (PxScenePtr != nullptr);

		for (TPair<FPhysXInstanceID, FPhysXInstanceData>& Pair : Instances)
		{
			if (MaxJobsPerFrame > 0 && NumJobsAdded >= MaxJobsPerFrame)
			{
				break;
			}

			FPhysXInstanceData& InstanceData = Pair.Value;

			if (!InstanceData.bSimulating)
			{
				continue;
			}

			PxRigidActor*   RigidActor   = InstanceData.Body.GetPxActor();
			PxRigidDynamic* RigidDynamic = RigidActor ? RigidActor->is<PxRigidDynamic>() : nullptr;
			if (!RigidDynamic)
			{
				InstanceData.bSimulating = false;
				continue;
			}

			// Skip bodies that are not yet in a scene.
			if (!RigidDynamic->getScene())
			{
				InstanceData.SleepTime = 0.0f;
				InstanceData.FallTime  = 0.0f;
				continue;
			}

			UInstancedStaticMeshComponent* ISMC = InstanceData.InstancedComponent.Get();
			if (!ISMC || !ISMC->IsValidLowLevelFast())
			{
				continue;
			}

			// Validate InstanceIndex.
			if (InstanceData.InstanceIndex != INDEX_NONE)
			{
				const int32 NumInstances = ISMC->GetInstanceCount();
				if (InstanceData.InstanceIndex < 0 || InstanceData.InstanceIndex >= NumInstances)
				{
					UE_LOG(LogTemp, Error,
						TEXT("[PhysXInstanced] AsyncPhysicsStep: InstanceIndex=%d is out of range [0, %d) for component %s. "
							 "Destroying PhysX body and disabling simulation for this instance."),
						InstanceData.InstanceIndex, NumInstances, *ISMC->GetName());

					InstanceData.Body.Destroy();
					InstanceData.bSimulating   = false;
					InstanceData.InstanceIndex = INDEX_NONE;
					InstanceData.SleepTime     = 0.0f;
					InstanceData.FallTime      = 0.0f;
					continue;
				}
			}

			// At this point the body is counted as tracked for stats.
			++LocalTotal;

			// Filter by active actors from the scene when the active list is available.
			bool bIsActiveActor = true;
			if (bUseActiveActorFilter)
			{
				// Only trust ActiveActorsSet if we queried the SAME PxScene the body belongs to.
				if (RigidDynamic->getScene() == PxScenePtr)
				{
					bIsActiveActor = ActiveActorsSet.Contains(RigidActor);
				}
				// If scenes differ (Sync vs Async), do NOT filter by ActiveActorsSet.
			}

			// Bodies reported as inactive by the scene are treated as sleeping and do not produce jobs.
			if (!bIsActiveActor)
			{
				InstanceData.SleepTime    += DeltaTime;
				InstanceData.bWasSleeping  = true;

				++LocalSleeping;
				continue;
			}

			// 1.1) Pull/cached per-actor config.
			FActorFrameConfig* ActorCfg = nullptr;

			if (AActor* Owner = ISMC->GetOwner())
			{
				if (const APhysXInstancedMeshActor* PhysXActor = Cast<APhysXInstancedMeshActor>(Owner))
				{
					ActorCfg = ActorConfigs.Find(PhysXActor);
					if (!ActorCfg)
					{
						FActorFrameConfig NewCfg;
						NewCfg.StopConfig         = PhysXActor->AutoStopConfig;
						NewCfg.CCDConfig          = PhysXActor->CCDConfig;
						NewCfg.bUseCustomKillZ    = PhysXActor->bUseCustomKillZ;
						NewCfg.CustomKillZ        = PhysXActor->CustomKillZ;
						NewCfg.LostInstanceAction = PhysXActor->LostInstanceAction;
						NewCfg.OwnerLocation      = PhysXActor->GetActorLocation();

						ActorCfg = &ActorConfigs.Add(PhysXActor, NewCfg);
					}
				}
			}

			FPhysXInstanceStopConfig  StopConfig;
			FPhysXInstanceCCDConfig   CCDConfig;
			bool                      bUseCustomKillZ    = false;
			float                     CustomKillZ        = 0.0f;
			EPhysXInstanceStopAction  LostInstanceAction = EPhysXInstanceStopAction::None;
			bool                      bHasOwnerLocation  = false;
			FVector                   OwnerLocation      = FVector::ZeroVector;

			if (ActorCfg)
			{
				StopConfig         = ActorCfg->StopConfig;
				CCDConfig          = ActorCfg->CCDConfig;
				bUseCustomKillZ    = ActorCfg->bUseCustomKillZ;
				CustomKillZ        = ActorCfg->CustomKillZ;
				LostInstanceAction = ActorCfg->LostInstanceAction;
				bHasOwnerLocation  = true;
				OwnerLocation      = ActorCfg->OwnerLocation;
			}

			// 1.2) Create a job for this instance.
			FPhysXInstanceAsyncStepJob Job;
			Job.ID           = Pair.Key;
			Job.Data         = &InstanceData;
			Job.ISMC         = ISMC;
			Job.RigidDynamic = RigidDynamic;

			Job.StopConfig         = StopConfig;
			Job.CCDConfig          = CCDConfig;
			Job.bUseCustomKillZ    = bUseCustomKillZ;
			Job.CustomKillZ        = CustomKillZ;
			Job.LostInstanceAction = LostInstanceAction;
			Job.bHasOwnerLocation  = bHasOwnerLocation;
			Job.OwnerLocation      = OwnerLocation;

			Job.NewSleepTime = InstanceData.SleepTime;
			Job.NewFallTime  = InstanceData.FallTime;

			// Cache the previous sleeping state for "just fell asleep" transform updates.
			Job.bWasSleepingInitial = InstanceData.bWasSleeping;

			Jobs.Add(Job);
			++NumJobsAdded;
		}
	}

	if (Jobs.Num() == 0)
	{
		NumBodiesTotal      = LocalTotal;
		NumBodiesSleeping   = LocalSleeping;
		NumBodiesSimulating = NumBodiesTotal - NumBodiesSleeping;

		SET_DWORD_STAT(STAT_PhysXInstanced_BodiesTotal,      NumBodiesTotal);
		SET_DWORD_STAT(STAT_PhysXInstanced_BodiesSimulating, NumBodiesSimulating);
		SET_DWORD_STAT(STAT_PhysXInstanced_BodiesSleeping,   NumBodiesSleeping);
		SET_DWORD_STAT(STAT_PhysXInstanced_JobsPerFrame,     0);
		const uint32 LifetimeClamped = (uint32)FMath::Min<uint64>(NumBodiesLifetimeCreated, (uint64)MAX_uint32);
		SET_DWORD_STAT(STAT_PhysXInstanced_BodiesLifetimeCreated, LifetimeClamped);

		return;
	}

	SET_DWORD_STAT(STAT_PhysXInstanced_JobsPerFrame, Jobs.Num());

	// --------------------------------------------------------------------
	// 2) Parallel phase: read PhysX state and compute decisions
	// --------------------------------------------------------------------

	auto StepAsyncJob = [DeltaTime](FPhysXInstanceAsyncStepJob& Job)
	{
		SCOPE_CYCLE_COUNTER(STAT_PhysXInstanced_AsyncJobWorker);

		if (!Job.Data || !Job.RigidDynamic)
		{
			return;
		}

		FPhysXInstanceData* InstanceData = Job.Data;

		// Sleeping status is computed before pose evaluation.
		const bool bSleepingNow = Job.RigidDynamic->isSleeping();
		Job.bSleeping = bSleepingNow;

		Job.bApplyStopAction = false;
		Job.ActionToApply    = EPhysXInstanceStopAction::None;
		Job.bEnableCCD       = false;
		Job.bDisableCCD      = false;

		const bool bHasAutoStop =
			(Job.StopConfig.bEnableAutoStop &&
			 Job.StopConfig.Action != EPhysXInstanceStopAction::None);

		const bool bHasSafetyRule =
			Job.StopConfig.bUseMaxFallTime ||
			Job.StopConfig.bUseMaxDistanceFromActor ||
			Job.bUseCustomKillZ;

		const bool bUsesAutoCCD =
			(Job.CCDConfig.Mode == EPhysXInstanceCCDMode::AutoByVelocity);

		const bool bCanUseSleepingFastPath =
			InstanceData->bWasSleeping &&
			bSleepingNow &&
			!bHasAutoStop &&
			!bHasSafetyRule &&
			!bUsesAutoCCD;

		if (bCanUseSleepingFastPath)
		{
			Job.NewSleepTime = InstanceData->SleepTime;
			Job.NewFallTime  = InstanceData->FallTime;
			return;
		}

		// Pose
		const PxTransform PxPose = Job.RigidDynamic->getGlobalPose();

		const FVector ULocation = P2UVector(PxPose.p);
		const FQuat   URotation = P2UQuat(PxPose.q);

		Job.NewWorldTransform = FTransform(URotation, ULocation, FVector::OneVector);
		Job.NewLocation       = ULocation;

		// Custom KillZ is evaluated before other rules.
		if (Job.bUseCustomKillZ && ULocation.Z < Job.CustomKillZ)
		{
			if (Job.LostInstanceAction != EPhysXInstanceStopAction::None)
			{
				Job.bApplyStopAction = true;
				Job.ActionToApply    = Job.LostInstanceAction;
			}

			Job.NewSleepTime = 0.0f;
			Job.NewFallTime  = 0.0f;
			return;
		}

		// If auto-stop is disabled, only the sleeping timer is tracked.
		if (!Job.StopConfig.bEnableAutoStop ||
			Job.StopConfig.Action == EPhysXInstanceStopAction::None)
		{
			Job.NewSleepTime = bSleepingNow
				? (InstanceData->SleepTime + DeltaTime)
				: 0.0f;

			Job.NewFallTime = 0.0f;
			return;
		}

		// Velocities are read only when required by stop rules or CCD.
		FVector LinVelU     = FVector::ZeroVector;
		float   LinearSpeed = 0.0f;
		float   AngSpeedDeg = 0.0f;

		const bool bNeedVelForStopCondition =
			(Job.StopConfig.Condition == EPhysXInstanceStopCondition::VelocityThreshold ||
			 Job.StopConfig.Condition == EPhysXInstanceStopCondition::SleepOrVelocity ||
			 Job.StopConfig.Condition == EPhysXInstanceStopCondition::SleepAndVelocity);

		const bool bNeedVelForFallTime = Job.StopConfig.bUseMaxFallTime;
		const bool bNeedVelForCCD      = (Job.CCDConfig.Mode == EPhysXInstanceCCDMode::AutoByVelocity);

		const bool bNeedAngularSpeed =
			bNeedVelForStopCondition && (Job.StopConfig.AngularSpeedThreshold > 0.0f);

		const bool bNeedLinearSpeed =
			bNeedVelForStopCondition || bNeedVelForFallTime || bNeedVelForCCD;

		if (bNeedLinearSpeed || bNeedAngularSpeed)
		{
			const PxVec3 LinVelPx = Job.RigidDynamic->getLinearVelocity();
			LinVelU               = P2UVector(LinVelPx);
			LinearSpeed           = LinVelU.Size();

			if (bNeedAngularSpeed)
			{
				const PxVec3 AngVelPx    = Job.RigidDynamic->getAngularVelocity();
				const float  AngSpeedRad = AngVelPx.magnitude();
				AngSpeedDeg = FMath::RadiansToDegrees(AngSpeedRad);
			}
		}

		// AutoByVelocity toggles CCD based on current linear speed.
		if (Job.CCDConfig.Mode == EPhysXInstanceCCDMode::AutoByVelocity)
		{
			const float MinVel = Job.CCDConfig.MinCCDVelocity;
			const bool  bShouldUseCCD = (LinearSpeed >= MinVel);

			const bool bCurrentlyCCD =
				Job.RigidDynamic->getRigidBodyFlags().isSet(PxRigidBodyFlag::eENABLE_CCD);

			if (bShouldUseCCD && !bCurrentlyCCD)
			{
				Job.bEnableCCD = true;
			}
			else if (!bShouldUseCCD && bCurrentlyCCD)
			{
				Job.bDisableCCD = true;
			}
		}

		// MaxFallTime accumulates time while Z velocity remains negative.
		if (Job.StopConfig.bUseMaxFallTime)
		{
			if (LinVelU.Z < 0.0f)
			{
				Job.NewFallTime += DeltaTime;
			}
			else
			{
				Job.NewFallTime = 0.0f;
			}

			if (Job.NewFallTime >= Job.StopConfig.MaxFallTime &&
				Job.StopConfig.Action != EPhysXInstanceStopAction::None)
			{
				Job.bApplyStopAction = true;
				Job.ActionToApply    = Job.StopConfig.Action;
				Job.NewSleepTime     = 0.0f;
				Job.NewFallTime      = 0.0f;
				return;
			}
		}
		else
		{
			Job.NewFallTime = 0.0f;
		}

		// MaxDistanceFromActor stops the instance when it exceeds the configured radius.
		if (Job.StopConfig.bUseMaxDistanceFromActor &&
			Job.bHasOwnerLocation &&
			Job.StopConfig.MaxDistanceFromActor > 0.0f &&
			Job.StopConfig.Action != EPhysXInstanceStopAction::None)
		{
			const float MaxDistSq = FMath::Square(Job.StopConfig.MaxDistanceFromActor);
			const float DistSq    = FVector::DistSquared(Job.OwnerLocation, ULocation);

			if (DistSq > MaxDistSq)
			{
				Job.bApplyStopAction = true;
				Job.ActionToApply    = Job.StopConfig.Action;
				Job.NewSleepTime     = 0.0f;
				Job.NewFallTime      = 0.0f;
				return;
			}
		}

		// Main stop condition based on the selected rule.
		const bool bBelowVelocityThreshold =
			(LinearSpeed <= Job.StopConfig.LinearSpeedThreshold) &&
			(AngSpeedDeg <= Job.StopConfig.AngularSpeedThreshold);

		bool bStopConditionNow = false;

		switch (static_cast<int32>(Job.StopConfig.Condition))
		{
		case 0: // PhysXSleepFlag
			bStopConditionNow = bSleepingNow;
			break;
		case 1: // VelocityThreshold
			bStopConditionNow = bBelowVelocityThreshold;
			break;
		case 2: // SleepOrVelocity
			bStopConditionNow = bSleepingNow || bBelowVelocityThreshold;
			break;
		case 3: // SleepAndVelocity
			bStopConditionNow = bSleepingNow && bBelowVelocityThreshold;
			break;
		default:
			bStopConditionNow = false;
			break;
		}

		if (!bStopConditionNow || Job.StopConfig.MinStoppedTime <= 0.0f)
		{
			Job.NewSleepTime = 0.0f;
			return;
		}

		// Accumulate time while the stop condition remains satisfied.
		Job.NewSleepTime += DeltaTime;
		if (Job.NewSleepTime >= Job.StopConfig.MinStoppedTime)
		{
			Job.bApplyStopAction = true;
			Job.ActionToApply    = Job.StopConfig.Action;
			Job.NewSleepTime     = 0.0f;
			Job.NewFallTime      = 0.0f;
		}
	};

	{
		SCOPE_CYCLE_COUNTER(STAT_PhysXInstanced_AsyncParallel);

		const bool bUseParallel =
			(CVarPhysXInstancedUseParallelStep.GetValueOnGameThread() != 0) &&
			(Jobs.Num() >= 64);

		if (bUseParallel)
		{
			ParallelFor(Jobs.Num(), [&Jobs, &StepAsyncJob](int32 JobIndex)
			{
				StepAsyncJob(Jobs[JobIndex]);
			});
		}
		else
		{
			for (int32 JobIndex = 0; JobIndex < Jobs.Num(); ++JobIndex)
			{
				StepAsyncJob(Jobs[JobIndex]);
			}
		}
	}

	// --------------------------------------------------------------------
	// 3) Apply results on the game thread (two passes)
	// --------------------------------------------------------------------

	{
		SCOPE_CYCLE_COUNTER(STAT_PhysXInstanced_AsyncApply);

		// Fallback components mark render state dirty once per component.
		TSet<UInstancedStaticMeshComponent*> DirtyComponents;
		DirtyComponents.Reserve(Jobs.Num());

		struct FComponentTransformBatch
		{
			TArray<int32>      InstanceIndices;
			TArray<FTransform> WorldTransforms;
		};

		// Batches are used only for UPhysXInstancedStaticMeshComponent.
		TMap<UPhysXInstancedStaticMeshComponent*, FComponentTransformBatch> ComponentBatches;
		ComponentBatches.Reserve(Jobs.Num());
		
		auto ApplyStopAction_GameThread = [this, &DirtyComponents](FPhysXInstanceAsyncStepJob& JobData)
		{
			FPhysXInstanceData*            InstanceData = JobData.Data;
			UInstancedStaticMeshComponent* ISMComponent = JobData.ISMC;
			PxRigidDynamic*                Rigid        = JobData.RigidDynamic;

			if (!InstanceData)
			{
				return;
			}

			switch (JobData.ActionToApply)
			{
			case EPhysXInstanceStopAction::DisableSimulation:
				if (Rigid)
				{
					Rigid->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC,       true);
					Rigid->setActorFlag     (PxActorFlag::eDISABLE_SIMULATION, true);
				}
				InstanceData->bSimulating = false;
				break;

			case EPhysXInstanceStopAction::DestroyBody:
				ClearInstanceUserData(JobData.ID);
				InstanceData->Body.Destroy();
				InstanceData->bSimulating = false;
				break;

			case EPhysXInstanceStopAction::DestroyBodyAndRemoveInstance:
			{
					const FPhysXInstanceID RemoveID = JobData.ID;

					// Remove both the body and the visual ISM instance by stable ID.
					RemoveInstanceByID(RemoveID, /*bRemoveVisualInstance=*/true);

					// IMPORTANT: RemoveInstanceByID() calls UnregisterInstance() -> map entry is gone.
					JobData.Data         = nullptr;
					JobData.ISMC         = nullptr;
					JobData.RigidDynamic = nullptr;
					return;
			}
				
			case EPhysXInstanceStopAction::ConvertToStorage:
				{
					const FPhysXInstanceID ConvertID = JobData.ID;

					// Moves the visual instance to a storage actor and destroys the PhysX body,
					// but the stable ID remains registered (re-bound to the storage component/index).
					if (ConvertInstanceToStaticStorage(ConvertID, /*bCreateStorageActorIfNeeded=*/true))
					{
						// After conversion, the PhysX body is gone.
						JobData.RigidDynamic = nullptr;
						InstanceData->bSimulating = false;
						break;
					}

					// Fallback if conversion failed:
					InstanceData->Body.Destroy();
					InstanceData->bSimulating = false;
					break;
				}

			default:
				break;
			}

			InstanceData->SleepTime = 0.0f;
			InstanceData->FallTime  = 0.0f;
		};

		// PASS 1: apply CCD toggles, stop actions, timers, and sleeping flags.
		for (FPhysXInstanceAsyncStepJob& JobData : Jobs)
		{
			FPhysXInstanceData* InstanceData = JobData.Data;
			if (!InstanceData)
			{
				continue;
			}

			// CCD toggles.
			if (JobData.bEnableCCD && JobData.RigidDynamic)
			{
				JobData.RigidDynamic->setRigidBodyFlag(PxRigidBodyFlag::eENABLE_CCD, true);
			}
			else if (JobData.bDisableCCD && JobData.RigidDynamic)
			{
				JobData.RigidDynamic->setRigidBodyFlag(PxRigidBodyFlag::eENABLE_CCD, false);
			}

			// Apply stop-action or store updated timers.
			if (JobData.bApplyStopAction && JobData.ActionToApply != EPhysXInstanceStopAction::None)
			{
				ApplyStopAction_GameThread(JobData);

				// After ConvertToStorage the instance may be unregistered -> pointer becomes invalid.
				InstanceData = JobData.Data;
				if (!InstanceData)
				{
					continue; // Converted & unregistered -> skip rest of this job in PASS 1
				}
			}
			else
			{
				InstanceData->SleepTime = JobData.NewSleepTime;
				InstanceData->FallTime  = JobData.NewFallTime;
			}


			if (JobData.bSleeping)
			{
				++LocalSleeping;
			}

			// Cache sleeping state for the next-frame fast path.
			InstanceData->bWasSleeping = JobData.bSleeping;
		}

		// PASS 2: batch transform updates for instances that still exist.
		for (FPhysXInstanceAsyncStepJob& JobData : Jobs)
		{
			FPhysXInstanceData* InstanceData = JobData.Data;
			if (!InstanceData)
			{
				continue;
			}

			UInstancedStaticMeshComponent* ISMComponent = JobData.ISMC;
			if (!ISMComponent || !ISMComponent->IsValidLowLevelFast())
			{
				continue;
			}

			// The instance may have been removed in PASS 1.
			if (InstanceData->InstanceIndex == INDEX_NONE)
			{
				continue;
			}

			const bool bWasSleeping = JobData.bWasSleepingInitial;
			const bool bIsSleeping  = JobData.bSleeping;

			// Transform is updated when the body is active or has just transitioned to sleeping.
			const bool bNeedTransformUpdate =
				(!bIsSleeping || !bWasSleeping);

			if (!bNeedTransformUpdate)
			{
				continue;
			}

			if (UPhysXInstancedStaticMeshComponent* PhysXISMC =
				Cast<UPhysXInstancedStaticMeshComponent>(ISMComponent))
			{
				FComponentTransformBatch& Batch = ComponentBatches.FindOrAdd(PhysXISMC);
				Batch.InstanceIndices.Add(InstanceData->InstanceIndex);
				Batch.WorldTransforms.Add(JobData.NewWorldTransform);
			}
			else
			{
				// Fallback: update a generic ISM component instance one by one.
				ISMComponent->UpdateInstanceTransform(
					InstanceData->InstanceIndex,
					JobData.NewWorldTransform,
					/*bWorldSpace=*/true,
					/*bMarkRenderStateDirty=*/false,
					/*bTeleport=*/false);

				DirtyComponents.Add(ISMComponent);
			}
		}

		// Apply batched updates for PhysX ISM components.
		for (auto& Pair : ComponentBatches)
		{
			UPhysXInstancedStaticMeshComponent* PhysXISMC = Pair.Key;
			FComponentTransformBatch&           Batch     = Pair.Value;

			if (!PhysXISMC || !PhysXISMC->IsValidLowLevelFast())
			{
				continue;
			}

			if (Batch.InstanceIndices.Num() == 0 ||
				Batch.InstanceIndices.Num() != Batch.WorldTransforms.Num())
			{
				continue;
			}

			PhysXISMC->UpdateInstancesFromPhysXBatch_MT(
				Batch.InstanceIndices,
				Batch.WorldTransforms,
				/*bTeleport=*/false);
		}

		// Mark render state dirty once per fallback component.
		for (UInstancedStaticMeshComponent* ISMC : DirtyComponents)
		{
			if (ISMC && ISMC->IsValidLowLevelFast())
			{
				ISMC->MarkRenderStateDirty();
			}
		}
	}

	// Final counters: total / sleeping / simulating.
	NumBodiesTotal      = LocalTotal;
	NumBodiesSleeping   = LocalSleeping;
	NumBodiesSimulating = NumBodiesTotal - NumBodiesSleeping;

	SET_DWORD_STAT(STAT_PhysXInstanced_BodiesTotal,      NumBodiesTotal);
	SET_DWORD_STAT(STAT_PhysXInstanced_BodiesSimulating, NumBodiesSimulating);
	SET_DWORD_STAT(STAT_PhysXInstanced_BodiesSleeping,   NumBodiesSleeping);

	const uint32 LifetimeClamped =
	(uint32)FMath::Min<uint64>(NumBodiesLifetimeCreated, (uint64)MAX_uint32);

	SET_DWORD_STAT(STAT_PhysXInstanced_BodiesLifetimeCreated, LifetimeClamped);

#endif // PHYSICS_INTERFACE_PHYSX
}

// ============================================================================
// High level physics control
// ============================================================================

bool UPhysXInstancedWorldSubsystem::SetInstancePhysicsEnabled(
	FPhysXInstanceID ID,
	bool bEnable,
	bool bDestroyBodyIfDisabling)
{
	FPhysXInstanceData* Data = Instances.Find(ID);
	if (!Data)
	{
		return false;
	}

	const bool bWasSimulating = Data->bSimulating;

#if !PHYSICS_INTERFACE_PHYSX
	// Without PhysX backend, simulation cannot be toggled.
	return false;
#else
	UInstancedStaticMeshComponent* ISMC = Data->InstancedComponent.Get();
	if (!ISMC || !ISMC->IsValidLowLevelFast())
	{
		return false;
	}

	physx::PxRigidActor*   Actor        = Data->Body.GetPxActor();
	physx::PxRigidDynamic* RigidDynamic = Actor ? Actor->is<physx::PxRigidDynamic>() : nullptr;

	if (bEnable)
	{
		// If there is no body yet, try to create one now.
		if (!RigidDynamic)
		{
			EPhysXInstanceShapeType ShapeType = EPhysXInstanceShapeType::Box;
			UStaticMesh* OverrideMesh         = nullptr;
			bool bUseGravity                  = true;

			if (AActor* Owner = ISMC->GetOwner())
			{
				if (const APhysXInstancedMeshActor* PhysXActor =
					Cast<APhysXInstancedMeshActor>(Owner))
				{
					ShapeType    = PhysXActor->InstanceShapeType;
					OverrideMesh = PhysXActor->OverrideCollisionMesh;
					bUseGravity  = PhysXActor->bInstancesUseGravity;
				}
			}

			if (!OverrideMesh)
			{
				OverrideMesh = ISMC->GetStaticMesh();
			}

			if (!GInstancedDefaultMaterial)
			{
				return false;
			}

			if (!Data->Body.CreateFromInstancedStaticMesh(
				ISMC,
				Data->InstanceIndex,
				/*bSimulate=*/true,
				GInstancedDefaultMaterial,
				ShapeType,
				OverrideMesh))
			{
				return false;
			}

			EnsureInstanceUserData(ID);
			EnqueueAddActorToScene(ID, ISMC);
			++NumBodiesLifetimeCreated;

			Actor        = Data->Body.GetPxActor();
			RigidDynamic = Actor ? Actor->is<physx::PxRigidDynamic>() : nullptr;
		}

		if (RigidDynamic)
		{
			// Switch from kinematic back to dynamic simulation.
			RigidDynamic->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, false);

			bool bUseGravity = true;
			if (AActor* Owner = ISMC->GetOwner())
			{
				if (const APhysXInstancedMeshActor* PhysXActor =
					Cast<APhysXInstancedMeshActor>(Owner))
				{
					bUseGravity = PhysXActor->bInstancesUseGravity;
				}
			}

			RigidDynamic->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, !bUseGravity);
			RigidDynamic->setActorFlag(PxActorFlag::eDISABLE_SIMULATION, false);

			Data->bSimulating = true;
		}
		else
		{
			Data->bSimulating = false;
		}
	}
	else
	{
		if (RigidDynamic)
		{
			if (bDestroyBodyIfDisabling)
			{
				ClearInstanceUserData(ID);
				// Destroy the PhysX body while keeping the visual instance.
				Data->Body.Destroy();
			}
			else
			{
				// Keep the body but disable simulation by switching to kinematic.
				RigidDynamic->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);
				RigidDynamic->setActorFlag(PxActorFlag::eDISABLE_SIMULATION, true);
			}
		}

		Data->bSimulating = false;
	}

	// Keep global sim-count in sync with the flag.
	if (bWasSimulating != Data->bSimulating)
	{
		if (Data->bSimulating)
		{
			++NumBodiesSimulating;
		}
		else if (NumBodiesSimulating > 0)
		{
			--NumBodiesSimulating;
		}
	}

	return true;
#endif // PHYSICS_INTERFACE_PHYSX
}

bool UPhysXInstancedWorldSubsystem::ConvertInstanceToStaticStorage(
	FPhysXInstanceID ID,
	bool bCreateStorageActorIfNeeded)
{
	FPhysXInstanceData* Data = Instances.Find(ID);
	if (!Data)
	{
		return false;
	}

	UInstancedStaticMeshComponent* ISMC = Data->InstancedComponent.Get();
	if (!ISMC || !ISMC->IsValidLowLevelFast())
	{
		return false;
	}

	const int32 InstanceIndex = Data->InstanceIndex;
	if (InstanceIndex == INDEX_NONE)
	{
		return false;
	}

	// World-space transform of the source instance.
	FTransform WorldTM;
	if (!ISMC->GetInstanceTransform(InstanceIndex, WorldTM, /*bWorldSpace=*/true))
	{
		return false;
	}

	APhysXInstancedMeshActor* SourceActor =
		Cast<APhysXInstancedMeshActor>(ISMC->GetOwner());
	if (!SourceActor)
	{
		return false;
	}

	UStaticMesh* StaticMesh = SourceActor->InstanceStaticMesh;
	if (!StaticMesh)
	{
		StaticMesh = ISMC->GetStaticMesh();
	}
	if (!StaticMesh)
	{
		return false;
	}

	// ---------------------------------------------------------------------
	// 1) Find or create a storage actor with matching mesh/material settings
	// ---------------------------------------------------------------------

	APhysXInstancedMeshActor* StorageActor = nullptr;

	auto DoMaterialsMatch = [](const APhysXInstancedMeshActor* A, const APhysXInstancedMeshActor* B)
	{
		if (!A || !B)
		{
			return false;
		}

		if (A->bOverrideInstanceMaterials != B->bOverrideInstanceMaterials)
		{
			return false;
		}

		if (!A->bOverrideInstanceMaterials)
		{
			return true;
		}

		const int32 NumA = A->InstanceOverrideMaterials.Num();
		const int32 NumB = B->InstanceOverrideMaterials.Num();
		const int32 Num  = FMath::Min(NumA, NumB);

		if (NumA != NumB)
		{
			return false;
		}
		for (int32 Index = 0; Index < NumA; ++Index)
		{
			if (A->InstanceOverrideMaterials[Index] != B->InstanceOverrideMaterials[Index])
			{
				return false;
			}
		}

		return true;
	};

	for (const TPair<FPhysXActorID, FPhysXActorData>& Pair : Actors)
	{
		APhysXInstancedMeshActor* Actor = Pair.Value.Actor.Get();
		if (!Actor || !Actor->IsValidLowLevelFast())
		{
			continue;
		}

		if (!Actor->bStorageOnly)
		{
			continue;
		}

		if (Actor->InstanceStaticMesh != StaticMesh)
		{
			continue;
		}

		if (!DoMaterialsMatch(Actor, SourceActor))
		{
			continue;
		}

		StorageActor = Actor;
		break;
	}

	// Create a new storage actor when allowed and none exists.
	if (!StorageActor)
	{
		if (!bCreateStorageActorIfNeeded)
		{
			return false;
		}

		UWorld* World = GetWorld();
		if (!World)
		{
			return false;
		}

		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		StorageActor = World->SpawnActor<APhysXInstancedMeshActor>(
			APhysXInstancedMeshActor::StaticClass(),
			WorldTM,
			SpawnParams);

		if (!StorageActor)
		{
			return false;
		}

		// Register the storage actor in the subsystem.
		const FPhysXActorID NewActorID = RegisterInstancedMeshActor(StorageActor);
		StorageActor->PhysXActorID = NewActorID;

		// Storage mode settings.
		StorageActor->bStorageOnly       = true;
		StorageActor->bIsStorageActor    = true;
		StorageActor->bSimulateInstances = false;
		StorageActor->bDisableISMPhysics = false;

		// Copy mesh/material settings from the source actor.
		StorageActor->InstanceStaticMesh         = StaticMesh;
		StorageActor->bOverrideInstanceMaterials = SourceActor->bOverrideInstanceMaterials;
		StorageActor->InstanceOverrideMaterials  = SourceActor->InstanceOverrideMaterials;

		// Copy storage collision/navigation settings from the source actor.
		StorageActor->bStorageInstancesAffectNavigation = SourceActor->bStorageInstancesAffectNavigation;
		StorageActor->StorageCollisionProfile           = SourceActor->StorageCollisionProfile;
		StorageActor->StorageCollisionEnabled           = SourceActor->StorageCollisionEnabled;

		if (StorageActor->InstancedMesh)
		{
			UPhysXInstancedStaticMeshComponent* StorageISMC = StorageActor->InstancedMesh;

			StorageISMC->SetStaticMesh(StaticMesh);
			StorageActor->ApplyInstanceMaterials();

			StorageISMC->SetSimulatePhysics(false);

			const FName StorageProfile =
				(StorageActor->StorageCollisionProfile.Name != NAME_None)
			? StorageActor->StorageCollisionProfile.Name
			: StorageActor->InstancesCollisionProfile.Name;
			if (StorageProfile != NAME_None)
				{
				StorageISMC->SetCollisionProfileName(StorageProfile);
				}
			
			StorageISMC->SetCollisionEnabled(StorageActor->StorageCollisionEnabled);

			StorageISMC->SetInstancesAffectNavigation(
				StorageActor->bStorageInstancesAffectNavigation);
		}
	}

	if (!StorageActor || !StorageActor->InstancedMesh)
	{
		return false;
	}

	UPhysXInstancedStaticMeshComponent* StorageISMC = StorageActor->InstancedMesh;

	// Ensure storage actor navigation settings are applied.
	StorageISMC->SetInstancesAffectNavigation(StorageActor->bStorageInstancesAffectNavigation);

	// ---------------------------------------------------------------------
	// 2) Add a new ISM instance into the storage actor
	// ---------------------------------------------------------------------

	const int32 StorageIndex =
		StorageISMC->AddInstanceWorldSpace(WorldTM);

	if (StorageIndex == INDEX_NONE)
	{
		return false;
	}

	// ---------------------------------------------------------------------
	// 3) Remove the source visual instance and destroy its PhysX body
	//    (ID stays registered; we just rebind it to the storage component/index).
	// ---------------------------------------------------------------------

	const int32 RemovedIndex = Data->InstanceIndex;

	// If the body is still queued for scene insertion, invalidate it now.
	// Otherwise ProcessPendingAddActors() may keep touching this ID after conversion.
#if PHYSICS_INTERFACE_PHYSX
	InvalidatePendingAddEntries(ID);
#endif

	// Remove old slot mapping (source component/index) BEFORE we mutate anything.
	RemoveSlotMapping(ID);

	// Remove from the dynamic component (this compacts indices).
	if (!ISMC->RemoveInstance(RemovedIndex))
	{
		// Roll back storage add.
		StorageISMC->RemoveInstance(StorageIndex);

		// Restore old slot mapping (Data still points to the old slot at this moment).
		AddSlotMapping(ID);
		RebuildSlotMappingForComponent(ISMC);

		return false;
	}

	// Keep actor bookkeeping in sync AFTER we know the remove succeeded.
	SourceActor->RegisteredInstanceIDs.Remove(ID);
	StorageActor->RegisteredInstanceIDs.Add(ID);

	// Fix indices for other IDs still pointing to the source component (RemoveAt shift).
	FixInstanceIndicesAfterRemoval(ISMC, RemovedIndex);
	ISMC->MarkRenderStateDirty();

#if PHYSICS_INTERFACE_PHYSX
	// Body is gone in storage mode.
	ClearInstanceUserData(ID);
	Data->Body.Destroy();
#endif

	// Rebind the stable ID to the storage slot.
	Data->bSimulating        = false;
	Data->InstancedComponent = StorageISMC;
	Data->InstanceIndex      = StorageIndex;

	// Add new slot mapping AFTER Data points to the storage slot.
	AddSlotMapping(ID);

	// Rebuild mappings for both components to guarantee correctness.
	RebuildSlotMappingForComponent(ISMC);
	RebuildSlotMappingForComponent(StorageISMC);

	// Storage component render state.
	StorageISMC->MarkRenderStateDirty();

	return true;
}

bool UPhysXInstancedWorldSubsystem::ConvertStorageInstanceToDynamic(
	FPhysXInstanceID ID,
	bool bCreateDynamicActorIfNeeded)
{
	FPhysXInstanceData* Data = Instances.Find(ID);
	if (!Data)
	{
		return false;
	}

	UInstancedStaticMeshComponent* StorageISMC = Data->InstancedComponent.Get();
	if (!StorageISMC || !StorageISMC->IsValidLowLevelFast())
	{
		return false;
	}

	const int32 StorageIndex = Data->InstanceIndex;
	if (StorageIndex == INDEX_NONE)
	{
		return false;
	}

	APhysXInstancedMeshActor* StorageActor = Cast<APhysXInstancedMeshActor>(StorageISMC->GetOwner());
	if (!StorageActor)
	{
		return false;
	}

	// Must be a storage owner.
	if (!(StorageActor->bIsStorageActor || StorageActor->bStorageOnly))
	{
		return false;
	}

	// Get world transform from the storage instance.
	FTransform WorldTM;
	if (!StorageISMC->GetInstanceTransform(StorageIndex, WorldTM, /*bWorldSpace=*/true))
	{
		return false;
	}

	UStaticMesh* StaticMesh = StorageActor->InstanceStaticMesh;
	if (!StaticMesh)
	{
		StaticMesh = StorageISMC->GetStaticMesh();
	}
	if (!StaticMesh)
	{
		return false;
	}

	// ---------------------------------------------------------------------
	// 1) Find or create a NON-storage actor with matching mesh/materials.
	// ---------------------------------------------------------------------

	auto DoMaterialsMatch = [](const APhysXInstancedMeshActor* A, const APhysXInstancedMeshActor* B)
	{
		if (!A || !B)
		{
			return false;
		}

		if (A->bOverrideInstanceMaterials != B->bOverrideInstanceMaterials)
		{
			return false;
		}

		if (!A->bOverrideInstanceMaterials)
		{
			return true;
		}

		if (A->InstanceOverrideMaterials.Num() != B->InstanceOverrideMaterials.Num())
		{
			return false;
		}

		for (int32 i = 0; i < A->InstanceOverrideMaterials.Num(); ++i)
		{
			if (A->InstanceOverrideMaterials[i] != B->InstanceOverrideMaterials[i])
			{
				return false;
			}
		}

		return true;
	};

	APhysXInstancedMeshActor* TargetActor = nullptr;

	for (const TPair<FPhysXActorID, FPhysXActorData>& Pair : Actors)
	{
		APhysXInstancedMeshActor* Actor = Pair.Value.Actor.Get();
		if (!Actor || !Actor->IsValidLowLevelFast())
		{
			continue;
		}

		// Skip storage actors.
		if (Actor->bIsStorageActor || Actor->bStorageOnly)
		{
			continue;
		}

		if (Actor->InstanceStaticMesh != StaticMesh)
		{
			continue;
		}

		if (!DoMaterialsMatch(Actor, StorageActor))
		{
			continue;
		}

		if (!Actor->InstancedMesh)
		{
			continue;
		}

		TargetActor = Actor;
		break;
	}

	if (!TargetActor)
	{
		if (!bCreateDynamicActorIfNeeded)
		{
			return false;
		}

		UWorld* World = GetWorld();
		if (!World)
		{
			return false;
		}

		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		TargetActor = World->SpawnActor<APhysXInstancedMeshActor>(
			APhysXInstancedMeshActor::StaticClass(),
			WorldTM,
			SpawnParams);

		if (!TargetActor)
		{
			return false;
		}

		// Register in subsystem.
		const FPhysXActorID NewActorID = RegisterInstancedMeshActor(TargetActor);
		TargetActor->PhysXActorID = NewActorID;

		// Dynamic mode defaults.
		TargetActor->bStorageOnly       = false;
		TargetActor->bIsStorageActor    = false;
		TargetActor->bSimulateInstances = true;

		// IMPORTANT: dynamic ISM collision should generally be disabled to avoid double collision.
		TargetActor->bDisableISMPhysics = true;

		// Copy mesh/material settings from the storage actor.
		TargetActor->InstanceStaticMesh         = StaticMesh;
		TargetActor->bOverrideInstanceMaterials = StorageActor->bOverrideInstanceMaterials;
		TargetActor->InstanceOverrideMaterials  = StorageActor->InstanceOverrideMaterials;

		TargetActor->ApplyInstanceMaterials();
	}

	if (!TargetActor || !TargetActor->InstancedMesh)
	{
		return false;
	}

	// Ensure the actor is registered (in case it existed but wasn't registered yet).
	if (TargetActor->PhysXActorID.GetUniqueID() == 0u)
	{
		const FPhysXActorID NewActorID = RegisterInstancedMeshActor(TargetActor);
		TargetActor->PhysXActorID = NewActorID;
	}

	UInstancedStaticMeshComponent* TargetISMC = TargetActor->InstancedMesh;
	if (!TargetISMC)
	{
		return false;
	}

	// ---------------------------------------------------------------------
	// 2) Add a visual instance to the target actor (dynamic container).
	// ---------------------------------------------------------------------

	const int32 TargetIndex = TargetISMC->AddInstanceWorldSpace(WorldTM);
	if (TargetIndex == INDEX_NONE)
	{
		return false;
	}

	// ---------------------------------------------------------------------
	// 3) Create a PhysX body for the NEW target slot first (so we can rollback safely).
	// ---------------------------------------------------------------------

#if PHYSICS_INTERFACE_PHYSX
	if (!GInstancedDefaultMaterial)
	{
		TargetISMC->RemoveInstance(TargetIndex);
		return false;
	}

	EPhysXInstanceShapeType ShapeType = EPhysXInstanceShapeType::Box;
	UStaticMesh* OverrideMesh = nullptr;

	if (const APhysXInstancedMeshActor* PhysXActor = Cast<APhysXInstancedMeshActor>(TargetISMC->GetOwner()))
	{
		ShapeType    = PhysXActor->InstanceShapeType;
		OverrideMesh = PhysXActor->OverrideCollisionMesh;
	}

	FPhysXInstanceBody NewBody;
	if (!NewBody.CreateFromInstancedStaticMesh(
		TargetISMC,
		TargetIndex,
		/*bSimulate=*/true,
		GInstancedDefaultMaterial,
		ShapeType,
		OverrideMesh))
	{
		TargetISMC->RemoveInstance(TargetIndex);
		return false;
	}
#endif // PHYSICS_INTERFACE_PHYSX

	// ---------------------------------------------------------------------
	// 4) Commit: remove storage instance and rebind the stable ID.
	// ---------------------------------------------------------------------

	InvalidatePendingAddEntries(ID);

	// Remove the old slot mapping BEFORE changing Data.
	RemoveSlotMapping(ID);

	// Remove from storage (this compacts indices).
	if (!StorageISMC->RemoveInstance(StorageIndex))
	{
		// Rollback: remove the new target instance.
		TargetISMC->RemoveInstance(TargetIndex);

		// Restore old mapping (Data still points to old storage slot).
		AddSlotMapping(ID);

#if PHYSICS_INTERFACE_PHYSX
		// NewBody owns a Px pointer, but its destructor does not destroy; do it explicitly.
		NewBody.Destroy();
#endif
		return false;
	}

	// Actor bookkeeping.
	StorageActor->RegisteredInstanceIDs.Remove(ID);
	TargetActor->RegisteredInstanceIDs.Add(ID);

	// Fix indices for other storage-bound IDs affected by compaction.
	FixInstanceIndicesAfterRemoval(StorageISMC, StorageIndex);

	// Rebind stable ID to the new dynamic slot.
	Data->InstancedComponent = TargetISMC;
	Data->InstanceIndex      = TargetIndex;
	Data->bSimulating        = true;
	Data->SleepTime          = 0.0f;
	Data->FallTime           = 0.0f;
	Data->bWasSleeping       = false;

#if PHYSICS_INTERFACE_PHYSX
	// Replace body (storage instances should have no body).
	ClearInstanceUserData(ID);
	Data->Body.Destroy();
	Data->Body = NewBody;

	EnsureInstanceUserData(ID);
	EnqueueAddActorToScene(ID, TargetISMC);
	++NumBodiesLifetimeCreated;
#endif

	// Add new slot mapping AFTER Data points to the target slot.
	AddSlotMapping(ID);

	RebuildSlotMappingForComponent(StorageISMC);
	RebuildSlotMappingForComponent(TargetISMC);

	StorageISMC->MarkRenderStateDirty();
	TargetISMC->MarkRenderStateDirty();

	// Optional: auto-destroy empty storage actors.
	if (StorageActor->RegisteredInstanceIDs.Num() == 0 && StorageActor->InstancedMesh && StorageActor->InstancedMesh->GetInstanceCount() == 0)
	{
		if (StorageActor->PhysXActorID.IsValid())
		{
			UnregisterInstancedMeshActor(StorageActor->PhysXActorID);
		}
		StorageActor->Destroy();
	}

	return true;
}

bool UPhysXInstancedWorldSubsystem::IsInstancePhysicsEnabled(FPhysXInstanceID ID) const
{
#if !PHYSICS_INTERFACE_PHYSX
	return false;
#else
	const FPhysXInstanceData* Data = Instances.Find(ID);
	if (!Data)
	{
		return false;
	}

	physx::PxRigidActor* Actor = Data->Body.GetPxActor();
	physx::PxRigidDynamic* RigidDynamic = Actor ? Actor->is<physx::PxRigidDynamic>() : nullptr;
	if (!RigidDynamic)
	{
		return false;
	}

	const PxActorFlags Flags = RigidDynamic->getActorFlags();
	const PxRigidBodyFlags BodyFlags = RigidDynamic->getRigidBodyFlags();

	const bool bSimDisabled = Flags.isSet(PxActorFlag::eDISABLE_SIMULATION);
	const bool bKinematic   = BodyFlags.isSet(PxRigidBodyFlag::eKINEMATIC);

	return !bSimDisabled && !bKinematic;
#endif
}

bool UPhysXInstancedWorldSubsystem::AddImpulseToInstance(
	FPhysXInstanceID ID,
	FVector WorldImpulse,
	bool bVelChange)
{
	// Backward-compatible wrapper: behave like AddRadialImpulse defaults.
	return AddImpulseToInstanceAdvanced(
		ID,
		WorldImpulse,
		bVelChange,
		/*bIncludeStorage=*/true,
		/*bConvertStorageToDynamic=*/true);
}

bool UPhysXInstancedWorldSubsystem::AddImpulseToInstanceAdvanced(
	FPhysXInstanceID ID,
	FVector WorldImpulse,
	bool bVelChange,
	bool bIncludeStorage,
	bool bConvertStorageToDynamic)
{
	if (!Instances.Contains(ID))
	{
		return false;
	}

	FPhysXInstanceTask Task;
	Task.Type = EPhysXInstanceTaskType::AddImpulse;
	Task.ID = ID;
	Task.Vector = WorldImpulse;
	Task.bModeFlag = bVelChange;
	Task.bIncludeStorage = bIncludeStorage;
	Task.bConvertStorageToDynamic = bConvertStorageToDynamic;

	EnqueueInstanceTask(Task);
	return true; // queued
}


bool UPhysXInstancedWorldSubsystem::AddRadialImpulse(
	const FVector& OriginWorld,
	float Radius,
	float Strength,
	bool bVelChange,
	bool bIncludeStorage,
	bool bConvertStorageToDynamic,
	bool bLinearFalloff,
	EPhysXInstancedQueryDebugMode DebugMode,
	float DebugDrawDuration)
{
	if (Radius <= 0.0f || FMath::IsNearlyZero(Strength))
	{
		return false;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	const float RadiusSq = Radius * Radius;

	struct FRadialImpulseTarget
	{
		FPhysXInstanceID ID;
		FVector          PositionUU = FVector::ZeroVector;
		FVector          ImpulseUU  = FVector::ZeroVector;
	};

	TArray<FRadialImpulseTarget> Targets;
	Targets.Reserve(128);

	// 1) Collect targets and compute per-instance impulse (distance-based).
	for (const TPair<FPhysXInstanceID, FPhysXInstanceData>& Pair : Instances)
	{
		const FPhysXInstanceID& ID = Pair.Key;
		const FPhysXInstanceData& Data = Pair.Value;

		if (!ID.IsValid())
		{
			continue;
		}

		UInstancedStaticMeshComponent* ISMC = Data.InstancedComponent.Get();
		if (!ISMC || !ISMC->IsValidLowLevelFast() || Data.InstanceIndex == INDEX_NONE)
		{
			continue;
		}

		const bool bIsStorageOwner = IsOwnerStorageActor(ISMC);

		if (bIsStorageOwner)
		{
			if (!bIncludeStorage)
			{
				continue;
			}

			// Cannot apply impulses to storage unless we are allowed to convert it to dynamic.
			if (!bConvertStorageToDynamic)
			{
				continue;
			}
		}

		// Validate index range to avoid stale IDs returning nonsense.
		const int32 NumInstances = ISMC->GetInstanceCount();
		if (Data.InstanceIndex < 0 || Data.InstanceIndex >= NumInstances)
		{
			continue;
		}

		FVector InstanceLoc = FVector::ZeroVector;
		if (!GetInstanceWorldLocation_Safe(Data, InstanceLoc))
		{
			continue;
		}

		const float DistSq = FVector::DistSquared(OriginWorld, InstanceLoc);
		if (DistSq > RadiusSq)
		{
			continue;
		}

		FVector Dir = (InstanceLoc - OriginWorld);
		const float Dist = Dir.Size();
		if (Dist <= KINDA_SMALL_NUMBER)
		{
			continue;
		}

		float Falloff = 1.0f;
		if (bLinearFalloff)
		{
			Falloff = 1.0f - (Dist / Radius);
			Falloff = FMath::Clamp(Falloff, 0.0f, 1.0f);
		}

		const FVector ImpulseUU = Dir.GetSafeNormal() * (Strength * Falloff);

		FRadialImpulseTarget& T = Targets.AddDefaulted_GetRef();
		T.ID         = ID;
		T.PositionUU = InstanceLoc;
		T.ImpulseUU  = ImpulseUU;
	}

	if (Targets.Num() == 0)
	{
		return false;
	}

#if !PHYSICS_INTERFACE_PHYSX
	return false;
#else

	bool bAppliedAny = false; // now means "queued at least one impulse task"

	// 2) Queue per-instance impulse tasks (conversion/body creation handled inside tasks).
	for (const FRadialImpulseTarget& T : Targets)
	{
		const bool bQueued = AddImpulseToInstanceAdvanced(
			T.ID,
			T.ImpulseUU,
			bVelChange,
			bIncludeStorage,
			bConvertStorageToDynamic);

		bAppliedAny |= bQueued;
	}

#if ENABLE_DRAW_DEBUG
	if (bAppliedAny && DebugMode != EPhysXInstancedQueryDebugMode::None)
	{
		DrawSphereSafe(World, OriginWorld, Radius, FColor::Green, DebugDrawDuration, 1.5f);

		if (DebugMode == EPhysXInstancedQueryDebugMode::Detailed)
		{
			const int32 MaxArrows = 64;
			const int32 NumToDraw = FMath::Min(Targets.Num(), MaxArrows);

			for (int32 i = 0; i < NumToDraw; ++i)
			{
				const FRadialImpulseTarget& D = Targets[i];
				DrawArrowSafe(World, OriginWorld, D.PositionUU, FColor::Cyan, DebugDrawDuration, 1.5f);
				DrawTextSafe(World, D.PositionUU + FVector(0, 0, 10.0f),
					FString::Printf(TEXT("ID=%u"), D.ID.GetUniqueID()),
					FColor::White, DebugDrawDuration);
			}

			if (Targets.Num() > MaxArrows)
			{
				DrawTextSafe(World,
					OriginWorld + FVector(0, 0, 20.0f),
					FString::Printf(TEXT("RadialImpulse: %d hits (showing %d)"), Targets.Num(), MaxArrows),
					FColor::White,
					DebugDrawDuration);
			}
		}
	}
	else if (!bAppliedAny && DebugMode != EPhysXInstancedQueryDebugMode::None)
	{
		DrawSphereSafe(World, OriginWorld, Radius, FColor::Red, DebugDrawDuration, 1.5f);
	}
#endif // ENABLE_DRAW_DEBUG

	return bAppliedAny;
#endif // PHYSICS_INTERFACE_PHYSX
}

bool UPhysXInstancedWorldSubsystem::AddForceToInstance(
	FPhysXInstanceID ID,
	FVector WorldForce,
	bool bAccelChange)
{
	// Backward-compatible wrapper: behave like AddRadialImpulse defaults.
	return AddForceToInstanceAdvanced(
		ID,
		WorldForce,
		bAccelChange,
		/*bIncludeStorage=*/true,
		/*bConvertStorageToDynamic=*/true);
}

bool UPhysXInstancedWorldSubsystem::AddForceToInstanceAdvanced(
	FPhysXInstanceID ID,
	FVector WorldForce,
	bool bAccelChange,
	bool bIncludeStorage,
	bool bConvertStorageToDynamic)
{
	if (!Instances.Contains(ID))
	{
		return false;
	}

	FPhysXInstanceTask Task;
	Task.Type = EPhysXInstanceTaskType::AddForce;
	Task.ID = ID;
	Task.Vector = WorldForce;
	Task.bModeFlag = bAccelChange;
	Task.bIncludeStorage = bIncludeStorage;
	Task.bConvertStorageToDynamic = bConvertStorageToDynamic;

	EnqueueInstanceTask(Task);
	return true; // queued
}


bool UPhysXInstancedWorldSubsystem::PutInstanceToSleep(FPhysXInstanceID ID)
{
	return PutInstanceToSleepAdvanced(
		ID,
		/*bIncludeStorage=*/true,
		/*bConvertStorageToDynamic=*/true);
}

bool UPhysXInstancedWorldSubsystem::WakeInstanceUp(FPhysXInstanceID ID)
{
	return WakeInstanceUpAdvanced(
		ID,
		/*bIncludeStorage=*/true,
		/*bConvertStorageToDynamic=*/true);
}

bool UPhysXInstancedWorldSubsystem::PutInstanceToSleepAdvanced(
	FPhysXInstanceID ID,
	bool bIncludeStorage,
	bool bConvertStorageToDynamic)
{
	if (!Instances.Contains(ID))
	{
		return false;
	}

	FPhysXInstanceTask Task;
	Task.Type = EPhysXInstanceTaskType::PutToSleep;
	Task.ID = ID;
	Task.bIncludeStorage = bIncludeStorage;
	Task.bConvertStorageToDynamic = bConvertStorageToDynamic;

	EnqueueInstanceTask(Task);
	return true;
}

bool UPhysXInstancedWorldSubsystem::WakeInstanceUpAdvanced(
	FPhysXInstanceID ID,
	bool bIncludeStorage,
	bool bConvertStorageToDynamic)
{
	if (!Instances.Contains(ID))
	{
		return false;
	}

	FPhysXInstanceTask Task;
	Task.Type = EPhysXInstanceTaskType::WakeUp;
	Task.ID = ID;
	Task.bIncludeStorage = bIncludeStorage;
	Task.bConvertStorageToDynamic = bConvertStorageToDynamic;

	EnqueueInstanceTask(Task);
	return true;
}


// ============================================================================
// Per-instance physics properties
// ============================================================================

bool UPhysXInstancedWorldSubsystem::SetInstanceGravityEnabled(FPhysXInstanceID ID, bool bEnable)
{
	FPhysXInstanceData* Data = Instances.Find(ID);
	if (!Data)
	{
		return false;
	}

#if !PHYSICS_INTERFACE_PHYSX
	return false;
#else
	physx::PxRigidActor* Actor = Data->Body.GetPxActor();
	physx::PxRigidDynamic* RigidDynamic =
		Actor ? Actor->is<physx::PxRigidDynamic>() : nullptr;

	if (!RigidDynamic)
	{
		return false;
	}

	// DISABLE_GRAVITY disables gravity when true, so it is inverted relative to bEnable.
	RigidDynamic->setActorFlag(physx::PxActorFlag::eDISABLE_GRAVITY, !bEnable);
	return true;
#endif
}

bool UPhysXInstancedWorldSubsystem::IsInstanceGravityEnabled(FPhysXInstanceID ID) const
{
#if !PHYSICS_INTERFACE_PHYSX
	return false;
#else
	const FPhysXInstanceData* Data = Instances.Find(ID);
	if (!Data)
	{
		return false;
	}

	physx::PxRigidActor* Actor = Data->Body.GetPxActor();
	physx::PxRigidDynamic* RigidDynamic =
		Actor ? Actor->is<physx::PxRigidDynamic>() : nullptr;

	if (!RigidDynamic)
	{
		return false;
	}

	const physx::PxActorFlags Flags = RigidDynamic->getActorFlags();
	return !Flags.isSet(physx::PxActorFlag::eDISABLE_GRAVITY);
#endif
}

bool UPhysXInstancedWorldSubsystem::SetInstanceLinearVelocity(
	FPhysXInstanceID ID,
	FVector NewVelocity,
	bool bAutoWake)
{
	FPhysXInstanceData* Data = Instances.Find(ID);
	if (!Data)
	{
		return false;
	}

#if !PHYSICS_INTERFACE_PHYSX
	return false;
#else
	physx::PxRigidActor* Actor = Data->Body.GetPxActor();
	physx::PxRigidDynamic* RigidDynamic =
		Actor ? Actor->is<physx::PxRigidDynamic>() : nullptr;

	if (!RigidDynamic)
	{
		return false;
	}

	const physx::PxVec3 PxVel = U2PVector(NewVelocity);
	RigidDynamic->setLinearVelocity(PxVel, bAutoWake);
	return true;
#endif
}

bool UPhysXInstancedWorldSubsystem::GetInstanceLinearVelocity(
	FPhysXInstanceID ID,
	FVector& OutVelocity) const
{
	OutVelocity = FVector::ZeroVector;

#if !PHYSICS_INTERFACE_PHYSX
	return false;
#else
	const FPhysXInstanceData* Data = Instances.Find(ID);
	if (!Data)
	{
		return false;
	}

	physx::PxRigidActor* Actor = Data->Body.GetPxActor();
	physx::PxRigidDynamic* RigidDynamic =
		Actor ? Actor->is<physx::PxRigidDynamic>() : nullptr;

	if (!RigidDynamic)
	{
		return false;
	}

	const physx::PxVec3 PxVel = RigidDynamic->getLinearVelocity();
	OutVelocity = P2UVector(PxVel);
	return true;
#endif
}

bool UPhysXInstancedWorldSubsystem::SetInstanceAngularVelocityInRadians(
	FPhysXInstanceID ID,
	FVector NewAngVelRad,
	bool bAutoWake)
{
	FPhysXInstanceData* Data = Instances.Find(ID);
	if (!Data)
	{
		return false;
	}

#if !PHYSICS_INTERFACE_PHYSX
	return false;
#else
	physx::PxRigidActor* Actor = Data->Body.GetPxActor();
	physx::PxRigidDynamic* RigidDynamic =
		Actor ? Actor->is<physx::PxRigidDynamic>() : nullptr;

	if (!RigidDynamic)
	{
		return false;
	}

	const physx::PxVec3 PxOmega = U2PVector(NewAngVelRad);
	RigidDynamic->setAngularVelocity(PxOmega, bAutoWake);
	return true;
#endif
}

bool UPhysXInstancedWorldSubsystem::GetInstanceAngularVelocityInRadians(
	FPhysXInstanceID ID,
	FVector& OutAngVelRad) const
{
	OutAngVelRad = FVector::ZeroVector;

#if !PHYSICS_INTERFACE_PHYSX
	return false;
#else
	const FPhysXInstanceData* Data = Instances.Find(ID);
	if (!Data)
	{
		return false;
	}

	physx::PxRigidActor* Actor = Data->Body.GetPxActor();
	physx::PxRigidDynamic* RigidDynamic =
		Actor ? Actor->is<physx::PxRigidDynamic>() : nullptr;

	if (!RigidDynamic)
	{
		return false;
	}

	const physx::PxVec3 PxOmega = RigidDynamic->getAngularVelocity();
	OutAngVelRad = P2UVector(PxOmega);
	return true;
#endif
}

// ============================================================================
// Query helpers / actor registry
// ============================================================================

bool UPhysXInstancedWorldSubsystem::IsInstanceValid(FPhysXInstanceID ID) const
{
	const FPhysXInstanceData* Data = Instances.Find(ID);
	if (!Data)
	{
		return false;
	}

	UInstancedStaticMeshComponent* ISMC = Data->InstancedComponent.Get();
	if (!ISMC || !ISMC->IsValidLowLevelFast())
	{
		return false;
	}

	if (Data->InstanceIndex == INDEX_NONE)
	{
		return false;
	}

	const int32 NumInstances = ISMC->GetInstanceCount();
	return (Data->InstanceIndex >= 0 && Data->InstanceIndex < NumInstances);
}

bool UPhysXInstancedWorldSubsystem::GetInstanceInfo(
	FPhysXInstanceID ID,
	UInstancedStaticMeshComponent*& OutComponent,
	int32& OutInstanceIndex) const
{
	OutComponent     = nullptr;
	OutInstanceIndex = INDEX_NONE;

	const FPhysXInstanceData* Data = Instances.Find(ID);
	if (!Data)
	{
		return false;
	}

	UInstancedStaticMeshComponent* ISMC = Data->InstancedComponent.Get();
	if (!ISMC || !ISMC->IsValidLowLevelFast() || Data->InstanceIndex == INDEX_NONE)
	{
		return false;
	}

	const int32 NumInstances = ISMC->GetInstanceCount();
	if (Data->InstanceIndex < 0 || Data->InstanceIndex >= NumInstances)
	{
		return false;
	}
	
	OutComponent     = ISMC;
	OutInstanceIndex = Data->InstanceIndex;
	return true;
}

TArray<FPhysXInstanceID> UPhysXInstancedWorldSubsystem::GetAllInstanceIDs() const
{
	TArray<FPhysXInstanceID> Result;
	Result.Reserve(Instances.Num());

	for (const TPair<FPhysXInstanceID, FPhysXInstanceData>& Pair : Instances)
	{
		Result.Add(Pair.Key);
	}

	return Result;
}

FPhysXInstanceID UPhysXInstancedWorldSubsystem::FindNearestInstance(
	FVector WorldLocation,
	UInstancedStaticMeshComponent* OptionalFilterComponent) const
{
	// Backward-compatible wrapper: ignore nothing, do not include storage.
	return FindNearestInstanceAdvanced(
		WorldLocation,
		OptionalFilterComponent,
		FPhysXInstanceID(),
		INDEX_NONE,
		/*bIncludeStorage=*/false);
}

FPhysXInstanceID UPhysXInstancedWorldSubsystem::FindNearestInstanceAdvanced(
	FVector WorldLocation,
	UInstancedStaticMeshComponent* OptionalFilterComponent,
	FPhysXInstanceID IgnoreInstanceID,
	int32 IgnoreInstanceIndex,
	bool bIncludeStorage) const
{
	FPhysXInstanceID BestID;
	float BestDistSq = TNumericLimits<float>::Max();

	for (const TPair<FPhysXInstanceID, FPhysXInstanceData>& Pair : Instances)
	{
		const FPhysXInstanceID&   ID   = Pair.Key;
		const FPhysXInstanceData& Data = Pair.Value;

		if (!ID.IsValid())
		{
			continue;
		}

		// Ignore self by stable ID (recommended; indices are not stable after removals).
		if (IgnoreInstanceID.IsValid() && ID == IgnoreInstanceID)
		{
			continue;
		}

		UInstancedStaticMeshComponent* ISMC = Data.InstancedComponent.Get();
		if (!ISMC || !ISMC->IsValidLowLevelFast())
		{
			continue;
		}

		if (OptionalFilterComponent && ISMC != OptionalFilterComponent)
		{
			continue;
		}

		// Ignore self by component-local index (only meaningful when the component is specified).
		if (OptionalFilterComponent && IgnoreInstanceIndex != INDEX_NONE && Data.InstanceIndex == IgnoreInstanceIndex)
		{
			continue;
		}

		if (Data.InstanceIndex == INDEX_NONE)
		{
			continue;
		}
		
		// Validate InstanceIndex to avoid returning IDs bound to non-existent visual instances.
		const int32 NumInstances = ISMC->GetInstanceCount();
		if (Data.InstanceIndex < 0 || Data.InstanceIndex >= NumInstances)
		{
			continue;
		}
		
		// Actor-level storage flags (dedicated storage actors).
		bool bOwnerIsStorageActor = false;
		if (AActor* Owner = ISMC->GetOwner())
		{
			if (const APhysXInstancedMeshActor* PhysXActor = Cast<APhysXInstancedMeshActor>(Owner))
			{
				bOwnerIsStorageActor = (PhysXActor->bIsStorageActor || PhysXActor->bStorageOnly);
			}
		}

#if PHYSICS_INTERFACE_PHYSX
		physx::PxRigidActor* RigidActor = Data.Body.GetPxActor();
#else
		void* RigidActor = nullptr;
#endif

		// If storage is excluded, require a real PhysX actor that is already in a scene.
		if (!bIncludeStorage)
		{
			if (bOwnerIsStorageActor)
			{
				continue;
			}

#if PHYSICS_INTERFACE_PHYSX
			if (!RigidActor)
			{
				continue;
			}
			if (!RigidActor->getScene())
			{
				// Body exists but is not inserted yet (pending add) -> not a valid "physics nearest".
				continue;
			}
#endif
		}

		FVector InstanceLocation = FVector::ZeroVector;
		bool bHasValidLocation = false;

#if PHYSICS_INTERFACE_PHYSX
		if (RigidActor)
		{
			const physx::PxTransform PxPose = RigidActor->getGlobalPose();
			InstanceLocation = P2UVector(PxPose.p);
			bHasValidLocation = true;
		}
		else
#endif
		{
			// Fallback path for storage/no-body instances.
			const int32 Num = ISMC->GetInstanceCount();
			if (Data.InstanceIndex < 0 || Data.InstanceIndex >= Num)
			{
				continue;
			}

			FTransform InstanceTM;
			if (ISMC->GetInstanceTransform(Data.InstanceIndex, InstanceTM, /*bWorldSpace=*/true))
			{
				InstanceLocation = InstanceTM.GetLocation();
				bHasValidLocation = true;
			}
		}

		if (!bHasValidLocation)
		{
			continue;
		}

		const float DistSq = FVector::DistSquared(WorldLocation, InstanceLocation);
		if (DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			BestID = ID;
		}
	}

	return BestID;
}


FPhysXActorID UPhysXInstancedWorldSubsystem::RegisterInstancedMeshActor(APhysXInstancedMeshActor* Actor)
{
	if (!Actor || !Actor->IsValidLowLevelFast())
	{
		return FPhysXActorID(); // invalid
	}

	// If already registered, return the existing ID.
	for (const TPair<FPhysXActorID, FPhysXActorData>& Pair : Actors)
	{
		if (Pair.Value.Actor.Get() == Actor)
		{
			return Pair.Key;
		}
	}

	const FPhysXActorID NewID(NextActorID++);

	FPhysXActorData NewData;
	NewData.Actor = Actor;

	Actors.Add(NewID, NewData);
	return NewID;
}

void UPhysXInstancedWorldSubsystem::UnregisterInstancedMeshActor(FPhysXActorID ActorID)
{
	Actors.Remove(ActorID);
}

bool UPhysXInstancedWorldSubsystem::IsActorValid(FPhysXActorID ActorID) const
{
	const FPhysXActorData* Data = Actors.Find(ActorID);
	return Data && Data->Actor.IsValid();
}

APhysXInstancedMeshActor* UPhysXInstancedWorldSubsystem::GetActorByID(FPhysXActorID ActorID) const
{
	const FPhysXActorData* Data = Actors.Find(ActorID);
	return Data ? Data->Actor.Get() : nullptr;
}

TArray<FPhysXActorID> UPhysXInstancedWorldSubsystem::GetAllActorIDs() const
{
	TArray<FPhysXActorID> Result;
	Actors.GetKeys(Result);
	return Result;
}

TArray<FPhysXInstanceID> UPhysXInstancedWorldSubsystem::GetInstanceIDsForActor(FPhysXActorID ActorID) const
{
	TArray<FPhysXInstanceID> OutIDs;

	const FPhysXActorData* ActorData = Actors.Find(ActorID);
	if (!ActorData)
	{
		return OutIDs;
	}

	APhysXInstancedMeshActor* Actor = ActorData->Actor.Get();
	if (!Actor)
	{
		return OutIDs;
	}

	// Walk all instances and collect those whose ISM owner is this actor.
	for (const TPair<FPhysXInstanceID, FPhysXInstanceData>& Pair : Instances)
	{
		const FPhysXInstanceData& Data = Pair.Value;

		UInstancedStaticMeshComponent* ISMC = Data.InstancedComponent.Get();
		if (!ISMC)
		{
			continue;
		}

		if (ISMC->GetOwner() == Actor)
		{
			OutIDs.Add(Pair.Key);
		}
	}

	return OutIDs;
}

FPhysXInstanceID UPhysXInstancedWorldSubsystem::GetInstanceIDForComponentAndIndex(
	UInstancedStaticMeshComponent* InstancedMesh,
	int32 InstanceIndex) const
{
	if (!InstancedMesh || InstanceIndex < 0)
	{
		return FPhysXInstanceID();
	}

	if (const FPhysXInstanceID* Found = InstanceIDBySlot.Find(FPhysXInstanceSlotKey(InstancedMesh, InstanceIndex)))
	{
		return *Found;
	}

	// Fallback (should be rare): rebuild and try again.
	const_cast<UPhysXInstancedWorldSubsystem*>(this)->RebuildSlotMappingForComponent(InstancedMesh);

	if (const FPhysXInstanceID* FoundAfter = InstanceIDBySlot.Find(FPhysXInstanceSlotKey(InstancedMesh, InstanceIndex)))
	{
		return *FoundAfter;
	}

	return FPhysXInstanceID();
}

// ============================================================================
// Stats / random helpers
// ============================================================================

int32 UPhysXInstancedWorldSubsystem::GetTotalInstanceCount() const
{
	return Instances.Num();
}

int32 UPhysXInstancedWorldSubsystem::GetInstanceCountForComponent(
	UInstancedStaticMeshComponent* Component) const
{
	if (!Component)
	{
		return 0;
	}

	int32 Count = 0;

	for (const TPair<FPhysXInstanceID, FPhysXInstanceData>& Pair : Instances)
	{
		const FPhysXInstanceData& Data = Pair.Value;

		if (Data.InstancedComponent.Get() == Component &&
			Data.InstanceIndex != INDEX_NONE)
		{
			++Count;
		}
	}

	return Count;
}

FPhysXInstanceID UPhysXInstancedWorldSubsystem::GetRandomInstanceID(bool bOnlySimulating) const
{
	TArray<FPhysXInstanceID> Candidates;
	Candidates.Reserve(Instances.Num());

	for (const TPair<FPhysXInstanceID, FPhysXInstanceData>& Pair : Instances)
	{
		const FPhysXInstanceData& Data = Pair.Value;

		if (Data.InstanceIndex == INDEX_NONE)
		{
			continue;
		}

		UInstancedStaticMeshComponent* ISMC = Data.InstancedComponent.Get();
		if (!ISMC || !ISMC->IsValidLowLevelFast())
		{
			continue;
		}

		if (bOnlySimulating && !IsInstancePhysicsEnabled(Pair.Key))
		{
			continue;
		}

		Candidates.Add(Pair.Key);
	}

	if (Candidates.Num() == 0)
	{
		return FPhysXInstanceID();
	}

	const int32 RandomIndex = FMath::RandHelper(Candidates.Num());
	return Candidates[RandomIndex];
}

FPhysXInstanceID UPhysXInstancedWorldSubsystem::GetRandomInstanceForComponent(
	UInstancedStaticMeshComponent* Component,
	bool bOnlySimulating) const
{
	if (!Component)
	{
		return FPhysXInstanceID();
	}

	TArray<FPhysXInstanceID> Candidates;

	for (const TPair<FPhysXInstanceID, FPhysXInstanceData>& Pair : Instances)
	{
		const FPhysXInstanceData& Data = Pair.Value;

		if (Data.InstanceIndex == INDEX_NONE)
		{
			continue;
		}

		UInstancedStaticMeshComponent* ISMC = Data.InstancedComponent.Get();
		if (ISMC != Component || !ISMC->IsValidLowLevelFast())
		{
			continue;
		}

		if (bOnlySimulating && !IsInstancePhysicsEnabled(Pair.Key))
		{
			continue;
		}

		Candidates.Add(Pair.Key);
	}

	if (Candidates.Num() == 0)
	{
		return FPhysXInstanceID();
	}

	const int32 RandomIndex = FMath::RandHelper(Candidates.Num());
	return Candidates[RandomIndex];
}

void UPhysXInstancedWorldSubsystem::FixInstanceIndicesAfterRemoval(
	UInstancedStaticMeshComponent* ISMC,
	int32 RemovedIndex)
{
	if (!ISMC || RemovedIndex < 0)
	{
		return;
	}

	// UInstancedStaticMeshComponent::RemoveInstance() compacts the array (RemoveAt),
	// so all indices after RemovedIndex shift by -1.
	for (TPair<FPhysXInstanceID, FPhysXInstanceData>& Pair : Instances)
	{
		FPhysXInstanceData& OtherData = Pair.Value;

		if (OtherData.InstancedComponent.Get() != ISMC)
		{
			continue;
		}

		if (OtherData.InstanceIndex != INDEX_NONE && OtherData.InstanceIndex > RemovedIndex)
		{
			OtherData.InstanceIndex -= 1;
		}
	}
}

bool UPhysXInstancedWorldSubsystem::RemoveInstanceByID(FPhysXInstanceID ID, bool bRemoveVisualInstance)
{
	FPhysXInstanceData* Data = Instances.Find(ID);
	if (!Data)
	{
		return false;
	}

	UInstancedStaticMeshComponent* ISMC = Data->InstancedComponent.Get();
	int32 InstanceIndex = Data->InstanceIndex;
	const bool bWasSimulating = Data->bSimulating;

	APhysXInstancedMeshActor* OwnerActor = nullptr;
	bool bOwnerIsStorageActor = false;

	if (ISMC)
	{
		OwnerActor = Cast<APhysXInstancedMeshActor>(ISMC->GetOwner());
		if (OwnerActor)
		{
			bOwnerIsStorageActor = (OwnerActor->bIsStorageActor || OwnerActor->bStorageOnly);
			OwnerActor->RegisteredInstanceIDs.Remove(ID);
		}
	}

	// -----------------------------
	// PhysX cleanup first (if any)
	// -----------------------------
#if PHYSICS_INTERFACE_PHYSX
	ClearInstanceUserData(ID);
	Data->Body.Destroy();
#endif

	InvalidatePendingAddEntries(ID);

	// Remove slot mapping even if indices were already corrupted.
	RemoveSlotMapping(ID);

	// Update counters before removing the record.
	if (NumBodiesTotal > 0)
	{
		--NumBodiesTotal;
	}
	if (bWasSimulating && NumBodiesSimulating > 0)
	{
		--NumBodiesSimulating;
	}

	// If we do not need to remove the visual instance, we can drop the record now.
	if (!bRemoveVisualInstance)
	{
		Instances.Remove(ID);
		return true;
	}

	if (!ISMC || !ISMC->IsValidLowLevelFast() || InstanceIndex == INDEX_NONE)
	{
		Instances.Remove(ID);
		return false;
	}

	// -----------------------------
	// Validate / resolve the slot
	// -----------------------------
	const int32 NumBefore = ISMC->GetInstanceCount();
	if (InstanceIndex < 0 || InstanceIndex >= NumBefore ||
		InstanceIDBySlot.FindRef(FPhysXInstanceSlotKey(ISMC, InstanceIndex)) != ID)
	{
		// Rebuild mapping and try to resolve ID -> current index.
		RebuildSlotMappingForComponent(ISMC);

		int32 ResolvedIndex = INDEX_NONE;
		for (const TPair<FPhysXInstanceSlotKey, FPhysXInstanceID>& SlotPair : InstanceIDBySlot)
		{
			if (SlotPair.Value == ID && SlotPair.Key.Component.Get() == ISMC)
			{
				ResolvedIndex = SlotPair.Key.InstanceIndex;
				break;
			}
		}

		if (ResolvedIndex == INDEX_NONE || ResolvedIndex < 0 || ResolvedIndex >= ISMC->GetInstanceCount())
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[PhysXInstanced] RemoveInstanceByID: failed to resolve slot for ID=%u (ISMC=%s). Removing record only."),
				ID.GetUniqueID(), *GetNameSafe(ISMC));

			Instances.Remove(ID);
			return false;
		}

		InstanceIndex = ResolvedIndex;
	}

	const int32 OldLastIndex = ISMC->GetInstanceCount() - 1;

	// Drop the record BEFORE mutating indices of others? (Either order is OK as long as we fix others based on removal index.)
	Instances.Remove(ID);

	const bool bRemoved = ISMC->RemoveInstance(InstanceIndex);
	if (!bRemoved)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[PhysXInstanced] RemoveInstanceByID: RemoveInstance failed for ISMC=%s, Index=%d"),
			*GetNameSafe(ISMC), InstanceIndex);

		RebuildSlotMappingForComponent(ISMC);
		return false;
	}

	// UE4 path: RemoveAt compaction -> indices after removed shift by -1.
	// UE5 path: optional RemoveAtSwap -> last element moves into removed slot.
	const bool bUsedRemoveSwap =
#if ENGINE_MAJOR_VERSION >= 5
		ISMC->bSupportRemoveAtSwap != 0;
#else
		false;
#endif

	if (bUsedRemoveSwap && OldLastIndex != InstanceIndex)
	{
		// Only the old last index moved to InstanceIndex.
		for (TPair<FPhysXInstanceID, FPhysXInstanceData>& Pair : Instances)
		{
			FPhysXInstanceData& Other = Pair.Value;
			if (Other.InstancedComponent.Get() == ISMC && Other.InstanceIndex == OldLastIndex)
			{
				Other.InstanceIndex = InstanceIndex;
				break;
			}
		}
	}
	else
	{
		// Shift by -1 for all indices after InstanceIndex.
		FixInstanceIndicesAfterRemoval(ISMC, InstanceIndex);
	}

	RebuildSlotMappingForComponent(ISMC);
	ISMC->MarkRenderStateDirty();

	// Optional: auto-destroy empty storage actors to avoid accumulating dead containers.
	if (bOwnerIsStorageActor && OwnerActor && OwnerActor->InstancedMesh)
	{
		if (OwnerActor->RegisteredInstanceIDs.Num() == 0 && OwnerActor->InstancedMesh->GetInstanceCount() == 0)
		{
			if (OwnerActor->PhysXActorID.IsValid())
			{
				UnregisterInstancedMeshActor(OwnerActor->PhysXActorID);
			}
			OwnerActor->Destroy();
		}
	}

	return true;
}

void UPhysXInstancedWorldSubsystem::EnqueueAddActorToScene(
	FPhysXInstanceID ID,
	UInstancedStaticMeshComponent* InstancedMesh)
{
	if (!ID.IsValid() || !InstancedMesh)
	{
		return;
	}

	FPendingAddActorEntry Entry;
	Entry.ID = ID;
	Entry.InstancedComponent = InstancedMesh;
	Entry.World = InstancedMesh->GetWorld(); // cache once per entry
	
	PendingAddActors.Add(Entry);
}



void UPhysXInstancedWorldSubsystem::ProcessPendingAddActors()
{
	const int32 NumPending = PendingAddActors.Num() - PendingAddActorsHead;
	if (NumPending <= 0)
	{
		PendingAddActors.Reset();
		PendingAddActorsHead = 0;
		return;
	}

	UWorld* World = CachedWorld.Get();
	if (!World)
	{
		World = GetWorld();
		if (World)
		{
			CachedWorld = World;
		}
	}
	if (!World)
	{
		return;
	}

	int32 Budget = MaxAddActorsPerFrame;
	Budget = (Budget <= 0) ? NumPending : FMath::Min(Budget, NumPending);

	const int32 EndIndex = PendingAddActorsHead + Budget;

	for (int32 Index = PendingAddActorsHead; Index < EndIndex; ++Index)
	{
		const FPendingAddActorEntry& Entry = PendingAddActors[Index];

		if (!Entry.ID.IsValid())
		{
			continue;
		}

		UInstancedStaticMeshComponent* ISMC = Entry.InstancedComponent.Get();
		if (!ISMC || !ISMC->IsValidLowLevelFast())
		{
			continue;
		}

		UWorld* EntryWorld = Entry.World.Get();
		if (!EntryWorld || EntryWorld != World)
		{
			continue;
		}

		FPhysXInstanceData* Data = Instances.Find(Entry.ID);
		if (!Data)
		{
			continue;
		}

		Data->Body.AddActorToScene(EntryWorld);
		
		// Ensure userData is still correct after deferred scene insertion.
		EnsureInstanceUserData(Entry.ID);
		// Force-start simulation for instances that were registered as simulating.
		// This fixes Manual/Grid bodies that may enter the scene sleeping and never wake.
		if (Data->bSimulating)
		{
			if (physx::PxRigidActor* PxActor = Data->Body.GetPxActor())
			{
				if (physx::PxRigidDynamic* RD = PxActor->is<physx::PxRigidDynamic>())
				{
					RD->setActorFlag(physx::PxActorFlag::eDISABLE_SIMULATION, false);
					RD->setRigidBodyFlag(physx::PxRigidBodyFlag::eKINEMATIC, false);
					RD->wakeUp();
				}
			}
		}
	}

	PendingAddActorsHead = EndIndex;

	if (PendingAddActorsHead >= PendingAddActors.Num())
	{
		PendingAddActors.Reset();
		PendingAddActorsHead = 0;
	}
	else if (PendingAddActorsHead > 1024 && PendingAddActorsHead * 2 >= PendingAddActors.Num())
	{
		PendingAddActors.RemoveAt(0, PendingAddActorsHead, /*bAllowShrinking=*/false);
		PendingAddActorsHead = 0;
	}
}


int32 UPhysXInstancedWorldSubsystem::GetMaxAddActorsPerFrame() const
{
	return MaxAddActorsPerFrame;
}

void UPhysXInstancedWorldSubsystem::SetMaxAddActorsPerFrame(int32 NewMax)
{
	MaxAddActorsPerFrame = FMath::Max(0, NewMax);
}

void UPhysXInstancedWorldSubsystem::EnqueueInstanceTask(const FPhysXInstanceTask& Task)
{
	if (!Task.ID.IsValid())
	{
		return;
	}

	PendingInstanceTasks.Add(Task);
}

void UPhysXInstancedWorldSubsystem::ProcessInstanceTasks()
{
	if (PendingInstanceTasks.Num() == 0)
	{
		return;
	}

#if !PHYSICS_INTERFACE_PHYSX
	// No PhysX: tasks cannot be executed.
	PendingInstanceTasks.Reset();
	return;
#else

	const int32 Budget = (MaxInstanceTasksPerFrame <= 0)
		? TNumericLimits<int32>::Max()
		: MaxInstanceTasksPerFrame;

	// Prevent infinite growth if something is permanently broken.
	static const int32 MaxAttempts = 600; // ~10s at 60 FPS

	int32 Executed = 0;

	TArray<FPhysXInstanceTask> Remaining;
	Remaining.Reserve(PendingInstanceTasks.Num());

	for (FPhysXInstanceTask& Task : PendingInstanceTasks)
	{
		const bool bCanAttempt = (Executed < Budget);

		if (bCanAttempt)
		{
			if (TryExecuteInstanceTask(Task))
			{
				++Executed;
				continue; // consumed (success or dropped)
			}

			// Not ready yet -> keep for retry
			++Task.Attempts;
			if (Task.Attempts > MaxAttempts)
			{
				UE_LOG(LogTemp, Warning,
					TEXT("[PhysXInstanced] Task dropped (too many retries). Type=%d ID=%u"),
					(int32)Task.Type, Task.ID.GetUniqueID());
				continue; // drop
			}
		}

		Remaining.Add(Task);
	}

	PendingInstanceTasks = MoveTemp(Remaining);

#endif // PHYSICS_INTERFACE_PHYSX
}

#if PHYSICS_INTERFACE_PHYSX
bool UPhysXInstancedWorldSubsystem::TryExecuteInstanceTask(FPhysXInstanceTask& Task)
{
	FPhysXInstanceData* Data = Instances.Find(Task.ID);
	if (!Data)
	{
		return true; // drop: unknown ID
	}

	UInstancedStaticMeshComponent* ISMC = Data->InstancedComponent.Get();
	if (!ISMC || !ISMC->IsValidLowLevelFast())
	{
		return true; // drop: component is gone
	}

	// Storage handling (same semantics as your Advanced functions).
	if (IsOwnerStorageActor(ISMC))
	{
		if (!Task.bIncludeStorage)
		{
			return true; // drop
		}

		if (!Task.bConvertStorageToDynamic)
		{
			return true; // drop
		}

		if (!ConvertStorageInstanceToDynamic(Task.ID, /*bCreateDynamicActorIfNeeded=*/true))
		{
			return false; // retry later
		}

		// Refresh after conversion.
		Data = Instances.Find(Task.ID);
		if (!Data)
		{
			return true; // drop
		}
	}

	// Ensure body exists for dynamic tasks.
	if (!Data->Body.GetPxActor())
	{
		if (!SetInstancePhysicsEnabled(Task.ID, /*bEnable=*/true, /*bDestroyBodyIfDisabling=*/false))
		{
			return false; // retry later
		}
	}

	physx::PxRigidActor* RA = Data->Body.GetPxActor();
	physx::PxRigidDynamic* RD = RA ? RA->is<physx::PxRigidDynamic>() : nullptr;
	if (!RD)
	{
		return false; // retry (body creation might still be pending)
	}

	// Critical: execute only when inserted into a scene.
	// This solves all "pending add" issues for both impulses and forces.
	if (!RD->getScene())
	{
		return false; // retry later
	}

	switch (Task.Type)
	{
	case EPhysXInstanceTaskType::AddImpulse:
	{
		const physx::PxVec3 PxImpulse = U2PVector(Task.Vector);
		const physx::PxForceMode::Enum Mode = Task.bModeFlag
			? physx::PxForceMode::eVELOCITY_CHANGE
			: physx::PxForceMode::eIMPULSE;

		RD->addForce(PxImpulse, Mode, /*autowake=*/true);
		return true;
	}

	case EPhysXInstanceTaskType::AddForce:
	{
		const physx::PxVec3 PxForce = U2PVector(Task.Vector);
		const physx::PxForceMode::Enum Mode = Task.bModeFlag
			? physx::PxForceMode::eACCELERATION
			: physx::PxForceMode::eFORCE;

		RD->addForce(PxForce, Mode, /*autowake=*/true);
		return true;
	}

	case EPhysXInstanceTaskType::PutToSleep:
		RD->putToSleep();
		return true;

	case EPhysXInstanceTaskType::WakeUp:
		RD->wakeUp();
		return true;

	default:
		return true; // drop unknown
	}
}
#endif // PHYSICS_INTERFACE_PHYSX

bool UPhysXInstancedWorldSubsystem::RaycastPhysXInstanceID(
const FVector& StartWorld,
const FVector& EndWorld,
FPhysXInstanceID& OutID,
EPhysXInstancedQueryDebugMode DebugMode,
float DebugDrawDuration) const
{
#if !PHYSICS_INTERFACE_PHYSX
	OutID = FPhysXInstanceID();
	return false;
#else
	OutID = FPhysXInstanceID();

	float DistUU = TNumericLimits<float>::Max();
	FVector HitPos = FVector::ZeroVector;
	FVector HitNormal = FVector::UpVector;

	const bool bHit = RaycastPhysXInstanceID_Internal(StartWorld, EndWorld, OutID, DistUU, HitPos, HitNormal);

#if ENABLE_DRAW_DEBUG
	if (IsDebugEnabled(DebugMode))
	{
		UWorld* World = GetWorld();
		DrawLineSafe(World, StartWorld, EndWorld, bHit ? FColor::Green : FColor::Red, DebugDrawDuration);

		if (bHit)
		{
			DrawPointSafe(World, HitPos, FColor::Green, DebugDrawDuration);

			if (DebugMode == EPhysXInstancedQueryDebugMode::Detailed)
			{
				DrawArrowSafe(World, HitPos, HitPos + HitNormal.GetSafeNormal() * 30.0f, FColor::Cyan, DebugDrawDuration);

				const FString Label = FString::Printf(TEXT("PhysX ID=%u Dist=%.1f"), OutID.GetUniqueID(), DistUU);
				DrawTextSafe(World, HitPos + FVector(0, 0, 10.0f), Label, FColor::White, DebugDrawDuration);
			}
		}
	}
#endif

	return bHit;
#endif
}

bool UPhysXInstancedWorldSubsystem::RaycastInstanceID(
	const FVector& StartWorld,
	const FVector& EndWorld,
	FPhysXInstanceID& OutID,
	bool bIncludeStorage,
	ECollisionChannel TraceChannel,
	EPhysXInstancedQueryDebugMode DebugMode,
	float DebugDrawDuration) const
{
	OutID = FPhysXInstanceID();

	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	FPhysXInstanceID BestID;
	float BestDistUU = TNumericLimits<float>::Max();

	bool bPhysXHit = false;
	FPhysXInstanceID PhysXID;
	float PhysXDistUU = TNumericLimits<float>::Max();
	FVector PhysXHitPos = FVector::ZeroVector;
	FVector PhysXHitNormal = FVector::UpVector;

#if PHYSICS_INTERFACE_PHYSX
	bPhysXHit = RaycastPhysXInstanceID_Internal(StartWorld, EndWorld, PhysXID, PhysXDistUU, PhysXHitPos, PhysXHitNormal);
	if (bPhysXHit)
	{
		BestID = PhysXID;
		BestDistUU = PhysXDistUU;
	}
#endif

	bool bTraceHit = false;
	FHitResult Hit;

	if (bIncludeStorage)
	{
		FCollisionQueryParams Params(SCENE_QUERY_STAT(PhysXInstanced_RaycastInstanceID), true);

		if (World->LineTraceSingleByChannel(Hit, StartWorld, EndWorld, TraceChannel, Params))
		{
			if (UInstancedStaticMeshComponent* ISMC = Cast<UInstancedStaticMeshComponent>(Hit.GetComponent()))
			{
				const int32 HitIndex = Hit.Item;
				if (HitIndex != INDEX_NONE)
				{
					const FPhysXInstanceID ID = GetInstanceIDForComponentAndIndex(ISMC, HitIndex);
					if (ID.IsValid())
					{
						bTraceHit = true;

						if (!BestID.IsValid() || Hit.Distance < BestDistUU)
						{
							BestID = ID;
							BestDistUU = Hit.Distance;
						}
					}
				}
			}
		}
	}

	const bool bHitAny = BestID.IsValid();
	if (bHitAny)
	{
		OutID = BestID;
	}

#if ENABLE_DRAW_DEBUG
	if (IsDebugEnabled(DebugMode))
	{
		DrawLineSafe(World, StartWorld, EndWorld, bHitAny ? FColor::Green : FColor::Red, DebugDrawDuration);

		if (DebugMode == EPhysXInstancedQueryDebugMode::Detailed)
		{
			if (bPhysXHit)
			{
				DrawPointSafe(World, PhysXHitPos, FColor::Cyan, DebugDrawDuration);
				DrawArrowSafe(World, PhysXHitPos, PhysXHitPos + PhysXHitNormal.GetSafeNormal() * 30.0f, FColor::Cyan, DebugDrawDuration);
				DrawTextSafe(World, PhysXHitPos + FVector(0, 0, 10.0f),
					FString::Printf(TEXT("PhysX ID=%u Dist=%.1f"), PhysXID.GetUniqueID(), PhysXDistUU),
					FColor::Cyan, DebugDrawDuration);
			}

			if (bTraceHit)
			{
				DrawPointSafe(World, Hit.ImpactPoint, FColor::Yellow, DebugDrawDuration);
				DrawArrowSafe(World, Hit.ImpactPoint, Hit.ImpactPoint + Hit.ImpactNormal.GetSafeNormal() * 30.0f, FColor::Yellow, DebugDrawDuration);
				DrawTextSafe(World, Hit.ImpactPoint + FVector(0, 0, 10.0f),
					FString::Printf(TEXT("Trace ID=%u Dist=%.1f"), OutID.GetUniqueID(), Hit.Distance),
					FColor::Yellow, DebugDrawDuration);
			}
		}
		else if (bHitAny)
		{
			// Basic: mark only the chosen hit.
			const FVector MarkPos =
				(bPhysXHit && OutID == PhysXID) ? PhysXHitPos :
				(bTraceHit ? Hit.ImpactPoint : (StartWorld + (EndWorld - StartWorld).GetSafeNormal() * BestDistUU));

			DrawPointSafe(World, MarkPos, FColor::Green, DebugDrawDuration);
		}
	}
#endif

	return bHitAny;
}


bool UPhysXInstancedWorldSubsystem::SweepSphereInstanceID(
	const FVector& StartWorld,
	const FVector& EndWorld,
	float Radius,
	FPhysXInstanceID& OutID,
	bool bIncludeStorage,
	ECollisionChannel TraceChannel,
	EPhysXInstancedQueryDebugMode DebugMode,
	float DebugDrawDuration) const
{
	OutID = FPhysXInstanceID();

	UWorld* World = GetWorld();
	if (!World || Radius <= 0.0f)
	{
		return false;
	}

	FPhysXInstanceID BestID;
	float BestDistUU = TNumericLimits<float>::Max();

	// --- Storage / UE sweep ---
	bool bTraceHit = false;
	FHitResult TraceHit;

	if (bIncludeStorage)
	{
		FCollisionQueryParams Params(SCENE_QUERY_STAT(PhysXInstanced_SweepSphereInstanceID), true);
		const FCollisionShape Shape = FCollisionShape::MakeSphere(Radius);

		if (World->SweepSingleByChannel(TraceHit, StartWorld, EndWorld, FQuat::Identity, TraceChannel, Shape, Params))
		{
			if (UInstancedStaticMeshComponent* ISMC = Cast<UInstancedStaticMeshComponent>(TraceHit.GetComponent()))
			{
				const int32 HitIndex = TraceHit.Item;
				if (HitIndex != INDEX_NONE)
				{
					const FPhysXInstanceID ID = GetInstanceIDForComponentAndIndex(ISMC, HitIndex);
					if (ID.IsValid())
					{
						bTraceHit  = true;
						BestID     = ID;
						BestDistUU = TraceHit.Distance;
					}
				}
			}
		}
	}

	// --- PhysX sweep ---
	bool bPhysXHit = false;
	bool bBestFromPhysX = false;

	FPhysXInstanceID PhysXID;
	float PhysXDistUU = TNumericLimits<float>::Max();
	FVector PhysXHitPos = FVector::ZeroVector;
	FVector PhysXHitNormal = FVector::UpVector;

#if PHYSICS_INTERFACE_PHYSX
	if (SweepSpherePhysXInstanceID_Internal(StartWorld, EndWorld, Radius, PhysXID, PhysXDistUU, PhysXHitPos, PhysXHitNormal))
	{
		bPhysXHit = true;

		if (!BestID.IsValid() || PhysXDistUU < BestDistUU)
		{
			BestID         = PhysXID;
			BestDistUU     = PhysXDistUU;
			bBestFromPhysX = true;
		}
	}
#endif

	const bool bHitAny = BestID.IsValid();
	if (bHitAny)
	{
		OutID = BestID;
	}

#if ENABLE_DRAW_DEBUG
	if (IsDebugEnabled(DebugMode))
	{
		const FColor LineColor = bHitAny ? FColor::Green : FColor::Red;

		DrawSphereSafe(World, StartWorld, Radius, FColor::Silver, DebugDrawDuration, 1.0f);
		DrawSphereSafe(World, EndWorld,   Radius, FColor::Silver, DebugDrawDuration, 1.0f);
		DrawLineSafe(World, StartWorld, EndWorld, LineColor, DebugDrawDuration, 1.5f);

		if (DebugMode == EPhysXInstancedQueryDebugMode::Detailed)
		{
			if (bTraceHit)
			{
				DrawPointSafe(World, TraceHit.ImpactPoint, FColor::Yellow, DebugDrawDuration);
				DrawArrowSafe(World,
					TraceHit.ImpactPoint,
					TraceHit.ImpactPoint + TraceHit.ImpactNormal.GetSafeNormal() * 30.0f,
					FColor::Yellow,
					DebugDrawDuration);

				DrawTextSafe(World,
					TraceHit.ImpactPoint + FVector(0, 0, 10.0f),
					FString::Printf(TEXT("Trace ID=%u Dist=%.1f"), BestID.IsValid() ? BestID.GetUniqueID() : 0u, TraceHit.Distance),
					FColor::Yellow,
					DebugDrawDuration);
			}

			if (bPhysXHit)
			{
				DrawPointSafe(World, PhysXHitPos, FColor::Cyan, DebugDrawDuration);
				DrawArrowSafe(World,
					PhysXHitPos,
					PhysXHitPos + PhysXHitNormal.GetSafeNormal() * 30.0f,
					FColor::Cyan,
					DebugDrawDuration);

				DrawTextSafe(World,
					PhysXHitPos + FVector(0, 0, 10.0f),
					FString::Printf(TEXT("PhysX ID=%u Dist=%.1f"), PhysXID.GetUniqueID(), PhysXDistUU),
					FColor::Cyan,
					DebugDrawDuration);
			}
		}
		else if (bHitAny)
		{
			const FVector MarkPos = (bBestFromPhysX && bPhysXHit)
				? PhysXHitPos
				: (bTraceHit ? TraceHit.ImpactPoint : StartWorld);

			DrawPointSafe(World, MarkPos, FColor::Green, DebugDrawDuration);
		}
	}
#endif

	return bHitAny;
}

bool UPhysXInstancedWorldSubsystem::OverlapSphereInstanceIDs(
	const FVector& CenterWorld,
	float Radius,
	TArray<FPhysXInstanceID>& OutIDs,
	bool bIncludeStorage,
	ECollisionChannel TraceChannel,
	EPhysXInstancedQueryDebugMode DebugMode,
	float DebugDrawDuration) const
{
	OutIDs.Reset();

	UWorld* World = GetWorld();
	if (!World || Radius <= 0.0f)
	{
		return false;
	}

	TSet<FPhysXInstanceID> Unique;

	// --- Storage overlaps via ISMC ---
	if (bIncludeStorage)
	{
		TSet<UInstancedStaticMeshComponent*> Components;
		Components.Reserve(Instances.Num());

		for (const TPair<FPhysXInstanceID, FPhysXInstanceData>& Pair : Instances)
		{
			const FPhysXInstanceData& Data = Pair.Value;
			UInstancedStaticMeshComponent* ISMC = Data.InstancedComponent.Get();
			if (!ISMC || !ISMC->IsValidLowLevelFast() || Data.InstanceIndex == INDEX_NONE)
			{
				continue;
			}
			Components.Add(ISMC);
		}

		for (UInstancedStaticMeshComponent* ISMC : Components)
		{
			if (!ISMC)
			{
				continue;
			}

			if (ISMC->GetCollisionEnabled() == ECollisionEnabled::NoCollision)
			{
				continue;
			}

			if (ISMC->GetCollisionResponseToChannel(TraceChannel) == ECR_Ignore)
			{
				continue;
			}

			const TArray<int32> Indices =
				ISMC->GetInstancesOverlappingSphere(CenterWorld, Radius, /*bSphereInWorldSpace=*/true);

			for (int32 InstanceIndex : Indices)
			{
				const FPhysXInstanceID ID = GetInstanceIDForComponentAndIndex(ISMC, InstanceIndex);
				if (ID.IsValid())
				{
					Unique.Add(ID);
				}
			}
		}
	}

#if PHYSICS_INTERFACE_PHYSX
	// --- Dynamic overlaps via PhysX ---
	if (physx::PxScene* PxScenePtr = GetPhysXSceneFromWorld(World))
	{
		const physx::PxVec3 CenterPx = U2PVector(CenterWorld);
		const physx::PxSphereGeometry Geom((physx::PxReal)U2PScalar(Radius));
		const physx::PxTransform Pose(CenterPx);

		// IMPORTANT: Overlap returns *touches*. Default PxOverlapBuffer has 0 maxTouches.
		physx::PxOverlapHit Hits[256];
		physx::PxOverlapBuffer Buf(Hits, UE_ARRAY_COUNT(Hits));


		struct FFilter : physx::PxQueryFilterCallback
		{
			const UPhysXInstancedWorldSubsystem* Subsystem = nullptr;

			virtual physx::PxQueryHitType::Enum preFilter(
				const physx::PxFilterData&,
				const physx::PxShape*,
				const physx::PxRigidActor* Actor,
				physx::PxHitFlags&) override
			{
				const FPhysXInstanceID ID = Subsystem->GetInstanceIDFromPxActor(Actor);
				return ID.IsValid() ? physx::PxQueryHitType::eTOUCH : physx::PxQueryHitType::eNONE;
			}

			virtual physx::PxQueryHitType::Enum postFilter(
				const physx::PxFilterData&,
				const physx::PxQueryHit&) override
			{
				return physx::PxQueryHitType::eTOUCH;
			}
		} Filter;

		Filter.Subsystem = this;

		physx::PxQueryFilterData FD;
		FD.flags = physx::PxQueryFlag::eDYNAMIC | physx::PxQueryFlag::ePREFILTER;

		if (PxScenePtr->overlap(Geom, Pose, Buf, FD, &Filter))
		{
			for (physx::PxU32 i = 0; i < Buf.getNbTouches(); ++i)
			{
				const physx::PxOverlapHit& H = Buf.getTouch(i);
				const FPhysXInstanceID ID = GetInstanceIDFromPxActor(H.actor);
				if (ID.IsValid())
				{
					Unique.Add(ID);
				}
			}
		}
	}
#endif

	OutIDs = Unique.Array();
	const bool bAny = (OutIDs.Num() > 0);

#if ENABLE_DRAW_DEBUG
	if (IsDebugEnabled(DebugMode))
	{
		DrawSphereSafe(World, CenterWorld, Radius, bAny ? FColor::Green : FColor::Red, DebugDrawDuration, 1.5f);

		if (DebugMode == EPhysXInstancedQueryDebugMode::Detailed && bAny)
		{
			const int32 MaxMarkers = 64;
			const int32 NumToDraw = FMath::Min(OutIDs.Num(), MaxMarkers);

			for (int32 i = 0; i < NumToDraw; ++i)
			{
				const FPhysXInstanceID ID = OutIDs[i];
				FVector Pos = CenterWorld;

				if (const FPhysXInstanceData* Data = Instances.Find(ID))
				{
					if (UInstancedStaticMeshComponent* ISMC = Data->InstancedComponent.Get())
					{
#if PHYSICS_INTERFACE_PHYSX
						if (physx::PxRigidActor* RA = Data->Body.GetPxActor())
						{
							const physx::PxTransform PxPose = RA->getGlobalPose();
							Pos = P2UVector(PxPose.p);
						}
						else
#endif
						{
							if (Data->InstanceIndex != INDEX_NONE)
							{
								FTransform TM;
								if (ISMC->GetInstanceTransform(Data->InstanceIndex, TM, /*bWorldSpace=*/true))
								{
									Pos = TM.GetLocation();
								}
							}
						}
					}
				}

				DrawPointSafe(World, Pos, FColor::Cyan, DebugDrawDuration, 10.0f);
				DrawTextSafe(World, Pos + FVector(0, 0, 10.0f), FString::Printf(TEXT("ID=%u"), ID.GetUniqueID()), FColor::White, DebugDrawDuration);
			}

			if (OutIDs.Num() > MaxMarkers)
			{
				DrawTextSafe(World,
					CenterWorld + FVector(0, 0, 20.0f),
					FString::Printf(TEXT("Overlap: %d hits (showing %d)"), OutIDs.Num(), MaxMarkers),
					FColor::White,
					DebugDrawDuration);
			}
		}
	}
#endif

	return bAny;
}

void UPhysXInstancedWorldSubsystem::AddSlotMapping(FPhysXInstanceID ID)
{
	const FPhysXInstanceData* Data = Instances.Find(ID);
	if (!Data || Data->InstanceIndex == INDEX_NONE)
	{
		return;
	}

	UInstancedStaticMeshComponent* ISMC = Data->InstancedComponent.Get();
	if (!ISMC)
	{
		return;
	}

	InstanceIDBySlot.Add(FPhysXInstanceSlotKey(ISMC, Data->InstanceIndex), ID);
}

void UPhysXInstancedWorldSubsystem::RemoveSlotMapping(FPhysXInstanceID ID)
{
	const FPhysXInstanceData* Data = Instances.Find(ID);

	bool bRemovedExpected = false;

	if (Data)
	{
		UInstancedStaticMeshComponent* ISMC = Data->InstancedComponent.Get();
		if (ISMC && Data->InstanceIndex != INDEX_NONE)
		{
			const FPhysXInstanceSlotKey Key(ISMC, Data->InstanceIndex);
			bRemovedExpected = (InstanceIDBySlot.Remove(Key) > 0);
		}
	}

	// If the expected slot removal didn't happen, purge any stale entries pointing to this ID.
	if (!bRemovedExpected)
	{
		for (auto It = InstanceIDBySlot.CreateIterator(); It; ++It)
		{
			if (It.Value() == ID)
			{
				It.RemoveCurrent();
			}
		}
	}
}

void UPhysXInstancedWorldSubsystem::RebuildSlotMappingForComponent(UInstancedStaticMeshComponent* ISMC)
{
	if (!ISMC)
	{
		return;
	}

	// Remove old entries for this component.
	for (auto It = InstanceIDBySlot.CreateIterator(); It; ++It)
	{
		if (It.Key().Component.Get() == ISMC)
		{
			It.RemoveCurrent();
		}
	}

	// Re-add from authoritative Instances map.
	for (const TPair<FPhysXInstanceID, FPhysXInstanceData>& Pair : Instances)
	{
		const FPhysXInstanceID& ID = Pair.Key;
		const FPhysXInstanceData& Data = Pair.Value;

		if (Data.InstanceIndex == INDEX_NONE)
		{
			continue;
		}

		if (Data.InstancedComponent.Get() == ISMC)
		{
			InstanceIDBySlot.Add(FPhysXInstanceSlotKey(ISMC, Data.InstanceIndex), ID);
		}
	}
}

void UPhysXInstancedWorldSubsystem::InvalidatePendingAddEntries(FPhysXInstanceID ID)
{
#if PHYSICS_INTERFACE_PHYSX
	if (!ID.IsValid())
	{
		return;
	}

	for (int32 i = PendingAddActorsHead; i < PendingAddActors.Num(); ++i)
	{
		if (PendingAddActors[i].ID == ID)
		{
			PendingAddActors[i].ID = FPhysXInstanceID(); // invalidate
		}
	}
#endif
}

bool UPhysXInstancedWorldSubsystem::RemoveInstance(FPhysXInstanceID ID, bool bRemoveVisualInstance)
{
	return RemoveInstanceByID(ID, bRemoveVisualInstance);
}

#if PHYSICS_INTERFACE_PHYSX

bool UPhysXInstancedWorldSubsystem::RaycastPhysXInstanceID_Internal(
	const FVector& StartWorld,
	const FVector& EndWorld,
	FPhysXInstanceID& OutID,
	float& OutDistanceUU,
	FVector& OutHitPosWorld,
	FVector& OutHitNormalWorld) const
{
	OutID = FPhysXInstanceID();
	OutDistanceUU = TNumericLimits<float>::Max();
	OutHitPosWorld = FVector::ZeroVector;
	OutHitNormalWorld = FVector::UpVector;

	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	physx::PxScene* PxScenePtr = GetPhysXSceneFromWorld(World);
	if (!PxScenePtr)
	{
		return false;
	}

	const FVector DirU = (EndWorld - StartWorld);
	const float DistU = DirU.Size();
	if (DistU <= KINDA_SMALL_NUMBER)
	{
		return false;
	}

	const physx::PxVec3 OriginPx = U2PVector(StartWorld);
	const physx::PxVec3 DirPx    = U2PVector(DirU / DistU);
	const physx::PxReal DistPx   = (physx::PxReal)U2PScalar(DistU);

	physx::PxRaycastBuffer Hit;
	const physx::PxHitFlags HitFlags = physx::PxHitFlag::eDEFAULT;

	struct FFilter : physx::PxQueryFilterCallback
	{
		const UPhysXInstancedWorldSubsystem* Subsystem = nullptr;

		virtual physx::PxQueryHitType::Enum preFilter(
			const physx::PxFilterData&,
			const physx::PxShape*,
			const physx::PxRigidActor* Actor,
			physx::PxHitFlags&) override
		{
			const FPhysXInstanceID ID = Subsystem->GetInstanceIDFromPxActor(Actor);
			return ID.IsValid() ? physx::PxQueryHitType::eBLOCK : physx::PxQueryHitType::eNONE;
		}

		virtual physx::PxQueryHitType::Enum postFilter(
			const physx::PxFilterData&,
			const physx::PxQueryHit&) override
		{
			return physx::PxQueryHitType::eBLOCK;
		}
	} Filter;

	Filter.Subsystem = this;

	physx::PxQueryFilterData FD;
	FD.flags = physx::PxQueryFlag::eDYNAMIC | physx::PxQueryFlag::ePREFILTER;

	const bool bHit = PxScenePtr->raycast(OriginPx, DirPx, DistPx, Hit, HitFlags, FD, &Filter);
	if (!bHit || !Hit.hasBlock)
	{
		return false;
	}

	OutID = GetInstanceIDFromPxActor(Hit.block.actor);
	if (!OutID.IsValid())
	{
		return false;
	}

	const FVector HitPosUU = P2UVector(Hit.block.position);
	const FVector HitNormalUU = P2UVector(Hit.block.normal).GetSafeNormal();

	OutHitPosWorld = HitPosUU;
	OutHitNormalWorld = HitNormalUU;

	OutDistanceUU = FVector::Dist(StartWorld, HitPosUU);
	return true;
}


bool UPhysXInstancedWorldSubsystem::SweepSpherePhysXInstanceID_Internal(
	const FVector& StartWorld,
	const FVector& EndWorld,
	float Radius,
	FPhysXInstanceID& OutID,
	float& OutDistanceUU,
	FVector& OutHitPosWorld,
	FVector& OutHitNormalWorld) const
{
	OutID = FPhysXInstanceID();
	OutDistanceUU = TNumericLimits<float>::Max();
	OutHitPosWorld = FVector::ZeroVector;
	OutHitNormalWorld = FVector::UpVector;

	UWorld* World = GetWorld();
	if (!World || Radius <= 0.0f)
	{
		return false;
	}

	physx::PxScene* PxScenePtr = GetPhysXSceneFromWorld(World);
	if (!PxScenePtr)
	{
		return false;
	}

	const FVector DirU = (EndWorld - StartWorld);
	const float DistU = DirU.Size();
	if (DistU <= KINDA_SMALL_NUMBER)
	{
		return false;
	}

	const physx::PxVec3 OriginPx = U2PVector(StartWorld);
	const physx::PxVec3 DirPx    = U2PVector(DirU / DistU);
	const physx::PxReal DistPx   = (physx::PxReal)U2PScalar(DistU);

	const physx::PxTransform Pose(OriginPx);
	const physx::PxSphereGeometry Geom((physx::PxReal)U2PScalar(Radius));

	physx::PxSweepBuffer Hit;
	const physx::PxHitFlags HitFlags = physx::PxHitFlag::eDEFAULT;

	struct FFilter : physx::PxQueryFilterCallback
	{
		const UPhysXInstancedWorldSubsystem* Subsystem = nullptr;

		virtual physx::PxQueryHitType::Enum preFilter(
			const physx::PxFilterData&,
			const physx::PxShape*,
			const physx::PxRigidActor* Actor,
			physx::PxHitFlags&) override
		{
			const FPhysXInstanceID ID = Subsystem->GetInstanceIDFromPxActor(Actor);
			return ID.IsValid() ? physx::PxQueryHitType::eBLOCK : physx::PxQueryHitType::eNONE;
		}

		virtual physx::PxQueryHitType::Enum postFilter(
			const physx::PxFilterData&,
			const physx::PxQueryHit&) override
		{
			return physx::PxQueryHitType::eBLOCK;
		}
	} Filter;

	Filter.Subsystem = this;

	physx::PxQueryFilterData FD;
	FD.flags = physx::PxQueryFlag::eDYNAMIC | physx::PxQueryFlag::ePREFILTER;

	const bool bHit = PxScenePtr->sweep(Geom, Pose, DirPx, DistPx, Hit, HitFlags, FD, &Filter);
	if (!bHit || !Hit.hasBlock)
	{
		return false;
	}

	OutID = GetInstanceIDFromPxActor(Hit.block.actor);
	if (!OutID.IsValid())
	{
		return false;
	}

	const FVector HitPosUU = P2UVector(Hit.block.position);
	const FVector HitNormalUU = P2UVector(Hit.block.normal).GetSafeNormal();

	OutHitPosWorld = HitPosUU;
	OutHitNormalWorld = HitNormalUU;

	OutDistanceUU = FVector::Dist(StartWorld, HitPosUU);
	return true;
}
#endif // PHYSICS_INTERFACE_PHYSX

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
	// Destroy all bodies that were created by this subsystem.
	for (TPair<FPhysXInstanceID, FPhysXInstanceData>& Pair : Instances)
	{
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
	// Add pending bodies to the PhysX scene with a per-frame budget.
	ProcessPendingAddActors();
#endif

	// Read back PhysX actor transforms into ISM instances.
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

	
	Instances.Add(NewID, NewData);
	
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

				if (OutInstanceIDs.IsValidIndex(JobIndex))
				{
					OutInstanceIDs[JobIndex] = FPhysXInstanceID(); // invalid
				}

				continue;
			}

			// Queue PhysX actor for scene insertion on the game thread.
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
		// Destroy the PhysX body associated with this instance.
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

		Instances.Remove(ID);
	}
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

	// Fast early-out: there are no simulating bodies at all.
	if (NumBodiesSimulating <= 0)
	{
		NumBodiesTotal      = 0;
		NumBodiesSimulating = 0;
		NumBodiesSleeping   = 0;

		SET_DWORD_STAT(STAT_PhysXInstanced_BodiesTotal,      NumBodiesTotal);
		SET_DWORD_STAT(STAT_PhysXInstanced_BodiesSimulating, NumBodiesSimulating);
		SET_DWORD_STAT(STAT_PhysXInstanced_BodiesSleeping,   NumBodiesSleeping);
		SET_DWORD_STAT(STAT_PhysXInstanced_JobsPerFrame,     0);
		SET_DWORD_STAT(STAT_PhysXInstanced_InstancesTotal,   Instances.Num());
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

	// --------------------------------------------------------------------
	// Helper: fix indices after RemoveInstance
	// --------------------------------------------------------------------

	auto FixInstanceIndicesAfterRemoval = [this](UInstancedStaticMeshComponent* ISMC, int32 RemovedIndex, int32 OldLastIndex)
	{
		if (!ISMC)
		{
			return;
		}
		
		if (RemovedIndex < 0 || OldLastIndex < 0)
		{
			return;
		}
		
		if (RemovedIndex == OldLastIndex)
			{
				return; 
				}
		
		for (TPair<FPhysXInstanceID, FPhysXInstanceData>& OtherPair : Instances)
			{
				FPhysXInstanceData& OtherData = OtherPair.Value;
				if (OtherData.InstancedComponent.Get() != ISMC)
					{
						continue;
					}
			
				if (OtherData.InstanceIndex == OldLastIndex)
					{
						OtherData.InstanceIndex = RemovedIndex;
						break;
						}
				}
	};

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
		const bool bUseActiveActorFilter = (ActiveActorsSet.Num() > 0);

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
				bIsActiveActor = ActiveActorsSet.Contains(RigidActor);
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

		auto ApplyStopAction_GameThread =
			[this, &FixInstanceIndicesAfterRemoval, &DirtyComponents](FPhysXInstanceAsyncStepJob& JobData)
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
				InstanceData->Body.Destroy();
				InstanceData->bSimulating = false;
				break;

			case EPhysXInstanceStopAction::DestroyBodyAndRemoveInstance:
			{
				InstanceData->Body.Destroy();
				InstanceData->bSimulating = false;

				if (ISMComponent && InstanceData->InstanceIndex != INDEX_NONE)
				{
					const int32 RemovedIndex = InstanceData->InstanceIndex;
					const int32 OldLastIndex = ISMComponent->GetInstanceCount() - 1;
					const bool bRemoved = ISMComponent->RemoveInstance(RemovedIndex);
					if (bRemoved)
					{
						FixInstanceIndicesAfterRemoval(ISMComponent, RemovedIndex, OldLastIndex);
						InstanceData->InstanceIndex = INDEX_NONE;
						DirtyComponents.Add(ISMComponent);
					}
					else
					{
						
					}

					DirtyComponents.Add(ISMComponent);
				}
				break;
			}
				
			case EPhysXInstanceStopAction::ConvertToStorage:
				{
					const FPhysXInstanceID ConvertID = JobData.ID;

					// Moves to storage actor and unregisters this ID from subsystem.
					if (ConvertInstanceToStaticStorage(ConvertID, /*bCreateStorageActorIfNeeded=*/true))
					{
						// IMPORTANT: ConvertInstanceToStaticStorage() calls UnregisterInstance(),
						// which removes the entry from Instances -> JobData.Data pointer becomes invalid.
						JobData.Data         = nullptr;
						JobData.ISMC         = nullptr;
						JobData.RigidDynamic = nullptr;
						return; // Exit the lambda immediately.
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
	// 3) Hide the source instance and remove its PhysX body
	// ---------------------------------------------------------------------

	FTransform HiddenTM = WorldTM;
	HiddenTM.AddToTranslation(FVector(0.0f, 0.0f, -1000000.0f));

	ISMC->UpdateInstanceTransform(
		InstanceIndex,
		HiddenTM,
		/*bWorldSpace=*/true,
		/*bMarkRenderStateDirty=*/true,
		/*bTeleport=*/true);
	
	// Keep the ISM index -> ID mapping stable: the visual instance is NOT removed, only hidden.
	// So we invalidate the entry instead of compacting the array.
	if (SourceActor)
		{
			if (SourceActor->RegisteredInstanceIDs.IsValidIndex(InstanceIndex) &&
				SourceActor->RegisteredInstanceIDs[InstanceIndex] == ID)
				{
		SourceActor->RegisteredInstanceIDs[InstanceIndex] = FPhysXInstanceID(); // invalid
		}
			else
				{
		const int32 Found = SourceActor->RegisteredInstanceIDs.IndexOfByKey(ID);
		if (Found != INDEX_NONE)
			{
				SourceActor->RegisteredInstanceIDs[Found] = FPhysXInstanceID(); // invalid
				}
		}
		}
	// Destroy the PhysX body and remove the instance record from the subsystem.
	UnregisterInstance(ID);

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
	FPhysXInstanceData* Data = Instances.Find(ID);
	if (!Data)
	{
		return false;
	}

#if !PHYSICS_INTERFACE_PHYSX
	// No PhysX back-end: nothing to do.
	return false;
#else
	physx::PxRigidActor*   Actor        = Data->Body.GetPxActor();
	physx::PxRigidDynamic* RigidDynamic = Actor ? Actor->is<physx::PxRigidDynamic>() : nullptr;
	if (!RigidDynamic)
	{
		return false;
	}

	const PxVec3 PxImpulse = U2PVector(WorldImpulse);

	const PxForceMode::Enum Mode = bVelChange
		? PxForceMode::eVELOCITY_CHANGE
		: PxForceMode::eIMPULSE;

	RigidDynamic->addForce(PxImpulse, Mode, /*autowake=*/true);

	return true;
#endif // PHYSICS_INTERFACE_PHYSX
}

bool UPhysXInstancedWorldSubsystem::AddForceToInstance(
	FPhysXInstanceID ID,
	FVector WorldForce,
	bool bAccelChange)
{
	FPhysXInstanceData* Data = Instances.Find(ID);
	if (!Data)
	{
		return false;
	}

#if !PHYSICS_INTERFACE_PHYSX
	return false;
#else
	physx::PxRigidActor*   Actor        = Data->Body.GetPxActor();
	physx::PxRigidDynamic* RigidDynamic = Actor ? Actor->is<physx::PxRigidDynamic>() : nullptr;
	if (!RigidDynamic)
	{
		return false;
	}

	const PxVec3 PxForce = U2PVector(WorldForce);

	const PxForceMode::Enum Mode = bAccelChange
		? PxForceMode::eACCELERATION
		: PxForceMode::eFORCE;

	RigidDynamic->addForce(PxForce, Mode, /*autowake=*/true);

	return true;
#endif // PHYSICS_INTERFACE_PHYSX
}

bool UPhysXInstancedWorldSubsystem::PutInstanceToSleep(FPhysXInstanceID ID)
{
	FPhysXInstanceData* Data = Instances.Find(ID);
	if (!Data)
	{
		return false;
	}

#if !PHYSICS_INTERFACE_PHYSX
	return false;
#else
	physx::PxRigidActor*   Actor        = Data->Body.GetPxActor();
	physx::PxRigidDynamic* RigidDynamic = Actor ? Actor->is<physx::PxRigidDynamic>() : nullptr;
	if (!RigidDynamic)
	{
		return false;
	}

	// Puts the actor to sleep; any forces/velocities are cleared.
	RigidDynamic->putToSleep();
	return true;
#endif // PHYSICS_INTERFACE_PHYSX
}

bool UPhysXInstancedWorldSubsystem::WakeInstanceUp(FPhysXInstanceID ID)
{
	FPhysXInstanceData* Data = Instances.Find(ID);
	if (!Data)
	{
		return false;
	}

#if !PHYSICS_INTERFACE_PHYSX
	return false;
#else
	physx::PxRigidActor*   Actor        = Data->Body.GetPxActor();
	physx::PxRigidDynamic* RigidDynamic = Actor ? Actor->is<physx::PxRigidDynamic>() : nullptr;
	if (!RigidDynamic)
	{
		return false;
	}

	// Wake the actor; PhysX will start integrating it again.
	RigidDynamic->wakeUp();
	return true;
#endif // PHYSICS_INTERFACE_PHYSX
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

	return (Data->InstanceIndex != INDEX_NONE);
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
	FPhysXInstanceID BestID;
	float BestDistSq = TNumericLimits<float>::Max();

	for (const TPair<FPhysXInstanceID, FPhysXInstanceData>& Pair : Instances)
	{
		const FPhysXInstanceData& Data = Pair.Value;

		UInstancedStaticMeshComponent* ISMC = Data.InstancedComponent.Get();
		if (!ISMC || !ISMC->IsValidLowLevelFast())
		{
			continue;
		}

		if (OptionalFilterComponent && ISMC != OptionalFilterComponent)
		{
			continue;
		}

		FVector InstanceLocation = FVector::ZeroVector;

#if PHYSICS_INTERFACE_PHYSX
		if (physx::PxRigidActor* RigidActor = Data.Body.GetPxActor())
		{
			const physx::PxTransform PxPose = RigidActor->getGlobalPose();
			InstanceLocation = P2UVector(PxPose.p);
		}
		else
#endif
		{
			FTransform InstanceTM;
			if (!ISMC->GetInstanceTransform(Data.InstanceIndex, InstanceTM, /*bWorldSpace=*/true))
			{
				continue;
			}
			InstanceLocation = InstanceTM.GetLocation();
		}

		const float DistSq = FVector::DistSquared(WorldLocation, InstanceLocation);
		if (DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			BestID     = Pair.Key;
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
		return FPhysXInstanceID(); // invalid
	}

	for (const TPair<FPhysXInstanceID, FPhysXInstanceData>& Pair : Instances)
	{
		const FPhysXInstanceData& Data = Pair.Value;

		if (Data.InstanceIndex == InstanceIndex &&
			Data.InstancedComponent.Get() == InstancedMesh)
		{
			return Pair.Key;
		}
	}

	return FPhysXInstanceID(); // not found
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

#if PHYSICS_INTERFACE_PHYSX

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

#endif // PHYSICS_INTERFACE_PHYSX

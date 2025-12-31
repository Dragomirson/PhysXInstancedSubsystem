/**
* Copyright (C) 2025 | Created by NordVader Inc.
* All rights reserved!
* My Discord Server: https://discord.gg/B8prpf3vzD
*/

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "Subsystems/WorldSubsystem.h"
#include "Templates/UniquePtr.h"

#include "Types/PhysXInstancedTypes.h"
#include "Processes/PhysXInstancedProcessPipeline.h"

#include "Actors/PhysXInstancedMeshActor.h"
#include "PhysicalMaterials/PhysicalMaterial.h"

#if PHYSICS_INTERFACE_PHYSX
	#include "PhysXPublic.h"
	#include "PhysXIncludes.h"
	#include "PxRigidBodyExt.h"
#endif


#include "PhysXInstancedWorldSubsystem.generated.h"

class UInstancedStaticMeshComponent;
class UPhysXInstancedStaticMeshComponent;
class APhysXInstancedMeshActor;

#if PHYSICS_INTERFACE_PHYSX
namespace physx
{
	class PxRigidActor;
	class PxRigidDynamic;
}
#endif

namespace PhysXIS
{
#if PHYSICS_INTERFACE_PHYSX
	class FAddActorsProcess;
	class FInstanceTasksProcess;

	class FPhysicsStepComputeProcess;
	class FPhysicsStepStopActionsProcess;
	class FPhysicsStepTransformSyncProcess;
	class FPhysicsStepFinalizeProcess;
#endif

	class FPhysicsStepProcess;
	class FLifetimeProcess;
}

/**
 * World-level subsystem that owns all PhysX-backed instanced bodies.
 *
 * Concept:
 * - APhysXInstancedMeshActor renders many instances through an ISM component.
 * - Any instance can optionally have its own PhysX body.
 * - Gameplay talks to instances through stable uint32-based IDs, not raw pointers.
 */
UCLASS(Config = Game)
class PHYSXINSTANCEDSUBSYSTEM_API UPhysXInstancedWorldSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	UPhysXInstancedWorldSubsystem();

	// ---------------------------------------------------------------------
	// UWorldSubsystem
	// ---------------------------------------------------------------------

	/** Called once when the subsystem is created for a given world. */
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	/** Called when the world/subsystem is being torn down. */
	virtual void Deinitialize() override;

	// ---------------------------------------------------------------------
	// UTickableWorldSubsystem
	// ---------------------------------------------------------------------

	/** Per-frame update: advances async work and pushes physics transforms back into ISM instances. */
	virtual void Tick(float DeltaTime) override;

	/** Required by the tick system for stat profiling. */
	virtual TStatId GetStatId() const override;

	// ---------------------------------------------------------------------
	// Spawn
	// ---------------------------------------------------------------------

	/**
	 * Creates or reuses a PhysXInstancedMeshActor and spawns one ISM visual instance,
	 * with an optional PhysX body depending on the request.
	 */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Spawn")
	FPhysXSpawnInstanceResult SpawnPhysicsInstance(const FPhysXSpawnInstanceRequest& Request);

	// ---------------------------------------------------------------------
	// Instance registration & removal
	// ---------------------------------------------------------------------

	/**
	 * Register a single ISM instance in the subsystem.
	 *
	 * @param InstancedMesh  Owning instanced static mesh component.
	 * @param InstanceIndex  Index inside the ISM (0..NumInstances-1).
	 * @param bSimulate      If true, a PhysX body is created and starts simulating.
	 *
	 * @return Stable handle used to control the instance later.
	 *         Returns an invalid ID (UniqueID == 0) on failure.
	 */
	FPhysXInstanceID RegisterInstance(
		UInstancedStaticMeshComponent* InstancedMesh,
		int32 InstanceIndex,
		bool bSimulate);

	/**
	 * Register many ISM instances in one pass.
	 * This path is intended for mass registration and may spread heavy work across frames.
	 */
	void RegisterInstancesBatch(
		UInstancedStaticMeshComponent* InstancedMesh,
		const TArray<int32>& InstanceIndices,
		bool bSimulate,
		TArray<FPhysXInstanceID>& OutInstanceIDs);

	/** Removes an instance record; if a PhysX body exists, it is destroyed as well. */
	void UnregisterInstance(FPhysXInstanceID ID);

	bool RemoveInstanceByID(FPhysXInstanceID ID, bool bRemoveVisualInstance);

	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Removal")
	bool RemoveInstance(FPhysXInstanceID ID, bool bRemoveVisualInstance = true);

	// ---------------------------------------------------------------------
	// Physics stepping
	// ---------------------------------------------------------------------

	/**
	 * Steps registered PhysX bodies and prepares pose updates for ISM instances.
	 * Usually driven by Tick, but can be called manually.
	 */
	void AsyncPhysicsStep(float DeltaTime, float SimTime);

	// ---------------------------------------------------------------------
	// Conversion (dynamic <-> storage)
	// ---------------------------------------------------------------------

	/**
	 * Converts a dynamic PhysX-backed instance into a storage instance:
	 * - PhysX body is destroyed
	 * - instance is moved to a storage actor (mesh/material match)
	 * - the stable FPhysXInstanceID stays the same
	 */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Conversion")
	bool ConvertInstanceToStaticStorage(FPhysXInstanceID ID, bool bCreateStorageActorIfNeeded = true);

	/**
	 * Converts a storage-only instance into a dynamic instance:
	 * - instance is moved from a storage actor to a dynamic actor
	 * - a PhysX body is created
	 * - the stable FPhysXInstanceID stays the same
	 */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Conversion")
	bool ConvertStorageInstanceToDynamic(FPhysXInstanceID ID, bool bCreateDynamicActorIfNeeded = true);
private:
	// Internal API used by TTL/AutoStop to pass a reason to events
	bool ConvertInstanceToStaticStorage_Internal(
		FPhysXInstanceID ID,
		bool bCreateStorageActorIfNeeded,
		EPhysXInstanceConvertReason Reason);

	bool ConvertStorageInstanceToDynamic_Internal(
		FPhysXInstanceID ID,
		bool bCreateDynamicActorIfNeeded,
		EPhysXInstanceConvertReason Reason);
public:
	// ---------------------------------------------------------------------
	// Physics enable/disable
	// ---------------------------------------------------------------------

	/**
	 * Enables or disables simulation for a specific instance.
	 *
	 * When disabling with bDestroyBodyIfDisabling == true:
	 * - PhysX body is destroyed
	 * - the visual instance remains in place
	 */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Physics")
	bool SetInstancePhysicsEnabled(FPhysXInstanceID ID, bool bEnable, bool bDestroyBodyIfDisabling = false);

	/** Returns true if the instance currently has an active dynamic PhysX body. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Physics")
	bool IsInstancePhysicsEnabled(FPhysXInstanceID ID) const;

	// ---------------------------------------------------------------------
	// Forces / impulses
	// ---------------------------------------------------------------------

	/** Adds a world-space impulse at the body's center of mass. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Forces")
	bool AddImpulseToInstance(FPhysXInstanceID ID, FVector WorldImpulse, bool bVelChange = false);

	/**
	 * Same as AddImpulseToInstance, with explicit storage handling.
	 * Storage instances can be accepted and optionally converted to dynamic first.
	 */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Forces",
		meta = (AdvancedDisplay = "bIncludeStorage,bConvertStorageToDynamic"))
	bool AddImpulseToInstanceAdvanced(
		FPhysXInstanceID ID,
		FVector WorldImpulse,
		bool bVelChange = false,
		bool bIncludeStorage = true,
		bool bConvertStorageToDynamic = true);

	/** Adds a continuous world-space force. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Forces")
	bool AddForceToInstance(FPhysXInstanceID ID, FVector WorldForce, bool bAccelChange = false);

	/** Same as AddForceToInstance, with explicit storage handling. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Forces",
		meta = (AdvancedDisplay = "bIncludeStorage,bConvertStorageToDynamic"))
	bool AddForceToInstanceAdvanced(
		FPhysXInstanceID ID,
		FVector WorldForce,
		bool bAccelChange = false,
		bool bIncludeStorage = true,
		bool bConvertStorageToDynamic = true);

	/** Puts the rigid body to sleep (storage handling controls apply). */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Forces",
		meta = (AdvancedDisplay = "bIncludeStorage,bConvertStorageToDynamic"))
	bool PutInstanceToSleepAdvanced(
		FPhysXInstanceID ID,
		bool bIncludeStorage = true,
		bool bConvertStorageToDynamic = true);

	/** Wakes up the rigid body (storage handling controls apply). */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Forces",
		meta = (AdvancedDisplay = "bIncludeStorage,bConvertStorageToDynamic"))
	bool WakeInstanceUpAdvanced(
		FPhysXInstanceID ID,
		bool bIncludeStorage = true,
		bool bConvertStorageToDynamic = true);

	/** Puts the rigid body to sleep (only if a dynamic body exists). */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Forces")
	bool PutInstanceToSleep(FPhysXInstanceID ID);

	/** Wakes up the rigid body so it resumes simulation. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Forces")
	bool WakeInstanceUp(FPhysXInstanceID ID);

	/**
	 * Applies a radial impulse around OriginWorld to all instances within Radius.
	 * Storage instances can be accepted and optionally converted to dynamic first.
	 */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Forces",
		meta = (AdvancedDisplay = "bIncludeStorage,bConvertStorageToDynamic,bLinearFalloff,DebugMode,DebugDrawDuration"))
	bool AddRadialImpulse(
		const FVector& OriginWorld,
		float Radius,
		float Strength,
		bool bVelChange = false,
		bool bIncludeStorage = true,
		bool bConvertStorageToDynamic = true,
		bool bLinearFalloff = true,
		EPhysXInstancedQueryDebugMode DebugMode = EPhysXInstancedQueryDebugMode::None,
		float DebugDrawDuration = 0.0f);

	// ---------------------------------------------------------------------
	// Per-instance physics properties
	// ---------------------------------------------------------------------

	/** Enables/disables gravity for the instance (PhysX: PxActorFlag::eDISABLE_GRAVITY). */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Physics")
	bool SetInstanceGravityEnabled(FPhysXInstanceID ID, bool bEnable);

	/** Returns true if gravity is enabled for the instance. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Physics")
	bool IsInstanceGravityEnabled(FPhysXInstanceID ID) const;

	/** Sets linear velocity (units per second). */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Physics")
	bool SetInstanceLinearVelocity(FPhysXInstanceID ID, FVector NewVelocity, bool bAutoWake = true);

	/** Reads linear velocity. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Physics")
	bool GetInstanceLinearVelocity(FPhysXInstanceID ID, FVector& OutVelocity) const;

	/** Sets angular velocity in radians per second. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Physics")
	bool SetInstanceAngularVelocityInRadians(FPhysXInstanceID ID, FVector NewAngVelRad, bool bAutoWake = true);

	/** Reads angular velocity in radians per second. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Physics")
	bool GetInstanceAngularVelocityInRadians(FPhysXInstanceID ID, FVector& OutAngVelRad) const;

	// ---------------------------------------------------------------------
	// Query helpers
	// ---------------------------------------------------------------------

	/** Returns true if the ID exists and its owning component is still valid. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Query")
	bool IsInstanceValid(FPhysXInstanceID ID) const;

	/** Resolves owning component and instance index for a given ID. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Query")
	bool GetInstanceInfo(
		FPhysXInstanceID ID,
		UInstancedStaticMeshComponent*& OutComponent,
		int32& OutInstanceIndex) const;

	/** Returns a snapshot array of all currently registered instance IDs. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Query")
	TArray<FPhysXInstanceID> GetAllInstanceIDs() const;

	/** Finds the instance closest to WorldLocation; can be restricted to a component. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Query")
	FPhysXInstanceID FindNearestInstance(
		FVector WorldLocation,
		UInstancedStaticMeshComponent* OptionalFilterComponent = nullptr) const;

	/**
	 * Nearest-instance search with exclusions.
	 * - If bIncludeStorage is false, storage instances are ignored.
	 * - IgnoreInstanceIndex is meaningful only when OptionalFilterComponent is set.
	 */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Query",
		meta = (AdvancedDisplay = "OptionalFilterComponent,IgnoreInstanceID,IgnoreInstanceIndex,bIncludeStorage"))
	FPhysXInstanceID FindNearestInstanceAdvanced(
		FVector WorldLocation,
		UInstancedStaticMeshComponent* OptionalFilterComponent,
		FPhysXInstanceID IgnoreInstanceID,
		int32 IgnoreInstanceIndex,
		bool bIncludeStorage) const;

	/** Looks up an instance ID by its component and index. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Query")
	FPhysXInstanceID GetInstanceIDForComponentAndIndex(
		UInstancedStaticMeshComponent* InstancedMesh,
		int32 InstanceIndex) const;

	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Query",
		meta = (AdvancedDisplay = "DebugMode,DebugDrawDuration"))
	bool RaycastPhysXInstanceID(
		const FVector& StartWorld,
		const FVector& EndWorld,
		FPhysXInstanceID& OutID,
		EPhysXInstancedQueryDebugMode DebugMode = EPhysXInstancedQueryDebugMode::None,
		float DebugDrawDuration = 0.0f) const;

	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Query",
		meta = (AdvancedDisplay = "bIncludeStorage,TraceChannel,DebugMode,DebugDrawDuration"))
	bool RaycastInstanceID(
		const FVector& StartWorld,
		const FVector& EndWorld,
		FPhysXInstanceID& OutID,
		bool bIncludeStorage = true,
		ECollisionChannel TraceChannel = ECC_Visibility,
		EPhysXInstancedQueryDebugMode DebugMode = EPhysXInstancedQueryDebugMode::None,
		float DebugDrawDuration = 0.0f) const;

	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Query",
		meta = (AdvancedDisplay = "bIncludeStorage,TraceChannel,DebugMode,DebugDrawDuration"))
	bool SweepSphereInstanceID(
		const FVector& StartWorld,
		const FVector& EndWorld,
		float Radius,
		FPhysXInstanceID& OutID,
		bool bIncludeStorage = true,
		ECollisionChannel TraceChannel = ECC_Visibility,
		EPhysXInstancedQueryDebugMode DebugMode = EPhysXInstancedQueryDebugMode::None,
		float DebugDrawDuration = 0.0f) const;

	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Query",
		meta = (AdvancedDisplay = "bIncludeStorage,TraceChannel,DebugMode,DebugDrawDuration"))
	bool OverlapSphereInstanceIDs(
		const FVector& CenterWorld,
		float Radius,
		TArray<FPhysXInstanceID>& OutIDs,
		bool bIncludeStorage = true,
		ECollisionChannel TraceChannel = ECC_Visibility,
		EPhysXInstancedQueryDebugMode DebugMode = EPhysXInstancedQueryDebugMode::None,
		float DebugDrawDuration = 0.0f) const;

	// ---------------------------------------------------------------------
	// Actor-level registration & query
	// ---------------------------------------------------------------------

	/** Registers an APhysXInstancedMeshActor and returns a stable actor ID. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Actor|Registration")
	FPhysXActorID RegisterInstancedMeshActor(APhysXInstancedMeshActor* Actor);

	/** Unregisters an actor. Typically called from EndPlay. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Actor|Registration")
	void UnregisterInstancedMeshActor(FPhysXActorID ActorID);

	/** Returns true if the actor ID is still valid. */
	UFUNCTION(BlueprintPure, Category = "Phys X Actor|Query")
	bool IsActorValid(FPhysXActorID ActorID) const;

	/** Resolves actor pointer from its ID; returns nullptr if invalid/destroyed. */
	UFUNCTION(BlueprintPure, Category = "Phys X Actor|Query")
	APhysXInstancedMeshActor* GetActorByID(FPhysXActorID ActorID) const;

	/** Returns a snapshot array of all registered actor IDs. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Actor|Query")
	TArray<FPhysXActorID> GetAllActorIDs() const;

	/** Returns all instance IDs that belong to a given actor (filtered from internal storage). */
	UFUNCTION(BlueprintCallable, Category = "Phys X Actor|Query")
	TArray<FPhysXInstanceID> GetInstanceIDsForActor(FPhysXActorID ActorID) const;

	// ---------------------------------------------------------------------
	// Stats / random
	// ---------------------------------------------------------------------

	/** Total number of registered instances in this world. */
	UFUNCTION(BlueprintPure, Category = "Phys X Instance|Stats")
	int32 GetTotalInstanceCount() const;

	/** Counts how many registered instances belong to a specific component. */
	UFUNCTION(BlueprintPure, Category = "Phys X Instance|Stats")
	int32 GetInstanceCountForComponent(UInstancedStaticMeshComponent* Component) const;

	/** Returns a random registered instance ID (optionally restricted to simulating only). */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Random")
	FPhysXInstanceID GetRandomInstanceID(bool bOnlySimulating = false) const;

	/** Returns a random instance ID from a specific component (optionally simulating only). */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Random")
	FPhysXInstanceID GetRandomInstanceForComponent(
		UInstancedStaticMeshComponent* Component,
		bool bOnlySimulating = false) const;

	// ---------------------------------------------------------------------
	// Performance tuning
	// ---------------------------------------------------------------------

	/** Current per-frame budget for adding new PhysX actors into the scene. */
	UFUNCTION(BlueprintPure, Category = "Phys X Instance|Performance")
	int32 GetMaxAddActorsPerFrame() const;

	/**
	 * Overrides per-frame budget for adding new PhysX actors into the scene.
	 * 0 means "no limit" (all pending bodies can be added in one frame).
	 */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Performance")
	void SetMaxAddActorsPerFrame(int32 NewMax);

	// ---------------------------------------------------------------------
	// Lifetime (TTL)
	// ---------------------------------------------------------------------

	/**
	 * Re-applies this actor's lifetime defaults to already registered instances.
	 *
	 * @param bForce If true, overwrites existing lifetime values.
	 * @return Number of instances updated.
	 */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Lifetime")
	int32 ApplyActorLifetimeDefaults(APhysXInstancedMeshActor* Actor, bool bForce = true);

private:
	virtual ~UPhysXInstancedWorldSubsystem() override;

	// ---------------------------------------------------------------------
	// Internal: process pipeline
	// ---------------------------------------------------------------------

	void BuildProcessPipeline();
	TUniquePtr<FPhysXISProcessManager> ProcessManager;

	// ---------------------------------------------------------------------
	// Internal: fast access & storage
	// ---------------------------------------------------------------------

	/** Cached owning world to avoid repeated GetWorld() calls in hot paths. */
	TWeakObjectPtr<UWorld> CachedWorld;

	/** Instance records, keyed by stable instance ID. */
	TMap<FPhysXInstanceID, FPhysXInstanceData> Instances;

	/** Actor records, keyed by stable actor ID. */
	TMap<FPhysXActorID, FPhysXActorData> Actors;

	/** Monotonic counters for issuing new IDs. */
	uint32 NextID      = 1;
	uint32 NextActorID = 1;

#if PHYSICS_INTERFACE_PHYSX
	FORCEINLINE void ApplyOwnerPhysicsOverrides(
		const APhysXInstancedMeshActor* OwnerActor,
		UInstancedStaticMeshComponent* ISMC,
		UStaticMesh* CollisionMeshUsed,
		physx::PxRigidDynamic* RD) const
	{
		if (!ISMC || !RD)
		{
			return;
		}

		// Use the same mesh that was used to build collision shapes for this body.
		UStaticMesh* MassMesh = CollisionMeshUsed ? CollisionMeshUsed : ISMC->GetStaticMesh();
		if (!MassMesh || !MassMesh->GetBodySetup())
		{
			return;
		}

		// UE PhysicalMaterial density is in g/cm^3.
		// Convert to kg/m^3 for PhysX: 1 g/cm^3 = 1000 kg/m^3.
		UPhysicalMaterial* PhysMat = MassMesh->GetBodySetup()->PhysMaterial;
		const float Density_g_per_cm3 = PhysMat ? PhysMat->Density : 1.0f;
		float Density_kg_per_m3 = FMath::Max(Density_g_per_cm3 * 1000.0f, 0.001f);

		// Optional: respect UE mass scale on the component (dimensionless).
		const float MassScale = FMath::Max(ISMC->BodyInstance.MassScale, KINDA_SMALL_NUMBER);
		Density_kg_per_m3 *= MassScale;

		// Recompute mass & inertia from shapes using density derived from the mesh.
		physx::PxRigidBodyExt::updateMassAndInertia(*RD, (physx::PxReal)Density_kg_per_m3);

		// If you want damping etc. to follow UE component defaults:
		RD->setLinearDamping((physx::PxReal)FMath::Max(0.0f, ISMC->BodyInstance.LinearDamping));
		RD->setAngularDamping((physx::PxReal)FMath::Max(0.0f, ISMC->BodyInstance.AngularDamping));

		// Gravity is already handled in your SetInstancePhysicsEnabled(),
		// but if you prefer to centralize it here you can:
		// if (OwnerActor) { RD->setActorFlag(physx::PxActorFlag::eDISABLE_GRAVITY, !OwnerActor->bInstancesUseGravity); }
	}
	
	static FORCEINLINE EPhysXInstanceConvertReason ConvertReasonFromRemoveReason(EPhysXInstanceRemoveReason R)
	{
		switch (R)
		{
		case EPhysXInstanceRemoveReason::Explicit: return EPhysXInstanceConvertReason::Explicit;
		case EPhysXInstanceRemoveReason::Expired:  return EPhysXInstanceConvertReason::Expired;
		case EPhysXInstanceRemoveReason::AutoStop: return EPhysXInstanceConvertReason::AutoStop;
		case EPhysXInstanceRemoveReason::KillZ:    return EPhysXInstanceConvertReason::AutoStop;
		case EPhysXInstanceRemoveReason::Lost:     return EPhysXInstanceConvertReason::AutoStop;
		default:                                   return EPhysXInstanceConvertReason::AutoStop;
		}
	}

#endif


	// ---------------------------------------------------------------------
	// Internal: lightweight lookup helpers
	// ---------------------------------------------------------------------

	FORCEINLINE FPhysXInstanceData* FindInstanceDataMutable(FPhysXInstanceID ID)
	{
		return Instances.Find(ID);
	}

	FORCEINLINE const FPhysXInstanceData* FindInstanceData(FPhysXInstanceID ID) const
	{
		return Instances.Find(ID);
	}

	FORCEINLINE bool IsValidIDValue(FPhysXInstanceID ID) const
	{
		return ID.IsValid();
	}

	// ---------------------------------------------------------------------
	// Internal: runtime counters (debug/stats)
	// ---------------------------------------------------------------------

	/** Total number of PhysX bodies ever created by this subsystem instance. */
	uint64 NumBodiesLifetimeCreated = 0;

	/** Total number of instance records tracked by the subsystem. */
	int32 NumBodiesTotal = 0;

	/** Number of instances that currently simulate (dynamic PhysX bodies). */
	int32 NumBodiesSimulating = 0;

	/** Number of simulating bodies that are currently sleeping (updated each tick). */
	int32 NumBodiesSleeping = 0;

	// ---------------------------------------------------------------------
	// Internal: physics-step apply batching
	// ---------------------------------------------------------------------

	struct FPhysicsStepTransformBatch
	{
		TArray<int32>      InstanceIndices;
		TArray<FTransform> WorldTransforms;
	};

	struct FPhysicsStepApplyContext
	{
		TSet<UInstancedStaticMeshComponent*> DirtyComponents;
		TMap<UPhysXInstancedStaticMeshComponent*, FPhysicsStepTransformBatch> ComponentBatches;

		FORCEINLINE void Reset(int32 ReserveCount)
		{
			DirtyComponents.Reset();
			ComponentBatches.Reset();

			DirtyComponents.Reserve(ReserveCount);
			ComponentBatches.Reserve(ReserveCount);
		}
	};

	bool  bPhysicsStepHasPendingApply = false;
	float PhysicsStepTimerDelta       = 0.0f;

	int32 PhysicsStepLocalTotal        = 0;
	int32 PhysicsStepLocalSleeping     = 0;

	FPhysicsStepApplyContext PhysicsStepApplyCtx;

	void PhysicsStep_Compute(float DeltaTime, float SimTime);
	void PhysicsStep_ApplyStopActionsAndCCD();
	void PhysicsStep_ApplyTransformSync();
	void PhysicsStep_Finalize();

	// ---------------------------------------------------------------------
	// Internal: scene insertion budget
	// ---------------------------------------------------------------------

	/**
	 * Max number of bodies to add to the PhysX scene per frame.
	 * 0 means "no limit".
	 *
	 * UPROPERTY can't be wrapped in PHYSICS_INTERFACE_PHYSX, so this stays unconditional.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Phys X Instance|Performance", meta = (ClampMin = "0"))
	int32 MaxAddActorsPerFrame = 64;

	// ---------------------------------------------------------------------
	// Internal: deferred instance tasks (forces/impulses/sleep/wake)
	// ---------------------------------------------------------------------

	enum class EPhysXInstanceTaskType : uint8
	{
		AddImpulse,
		AddForce,
		PutToSleep,
		WakeUp,

		Count
	};

	struct FPhysXInstanceTask
	{
		EPhysXInstanceTaskType Type = EPhysXInstanceTaskType::AddImpulse;
		FPhysXInstanceID       ID;

		/** AddImpulse/AddForce payload. */
		FVector Vector = FVector::ZeroVector;

		/** AddImpulse: bVelChange, AddForce: bAccelChange. */
		bool bModeFlag = false;

		/** Storage handling flags (matches Advanced API semantics). */
		bool bIncludeStorage         = true;
		bool bConvertStorageToDynamic = true;

		/** Retry counter for cases where the body isn't ready (e.g. not in scene yet). */
		int32 Attempts = 0;
	};

	/** Max number of queued instance tasks to execute per frame. 0 means "no limit". */
	UPROPERTY(EditAnywhere, Config, Category = "Phys X Instance|Performance", meta = (ClampMin = "0"))
	int32 MaxInstanceTasksPerFrame = 4096;

	/** FIFO queue for requested operations executed during Tick. */
	TArray<FPhysXInstanceTask> PendingInstanceTasks;

	void EnqueueInstanceTask(const FPhysXInstanceTask& Task);
	void ProcessInstanceTasks();

#if PHYSICS_INTERFACE_PHYSX
	bool TryExecuteInstanceTask(FPhysXInstanceTask& Task);
	bool HandleInstanceTask_AddImpulse(FPhysXInstanceTask& Task, physx::PxRigidDynamic* RD);
	bool HandleInstanceTask_AddForce(FPhysXInstanceTask& Task, physx::PxRigidDynamic* RD);
	bool HandleInstanceTask_PutToSleep(FPhysXInstanceTask& Task, physx::PxRigidDynamic* RD);
	bool HandleInstanceTask_WakeUp(FPhysXInstanceTask& Task, physx::PxRigidDynamic* RD);
#endif

	// ---------------------------------------------------------------------
	// Internal: lifetime (TTL)
	// ---------------------------------------------------------------------

	/** Max number of lifetime expirations processed per tick. 0 means "no limit". */
	UPROPERTY(EditAnywhere, Config, Category = "Phys X Instance|Lifetime", meta = (ClampMin = "0"))
	int32 MaxLifetimeExpirationsPerTick = 4096;

	struct FLifetimeHeapEntry
	{
		float            ExpireAt = 0.0f;
		FPhysXInstanceID ID;
		uint32           Serial = 0;
	};

	/** Min-heap predicate: the earliest ExpireAt is kept at the top. */
	struct FLifetimeHeapPred
	{
		FORCEINLINE bool operator()(const FLifetimeHeapEntry& A, const FLifetimeHeapEntry& B) const
		{
			if (A.ExpireAt != B.ExpireAt)
			{
				return A.ExpireAt < B.ExpireAt;
			}
			return A.ID.GetUniqueID() < B.ID.GetUniqueID();
		}
	};

	/** Min-heap of lifetime expirations. Stale entries are discarded lazily. */
	TArray<FLifetimeHeapEntry> LifetimeHeap;

	FORCEINLINE float GetWorldTimeSecondsSafe() const
	{
		if (UWorld* World = CachedWorld.Get())
		{
			return World->GetTimeSeconds();
		}
		if (UWorld* World = GetWorld())
		{
			return World->GetTimeSeconds();
		}
		return 0.0f;
	}

	void ApplyDefaultLifetimeForNewInstance(FPhysXInstanceID ID, UInstancedStaticMeshComponent* InstancedMesh);
	void ApplyLifetimeOverrideForNewInstance(FPhysXInstanceID ID, const FPhysXSpawnInstanceRequest& Request);
	void SetInstanceLifetime_Internal(FPhysXInstanceID ID, float NowSeconds, float LifetimeSeconds, EPhysXInstanceStopAction Action);
	void DisableInstanceLifetime_Internal(FPhysXInstanceID ID);
	void ProcessLifetimeExpirations();
	void ApplyLifetimeAction(FPhysXInstanceID ID, EPhysXInstanceStopAction Action);

	// ---------------------------------------------------------------------
	// Internal: stop actions (shared by async-step and TTL)
	// ---------------------------------------------------------------------

	struct FStopActionExecOptions
	{
		EPhysXInstanceRemoveReason RemoveReason = EPhysXInstanceRemoveReason::Expired;

		bool bRemoveVisualInstance       = true;
		bool bCreateStorageActorIfNeeded = true;

		/** If true, uses SetInstancePhysicsEnabled() when applying DisableSimulation/DestroyBody. */
		bool bUseSetInstancePhysicsEnabled = false;

		/** Resets per-instance stop timers after applying an action. */
		bool bResetTimers = true;

		/** If ConvertToStorage fails: destroys body to keep state consistent. */
		bool bDestroyBodyOnConvertFailure = true;
	};

	bool ExecuteInstanceStopAction_Internal(
		FPhysXInstanceID ID,
		EPhysXInstanceStopAction Action,
		const FStopActionExecOptions& Opt);

	bool HandleStopAction_None(
		FPhysXInstanceID ID,
		const FStopActionExecOptions& Opt,
		FPhysXInstanceData& Data);

	bool HandleStopAction_DisableSimulation(
		FPhysXInstanceID ID,
		const FStopActionExecOptions& Opt,
		FPhysXInstanceData& Data);

	bool HandleStopAction_DestroyBody(
		FPhysXInstanceID ID,
		const FStopActionExecOptions& Opt,
		FPhysXInstanceData& Data);

	bool HandleStopAction_DestroyBodyAndRemoveInstance(
		FPhysXInstanceID ID,
		const FStopActionExecOptions& Opt,
		FPhysXInstanceData& Data);

	bool HandleStopAction_ConvertToStorage(
		FPhysXInstanceID ID,
		const FStopActionExecOptions& Opt,
		FPhysXInstanceData& Data);

	// ---------------------------------------------------------------------
	// Internal: slot mapping (Component + InstanceIndex -> ID)
	// ---------------------------------------------------------------------

	struct FPhysXInstanceSlotKey
	{
		TWeakObjectPtr<UInstancedStaticMeshComponent> Component;
		int32 InstanceIndex = INDEX_NONE;

		FPhysXInstanceSlotKey() = default;

		FPhysXInstanceSlotKey(UInstancedStaticMeshComponent* InComp, int32 InIndex)
			: Component(InComp)
			, InstanceIndex(InIndex)
		{
		}

		bool operator==(const FPhysXInstanceSlotKey& Other) const
		{
			return Component == Other.Component && InstanceIndex == Other.InstanceIndex;
		}

		friend uint32 GetTypeHash(const FPhysXInstanceSlotKey& Key)
		{
			return HashCombine(::GetTypeHash(Key.Component.Get()), ::GetTypeHash(Key.InstanceIndex));
		}
	};

	TMap<FPhysXInstanceSlotKey, FPhysXInstanceID> InstanceIDBySlot;

	void AddSlotMapping(FPhysXInstanceID ID);
	void RemoveSlotMapping(FPhysXInstanceID ID);
	void RebuildSlotMappingForComponent(UInstancedStaticMeshComponent* ISMC);

	void FixInstanceIndicesAfterRemoval(UInstancedStaticMeshComponent* ISMC, int32 RemovedIndex);

	// ---------------------------------------------------------------------
	// Internal: removal
	// ---------------------------------------------------------------------

	bool RemoveInstanceByID_Internal(FPhysXInstanceID ID, bool bRemoveVisualInstance, EPhysXInstanceRemoveReason Reason);

	// ---------------------------------------------------------------------
	// Internal: PhysX backend
	// ---------------------------------------------------------------------

#if PHYSICS_INTERFACE_PHYSX

	struct FPhysXInstanceUserData;

	/** One allocation per ID, owned by the subsystem (stable address). */
	TMap<FPhysXInstanceID, FPhysXInstanceUserData*> UserDataByID;

	void EnsureInstanceUserData(FPhysXInstanceID ID);
	void ClearInstanceUserData(FPhysXInstanceID ID);
	FPhysXInstanceID GetInstanceIDFromPxActor(const physx::PxRigidActor* Actor) const;

	// -----------------------------------------------------------------
	// PhysX: pending scene adds
	// -----------------------------------------------------------------

	struct FPendingAddActorEntry
	{
		FPhysXInstanceID ID;
		TWeakObjectPtr<UInstancedStaticMeshComponent> InstancedComponent;

		/** Cached world pointer captured at enqueue time. */
		TWeakObjectPtr<UWorld> World;
	};

	TArray<FPendingAddActorEntry> PendingAddActors;
	int32 PendingAddActorsHead = 0;

	void EnqueueAddActorToScene(FPhysXInstanceID ID, UInstancedStaticMeshComponent* InstancedMesh);
	void ProcessPendingAddActors();

	/** Marks pending scene-add entries as invalid when an instance is removed or rebuilt. */
	void InvalidatePendingAddEntries(FPhysXInstanceID ID);

	// -----------------------------------------------------------------
	// PhysX: internal queries
	// -----------------------------------------------------------------

	bool RaycastPhysXInstanceID_Internal(
		const FVector& StartWorld,
		const FVector& EndWorld,
		FPhysXInstanceID& OutID,
		float& OutDistanceUU,
		FVector& OutHitPosWorld,
		FVector& OutHitNormalWorld) const;

	bool SweepSpherePhysXInstanceID_Internal(
		const FVector& StartWorld,
		const FVector& EndWorld,
		float Radius,
		FPhysXInstanceID& OutID,
		float& OutDistanceUU,
		FVector& OutHitPosWorld,
		FVector& OutHitNormalWorld) const;

#else

	/** No-op stub for non-PhysX builds; keeps shared codepaths clean. */
	void InvalidatePendingAddEntries(FPhysXInstanceID /*ID*/) {}

#endif // PHYSICS_INTERFACE_PHYSX

	// ---------------------------------------------------------------------
	// Friends: pipeline processes
	// ---------------------------------------------------------------------

	friend class PhysXIS::FPhysicsStepProcess;
	friend class PhysXIS::FLifetimeProcess;

#if PHYSICS_INTERFACE_PHYSX
	friend class PhysXIS::FAddActorsProcess;
	friend class PhysXIS::FInstanceTasksProcess;

	friend class PhysXIS::FPhysicsStepComputeProcess;
	friend class PhysXIS::FPhysicsStepStopActionsProcess;
	friend class PhysXIS::FPhysicsStepTransformSyncProcess;
	friend class PhysXIS::FPhysicsStepFinalizeProcess;
#endif
};

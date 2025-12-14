/**
* Copyright (C) 2025 | Created by NordVader Inc.
* All rights reserved!
* My Discord Server: https://discord.gg/B8prpf3vzD
*/

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Types/PhysXInstancedTypes.h"
#include "PhysXInstancedWorldSubsystem.generated.h"

class UInstancedStaticMeshComponent;
class APhysXInstancedMeshActor;

/**
 * World-level subsystem that owns all PhysX-backed instanced bodies.
 *
 * Core model:
 *  - APhysXInstancedMeshActor owns an instanced static mesh with many visual instances.
 *  - Each visual instance can optionally have a PhysX body.
 *  - This subsystem stores instance and actor records behind stable uint32-based IDs,
 *    so gameplay code can address them without holding raw pointers.
 */
UCLASS(Config = Game)
class PHYSXINSTANCEDSUBSYSTEM_API UPhysXInstancedWorldSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	UPhysXInstancedWorldSubsystem();

	// === UWorldSubsystem =====================================================

	/** Called once when the subsystem is created for a given world. */
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	/** Called when the world/subsystem is being torn down. */
	virtual void Deinitialize() override;

	// === UTickableWorldSubsystem ============================================

	/** Called every frame to sync PhysX poses back into ISM instances. */
	virtual void Tick(float DeltaTime) override;

	/** Required by the tick system for stat profiling. */
	virtual TStatId GetStatId() const override;

	// === High-level spawn ====================================================

	/**
	 * Create or reuse a PhysXInstancedMeshActor and spawn one visual ISM instance
	 * with an optional PhysX body.
	 *
	 * See FPhysXSpawnInstanceRequest / FPhysXSpawnInstanceResult for request/result details.
	 */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Spawn")
	FPhysXSpawnInstanceResult SpawnPhysicsInstance(const FPhysXSpawnInstanceRequest& Request);

	// === Registration ========================================================

	/**
	 * Register a single ISM instance in the subsystem.
	 *
	 * @param InstancedMesh  Owning instanced static mesh component.
	 * @param InstanceIndex  Index inside the ISM (0..NumInstances-1).
	 * @param bSimulate      If true, a PhysX body is created immediately and starts simulating.
	 *
	 * @return Stable handle used to control the instance later.
	 *         Returns an invalid ID (UniqueID == 0) on failure.
	 */
	FPhysXInstanceID RegisterInstance(
		UInstancedStaticMeshComponent* InstancedMesh,
		int32 InstanceIndex,
		bool bSimulate);

	/**
	 * Register multiple ISM instances in one batch.
	 *
	 * This path is intended for mass registration and may create PhysX bodies in parallel.
	 */
	void RegisterInstancesBatch(
		UInstancedStaticMeshComponent* InstancedMesh,
		const TArray<int32>& InstanceIndices,
		bool bSimulate,
		TArray<FPhysXInstanceID>& OutInstanceIDs);

	/**
	 * Remove an instance from the subsystem.
	 * If a PhysX body exists, it is destroyed as well.
	 */
	void UnregisterInstance(FPhysXInstanceID ID);

	// === Physics update ======================================================

	/**
	 * Step registered PhysX bodies and write their poses back into ISM instances.
	 *
	 * Normally called from Tick, but can be used to drive the update manually.
	 *
	 * @param DeltaTime Game time step.
	 * @param SimTime   Time step used by the physics scene (usually the same as DeltaTime).
	 */
	void AsyncPhysicsStep(float DeltaTime, float SimTime);

	// === Instance conversion =================================================

	/**
	 * Convert a dynamic PhysX-backed instance into a static ISM instance stored on a storage-only actor.
	 *
	 * The PhysX body is destroyed and the instance is removed from the subsystem.
	 * A new visual instance with collision is added to a storage actor that matches
	 * the source mesh and materials.
	 */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Conversion")
	bool ConvertInstanceToStaticStorage(FPhysXInstanceID ID, bool bCreateStorageActorIfNeeded = true);

	// === High-level physics control =========================================

	/**
	 * Enable or disable physics simulation for a specific instance.
	 *
	 * When bEnable == false and bDestroyBodyIfDisabling == true:
	 *  - The PhysX body is destroyed.
	 *  - The visual ISM instance remains where it is.
	 */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Physics")
	bool SetInstancePhysicsEnabled(FPhysXInstanceID ID, bool bEnable, bool bDestroyBodyIfDisabling = false);

	/**
	 * Check whether the instance is currently simulating physics.
	 *
	 * On non-PhysX builds this always returns false.
	 */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Physics")
	bool IsInstancePhysicsEnabled(FPhysXInstanceID ID) const;

	// === Forces / impulses ===================================================

	/**
	 * Add a world-space impulse to the instance.
	 * The impulse is applied at the body's center of mass.
	 *
	 * @param ID           Instance handle.
	 * @param WorldImpulse Impulse vector in Unreal world units.
	 * @param bVelChange   If true, ignores mass and treats the impulse as a direct velocity change.
	 */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Forces")
	bool AddImpulseToInstance(FPhysXInstanceID ID, FVector WorldImpulse, bool bVelChange = false);

	/**
	 * Add a continuous world-space force to the instance.
	 * The force is applied at the body's center of mass.
	 *
	 * @param ID           Instance handle.
	 * @param WorldForce   Force vector in Unreal world units.
	 * @param bAccelChange If true, treats the value as acceleration (independent of mass).
	 */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Forces")
	bool AddForceToInstance(FPhysXInstanceID ID, FVector WorldForce, bool bAccelChange = false);

	/** Put the instance's rigid body to sleep (if it exists and is dynamic). */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Forces")
	bool PutInstanceToSleep(FPhysXInstanceID ID);

	/** Wake the instance's rigid body so it starts simulating again. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Forces")
	bool WakeInstanceUp(FPhysXInstanceID ID);

	// === Per-instance physics properties ====================================

	/**
	 * Enable or disable gravity for a specific instance.
	 * Internally toggles PxActorFlag::eDISABLE_GRAVITY on the PhysX body.
	 */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Physics")
	bool SetInstanceGravityEnabled(FPhysXInstanceID ID, bool bEnable);

	/** Check if gravity is currently enabled for the instance. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Physics")
	bool IsInstanceGravityEnabled(FPhysXInstanceID ID) const;

	/** Set linear velocity for the instance (units per second). */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Physics")
	bool SetInstanceLinearVelocity(FPhysXInstanceID ID, FVector NewVelocity, bool bAutoWake = true);

	/** Read linear velocity of the instance. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Physics")
	bool GetInstanceLinearVelocity(FPhysXInstanceID ID, FVector& OutVelocity) const;

	/** Set angular velocity in radians per second (local space, same convention as PxRigidDynamic). */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Physics")
	bool SetInstanceAngularVelocityInRadians(FPhysXInstanceID ID, FVector NewAngVelRad, bool bAutoWake = true);

	/** Read angular velocity in radians per second. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Physics")
	bool GetInstanceAngularVelocityInRadians(FPhysXInstanceID ID, FVector& OutAngVelRad) const;

	// === Query helpers =======================================================

	/** Validity check: ID exists in the map and the ISM component is still alive. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Query")
	bool IsInstanceValid(FPhysXInstanceID ID) const;

	/**
	 * Resolve owning component and instance index for a given instance ID.
	 *
	 * @return true if the ID is known and the component is still valid.
	 */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Query")
	bool GetInstanceInfo(
		FPhysXInstanceID ID,
		UInstancedStaticMeshComponent*& OutComponent,
		int32& OutInstanceIndex) const;

	/** Collect all currently registered instance IDs. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Query")
	TArray<FPhysXInstanceID> GetAllInstanceIDs() const;

	/**
	 * Find the instance closest to the given world-space location.
	 *
	 * @param WorldLocation           Location in world space.
	 * @param OptionalFilterComponent If set, only instances belonging to this ISM are considered.
	 */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Query")
	FPhysXInstanceID FindNearestInstance(
		FVector WorldLocation,
		UInstancedStaticMeshComponent* OptionalFilterComponent = nullptr) const;

	/**
	 * Find an instance ID by its owning component and instance index.
	 *
	 * @return Valid ID on success, or invalid ID (UniqueID == 0) if no match is found.
	 */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Query")
	FPhysXInstanceID GetInstanceIDForComponentAndIndex(
		UInstancedStaticMeshComponent* InstancedMesh,
		int32 InstanceIndex) const;

	// === Actor-level registration & query ===================================

	/**
	 * Register a PhysXInstancedMeshActor and obtain a stable actor ID.
	 * Typically called from APhysXInstancedMeshActor::BeginPlay.
	 */
	UFUNCTION(BlueprintCallable, Category = "Phys X Actor|Registration")
	FPhysXActorID RegisterInstancedMeshActor(APhysXInstancedMeshActor* Actor);

	/** Remove an actor from the subsystem. Typically called from EndPlay. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Actor|Registration")
	void UnregisterInstancedMeshActor(FPhysXActorID ActorID);

	/** Check whether an actor ID is valid (actor still exists). */
	UFUNCTION(BlueprintPure, Category = "Phys X Actor|Query")
	bool IsActorValid(FPhysXActorID ActorID) const;

	/** Resolve an actor pointer from its ID. Returns nullptr if invalid or destroyed. */
	UFUNCTION(BlueprintPure, Category = "Phys X Actor|Query")
	APhysXInstancedMeshActor* GetActorByID(FPhysXActorID ActorID) const;

	/** Collect all currently registered actor IDs. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Actor|Query")
	TArray<FPhysXActorID> GetAllActorIDs() const;

	/**
	 * Collect all instance IDs that belong to a given actor.
	 * This walks the internal Instances map and filters by owner.
	 */
	UFUNCTION(BlueprintCallable, Category = "Phys X Actor|Query")
	TArray<FPhysXInstanceID> GetInstanceIDsForActor(FPhysXActorID ActorID) const;

	// === Stats / random helpers =============================================

	/** Total number of registered instances in this world (all actors combined). */
	UFUNCTION(BlueprintPure, Category = "Phys X Instance|Stats")
	int32 GetTotalInstanceCount() const;

	/** Count how many instances belong to a specific ISM component. */
	UFUNCTION(BlueprintPure, Category = "Phys X Instance|Stats")
	int32 GetInstanceCountForComponent(UInstancedStaticMeshComponent* Component) const;

	/**
	 * Pick a random registered instance.
	 *
	 * @param bOnlySimulating If true, only instances that currently simulate physics are considered.
	 */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Random")
	FPhysXInstanceID GetRandomInstanceID(bool bOnlySimulating = false) const;

	/**
	 * Pick a random instance belonging to a specific ISM component.
	 *
	 * @param Component       Only instances belonging to this component are considered.
	 * @param bOnlySimulating If true, only simulating instances are considered.
	 */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Random")
	FPhysXInstanceID GetRandomInstanceForComponent(
		UInstancedStaticMeshComponent* Component,
		bool bOnlySimulating = false) const;

	// === Performance tuning ==================================================

	/** Get current per-frame budget for adding new PhysX actors to the scene. */
	UFUNCTION(BlueprintPure, Category = "Phys X Instance|Performance")
	int32 GetMaxAddActorsPerFrame() const;

	/**
	 * Override per-frame budget for adding new PhysX actors to the scene.
	 * 0 means "no limit" (all pending bodies are added in a single frame).
	 */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Performance")
	void SetMaxAddActorsPerFrame(int32 NewMax);

private:
	// === Storage =============================================================

	/** Per-instance storage, keyed by FPhysXInstanceID (uint32 handle). */
	TMap<FPhysXInstanceID, FPhysXInstanceData> Instances;

	/** Per-actor storage, keyed by FPhysXActorID (uint32 handle). */
	TMap<FPhysXActorID, FPhysXActorData> Actors;

	/** Incrementing counter for instance IDs. */
	uint32 NextID = 1;

	/** Incrementing counter for actor IDs. */
	uint32 NextActorID = 1;

	// === Runtime counters ====================================================

	/** Total number of instance records tracked by the subsystem. */
	int32 NumBodiesTotal = 0;

	/** Number of instances that currently have a valid dynamic PhysX body and simulate. */
	int32 NumBodiesSimulating = 0;

	/** Number of dynamic bodies that are currently sleeping (updated each tick). */
	int32 NumBodiesSleeping = 0;

	// === Scene insertion budget =============================================

	/**
	 * Max number of bodies to add to the PhysX scene per frame.
	 * 0 means "no limit" (all pending bodies in a single frame).
	 *
	 * This value is loaded from config and can be changed at runtime.
	 *
	 * NOTE: UPROPERTY must not be placed inside preprocessor blocks, so this field
	 * remains outside of #if PHYSICS_INTERFACE_PHYSX.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Phys X Instance|Performance", meta = (ClampMin = "0"))
	int32 MaxAddActorsPerFrame = 64;

#if PHYSICS_INTERFACE_PHYSX

	// === Pending scene adds ==================================================

	/** Entry describing a pending "add actor to PhysX scene" request. */
	struct FPendingAddActorEntry
	{
		FPhysXInstanceID ID;
		TWeakObjectPtr<UInstancedStaticMeshComponent> InstancedComponent;
	};

	/** Queue of bodies that still need to be added to the PhysX scene. */
	TArray<FPendingAddActorEntry> PendingAddActors;

	/** Enqueue an instance for deferred insertion into the PhysX scene. */
	void EnqueueAddActorToScene(FPhysXInstanceID ID, UInstancedStaticMeshComponent* InstancedMesh);

	/** Process queued scene insertions respecting MaxAddActorsPerFrame. */
	void ProcessPendingAddActors();

#endif // PHYSICS_INTERFACE_PHYSX
};

/**
* Copyright (C) 2025 | Created by NordVader Inc.
* All rights reserved!
* My Discord Server: https://discord.gg/B8prpf3vzD
*/

#pragma once

#include "CoreMinimal.h"
#include "Components/InstancedStaticMeshComponent.h"

#include "PhysXInstancedTypes.generated.h"

namespace physx
{
	class PxRigidActor;
	class PxRigidDynamic;
	class PxScene;
	class PxMaterial;
}

class UWorld;
class UStaticMesh;
class UMaterialInterface;
class UInstancedStaticMeshComponent;
class APhysXInstancedMeshActor;

// ============================================================================
//  Public enums / configs
// ============================================================================

/** PhysX shape type for a single instance. */
UENUM(BlueprintType)
enum class EPhysXInstanceShapeType : uint8
{
	Box                UMETA(DisplayName = "Box"),
	Sphere             UMETA(DisplayName = "Sphere"),
	Capsule            UMETA(DisplayName = "Capsule"),
	Convex             UMETA(DisplayName = "Convex Mesh"),
	TriangleMeshStatic UMETA(DisplayName = "Triangle Mesh (Static/Kinematic Only)")
};

/** How continuous collision detection (CCD) is configured for instanced bodies. */
UENUM(BlueprintType)
enum class EPhysXInstanceCCDMode : uint8
{
	/** Do not enable CCD for instance bodies. */
	Off UMETA(DisplayName = "Off"),

	/** Enable CCD only for bodies that are actually simulating (dynamic). */
	Simulating UMETA(DisplayName = "Simulating bodies only"),

	/** Enable CCD automatically when the body's speed exceeds MinCCDVelocity. */
	AutoByVelocity UMETA(DisplayName = "Auto (by velocity)"),

	/** Always enable CCD for all created bodies. */
	All UMETA(DisplayName = "All bodies")
};

/** How the subsystem chooses which PhysXInstancedMeshActor to use for a new instance. */
UENUM(BlueprintType)
enum class EPhysXInstanceActorMode : uint8
{
	/** Always spawn a brand new APhysXInstancedMeshActor. */
	AlwaysCreateNew UMETA(DisplayName = "Always create new actor"),

	/**
	 * Reuse an existing APhysXInstancedMeshActor that matches the StaticMesh and
	 * the effective materials. If none is found, create a new one.
	 */
	FindOrCreateByMeshAndMats UMETA(DisplayName = "Find or create by mesh + materials"),

	/**
	 * Use an explicitly provided APhysXInstancedMeshActor from the request.
	 * The actor is expected to be already configured (mesh, materials, physics).
	 */
	UseExplicitActor UMETA(DisplayName = "Use explicit actor")
};

/** How we decide that a PhysX instance is considered "stopped". */
UENUM(BlueprintType)
enum class EPhysXInstanceStopCondition : uint8
{
	/** Use PhysX sleeping flag only (PxRigidDynamic::isSleeping()). */
	PhysXSleepFlag UMETA(DisplayName = "PhysX sleeping flag only"),

	/** Use velocity thresholds only (linear and angular speed). */
	VelocityThreshold UMETA(DisplayName = "Velocity thresholds only"),

	/** Consider stopped if either PhysX sleep flag OR velocity thresholds are satisfied. */
	SleepOrVelocity UMETA(DisplayName = "Sleep flag OR velocity"),

	/** Consider stopped only if BOTH sleep flag and velocity thresholds are satisfied. */
	SleepAndVelocity UMETA(DisplayName = "Sleep flag AND velocity")
};

/** What action to perform once an instance is considered "stopped". */
UENUM(BlueprintType)
enum class EPhysXInstanceStopAction : uint8
{
	/** Do nothing; only track that the instance is stopped. */
	None UMETA(DisplayName = "Do nothing"),

	/**
	 * Disable simulation for the body but keep it alive.
	 * Typically: make it kinematic or set DISABLE_SIMULATION.
	 */
	DisableSimulation UMETA(DisplayName = "Disable simulation (keep body)"),

	/**
	 * Destroy the PhysX body for this instance.
	 * The visual ISM instance remains where it is.
	 */
	DestroyBody UMETA(DisplayName = "Destroy body (keep instance)"),

	/**
	 * Destroy the PhysX body and also remove the ISM instance.
	 * This may require user-side handling of changed instance indices in the ISM component.
	 */
	DestroyBodyAndRemoveInstance UMETA(DisplayName = "Destroy body and remove instance"),

	/** Convert a dynamic instance into a static storage instance on a separate actor. */
	ConvertToStorage UMETA(DisplayName = "Convert to storage")
};

UENUM(BlueprintType)
enum class EPhysXInstancedQueryDebugMode : uint8
{
	None     UMETA(DisplayName = "None"),
	Basic    UMETA(DisplayName = "Basic"),
	Detailed UMETA(DisplayName = "Detailed"),
};

UENUM(BlueprintType, meta=(Bitflags, UseEnumValuesAsMaskValuesInEditor="true"))
enum class EPhysXInstanceEventFlags : uint8
{
	None        = 0,
	PreRemove   = 1 << 0,
	PostRemove  = 1 << 1,
	PreConvert  = 1 << 2,
	PostConvert = 1 << 3,
	PrePhysics  = 1 << 4,
	PostPhysics = 1 << 5,
	// Add more when needed.
};
ENUM_CLASS_FLAGS(EPhysXInstanceEventFlags);

UENUM(BlueprintType)
enum class EPhysXInstanceRemoveReason : uint8
{
	Explicit,   // User called RemoveInstance
	Expired,    // TTL
	AutoStop,   // Auto-stop rule triggered
	KillZ,      // Custom kill Z
	Lost,       // Any "lost instance" logic you use
};

UENUM(BlueprintType)
enum class EPhysXInstanceConvertReason : uint8
{
	Explicit,
	AutoStop,
	Expired,
};

UENUM(BlueprintType)
enum class EPhysXInstanceConvertDirection : uint8
{
	ToStorage,
	ToDynamic,
};

/**
 * Configuration for automatic "stop" handling of instances.
 * This can be owned by an actor and read by the subsystem for each instance.
 */
USTRUCT(BlueprintType)
struct FPhysXInstanceStopConfig
{
	GENERATED_BODY()

	/** Enable or disable automatic stop handling. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stop")
	bool bEnableAutoStop = false;

	/** Condition used to determine whether an instance is considered stopped. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stop")
	EPhysXInstanceStopCondition Condition = EPhysXInstanceStopCondition::PhysXSleepFlag;

	/** Linear speed threshold (cm/s) used by velocity-based conditions. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stop", meta = (ClampMin = "0.0"))
	float LinearSpeedThreshold = 5.0f;

	/** Angular speed threshold (deg/s) used by velocity-based conditions. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stop", meta = (ClampMin = "0.0"))
	float AngularSpeedThreshold = 5.0f;

	/** Time (seconds) the stop condition must remain true before the action fires. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stop", meta = (ClampMin = "0.0"))
	float MinStoppedTime = 0.5f;

	/** Action executed once the instance is considered stopped. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stop")
	EPhysXInstanceStopAction Action = EPhysXInstanceStopAction::DestroyBody;

	// --- Safety rules --------------------------------------------------------

	/**
	 * If enabled, an instance falling for longer than MaxFallTime (continuous time with negative Z velocity)
	 * will be stopped using Action even if the main stop condition is not satisfied.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stop|Safety")
	bool bUseMaxFallTime = false;

	/** Maximum continuous fall time (seconds) before forcing a stop. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stop|Safety",
		meta = (ClampMin = "0.0", EditCondition = "bUseMaxFallTime"))
	float MaxFallTime = 10.0f;

	/**
	 * If enabled, an instance that moves farther than MaxDistanceFromActor from its owning actor
	 * will be stopped using Action.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stop|Safety")
	bool bUseMaxDistanceFromActor = false;

	/** Maximum allowed distance (cm) from the owning actor before forcing a stop. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stop|Safety",
		meta = (ClampMin = "0.0", EditCondition = "bUseMaxDistanceFromActor"))
	float MaxDistanceFromActor = 50000.0f;
};

/**
 * Configuration for continuous collision detection (CCD) for instanced bodies.
 * This can be owned by an actor and read by the subsystem when creating bodies.
 */
USTRUCT(BlueprintType)
struct FPhysXInstanceCCDConfig
{
	GENERATED_BODY()

	/** CCD mode applied when creating PhysX bodies for instances. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CCD")
	EPhysXInstanceCCDMode Mode = EPhysXInstanceCCDMode::Off;

	/**
	 * Minimal linear speed (cm/s) at which CCD is enabled when Mode == AutoByVelocity.
	 * Below this value CCD may stay disabled to reduce cost.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CCD",
		meta = (ClampMin = "0.0", EditCondition = "Mode == EPhysXInstanceCCDMode::AutoByVelocity"))
	float MinCCDVelocity = 2000.0f;

	/**
	 * Optional upper speed (cm/s) used to clamp/scale velocity-based CCD logic.
	 * 0 means "no upper limit".
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CCD",
		meta = (ClampMin = "0.0", EditCondition = "Mode == EPhysXInstanceCCDMode::AutoByVelocity"))
	float MaxCCDVelocity = 0.0f;
};

// ============================================================================
//  Stable IDs (Blueprint-facing)
// ============================================================================

/**
 * Lightweight handle for an instance.
 *
 * This is a small wrapper around an internal uint32 key.
 */
USTRUCT(BlueprintType)
struct FPhysXInstanceID
{
	GENERATED_BODY()

private:
	/** Underlying numeric ID. 0 means "invalid". */
	UPROPERTY()
	uint32 UniqueID;

public:
	FPhysXInstanceID()
		: UniqueID(0)
	{
	}

	explicit FPhysXInstanceID(uint32 InID)
		: UniqueID(InID)
	{
	}

	/** Return the raw numeric ID. */
	FORCEINLINE uint32 GetUniqueID() const { return UniqueID; }

	/** Set the raw numeric ID. Intended for subsystem use. */
	FORCEINLINE void SetUniqueID(uint32 InID) { UniqueID = InID; }

	/** Validity check: zero means "no instance". */
	FORCEINLINE bool IsValid() const { return UniqueID != 0u; }


	bool operator==(const FPhysXInstanceID& Other) const
	{
		return UniqueID == Other.UniqueID;
	}

	bool operator!=(const FPhysXInstanceID& Other) const
	{
		return UniqueID != Other.UniqueID;
	}
};

/** Hash function so FPhysXInstanceID can be used as a TMap key. */
FORCEINLINE uint32 GetTypeHash(const FPhysXInstanceID& ID)
{
	return ID.GetUniqueID();
}

/**
 * Actor-level handle used by the subsystem.
 * Identifies a PhysXInstancedMeshActor using a uint32 ID.
 */
USTRUCT(BlueprintType)
struct FPhysXActorID
{
	GENERATED_BODY()

private:
	/** Underlying numeric ID. 0 means "invalid". */
	UPROPERTY()
	uint32 UniqueID = 0;

public:
	FPhysXActorID() = default;

	explicit FPhysXActorID(uint32 InUniqueID)
		: UniqueID(InUniqueID)
	{
	}

	/** Validity check: zero means "no actor". */
	bool IsValid() const
	{
		return UniqueID != 0u;
	}

	/** Return the raw numeric ID. */
	uint32 GetUniqueID() const
	{
		return UniqueID;
	}

	/** Set the raw numeric ID. Intended for subsystem use. */
	void SetUniqueID(uint32 InUniqueID)
	{
		UniqueID = InUniqueID;
	}

	bool operator==(const FPhysXActorID& Other) const
	{
		return UniqueID == Other.UniqueID;
	}

	bool operator!=(const FPhysXActorID& Other) const
	{
		return UniqueID != Other.UniqueID;
	}
};

/** Hash function so FPhysXActorID can be used as a TMap key. */
FORCEINLINE uint32 GetTypeHash(const FPhysXActorID& ID)
{
	return ID.GetUniqueID();
}

// ============================================================================
//  Spawn request / result (Blueprint-facing)
// ============================================================================

/**
 * Request for spawning a single PhysX-driven instance via the subsystem.
 *
 * High-level model:
 *  - Optionally specify a StaticMesh and override materials.
 *  - Choose how the owner actor is selected/created (ActorMode).
 *  - Provide desired world-space transform and optional initial velocities.
 */
USTRUCT(BlueprintType)
struct FPhysXSpawnInstanceRequest
{
	GENERATED_BODY()

	/** How the subsystem should choose or create the owning actor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawn")
	EPhysXInstanceActorMode ActorMode = EPhysXInstanceActorMode::FindOrCreateByMeshAndMats;

	/**
	 * Static mesh used for rendering.
	 * Required for AlwaysCreateNew and FindOrCreateByMeshAndMats. Ignored for UseExplicitActor.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawn")
	UStaticMesh* StaticMesh = nullptr;

	/**
	 * If true and OverrideMaterials is not empty, those materials are used instead of StaticMesh materials.
	 * In FindOrCreateByMeshAndMats mode this also participates in actor matching.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawn")
	bool bUseOverrideMaterials = false;

	/** Material overrides per slot when bUseOverrideMaterials is true. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawn", meta = (EditCondition = "bUseOverrideMaterials"))
	TArray<UMaterialInterface*> OverrideMaterials;

	/**
	 * Explicit actor used when ActorMode == UseExplicitActor.
	 * The actor is expected to already have mesh/materials configured.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawn")
	TWeakObjectPtr<APhysXInstancedMeshActor> ExplicitActor;

	/**
	 * Desired world-space transform for the new instance.
	 * Internally converted to actor-space relative transform for ISM insertion.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawn")
	FTransform InstanceWorldTransform = FTransform::Identity;

	/**
	 * If true, the new PhysX body starts simulating immediately
	 * (subject to actor-level settings such as bSimulateInstances).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawn")
	bool bStartSimulating = true;

	/** Optional initial linear velocity (cm/s) in world space. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawn")
	FVector InitialLinearVelocity = FVector::ZeroVector;

	/** Optional initial angular velocity (radians/s) in world space. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawn")
	FVector InitialAngularVelocityRad = FVector::ZeroVector;
	
	// --- Lifetime override ---------------------------------------------------

	/** If true, overrides actor default lifetime settings for this spawn. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawn|Lifetime")
	bool bOverrideLifetime = false;

	/** Lifetime in seconds starting from spawn time. 0 disables lifetime for this instance. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawn|Lifetime",
		meta = (ClampMin = "0.0", EditCondition = "bOverrideLifetime"))
	float LifeTimeSeconds = 0.0f;

	/** Action executed when lifetime expires (only used when bOverrideLifetime is true). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawn|Lifetime",
		meta = (EditCondition = "bOverrideLifetime"))
	EPhysXInstanceStopAction LifetimeAction = EPhysXInstanceStopAction::DestroyBody;

};

/** Result of a SpawnPhysicsInstance() call. */
USTRUCT(BlueprintType)
struct FPhysXSpawnInstanceResult
{
	GENERATED_BODY()

	/** True if the instance was successfully spawned and registered. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Result")
	bool bSuccess = false;

	/** Owning actor that contains the visual ISM instance. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Result")
	APhysXInstancedMeshActor* Actor = nullptr;

	/** Index inside the InstancedMesh component on the owning actor. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Result")
	int32 InstanceIndex = INDEX_NONE;

	/** Handle of the instance registered in the subsystem. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Result")
	FPhysXInstanceID InstanceID;

	/** World-space transform that was finally applied to the ISM instance. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Result")
	FTransform FinalWorldTransform = FTransform::Identity;
};

// ============================================================================
//  Internal runtime data (subsystem-owned, not exposed to reflection)
// ============================================================================

/**
 * Thin wrapper over a PhysX rigid body for a single ISM instance.
 * This is intentionally not a USTRUCT (no reflection needed).
 */
struct FPhysXInstanceBody
{
#if PHYSICS_INTERFACE_PHYSX
	/** Underlying PhysX rigid dynamic body. */
	physx::PxRigidDynamic* PxBody = nullptr;
#else
	/** Dummy pointer for non-PhysX builds. */
	void* PxBody = nullptr;
#endif

	FPhysXInstanceBody() = default;
	~FPhysXInstanceBody() = default; // Destroy() is called explicitly, not from the destructor.

	/**
	 * Create a PhysX body for a specific ISM instance.
	 *
	 * @param InstancedMesh          Owning instanced static mesh component.
	 * @param InstanceIndex          Instance index inside the component.
	 * @param bSimulate              Whether the created body should simulate (dynamic) or be kinematic.
	 * @param DefaultMaterial        PhysX material used when no per-shape material is available.
	 * @param ShapeType              Collision shape type to build for this instance.
	 * @param OverrideCollisionMesh  Optional mesh used for convex/triangle collision generation.
	 */
	bool CreateFromInstancedStaticMesh(
		UInstancedStaticMeshComponent* InstancedMesh,
		int32 InstanceIndex,
		bool bSimulate,
		physx::PxMaterial* DefaultMaterial,
		EPhysXInstanceShapeType ShapeType,
		UStaticMesh* OverrideCollisionMesh);

	/** Destroy the underlying PhysX body and release associated resources. */
	void Destroy();

	/** Add the created rigid body to the PhysX scene associated with the given world. */
	void AddActorToScene(UWorld* World);

	/** Get the underlying PhysX actor pointer (rigid dynamic as rigid actor). */
	physx::PxRigidActor* GetPxActor() const;
};

/**
 * Internal data for a single registered ISM instance.
 * Stores ownership, index mapping, and bookkeeping needed by the subsystem.
 */
struct FPhysXInstanceData
{
	/** Owning ISM component stored as a weak pointer to avoid GC issues. */
	TWeakObjectPtr<UInstancedStaticMeshComponent> InstancedComponent;

	/** Index inside the ISM (0..NumInstances-1). */
	int32 InstanceIndex = INDEX_NONE;

	/** PhysX body wrapper for this instance. May be null if the body is not present. */
	FPhysXInstanceBody Body;

	/**
	 * Bookkeeping flag indicating whether this instance is expected to be simulating.
	 * The authoritative state is stored on the PhysX actor when available.
	 */
	bool bSimulating = false;

	/**
	 * Cached PhysX sleeping flag from the previous frame.
	 * Used to detect sleep transitions and reduce unnecessary transform updates.
	 */
	bool bWasSleeping = false;

	/** Accumulated time (seconds) while the instance is considered "stopped". */
	float SleepTime = 0.0f;

	/** Accumulated continuous fall time (seconds) while velocity Z is negative. */
	float FallTime = 0.0f;

	FPhysXInstanceData() = default;
	
	// --- Lifetime (TTL) ------------------------------------------------------

	/** True if this instance has an active lifetime timer. */
	bool bHasLifetime = false;

	/** Absolute world time (GetTimeSeconds) when this instance should expire. */
	float ExpireAt = 0.0f;

	/** Action executed when the instance expires. */
	EPhysXInstanceStopAction LifetimeAction = EPhysXInstanceStopAction::None;

	/**
	 * Monotonic serial used to invalidate stale heap entries when lifetime is updated.
	 * Incremented each time lifetime state changes.
	 */
	uint32 LifetimeSerial = 0;
};

/** Runtime info about a PhysXInstancedMeshActor stored by the subsystem. */
struct FPhysXActorData
{
	/** Weak pointer to the actor so it does not prevent GC. */
	TWeakObjectPtr<APhysXInstancedMeshActor> Actor;

	FPhysXActorData() = default;
};

// ============================================================================
//  Multithreaded evaluation helpers (internal)
// ============================================================================

/**
 * Read-only snapshot used when evaluating a single instance on worker threads.
 *
 * Threading model:
 *  - Worker threads never touch UObjects directly.
 *  - Required actor/config data is copied on the game thread into this struct.
 *  - PhysX pointers are read-only in worker threads.
 */
struct FPhysXInstanceParallelEntry
{
	/** Stable handle of the instance inside the subsystem. */
	FPhysXInstanceID ID;

	/** Pointer to instance runtime data owned by the subsystem. */
	FPhysXInstanceData* InstanceData = nullptr;

	/** Owning ISM component for the visual instance (game-thread only). */
	UInstancedStaticMeshComponent* InstancedComponent = nullptr;

#if PHYSICS_INTERFACE_PHYSX
	/** Underlying PhysX rigid body (dynamic). Read-only in worker threads. */
	physx::PxRigidDynamic* RigidDynamic = nullptr;
#else
	/** Dummy pointer for non-PhysX builds. */
	void* RigidDynamic = nullptr;
#endif

	// --- Cached actor-level configuration -----------------------------------

	/** Auto-stop configuration copied from the owning actor, if any. */
	FPhysXInstanceStopConfig StopConfig;

	/** True if StopConfig was populated from an owning actor. */
	bool bHasStopConfig = false;

	/** Whether a custom KillZ is used for this actor. */
	bool bUseCustomKillZ = false;

	/** Custom world-space KillZ value (only valid if bUseCustomKillZ is true). */
	float CustomKillZ = 0.0f;

	/** Action used when an instance is considered "lost" (e.g., fell below CustomKillZ). */
	EPhysXInstanceStopAction LostInstanceAction = EPhysXInstanceStopAction::None;

	/** Cached actor world location for max-distance checks. */
	FVector OwnerLocation = FVector::ZeroVector;
};

/**
 * Result of parallel evaluation for a single instance.
 *
 * Worker threads fill this struct based on read-only data from PhysX and the parallel entry.
 * The game thread later applies results back to UObjects and the PhysX scene.
 */
struct FPhysXInstanceParallelResult
{
	/** True if the instance was valid and processed this frame. */
	bool bValid = false;

	/** Whether the instance was marked as simulating before this frame. */
	bool bWasSimulating = false;

	/** PhysX sleeping flag at the time of evaluation. */
	bool bIsSleeping = false;

	/** Latest world-space transform read from PhysX. */
	FTransform WorldTransform = FTransform::Identity;

	/** Linear speed magnitude (cm/s). */
	float LinearSpeed = 0.0f;

	/** Angular speed magnitude (deg/s). */
	float AngularSpeedDeg = 0.0f;

	/** Updated accumulated stop time (seconds). */
	float NewSleepTime = 0.0f;

	/** Updated accumulated fall time (seconds) while velocity Z < 0. */
	float NewFallTime = 0.0f;

	// --- KillZ / auto-stop decisions ----------------------------------------

	/** True if the instance triggered custom KillZ this frame. */
	bool bKillZTriggered = false;

	/** True if an auto-stop config was present for this instance. */
	bool bHasAutoStopConfig = false;

	/** Shortcut for StopConfig.bEnableAutoStop && StopConfig.Action != None. */
	bool bAutoStopEnabled = false;

	/** True if the main stop condition is satisfied on this evaluation step. */
	bool bStopConditionNow = false;

	/**
	 * True if the stop condition has been satisfied for at least MinStoppedTime seconds
	 * and the stop action should fire.
	 */
	bool bReachedMinStoppedTime = false;
};

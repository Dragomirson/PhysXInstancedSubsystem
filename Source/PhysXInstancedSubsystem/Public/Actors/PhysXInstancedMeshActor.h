/**
* Copyright (C) 2025 | Created by NordVader Inc.
* All rights reserved!
* My Discord Server: https://discord.gg/B8prpf3vzD
*/

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "Engine/EngineTypes.h"            // ECollisionEnabled, FCollisionResponse, etc.
#include "Engine/CollisionProfile.h"       // FCollisionProfileName

#include "Types/PhysXInstancedTypes.h"     // FPhysXInstanceID, FPhysXActorID, configs, enums

#include "PhysXInstancedMeshActor.generated.h"

class USceneComponent;
class UStaticMesh;
class UMaterialInterface;
class UPhysXInstancedStaticMeshComponent;
class UPhysXInstancedWorldSubsystem;
struct FPropertyChangedEvent;

/** How instance transforms are generated for this actor. */
UENUM(BlueprintType)
enum class EPhysXInstanceSpawnMode : uint8
{
	Manual UMETA(DisplayName = "Manual"),
	Grid2D UMETA(DisplayName = "Grid (Rows x Columns x Layers)")
};

/**
 * Actor that owns a PhysX-driven instanced static mesh.
 *
 * Responsibilities:
 *  - Owns a UPhysXInstancedStaticMeshComponent for rendering.
 *  - Generates instance transforms (manual list or grid).
 *  - Optionally registers instances in the PhysX instanced subsystem on BeginPlay.
 *  - Exposes runtime Blueprint API to query and control instance physics.
 */
UCLASS(
	ClassGroup = (PhysX),
	BlueprintType,
	Blueprintable,
	hideCategories = (
		Input,
		Replication,
		Actor,
		Tags,
		Activation,
		Instances,
		Physics,
		LOD,
		Cooking,
		HLOD,
		WorldPartition,
		DataLayers
	),
	showCategories = ("Rendering", "Phys X Instance")
)
class APhysXInstancedMeshActor : public AActor
{
	GENERATED_BODY()

	/** Subsystem is allowed to access internal runtime fields. */
	friend class UPhysXInstancedWorldSubsystem;

public:
	APhysXInstancedMeshActor();

	// === Components ==========================================================

	/** Scene root to move/rotate the whole group of instances. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Rendering")
	USceneComponent* SceneRoot;

	/** Render-only instanced mesh component (physics is handled by the subsystem). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Rendering")
	UPhysXInstancedStaticMeshComponent* InstancedMesh;

	// ========================================================================
	//   Phys X Instance
	// ========================================================================

	// --- Rendering -----------------------------------------------------------

	/** Static mesh used by the instanced mesh component. */
	UPROPERTY(EditAnywhere, Category = "Phys X Instance", meta = (DisplayName = "Static Mesh"))
	UStaticMesh* InstanceStaticMesh;

	/** If true, InstanceOverrideMaterials are used instead of the mesh materials. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Phys X Instance|Rendering")
	bool bOverrideInstanceMaterials = false;

	/** Whether instances owned by this actor should cast shadows. */
	UPROPERTY(EditAnywhere, Category = "Phys X Instance|Rendering")
	bool bInstancesCastShadow = true;

	/** Override materials applied per material slot when bOverrideInstanceMaterials is true. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Phys X Instance|Rendering", meta = (EditCondition = "bOverrideInstanceMaterials"))
	TArray<UMaterialInterface*> InstanceOverrideMaterials;

	// --- Instance generation -------------------------------------------------

	/** If true, instances are registered in the subsystem automatically on BeginPlay. */
	UPROPERTY(EditAnywhere, Category = "Phys X Instance")
	bool bAutoRegisterOnBeginPlay;

	/** How instance transforms are generated (manual list vs. grid). */
	UPROPERTY(EditAnywhere, Category = "Phys X Instance")
	EPhysXInstanceSpawnMode SpawnMode;

	/**
	 * Per-instance transforms in actor space used in Manual mode.
	 * In Grid mode this array is generated automatically at runtime.
	 */
	UPROPERTY(EditAnywhere, Category = "Phys X Instance", meta = (EditCondition = "SpawnMode==EPhysXInstanceSpawnMode::Manual"))
	TArray<FTransform> InstanceRelativeTransforms;

	/**
	 * Additional offset for collision shapes relative to the rendered mesh.
	 * Applied when building PhysX shapes for instances.
	 */
	UPROPERTY(EditAnywhere, Category = "Phys X Instance")
	FTransform ShapeCollisionOffset;

	// --- Grid settings -------------------------------------------------------

	/** Number of rows in the grid (X axis in actor space). */
	UPROPERTY(EditAnywhere, Category = "Phys X Instance", meta = (ClampMin = "1", EditCondition = "SpawnMode==EPhysXInstanceSpawnMode::Grid2D"))
	int32 GridRows;

	/** Number of columns in the grid (Y axis in actor space). */
	UPROPERTY(EditAnywhere, Category = "Phys X Instance", meta = (ClampMin = "1", EditCondition = "SpawnMode==EPhysXInstanceSpawnMode::Grid2D"))
	int32 GridColumns;

	/** Number of layers in the grid (Z axis in actor space). */
	UPROPERTY(EditAnywhere, Category = "Phys X Instance", meta = (ClampMin = "1", EditCondition = "SpawnMode==EPhysXInstanceSpawnMode::Grid2D"))
	int32 GridLayers;

	/** Distance between instances along X. */
	UPROPERTY(EditAnywhere, Category = "Phys X Instance", meta = (ClampMin = "0.0", EditCondition = "SpawnMode==EPhysXInstanceSpawnMode::Grid2D"))
	float GridSpacingX;

	/** Distance between instances along Y. */
	UPROPERTY(EditAnywhere, Category = "Phys X Instance", meta = (ClampMin = "0.0", EditCondition = "SpawnMode==EPhysXInstanceSpawnMode::Grid2D"))
	float GridSpacingY;

	/** Distance between layers along Z. */
	UPROPERTY(EditAnywhere, Category = "Phys X Instance", meta = (ClampMin = "0.0", EditCondition = "SpawnMode==EPhysXInstanceSpawnMode::Grid2D"))
	float GridSpacingZ;

	/** Whether to center the grid in XY around the actor origin. */
	UPROPERTY(EditAnywhere, Category = "Phys X Instance", meta = (EditCondition = "SpawnMode==EPhysXInstanceSpawnMode::Grid2D"))
	bool bCenterGridXY;

	/** Whether to center the grid along Z around the actor origin. */
	UPROPERTY(EditAnywhere, Category = "Phys X Instance", meta = (EditCondition = "SpawnMode==EPhysXInstanceSpawnMode::Grid2D"))
	bool bCenterGridZ;

	// --- Physics behaviour ---------------------------------------------------

	/** If true, PhysX instances simulate; otherwise they are kinematic. */
	UPROPERTY(EditAnywhere, Category = "Phys X Instance")
	bool bSimulateInstances;

	/** If true, gravity is enabled for PhysX bodies created for this actor. */
	UPROPERTY(EditAnywhere, Category = "Phys X Instance")
	bool bInstancesUseGravity;

	/** If true, mass is overridden using InstanceMassInKg. */
	UPROPERTY(EditAnywhere, Category = "Phys X Instance")
	bool bOverrideInstanceMass;

	/** Mass (in kilograms) used when bOverrideInstanceMass is true. */
	UPROPERTY(EditAnywhere, Category = "Phys X Instance", meta = (EditCondition = "bOverrideInstanceMass", ClampMin = "0.0"))
	float InstanceMassInKg;

	/** Linear damping applied to bodies created for this actor. */
	UPROPERTY(EditAnywhere, Category = "Phys X Instance", meta = (ClampMin = "0.0"))
	float InstanceLinearDamping;

	/** Angular damping applied to bodies created for this actor. */
	UPROPERTY(EditAnywhere, Category = "Phys X Instance", meta = (ClampMin = "0.0"))
	float InstanceAngularDamping;

	/**
	 * If true, this actor is used only as a static storage of ISM instances
	 * and does not create PhysX bodies.
	 */
	UPROPERTY(EditAnywhere, Category = "Phys X Instance")
	bool bStorageOnly = false;

	/**
	 * If true, native physics/collision on InstancedMesh is disabled so the render ISM
	 * does not participate in physics. Only subsystem-created PhysX bodies are used.
	 */
	UPROPERTY(EditAnywhere, Category = "Phys X Instance")
	bool bDisableISMPhysics;

	/** Destroy a PhysX body when disabling simulation via this actor. */
	UPROPERTY(EditAnywhere, Category = "Phys X Instance")
	bool bDestroyBodyOnDisable;

	// --- Navigation ----------------------------------------------------------

	/** Whether dynamic (non-storage) instances affect navigation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Phys X Instance|Navigation")
	bool bDynamicInstancesAffectNavigation = false;

	/** Whether storage instances affect navigation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Phys X Instance|Navigation")
	bool bStorageInstancesAffectNavigation = true;

	/** Whether regular instances affect navigation (applies to non-storage instances). */
	UPROPERTY(EditAnywhere, Category = "Phys X Instance|Navigation")
	bool bInstancesAffectNavigation;

	// --- Collision presets ---------------------------------------------------

	/**
	 * Collision profile used when creating PhysX bodies for dynamic instances
	 * (and for the ISM BodyInstance when needed).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Phys X Instance",
		meta = (DisplayName = "Instances Collision Profile", ProfileName = "BlockAllDynamic"))
	FCollisionProfileName InstancesCollisionProfile;

	// --- Storage instances ---------------------------------------------------

	/**
	 * Indicates that this actor is a storage actor created/managed by the subsystem.
	 * Storage actors are used as static ISM holders (no PhysX bodies).
	 */
	UPROPERTY(VisibleAnywhere, Category = "Phys X Instance|Storage")
	bool bIsStorageActor = false;

	/** Collision profile for storage-only instances (static ISM, no PhysX bodies). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Phys X Instance",
		meta = (DisplayName = "Storage Collision Profile", ProfileName = "BlockAllDynamic"))
	FCollisionProfileName StorageCollisionProfile;

	/** Collision enabled state for storage-only instances. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Phys X Instance")
	TEnumAsByte<ECollisionEnabled::Type> StorageCollisionEnabled = ECollisionEnabled::QueryAndPhysics;

	// --- Collision shape -----------------------------------------------------

	/** Collision shape type used for each PhysX instance. */
	UPROPERTY(EditAnywhere, Category = "Phys X Instance")
	EPhysXInstanceShapeType InstanceShapeType;

	/**
	 * Optional custom mesh used for collision instead of the rendering mesh.
	 * Used for Convex and TriangleMeshStatic shapes.
	 */
	UPROPERTY(EditAnywhere, Category = "Phys X Instance",
		meta = (EditCondition =
			"InstanceShapeType==EPhysXInstanceShapeType::Convex || InstanceShapeType==EPhysXInstanceShapeType::TriangleMeshStatic",
			EditConditionHides))
	UStaticMesh* OverrideCollisionMesh;

	// --- Runtime behaviour ---------------------------------------------------

	/**
	 * Automatic stop handling for instances owned by this actor.
	 * Read by the subsystem during async physics updates.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Phys X Instance|Runtime")
	FPhysXInstanceStopConfig AutoStopConfig;

	/**
	 * Continuous collision detection (CCD) settings for instance bodies created by this actor.
	 * Read by the subsystem when spawning PxRigidDynamic.
	 */
	UPROPERTY(EditAnywhere, Category = "Phys X Instance|CCD")
	FPhysXInstanceCCDConfig CCDConfig;

	/** If enabled, instances owned by this actor use CustomKillZ checks. */
	UPROPERTY(EditAnywhere, Category = "Phys X Instance|Bounds")
	bool bUseCustomKillZ = false;

	/** World-space Z below which an instance is considered lost (only if bUseCustomKillZ is true). */
	UPROPERTY(EditAnywhere, Category = "Phys X Instance|Bounds", meta = (EditCondition = "bUseCustomKillZ"))
	float CustomKillZ = -100000.0f;

	/** Action applied to instances that fall below CustomKillZ. */
	UPROPERTY(EditAnywhere, Category = "Phys X Instance|Bounds", meta = (EditCondition = "bUseCustomKillZ"))
	EPhysXInstanceStopAction LostInstanceAction = EPhysXInstanceStopAction::DestroyBody;

	// === Auto-stop configuration (runtime API) ===============================

	/** Enable or disable automatic auto-stop logic at runtime. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Runtime")
	void SetAutoStopEnabled(bool bEnable);

	/** Configure main auto-stop condition and thresholds at runtime. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Runtime")
	void ConfigureAutoStopBasic(
		EPhysXInstanceStopCondition NewCondition,
		float NewLinearSpeedThreshold,
		float NewAngularSpeedThreshold,
		float NewMinStoppedTime
	);

	/** Choose what to do when an instance is considered stopped. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Runtime")
	void SetAutoStopAction(EPhysXInstanceStopAction NewAction);

	/** Configure extra safety rules (fall time / max distance) at runtime. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Runtime")
	void ConfigureAutoStopSafety(
		bool bInUseMaxFallTime,
		float NewMaxFallTime,
		bool bInUseMaxDistanceFromActor,
		float NewMaxDistanceFromActor
	);

	/** Get current auto-stop configuration. */
	UFUNCTION(BlueprintPure, Category = "Phys X Instance|Runtime")
	FPhysXInstanceStopConfig GetAutoStopConfig() const { return AutoStopConfig; }

	// === Runtime physics control (index based) ==============================

	/** Enable or disable physics for a specific instance index in InstancedMesh. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Runtime")
	void SetInstancePhysicsEnabled(int32 InstanceIndex, bool bEnable);

	/** Enable or disable physics for all instances owned by this actor. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Runtime")
	void SetAllInstancesPhysicsEnabled(bool bEnable);

	/** Check if physics is enabled for a specific instance index. */
	UFUNCTION(BlueprintPure, Category = "Phys X Instance|Runtime")
	bool IsInstancePhysicsEnabledByIndex(int32 InstanceIndex) const;

	/** Disable physics for an instance and force destruction of its PhysX body. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Runtime")
	void DisableInstanceAndDestroyBody(int32 InstanceIndex);

	/** Enable or disable gravity for a specific instance index. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Runtime")
	void SetInstanceGravityEnabledByIndex(int32 InstanceIndex, bool bEnable);

	/** Check if gravity is enabled for a specific instance index. */
	UFUNCTION(BlueprintPure, Category = "Phys X Instance|Runtime")
	bool IsInstanceGravityEnabledByIndex(int32 InstanceIndex) const;

	/** Set linear velocity for a specific instance index. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Runtime")
	void SetInstanceLinearVelocityByIndex(int32 InstanceIndex, FVector NewVelocity, bool bAutoWake = true);

	/** Read linear velocity for a specific instance index. */
	UFUNCTION(BlueprintPure, Category = "Phys X Instance|Runtime")
	bool GetInstanceLinearVelocityByIndex(int32 InstanceIndex, FVector& OutVelocity) const;

	/** Set angular velocity (radians per second) for a specific instance index. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Runtime")
	void SetInstanceAngularVelocityByIndex(int32 InstanceIndex, FVector NewAngVelRad, bool bAutoWake = true);

	/** Read angular velocity (radians per second) for a specific instance index. */
	UFUNCTION(BlueprintPure, Category = "Phys X Instance|Runtime")
	bool GetInstanceAngularVelocityByIndex(int32 InstanceIndex, FVector& OutAngVelRad) const;

	// === Runtime helpers (IDs / counts) =====================================

	/** Number of visual ISM instances owned by this actor. */
	UFUNCTION(BlueprintPure, Category = "Phys X Instance|Runtime")
	int32 GetInstanceCount() const;

	/**
	 * Get the PhysX instance ID corresponding to an InstancedMesh instance index.
	 * Returns an invalid ID (UniqueID == 0) if the index is out of range or not registered.
	 */
	UFUNCTION(BlueprintPure, Category = "Phys X Instance|Runtime")
	FPhysXInstanceID GetInstanceIDByIndex(int32 InstanceIndex) const;

	/**
	 * Get the raw numeric UniqueID for a given instance index.
	 * Returns 0 if the index is invalid or the instance was not registered.
	 */
	UFUNCTION(BlueprintPure, Category = "Phys X Instance|Runtime")
	int32 GetInstanceNumericIDByIndex(int32 InstanceIndex) const;

	/**
	 * Pick a random registered instance ID that belongs to this actor.
	 * Returns an invalid ID if no suitable instances were found.
	 */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Runtime")
	FPhysXInstanceID GetRandomInstanceID(bool bOnlySimulating = false) const;

	/**
	 * Ask the subsystem for the ID that corresponds to this actor's InstancedMesh
	 * and the given instance index.
	 */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Runtime")
	FPhysXInstanceID GetInstanceIDFromSubsystemByIndex(int32 InstanceIndex) const;

	// === High-level spawn API ===============================================

	/**
	 * Spawn a single PhysX-driven instance visually owned by this actor.
	 * Uses the subsystem spawn path that targets this explicit actor.
	 */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Spawn")
	FPhysXSpawnInstanceResult SpawnPhysicsInstanceFromActor(
		const FTransform& InstanceWorldTransform,
		bool bStartSimulating = true,
		FVector InitialLinearVelocity = FVector::ZeroVector,
		FVector InitialAngularVelocityRad = FVector::ZeroVector);

	/**
	 * Spawn multiple PhysX-driven instances visually owned by this actor.
	 * Calls SpawnPhysicsInstanceFromActor() for every transform and returns per-instance results.
	 */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Spawn")
	void SpawnPhysicsInstancesFromActorBatch(
		const TArray<FTransform>& InstanceWorldTransforms,
		bool bStartSimulating,
		TArray<FPhysXSpawnInstanceResult>& OutResults);

	/**
	 * Build instance transforms (Manual or Grid) and register them in the subsystem.
	 * Can be called in editor or at runtime.
	 */
	UFUNCTION(CallInEditor, Category = "Phys X Instance")
	void BuildAndRegisterInstances();

	// === Actor ID helpers ====================================================

	/** Get this actor's PhysX actor handle stored in the subsystem. */
	UFUNCTION(BlueprintPure, Category = "Phys X Actor|Runtime")
	FPhysXActorID GetPhysXActorID() const { return PhysXActorID; }

	/** Convenience: get raw numeric UniqueID (0 if invalid) for this actor. */
	UFUNCTION(BlueprintPure, Category = "Phys X Actor|Runtime")
	int32 GetPhysXActorNumericID() const
	{
		return PhysXActorID.IsValid()
			? static_cast<int32>(PhysXActorID.GetUniqueID())
			: 0;
	}

protected:
	// === AActor interface ====================================================

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	// === Internal helpers ====================================================

	/** Apply mesh and override materials on the InstancedMesh component. */
	void ApplyInstanceMaterials();

	/** Generate InstanceRelativeTransforms for Grid mode. */
	void GenerateGridTransforms();

	// === Runtime state =======================================================

	/** Cached pointer to the PhysX instanced subsystem for this world. */
	UPROPERTY(Transient)
	UPhysXInstancedWorldSubsystem* CachedSubsystem;

	/**
	 * Actor-level ID inside the PhysX instanced subsystem.
	 * This is a uint32-based handle stored as a lightweight value type.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Phys X Instance|Runtime")
	FPhysXActorID PhysXActorID;

	/**
	 * IDs of all instances registered in the subsystem.
	 * Order matches InstancedMesh instance indices when BuildAndRegisterInstances() ran.
	 */
	UPROPERTY(Transient)
	TArray<FPhysXInstanceID> RegisteredInstanceIDs;
};

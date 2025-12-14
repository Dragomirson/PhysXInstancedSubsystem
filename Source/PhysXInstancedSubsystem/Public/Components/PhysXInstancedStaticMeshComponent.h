/**
* Copyright (C) 2025 | Created by NordVader Inc.
* All rights reserved!
* My Discord Server: https://discord.gg/B8prpf3vzD
*/

#pragma once

#include "CoreMinimal.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "PhysXInstancedStaticMeshComponent.generated.h"

class APhysXInstancedMeshActor;

/**
 * Instanced mesh component used by the PhysX instanced world.
 *
 * Responsibilities:
 *  - Provides a reference to the owning PhysX instanced actor.
 *  - Applies per-instance transforms and custom data coming from PhysX.
 *  - Controls whether per-instance updates trigger navigation updates.
 */
UCLASS(ClassGroup = (PhysX), BlueprintType, Blueprintable, meta = (BlueprintSpawnableComponent))
class PHYSXINSTANCEDSUBSYSTEM_API UPhysXInstancedStaticMeshComponent : public UInstancedStaticMeshComponent
{
	GENERATED_BODY()

public:
	UPhysXInstancedStaticMeshComponent(const FObjectInitializer& ObjectInitializer);

	// --- Ownership -----------------------------------------------------------

	/** Owning PhysX instanced actor (optional). */
	UPROPERTY(BlueprintReadOnly, Category = "Phys X Instance")
	APhysXInstancedMeshActor* OwningPhysXActor;

	// --- Navigation ----------------------------------------------------------

	/**
	 * If true, per-instance transform changes call PartialNavigationUpdate()
	 * to update navigation data for the affected instance.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Phys X Instance|Navigation")
	bool bInstancesAffectNavigation;

	/** Set whether per-instance updates should trigger navigation updates. */
	UFUNCTION(BlueprintCallable, Category = "Phys X Instance|Navigation")
	void SetInstancesAffectNavigation(bool bNewValue);

	/** Get whether per-instance updates trigger navigation updates. */
	bool GetInstancesAffectNavigation() const { return bInstancesAffectNavigation; }

	// --- PhysX sync helpers --------------------------------------------------

	/** Rebuild all instances from a list of world-space transforms provided by PhysX. */
	void RebuildFromPhysXTransforms(const TArray<FTransform>& WorldTransforms);

	/** Update a single instance from a world-space transform provided by PhysX. */
	void UpdateInstanceFromPhysX(int32 InstanceIndex, const FTransform& WorldTransform, bool bTeleport);

	/**
	 * Batch update of instance transforms from PhysX.
	 *
	 * @param InstanceIndices  ISM instance indices (PerInstanceSMData).
	 * @param WorldTransforms  World-space transforms with matching length.
	 */
	void UpdateInstancesFromPhysXBatch_MT(
		const TArray<int32>& InstanceIndices,
		const TArray<FTransform>& WorldTransforms,
		bool bTeleport);

	/** Update PerInstanceCustomData for a single instance from PhysX-provided data. */
	void SetInstanceCustomDataFromPhysX(int32 InstanceIndex, const TArray<float>& CustomData);

protected:
	// --- Registration --------------------------------------------------------

	virtual void OnRegister() override;
	virtual void OnUnregister() override;

	// --- Internal helpers ----------------------------------------------------

	/** Apply a local-space transform to an instance with optional render-state invalidation. */
	void SetInstanceLocalTransformFromPhysX(
		int32 InstanceIndex,
		const FTransform& LocalTransform,
		bool bMarkRenderStateDirty,
		bool bTeleport);

	// --- Navigation ----------------------------------------------------------

	virtual void PartialNavigationUpdate(int32 InstanceIndex) override;
};

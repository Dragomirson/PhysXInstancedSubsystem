/**
* Copyright (C) 2025 | Created by NordVader Inc.
* All rights reserved!
* My Discord Server: https://discord.gg/B8prpf3vzD
*/

#include "Components/PhysXInstancedStaticMeshComponent.h"
#include "Actors/PhysXInstancedMeshActor.h"

#include "Async/ParallelFor.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "NavigationSystem.h"

// ============================================================================
// UPhysXInstancedStaticMeshComponent
// ============================================================================

UPhysXInstancedStaticMeshComponent::UPhysXInstancedStaticMeshComponent(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// By default this component does not simulate physics and has collision disabled.
	BodyInstance.bSimulatePhysics = false;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);

	// Per-instance custom data is used to push extra runtime values to materials.
	NumCustomDataFloats = 4;

	// Navigation updates are disabled by default for performance.
	bInstancesAffectNavigation = false;
	SetCanEverAffectNavigation(false);
}

// ============================================================================
// Navigation
// ============================================================================

void UPhysXInstancedStaticMeshComponent::SetInstancesAffectNavigation(bool bNewValue)
{
	if (bInstancesAffectNavigation == bNewValue)
	{
		return;
	}

	bInstancesAffectNavigation = bNewValue;

	// Enable/disable nav-relevance for this component.
	SetCanEverAffectNavigation(bInstancesAffectNavigation);

	// Request a nav-octree update for this component when the navigation system is available.
	if (GetWorld() && FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld()))
	{
		UNavigationSystemV1::UpdateComponentInNavOctree(*this);
	}
}

void UPhysXInstancedStaticMeshComponent::OnRegister()
{
	Super::OnRegister();

	// Cache owning actor pointer if it was not provided explicitly.
	if (!OwningPhysXActor)
	{
		OwningPhysXActor = Cast<APhysXInstancedMeshActor>(GetOwner());
	}

	// Pull navigation settings from the owning actor when available.
	if (OwningPhysXActor)
	{
		SetInstancesAffectNavigation(OwningPhysXActor->bInstancesAffectNavigation);
	}
}

void UPhysXInstancedStaticMeshComponent::OnUnregister()
{
	Super::OnUnregister();

	// Clear cached owner pointer when it matches the current owner.
	if (OwningPhysXActor == GetOwner())
	{
		OwningPhysXActor = nullptr;
	}
}

void UPhysXInstancedStaticMeshComponent::PartialNavigationUpdate(int32 InstanceIndex)
{
	if (!bInstancesAffectNavigation)
	{
		return;
	}

	Super::PartialNavigationUpdate(InstanceIndex);
}

// ============================================================================
// PhysX -> ISM sync helpers
// ============================================================================

void UPhysXInstancedStaticMeshComponent::RebuildFromPhysXTransforms(
	const TArray<FTransform>& WorldTransforms)
{
	ClearInstances();

	if (!GetWorld() || WorldTransforms.Num() == 0)
	{
		return;
	}

	// Convert world-space transforms to component-local transforms.
	const FTransform ToWorld          = GetComponentTransform();
	const FTransform WorldToComponent = ToWorld.Inverse();

	const int32 Count = WorldTransforms.Num();

	TArray<FTransform> LocalTransforms;
	LocalTransforms.SetNumUninitialized(Count);

	// Large batches convert transforms in parallel when supported.
	constexpr int32 ParallelThreshold = 256;

	if (FPlatformProcess::SupportsMultithreading() && Count > ParallelThreshold)
	{
		ParallelFor(Count, [&](int32 Index)
		{
			LocalTransforms[Index] = WorldTransforms[Index] * WorldToComponent;
		});
	}
	else
	{
		for (int32 Index = 0; Index < Count; ++Index)
		{
			LocalTransforms[Index] = WorldTransforms[Index] * WorldToComponent;
		}
	}

	AddInstances(LocalTransforms, /*bShouldReturnIndices*/ false);
	MarkRenderStateDirty();
}

void UPhysXInstancedStaticMeshComponent::SetInstanceLocalTransformFromPhysX(
	int32 InstanceIndex,
	const FTransform& LocalTransform,
	bool bMarkRenderStateDirty,
	bool /*bTeleport*/)
{
	if (!PerInstanceSMData.IsValidIndex(InstanceIndex))
	{
		return;
	}

	// Modify() is used so editor transactions can track per-instance changes when applicable.
	Modify();

	FInstancedStaticMeshInstanceData& InstanceData = PerInstanceSMData[InstanceIndex];
	InstanceData.Transform = LocalTransform.ToMatrixWithScale();

	PartialNavigationUpdate(InstanceIndex);

	if (bMarkRenderStateDirty)
	{
		MarkRenderStateDirty();
	}
}

void UPhysXInstancedStaticMeshComponent::UpdateInstanceFromPhysX(
	int32 InstanceIndex,
	const FTransform& WorldTransform,
	bool bTeleport)
{
	if (!PerInstanceSMData.IsValidIndex(InstanceIndex))
	{
		return;
	}

	// Convert the incoming world-space transform into component-local space.
	const FTransform ToWorld          = GetComponentTransform();
	const FTransform WorldToComponent = ToWorld.Inverse();

	const FTransform LocalTM = WorldTransform * WorldToComponent;

	SetInstanceLocalTransformFromPhysX(
		InstanceIndex,
		LocalTM,
		/*bMarkRenderStateDirty*/ true,
		bTeleport);
}

void UPhysXInstancedStaticMeshComponent::UpdateInstancesFromPhysXBatch_MT(
	const TArray<int32>& InstanceIndices,
	const TArray<FTransform>& WorldTransforms,
	bool bTeleport)
{
	// This function applies results on the game thread while optionally using parallel work for conversion.
	check(IsInGameThread());

	const int32 Count = InstanceIndices.Num();
	if (Count == 0 || Count != WorldTransforms.Num())
	{
		return;
	}

	if (!GetStaticMesh() || !GetWorld())
	{
		return;
	}

	// Convert world-space transforms to component-local transforms once, then apply by instance index.
	const FTransform ToWorld          = GetComponentTransform();
	const FTransform WorldToComponent = ToWorld.Inverse();

	TArray<FTransform> LocalTransforms;
	LocalTransforms.SetNumUninitialized(Count);

	constexpr int32 ParallelThreshold = 256;

	if (FPlatformProcess::SupportsMultithreading() && Count > ParallelThreshold)
	{
		ParallelFor(Count, [&](int32 i)
		{
			LocalTransforms[i] = WorldTransforms[i] * WorldToComponent;
		});
	}
	else
	{
		for (int32 i = 0; i < Count; ++i)
		{
			LocalTransforms[i] = WorldTransforms[i] * WorldToComponent;
		}
	}

	for (int32 i = 0; i < Count; ++i)
	{
		const int32 InstanceIndex = InstanceIndices[i];
		if (!PerInstanceSMData.IsValidIndex(InstanceIndex))
		{
			continue;
		}

		SetInstanceLocalTransformFromPhysX(
			InstanceIndex,
			LocalTransforms[i],
			/*bMarkRenderStateDirty*/ false,
			bTeleport);
	}

	MarkRenderStateDirty();
}

void UPhysXInstancedStaticMeshComponent::SetInstanceCustomDataFromPhysX(
	int32 InstanceIndex,
	const TArray<float>& CustomData)
{
	if (!PerInstanceSMData.IsValidIndex(InstanceIndex) || CustomData.Num() == 0)
	{
		return;
	}

	// Update per-instance custom data and mark render state dirty so materials see new values.
	SetCustomData(
		InstanceIndex,
		CustomData,
		/*bMarkRenderStateDirty*/ true);
}

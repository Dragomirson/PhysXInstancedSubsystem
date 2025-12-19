/**
* Copyright (C) 2025 | Created by NordVader Inc.
* All rights reserved!
* My Discord Server: https://discord.gg/B8prpf3vzD
*/

#include "Actors/PhysXInstancedMeshActor.h"

#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Components/PhysXInstancedStaticMeshComponent.h"
#include "Subsystems/PhysXInstancedWorldSubsystem.h"

#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"            // FStaticMaterial, GetStaticMaterials
#include "Engine/World.h"
#include "Interfaces/IPluginManager.h"
#include "PhysicsEngine/BodyInstance.h"
#include "UObject/StrongObjectPtr.h"

// ============================================================================
// APhysXInstancedMeshActor
// ============================================================================

#if WITH_EDITORONLY_DATA
static UTexture2D* LoadPhysXBillboardIcon()
{
    static TStrongObjectPtr<UTexture2D> Cached;

    if (Cached.IsValid())
    {
        return Cached.Get();
    }

    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PhysXInstancedSubsystem"));
    if (!Plugin.IsValid())
    {
        return nullptr;
    }

    const FString PngPath = FPaths::Combine(
        Plugin->GetBaseDir(),
        TEXT("Resources"),
        TEXT("T_PhysXInstancedMeshActorIcon.png")
    );

    TArray<uint8> CompressedData;
    if (!FFileHelper::LoadFileToArray(CompressedData, *PngPath))
    {
        return nullptr;
    }

    IImageWrapperModule& ImageWrapperModule =
        FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));

    const TSharedPtr<IImageWrapper> Wrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
    if (!Wrapper.IsValid() || !Wrapper->SetCompressed(CompressedData.GetData(), CompressedData.Num()))
    {
        return nullptr;
    }

    TArray<uint8> RawBGRA;
    if (!Wrapper->GetRaw(ERGBFormat::BGRA, 8, RawBGRA))
    {
        return nullptr;
    }

    const int32 Width  = Wrapper->GetWidth();
    const int32 Height = Wrapper->GetHeight();

    UTexture2D* Tex = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
    if (!Tex)
    {
        return nullptr;
    }

    // Make it look like an icon: no streaming, no mips.
    Tex->NeverStream = true;
    Tex->SRGB = true;

#if ENGINE_MAJOR_VERSION >= 5
    FTexturePlatformData* PlatformData = Tex->GetPlatformData();
#else
    FTexturePlatformData* PlatformData = Tex->PlatformData;
#endif

    if (!PlatformData || PlatformData->Mips.Num() == 0)
    {
        return nullptr;
    }

    FTexture2DMipMap& Mip = PlatformData->Mips[0];
    void* MipData = Mip.BulkData.Lock(LOCK_READ_WRITE);
    FMemory::Memcpy(MipData, RawBGRA.GetData(), RawBGRA.Num());
    Mip.BulkData.Unlock();

    Tex->UpdateResource();

    Cached.Reset(Tex);
    return Tex;
}
#endif


/** Construct the actor and initialize defaults for rendering and instance behavior. */
APhysXInstancedMeshActor::APhysXInstancedMeshActor()
{
	PrimaryActorTick.bCanEverTick = false;

	// --- Actor mode flags ----------------------------------------------------

	bStorageOnly    = false;
	bIsStorageActor = false;

	// --- Navigation defaults -------------------------------------------------

	bInstancesAffectNavigation        = false;
	bStorageInstancesAffectNavigation = true;

	// --- Storage collision defaults -----------------------------------------

	StorageCollisionProfile.Name = TEXT("BlockAll");
	StorageCollisionEnabled      = ECollisionEnabled::QueryAndPhysics;

	// --- Components ----------------------------------------------------------

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);
	
	// --- Billboard ----------------------------------------------------------

	#if WITH_EDITORONLY_DATA
	PhysXBillboard = CreateDefaultSubobject<UBillboardComponent>(TEXT("PhysXBillboard"));
	PhysXBillboard->SetupAttachment(SceneRoot);
	PhysXBillboard->bHiddenInGame = true;
	PhysXBillboard->SetIsVisualizationComponent(true);

	if (UTexture2D* IconTex = LoadPhysXBillboardIcon())
	{
		PhysXBillboard->Sprite = IconTex;
	}
#endif

	InstancedMesh = CreateDefaultSubobject<UPhysXInstancedStaticMeshComponent>(TEXT("InstancedMesh"));
	InstancedMesh->SetupAttachment(SceneRoot);
	InstancedMesh->OwningPhysXActor = this;

	// Instances are expected to move, so keep lighting fully dynamic.
	InstancedMesh->SetMobility(EComponentMobility::Movable);

	// Default shadow flags (final state is also synchronized in OnConstruction/BeginPlay).
	InstancedMesh->bCastStaticShadow  = false;
	InstancedMesh->bCastDynamicShadow = true;

	InstancedMesh->SetCastShadow(true);
	InstancedMesh->bCastDynamicShadow = true;
	InstancedMesh->bCastStaticShadow  = true;

	// --- Visual mesh ---------------------------------------------------------

	InstanceStaticMesh = nullptr;

	// --- Default spawn / build settings -------------------------------------

	bAutoRegisterOnBeginPlay = true;

	SpawnMode   = EPhysXInstanceSpawnMode::Grid2D;
	GridRows    = 5;
	GridColumns = 5;
	GridLayers  = 1;

	GridSpacingX = 200.0f;
	GridSpacingY = 200.0f;
	GridSpacingZ = 200.0f;

	bCenterGridXY = true;
	bCenterGridZ  = false;

	// --- Physics defaults ----------------------------------------------------

	bSimulateInstances     = true;
	bInstancesUseGravity   = true;
	bOverrideInstanceMass  = false;
	InstanceMassInKg       = 10.0f;
	InstanceLinearDamping  = 0.0f;
	InstanceAngularDamping = 0.05f;
	bDisableISMPhysics     = true;

	InstanceShapeType     = EPhysXInstanceShapeType::Box;
	OverrideCollisionMesh = nullptr;

	InstancesCollisionProfile.Name = TEXT("BlockAllDynamic");
	bDestroyBodyOnDisable          = false;

	ShapeCollisionOffset = FTransform::Identity;

	// --- Runtime bookkeeping ------------------------------------------------

	CachedSubsystem = nullptr;

	// --- Rendering toggles ---------------------------------------------------

	bInstancesCastShadow = true;

	// Override defaults with engine-defined collision profile names.
	InstancesCollisionProfile.Name = UCollisionProfile::BlockAllDynamic_ProfileName;
	StorageCollisionProfile.Name   = UCollisionProfile::BlockAllDynamic_ProfileName;
	StorageCollisionEnabled        = ECollisionEnabled::QueryAndPhysics;
}

void APhysXInstancedMeshActor::ApplyCollisionSettings()
{
	if (!InstancedMesh)
		return;

	InstancedMesh->SetSimulatePhysics(false);

	if (bStorageOnly)
	{
		const FName Profile =
			(StorageCollisionProfile.Name != NAME_None)
				? StorageCollisionProfile.Name
				: InstancesCollisionProfile.Name;

		if (Profile != NAME_None)
		{
			InstancedMesh->SetCollisionProfileName(Profile);
		}

		InstancedMesh->SetCollisionEnabled(StorageCollisionEnabled);
		InstancedMesh->SetInstancesAffectNavigation(bStorageInstancesAffectNavigation);
	}
	else
	{
		if (InstancesCollisionProfile.Name != NAME_None)
		{
			InstancedMesh->SetCollisionProfileName(InstancesCollisionProfile.Name);
		}

		InstancedMesh->SetCollisionEnabled(
			bDisableISMPhysics ? ECollisionEnabled::NoCollision
							   : ECollisionEnabled::QueryAndPhysics);

		InstancedMesh->SetInstancesAffectNavigation(bInstancesAffectNavigation);
	}
}

/** Apply editor/runtime property state to the InstancedMesh component. */
void APhysXInstancedMeshActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	if (!InstancedMesh)
	{
		return;
	}

	// --- Shadow flags --------------------------------------------------------

	InstancedMesh->SetCastShadow(bInstancesCastShadow);
	InstancedMesh->bCastDynamicShadow = bInstancesCastShadow;
	InstancedMesh->bCastStaticShadow  = bInstancesCastShadow;

	// --- Navigation flags ----------------------------------------------------

	// Keep the component under this actor transform (visual alignment only).
	InstancedMesh->SetWorldLocationAndRotation(
		Transform.GetLocation(),
		Transform.GetRotation());

	if (InstanceStaticMesh && InstancedMesh->GetStaticMesh() != InstanceStaticMesh)
	{
		InstancedMesh->SetStaticMesh(InstanceStaticMesh);
	}

	// Apply either mesh materials or actor overrides.
	ApplyInstanceMaterials();

	// Apply collision profile to the InstancedMesh body instance.
	
	if (InstancesCollisionProfile.Name != NAME_None)
		{
		InstancedMesh->SetCollisionProfileName(InstancesCollisionProfile.Name);
		}

	// Apply mass and damping overrides to the BodyInstance.
	// Subsystem-created PhysX bodies read these settings when spawning.
	FBodyInstance& BodyInstance = InstancedMesh->BodyInstance;

	// If bOverrideInstanceMass is false, the override value is stored but auto mass is still used.
	BodyInstance.SetMassOverride(InstanceMassInKg, bOverrideInstanceMass);
	BodyInstance.LinearDamping  = InstanceLinearDamping;
	BodyInstance.AngularDamping = InstanceAngularDamping;

	// InstancedMesh is typically a rendering proxy and should not participate in native collision.
	// Storage-only actors keep static ISM collision enabled.
	if (bStorageOnly)
	{
		InstancedMesh->SetSimulatePhysics(false);
		
		if (StorageCollisionProfile.Name != NAME_None)
		{
			InstancedMesh->SetCollisionProfileName(StorageCollisionProfile.Name);
		}
		else if (InstancesCollisionProfile.Name != NAME_None)
		{
			InstancedMesh->SetCollisionProfileName(InstancesCollisionProfile.Name);
		}

		InstancedMesh->SetCollisionEnabled(StorageCollisionEnabled);
		InstancedMesh->SetInstancesAffectNavigation(bStorageInstancesAffectNavigation);
	}
	else if (bDisableISMPhysics)
	{
		InstancedMesh->SetSimulatePhysics(false);
		InstancedMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	else
	{
		// ISM collision is enabled when explicitly requested by the actor settings.
		InstancedMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	}

	// Keep gravity flag in sync with actor settings (visual proxy; real bodies read the same flag).
	InstancedMesh->SetEnableGravity(bInstancesUseGravity);

	if (InstancedMesh)
	{
		InstancedMesh->SetInstancesAffectNavigation(bInstancesAffectNavigation);
	}
}

#if WITH_EDITOR

/** React to editor property changes that affect mesh/materials/shadows. */
void APhysXInstancedMeshActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropertyName = PropertyChangedEvent.GetPropertyName();

	// Re-apply materials when mesh or material override settings change.
	if (PropertyName == GET_MEMBER_NAME_CHECKED(APhysXInstancedMeshActor, InstanceStaticMesh) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(APhysXInstancedMeshActor, bOverrideInstanceMaterials) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(APhysXInstancedMeshActor, InstanceOverrideMaterials))
	{
		ApplyInstanceMaterials();
	}

	// Synchronize shadow flags when the actor toggle changes.
	if (PropertyName == GET_MEMBER_NAME_CHECKED(APhysXInstancedMeshActor, bInstancesCastShadow))
	{
		if (InstancedMesh)
		{
			InstancedMesh->SetCastShadow(bInstancesCastShadow);
			InstancedMesh->bCastDynamicShadow = bInstancesCastShadow;
			InstancedMesh->bCastStaticShadow  = bInstancesCastShadow;
		}
	}
}

#endif // WITH_EDITOR

/** Cache subsystem pointer, register actor, then optionally build and register instances. */
void APhysXInstancedMeshActor::BeginPlay()
{
	Super::BeginPlay();

	ApplyInstanceMaterials();

	if (UWorld* World = GetWorld())
	{
		CachedSubsystem = World->GetSubsystem<UPhysXInstancedWorldSubsystem>();
	}

	if (CachedSubsystem)
	{
		PhysXActorID = CachedSubsystem->RegisterInstancedMeshActor(this);
	}

	if (InstancedMesh)
	{
		// Shadow flags can be overridden by the actor at runtime.
		InstancedMesh->SetCastShadow(bInstancesCastShadow);
		InstancedMesh->bCastDynamicShadow = bInstancesCastShadow;
		InstancedMesh->bCastStaticShadow  = bInstancesCastShadow;

		// Ensure the component uses the actor mesh.
		if (InstanceStaticMesh)
		{
			InstancedMesh->SetStaticMesh(InstanceStaticMesh);
		}

		FBodyInstance& BodyInstance = InstancedMesh->BodyInstance;

		BodyInstance.SetMassOverride(InstanceMassInKg, bOverrideInstanceMass);
		BodyInstance.LinearDamping  = InstanceLinearDamping;
		BodyInstance.AngularDamping = InstanceAngularDamping;

		if (bStorageOnly)
		{
			// Storage-only: static ISM with collision, no PhysX bodies.
			InstancedMesh->SetSimulatePhysics(false);

			if (StorageCollisionProfile.Name != NAME_None)
				{
				InstancedMesh->SetCollisionProfileName(StorageCollisionProfile.Name);
				}
			else if (InstancesCollisionProfile.Name != NAME_None)
				{
				InstancedMesh->SetCollisionProfileName(InstancesCollisionProfile.Name);
				}
			
			InstancedMesh->SetCollisionEnabled(StorageCollisionEnabled);
			InstancedMesh->SetInstancesAffectNavigation(bStorageInstancesAffectNavigation);
		}
		else
		{
			// Dynamic actor: PhysX bodies are managed by the subsystem; ISM stays non-simulating.
			InstancedMesh->SetSimulatePhysics(false);

			if (InstancesCollisionProfile.Name != NAME_None)
			{
				InstancedMesh->SetCollisionProfileName(InstancesCollisionProfile.Name);
			}
			
			if (bDisableISMPhysics)
			{
				// Visual-only: collision is disabled; all collision/physics comes from subsystem bodies.
				InstancedMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			}
			else
			{
				// Native ISM collision stays enabled when requested (in addition to subsystem physics).
				InstancedMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			}

			InstancedMesh->SetInstancesAffectNavigation(bDynamicInstancesAffectNavigation);
		}

		InstancedMesh->SetEnableGravity(bInstancesUseGravity);
	}

	if (bAutoRegisterOnBeginPlay)
	{
		BuildAndRegisterInstances();
	}
}

/** Unregister all instances and the actor handle from the world subsystem. */
void APhysXInstancedMeshActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 1) Unregister all instance handles from the subsystem.
	if (CachedSubsystem)
	{
		for (const FPhysXInstanceID& ID : RegisteredInstanceIDs)
		{
			if (ID.IsValid())
			{
				CachedSubsystem->UnregisterInstance(ID);
			}
		}

		// 2) Unregister this actor handle as well (if it was registered).
		if (PhysXActorID.IsValid())
		{
			CachedSubsystem->UnregisterInstancedMeshActor(PhysXActorID);
		}
	}

	RegisteredInstanceIDs.Empty();
	PhysXActorID     = FPhysXActorID();
	CachedSubsystem  = nullptr;

	Super::EndPlay(EndPlayReason);
}

// ============================================================================
// Materials
// ============================================================================

/** Apply StaticMesh materials or actor-provided overrides onto the InstancedMesh component. */
void APhysXInstancedMeshActor::ApplyInstanceMaterials()
{
	if (!InstancedMesh)
	{
		return;
	}

	// Ensure the component uses the actor's configured static mesh.
	if (InstanceStaticMesh && InstancedMesh->GetStaticMesh() != InstanceStaticMesh)
	{
		InstancedMesh->SetStaticMesh(InstanceStaticMesh);
	}

	// If there is no mesh, there is nothing meaningful to apply.
	if (!InstanceStaticMesh)
	{
		return;
	}

	const int32 NumMeshSlots = InstanceStaticMesh->GetStaticMaterials().Num();

	// Case 1: use explicit override materials.
	if (bOverrideInstanceMaterials && InstanceOverrideMaterials.Num() > 0)
	{
		for (int32 SlotIndex = 0; SlotIndex < NumMeshSlots; ++SlotIndex)
		{
			// If override is missing for a slot, fall back to the mesh material.
			UMaterialInterface* Material = nullptr;

			if (InstanceOverrideMaterials.IsValidIndex(SlotIndex))
			{
				Material = InstanceOverrideMaterials[SlotIndex];
			}

			if (!Material)
			{
				Material = InstanceStaticMesh->GetMaterial(SlotIndex);
			}

			InstancedMesh->SetMaterial(SlotIndex, Material);
		}
	}
	else
	{
		// Case 2: no overrides enabled — mirror materials from the mesh.
		for (int32 SlotIndex = 0; SlotIndex < NumMeshSlots; ++SlotIndex)
		{
			UMaterialInterface* Material = InstanceStaticMesh->GetMaterial(SlotIndex);
			InstancedMesh->SetMaterial(SlotIndex, Material);
		}
	}

	// If the component currently has more material slots than the mesh, clear extra ones.
	const int32 NumComponentSlots = InstancedMesh->GetNumMaterials();
	for (int32 SlotIndex = NumMeshSlots; SlotIndex < NumComponentSlots; ++SlotIndex)
	{
		InstancedMesh->SetMaterial(SlotIndex, nullptr);
	}
}

// ============================================================================
// Grid transform generation
// ============================================================================

/** Build InstanceRelativeTransforms for Grid2D spawn mode. */
void APhysXInstancedMeshActor::GenerateGridTransforms()
{
	InstanceRelativeTransforms.Empty();

	if (GridRows <= 0 || GridColumns <= 0 || GridLayers <= 0)
	{
		return;
	}

	// Optional XY centering around actor origin.
	FVector CenterOffsetXY = FVector::ZeroVector;
	if (bCenterGridXY)
	{
		const float TotalSizeX = (GridRows - 1) * GridSpacingX;
		const float TotalSizeY = (GridColumns - 1) * GridSpacingY;
		CenterOffsetXY = FVector(TotalSizeX * 0.5f, TotalSizeY * 0.5f, 0.0f);
	}

	// Optional Z centering around actor origin.
	float CenterOffsetZ = 0.0f;
	if (bCenterGridZ && GridLayers > 1)
	{
		const float TotalSizeZ = (GridLayers - 1) * GridSpacingZ;
		CenterOffsetZ = TotalSizeZ * 0.5f;
	}

	for (int32 Layer = 0; Layer < GridLayers; ++Layer)
	{
		for (int32 Row = 0; Row < GridRows; ++Row)
		{
			for (int32 Col = 0; Col < GridColumns; ++Col)
			{
				FVector Location(
					Row   * GridSpacingX,
					Col   * GridSpacingY,
					Layer * GridSpacingZ);

				if (bCenterGridXY)
				{
					Location -= CenterOffsetXY;
				}

				if (bCenterGridZ)
				{
					Location.Z -= CenterOffsetZ;
				}

				InstanceRelativeTransforms.Add(FTransform(Location));
			}
		}
	}
}

// ============================================================================
// Build / registration
// ============================================================================

/** Build ISM instances (manual or grid) and register them in the world subsystem. */
void APhysXInstancedMeshActor::BuildAndRegisterInstances()
{
	if (!InstancedMesh || !InstancedMesh->GetStaticMesh())
	{
		return;
	}

	// Refresh cached subsystem pointer when needed.
	if (!CachedSubsystem)
	{
		if (UWorld* World = GetWorld())
		{
			CachedSubsystem = World->GetSubsystem<UPhysXInstancedWorldSubsystem>();
		}
	}

	if (!CachedSubsystem)
	{
		return;
	}

	// --------------------------------------------------------------------
	// 0) Generate local instance transforms (Manual or Grid2D).
	// --------------------------------------------------------------------

	switch (SpawnMode)
	{
	case EPhysXInstanceSpawnMode::Grid2D:
		GridLayers = FMath::Max(GridLayers, 1);
		GenerateGridTransforms();
		break;

	case EPhysXInstanceSpawnMode::Manual:
	default:
		if (InstanceRelativeTransforms.Num() == 0)
		{
			// Manual mode with empty list falls back to a single identity transform.
			InstanceRelativeTransforms.Add(FTransform::Identity);
		}
		break;
	}

	// --------------------------------------------------------------------
	// 1) Remove previously registered instances and their bodies.
	// --------------------------------------------------------------------

	for (const FPhysXInstanceID& ID : RegisteredInstanceIDs)
	{
		if (ID.IsValid())
		{
			CachedSubsystem->UnregisterInstance(ID);
		}
	}
	RegisteredInstanceIDs.Reset();

	InstancedMesh->ClearInstances();

	// --------------------------------------------------------------------
	// 2) Storage-only: create ISM instances only, no PhysX bodies.
	// --------------------------------------------------------------------

	if (bStorageOnly)
	{
		for (const FTransform& LocalTM : InstanceRelativeTransforms)
		{
			const FTransform WorldTM = LocalTM * GetActorTransform();
			InstancedMesh->AddInstanceWorldSpace(WorldTM);
		}

		return;
	}

	// --------------------------------------------------------------------
	// 3) Dynamic actor: create ISM instances and register them as a batch.
	// --------------------------------------------------------------------

	TArray<int32> IndicesToRegister;
	IndicesToRegister.Reserve(InstanceRelativeTransforms.Num());

	for (const FTransform& LocalTM : InstanceRelativeTransforms)
	{
		const FTransform WorldTM = LocalTM * GetActorTransform();

		const int32 NewIndex = InstancedMesh->AddInstanceWorldSpace(WorldTM);
		if (NewIndex == INDEX_NONE)
		{
			continue;
		}

		IndicesToRegister.Add(NewIndex);
	}

	if (IndicesToRegister.Num() == 0)
	{
		return;
	}

	const bool bSimulate = bSimulateInstances;

	// Batch registration fills RegisteredInstanceIDs in the same order as IndicesToRegister.
	CachedSubsystem->RegisterInstancesBatch(
		InstancedMesh,
		IndicesToRegister,
		bSimulate,
		/*out*/ RegisteredInstanceIDs);
}

// ============================================================================
// Auto-stop configuration (runtime API)
// ============================================================================

/** Enable or disable automatic auto-stop logic for instances owned by this actor. */
void APhysXInstancedMeshActor::SetAutoStopEnabled(bool bEnable)
{
	AutoStopConfig.bEnableAutoStop = bEnable;
}

/** Update the main auto-stop condition and thresholds. */
void APhysXInstancedMeshActor::ConfigureAutoStopBasic(
	EPhysXInstanceStopCondition NewCondition,
	float NewLinearSpeedThreshold,
	float NewAngularSpeedThreshold,
	float NewMinStoppedTime)
{
	AutoStopConfig.Condition             = NewCondition;
	AutoStopConfig.LinearSpeedThreshold  = FMath::Max(0.0f, NewLinearSpeedThreshold);
	AutoStopConfig.AngularSpeedThreshold = FMath::Max(0.0f, NewAngularSpeedThreshold);
	AutoStopConfig.MinStoppedTime        = FMath::Max(0.0f, NewMinStoppedTime);
}

/** Update the action executed when an instance is considered stopped. */
void APhysXInstancedMeshActor::SetAutoStopAction(EPhysXInstanceStopAction NewAction)
{
	AutoStopConfig.Action = NewAction;
}

/** Update the extra safety rules used by the auto-stop logic. */
void APhysXInstancedMeshActor::ConfigureAutoStopSafety(
	bool bInUseMaxFallTime,
	float NewMaxFallTime,
	bool bInUseMaxDistanceFromActor,
	float NewMaxDistanceFromActor)
{
	AutoStopConfig.bUseMaxFallTime           = bInUseMaxFallTime;
	AutoStopConfig.bUseMaxDistanceFromActor  = bInUseMaxDistanceFromActor;

	AutoStopConfig.MaxFallTime          = FMath::Max(0.0f, NewMaxFallTime);
	AutoStopConfig.MaxDistanceFromActor = FMath::Max(0.0f, NewMaxDistanceFromActor);
}

// ============================================================================
// Physics control (index based)
// ============================================================================

/** Enable or disable physics for a single instance (by ISM instance index). */
void APhysXInstancedMeshActor::SetInstancePhysicsEnabled(int32 InstanceIndex, bool bEnable)
{
	if (!InstancedMesh || !RegisteredInstanceIDs.IsValidIndex(InstanceIndex))
	{
		return;
	}

	// Refresh subsystem pointer if needed.
	if (!CachedSubsystem)
	{
		if (UWorld* World = GetWorld())
		{
			CachedSubsystem = World->GetSubsystem<UPhysXInstancedWorldSubsystem>();
		}
	}

	if (!CachedSubsystem)
	{
		return;
	}

	const FPhysXInstanceID ID = RegisteredInstanceIDs[InstanceIndex];
	if (!ID.IsValid())
	{
		return;
	}

	CachedSubsystem->SetInstancePhysicsEnabled(ID, bEnable, bDestroyBodyOnDisable);
}

/** Enable or disable physics for all instances owned by this actor. */
void APhysXInstancedMeshActor::SetAllInstancesPhysicsEnabled(bool bEnable)
{
	if (!InstancedMesh)
	{
		return;
	}

	// Refresh subsystem pointer if needed.
	if (!CachedSubsystem)
	{
		if (UWorld* World = GetWorld())
		{
			CachedSubsystem = World->GetSubsystem<UPhysXInstancedWorldSubsystem>();
		}
	}

	if (!CachedSubsystem)
	{
		return;
	}

	for (int32 Index = 0; Index < RegisteredInstanceIDs.Num(); ++Index)
	{
		const FPhysXInstanceID ID = RegisteredInstanceIDs[Index];
		if (ID.IsValid())
		{
			CachedSubsystem->SetInstancePhysicsEnabled(ID, bEnable, bDestroyBodyOnDisable);
		}
	}
}

/** Check whether physics is enabled for a single instance (by ISM instance index). */
bool APhysXInstancedMeshActor::IsInstancePhysicsEnabledByIndex(int32 InstanceIndex) const
{
	if (!CachedSubsystem)
	{
		if (UWorld* World = GetWorld())
		{
			// const_cast is used only to cache the subsystem pointer.
			UPhysXInstancedWorldSubsystem* MutableSubsystem =
				World->GetSubsystem<UPhysXInstancedWorldSubsystem>();
			const_cast<APhysXInstancedMeshActor*>(this)->CachedSubsystem = MutableSubsystem;
		}
	}

	if (!CachedSubsystem)
	{
		return false;
	}

	if (!RegisteredInstanceIDs.IsValidIndex(InstanceIndex))
	{
		return false;
	}

	const FPhysXInstanceID ID = RegisteredInstanceIDs[InstanceIndex];
	if (!ID.IsValid())
	{
		return false;
	}

	return CachedSubsystem->IsInstancePhysicsEnabled(ID);
}

/** Enable or disable gravity for a single instance (by ISM instance index). */
void APhysXInstancedMeshActor::SetInstanceGravityEnabledByIndex(int32 InstanceIndex, bool bEnable)
{
	if (!RegisteredInstanceIDs.IsValidIndex(InstanceIndex))
	{
		return;
	}

	if (!CachedSubsystem)
	{
		if (UWorld* World = GetWorld())
		{
			CachedSubsystem = World->GetSubsystem<UPhysXInstancedWorldSubsystem>();
		}
	}

	if (!CachedSubsystem)
	{
		return;
	}

	const FPhysXInstanceID ID = RegisteredInstanceIDs[InstanceIndex];
	if (!ID.IsValid())
	{
		return;
	}

	CachedSubsystem->SetInstanceGravityEnabled(ID, bEnable);
}

/** Check whether gravity is enabled for a single instance (by ISM instance index). */
bool APhysXInstancedMeshActor::IsInstanceGravityEnabledByIndex(int32 InstanceIndex) const
{
	if (!RegisteredInstanceIDs.IsValidIndex(InstanceIndex))
	{
		return false;
	}

	if (!CachedSubsystem)
	{
		if (UWorld* World = GetWorld())
		{
			UPhysXInstancedWorldSubsystem* Subsys =
				World->GetSubsystem<UPhysXInstancedWorldSubsystem>();
			const_cast<APhysXInstancedMeshActor*>(this)->CachedSubsystem = Subsys;
		}
	}

	if (!CachedSubsystem)
	{
		return false;
	}

	const FPhysXInstanceID ID = RegisteredInstanceIDs[InstanceIndex];
	if (!ID.IsValid())
	{
		return false;
	}

	return CachedSubsystem->IsInstanceGravityEnabled(ID);
}

/** Set linear velocity for a single instance (by ISM instance index). */
void APhysXInstancedMeshActor::SetInstanceLinearVelocityByIndex(
	int32 InstanceIndex,
	FVector NewVelocity,
	bool bAutoWake)
{
	if (!RegisteredInstanceIDs.IsValidIndex(InstanceIndex))
	{
		return;
	}

	if (!CachedSubsystem)
	{
		if (UWorld* World = GetWorld())
		{
			CachedSubsystem = World->GetSubsystem<UPhysXInstancedWorldSubsystem>();
		}
	}

	if (!CachedSubsystem)
	{
		return;
	}

	const FPhysXInstanceID ID = RegisteredInstanceIDs[InstanceIndex];
	if (!ID.IsValid())
	{
		return;
	}

	CachedSubsystem->SetInstanceLinearVelocity(ID, NewVelocity, bAutoWake);
}

/** Read linear velocity for a single instance (by ISM instance index). */
bool APhysXInstancedMeshActor::GetInstanceLinearVelocityByIndex(
	int32 InstanceIndex,
	FVector& OutVelocity) const
{
	OutVelocity = FVector::ZeroVector;

	if (!RegisteredInstanceIDs.IsValidIndex(InstanceIndex))
	{
		return false;
	}

	if (!CachedSubsystem)
	{
		if (UWorld* World = GetWorld())
		{
			UPhysXInstancedWorldSubsystem* Subsys =
				World->GetSubsystem<UPhysXInstancedWorldSubsystem>();
			const_cast<APhysXInstancedMeshActor*>(this)->CachedSubsystem = Subsys;
		}
	}

	if (!CachedSubsystem)
	{
		return false;
	}

	const FPhysXInstanceID ID = RegisteredInstanceIDs[InstanceIndex];
	if (!ID.IsValid())
	{
		return false;
	}

	return CachedSubsystem->GetInstanceLinearVelocity(ID, OutVelocity);
}

/** Set angular velocity (radians/sec) for a single instance (by ISM instance index). */
void APhysXInstancedMeshActor::SetInstanceAngularVelocityByIndex(
	int32 InstanceIndex,
	FVector NewAngVelRad,
	bool bAutoWake)
{
	if (!RegisteredInstanceIDs.IsValidIndex(InstanceIndex))
	{
		return;
	}

	if (!CachedSubsystem)
	{
		if (UWorld* World = GetWorld())
		{
			CachedSubsystem = World->GetSubsystem<UPhysXInstancedWorldSubsystem>();
		}
	}

	if (!CachedSubsystem)
	{
		return;
	}

	const FPhysXInstanceID ID = RegisteredInstanceIDs[InstanceIndex];
	if (!ID.IsValid())
	{
		return;
	}

	CachedSubsystem->SetInstanceAngularVelocityInRadians(ID, NewAngVelRad, bAutoWake);
}

/** Read angular velocity (radians/sec) for a single instance (by ISM instance index). */
bool APhysXInstancedMeshActor::GetInstanceAngularVelocityByIndex(
	int32 InstanceIndex,
	FVector& OutAngVelRad) const
{
	OutAngVelRad = FVector::ZeroVector;

	if (!RegisteredInstanceIDs.IsValidIndex(InstanceIndex))
	{
		return false;
	}

	if (!CachedSubsystem)
	{
		if (UWorld* World = GetWorld())
		{
			UPhysXInstancedWorldSubsystem* Subsys =
				World->GetSubsystem<UPhysXInstancedWorldSubsystem>();
			const_cast<APhysXInstancedMeshActor*>(this)->CachedSubsystem = Subsys;
		}
	}

	if (!CachedSubsystem)
	{
		return false;
	}

	const FPhysXInstanceID ID = RegisteredInstanceIDs[InstanceIndex];
	if (!ID.IsValid())
	{
		return false;
	}

	return CachedSubsystem->GetInstanceAngularVelocityInRadians(ID, OutAngVelRad);
}

/** Disable physics for an instance and force its PhysX body to be destroyed. */
void APhysXInstancedMeshActor::DisableInstanceAndDestroyBody(int32 InstanceIndex)
{
	// Temporarily force body destruction for this call.
	const bool bOldDestroyFlag = bDestroyBodyOnDisable;
	bDestroyBodyOnDisable = true;

	SetInstancePhysicsEnabled(InstanceIndex, false);

	bDestroyBodyOnDisable = bOldDestroyFlag;
}

// ============================================================================
// High-level spawn API
// ============================================================================

/** Spawn a single instance through the subsystem while using this actor as the visual owner. */
FPhysXSpawnInstanceResult APhysXInstancedMeshActor::SpawnPhysicsInstanceFromActor(
	const FTransform& InstanceWorldTransform,
	bool bStartSimulating,
	FVector InitialLinearVelocity,
	FVector InitialAngularVelocityRad)
{
	FPhysXSpawnInstanceResult Result;

	if (!InstanceStaticMesh)
	{
		return Result;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return Result;
	}

	// Cache subsystem pointer to avoid repeated GetSubsystem() calls.
	UPhysXInstancedWorldSubsystem* Subsystem = CachedSubsystem;
	if (!Subsystem)
	{
		Subsystem = World->GetSubsystem<UPhysXInstancedWorldSubsystem>();
		CachedSubsystem = Subsystem;
	}

	if (!Subsystem)
	{
		return Result;
	}

	FPhysXSpawnInstanceRequest Request;

	// Request fields shared across all actor selection modes.
	Request.InstanceWorldTransform    = InstanceWorldTransform;
	Request.InitialLinearVelocity     = InitialLinearVelocity;
	Request.InitialAngularVelocityRad = InitialAngularVelocityRad;
	Request.bStartSimulating          = bStartSimulating;

	Request.StaticMesh            = InstanceStaticMesh;
	Request.bUseOverrideMaterials = bOverrideInstanceMaterials;
	Request.OverrideMaterials     = InstanceOverrideMaterials;

	if (bIsStorageActor)
	{
		// Storage actors do not own dynamic bodies; pick or create a matching non-storage actor.
		Request.ActorMode = EPhysXInstanceActorMode::FindOrCreateByMeshAndMats;
	}
	else
	{
		// Non-storage actors can be used directly as the explicit owner actor.
		Request.ActorMode     = EPhysXInstanceActorMode::UseExplicitActor;
		Request.ExplicitActor = this;
	}

	return Subsystem->SpawnPhysicsInstance(Request);
}

/** Spawn multiple instances through SpawnPhysicsInstanceFromActor() and return per-instance results. */
void APhysXInstancedMeshActor::SpawnPhysicsInstancesFromActorBatch(
	const TArray<FTransform>& InstanceWorldTransforms,
	bool bStartSimulating,
	TArray<FPhysXSpawnInstanceResult>& OutResults)
{
	OutResults.Reset();

	if (InstanceWorldTransforms.Num() == 0)
	{
		return;
	}

	OutResults.Reserve(InstanceWorldTransforms.Num());

	for (const FTransform& WorldTM : InstanceWorldTransforms)
	{
		const FPhysXSpawnInstanceResult ResultItem =
			SpawnPhysicsInstanceFromActor(
				WorldTM,
				bStartSimulating,
				/*InitialLinearVelocity*/ FVector::ZeroVector,
				/*InitialAngularVelocityRad*/ FVector::ZeroVector);

		OutResults.Add(ResultItem);
	}
}

// ============================================================================
// ID / count helpers
// ============================================================================

/** Return the number of visual ISM instances owned by this actor. */
int32 APhysXInstancedMeshActor::GetInstanceCount() const
{
	return InstancedMesh ? InstancedMesh->GetInstanceCount() : 0;
}

/** Map ISM instance index to the corresponding subsystem instance handle. */
FPhysXInstanceID APhysXInstancedMeshActor::GetInstanceIDByIndex(int32 InstanceIndex) const
{
	// RegisteredInstanceIDs follows ISM instance order at the time of BuildAndRegisterInstances().
	if (InstanceIndex < 0 || !RegisteredInstanceIDs.IsValidIndex(InstanceIndex))
	{
		return FPhysXInstanceID(); // invalid
	}

	return RegisteredInstanceIDs[InstanceIndex];
}

/** Return the raw numeric handle (UniqueID) for the given ISM instance index. */
int32 APhysXInstancedMeshActor::GetInstanceNumericIDByIndex(int32 InstanceIndex) const
{
	const FPhysXInstanceID ID = GetInstanceIDByIndex(InstanceIndex);

	// 0 means invalid, matching the default-constructed FPhysXInstanceID.
	return ID.IsValid()
		? static_cast<int32>(ID.GetUniqueID())
		: 0;
}

/** Pick a random registered instance ID owned by this actor. */
FPhysXInstanceID APhysXInstancedMeshActor::GetRandomInstanceID(bool bOnlySimulating) const
{
	// Early out if we have no registered instances at all.
	if (RegisteredInstanceIDs.Num() == 0)
	{
		return FPhysXInstanceID();
	}

	// If simulation state is not required, select a random valid ID locally.
	if (!bOnlySimulating)
	{
		TArray<FPhysXInstanceID> ValidIDs;
		ValidIDs.Reserve(RegisteredInstanceIDs.Num());

		for (const FPhysXInstanceID& ID : RegisteredInstanceIDs)
		{
			if (ID.IsValid())
			{
				ValidIDs.Add(ID);
			}
		}

		if (ValidIDs.Num() == 0)
		{
			return FPhysXInstanceID();
		}

		const int32 RandomIndex = FMath::RandHelper(ValidIDs.Num());
		return ValidIDs[RandomIndex];
	}

	// If simulating-only is requested, the authoritative state is stored in the subsystem.
	UWorld* World = GetWorld();
	if (!World)
	{
		return FPhysXInstanceID();
	}

	UPhysXInstancedWorldSubsystem* Subsystem = CachedSubsystem;
	if (!Subsystem)
	{
		Subsystem = World->GetSubsystem<UPhysXInstancedWorldSubsystem>();
		const_cast<APhysXInstancedMeshActor*>(this)->CachedSubsystem = Subsystem;
	}

	if (!Subsystem)
	{
		return FPhysXInstanceID();
	}

	TArray<FPhysXInstanceID> Candidates;

	for (const FPhysXInstanceID& ID : RegisteredInstanceIDs)
	{
		if (!ID.IsValid())
		{
			continue;
		}

		if (Subsystem->IsInstancePhysicsEnabled(ID))
		{
			Candidates.Add(ID);
		}
	}

	if (Candidates.Num() == 0)
	{
		return FPhysXInstanceID();
	}

	const int32 RandomIndex = FMath::RandHelper(Candidates.Num());
	return Candidates[RandomIndex];
}

/** Resolve instance ID via the subsystem using the component pointer and instance index. */
FPhysXInstanceID APhysXInstancedMeshActor::GetInstanceIDFromSubsystemByIndex(int32 InstanceIndex) const
{
	if (!InstancedMesh || InstanceIndex < 0)
	{
		return FPhysXInstanceID();
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return FPhysXInstanceID();
	}

	UPhysXInstancedWorldSubsystem* Subsystem = CachedSubsystem;
	if (!Subsystem)
	{
		Subsystem = World->GetSubsystem<UPhysXInstancedWorldSubsystem>();
		const_cast<APhysXInstancedMeshActor*>(this)->CachedSubsystem = Subsystem;
	}

	if (!Subsystem)
	{
		return FPhysXInstanceID();
	}

	return Subsystem->GetInstanceIDForComponentAndIndex(InstancedMesh, InstanceIndex);
}

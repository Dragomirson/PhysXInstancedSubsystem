/**
 * Copyright (C) 2025 | Created by NordVader Inc.
 * All rights reserved!
 * My Discord Server: https://discord.gg/B8prpf3vzD
 */

#include "Subsystems/PhysXInstancedWorldSubsystem.h"

// Plugin
#include "Actors/PhysXInstancedMeshActor.h"
#include "Components/PhysXInstancedStaticMeshComponent.h"
#include "Debug/PhysXInstancedStats.h"
#include "PhysXInstancedBody.h"
#include "Processes/PhysXInstancedDefaultProcesses.h"
#include "Processes/PhysXInstancedProcessPipeline.h"
#include "Types/PhysXInstanceEvents.h"
#include "Types/PhysXInstancedTypes.h"

// UE
#include "Async/ParallelFor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "PhysicsEngine/PhysicsSettings.h"

// PhysX support glue
#include "Misc/ScopeExit.h"
#include "PhysXInstancedSubsystem/Public/PhysXSupportCore.h"

#if PHYSICS_INTERFACE_PHYSX
	#include "Debug/PhysXInstancedDebugDraw.h"
	#include "PhysXIncludes.h"
#include "PxRigidBodyExt.h" // PhysX extensions: setMassAndUpdateInertia

	#if __has_include("PhysXPublicCore.h")
		#include "PhysXPublicCore.h"
	#else
		#include "PhysXPublic.h"
	#endif

	using namespace physx;

	// Single shared material (module-wide) with a refcount to survive multiple UWorlds in editor.
	static PxMaterial* GInstancedDefaultMaterial = nullptr;
	static int32       GInstancedDefaultMaterialRefCount = 0;

struct UPhysXInstancedWorldSubsystem::FPhysXInstanceUserData
{
	static constexpr uint32 MagicValue = 0x50584944; // 'PXID'

	uint32           Magic = MagicValue;
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

// ============================================================================
// PhysX globals / console variables
// ============================================================================

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
	if (!World)
	{
		return;
	}

	bool  bPersistent = false;
	float LifeTime = 0.0f;
	float StringDuration = 0.0f;
	MakeDebugDrawParams(Duration, bPersistent, LifeTime, StringDuration);

	DrawDebugLine(World, A, B, Color, bPersistent, LifeTime, 0, Thickness);
}

static void DrawPointSafe(UWorld* World, const FVector& P, const FColor& Color, float Duration, float Size = 12.0f)
{
	if (!World)
	{
		return;
	}

	bool  bPersistent = false;
	float LifeTime = 0.0f;
	float StringDuration = 0.0f;
	MakeDebugDrawParams(Duration, bPersistent, LifeTime, StringDuration);

	DrawDebugPoint(World, P, Size, Color, bPersistent, LifeTime, 0);
}

static void DrawSphereSafe(UWorld* World, const FVector& C, float R, const FColor& Color, float Duration, float Thickness = 1.0f)
{
	if (!World)
	{
		return;
	}

	bool  bPersistent = false;
	float LifeTime = 0.0f;
	float StringDuration = 0.0f;
	MakeDebugDrawParams(Duration, bPersistent, LifeTime, StringDuration);

	DrawDebugSphere(World, C, R, 16, Color, bPersistent, LifeTime, 0, Thickness);
}

static void DrawArrowSafe(UWorld* World, const FVector& From, const FVector& To, const FColor& Color, float Duration, float Thickness = 1.5f)
{
	if (!World)
	{
		return;
	}

	bool  bPersistent = false;
	float LifeTime = 0.0f;
	float StringDuration = 0.0f;
	MakeDebugDrawParams(Duration, bPersistent, LifeTime, StringDuration);

	DrawDebugDirectionalArrow(World, From, To, 12.0f, Color, bPersistent, LifeTime, 0, Thickness);
}

static void DrawTextSafe(UWorld* World, const FVector& At, const FString& Text, const FColor& Color, float Duration)
{
	if (!World)
	{
		return;
	}

	bool  bPersistent = false;
	float LifeTime = 0.0f;
	float StringDuration = 0.0f;
	MakeDebugDrawParams(Duration, bPersistent, LifeTime, StringDuration);

	// Note: DrawDebugString does not use bPersistentLines; it relies on Duration.
	// We treat Duration <= 0 as "infinite" by passing negative duration.
	DrawDebugString(World, At, Text, nullptr, Color, StringDuration, /*bDrawShadow=*/true);
}

#endif // ENABLE_DRAW_DEBUG

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

static bool IsEventEnabled(const APhysXInstancedMeshActor* Owner, EPhysXInstanceEventFlags Flag)
{
	return Owner
		&& Owner->InstanceEventMask != 0
		&& ((Owner->InstanceEventMask & (int32)Flag) != 0);
}

static bool HasInterfaceEvents(const APhysXInstancedMeshActor* Owner)
{
	return Owner
		&& Owner->GetClass()->ImplementsInterface(UPhysXInstanceEvents::StaticClass());
}
	
static bool GetInstanceWorldTransform_Safe(const FPhysXInstanceData& Data, FTransform& OutWorldTM)
{
	OutWorldTM = FTransform::Identity;

#if PHYSICS_INTERFACE_PHYSX
	if (physx::PxRigidActor* RA = Data.Body.GetPxActor())
	{
		const physx::PxTransform PxPose = RA->getGlobalPose();
		OutWorldTM = FTransform(P2UQuat(PxPose.q), P2UVector(PxPose.p), FVector::OneVector);
		return true;
	}
#endif

	UInstancedStaticMeshComponent* ISMC = Data.InstancedComponent.Get();
	if (!ISMC || !ISMC->IsValidLowLevelFast() || Data.InstanceIndex == INDEX_NONE)
	{
		return false;
	}

	return ISMC->GetInstanceTransform(Data.InstanceIndex, OutWorldTM, /*bWorldSpace=*/true);
}

static bool GetInstanceWorldLocation_Safe(const FPhysXInstanceData& Data, FVector& OutLocation)
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

	static void FirePrePhysics(
	APhysXInstancedMeshActor* Owner,
	FPhysXInstanceID ID,
	bool bEnable,
	bool bDestroyBodyIfDisabling)
{
	if (!IsValid(Owner))
	{
		return;
	}

	const bool bWants =
		IsEventEnabled(Owner, EPhysXInstanceEventFlags::PrePhysics) &&
		(Owner->OnInstancePrePhysics.IsBound() || HasInterfaceEvents(Owner));

	if (!bWants)
	{
		return;
	}

	Owner->OnInstancePrePhysics.Broadcast(ID, bEnable, bDestroyBodyIfDisabling);

	if (HasInterfaceEvents(Owner))
	{
		IPhysXInstanceEvents::Execute_OnInstancePrePhysics(Owner, ID, bEnable, bDestroyBodyIfDisabling);
	}
}

	static void FirePostPhysics(
		APhysXInstancedMeshActor* Owner,
		FPhysXInstanceID ID,
		bool bEnable,
		bool bDestroyBodyIfDisabling,
		bool bSuccess)
{
	if (!IsValid(Owner))
	{
		return;
	}

	const bool bWants =
		IsEventEnabled(Owner, EPhysXInstanceEventFlags::PostPhysics) &&
		(Owner->OnInstancePostPhysics.IsBound() || HasInterfaceEvents(Owner));

	if (!bWants)
	{
		return;
	}

	Owner->OnInstancePostPhysics.Broadcast(ID, bEnable, bDestroyBodyIfDisabling, bSuccess);

	if (HasInterfaceEvents(Owner))
	{
		IPhysXInstanceEvents::Execute_OnInstancePostPhysics(Owner, ID, bEnable, bDestroyBodyIfDisabling, bSuccess);
	}
}

} // namespace


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

UPhysXInstancedWorldSubsystem::~UPhysXInstancedWorldSubsystem()
{
	ProcessManager.Reset();
}

void UPhysXInstancedWorldSubsystem::BuildProcessPipeline()
{
	if (!ProcessManager.IsValid())
	{
		ProcessManager = MakeUnique<FPhysXISProcessManager>();
	}

	ProcessManager->Reset();

	PhysXIS::RegisterDefaultProcesses(*ProcessManager);

	FPhysXISProcessContext Ctx;
	Ctx.Subsystem = this;
	Ctx.World     = CachedWorld.Get() ? CachedWorld.Get() : GetWorld();
	Ctx.DeltaTime = 0.f;
	Ctx.SimTime   = 0.f;

	ProcessManager->InitializeAll(Ctx);
}

// ============================================================================
// Stop actions shared by async-step and lifetime (TTL)
// ============================================================================

static_assert((int32)EPhysXInstanceStopAction::None == 0, "EPhysXInstanceStopAction order changed.");
static_assert((int32)EPhysXInstanceStopAction::ConvertToStorage == 4, "EPhysXInstanceStopAction order changed.");

bool UPhysXInstancedWorldSubsystem::HandleStopAction_None(
	FPhysXInstanceID ID,
	const FStopActionExecOptions& Opt,
	FPhysXInstanceData& Data)
{
	return true;
}

bool UPhysXInstancedWorldSubsystem::HandleStopAction_DisableSimulation(
	FPhysXInstanceID ID,
	const FStopActionExecOptions& Opt,
	FPhysXInstanceData& Data)
{
	if (Opt.bUseSetInstancePhysicsEnabled)
	{
		return SetInstancePhysicsEnabled(ID, /*bEnable=*/false, /*bDestroyBodyIfDisabling=*/false);
	}

#if PHYSICS_INTERFACE_PHYSX
	if (physx::PxRigidActor* Actor = Data.Body.GetPxActor())
	{
		if (physx::PxRigidDynamic* RD = Actor->is<physx::PxRigidDynamic>())
		{
			RD->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);
			RD->setActorFlag(PxActorFlag::eDISABLE_SIMULATION, true);
		}
	}
#endif

	Data.bSimulating = false;
	return true;
}

bool UPhysXInstancedWorldSubsystem::HandleStopAction_DestroyBody(
	FPhysXInstanceID ID,
	const FStopActionExecOptions& Opt,
	FPhysXInstanceData& Data)
{
	if (Opt.bUseSetInstancePhysicsEnabled)
	{
		return SetInstancePhysicsEnabled(ID, /*bEnable=*/false, /*bDestroyBodyIfDisabling=*/true);
	}

#if PHYSICS_INTERFACE_PHYSX
	// IMPORTANT(PXIS_DEFERRED_ADD):
	// This instance might still be queued in PendingAddActors for deferred scene insertion.
	// If we destroy the body without invalidating the queue, ProcessPendingAddActors() may call
	// AddActorToScene() on a destroyed/rebound body -> crash or undefined behavior.
	InvalidatePendingAddEntries(ID);

	ClearInstanceUserData(ID);
	Data.Body.Destroy();
#endif

	Data.bSimulating = false;
	return true;
}

bool UPhysXInstancedWorldSubsystem::HandleStopAction_DestroyBodyAndRemoveInstance(
	FPhysXInstanceID ID,
	const FStopActionExecOptions& Opt,
	FPhysXInstanceData& Data)
{
	RemoveInstanceByID_Internal(ID, Opt.bRemoveVisualInstance, Opt.RemoveReason);
	return false;
}

bool UPhysXInstancedWorldSubsystem::HandleStopAction_ConvertToStorage(
	FPhysXInstanceID ID,
	const FStopActionExecOptions& Opt,
	FPhysXInstanceData& Data)
{
	const FPhysXInstanceData* Current = Instances.Find(ID);
	UInstancedStaticMeshComponent* ISMC = Current ? Current->InstancedComponent.Get() : nullptr;
	const APhysXInstancedMeshActor* OwnerActor = ISMC ? Cast<APhysXInstancedMeshActor>(ISMC->GetOwner()) : nullptr;
	const bool bAlreadyStorage = OwnerActor && (OwnerActor->bIsStorageActor || OwnerActor->bStorageOnly);

	if (!bAlreadyStorage)
	{
		const EPhysXInstanceConvertReason ConvertReason = ConvertReasonFromRemoveReason(Opt.RemoveReason);

		if (ConvertInstanceToStaticStorage_Internal(ID, Opt.bCreateStorageActorIfNeeded, ConvertReason))
		{
			if (FPhysXInstanceData* After = Instances.Find(ID))
			{
				After->bSimulating = false;
			}
			return true;
		}

		if (Opt.bDestroyBodyOnConvertFailure)
		{
#if PHYSICS_INTERFACE_PHYSX
			// IMPORTANT(PXIS_DEFERRED_ADD): see comment in HandleStopAction_DestroyBody().
			InvalidatePendingAddEntries(ID);
			
			ClearInstanceUserData(ID);
			Data.Body.Destroy();
#endif
			Data.bSimulating = false;
		}
	}

	return true;
}

#if PHYSICS_INTERFACE_PHYSX

bool UPhysXInstancedWorldSubsystem::HandleInstanceTask_AddImpulse(FPhysXInstanceTask& Task, physx::PxRigidDynamic* RD)
{
	if (!RD)
	{
		return false;
	}

	const physx::PxVec3 PxImpulse = U2PVector(Task.Vector);
	const physx::PxForceMode::Enum Mode = Task.bModeFlag
		? physx::PxForceMode::eVELOCITY_CHANGE
		: physx::PxForceMode::eIMPULSE;

	RD->addForce(PxImpulse, Mode, /*autowake=*/true);
	return true;
}

bool UPhysXInstancedWorldSubsystem::HandleInstanceTask_AddForce(FPhysXInstanceTask& Task, physx::PxRigidDynamic* RD)
{
	if (!RD)
	{
		return false;
	}

	const physx::PxVec3 PxForce = U2PVector(Task.Vector);
	const physx::PxForceMode::Enum Mode = Task.bModeFlag
		? physx::PxForceMode::eACCELERATION
		: physx::PxForceMode::eFORCE;

	RD->addForce(PxForce, Mode, /*autowake=*/true);
	return true;
}

bool UPhysXInstancedWorldSubsystem::HandleInstanceTask_PutToSleep(FPhysXInstanceTask& Task, physx::PxRigidDynamic* RD)
{
	if (!RD)
	{
		return false;
	}

	RD->putToSleep();
	return true;
}

bool UPhysXInstancedWorldSubsystem::HandleInstanceTask_WakeUp(FPhysXInstanceTask& Task, physx::PxRigidDynamic* RD)
{
	if (!RD)
	{
		return false;
	}

	RD->wakeUp();
	return true;
}


#endif // PHYSICS_INTERFACE_PHYSX

// ============================================================================
// UWorldSubsystem interface
// ============================================================================

void UPhysXInstancedWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	CachedWorld = GetWorld();

	// Reset runtime state.
	LifetimeHeap.Reset();

	PendingInstanceTasks.Reset();
	InstanceIDBySlot.Reset();

#if PHYSICS_INTERFACE_PHYSX
	PendingAddActorsHead = 0;
	PendingAddActors.Reset();
#endif

	Instances.Reset();
	NextID = 1;

	Actors.Reset();
	NextActorID = 1;

	NumBodiesLifetimeCreated = 0;
	NumBodiesTotal           = 0;
	NumBodiesSimulating      = 0;
	NumBodiesSleeping        = 0;

#if PHYSICS_INTERFACE_PHYSX
	// Create one default material shared across worlds; keep it alive via refcount.
	if (GPhysXSDK)
	{
		if (!GInstancedDefaultMaterial)
		{
			const PxReal StaticFriction  = 0.6f;
			const PxReal DynamicFriction = 0.6f;
			const PxReal Restitution     = 0.1f;

			GInstancedDefaultMaterial = GPhysXSDK->createMaterial(
				StaticFriction,
				DynamicFriction,
				Restitution);
		}

		if (GInstancedDefaultMaterial)
		{
			++GInstancedDefaultMaterialRefCount;
		}
	}
#endif // PHYSICS_INTERFACE_PHYSX
	BuildProcessPipeline();
}

void UPhysXInstancedWorldSubsystem::Deinitialize()
{
	// Stop any deferred work first.
	PendingInstanceTasks.Reset();
	LifetimeHeap.Reset();

#if PHYSICS_INTERFACE_PHYSX
	PendingAddActorsHead = 0;
	PendingAddActors.Reset();

	for (TPair<FPhysXInstanceID, FPhysXInstanceData>& Pair : Instances)
	{
		ClearInstanceUserData(Pair.Key);
		Pair.Value.Body.Destroy();
	}

	UserDataByID.Reset();

	// Release shared material only when the last world subsystem goes away.
	if (GInstancedDefaultMaterial)
	{
		GInstancedDefaultMaterialRefCount = FMath::Max(0, GInstancedDefaultMaterialRefCount - 1);
		if (GInstancedDefaultMaterialRefCount == 0)
		{
			GInstancedDefaultMaterial->release();
			GInstancedDefaultMaterial = nullptr;
		}
	}
#endif // PHYSICS_INTERFACE_PHYSX

	Instances.Reset();
	Actors.Reset();
	InstanceIDBySlot.Reset();
	CachedWorld.Reset();

	if (ProcessManager.IsValid())
	{
		FPhysXISProcessContext Ctx;
		Ctx.Subsystem = this;
		Ctx.World     = CachedWorld.Get() ? CachedWorld.Get() : GetWorld();
		Ctx.DeltaTime = 0.f;
		Ctx.SimTime   = 0.f;

		ProcessManager->DeinitializeAll(Ctx);
		ProcessManager.Reset();
	}

	Super::Deinitialize();
}

// ============================================================================
// UTickableWorldSubsystem interface
// ============================================================================

void UPhysXInstancedWorldSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	float SimTime = DeltaTime;
	if (const UPhysicsSettings* PhysSettings = UPhysicsSettings::Get())
	{
		const float MaxPhysDt = PhysSettings->MaxPhysicsDeltaTime;
		if (MaxPhysDt > 0.0f)
		{
			SimTime = FMath::Min(SimTime, MaxPhysDt);
		}
	}
	SimTime = FMath::Max(0.0f, SimTime);

	if (!ProcessManager.IsValid())
	{
		BuildProcessPipeline();
	}

	if (ProcessManager.IsValid())
	{
		FPhysXISProcessContext Ctx;
		Ctx.Subsystem = this;
		Ctx.World     = CachedWorld.Get() ? CachedWorld.Get() : GetWorld();
		Ctx.DeltaTime = DeltaTime;
		Ctx.SimTime   = SimTime;

		ProcessManager->TickAll(Ctx);
		return;
	}

#if PHYSICS_INTERFACE_PHYSX
	ProcessPendingAddActors();
	ProcessInstanceTasks();
#endif

	AsyncPhysicsStep(DeltaTime, SimTime);
	ProcessLifetimeExpirations();
}

TStatId UPhysXInstancedWorldSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UPhysXInstancedSubsystem, STATGROUP_Tickables);
}

// ============================================================================
// Lifetime (TTL)
// ============================================================================

int32 UPhysXInstancedWorldSubsystem::ApplyActorLifetimeDefaults(APhysXInstancedMeshActor* Actor, bool bForce)
{
	if (!Actor)
	{
		return 0;
	}

	const float Now = GetWorldTimeSecondsSafe();

	const bool bEnable = Actor->bEnableLifetime && (Actor->DefaultLifeTimeSeconds > 0.0f);
	const float LifetimeSeconds = Actor->DefaultLifeTimeSeconds;
	const EPhysXInstanceStopAction Action = Actor->DefaultLifetimeAction;

	int32 Updated = 0;

	for (const FPhysXInstanceID& ID : Actor->RegisteredInstanceIDs)
	{
		if (!ID.IsValid())
		{
			continue;
		}

		FPhysXInstanceData* Data = Instances.Find(ID);
		if (!Data)
		{
			continue;
		}

		if (bEnable)
		{
			if (bForce || !Data->bHasLifetime)
			{
				SetInstanceLifetime_Internal(ID, Now, LifetimeSeconds, Action);
				++Updated;
			}
		}
		else
		{
			if (bForce || Data->bHasLifetime)
			{
				DisableInstanceLifetime_Internal(ID);
				++Updated;
			}
		}
	}

	return Updated;
}

void UPhysXInstancedWorldSubsystem::ApplyDefaultLifetimeForNewInstance(
	FPhysXInstanceID ID,
	UInstancedStaticMeshComponent* InstancedMesh)
{
	if (!ID.IsValid() || !InstancedMesh)
	{
		return;
	}

	const APhysXInstancedMeshActor* OwnerActor =
		Cast<APhysXInstancedMeshActor>(InstancedMesh->GetOwner());

	if (!OwnerActor || !OwnerActor->bEnableLifetime)
	{
		return;
	}

	const float LifetimeSeconds = OwnerActor->DefaultLifeTimeSeconds;
	if (LifetimeSeconds <= 0.0f)
	{
		return;
	}

	SetInstanceLifetime_Internal(
		ID,
		GetWorldTimeSecondsSafe(),
		LifetimeSeconds,
		OwnerActor->DefaultLifetimeAction);
}

void UPhysXInstancedWorldSubsystem::ApplyLifetimeOverrideForNewInstance(
	FPhysXInstanceID ID,
	const FPhysXSpawnInstanceRequest& Request)
{
	if (!ID.IsValid())
	{
		return;
	}

	if (!Request.bOverrideLifetime)
	{
		return;
	}

	const float LifetimeSeconds = Request.LifeTimeSeconds;
	if (LifetimeSeconds <= 0.0f)
	{
		DisableInstanceLifetime_Internal(ID);
		return;
	}

	SetInstanceLifetime_Internal(
		ID,
		GetWorldTimeSecondsSafe(),
		LifetimeSeconds,
		Request.LifetimeAction);
}

void UPhysXInstancedWorldSubsystem::SetInstanceLifetime_Internal(
	FPhysXInstanceID ID,
	float NowSeconds,
	float LifetimeSeconds,
	EPhysXInstanceStopAction Action)
{
	FPhysXInstanceData* Data = Instances.Find(ID);
	if (!Data)
	{
		return;
	}

	if (LifetimeSeconds <= 0.0f)
	{
		DisableInstanceLifetime_Internal(ID);
		return;
	}

	++Data->LifetimeSerial;
	Data->bHasLifetime   = true;
	Data->ExpireAt       = NowSeconds + LifetimeSeconds;
	Data->LifetimeAction = Action;

	FLifetimeHeapEntry Entry;
	Entry.ExpireAt = Data->ExpireAt;
	Entry.ID       = ID;
	Entry.Serial   = Data->LifetimeSerial;

	LifetimeHeap.HeapPush(Entry, FLifetimeHeapPred());
	
	//UE_LOG(LogTemp, Warning, TEXT("[TTL] Set ID=%u ExpireAt=%.3f Action=%d Serial=%u"),
	//ID.GetUniqueID(), Data->ExpireAt, (int32)Action, Data->LifetimeSerial);
}

void UPhysXInstancedWorldSubsystem::DisableInstanceLifetime_Internal(FPhysXInstanceID ID)
{
	FPhysXInstanceData* Data = Instances.Find(ID);
	if (!Data)
	{
		return;
	}

	++Data->LifetimeSerial;
	Data->bHasLifetime   = false;
	Data->ExpireAt       = 0.0f;
	Data->LifetimeAction = EPhysXInstanceStopAction::None;
}

void UPhysXInstancedWorldSubsystem::ProcessLifetimeExpirations()
{
	if (LifetimeHeap.Num() == 0)
	{
		return;
	}

	const float Now = GetWorldTimeSecondsSafe();
	
	//UE_LOG(LogTemp, Warning, TEXT("[TTL] Tick Now=%.3f Heap=%d TopExpire=%.3f"),
	//Now, LifetimeHeap.Num(), (LifetimeHeap.Num() > 0) ? LifetimeHeap.HeapTop().ExpireAt : -1.0f);
	
	const int32 MaxToProcess = (MaxLifetimeExpirationsPerTick <= 0) ? MAX_int32 : MaxLifetimeExpirationsPerTick;

	struct FExpiredLifetime
	{
		FPhysXInstanceID         ID;
		EPhysXInstanceStopAction Action = EPhysXInstanceStopAction::None;
	};

	TArray<FExpiredLifetime> Expired;
	Expired.Reserve(64);

	int32 Processed = 0;

	while (LifetimeHeap.Num() > 0 && Processed < MaxToProcess)
	{
		const FLifetimeHeapEntry& Top = LifetimeHeap.HeapTop();
		if (Top.ExpireAt > Now)
		{
			break;
		}

		FLifetimeHeapEntry Entry;
		LifetimeHeap.HeapPop(Entry, FLifetimeHeapPred(), /*bAllowShrinking=*/false);

		FPhysXInstanceData* Data = Instances.Find(Entry.ID);
		if (!Data)
		{
			continue;
		}

		if (!Data->bHasLifetime)
		{
			continue;
		}

		if (Data->LifetimeSerial != Entry.Serial)
		{
			continue;
		}

		if (Data->ExpireAt != Entry.ExpireAt)
		{
			continue;
		}

		FExpiredLifetime& Out = Expired.AddDefaulted_GetRef();
		Out.ID     = Entry.ID;
		Out.Action = Data->LifetimeAction;

		// Disable lifetime before doing anything potentially destructive.
		++Data->LifetimeSerial;
		Data->bHasLifetime   = false;
		Data->ExpireAt       = 0.0f;
		Data->LifetimeAction = EPhysXInstanceStopAction::None;

		++Processed;
	}

	for (const FExpiredLifetime& Item : Expired)
	{
		ApplyLifetimeAction(Item.ID, Item.Action);
	}
}

void UPhysXInstancedWorldSubsystem::ApplyLifetimeAction(FPhysXInstanceID ID, EPhysXInstanceStopAction Action)
{
	//UE_LOG(LogTemp, Warning, TEXT("[TTL] EXPIRED ID=%u Action=%d"), ID.GetUniqueID(), (int32)Action);

	FStopActionExecOptions Opt;
	Opt.RemoveReason               = EPhysXInstanceRemoveReason::Expired;
	Opt.bRemoveVisualInstance      = true;
	Opt.bCreateStorageActorIfNeeded = true;

	Opt.bUseSetInstancePhysicsEnabled = true;   // TTL uses the high-level API
	Opt.bResetTimers                 = false;  // TTL doesn't care about stop timers
	Opt.bDestroyBodyOnConvertFailure = false;  // matches your current TTL behavior

	ExecuteInstanceStopAction_Internal(ID, Action, Opt);
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
	// Apply per-spawn lifetime overrides (actor defaults are handled during registration).
	ApplyLifetimeOverrideForNewInstance(NewInstanceID, Request);


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

	FPhysXInstanceData NewData{};
	NewData.InstancedComponent = InstancedMesh;
	NewData.InstanceIndex      = InstanceIndex;
	NewData.bSimulating        = bSimulate;
	NewData.SleepTime          = 0.0f;
	NewData.FallTime           = 0.0f;
	NewData.bWasSleeping       = false;
	NewData.bHasLifetime   = false;
	NewData.ExpireAt       = 0.0f;
	NewData.LifetimeAction = EPhysXInstanceStopAction::None;
	NewData.LifetimeSerial = 0;

#if !PHYSICS_INTERFACE_PHYSX
	// Without PhysX, only bookkeeping data is stored.
	Instances.Add(NewID, NewData);
	AddSlotMapping(NewID);
	ApplyDefaultLifetimeForNewInstance(NewID, InstancedMesh);
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
		ApplyDefaultLifetimeForNewInstance(NewID, InstancedMesh);
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
	
	// Apply actor-level overrides (mass/damping) before storing/enqueueing.
	if (const APhysXInstancedMeshActor* OwnerActor = Cast<APhysXInstancedMeshActor>(InstancedMesh->GetOwner()))
	{
		if (physx::PxRigidActor* RA = NewData.Body.GetPxActor())
		{
			if (physx::PxRigidDynamic* RD = RA->is<physx::PxRigidDynamic>())
			{
				ApplyOwnerPhysicsOverrides(OwnerActor, InstancedMesh, OverrideMesh, RD);
			}
		}
	}
	
	// IMPORTANT:
	// userData setup requires the instance record to exist in Instances.
	Instances.Add(NewID, NewData);
	AddSlotMapping(NewID);
	ApplyDefaultLifetimeForNewInstance(NewID, InstancedMesh);
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

			FPhysXInstanceData NewData{};
			NewData.InstancedComponent = InstancedMesh;
			NewData.InstanceIndex      = InstanceIndex;
			NewData.bSimulating        = bSimulate;
			NewData.SleepTime          = 0.0f;
			NewData.FallTime           = 0.0f;
			NewData.bWasSleeping       = false;
			NewData.bHasLifetime   = false;
			NewData.ExpireAt       = 0.0f;
			NewData.LifetimeAction = EPhysXInstanceStopAction::None;
			NewData.LifetimeSerial = 0;

			FPhysXInstanceData& StoredData = Instances.Add(NewID, NewData);
			AddSlotMapping(NewID);
			ApplyDefaultLifetimeForNewInstance(NewID, InstancedMesh);

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

			// After success, apply overrides on the game thread.
			if (const APhysXInstancedMeshActor* OwnerActor = Cast<APhysXInstancedMeshActor>(Job.ISMC ? Job.ISMC->GetOwner() : nullptr))
			{
				if (physx::PxRigidActor* RA = Job.Data ? Job.Data->Body.GetPxActor() : nullptr)
				{
					if (physx::PxRigidDynamic* RD = RA->is<physx::PxRigidDynamic>())
					{
						ApplyOwnerPhysicsOverrides(OwnerActor, InstancedMesh, OverrideMesh, RD);
					}
				}
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
		EPhysXInstanceRemoveReason      RemoveReason = EPhysXInstanceRemoveReason::AutoStop;
		EPhysXInstanceStopAction       ActionToApply = EPhysXInstanceStopAction::None;

		// CCD decisions.
		bool                           bEnableCCD  = false;
		bool                           bDisableCCD = false;

		// Velocity caches (computed only when required by rules).
		FVector                        CachedLinearVelocityU = FVector::ZeroVector;
		float                          CachedLinearSpeed = 0.0f;
		float                          CachedAngularSpeedDeg = 0.0f;
		
		// Updated timers.
		float                          NewSleepTime = 0.0f;
		float                          NewFallTime  = 0.0f;
	};

	// Reused every frame to avoid per-tick allocations in AsyncPhysicsStep.
	static TArray<FPhysXInstanceAsyncStepJob> GAsyncStepJobs;
	
	using FPhysXISAsyncPreComputeRuleFn  = bool (*)(float /*TimerDelta*/, FPhysXInstanceAsyncStepJob& /*Job*/);
using FPhysXISAsyncPostComputeRuleFn = void (*)(float /*TimerDelta*/, FPhysXInstanceAsyncStepJob& /*Job*/);
using FPhysXISAsyncPostApplyRuleFn   = void (*)(UPhysXInstancedWorldSubsystem& /*Subsystem*/, float /*TimerDelta*/, TArray<FPhysXInstanceAsyncStepJob>& /*Jobs*/);

static const FPhysXISAsyncPreComputeRuleFn  GPhysXISAsyncPreComputeRules[]  = { nullptr };
static const FPhysXISAsyncPostComputeRuleFn GPhysXISAsyncPostComputeRules[] = { nullptr };
static const FPhysXISAsyncPostApplyRuleFn   GPhysXISAsyncPostApplyRules[]   = { nullptr };

static FORCEINLINE bool RunAsyncPreComputeRules(float TimerDelta, FPhysXInstanceAsyncStepJob& Job)
{
	for (int32 Index = 0; GPhysXISAsyncPreComputeRules[Index] != nullptr; ++Index)
	{
		if (!GPhysXISAsyncPreComputeRules[Index](TimerDelta, Job))
		{
			return false;
		}
	}
	return true;
}

static FORCEINLINE void RunAsyncPostComputeRules(float TimerDelta, FPhysXInstanceAsyncStepJob& Job)
{
	for (int32 Index = 0; GPhysXISAsyncPostComputeRules[Index] != nullptr; ++Index)
	{
		GPhysXISAsyncPostComputeRules[Index](TimerDelta, Job);
	}
}

static FORCEINLINE void RunAsyncPostApplyRules(UPhysXInstancedWorldSubsystem& Subsystem, float TimerDelta, TArray<FPhysXInstanceAsyncStepJob>& Jobs)
{
	for (int32 Index = 0; GPhysXISAsyncPostApplyRules[Index] != nullptr; ++Index)
	{
		GPhysXISAsyncPostApplyRules[Index](Subsystem, TimerDelta, Jobs);
	}
}

using FPhysXISAsyncComputeRuleFn = bool (*)(float /*TimerDelta*/, FPhysXInstanceAsyncStepJob& /*Job*/);

static bool ComputeRule_InitAndFastPath(float TimerDelta, FPhysXInstanceAsyncStepJob& Job)
{
	if (!Job.Data || !Job.RigidDynamic)
	{
		return false;
	}

	FPhysXInstanceData* InstanceData = Job.Data;

	const bool bSleepingNow = Job.RigidDynamic->isSleeping();
	Job.bSleeping = bSleepingNow;

	Job.bApplyStopAction = false;
	Job.ActionToApply    = EPhysXInstanceStopAction::None;
	Job.bEnableCCD       = false;
	Job.bDisableCCD      = false;

	Job.CachedLinearVelocityU = FVector::ZeroVector;
	Job.CachedLinearSpeed     = 0.0f;
	Job.CachedAngularSpeedDeg = 0.0f;

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
		return false;
	}

	return true;
}

static bool ComputeRule_ReadPose(float /*TimerDelta*/, FPhysXInstanceAsyncStepJob& Job)
{
	const PxTransform PxPose = Job.RigidDynamic->getGlobalPose();

	const FVector ULocation = P2UVector(PxPose.p);
	const FQuat   URotation = P2UQuat(PxPose.q);

	Job.NewWorldTransform = FTransform(URotation, ULocation, FVector::OneVector);
	Job.NewLocation       = ULocation;

	return true;
}

static bool ComputeRule_CustomKillZ(float /*TimerDelta*/, FPhysXInstanceAsyncStepJob& Job)
{
	if (Job.bUseCustomKillZ && Job.NewLocation.Z < Job.CustomKillZ)
	{
		if (Job.LostInstanceAction != EPhysXInstanceStopAction::None)
		{
			Job.bApplyStopAction = true;
			Job.ActionToApply    = Job.LostInstanceAction;
		}

		Job.NewSleepTime = 0.0f;
		Job.NewFallTime  = 0.0f;
		Job.RemoveReason = EPhysXInstanceRemoveReason::KillZ;
		return false;
	}

	return true;
}

static bool ComputeRule_AutoStopDisabled(float TimerDelta, FPhysXInstanceAsyncStepJob& Job)
{
	if (!Job.StopConfig.bEnableAutoStop ||
		Job.StopConfig.Action == EPhysXInstanceStopAction::None)
	{
		const bool bSleepingNow = Job.bSleeping;

		FPhysXInstanceData* InstanceData = Job.Data;

		Job.NewSleepTime = bSleepingNow
			? (InstanceData->SleepTime + TimerDelta)
			: 0.0f;

		Job.NewFallTime = 0.0f;
		return false;
	}

	return true;
}

static bool ComputeRule_ReadVelocitiesAndCCD(float /*TimerDelta*/, FPhysXInstanceAsyncStepJob& Job)
{
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
		Job.CachedLinearVelocityU = P2UVector(LinVelPx);
		Job.CachedLinearSpeed     = Job.CachedLinearVelocityU.Size();

		if (bNeedAngularSpeed)
		{
			const PxVec3 AngVelPx    = Job.RigidDynamic->getAngularVelocity();
			const float  AngSpeedRad = AngVelPx.magnitude();
			Job.CachedAngularSpeedDeg = FMath::RadiansToDegrees(AngSpeedRad);
		}
	}

	if (Job.CCDConfig.Mode == EPhysXInstanceCCDMode::AutoByVelocity)
	{
		const float MinVel = Job.CCDConfig.MinCCDVelocity;
		const bool  bShouldUseCCD = (Job.CachedLinearSpeed >= MinVel);

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

	return true;
}

static bool ComputeRule_MaxFallTime(float TimerDelta, FPhysXInstanceAsyncStepJob& Job)
{
	if (Job.StopConfig.bUseMaxFallTime)
	{
		if (Job.CachedLinearVelocityU.Z < 0.0f)
		{
			Job.NewFallTime += TimerDelta;
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
			return false;
		}
	}
	else
	{
		Job.NewFallTime = 0.0f;
	}

	return true;
}

static bool ComputeRule_MaxDistanceFromActor(float /*TimerDelta*/, FPhysXInstanceAsyncStepJob& Job)
{
	if (Job.StopConfig.bUseMaxDistanceFromActor &&
		Job.bHasOwnerLocation &&
		Job.StopConfig.MaxDistanceFromActor > 0.0f &&
		Job.StopConfig.Action != EPhysXInstanceStopAction::None)
	{
		const float MaxDistSq = FMath::Square(Job.StopConfig.MaxDistanceFromActor);
		const float DistSq    = FVector::DistSquared(Job.OwnerLocation, Job.NewLocation);

		if (DistSq > MaxDistSq)
		{
			Job.bApplyStopAction = true;
			Job.ActionToApply    = Job.StopConfig.Action;
			Job.NewSleepTime     = 0.0f;
			Job.NewFallTime      = 0.0f;
			return false;
		}
	}

	return true;
}

static bool ComputeRule_StopCondition(float TimerDelta, FPhysXInstanceAsyncStepJob& Job)
{
	const bool bSleepingNow = Job.bSleeping;

	const bool bBelowVelocityThreshold =
		(Job.CachedLinearSpeed <= Job.StopConfig.LinearSpeedThreshold) &&
		(Job.CachedAngularSpeedDeg <= Job.StopConfig.AngularSpeedThreshold);

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
		return false;
	}

	Job.NewSleepTime += TimerDelta;
	if (Job.NewSleepTime >= Job.StopConfig.MinStoppedTime)
	{
		Job.bApplyStopAction = true;
		Job.ActionToApply    = Job.StopConfig.Action;
		Job.NewSleepTime     = 0.0f;
		Job.NewFallTime      = 0.0f;
	}

	return false;
}

static const FPhysXISAsyncComputeRuleFn GPhysXISAsyncComputeRules[] =
{
	&ComputeRule_InitAndFastPath,
	&ComputeRule_ReadPose,
	&ComputeRule_CustomKillZ,
	&ComputeRule_AutoStopDisabled,
	&ComputeRule_ReadVelocitiesAndCCD,
	&ComputeRule_MaxFallTime,
	&ComputeRule_MaxDistanceFromActor,
	&ComputeRule_StopCondition,
	nullptr
};

static void ComputeAsyncStep_Core(float TimerDelta, FPhysXInstanceAsyncStepJob& Job)
{
	SCOPE_CYCLE_COUNTER(STAT_PhysXInstanced_AsyncJobWorker);

	for (int32 Index = 0; GPhysXISAsyncComputeRules[Index] != nullptr; ++Index)
	{
		if (!GPhysXISAsyncComputeRules[Index](TimerDelta, Job))
		{
			break;
		}
	}
}
}

bool UPhysXInstancedWorldSubsystem::ExecuteInstanceStopAction_Internal(
	FPhysXInstanceID ID,
	EPhysXInstanceStopAction Action,
	const FStopActionExecOptions& Opt)
{
	if (Action == EPhysXInstanceStopAction::None)
	{
		return Instances.Contains(ID);
	}

	FPhysXInstanceData* Data = Instances.Find(ID);
	if (!Data)
	{
		return false;
	}

	using FHandler = bool (UPhysXInstancedWorldSubsystem::*)(
		FPhysXInstanceID,
		const FStopActionExecOptions&,
		FPhysXInstanceData&);

	static const FHandler Handlers[] =
	{
		&UPhysXInstancedWorldSubsystem::HandleStopAction_None,                       // None
		&UPhysXInstancedWorldSubsystem::HandleStopAction_DisableSimulation,          // DisableSimulation
		&UPhysXInstancedWorldSubsystem::HandleStopAction_DestroyBody,                // DestroyBody
		&UPhysXInstancedWorldSubsystem::HandleStopAction_DestroyBodyAndRemoveInstance,// DestroyBodyAndRemoveInstance
		&UPhysXInstancedWorldSubsystem::HandleStopAction_ConvertToStorage            // ConvertToStorage
	};

	const int32 ActionIndex = (int32)Action;
	if (ActionIndex < 0 || ActionIndex >= UE_ARRAY_COUNT(Handlers) || !Handlers[ActionIndex])
	{
		return true;
	}

	const bool bStillExists = (this->*Handlers[ActionIndex])(ID, Opt, *Data);
	if (!bStillExists)
	{
		return false;
	}

	if (Opt.bResetTimers)
	{
		if (FPhysXInstanceData* After = Instances.Find(ID))
		{
			After->SleepTime = 0.0f;
			After->FallTime  = 0.0f;
		}
	}

	return true;
}

#endif // PHYSICS_INTERFACE_PHYSX

// ============================================================================
// Physics update
// ============================================================================
void UPhysXInstancedWorldSubsystem::PhysicsStep_ApplyStopActionsAndCCD()
{
	if (!bPhysicsStepHasPendingApply)
	{
		return;
	}

	SCOPE_CYCLE_COUNTER(STAT_PhysXInstanced_AsyncApply);

	TArray<FPhysXInstanceAsyncStepJob>& Jobs = GAsyncStepJobs;

	for (FPhysXInstanceAsyncStepJob& JobData : Jobs)
	{
		FPhysXInstanceData* InstanceData = JobData.Data;
		if (!InstanceData)
		{
			continue;
		}

		if (JobData.bEnableCCD && JobData.RigidDynamic)
		{
			JobData.RigidDynamic->setRigidBodyFlag(PxRigidBodyFlag::eENABLE_CCD, true);
		}
		else if (JobData.bDisableCCD && JobData.RigidDynamic)
		{
			JobData.RigidDynamic->setRigidBodyFlag(PxRigidBodyFlag::eENABLE_CCD, false);
		}

		if (JobData.bApplyStopAction && JobData.ActionToApply != EPhysXInstanceStopAction::None)
		{
			FStopActionExecOptions Opt;
			Opt.RemoveReason                = JobData.RemoveReason;
			Opt.bRemoveVisualInstance       = true;
			Opt.bCreateStorageActorIfNeeded = true;

			Opt.bUseSetInstancePhysicsEnabled = false;
			Opt.bResetTimers                 = true;
			Opt.bDestroyBodyOnConvertFailure = true;

			const bool bStillExists = ExecuteInstanceStopAction_Internal(JobData.ID, JobData.ActionToApply, Opt);

			if (!bStillExists || JobData.ActionToApply == EPhysXInstanceStopAction::ConvertToStorage)
			{
				JobData.Data         = nullptr;
				JobData.ISMC         = nullptr;
				JobData.RigidDynamic = nullptr;
				continue;
			}
		}
		else
		{
			InstanceData->SleepTime = JobData.NewSleepTime;
			InstanceData->FallTime  = JobData.NewFallTime;
		}

		if (JobData.bSleeping)
		{
			++PhysicsStepLocalSleeping;
		}

		InstanceData->bWasSleeping = JobData.bSleeping;
	}
}

void UPhysXInstancedWorldSubsystem::PhysicsStep_ApplyTransformSync()
{
	if (!bPhysicsStepHasPendingApply)
	{
		return;
	}

	SCOPE_CYCLE_COUNTER(STAT_PhysXInstanced_AsyncApply);

	TArray<FPhysXInstanceAsyncStepJob>& Jobs = GAsyncStepJobs;

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

		if (InstanceData->InstanceIndex == INDEX_NONE)
		{
			continue;
		}

		const bool bWasSleeping = JobData.bWasSleepingInitial;
		const bool bIsSleeping  = JobData.bSleeping;

		if (bIsSleeping && bWasSleeping)
		{
			continue;
		}

		if (UPhysXInstancedStaticMeshComponent* PhysXISMC = Cast<UPhysXInstancedStaticMeshComponent>(ISMComponent))
		{
			FPhysicsStepTransformBatch& Batch = PhysicsStepApplyCtx.ComponentBatches.FindOrAdd(PhysXISMC);
			Batch.InstanceIndices.Add(InstanceData->InstanceIndex);
			Batch.WorldTransforms.Add(JobData.NewWorldTransform);
		}
		else
		{
			ISMComponent->UpdateInstanceTransform(
				InstanceData->InstanceIndex,
				JobData.NewWorldTransform,
				/*bWorldSpace=*/true,
				/*bMarkRenderStateDirty=*/false,
				/*bTeleport=*/false);

			PhysicsStepApplyCtx.DirtyComponents.Add(ISMComponent);
		}
	}

	for (auto& Pair : PhysicsStepApplyCtx.ComponentBatches)
	{
		UPhysXInstancedStaticMeshComponent* PhysXISMC = Pair.Key;
		FPhysicsStepTransformBatch&         Batch     = Pair.Value;

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
}

void UPhysXInstancedWorldSubsystem::PhysicsStep_Finalize()
{
	if (!bPhysicsStepHasPendingApply)
	{
		return;
	}

	for (UInstancedStaticMeshComponent* ISMC : PhysicsStepApplyCtx.DirtyComponents)
	{
		if (ISMC && ISMC->IsValidLowLevelFast())
		{
			ISMC->MarkRenderStateDirty();
		}
	}

	TArray<FPhysXInstanceAsyncStepJob>& Jobs = GAsyncStepJobs;
	RunAsyncPostApplyRules(*this, PhysicsStepTimerDelta, Jobs);

	NumBodiesTotal      = PhysicsStepLocalTotal;
	NumBodiesSleeping   = PhysicsStepLocalSleeping;
	NumBodiesSimulating = NumBodiesTotal - NumBodiesSleeping;

	SET_DWORD_STAT(STAT_PhysXInstanced_BodiesTotal,      NumBodiesTotal);
	SET_DWORD_STAT(STAT_PhysXInstanced_BodiesSimulating, NumBodiesSimulating);
	SET_DWORD_STAT(STAT_PhysXInstanced_BodiesSleeping,   NumBodiesSleeping);

	const uint32 LifetimeClamped = (uint32)FMath::Min<uint64>(NumBodiesLifetimeCreated, (uint64)MAX_uint32);
	SET_DWORD_STAT(STAT_PhysXInstanced_BodiesLifetimeCreated, LifetimeClamped);

	bPhysicsStepHasPendingApply = false;
}


void UPhysXInstancedWorldSubsystem::AsyncPhysicsStep(float DeltaTime, float SimTime)
{
#if !PHYSICS_INTERFACE_PHYSX
	return;
#else
	PhysicsStep_Compute(DeltaTime, SimTime);
	PhysicsStep_ApplyStopActionsAndCCD();
	PhysicsStep_ApplyTransformSync();
	PhysicsStep_Finalize();
#endif
}

void UPhysXInstancedWorldSubsystem::PhysicsStep_Compute(float DeltaTime, float SimTime)
{
	const float TimerDelta = FMath::Max(0.0f, SimTime);

	PhysicsStepTimerDelta       = TimerDelta;
	bPhysicsStepHasPendingApply = false;

	TArray<FPhysXInstanceAsyncStepJob>& Jobs = GAsyncStepJobs;
	Jobs.Reset();

	PhysicsStepApplyCtx.Reset(0);

	if (Instances.Num() == 0)
	{
		NumBodiesTotal      = 0;
		NumBodiesSimulating = 0;
		NumBodiesSleeping   = 0;

		SET_DWORD_STAT(STAT_PhysXInstanced_BodiesTotal,      0);
		SET_DWORD_STAT(STAT_PhysXInstanced_BodiesSimulating, 0);
		SET_DWORD_STAT(STAT_PhysXInstanced_BodiesSleeping,   0);
		SET_DWORD_STAT(STAT_PhysXInstanced_JobsPerFrame,     0);
		SET_DWORD_STAT(STAT_PhysXInstanced_InstancesTotal,   0);

		const uint32 LifetimeClamped = (uint32)FMath::Min<uint64>(NumBodiesLifetimeCreated, (uint64)MAX_uint32);
		SET_DWORD_STAT(STAT_PhysXInstanced_BodiesLifetimeCreated, LifetimeClamped);
		return;
	}

	SET_DWORD_STAT(STAT_PhysXInstanced_InstancesTotal, Instances.Num());

	SCOPE_CYCLE_COUNTER(STAT_PhysXInstanced_AsyncPhysicsStep);

	int32 LocalTotal    = 0;
	int32 LocalSleeping = 0;

	PxScene* PxScenePtr = nullptr;
	if (UWorld* World = GetWorld())
	{
		PxScenePtr = GetPhysXSceneFromWorld(World);
	}

	TSet<PxRigidActor*> ActiveActorsSet;
	int32 NumActiveActorsFromScene = 0;

	if (PxScenePtr)
	{
		PxU32     NumActive   = 0;
		PxActor** ActiveArray = PxScenePtr->getActiveActors(NumActive);

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
	{
		TMap<TPair<UInstancedStaticMeshComponent*, int32>, FPhysXInstanceID> SlotOwners;

		for (const TPair<FPhysXInstanceID, FPhysXInstanceData>& Pair : Instances)
		{
			const FPhysXInstanceData& DataCheck = Pair.Value;
			UInstancedStaticMeshComponent* ISMC = DataCheck.InstancedComponent.Get();
			if (!ISMC || DataCheck.InstanceIndex == INDEX_NONE)
			{
				continue;
			}

			const TPair<UInstancedStaticMeshComponent*, int32> Key(ISMC, DataCheck.InstanceIndex);
			if (const FPhysXInstanceID* Existing = SlotOwners.Find(Key))
			{
				ensureMsgf(false, TEXT("Duplicate ISM slot owner: ID=%u and ID=%u on Component=%s Index=%d"),
					Existing->GetUniqueID(), Pair.Key.GetUniqueID(),
					*GetNameSafe(ISMC), DataCheck.InstanceIndex);
			}
			else
			{
				SlotOwners.Add(Key, Pair.Key);
			}
		}
	}
#endif

	Jobs.Reserve(Instances.Num());

	int32 NumJobsAdded = 0;

	for (TPair<FPhysXInstanceID, FPhysXInstanceData>& Pair : Instances)
	{
		const FPhysXInstanceID ID = Pair.Key;
		FPhysXInstanceData& InstanceData = Pair.Value;

		if (!InstanceData.bSimulating)
		{
			continue;
		}

		physx::PxRigidActor* PxActor = InstanceData.Body.GetPxActor();
		if (!PxActor)
		{
			continue;
		}

		physx::PxRigidDynamic* RigidDynamic = PxActor->is<physx::PxRigidDynamic>();
		if (!RigidDynamic)
		{
			continue;
		}

		++LocalTotal;

		const bool bSleepingNow = RigidDynamic->isSleeping();
		const bool bIsActive    = ActiveActorsSet.Contains(PxActor);

		if (!bIsActive && bSleepingNow)
		{
			++LocalSleeping;
			InstanceData.bWasSleeping = true;
			continue;
		}

		UInstancedStaticMeshComponent* ISMC = InstanceData.InstancedComponent.Get();
		if (!ISMC)
		{
			continue;
		}

		const APhysXInstancedMeshActor* OwnerActor = Cast<APhysXInstancedMeshActor>(ISMC->GetOwner());
		if (!OwnerActor)
		{
			continue;
		}

		const FPhysXInstanceStopConfig StopConfig = OwnerActor->AutoStopConfig;
		const FPhysXInstanceCCDConfig  CCDConfig  = OwnerActor->CCDConfig;

		const bool bUseCustomKillZ = OwnerActor->bUseCustomKillZ;
		const float CustomKillZ    = OwnerActor->CustomKillZ;

		const EPhysXInstanceStopAction LostInstanceAction = OwnerActor->LostInstanceAction;

		const bool bHasOwnerLocation = true;
		const FVector OwnerLocation  = OwnerActor->GetActorLocation();

		FPhysXInstanceAsyncStepJob Job;
		Job.ID           = ID;
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

		Job.bWasSleepingInitial = InstanceData.bWasSleeping;

		Jobs.Add(Job);
		++NumJobsAdded;
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

	auto StepAsyncJob = [TimerDelta](FPhysXInstanceAsyncStepJob& Job)
	{
		if (!Job.Data || !Job.RigidDynamic)
		{
			return;
		}

		if (!RunAsyncPreComputeRules(TimerDelta, Job))
		{
			return;
		}

		ComputeAsyncStep_Core(TimerDelta, Job);

		RunAsyncPostComputeRules(TimerDelta, Job);
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

	PhysicsStepLocalTotal    = LocalTotal;
	PhysicsStepLocalSleeping = LocalSleeping;

	PhysicsStepApplyCtx.Reset(Jobs.Num());
	bPhysicsStepHasPendingApply = true;
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

	APhysXInstancedMeshActor* Owner = Cast<APhysXInstancedMeshActor>(ISMC->GetOwner());
	const TWeakObjectPtr<APhysXInstancedMeshActor> OwnerWeak(Owner);

	// ---------------------------------------------------------------------
	// PRE/POST PHYSICS EVENTS
	// Pre: fire right before we start touching PhysX/flags
	// Post: always fire on exit with bSuccess (final return value)
	// ---------------------------------------------------------------------

	const bool bFirePre =
		IsValid(Owner) &&
		IsEventEnabled(Owner, EPhysXInstanceEventFlags::PrePhysics) &&
		(Owner->OnInstancePrePhysics.IsBound() || HasInterfaceEvents(Owner));

	const bool bFirePost =
		IsValid(Owner) &&
		IsEventEnabled(Owner, EPhysXInstanceEventFlags::PostPhysics) &&
		(Owner->OnInstancePostPhysics.IsBound() || HasInterfaceEvents(Owner));

	if (bFirePre)
	{
		// Use shared helper from anonymous namespace (defined выше в файле).
		::FirePrePhysics(Owner, ID, bEnable, bDestroyBodyIfDisabling);
	}

	bool bSuccess = false;
	ON_SCOPE_EXIT
	{
		if (!bFirePost)
		{
			return;
		}

		if (OwnerWeak.IsValid())
		{
			::FirePostPhysics(OwnerWeak.Get(), ID, bEnable, bDestroyBodyIfDisabling, bSuccess);
		}
	};

	// ---------------------------------------------------------------------
	// Main logic
	// ---------------------------------------------------------------------

	physx::PxRigidActor*   Actor        = Data->Body.GetPxActor();
	physx::PxRigidDynamic* RigidDynamic = Actor ? Actor->is<physx::PxRigidDynamic>() : nullptr;

	if (bEnable)
	{
		// Read settings once (authoritative source is the owner actor).
		EPhysXInstanceShapeType ShapeType = EPhysXInstanceShapeType::Box;
		UStaticMesh* OverrideMesh         = nullptr;
		bool bUseGravity                  = true;

		if (const APhysXInstancedMeshActor* OwnerActor = Cast<APhysXInstancedMeshActor>(ISMC->GetOwner()))
		{
			ShapeType    = OwnerActor->InstanceShapeType;
			OverrideMesh = OwnerActor->OverrideCollisionMesh;
			bUseGravity  = OwnerActor->bInstancesUseGravity;
		}

		// Fallback to component mesh if no override mesh.
		if (!OverrideMesh)
		{
			OverrideMesh = ISMC->GetStaticMesh();
		}

		// If there is no body yet, try to create one now.
		if (!RigidDynamic)
		{
			if (!GInstancedDefaultMaterial)
			{
				// bSuccess stays false; PostPhysics will get false.
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
				// bSuccess stays false; PostPhysics will get false.
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

			RigidDynamic->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, !bUseGravity);
			RigidDynamic->setActorFlag(PxActorFlag::eDISABLE_SIMULATION, false);

			// Apply mass/damping overrides every time we (re)enable.
			if (const APhysXInstancedMeshActor* OwnerActor = Cast<APhysXInstancedMeshActor>(ISMC->GetOwner()))
			{
				ApplyOwnerPhysicsOverrides(OwnerActor, ISMC, OverrideMesh, RigidDynamic);
			}

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
				// IMPORTANT: if this ID is queued for deferred AddActorToScene, kill those entries.
				InvalidatePendingAddEntries(ID);

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

	// Success must reflect the actual outcome.
	bSuccess = bEnable ? Data->bSimulating : true;
	return bSuccess;
#endif // PHYSICS_INTERFACE_PHYSX
}



bool UPhysXInstancedWorldSubsystem::ConvertInstanceToStaticStorage(
	FPhysXInstanceID ID,
	bool bCreateStorageActorIfNeeded)
{
	// Public call is always explicit.
	return ConvertInstanceToStaticStorage_Internal(
		ID,
		bCreateStorageActorIfNeeded,
		EPhysXInstanceConvertReason::Explicit);
}


bool UPhysXInstancedWorldSubsystem::ConvertStorageInstanceToDynamic(
	FPhysXInstanceID ID,
	bool bCreateDynamicActorIfNeeded)
{
	// Public call is always explicit.
	return ConvertStorageInstanceToDynamic_Internal(
		ID,
		bCreateDynamicActorIfNeeded,
		EPhysXInstanceConvertReason::Explicit);
}


bool UPhysXInstancedWorldSubsystem::ConvertInstanceToStaticStorage_Internal(
	FPhysXInstanceID ID,
	bool bCreateStorageActorIfNeeded,
	EPhysXInstanceConvertReason Reason)
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

	APhysXInstancedMeshActor* SourceActor = Cast<APhysXInstancedMeshActor>(ISMC->GetOwner());
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
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

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

			StorageISMC->SetInstancesAffectNavigation(StorageActor->bStorageInstancesAffectNavigation);
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
	// PRE/POST CONVERT EVENTS
	// Signature assumed: (ID, Reason, FromActor, ToActor, WorldTM)
	// ---------------------------------------------------------------------

	const bool bFirePre_Source =
		IsEventEnabled(SourceActor, EPhysXInstanceEventFlags::PreConvert) &&
		(SourceActor->OnInstancePreConvert.IsBound() || HasInterfaceEvents(SourceActor));

	const bool bFirePost_Source =
		IsEventEnabled(SourceActor, EPhysXInstanceEventFlags::PostConvert) &&
		(SourceActor->OnInstancePostConvert.IsBound() || HasInterfaceEvents(SourceActor));

	const bool bFirePre_Storage =
		IsEventEnabled(StorageActor, EPhysXInstanceEventFlags::PreConvert) &&
		(StorageActor->OnInstancePreConvert.IsBound() || HasInterfaceEvents(StorageActor));

	const bool bFirePost_Storage =
		IsEventEnabled(StorageActor, EPhysXInstanceEventFlags::PostConvert) &&
		(StorageActor->OnInstancePostConvert.IsBound() || HasInterfaceEvents(StorageActor));

	auto FirePreConvert = [&](APhysXInstancedMeshActor* Actor)
	{
		if (!Actor)
		{
			return;
		}

		Actor->OnInstancePreConvert.Broadcast(ID, Reason, SourceActor, StorageActor, WorldTM);

		if (HasInterfaceEvents(Actor))
		{
			IPhysXInstanceEvents::Execute_OnInstancePreConvert(Actor, ID, Reason, SourceActor, StorageActor, WorldTM);
		}
	};

	auto FirePostConvert = [&](APhysXInstancedMeshActor* Actor)
	{
		if (!Actor)
		{
			return;
		}

		Actor->OnInstancePostConvert.Broadcast(ID, Reason, SourceActor, StorageActor, WorldTM);

		if (HasInterfaceEvents(Actor))
		{
			IPhysXInstanceEvents::Execute_OnInstancePostConvert(Actor, ID, Reason, SourceActor, StorageActor, WorldTM);
		}
	};

	// Fire PreConvert right before we start mutating data.
	if (bFirePre_Source)
	{
		FirePreConvert(SourceActor);
	}
	if (bFirePre_Storage && StorageActor != SourceActor)
	{
		FirePreConvert(StorageActor);
	}

	// Always fire PostConvert even on early returns after this point.
	const bool bNeedPost = (bFirePost_Source || bFirePost_Storage);
	ON_SCOPE_EXIT
	{
		if (!bNeedPost)
		{
			return;
		}

		if (bFirePost_Source)
		{
			FirePostConvert(SourceActor);
		}
		if (bFirePost_Storage && StorageActor != SourceActor)
		{
			FirePostConvert(StorageActor);
		}
	};

	// ---------------------------------------------------------------------
	// 2) Add a new ISM instance into the storage actor
	// ---------------------------------------------------------------------

	const int32 StorageIndex = StorageISMC->AddInstanceWorldSpace(WorldTM);
	if (StorageIndex == INDEX_NONE)
	{
		return false;
	}

	// ---------------------------------------------------------------------
	// 3) Remove the source visual instance and destroy its PhysX body
	//    (ID stays registered; we just rebind it to the storage component/index).
	// ---------------------------------------------------------------------

	const int32 RemovedIndex = Data->InstanceIndex;

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


bool UPhysXInstancedWorldSubsystem::ConvertStorageInstanceToDynamic_Internal(
	FPhysXInstanceID ID,
	bool bCreateDynamicActorIfNeeded,
	EPhysXInstanceConvertReason Reason)
{
	FPhysXInstanceData* Data = Instances.Find(ID);
	if (!Data)
	{
		return false;
	}

	UInstancedStaticMeshComponent* StorageISMC_Base = Data->InstancedComponent.Get();
	if (!StorageISMC_Base || !StorageISMC_Base->IsValidLowLevelFast())
	{
		return false;
	}

	const int32 StorageIndex = Data->InstanceIndex;
	if (StorageIndex == INDEX_NONE)
	{
		return false;
	}

	APhysXInstancedMeshActor* StorageActor = Cast<APhysXInstancedMeshActor>(StorageISMC_Base->GetOwner());
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
	if (!StorageISMC_Base->GetInstanceTransform(StorageIndex, WorldTM, /*bWorldSpace=*/true))
	{
		return false;
	}

	UStaticMesh* StaticMesh = StorageActor->InstanceStaticMesh;
	if (!StaticMesh)
	{
		StaticMesh = StorageISMC_Base->GetStaticMesh();
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

		// IMPORTANT: dynamic ISM collision should be disabled to avoid double collision.
		TargetActor->bDisableISMPhysics = true;

		// Copy mesh/material settings from the storage actor.
		TargetActor->InstanceStaticMesh         = StaticMesh;
		TargetActor->bOverrideInstanceMaterials = StorageActor->bOverrideInstanceMaterials;
		TargetActor->InstanceOverrideMaterials  = StorageActor->InstanceOverrideMaterials;
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

	UPhysXInstancedStaticMeshComponent* TargetISMC = TargetActor->InstancedMesh;
	if (!TargetISMC || !TargetISMC->IsValidLowLevelFast())
	{
		return false;
	}

	// Enforce dynamic container invariants (avoid double collision).
	TargetActor->bStorageOnly       = false;
	TargetActor->bIsStorageActor    = false;
	TargetActor->bSimulateInstances = true;
	TargetActor->bDisableISMPhysics = true;

	if (TargetISMC->GetStaticMesh() != StaticMesh)
	{
		TargetISMC->SetStaticMesh(StaticMesh);
	}
	TargetActor->ApplyInstanceMaterials();

	TargetISMC->SetSimulatePhysics(false);
	TargetISMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	// ---------------------------------------------------------------------
	// PRE/POST CONVERT EVENTS
	// Signature assumed: (ID, Reason, FromActor, ToActor, WorldTM)
	// ---------------------------------------------------------------------

	const bool bFirePre_From =
		IsEventEnabled(StorageActor, EPhysXInstanceEventFlags::PreConvert) &&
		(StorageActor->OnInstancePreConvert.IsBound() || HasInterfaceEvents(StorageActor));

	const bool bFirePost_From =
		IsEventEnabled(StorageActor, EPhysXInstanceEventFlags::PostConvert) &&
		(StorageActor->OnInstancePostConvert.IsBound() || HasInterfaceEvents(StorageActor));

	const bool bFirePre_To =
		IsEventEnabled(TargetActor, EPhysXInstanceEventFlags::PreConvert) &&
		(TargetActor->OnInstancePreConvert.IsBound() || HasInterfaceEvents(TargetActor));

	const bool bFirePost_To =
		IsEventEnabled(TargetActor, EPhysXInstanceEventFlags::PostConvert) &&
		(TargetActor->OnInstancePostConvert.IsBound() || HasInterfaceEvents(TargetActor));

	auto FirePreConvert = [&](APhysXInstancedMeshActor* Actor)
	{
		if (!Actor)
		{
			return;
		}

		Actor->OnInstancePreConvert.Broadcast(ID, Reason, StorageActor, TargetActor, WorldTM);

		if (HasInterfaceEvents(Actor))
		{
			IPhysXInstanceEvents::Execute_OnInstancePreConvert(Actor, ID, Reason, StorageActor, TargetActor, WorldTM);
		}
	};

	auto FirePostConvert = [&](APhysXInstancedMeshActor* Actor)
	{
		if (!Actor)
		{
			return;
		}

		Actor->OnInstancePostConvert.Broadcast(ID, Reason, StorageActor, TargetActor, WorldTM);

		if (HasInterfaceEvents(Actor))
		{
			IPhysXInstanceEvents::Execute_OnInstancePostConvert(Actor, ID, Reason, StorageActor, TargetActor, WorldTM);
		}
	};

	// Fire PreConvert right before we start mutating data/instances.
	if (bFirePre_From)
	{
		FirePreConvert(StorageActor);
	}
	if (bFirePre_To && TargetActor != StorageActor)
	{
		FirePreConvert(TargetActor);
	}

	bool bConvertedSuccessfully = false;
	ON_SCOPE_EXIT
	{
		if (!bConvertedSuccessfully)
		{
			return;
		}

		if (bFirePost_From)
		{
			FirePostConvert(StorageActor);
		}
		if (bFirePost_To && TargetActor != StorageActor)
		{
			FirePostConvert(TargetActor);
		}
	};

	// ---------------------------------------------------------------------
	// 2) Add a visual instance to the target actor (dynamic container).
	// ---------------------------------------------------------------------

	const int32 TargetIndex = TargetISMC->AddInstanceWorldSpace(WorldTM);
	if (TargetIndex == INDEX_NONE)
	{
		return false;
	}

#if PHYSICS_INTERFACE_PHYSX
	// ---------------------------------------------------------------------
	// 3) Create a PhysX body for the NEW target slot first (so we can rollback safely).
	// ---------------------------------------------------------------------

	if (!GInstancedDefaultMaterial)
	{
		TargetISMC->RemoveInstance(TargetIndex);
		return false;
	}

	EPhysXInstanceShapeType ShapeType = EPhysXInstanceShapeType::Box;
	UStaticMesh* OverrideMesh = nullptr;

	ShapeType    = TargetActor->InstanceShapeType;
	OverrideMesh = TargetActor->OverrideCollisionMesh;

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

#if PHYSICS_INTERFACE_PHYSX
	// If ID was still queued from a previous dynamic state, kill those entries before rebinding.
	InvalidatePendingAddEntries(ID);
#endif

	// Remove the old slot mapping BEFORE changing Data.
	RemoveSlotMapping(ID);

	// Remove from storage (this compacts indices).
	if (!StorageISMC_Base->RemoveInstance(StorageIndex))
	{
		// Rollback: remove the new target instance.
		TargetISMC->RemoveInstance(TargetIndex);

		// Restore old mapping (Data still points to old storage slot).
		AddSlotMapping(ID);

#if PHYSICS_INTERFACE_PHYSX
		// NewBody owns a Px pointer; destroy explicitly on rollback.
		NewBody.Destroy();
#endif
		return false;
	}

	// Actor bookkeeping.
	StorageActor->RegisteredInstanceIDs.Remove(ID);
	TargetActor->RegisteredInstanceIDs.Add(ID);

	// Fix indices for other storage-bound IDs affected by compaction.
	FixInstanceIndicesAfterRemoval(StorageISMC_Base, StorageIndex);

	// Rebind stable ID to the new dynamic slot.
	Data->InstancedComponent = TargetISMC;
	Data->InstanceIndex      = TargetIndex;

#if PHYSICS_INTERFACE_PHYSX
	Data->bSimulating  = true;
	Data->SleepTime    = 0.0f;
	Data->FallTime     = 0.0f;
	Data->bWasSleeping = false;

	// Replace body (storage instances should have no body).
	ClearInstanceUserData(ID);
	Data->Body.Destroy();
	Data->Body = NewBody;

	EnsureInstanceUserData(ID);
	++NumBodiesLifetimeCreated;
#else
	Data->bSimulating  = false;
	Data->SleepTime    = 0.0f;
	Data->FallTime     = 0.0f;
	Data->bWasSleeping = false;
#endif

	// Add new slot mapping AFTER Data points to the target slot.
	AddSlotMapping(ID);

	RebuildSlotMappingForComponent(StorageISMC_Base);
	RebuildSlotMappingForComponent(TargetISMC);

#if PHYSICS_INTERFACE_PHYSX
	// Queue for scene insertion AFTER rebinding.
	EnqueueAddActorToScene(ID, TargetISMC);
#endif

	StorageISMC_Base->MarkRenderStateDirty();
	TargetISMC->MarkRenderStateDirty();

	// Optional: auto-destroy empty storage actors.
	if (StorageActor != TargetActor &&
		StorageActor->RegisteredInstanceIDs.Num() == 0 &&
		StorageActor->InstancedMesh &&
		StorageActor->InstancedMesh->GetInstanceCount() == 0)
	{
		if (StorageActor->PhysXActorID.IsValid())
		{
			UnregisterInstancedMeshActor(StorageActor->PhysXActorID);
		}
		StorageActor->Destroy();
	}

	bConvertedSuccessfully = true;
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
	return RemoveInstanceByID_Internal(ID, bRemoveVisualInstance, EPhysXInstanceRemoveReason::Explicit);
}

bool UPhysXInstancedWorldSubsystem::RemoveInstanceByID_Internal(
	FPhysXInstanceID ID,
	bool bRemoveVisualInstance,
	EPhysXInstanceRemoveReason Reason)
{
	FPhysXInstanceData* Data = Instances.Find(ID);
	if (!Data)
	{
		return false;
	}

	UInstancedStaticMeshComponent* ISMC = Data->InstancedComponent.Get();
	int32 InstanceIndex = Data->InstanceIndex;
	const bool bWasSimulating = Data->bSimulating;

	APhysXInstancedMeshActor* OwnerActor =
		ISMC ? Cast<APhysXInstancedMeshActor>(ISMC->GetOwner()) : nullptr;

	const bool bFirePre =
		IsEventEnabled(OwnerActor, EPhysXInstanceEventFlags::PreRemove) &&
		(OwnerActor->OnInstancePreRemove.IsBound() || HasInterfaceEvents(OwnerActor));

	const bool bFirePost =
		IsEventEnabled(OwnerActor, EPhysXInstanceEventFlags::PostRemove) &&
		(OwnerActor->OnInstancePostRemove.IsBound() || HasInterfaceEvents(OwnerActor));

	FTransform SnapshotTM = FTransform::Identity;
	if (bFirePre || bFirePost)
	{
		GetInstanceWorldTransform_Safe(*Data, SnapshotTM);
	}

	if (bFirePre)
	{
		OwnerActor->OnInstancePreRemove.Broadcast(ID, Reason, SnapshotTM);

		if (HasInterfaceEvents(OwnerActor))
		{
			IPhysXInstanceEvents::Execute_OnInstancePreRemove(OwnerActor, ID, Reason, SnapshotTM);
		}
	}

	bool bOwnerIsStorageActor = false;
	if (OwnerActor)
	{
		bOwnerIsStorageActor = (OwnerActor->bIsStorageActor || OwnerActor->bStorageOnly);
		OwnerActor->RegisteredInstanceIDs.Remove(ID);
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

	auto FirePost = [&](bool bSuccess)
	{
		if (!bFirePost)
		{
			return;
		}

		OwnerActor->OnInstancePostRemove.Broadcast(ID, Reason, SnapshotTM);

		if (HasInterfaceEvents(OwnerActor))
		{
			IPhysXInstanceEvents::Execute_OnInstancePostRemove(OwnerActor, ID, Reason, SnapshotTM);
		}
	};

	// If we do not need to remove the visual instance, we can drop the record now.
	if (!bRemoveVisualInstance)
	{
		Instances.Remove(ID);
		FirePost(/*bSuccess=*/true);
		return true;
	}

	if (!ISMC || !ISMC->IsValidLowLevelFast() || InstanceIndex == INDEX_NONE)
	{
		Instances.Remove(ID);
		FirePost(/*bSuccess=*/false);
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
			FirePost(/*bSuccess=*/false);
			return false;
		}

		InstanceIndex = ResolvedIndex;
	}

	const int32 OldLastIndex = ISMC->GetInstanceCount() - 1;

	// Drop the record BEFORE mutating indices of others.
	Instances.Remove(ID);

	const bool bRemoved = ISMC->RemoveInstance(InstanceIndex);
	if (!bRemoved)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[PhysXInstanced] RemoveInstanceByID: RemoveInstance failed for ISMC=%s, Index=%d"),
			*GetNameSafe(ISMC), InstanceIndex);

		RebuildSlotMappingForComponent(ISMC);
		FirePost(/*bSuccess=*/false);
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

	FirePost(/*bSuccess=*/true);

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
		FPendingAddActorEntry& Entry = PendingAddActors[Index];

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
			Entry.ID = FPhysXInstanceID(); // stale entry
			continue;
		}

		// IMPORTANT(PXIS_DEFERRED_ADD):
		// Body might have been destroyed/replaced after enqueue but before processing.
		if (!Data->Body.GetPxActor())
		{
			Entry.ID = FPhysXInstanceID(); // prevent repeated retries/budget waste
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

	using FTaskHandler = bool (UPhysXInstancedWorldSubsystem::*)(FPhysXInstanceTask&, physx::PxRigidDynamic*);

	static const FTaskHandler Handlers[] =
	{
		&UPhysXInstancedWorldSubsystem::HandleInstanceTask_AddImpulse,  // AddImpulse
		&UPhysXInstancedWorldSubsystem::HandleInstanceTask_AddForce,    // AddForce
		&UPhysXInstancedWorldSubsystem::HandleInstanceTask_PutToSleep,  // PutToSleep
		&UPhysXInstancedWorldSubsystem::HandleInstanceTask_WakeUp       // WakeUp
	};

	static_assert(UE_ARRAY_COUNT(Handlers) == (int32)EPhysXInstanceTaskType::Count, "Instance task handler table mismatch.");

	const int32 TypeIndex = (int32)Task.Type;
	if (TypeIndex < 0 || TypeIndex >= (int32)EPhysXInstanceTaskType::Count)
	{
		return true;
	}

	FTaskHandler Handler = Handlers[TypeIndex];
	if (!Handler)
	{
		return true;
	}

	return (this->*Handler)(Task, RD);

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

/**
* Copyright (C) 2025 | Created by NordVader Inc.
* All rights reserved!
* My Discord Server: https://discord.gg/B8prpf3vzD
*/

#pragma once

#include "CoreMinimal.h"
#include "Algo/Sort.h"
#include "Templates/UniquePtr.h"

class UWorld;
class UPhysXInstancedWorldSubsystem;

enum class EPhysXISProcessCategory : uint8
{
	SceneInsertion,
	DeferredInstanceOps,
	PhysicsStep,
	Lifetime,
	Other
};

struct FPhysXISProcessContext
{
	UPhysXInstancedWorldSubsystem* Subsystem = nullptr;
	UWorld*                        World     = nullptr;
	float                          DeltaTime = 0.0f;
	float                          SimTime   = 0.0f;
};

class IPhysXISProcess
{
public:
	virtual ~IPhysXISProcess() = default;

	virtual const TCHAR* GetName() const = 0;
	virtual int32 GetOrder() const = 0;
	virtual EPhysXISProcessCategory GetCategory() const { return EPhysXISProcessCategory::Other; }

	virtual void Initialize(FPhysXISProcessContext& Context) {}
	virtual void Deinitialize(FPhysXISProcessContext& Context) {}
	virtual void Tick(FPhysXISProcessContext& Context) {}
};

class FPhysXISProcessManager
{
public:
	void Reset()
	{
		Entries.Reset();
		NextIndex = 0;
	}

	template<typename TProcess, typename... TArgs>
	TProcess& AddProcess(TArgs&&... Args)
	{
		FEntry Entry;
		Entry.Index   = NextIndex++;
		Entry.Process = MakeUnique<TProcess>(Forward<TArgs>(Args)...);
		Entry.Order   = Entry.Process.IsValid() ? Entry.Process->GetOrder() : 0;

		TProcess& Ref = static_cast<TProcess&>(*Entry.Process);
		Entries.Add(MoveTemp(Entry));

		SortEntries();
		return Ref;
	}

	void InitializeAll(FPhysXISProcessContext& Context)
	{
		for (FEntry& Entry : Entries)
		{
			if (Entry.Process.IsValid())
			{
				Entry.Process->Initialize(Context);
			}
		}
	}

	void DeinitializeAll(FPhysXISProcessContext& Context)
	{
		for (int32 i = Entries.Num() - 1; i >= 0; --i)
		{
			if (Entries[i].Process.IsValid())
			{
				Entries[i].Process->Deinitialize(Context);
			}
		}
	}

	void TickAll(FPhysXISProcessContext& Context)
	{
		for (FEntry& Entry : Entries)
		{
			if (Entry.Process.IsValid())
			{
				Entry.Process->Tick(Context);
			}
		}
	}

private:
	struct FEntry
	{
		int32 Order = 0;
		int32 Index = 0;
		TUniquePtr<IPhysXISProcess> Process;
	};

	int32 NextIndex = 0;
	TArray<FEntry> Entries;

	void SortEntries()
	{
		Algo::Sort(Entries, [](const FEntry& A, const FEntry& B)
		{
			if (A.Order != B.Order)
			{
				return A.Order < B.Order;
			}
			return A.Index < B.Index;
		});
	}
};

/**
* Copyright (C) 2025 | Created by NordVader Inc.
* All rights reserved!
* My Discord Server: https://discord.gg/B8prpf3vzD
*/

#include "Processes/PhysXInstancedDefaultProcesses.h"
#include "Processes/PhysXInstancedProcessPipeline.h"
#include "Subsystems/PhysXInstancedWorldSubsystem.h"

namespace PhysXIS
{
	static constexpr int32 Order_AddActors          = 10;
	static constexpr int32 Order_InstanceTasks      = 20;
	static constexpr int32 Order_PhysicsStepCompute = 30;
	static constexpr int32 Order_PhysicsStepStop    = 31;
	static constexpr int32 Order_PhysicsStepSync    = 32;
	static constexpr int32 Order_PhysicsStepFinalize= 33;
	static constexpr int32 Order_Lifetime           = 40;

#if PHYSICS_INTERFACE_PHYSX

	class FAddActorsProcess final : public IPhysXISProcess
	{
	public:
		virtual const TCHAR* GetName() const override { return TEXT("PhysXIS.AddActors"); }
		virtual int32 GetOrder() const override { return Order_AddActors; }
		virtual EPhysXISProcessCategory GetCategory() const override { return EPhysXISProcessCategory::SceneInsertion; }

		virtual void Tick(FPhysXISProcessContext& Context) override
		{
			if (Context.Subsystem)
			{
				Context.Subsystem->ProcessPendingAddActors();
			}
		}
	};

	class FInstanceTasksProcess final : public IPhysXISProcess
	{
	public:
		virtual const TCHAR* GetName() const override { return TEXT("PhysXIS.InstanceTasks"); }
		virtual int32 GetOrder() const override { return Order_InstanceTasks; }
		virtual EPhysXISProcessCategory GetCategory() const override { return EPhysXISProcessCategory::DeferredInstanceOps; }

		virtual void Tick(FPhysXISProcessContext& Context) override
		{
			if (Context.Subsystem)
			{
				Context.Subsystem->ProcessInstanceTasks();
			}
		}
	};

	class FPhysicsStepComputeProcess final : public IPhysXISProcess
	{
	public:
		virtual const TCHAR* GetName() const override { return TEXT("PhysXIS.PhysicsStepCompute"); }
		virtual int32 GetOrder() const override { return Order_PhysicsStepCompute; }
		virtual EPhysXISProcessCategory GetCategory() const override { return EPhysXISProcessCategory::PhysicsStep; }

		virtual void Tick(FPhysXISProcessContext& Context) override
		{
			if (Context.Subsystem)
			{
				Context.Subsystem->PhysicsStep_Compute(Context.DeltaTime, Context.SimTime);
			}
		}
	};

	class FPhysicsStepStopActionsProcess final : public IPhysXISProcess
	{
	public:
		virtual const TCHAR* GetName() const override { return TEXT("PhysXIS.PhysicsStepStopActions"); }
		virtual int32 GetOrder() const override { return Order_PhysicsStepStop; }
		virtual EPhysXISProcessCategory GetCategory() const override { return EPhysXISProcessCategory::PhysicsStep; }

		virtual void Tick(FPhysXISProcessContext& Context) override
		{
			if (Context.Subsystem)
			{
				Context.Subsystem->PhysicsStep_ApplyStopActionsAndCCD();
			}
		}
	};

	class FPhysicsStepTransformSyncProcess final : public IPhysXISProcess
	{
	public:
		virtual const TCHAR* GetName() const override { return TEXT("PhysXIS.PhysicsStepTransformSync"); }
		virtual int32 GetOrder() const override { return Order_PhysicsStepSync; }
		virtual EPhysXISProcessCategory GetCategory() const override { return EPhysXISProcessCategory::PhysicsStep; }

		virtual void Tick(FPhysXISProcessContext& Context) override
		{
			if (Context.Subsystem)
			{
				Context.Subsystem->PhysicsStep_ApplyTransformSync();
			}
		}
	};

	class FPhysicsStepFinalizeProcess final : public IPhysXISProcess
	{
	public:
		virtual const TCHAR* GetName() const override { return TEXT("PhysXIS.PhysicsStepFinalize"); }
		virtual int32 GetOrder() const override { return Order_PhysicsStepFinalize; }
		virtual EPhysXISProcessCategory GetCategory() const override { return EPhysXISProcessCategory::PhysicsStep; }

		virtual void Tick(FPhysXISProcessContext& Context) override
		{
			if (Context.Subsystem)
			{
				Context.Subsystem->PhysicsStep_Finalize();
			}
		}
	};

#endif // PHYSICS_INTERFACE_PHYSX

	class FLifetimeProcess final : public IPhysXISProcess
	{
	public:
		virtual const TCHAR* GetName() const override { return TEXT("PhysXIS.Lifetime"); }
		virtual int32 GetOrder() const override { return Order_Lifetime; }
		virtual EPhysXISProcessCategory GetCategory() const override { return EPhysXISProcessCategory::Lifetime; }

		virtual void Tick(FPhysXISProcessContext& Context) override
		{
			if (Context.Subsystem)
			{
				Context.Subsystem->ProcessLifetimeExpirations();
			}
		}
	};

	void RegisterDefaultProcesses(FPhysXISProcessManager& Manager)
	{
#if PHYSICS_INTERFACE_PHYSX
		Manager.AddProcess<FAddActorsProcess>();
		Manager.AddProcess<FInstanceTasksProcess>();

		Manager.AddProcess<FPhysicsStepComputeProcess>();
		Manager.AddProcess<FPhysicsStepStopActionsProcess>();
		Manager.AddProcess<FPhysicsStepTransformSyncProcess>();
		Manager.AddProcess<FPhysicsStepFinalizeProcess>();
#endif
		Manager.AddProcess<FLifetimeProcess>();
	}
}
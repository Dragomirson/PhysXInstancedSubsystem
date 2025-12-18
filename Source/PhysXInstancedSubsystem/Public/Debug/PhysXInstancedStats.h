/**
* Copyright (C) 2025 | Created by NordVader Inc.
* All rights reserved!
* My Discord Server: https://discord.gg/B8prpf3vzD
*/

#pragma once

#include "CoreMinimal.h"
#include "Stats/Stats.h"

// ============================================================================
// PhysXInstanced stats
// ============================================================================
//
// Console:
//   stat PhysXInstanced
//
// This group tracks CPU time and key counters for the PhysX instanced subsystem.
// ============================================================================

/**
 * Stat group for the PhysX instanced plugin.
 *
 * Console:
 *   stat PhysXInstanced
 */
DECLARE_STATS_GROUP(TEXT("PhysXInstanced"), STATGROUP_PhysXInstanced, STATCAT_Advanced);

// --- Main cycle stats ------------------------------------------------------

/** CPU time spent inside UPhysXInstancedSubsystem::AsyncPhysicsStep. */
DECLARE_CYCLE_STAT_EXTERN(TEXT("AsyncPhysicsStep"), STAT_PhysXInstanced_AsyncPhysicsStep, STATGROUP_PhysXInstanced, );

/** CPU time spent creating PhysX rigid bodies for instances. */
DECLARE_CYCLE_STAT_EXTERN(TEXT("CreateBody"), STAT_PhysXInstanced_CreateBody, STATGROUP_PhysXInstanced, );

/** CPU time spent registering and bookkeeping instances in the subsystem. */
DECLARE_CYCLE_STAT_EXTERN(TEXT("RegisterInstance"), STAT_PhysXInstanced_RegisterInstance, STATGROUP_PhysXInstanced, );

/** CPU time spent drawing debug shapes for PhysX instances. */
DECLARE_CYCLE_STAT_EXTERN(TEXT("DebugDraw"), STAT_PhysXInstanced_DebugDraw, STATGROUP_PhysXInstanced, );

// --- Async step breakdown --------------------------------------------------

/** Async step: build the job list on the game thread. */
DECLARE_CYCLE_STAT_EXTERN(TEXT("Async Step - BuildJobs"), STAT_PhysXInstanced_AsyncBuildJobs, STATGROUP_PhysXInstanced, );

/** Async step: parallel work performed in the ParallelFor body. */
DECLARE_CYCLE_STAT_EXTERN(TEXT("Async Step - Parallel"), STAT_PhysXInstanced_AsyncParallel, STATGROUP_PhysXInstanced, );

/** Async step: apply results back on the game thread. */
DECLARE_CYCLE_STAT_EXTERN(TEXT("Async Step - ApplyResults"), STAT_PhysXInstanced_AsyncApply, STATGROUP_PhysXInstanced, );

// --- Registration breakdown ------------------------------------------------

/** Batched registration: ParallelFor over CreateBody jobs. */
DECLARE_CYCLE_STAT_EXTERN(TEXT("Register - ParallelFor"), STAT_PhysXInstanced_RegisterParallel, STATGROUP_PhysXInstanced, );

// --- Counters --------------------------------------------------------------

/** Async step: number of jobs (simulated bodies) processed this frame. */
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Async Jobs Per Frame"), STAT_PhysXInstanced_JobsPerFrame, STATGROUP_PhysXInstanced, );

DECLARE_DWORD_COUNTER_STAT(TEXT("PhysX Bodies Lifetime Created"), STAT_PhysXInstanced_BodiesLifetimeCreated, STATGROUP_PhysXInstanced);

/** Total number of PhysX bodies tracked by the instanced subsystem. */
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Bodies Total"), STAT_PhysXInstanced_BodiesTotal, STATGROUP_PhysXInstanced, );

/** Number of bodies that are currently simulating. */
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Bodies Simulating"), STAT_PhysXInstanced_BodiesSimulating, STATGROUP_PhysXInstanced, );

/** Number of bodies that are currently sleeping. */
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Bodies Sleeping"), STAT_PhysXInstanced_BodiesSleeping, STATGROUP_PhysXInstanced, );

/** Total number of instances registered in the subsystem. */
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Instances Registered Total"), STAT_PhysXInstanced_InstancesTotal, STATGROUP_PhysXInstanced, );

/** Number of active PhysX actors reported by the PhysX scene. */
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Active Actors (PhysX Scene)"), STAT_PhysXInstanced_ActiveActorsFromScene, STATGROUP_PhysXInstanced, );

// --- Internal worker timings -----------------------------------------------

/** Async step: worker task cost for processing an individual job batch. */
DECLARE_CYCLE_STAT_EXTERN(TEXT("Async Step - Job Worker"), STAT_PhysXInstanced_AsyncJobWorker, STATGROUP_PhysXInstanced, NO_API);

/** RegisterInstancesBatch: cost of preparing job data. */
DECLARE_CYCLE_STAT_EXTERN(TEXT("RegisterInstancesBatch - Prepare Jobs"), STAT_PhysXInstanced_RegisterPrepareJobs, STATGROUP_PhysXInstanced, NO_API);

/** RegisterInstancesBatch: worker cost of CreateBody processing. */
DECLARE_CYCLE_STAT_EXTERN(TEXT("RegisterInstancesBatch - CreateBody Worker"), STAT_PhysXInstanced_RegisterCreateBodyWorker, STATGROUP_PhysXInstanced, NO_API);

/** RegisterInstancesBatch: cost of finalization and bookkeeping. */
DECLARE_CYCLE_STAT_EXTERN(TEXT("RegisterInstancesBatch - Finalize"), STAT_PhysXInstanced_RegisterFinalize, STATGROUP_PhysXInstanced, NO_API);

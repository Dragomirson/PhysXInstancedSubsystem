/**
* Copyright (C) 2025 | Created by NordVader Inc.
* All rights reserved!
* My Discord Server: https://discord.gg/B8prpf3vzD
*/

// PhysXInstancedStats.cpp

#include "Debug/PhysXInstancedStats.h"

// ============================================================================
// PhysXInstanced stat definitions
// ============================================================================
//
// This file defines all stats declared in PhysXInstancedStats.h.
//
// Console:
//   stat PhysXInstanced
//
// The stat group exposes CPU cycle timings and runtime counters for the
// PhysX instanced subsystem.
// ============================================================================

// --- Main cycle stats -------------------------------------------------------

DEFINE_STAT(STAT_PhysXInstanced_AsyncPhysicsStep);
DEFINE_STAT(STAT_PhysXInstanced_CreateBody);
DEFINE_STAT(STAT_PhysXInstanced_RegisterInstance);
DEFINE_STAT(STAT_PhysXInstanced_RegisterParallel);
DEFINE_STAT(STAT_PhysXInstanced_DebugDraw);

// --- Core counters ----------------------------------------------------------

DEFINE_STAT(STAT_PhysXInstanced_BodiesTotal);
DEFINE_STAT(STAT_PhysXInstanced_BodiesSimulating);
DEFINE_STAT(STAT_PhysXInstanced_BodiesSleeping);

// --- Async step breakdown ---------------------------------------------------

DEFINE_STAT(STAT_PhysXInstanced_AsyncBuildJobs);
DEFINE_STAT(STAT_PhysXInstanced_AsyncParallel);
DEFINE_STAT(STAT_PhysXInstanced_AsyncApply);
DEFINE_STAT(STAT_PhysXInstanced_JobsPerFrame);

// --- World-level counters ---------------------------------------------------

DEFINE_STAT(STAT_PhysXInstanced_InstancesTotal);
DEFINE_STAT(STAT_PhysXInstanced_ActiveActorsFromScene);

// --- Internal worker timings ------------------------------------------------

DEFINE_STAT(STAT_PhysXInstanced_AsyncJobWorker);
DEFINE_STAT(STAT_PhysXInstanced_RegisterPrepareJobs);
DEFINE_STAT(STAT_PhysXInstanced_RegisterCreateBodyWorker);
DEFINE_STAT(STAT_PhysXInstanced_RegisterFinalize);

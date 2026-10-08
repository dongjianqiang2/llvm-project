//===-- EJitSmallTableSreBridge.h - opt-in SRE small-table controls -*- C -*-===//
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITSMALLTABLESREBRIDGE_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITSMALLTABLESREBRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EJIT_STAB_SRE_ABI_VERSION 1u
#define EJIT_STAB_SRE_MAX_DIMS 4u
#define EJIT_STAB_SRE_MAX_MEMBERS 32u
#define EJIT_STAB_SRE_MAX_COUNTERS 32u
#define EJIT_STAB_SRE_MAX_COUNTER_WORDS 256u
#define EJIT_STAB_SRE_NAME_BYTES 96u
#define EJIT_STAB_SRE_OK 0
#define EJIT_STAB_SRE_BLOCKED (-1)
#define EJIT_STAB_SRE_INVALID (-2)
#define EJIT_STAB_SRE_BUSY (-3)
#define EJIT_STAB_SRE_FAILED (-4)
#define EJIT_STAB_SRE_DATA_READ 1u
#define EJIT_STAB_SRE_DATA_WRITE 2u
#define EJIT_STAB_SRE_ENABLE_FIXED_DOMAIN 1u

/// LOCAL bindings: callbacks/context are never copied into shared commands.
/// current_task_id must return the real platform task token (nonzero), NOT a
/// core ID, simulated TLS ID, or taskpool diagnostic workerTaskId.
/// prepare_shared_data proves/installs same-VA coherent readable (or writable)
/// mapping on THIS calling core. A no-op or merely same VA is not this proof.
typedef struct {
  uint32_t abiVersion;
  uint32_t structSize;
  uint32_t flags;
  uint32_t waitRounds;
  uint64_t (*current_task_id)(void *context);
  void (*delay_ticks)(void *context, uint32_t ticks);
  int (*prepare_shared_data)(void *context, uintptr_t address, uint64_t bytes,
                             uint32_t access);
  void *context;
} ejit_small_table_sre_bindings_t;

/// Product/demo configuration commit state in genuinely coherent shared RAM.
/// Writers CAS writerBlocked from 0 to 1, wait for readers==0, mutate the source
/// and publish epoch/revision with release, then release writerBlocked to 0.
/// Borrow acquisition increments readers then rechecks writerBlocked and both
/// identities. Holding a borrow guarantees source lifetime AND stable reads.
/// Never mutate/free the source while readers!=0; cancel is not a real leave.
typedef struct {
  uint64_t epoch;
  uint64_t revision;
  uint32_t writerBlocked;
  uint32_t readers;
} ejit_small_table_sre_source_state_t;

typedef struct {
  uint32_t argumentIndex;
  uint32_t reserved;
  uint64_t modulus;
  uint64_t extent;
  char periodName[EJIT_STAB_SRE_NAME_BYTES];
} ejit_small_table_sre_dim_t;

typedef struct {
  uint64_t coordinate[EJIT_STAB_SRE_MAX_DIMS];
  uint64_t configurationGeneration;
  uint32_t fieldsInitialized;
  uint32_t reserved;
} ejit_small_table_sre_member_t;

/// All command fields are copied by value. Addresses name only real code or
/// coherent POD/source storage, never a C++ Host, callback, closure or container.
/// readyMembers/domainCoverage are explicit completed configuration facts;
/// activation, worker-ready, elapsed time and a matching fingerprint are not.
typedef struct {
  uint32_t abiVersion;
  uint32_t structSize;
  char entryName[EJIT_STAB_SRE_NAME_BYTES];
  char sourceVarName[EJIT_STAB_SRE_NAME_BYTES];
  uintptr_t sourceAddress;
  uint64_t sourceBytes;
  uintptr_t sourceState;
  uintptr_t aotEntry;
  uint64_t sourceEpoch;
  uint64_t configurationRevision;
  uint64_t codeGeneration;
  uint64_t sampleLimit;
  uint32_t numDims;
  uint32_t numMembers;
  uint32_t domainCoverage;
  uint32_t reserved;
  ejit_small_table_sre_dim_t dims[EJIT_STAB_SRE_MAX_DIMS];
  ejit_small_table_sre_member_t members[EJIT_STAB_SRE_MAX_MEMBERS];
} ejit_small_table_sre_request_t;

typedef struct {
  char name[EJIT_STAB_SRE_NAME_BYTES];
  uint64_t hash;
  uint32_t firstWord;
  uint32_t wordCount;
} ejit_small_table_sre_counter_t;

/// A bounded EXACT inventory, never a silently truncated profile. If any real
/// pair/name/counts exceed these bounds, snapshot/finish fail closed. fullProfile
/// Valid is set only after parsing the actual frozen profile and checking every
/// expected name/hash/count word; sampleCount alone does not imply that flag.
typedef struct {
  uint32_t abiVersion;
  uint32_t structSize;
  int32_t status;
  uint32_t funcIndex;
  uint64_t ownerIdentity;
  uint64_t workerTaskIdentity;
  uint64_t policyEpoch;
  uint64_t codeGeneration;
  uint64_t resourceGeneration;
  uint64_t sourceEpoch;
  uint64_t configurationRevision;
  uint64_t sampleLimit;
  uint64_t sampleCount;
  uint64_t inFlight;
  uint64_t physicalExecutions;
  uint64_t retainedExecutions;
  uint64_t borrowReaders;
  uint64_t admittedMembers;
  uint64_t publishedSlots;
  uint64_t ownerWorkerOperations;
  uint64_t profileBytes;
  uint64_t rootEntryCount;
  uint64_t countersDigest;
  uint64_t genericAsyncEnqueues;
  uint64_t genericAsyncCompiles;
  uint64_t genericPending;
  uint32_t tier; // 0=no entry, 1=common T1, 2=common T2.
  uint32_t expectedCounterPairs;
  uint32_t counterPairs;
  uint32_t counterWordCount;
  uint32_t fullProfileValid;
  uint32_t rootEntryCountValid; // Actual PGOUse metadata, only after full T2.
  char reason[192];
  ejit_small_table_sre_counter_t counters[EJIT_STAB_SRE_MAX_COUNTERS];
  uint64_t counts[EJIT_STAB_SRE_MAX_COUNTER_WORDS];
} ejit_small_table_sre_snapshot_t;

/// Call prepare once on each private runtime/core, BEFORE ejit_init_pgo. Fixed
/// domain opt-in is requested on the worker core only. Existing ABI/defaults are
/// unchanged; no call automatically enables small-table policy or changes PGO.
int ejit_small_table_sre_prepare(const ejit_small_table_sre_bindings_t *bindings);
int ejit_small_table_sre_request(const ejit_small_table_sre_request_t *request);
int ejit_small_table_sre_get_snapshot(uint32_t funcIndex,
                                    ejit_small_table_sre_snapshot_t *snapshot);
/// Explicit finish only: quota+1 can be tested on AOT BEFORE freeze/T2. Refuses
/// while any true entered call is in flight; never fabricates a completion.
int ejit_small_table_sre_finish(uint32_t funcIndex);
int ejit_small_table_sre_cancel(uint32_t funcIndex);
int ejit_small_table_sre_print(uint32_t funcIndex);

#ifdef __cplusplus
} // extern "C"
#endif
#endif

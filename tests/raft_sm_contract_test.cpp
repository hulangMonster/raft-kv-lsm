
// M6.9.1: KvStateMachine (mem) instantiation of the shared contract body.
#include <memory>

#include "gtest/gtest.h"
#include "kv/kv_state_machine.h"
#include "sm_contract_body.h"

using namespace raftkv;
using namespace raftkv::smtest;

namespace {
Factory<KvStateMachine> Mem() {
  return []() { return std::make_unique<KvStateMachine>(); };
}
}  // namespace

TEST(KvStateMachineContract, Empty) { Contract_Empty<KvStateMachine>(Mem()); }
TEST(KvStateMachineContract, BasicOps) { Contract_BasicOps<KvStateMachine>(Mem()); }
TEST(KvStateMachineContract, IdempotentSameIndex) {
  Contract_IdempotentSameIndex<KvStateMachine>(Mem());
}
TEST(KvStateMachineContract, IdempotentHigherIndexAdvancesApplied) {
  Contract_IdempotentHigherIndexAdvancesApplied<KvStateMachine>(Mem());
}
TEST(KvStateMachineContract, MarkersAdvanceApplied) {
  Contract_MarkersAdvanceApplied<KvStateMachine>(Mem());
}
TEST(KvStateMachineContract, CanonicalState) {
  Contract_CanonicalState<KvStateMachine>(Mem());
}
TEST(KvStateMachineContract, SnapshotDeterministic) {
  Contract_SnapshotDeterministic<KvStateMachine>(Mem());
}
TEST(KvStateMachineContract, StreamMatchesSerialize) {
  Contract_StreamMatchesSerialize<KvStateMachine>(Mem());
}
TEST(KvStateMachineContract, SnapshotRoundTrip) {
  Contract_SnapshotRoundTrip<KvStateMachine>(Mem());
}
TEST(KvStateMachineContract, RestoreRejectsMalformed) {
  Contract_RestoreRejectsMalformed<KvStateMachine>(Mem());
}

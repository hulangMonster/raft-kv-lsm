
#pragma once
// M6.9.1: shared StateMachine contract body. Instantiated for KvStateMachine (mem)
// and LsmKvStateMachine (lsm) with the SAME assertions (design 10.9).
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "raft/state_machine.h"
#include "raft/types.h"

namespace raftkv::smtest {

using raft::Index;
using raft::LogEntry;
using raft::kNoIndex;

template <typename SM>
using Factory = std::function<std::unique_ptr<SM>()>;

inline LogEntry mk(Index idx, OpCode op, const std::string& key,
                   const std::string& value, uint64_t cid, uint64_t rid) {
  LogEntry e;
  e.index = idx;
  e.term = 1;
  e.op = op;
  e.key = key;
  e.value = value;
  e.clientId = cid;
  e.requestId = rid;
  return e;
}

inline LogEntry put(Index idx, const std::string& k, const std::string& v,
                    uint64_t cid = 1, uint64_t rid = 0) {
  return mk(idx, OpCode::kPut, k, v, cid, rid != 0 ? rid : idx);
}
inline LogEntry del(Index idx, const std::string& k, uint64_t cid = 1,
                    uint64_t rid = 0) {
  return mk(idx, OpCode::kDel, k, std::string(), cid, rid != 0 ? rid : idx);
}
inline LogEntry noop(Index idx, OpCode op) {
  return mk(idx, op, std::string(), std::string(), 0, 0);
}

// Canonical op sequence shared by every backend assertion.
inline std::vector<LogEntry> canonicalOps() {
  return {
      put(1, "a", "1", 10, 1),
      put(2, "b", "2", 10, 2),
      put(3, "a", "1b", 10, 3),
      del(4, "b", 10, 4),
      noop(5, OpCode::kGet),
      put(6, "c", "3", 10, 5),
      noop(7, OpCode::kConfig),
  };
}

inline Bytes be64(uint64_t v) {
  Bytes o;
  for (int i = 7; i >= 0; --i) o.push_back(static_cast<Byte>((v >> (i * 8)) & 0xff));
  return o;
}
inline Bytes be32(uint32_t v) {
  Bytes o;
  for (int i = 3; i >= 0; --i) o.push_back(static_cast<Byte>((v >> (i * 8)) & 0xff));
  return o;
}
inline Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes o;
  for (const Bytes& p : parts) o.insert(o.end(), p.begin(), p.end());
  return o;
}

template <typename SM>
void checkCanonicalState(SM& sm) {
  std::string out;
  ASSERT_TRUE(sm.get("a", out));
  EXPECT_EQ(out, "1b");
  EXPECT_FALSE(sm.get("b", out));
  ASSERT_TRUE(sm.get("c", out));
  EXPECT_EQ(out, "3");
  EXPECT_EQ(sm.lastApplied(), static_cast<Index>(7));
}

template <typename SM>
void assertSnapshotEquals(SM& sm, const Bytes& expected) {
  auto view = sm.snapshotView();
  ASSERT_NE(view, nullptr);
  EXPECT_EQ(view->serialize(), expected);
}

template <typename SM>
void Contract_Empty(const Factory<SM>& make) {
  auto sm = make();
  ASSERT_NE(sm, nullptr);
  EXPECT_EQ(sm->lastApplied(), kNoIndex);
  std::string out;
  EXPECT_FALSE(sm->get("nope", out));
  auto view = sm->snapshotView();
  ASSERT_NE(view, nullptr);
  EXPECT_EQ(view->serialize().size(), static_cast<size_t>(16));
}

template <typename SM>
void Contract_BasicOps(const Factory<SM>& make) {
  auto sm = make();
  sm->apply(put(1, "k", "v", 7, 42));
  std::string out;
  ASSERT_TRUE(sm->get("k", out));
  EXPECT_EQ(out, "v");
  EXPECT_EQ(sm->lastApplied(), static_cast<Index>(1));
  sm->apply(put(2, "k", "v2", 7, 43));
  ASSERT_TRUE(sm->get("k", out));
  EXPECT_EQ(out, "v2");
  sm->apply(del(3, "k", 7, 44));
  EXPECT_FALSE(sm->get("k", out));
  EXPECT_EQ(sm->lastApplied(), static_cast<Index>(3));
}

template <typename SM>
void Contract_IdempotentSameIndex(const Factory<SM>& make) {
  auto sm = make();
  sm->apply(put(1, "k", "v", 7, 42));
  sm->apply(put(1, "k", "MUST-NOT-APPLY", 7, 42));
  std::string out;
  ASSERT_TRUE(sm->get("k", out));
  EXPECT_EQ(out, "v");
  EXPECT_EQ(sm->lastApplied(), static_cast<Index>(1));
}

// A retried request is re-appended at a HIGHER index. m2-design 6.5 defines the
// duplicate rule as "requestId <= lastRequestId -> directly discard": the data
// must not change and the entry is not applied, so the applied watermark does
// not move either. The mem baseline (src/kv/kv_state_machine.cpp) is the
// reference and is deliberately NOT modified for M6.9; the LSM backend mirrors
// it exactly (see docs/m6-design.md 10.14).
template <typename SM>
void Contract_IdempotentHigherIndexKeepsData(const Factory<SM>& make) {
  auto sm = make();
  sm->apply(put(1, "k", "v", 7, 42));
  sm->apply(put(2, "k", "MUST-NOT-APPLY", 7, 42));
  std::string out;
  ASSERT_TRUE(sm->get("k", out));
  EXPECT_EQ(out, "v");
  EXPECT_EQ(sm->lastApplied(), static_cast<Index>(1));  // discarded, not applied
}

template <typename SM>
void Contract_MarkersAdvanceApplied(const Factory<SM>& make) {
  auto sm = make();
  sm->apply(noop(1, OpCode::kGet));
  EXPECT_EQ(sm->lastApplied(), static_cast<Index>(1));
  sm->apply(noop(2, OpCode::kConfig));
  EXPECT_EQ(sm->lastApplied(), static_cast<Index>(2));
  std::string out;
  EXPECT_FALSE(sm->get("k", out));
}

template <typename SM>
void Contract_CanonicalState(const Factory<SM>& make) {
  auto sm = make();
  for (const LogEntry& e : canonicalOps()) sm->apply(e);
  checkCanonicalState(*sm);
}

template <typename SM>
void Contract_SnapshotDeterministic(const Factory<SM>& make) {
  auto a = make();
  auto b = make();
  for (const LogEntry& e : canonicalOps()) {
    a->apply(e);
    b->apply(e);
  }
  auto va = a->snapshotView();
  auto vb = b->snapshotView();
  ASSERT_NE(va, nullptr);
  ASSERT_NE(vb, nullptr);
  EXPECT_EQ(va->serialize(), vb->serialize());
}

template <typename SM>
void Contract_StreamMatchesSerialize(const Factory<SM>& make) {
  auto sm = make();
  for (const LogEntry& e : canonicalOps()) sm->apply(e);
  auto view = sm->snapshotView();
  ASSERT_NE(view, nullptr);
  const Bytes expected = view->serialize();
  const size_t chunks[] = {1, 2, 3, 7, 16, 1024};
  for (size_t chunk : chunks) {
    auto stream = view->stream();
    ASSERT_NE(stream, nullptr);
    Bytes got;
    Bytes piece;
    while (stream->next(piece, chunk)) {
      got.insert(got.end(), piece.begin(), piece.end());
    }
    EXPECT_EQ(got, expected) << "chunk=" << chunk;
  }
}

template <typename SM>
void Contract_SnapshotRoundTrip(const Factory<SM>& make) {
  auto a = make();
  for (const LogEntry& e : canonicalOps()) a->apply(e);
  auto view = a->snapshotView();
  ASSERT_NE(view, nullptr);
  const Bytes payload = view->serialize();

  auto b = make();
  ASSERT_TRUE(b->restore(payload));
  checkCanonicalState(*b);
  // Re-serialising may reorder unordered_map entries, so compare the logical
  // state and require the re-serialised payload to be a state-level fixed point.
  auto vb = b->snapshotView();
  ASSERT_NE(vb, nullptr);
  auto c = make();
  ASSERT_TRUE(c->restore(vb->serialize()));
  checkCanonicalState(*c);
}

template <typename SM>
void Contract_RestoreRejectsMalformed(const Factory<SM>& make) {
  auto a = make();
  for (const LogEntry& e : canonicalOps()) a->apply(e);
  auto view = a->snapshotView();
  ASSERT_NE(view, nullptr);
  const Bytes payload = view->serialize();

  auto b = make();
  ASSERT_TRUE(b->restore(payload));
  checkCanonicalState(*b);

  // every proper prefix must be rejected, and must not panic under ASan
  for (size_t i = 0; i < payload.size(); ++i) {
    Bytes prefix(payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(i));
    EXPECT_FALSE(b->restore(prefix)) << "prefix_len=" << i;
  }
  Bytes trailing = payload;
  trailing.push_back(0xAB);
  EXPECT_FALSE(b->restore(trailing));
  EXPECT_FALSE(b->restore(cat({be64(1), be32(0xFFFFFFFFu)})));
  EXPECT_FALSE(b->restore(cat({be64(1), be32(1)})));
  EXPECT_FALSE(b->restore(cat({be64(1), be32(1), be32(0xFFFFFFFFu), be32(0)})));
  EXPECT_FALSE(b->restore(cat({be64(1), be32(1), be32(1), be32(0xFFFFFFFFu),
                               Bytes(1, static_cast<Byte>('k'))})));
  EXPECT_FALSE(b->restore(cat({be64(1), be32(0), be32(0xFFFFFFFFu)})));

  // state must be unchanged by all the rejected attempts
  checkCanonicalState(*b);
}

template <typename SMA, typename SMB>
void Contract_CrossRestore(const Factory<SMA>& makeA, const Factory<SMB>& makeB) {
  auto a = makeA();
  for (const LogEntry& e : canonicalOps()) a->apply(e);
  auto view = a->snapshotView();
  ASSERT_NE(view, nullptr);
  const Bytes payload = view->serialize();
  auto b = makeB();
  ASSERT_TRUE(b->restore(payload));
  checkCanonicalState(*b);
}

}  // namespace raftkv::smtest

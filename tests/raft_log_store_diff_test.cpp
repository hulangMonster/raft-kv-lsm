
// M6.10 N8: cross-engine differential test, FileLogStore vs LsmLogStore.
//
// Both engines execute the SAME op sequence; their observable state
// (lastIndex/lastTerm/firstIndex/lastIncluded*/termAt/slice with all clamp and
// out-of-range variants) must match at every step. Ported from the reviewer's
// /tmp/diff_test.cpp (which reported TOTAL Mismatches=0). The same body also
// carries the N2 setBoundary-backwards regression.
#include <gtest/gtest.h>

#include <climits>
#include <cstdint>
#include <filesystem>
#include <random>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "raft/log_store.h"
#include "raft/lsm_log_store.h"

using namespace raftkv::raft;

namespace {

std::string tempDir(const std::string& tag) {
  static int c = 0;
  auto p = std::filesystem::temp_directory_path() /
           ("rkl_diff_" + tag + "_" + std::to_string(::getpid()) + "_" +
            std::to_string(c++));
  std::filesystem::create_directories(p);
  return p.string();
}

LogEntry E(Index i, Term t, const std::string& k, const std::string& v) {
  LogEntry e;
  e.index = i;
  e.term = t;
  e.op = raftkv::OpCode::kPut;
  e.key = k;
  e.value = v;
  e.clientId = 7;
  e.requestId = i;
  return e;
}

std::string dump(LogStore& s) {
  std::ostringstream o;
  o << "last=" << s.lastIndex() << " lastTerm=" << s.lastTerm()
    << " first=" << s.firstIndex() << " lii=" << s.lastIncludedIndex()
    << " lit=" << s.lastIncludedTerm();
  for (Index i = 0; i <= 12; ++i) o << " t" << i << "=" << s.termAt(i);
  auto enc = [](const std::vector<LogEntry>& v) {
    std::ostringstream x;
    x << "n" << v.size();
    for (const auto& e : v) {
      x << "(" << e.index << "," << e.term << "," << e.key << "," << e.value << ")";
    }
    return x.str();
  };
  o << " S1=" << enc(s.slice(1, 10, SIZE_MAX));
  o << " S3=" << enc(s.slice(3, 2, SIZE_MAX));
  o << " Sclamp=" << enc(s.slice(1, 10, 1));
  o << " Sbig=" << enc(s.slice(99, 10, SIZE_MAX));
  o << " Szero=" << enc(s.slice(1, 0, SIZE_MAX));
  return o.str();
}

struct Op {
  int kind;  // 0 append, 1 appendNoSync+sync, 2 truncSuffix, 3 truncNoSync+sync, 4 compact, 5 setBoundary, 6 persistMeta
  std::vector<LogEntry> entries;
  Index index = 0;
  Term term = 0;
  Term metaTerm = 0;
  int metaVote = 0;
};

bool applyOp(LogStore& s, const Op& o) {
  switch (o.kind) {
    case 0:
      return s.append(o.entries);
    case 1: {
      bool ok = s.appendNoSync(o.entries);
      return ok && s.sync();
    }
    case 2:
      return s.truncateSuffix(o.index);
    case 3: {
      bool ok = s.truncateSuffixNoSync(o.index);
      return ok && s.sync();
    }
    case 4:
      return s.compact(o.index, o.term);
    case 5:
      s.setBoundary(o.index, o.term);
      return true;
    case 6:
      return s.persistMeta(o.metaTerm, o.metaVote);
    default:
      return false;
  }
}

void compareStep(LogStore& a, LogStore& b, const Op& o, int step, int* mismatches) {
  const int ra = applyOp(a, o) ? 1 : 0;
  const int rb = applyOp(b, o) ? 1 : 0;
  const std::string sa = dump(a);
  const std::string sb = dump(b);
  EXPECT_EQ(ra, rb) << "step=" << step << " kind=" << o.kind;
  EXPECT_EQ(sa, sb) << "step=" << step << " kind=" << o.kind;
  if (ra != rb || sa != sb) ++(*mismatches);
}

}  // namespace

TEST(RaftLogStoreDiff, FixedEdgeSequence) {
  const std::string da = tempDir("fa");
  const std::string db = tempDir("lb");
  FileLogStore A(da);
  LsmLogStore B(db);
  std::vector<Op> ops;
  auto app = [&](Index lo, Index hi, Term t) {
    Op o;
    o.kind = 0;
    for (Index i = lo; i <= hi; ++i) {
      o.entries.push_back(E(i, t, "k" + std::to_string(i), "v" + std::to_string(i)));
    }
    ops.push_back(o);
  };
  app(1, 4, 1);
  ops.push_back({2, {}, 0});
  ops.push_back({2, {}, 5});
  ops.push_back({2, {}, 6});
  ops.push_back({2, {}, 4});
  ops.push_back({2, {}, 3});
  ops.push_back({2, {}, 3});
  app(3, 4, 9);
  ops.push_back({4, {}, 2, 1});
  ops.push_back({2, {}, 2});
  ops.push_back({2, {}, 1});
  ops.push_back({2, {}, 6});
  ops.push_back({2, {}, 5});
  ops.push_back({4, {}, 10, 3});
  ops.push_back({0, {}, 0});
  ops.push_back({2, {}, 11});
  ops.push_back({0, {}, 0});
  ops.push_back({5, {}, 12, 4});
  ops.push_back({0, {}, 0});

  int mismatches = 0;
  int step = 0;
  for (const Op& o : ops) compareStep(A, B, o, step++, &mismatches);
  EXPECT_EQ(mismatches, 0) << "EDGE sequence mismatch";
  std::filesystem::remove_all(da);
  std::filesystem::remove_all(db);
}

TEST(RaftLogStoreDiff, RandomFuzz4000Steps) {
  const std::string da = tempDir("fa2");
  const std::string db = tempDir("lb2");
  FileLogStore A(da);
  LsmLogStore B(db);
  std::mt19937_64 rng(12345);
  Index next = 1;
  int mismatches = 0;
  for (int i = 0; i < 4000; ++i) {
    const int kind = static_cast<int>(rng() % 5);
    Op o;
    o.kind = kind;
    const Index last = A.lastIndex();
    const Index first = A.firstIndex();
    switch (kind) {
      case 0:
      case 1: {
        const int n = static_cast<int>(rng() % 4);
        const Term t = 1 + static_cast<Term>(rng() % 5);
        for (int j = 0; j < n; ++j) {
          o.entries.push_back(E(next, t, "k" + std::to_string(next), "v" + std::to_string(next)));
          ++next;
        }
        break;
      }
      case 2:
      case 3: {
        const uint64_t r = rng() % 10;
        const Index cand[] = {0, last, last + 1, last + 2, first, first > 0 ? first - 1 : 0, 1};
        o.index = cand[r % 7];
        break;
      }
      case 4: {
        const uint64_t r = rng() % 4;
        o.index = (r == 0) ? 0 : (r == 1 ? last : (r == 2 ? last + 1 : first));
        o.term = A.termAt(o.index);
        if (o.term == kNoTerm) o.term = 1;
        break;
      }
    }
    compareStep(A, B, o, i, &mismatches);
    if (mismatches > 30) break;
  }
  EXPECT_EQ(mismatches, 0) << "FUZZ sequence mismatch";
  std::filesystem::remove_all(da);
  std::filesystem::remove_all(db);
}

// M6.10 N2 regression (reviewer repro): after compact(15,1), a BACKWARDS
// setBoundary(0,2) must leave LsmLogStore observationally identical to
// FileLogStore. Before the fix LsmLogStore kept lastIndex=15 and termAt(1) read
// out of bounds of an empty terms_ vector.
TEST(RaftLogStoreDiff, SetBoundaryBackwardsMatchesFileLogStore) {
  const std::string da = tempDir("fa3");
  const std::string db = tempDir("lb3");
  FileLogStore A(da);
  LsmLogStore B(db);
  std::vector<LogEntry> v;
  for (Index i = 1; i <= 15; ++i) v.push_back(E(i, 1, "k", "v"));
  ASSERT_TRUE(A.append(v));
  ASSERT_TRUE(B.append(v));
  ASSERT_TRUE(A.compact(15, 1));
  ASSERT_TRUE(B.compact(15, 1));
  A.setBoundary(0, 2);
  B.setBoundary(0, 2);
  const std::string sa = dump(A);
  const std::string sb = dump(B);
  EXPECT_EQ(sa, sb);
  EXPECT_EQ(B.lastIndex(), static_cast<Index>(0));
  EXPECT_EQ(B.firstIndex(), static_cast<Index>(1));
  EXPECT_EQ(B.termAt(1), kNoTerm);
  EXPECT_TRUE(B.slice(1, 10, SIZE_MAX).empty());
  std::filesystem::remove_all(da);
  std::filesystem::remove_all(db);
}

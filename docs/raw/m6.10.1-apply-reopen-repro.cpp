
// M6.10.1 BLOCKER repro (standalone): LsmKvStateMachine::apply of a value above
// the lsm write_buffer_size (default 4 MiB), then process-exit/Close -> reopen.
//
// Reproduced deterministically on lsm f06a44d: the engine leaves an orphan
// *.sst without a MANIFEST, so DB::Open fails with
//   VersionSet::RecoverManifest: 元数据缺失但目录非空（*.sst 或 MANIFEST-*）
// See docs/raw/m6.10.1-BLOCKER-lsm-flush-close.md for the raw-lsm repro and the
// proposed engine fix. Build:
//   g++ -std=c++17 -I src -I /tmp/lsm-pin-f06a44d/src //       docs/raw/m6.10.1-apply-reopen-repro.cpp -o /tmp/apply_reopen //       build/libraftkv_raft.a build/lsm-build/liblsm.a -lpthread
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <unistd.h>
#include "kv/lsm_kv_state_machine.h"
using namespace raftkv;
int main() {
  auto p = std::filesystem::temp_directory_path() / ("sm_apply_reopen_" + std::to_string(getpid()));
  std::filesystem::create_directories(p);
  const std::string dir = p.string();
  {
    LsmKvStateMachine sm(dir);
    raft::LogEntry e;
    e.index = 1; e.term = 1; e.op = OpCode::kPut;
    e.key = "huge"; e.value = std::string(6u * 1024u * 1024u, 'h');
    e.clientId = 1; e.requestId = 1;
    sm.apply(e);
    std::string out;
    std::printf("in-process get=%d size=%zu\n", (int)sm.get("huge", out), out.size());
  }
  try {
    LsmKvStateMachine sm2(dir);
    std::string out;
    std::printf("reopen get=%d size=%zu\n", (int)sm2.get("huge", out), out.size());
  } catch (const std::exception& ex) {
    std::printf("reopen FAILED: %s\n", ex.what());
  }
  std::filesystem::remove_all(dir);
  return 0;
}

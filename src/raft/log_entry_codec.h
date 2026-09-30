// raft 持久化层的**唯一**条目/元数据编码实现（M6.1 抽出）。
//
// 为什么抽出来：M6 的 LsmLogStore 必须与 FileLogStore **共用同一份**字节布局
// （docs/m6-design.md §2.2「value 逐字复用 encodeEntry()」），而原实现位于
// file_log_store.cpp 的匿名 namespace 内 —— 外部 TU 无法链接。抽到本头后
// 两个引擎都用这一份实现，「不漂移」由编译器保证，而不是靠人盯两份拷贝。
//
// 布局（全大端，与 M2.4 的磁盘格式逐字相同）：
//   entry payload = [index:8][term:8][op:1][keyLen:4][valLen:4]
//                   [clientId:8][requestId:8][key...][value...]
//   meta  payload = [term:8][votedFor:4]   （votedFor 是 int 的位模式；-1 ⇒ 0xFFFFFFFF）
#ifndef RAFTKV_RAFT_LOG_ENTRY_CODEC_H_
#define RAFTKV_RAFT_LOG_ENTRY_CODEC_H_

#include <cstdint>
#include <cstddef>

#include "common.h"
#include "raft/types.h"

namespace raftkv::raft {

constexpr size_t kEntryFixedLen = 41;   // 8+8+1+4+4+8+8
constexpr size_t kMetaPayloadLen = 12;  // term(8) + votedFor(4)

inline void putU64BE(Bytes& out, uint64_t v) {
  for (int i = 7; i >= 0; --i) {
    out.push_back(static_cast<Byte>((v >> (i * 8)) & 0xff));
  }
}

inline uint64_t getU64BE(const Byte* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
  return v;
}

// 逐字搬运自 file_log_store.cpp（M2.4 L105-118）。
inline Bytes encodeEntry(const LogEntry& e) {
  Bytes p;
  p.reserve(kEntryFixedLen + e.key.size() + e.value.size());
  putU64BE(p, e.index);
  putU64BE(p, e.term);
  p.push_back(static_cast<Byte>(e.op));
  putU32(p, static_cast<uint32_t>(e.key.size()));
  putU32(p, static_cast<uint32_t>(e.value.size()));
  putU64BE(p, e.clientId);
  putU64BE(p, e.requestId);
  p.insert(p.end(), e.key.begin(), e.key.end());
  p.insert(p.end(), e.value.begin(), e.value.end());
  return p;
}

// 逐字搬运自 file_log_store.cpp（M2.4 L120-142）：校验定长 + keyLen/valLen 自洽 + op 合法。
inline bool decodeEntry(const Byte* p, size_t n, LogEntry& e) {
  if (n < kEntryFixedLen) return false;
  const uint8_t op = p[16];
  if (op != static_cast<uint8_t>(OpCode::kPut) &&
      op != static_cast<uint8_t>(OpCode::kGet) &&
      op != static_cast<uint8_t>(OpCode::kDel) &&
      op != static_cast<uint8_t>(OpCode::kConfig)) {  // M4: 配置条目（m4-prerequisites §5.1-14）
    return false;
  }
  const size_t keyLen = getU32(p + 17);
  const size_t valLen = getU32(p + 21);
  if (n != kEntryFixedLen + keyLen + valLen) return false;

  e.index = getU64BE(p);
  e.term = getU64BE(p + 8);
  e.op = static_cast<OpCode>(op);
  e.clientId = getU64BE(p + 25);
  e.requestId = getU64BE(p + 33);
  e.key.assign(reinterpret_cast<const char*>(p + kEntryFixedLen), keyLen);
  e.value.assign(
      reinterpret_cast<const char*>(p + kEntryFixedLen + keyLen), valLen);
  return true;
}

// meta payload：与 file_log_store.cpp 的 kMetaPayloadLen 同构（M6 设计 §2.2）。
inline Bytes encodeMeta(Term term, int votedFor) {
  Bytes payload;
  payload.reserve(kMetaPayloadLen);
  putU64BE(payload, term);
  putU32(payload, static_cast<uint32_t>(votedFor));
  return payload;
}

inline bool decodeMeta(const Byte* p, size_t n, Term& term, int& votedFor) {
  if (n != kMetaPayloadLen) return false;
  term = getU64BE(p);
  votedFor = static_cast<int>(getU32(p + 8));
  return true;
}

}  // namespace raftkv::raft

#endif  // RAFTKV_RAFT_LOG_ENTRY_CODEC_H_

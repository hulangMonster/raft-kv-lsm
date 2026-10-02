
#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>
#include "raft/lsm_log_store.h"
using namespace raftkv;
using namespace raftkv::raft;
int main(int argc, char** argv){
  const int mb = (argc>1)? std::stoi(argv[1]) : 2;
  auto dir = (std::filesystem::temp_directory_path()/("lsmstore_"+std::to_string(getpid()))).string();
  std::filesystem::create_directories(dir);
  {
    LsmLogStore s(dir);
    std::vector<LogEntry> v;
    for (int i=1;i<=3;i++){
      LogEntry e; e.index=i; e.term=1; e.op=OpCode::kPut; e.key="k"+std::to_string(i);
      e.value=std::string(mb*1024*1024,'x'); e.clientId=1; e.requestId=i; v.push_back(e);
    }
    bool ok = s.append(v);
    std::printf("append(%dMiB x3) ok=%d\n", mb, (int)ok);
  }
  {
    LsmLogStore s2(dir);
    Term t=0; int vf=-1; Index li=0;
    bool ok = s2.load(t, vf, li);
    std::printf("reopen load=%d lastIndex=%llu\n", (int)ok, (unsigned long long)li);
  }
  std::filesystem::remove_all(dir);
  return 0;
}


#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <unistd.h>
#include "db.h"
#include "write_batch.h"
int main(int argc, char** argv){
  const size_t mb = std::stoul(argv[1]);
  const int count = std::stoi(argv[2]);
  const int sleepMs = std::stoi(argv[3]);
  const size_t wbuf = std::stoul(argv[4]);
  const int doGet = (argc > 5) ? std::stoi(argv[5]) : 0;
  auto dir = (std::filesystem::temp_directory_path()/("raw_lsm2_"+std::to_string(getpid()))).string();
  std::filesystem::create_directories(dir);
  lsm::Options o; o.write_buffer_size = wbuf*1024*1024;
  {
    lsm::DB* db=nullptr;
    auto st = lsm::DB::Open(o, dir, &db);
    lsm::WriteOptions wo; wo.sync=false;
    std::string v(mb*1024*1024,'x');
    for (int i=0;i<count;i++){
      lsm::WriteBatch wb; wb.Put(lsm::Slice("k"+std::to_string(i)), lsm::Slice(v));
      auto s = db->Write(wo, &wb);
      if(!s.ok()){ std::printf("write %d FAIL %s\n", i, s.ToString().c_str()); }
      if(doGet){ std::string g; auto gs=db->Get(lsm::Slice("k0"), &g); (void)gs; }
    }
    if (sleepMs>0) std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
    db->Close(); delete db;
  }
  lsm::DB* db=nullptr;
  auto st = lsm::DB::Open(o, dir, &db);
  std::printf("mb=%zu count=%d sleep=%d wbuf=%zu get=%d -> open2 ok=%d %s\n",
    mb,count,sleepMs,wbuf,doGet,(int)st.ok(), st.ok()?"":st.ToString().c_str());
  if (db){ db->Close(); delete db; }
  std::filesystem::remove_all(dir);
  return 0;
}

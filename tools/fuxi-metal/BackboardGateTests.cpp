/* Exercises the production record/locking logic on a private host directory. */
#include "BackboardGate.hpp"
#include <cassert>
#include <cstdlib>
#include <string>
#include <memory>
#include <sys/wait.h>
static constexpr const char *bootA="aaaaaaaa-1111-2222-3333-444444444444",*bootB="bbbbbbbb-1111-2222-3333-444444444444";
static int storage(const char *p){return open(p,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);}
static std::string record(const char *p){char b[128]={};int dir=storage(p);int f=openat(dir,"gate.state",O_RDONLY);assert(f>=0);assert(read(f,b,sizeof(b)-1)>0);close(f);close(dir);return b;}
static void clear(const char *p){int d=storage(p);unlinkat(d,"gate.state",0);unlinkat(d,"gate.lock",0);unlinkat(d,"gate.tmp",0);close(d);}
static void childCrash(const char *p){
 pid_t child=fork();assert(child>=0);if(!child){auto gate=std::make_unique<fuxi::BackboardGate>();auto &g=*gate;assert(g.admit(bootA,storage(p)));_exit(0);}
 int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
}
// Each gate owns a bounded trace window. Keep independent fixtures off the
// ASan-instrumented main stack as stage attribution grows.
int main(){
 char path[]="/tmp/fuxi-backboard-gate-XXXXXX";assert(mkdtemp(path));
 childCrash(path);assert(record(path).find("0 armed")!=std::string::npos);
 {auto gate=std::make_unique<fuxi::BackboardGate>();auto &g=*gate;assert(!g.admit(bootA,storage(path)));} // One early death disables this boot.
 assert(record(path).find("1 denied")!=std::string::npos);
 {auto gate=std::make_unique<fuxi::BackboardGate>();auto &g=*gate;assert(!g.admit(bootA,storage(path)));}
 {auto gate=std::make_unique<fuxi::BackboardGate>();auto &g=*gate;assert(g.admit(bootB,storage(path)));assert(record(path).find("0 armed")!=std::string::npos);
  auto second=std::make_unique<fuxi::BackboardGate>();auto &overlapping=*second;assert(!overlapping.admit(bootB,storage(path)));
  for(unsigned i=0;i<fuxi::BackboardGate::goodFrames-1;++i)g.completed(3600000,8);
  assert(record(path).find("armed")!=std::string::npos);g.completed(3600000,8);
  assert(record(path).find("good")!=std::string::npos);
 }
 {auto gate=std::make_unique<fuxi::BackboardGate>();auto &g=*gate;assert(g.admit(bootB,storage(path)));
  for(unsigned i=0;i<fuxi::BackboardGate::goodFrames;++i)g.completed(3600000,8);
  g.fault();assert(g.blocked());g.completed(3600000,8);assert(record(path).find("denied")!=std::string::npos);
 }
 {auto gate=std::make_unique<fuxi::BackboardGate>();auto &g=*gate;assert(!g.admit(bootB,storage(path)));}
 {auto gate=std::make_unique<fuxi::BackboardGate>();auto &g=*gate;assert(g.admit(bootA,storage(path)));}
 clear(path);
 // Missing UUID, corrupt state, unsafe mode, symlink, hard link, and FIFO refuse.
 {auto gate=std::make_unique<fuxi::BackboardGate>();auto &g=*gate;assert(!g.admit("",storage(path)));}
 int dir=storage(path);int fd=openat(dir,"gate.state",O_WRONLY|O_CREAT|O_EXCL,0600);assert(fd>=0);
 assert(fuxi::privateStorage(dir,true,geteuid()));assert(!fuxi::privateStorage(dir,true,geteuid()+1));
 assert(write(fd,"bad\n",4)==4);close(fd);
 {auto gate=std::make_unique<fuxi::BackboardGate>();auto &g=*gate;assert(!g.admit(bootA,storage(path)));}clear(path);
 chmod(path,0755);{auto gate=std::make_unique<fuxi::BackboardGate>();auto &g=*gate;assert(!g.admit(bootA,storage(path)));}chmod(path,0700);
 assert(!symlinkat("missing",dir,"gate.state"));{auto gate=std::make_unique<fuxi::BackboardGate>();auto &g=*gate;assert(!g.admit(bootA,storage(path)));}clear(path);
 fd=openat(dir,"gate.state",O_WRONLY|O_CREAT|O_EXCL,0644);assert(fd>=0);close(fd);
 {auto gate=std::make_unique<fuxi::BackboardGate>();auto &g=*gate;assert(!g.admit(bootA,storage(path)));}
 assert(!fchmodat(dir,"gate.state",0600,0));assert(!linkat(dir,"gate.state",dir,"extra",0));
 {auto gate=std::make_unique<fuxi::BackboardGate>();auto &g=*gate;assert(!g.admit(bootA,storage(path)));}unlinkat(dir,"extra",0);clear(path);
 assert(!mkfifoat(dir,"gate.state",0600));{auto gate=std::make_unique<fuxi::BackboardGate>();auto &g=*gate;assert(!g.admit(bootA,storage(path)));}clear(path);
 {auto gate=std::make_unique<fuxi::BackboardGate>();auto &g=*gate;assert(g.admit(bootA,storage(path)));struct stat st{};assert(!fstatat(dir,"gate.state",&st,0));
  assert(st.st_uid==geteuid() && (st.st_mode&07777)==0600 && st.st_nlink==1);
  // Failure recording good still durably disables the next process.
  assert(!mkdirat(dir,"gate.tmp",0700));for(unsigned i=0;i<fuxi::BackboardGate::goodFrames;++i)g.completed(1,1);
  assert(g.blocked());assert(record(path).find("denied")!=std::string::npos);assert(!unlinkat(dir,"gate.tmp",AT_REMOVEDIR));
 }
 {auto gate=std::make_unique<fuxi::BackboardGate>();auto &g=*gate;assert(!g.admit(bootA,storage(path)));}
 {auto gate=std::make_unique<fuxi::BackboardGate>();auto &g=*gate;assert(g.admit(bootB,storage(path)));
  for(unsigned i=0;i<fuxi::BackboardGate::goodFrames;++i)g.completed(1,1);
  assert(!mkdirat(dir,"gate.tmp",0700));g.fault();assert(g.blocked());assert(record(path).find("denied")!=std::string::npos);
  assert(!unlinkat(dir,"gate.tmp",AT_REMOVEDIR));
 }
 {auto gate=std::make_unique<fuxi::BackboardGate>();auto &g=*gate;assert(!g.admit(bootB,storage(path)));}
 clear(path);close(dir);assert(!rmdir(path));
 puts("PASS: backboardd boot reset, crash strike/deny, good/rearm, runtime fault, exclusive lease, private record integrity and write failure");
}

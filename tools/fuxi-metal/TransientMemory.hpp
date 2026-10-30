/* Quiescent, non-imported, non-dedicated allocations only. Caller serializes and owns retirement. */
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
namespace fuxi {
inline std::atomic<uint64_t> surfaceImportHits{0},surfaceImportMisses{0},texturePoolHits{0},texturePoolMisses{0},texturePoolTrims{0},texturePoolBytes{0},texturePoolWasted{0},texturePoolIdleTrims{0},texturePoolPressureTrims{0},texturePoolRejectFull{0},texturePoolRejectBudget{0};
// Why a pool lookup missed (see MissCause); every miss lands in exactly one.
enum MissCause:unsigned{missCold,missInflight,missType,missSlack,missSmall,missIdleTrim,missPressure,missRejectFull,missRejectBudget,missCauses};
inline const char *const missCauseNames[missCauses]={"cold","inflight","type","slack","small","idle_trim","pressure","reject_full","reject_budget"};
inline std::array<std::atomic<uint64_t>,missCauses> texturePoolMissCause{};
inline std::array<std::atomic<uint64_t>,40> texturePoolClassHits{},texturePoolClassMisses{};
// Memory is allocated at a size class (1/8 steps above 32 KiB, 4 KiB below) and bound at offset 0 to any image whose requirement fits.
inline uint64_t memorySizeClass(uint64_t n){
 if(!n)return 0;unsigned top=63-__builtin_clzll(n);unsigned shift=top>15?top-3:12;uint64_t step=uint64_t(1)<<shift;return (n+step-1)&~(step-1);
}
inline unsigned memoryClassBucket(uint64_t n){unsigned b=n?64-__builtin_clzll(n):0;return b<40?b:39;}
struct MemoryKey {
 uint64_t bytes{};uint32_t type{};
 bool operator==(const MemoryKey &b) const{return bytes==b.bytes && type==b.type;}
};
template<class Handle> class TransientMemory {
public:
 static constexpr size_t maxEntries=256;
private:
 struct Entry {MemoryKey key{};Handle memory{};uint64_t returned{};};
 std::array<Entry,maxEntries> entries{};size_t used=0;
 struct Drop {uint64_t bytes{};MissCause cause{};};std::array<Drop,32> drops{};unsigned dropNext=0;std::array<uint64_t,256> seen{};
 void noteDrop(uint64_t bytes,MissCause c){drops[dropNext++%drops.size()]={bytes,c};}
 bool markSeen(uint64_t bytes){size_t i=(bytes>>12)*0x9E3779B1u%seen.size();for(size_t n=0;n<seen.size();++n,i=(i+1)%seen.size()){if(seen[i]==bytes)return true;if(!seen[i]){seen[i]=bytes;return false;}}return true;}
 static bool fits(uint64_t have,uint64_t need){return have>=need && have-need<=need/4+4096;}
 template<class Free> void drop(Entry &e,Free &free,std::atomic<uint64_t> &reason){free(e.memory,e.key.bytes);used-=e.key.bytes;texturePoolBytes-=e.key.bytes;noteDrop(e.key.bytes,&reason==&texturePoolIdleTrims?missIdleTrim:missPressure);e={};++texturePoolTrims;++reason;}
public:
 static constexpr size_t budget=96u*1024*1024;
 // Frames reuse blocks within milliseconds; keep idle ones across input pauses and animation gaps.
 static constexpr uint64_t idleNanos=30ull*1000*1000*1000;
 size_t bytes() const{return used;}
 // Smallest pooled block with bytes>=need, a permitted memory type, and at most need/4+4 KiB slack.
 bool take(uint64_t need,uint32_t typeBits,Handle &memory,MemoryKey &got){
  Entry *best=nullptr;
  for(auto &e:entries)if(e.memory && e.key.bytes>=need && e.key.bytes-need<=need/4+4096 && e.key.type<32 && (typeBits&(1u<<e.key.type)) && (!best || e.key.bytes<best->key.bytes))best=&e;
  if(!best){++texturePoolMisses;++texturePoolClassMisses[memoryClassBucket(need)];++texturePoolMissCause[classify(need)];return false;}
  memory=best->memory;got=best->key;used-=got.bytes;texturePoolBytes-=got.bytes;best->memory={};++texturePoolHits;++texturePoolClassHits[memoryClassBucket(got.bytes)];texturePoolWasted+=got.bytes-need;return true;
 }
 // A fitting block that take() refused can only differ in memory type; slack/small mean a pooled block within 2x/0.5x of the request.
 MissCause classify(uint64_t need){
  bool type=false,slack=false,small=false;
  for(auto &e:entries)if(e.memory){if(fits(e.key.bytes,need))type=true;else if(e.key.bytes<need){if(e.key.bytes>=need/2)small=true;}else if(e.key.bytes<=need*2)slack=true;}
  bool wasSeen=markSeen(memorySizeClass(need));
  if(type)return missType;
  for(unsigned i=0;i<drops.size();++i){auto &d=drops[(dropNext+drops.size()-1-i)%drops.size()];if(d.bytes && fits(d.bytes,need))return d.cause;}
  if(slack)return missSlack;
  if(small)return missSmall;
  if(!wasSeen)return missCold;
  return missInflight;
 }
 bool put(const MemoryKey &key,Handle memory,uint64_t now){
  if(!memory || !key.bytes)return false;
  if(key.bytes>budget-used){++texturePoolRejectBudget;noteDrop(key.bytes,missRejectBudget);return false;}
  for(auto &e:entries)if(!e.memory){e={key,memory,now};markSeen(key.bytes);used+=key.bytes;texturePoolBytes+=key.bytes;return true;}
  ++texturePoolRejectFull;noteDrop(key.bytes,missRejectFull);return false;
 }
 template<class Free> void trim(uint64_t now,bool all,Free free){
  for(auto &e:entries)if(e.memory && (all || (now>=e.returned && now-e.returned>=idleNanos)))drop(e,free,all?texturePoolPressureTrims:texturePoolIdleTrims);
 }
 // Free the longest-idle blocks until at least `need` bytes are released.
 template<class Free> void reclaim(uint64_t need,Free free){
  while(need){Entry *oldest=nullptr;for(auto &e:entries)if(e.memory && (!oldest || e.returned<oldest->returned))oldest=&e;
   if(!oldest)return;auto n=oldest->key.bytes;drop(*oldest,free,texturePoolPressureTrims);need=n>=need?0:need-n;}
 }
};
}

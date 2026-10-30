/* Pool policy and CPU storage ownership; no Vulkan or platform dependencies. */
#include "TransientMemory.hpp"
#include "CommandStorage.hpp"
#include <cassert>
#include <memory>
#include <thread>
int main(){
 using namespace fuxi;TransientMemory<unsigned> pool;
 // Size classes: 1/8 steps, monotone, never smaller than the request, bounded waste.
 assert(memorySizeClass(1)==4096 && memorySizeClass(4096)==4096 && memorySizeClass(4097)==8192 && memorySizeClass(0)==0);
 for(uint64_t n=1;n<(1ull<<28);n=n*9/8+7){auto c=memorySizeClass(n);assert(c>=n && c-n<=(n>>3)+4096 && memorySizeClass(c)==c);}
 assert(memorySizeClass(11059200)==11534336 && memorySizeClass(11500000)==11534336);
 unsigned h=0;MemoryKey got{};MemoryKey key{memorySizeClass(100000),1};
 assert(!pool.take(100000,~0u,h,got));assert(pool.put(key,1,0));
 assert(!pool.take(key.bytes+1,~0u,h,got));            // too small
 assert(!pool.take(100000,~2u,h,got));                  // memory type 1 not permitted
 assert(!pool.take(key.bytes/2,~0u,h,got));             // too much slack
 auto wasted=texturePoolWasted.load();assert(pool.take(99000,1u<<1,h,got) && h==1 && got==key && !pool.bytes() && texturePoolWasted==wasted+key.bytes-99000);
 // Smallest sufficient block wins; the larger stays pooled.
 MemoryKey big{key.bytes+8192,1};assert(pool.put(big,2,0) && pool.put(key,3,0));assert(pool.take(100000,2,h,got) && h==3 && pool.bytes()==big.bytes);
 assert(pool.take(100000,2,h,got) && h==2 && !pool.bytes());
 unsigned freed=0;auto free=[&](unsigned,size_t){++freed;};
 MemoryKey tiny{4096,1};constexpr unsigned cap=TransientMemory<unsigned>::maxEntries;for(unsigned i=1;i<=cap;++i)assert(pool.put(tiny,i,0));assert(!pool.put(tiny,cap+1,0) && texturePoolRejectFull==1);
 pool.trim(pool.idleNanos-1,false,free);assert(!freed);pool.trim(pool.idleNanos,false,free);assert(freed==cap && !pool.bytes() && texturePoolIdleTrims==cap && pool.idleNanos>=30ull*1000*1000*1000);
 // Miss causes: a re-request after the entry was trimmed/rejected is attributed to that event, not to cold demand.
 {std::array<uint64_t,missCauses> before{};for(unsigned i=0;i<missCauses;++i)before[i]=texturePoolMissCause[i].load();auto n=[&](MissCause c){return texturePoolMissCause[c].load()-before[c];};
  assert(!pool.take(4096,~0u,h,got) && n(missIdleTrim)==1);                                   // idle-trimmed class
  assert(!pool.take(3000000,~0u,h,got) && n(missCold)==1);                                    // never seen
  assert(!pool.take(3000000,~0u,h,got) && n(missInflight)==1);                                // seen, nothing pooled: blocks are out
  MemoryKey wide{memorySizeClass(1000000),1};assert(pool.put(wide,7,0));assert(!pool.take(wide.bytes*3/5,~0u,h,got) && n(missSlack)==1);assert(!pool.take(wide.bytes*4/3,~0u,h,got) && n(missSmall)==1);
  assert(!pool.take(wide.bytes,1u<<3,h,got) && n(missType)==1);pool.trim(0,true,free);assert(!pool.bytes());}
 auto large=key;large.bytes=pool.budget;assert(pool.put(large,40,100));assert(!pool.put(key,41,100) && texturePoolRejectBudget==1);
 pool.trim(99,false,free);assert(pool.bytes()==pool.budget);pool.trim(100,true,free);assert(!pool.bytes() && freed==cap+2);
 // Pressure reclaims only as much as needed, longest-idle first.
 MemoryKey m{1<<20,0};for(unsigned i=0;i<4;++i)assert(pool.put(m,10+i,10-i));freed=0;unsigned last=0;auto track=[&](unsigned v,size_t){++freed;last=v;};
 pool.reclaim(m.bytes+1,track);assert(freed==2 && last==12 && pool.bytes()==2*m.bytes);pool.reclaim(0,track);assert(freed==2);pool.reclaim(UINT64_MAX,track);assert(freed==4 && !pool.bytes());
 assert(texturePoolHits==3 && texturePoolMisses==10 && !texturePoolBytes && texturePoolClassHits[memoryClassBucket(key.bytes)]==3 && texturePoolPressureTrims==6);
 CommandStorage<std::shared_ptr<int>> storage;auto a=storage.acquire();auto *identity=a.data();auto owned=std::make_shared<int>(12);a.push_back(owned);
 auto b=storage.acquire();assert(b.data()!=identity);storage.release(a);assert(owned.use_count()==1);
 auto c=storage.acquire();assert(c.empty() && c.data()==identity && c.capacity()>=256);
 c.reserve(1024);identity=c.data();storage.release(c);auto d=storage.acquire();assert(d.data()==identity && d.capacity()==1024);
 // Concurrent return/acquire owns disjoint vectors; live command data is never borrowed.
 std::thread other([&]{for(unsigned i=0;i<100;++i){auto v=storage.acquire();v.push_back(owned);storage.release(v);}});
 for(unsigned i=0;i<100;++i){auto v=storage.acquire();assert(v.data()!=d.data());storage.release(v);}other.join();
 struct Block {uint8_t bytes[64*1024];};UploadStorage<Block *> uploads;UploadRing<Block *> ring,flight;
 unsigned allocations=0,releases=0;auto allocate=[&](size_t n){assert(n==sizeof(Block));++allocations;return new Block{};};auto contents=[](Block *block){return block->bytes;};auto drop=[&](Block *block){++releases;delete block;};
 uint8_t bytes[4096];memset(bytes,0x34,sizeof(bytes));auto first=ring.copy(bytes,37,uploads,allocate,contents);memset(bytes,0x56,sizeof(bytes));
 auto second=ring.copy(bytes,128,uploads,allocate,contents);assert(first.handle==second.handle && second.offset==256 && first.length==37 && first.handle->bytes[0]==0x34 && !first.handle->bytes[37]);
 auto pending=flight.copy(bytes,128,uploads,allocate,contents);assert(pending.handle!=first.handle);
 for(unsigned i=0;i<20;++i)ring.copy(bytes,4096,uploads,allocate,contents);assert(allocations==3);
 ring.release(uploads,true,drop);auto again=ring.copy(bytes,128,uploads,allocate,contents);assert(again.handle==first.handle && allocations==3);
 flight.release(uploads,false,drop);assert(releases==1);ring.release(uploads,true,drop);uploads.clear(drop);assert(allocations==releases);
 bool refused=false;try{ring.copy(bytes,4097,uploads,allocate,contents);}catch(const std::runtime_error&){refused=true;}assert(refused);
 // Overflow storage is discarded rather than retained by the bounded cache.
 CommandStorage<uint8_t> bounded;auto huge=bounded.acquire();huge.reserve(bounded.budget+1);auto *largeIdentity=huge.data();bounded.release(huge);auto small=bounded.acquire();assert(small.data()!=largeIdentity);
 puts("PASS: size-class memory reuse (type/slack/best fit), entry/byte bounds, idle trim, incremental pressure reclaim, entry/byte bounds, idle/pressure trim, disjoint storage reuse, inline slices/copy/padding, flight isolation and poison discard");
}

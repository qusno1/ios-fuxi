/* Cache collisions must affect cost only; full-copy reasons and ownership stay exact. */
#define FUXI_MTL_HOST_TEST
#include "FrontendEncoding.hpp"
#include <cassert>
#include <unordered_set>
int main(){
 using namespace fuxi;CommandResourceCache cache;alignas(16) uint8_t identities[2048]{};auto *a=identities,*collision=identities+1024;
 assert(cache.seen(nullptr,true));assert(!cache.seen(a,true));assert(cache.seen(a,true));
 assert(!cache.seen(a,false,ShadowFullReason::useResource));assert(cache.seen(a,false,ShadowFullReason::useResource));
 assert(cache.seen(a,true));assert(!cache.seen(a,false,ShadowFullReason::blit));assert(cache.seen(a,false,ShadowFullReason::blit));
 assert(!cache.seen(collision,true));assert(!cache.seen(a,true));cache.clear();assert(!cache.seen(a,true));
 // Simulate the owning fallback, with cache evictions, encoder boundaries and aliases.
 cache.clear();std::unordered_set<void *> owners;unsigned retains=0,releases=0;ShadowRanges ranges(4096);
 auto retain=[&](void *r,bool bounded,ShadowFullReason reason){if(cache.seen(r,bounded,reason))return;if(!bounded)ranges.full(reason);if(owners.insert(r).second)++retains;};
 for(unsigned encoder=0;encoder<31;++encoder)for(unsigned draw=0;draw<5;++draw){retain(a,true,ShadowFullReason::other);retain(collision,true,ShadowFullReason::other);retain(a,false,ShadowFullReason::useResource);retain(a,false,ShadowFullReason::blit);}
 assert(retains==2 && owners.size()==2 && ranges.bytes()==4096 && ranges.fullReason(ShadowFullReason::useResource) && ranges.fullReason(ShadowFullReason::blit));
 for(auto r:owners){(void)r;++releases;}owners.clear();cache.clear();assert(releases==retains);retain(a,true,ShadowFullReason::other);assert(retains==3);
 OperationDiagnostic diagnostic;assert(diagnostic.empty() && !*diagnostic.c_str());std::string original(4096,'x');diagnostic=original.c_str();auto copy=diagnostic;
 assert(copy.c_str()==diagnostic.c_str());diagnostic="replacement";assert(copy.c_str()!=diagnostic.c_str() && original==copy.c_str());
 auto moved=std::move(copy);assert(moved.c_str()==original && copy.empty());assert(std::string("desc=")+moved=="desc="+original);
 diagnostic.clear();assert(diagnostic.empty());diagnostic="";assert(diagnostic.empty());diagnostic=nullptr;assert(diagnostic.empty());
 OperationDiagnostic equal;equal=original.c_str();assert(equal==moved);equal="different";assert(!(equal==moved));
 { // lazy diagnostic text: produced on first use, once, shared by copies
  int made=0;LazyText text([&]{++made;return std::string("lazy pass text");});OperationDiagnostic lazy;lazy=text;auto copy2=lazy;
  assert(!lazy.empty() && made==0 && LazyText().empty() && !*LazyText().c_str());
  assert(std::string(copy2.c_str())=="lazy pass text" && std::string(lazy.c_str())=="lazy pass text" && made==1);
  OperationDiagnostic same;same=text;assert(same==lazy && made==1);OperationDiagnostic other;other="lazy pass text";assert(other==lazy);other="x";assert(!(other==lazy));
  lazy="plain";assert(std::string(lazy.c_str())=="plain" && std::string(copy2.c_str())=="lazy pass text");lazy.clear();assert(lazy.empty());
  LazyText thrower([]()->std::string{throw 1;});assert(!*thrower.c_str());
 }
 puts("PASS: resource cache collisions, bounded/full escalation and distinct reasons, command reset, immutable diagnostic copy/move/lifetime");
}

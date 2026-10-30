/* Slab free-list policy: alignment, splitting/coalescing, best fit, grow causes, bounds, trim. */
#include "SlabMemory.hpp"
#include <cassert>
#include <cstdio>
int main(){
 using namespace fuxi;using S=SlabMemory<unsigned>;S s;S::Block a{},b{},c{},d{};constexpr uint64_t slab=S::slabSize;
 assert(s.alloc(4096,4096,1,a)==S::grow && slabGrowCold==1 && !s.bytes());
 assert(s.alloc(0,4096,1,a)==S::ineligible && s.alloc(S::maxBlock+1,4096,1,a)==S::ineligible && s.alloc(4096,3,1,a)==S::ineligible);
 assert(s.add(100,0,0) && s.bytes()==slab && slabBytes==slab && slabFreeBytes==slab);
 // Offsets are aligned, sizes round up to the alignment, ranges are disjoint and inside the slab.
 assert(s.alloc(5000,4096,1,a)==S::ok && a.offset==0 && a.size==8192 && a.memory==100);
 assert(s.alloc(100,65536,1,b)==S::ok && b.offset==65536 && b.size==65536);               // leaves a gap before it
 assert(s.alloc(4096,4096,1,c)==S::ok && c.offset==8192);                                  // best fit fills the gap first
 assert(s.alloc(4096,1,2,d)==S::grow && slabGrowCold==2);                                  // type 1 is not permitted by this slab (type 0)
 // Free/coalesce restores one range.
 s.release(c,1);s.release(a,2);s.release(b,3);assert(slabFreeBytes==slab && slabFrees==3);
 assert(s.alloc(slab>>1,4096,1,a)==S::ineligible);
 assert(s.alloc(S::maxBlock,4096,1,a)==S::ok && a.offset==0);assert(s.alloc(S::maxBlock,4096,1,b)==S::ok);assert(s.alloc(S::maxBlock,4096,1,c)==S::ok);
 assert(s.alloc(S::maxBlock,4096,1,d)==S::ok && s.alloc(S::maxBlock,4096,1,d)==S::grow && slabGrowFull==1);
 // Fragmentation: free bytes exist but no contiguous range fits.
 S f;assert(f.add(1,0,0));S::Block p[8];for(auto &x:p)assert(f.alloc(S::maxBlock/2,4096,1,x)==S::ok);
 for(int i=0;i<8;i+=2)f.release(p[i],1);S::Block big{};assert(f.alloc(S::maxBlock,4096,1,big)==S::grow && slabGrowFrag==1);
 f.release(p[1],1);assert(f.alloc(S::maxBlock,4096,1,big)==S::ok);                           // adjacent frees coalesce
 // Stale or foreign releases are ignored.
 auto free0=slabFreeBytes.load();f.release(S::Block{9,0,4096,0},2);f.release(S::Block{1,0,4096,7},2);assert(slabFreeBytes==free0);
 // Slab count is capped.
 S cap;for(unsigned i=0;i<S::maxSlabs;++i)assert(cap.add(i+1,0,i));assert(!cap.canGrow() && !cap.add(99,0,0) && slabGrowRefused==1 && cap.bytes()==S::maxSlabs*slab);
 // Trim: busy slabs stay; idle empty slabs go except the newest warm one; `all` frees the rest.
 unsigned freed=0,last=0;auto track=[&](unsigned h,size_t n){++freed;last=h;assert(n==slab);};
 constexpr uint64_t sec=1000000000ull;S t;assert(t.add(1,0,0) && t.add(2,0,20*sec) && t.add(3,0,40*sec));
 t.trim(30*sec,false,track);assert(freed==1 && last==1 && t.bytes()==2*slab);                  // slab 1 idle 30 s; slab 2 not yet; slab 3 is the warm one
 S::Block hold{};assert(t.alloc(4096,4096,1,hold)==S::ok && hold.memory==2);t.trim(UINT64_MAX,false,track);assert(t.bytes()==2*slab && freed==1); // in use and warm slabs stay
 t.release(hold,50*sec);t.trim(0,true,track);assert(!t.bytes() && !t.emptyBytes() && freed==3);
 S r;assert(r.add(1,0,1) && r.add(2,0,2) && r.add(3,0,3));S::Block x{};assert(r.alloc(4096,4096,1,x)==S::ok);freed=0;r.reclaim(slab+1,track);assert(freed==2 && r.bytes()==slab && r.emptyBytes()==0);
 puts("PASS: slab allocator (alignment, best fit, coalescing, grow causes, cap, stale release, warm trim, reclaim)");
}

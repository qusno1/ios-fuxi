/* Real Vulkan tickets hold hazards live independently of GPU execution speed. */
#include "Backend.hpp"
#include "CommandStorage.hpp"
#include <cassert>
#include <future>
using namespace fuxi;
int main(){
 Backend b;ShadowPool pool;UploadStorage<Buffer *> uploads;UploadRing<Buffer *> ring;
 uint16_t triangle[]={0,1,2};auto slice=ring.copy(triangle,sizeof(triangle),uploads,[&](size_t n){auto *p=b.allocate(n);p->upload=true;return p;},[](Buffer *p){return p->contents;});
 auto *work=b.allocate(4096),*indices=b.allocate(4096),*source=b.allocate(65536);auto *target=b.allocateTexture(80,5,16,16),*sample=b.linearTexture(source,1,1,16,16,0,64);auto *sampler=b.allocateSampler(false,false);
 memset(source->contents,19,source->length);memcpy(indices->contents,triangle,sizeof(triangle));CompileOptions options;auto *pipeline=b.compilePipeline(nullptr,"control",nullptr,"control",options);
 Operation draw{};draw.pipeline=pipeline;draw.texture=target;draw.samples[0]=sample;draw.samplers[0]=sampler;draw.indices=slice.handle;draw.indexOffset=slice.offset;draw.viewport={0,16,16,-16,0,1};draw.scissor={{0,0},{16,16}};
 auto run=[&](const Operation &previous,Operation next,bool wait,bool prepare,ShadowSnapshot *snapshot,FrameSample &trace,const std::vector<PrepareWrite> *declared=nullptr){
  std::function<void()> x,y;b.submit({previous},{},&x);std::promise<void> started;std::atomic<bool> prepared{false};
  auto future=std::async(std::launch::async,[&]{FrameTraceScope scope(&trace);started.set_value();std::function<void()> refresh;if(prepare || snapshot)refresh=[&]{prepared=true;if(snapshot){auto start=FrameClock::now();trace.shadowCopyBytes+=snapshot->apply(pool);trace.shadowCopy+=frameNanos(start);}};b.submit({next},{},&y,refresh,declared);});
  started.get_future().wait();assert(future.wait_for(std::chrono::milliseconds(wait?50:5000))==(wait?std::future_status::timeout:std::future_status::ready));
  if(wait && (prepare || snapshot))assert(!prepared);x();future.get();assert(bool(y));y();if(prepare || snapshot)assert(prepared);
 };
 Operation fill{};fill.dst=work;fill.size=4096;fill.value=7;
 FrameSample upload;run(fill,draw,false,false,nullptr,upload);assert(!upload.drainSubmits && !upload.drainWaits && upload.indexUploadSubmits==1);
 draw.indices=indices;FrameSample readonly;run(draw,draw,false,false,nullptr,readonly);assert(!readonly.drainSubmits && readonly.indexReadOnlySubmits==1);
 fill.dst=indices;fill.value=0;FrameSample written;run(fill,draw,true,false,nullptr,written);assert(written.drainWaits==1 && written.indexGPUWritableDrains==1 && written.indexGPUWritableSubmits==1);
 fill.dst=work;FrameSample prepare;run(fill,draw,true,true,nullptr,prepare);assert(prepare.prepareDrains==1 && prepare.prepareUnknownDrains==1 && !prepare.prepareConflictWaits && prepare.drainSubmits==1 && prepare.drainWaits==1);
 // A declared prepare waits only for flights using the memory it writes (including coherent allocations).
 std::vector<PrepareWrite> toSource{{source,0,64}},toWork{{work,0,64}},none;
 fill.dst=work;FrameSample disjoint;run(fill,draw,false,true,nullptr,disjoint,&toSource);assert(disjoint.prepareDrains==1 && !disjoint.prepareUnknownDrains && !disjoint.prepareConflictWaits && !disjoint.drainSubmits && !disjoint.drainWaits && !disjoint.conflictWaits);
 FrameSample empty;run(fill,draw,false,true,nullptr,empty,&none);assert(!empty.prepareConflictWaits && !empty.drainWaits);
 FrameSample writerFlight;run(fill,draw,true,true,nullptr,writerFlight,&toWork);assert(writerFlight.prepareConflictWaits==1 && !writerFlight.prepareUnknownDrains && !writerFlight.drainSubmits && !writerFlight.drainWaits && writerFlight.conflictWaits==1);
 FrameSample readerFlight;run(draw,draw,true,true,nullptr,readerFlight,&toSource);assert(readerFlight.prepareConflictWaits==1 && !readerFlight.drainSubmits && readerFlight.conflictWaits==1);
 fill.dst=source;FrameSample gpuWritten;run(fill,draw,true,true,nullptr,gpuWritten,&toSource);assert(gpuWritten.prepareConflictWaits==1 && !gpuWritten.drainSubmits);fill.dst=work;
 // A mapped linear attachment aliases an index allocation, without op.dst.
 auto *alias=b.linearTexture(indices,80,5,16,16,0,64);assert(alias->borrowedMemory);Operation clear{};clear.texture=alias;clear.clearOnly=true;clear.clear[3]=1;
 FrameSample attachment;run(clear,draw,true,false,nullptr,attachment);assert(attachment.indexGPUWritableDrains==1);b.destroy(alias);
 auto *writer=b.compilePipeline(nullptr,"control",nullptr,"writer",options);Operation storage=draw;storage.pipeline=writer;storage.indices=nullptr;storage.buffers[1][1]={indices,0,false,0};
 FrameSample shader;run(storage,draw,true,false,nullptr,shader);assert(shader.indexGPUWritableDrains==1);uint16_t produced[3];memcpy(produced,indices->contents,sizeof(produced));assert(produced[0]==0 && produced[1]==1 && produced[2]==2);b.destroy(writer);
 std::vector<uint8_t> cpu(source->length,19);ShadowRanges ranges(source->length);ranges.texture(0,64,16,16,1);ShadowSnapshot snapshot;
 auto capture=[&]{snapshot.add(source->contents,cpu.data(),ranges);snapshot.capture(pool);};
 // Restored opaque refresh always drains, even if the bytes are unchanged
 // or the previous flight uses an unrelated allocation. No comparisons/skips.
 auto copied=[&](const FrameSample &trace){assert(trace.prepareDrains==1 && trace.drainSubmits==1 && trace.drainWaits==1 && !trace.shadowWaits && trace.shadowCopyBytes==ranges.bytes() && !trace.shadowSkippedBytes);};
 capture();FrameSample same;run(draw,draw,true,false,&snapshot,same);copied(same);
 cpu[65]=29;capture();FrameSample changed;run(draw,draw,true,false,&snapshot,changed);copied(changed);assert(((uint8_t *)source->contents)[65]==29);
 capture();fill.dst=source;FrameSample gpuWriter;run(fill,draw,true,false,&snapshot,gpuWriter);copied(gpuWriter);assert(((uint8_t *)source->contents)[65]==29);
 cpu[66]=39;capture();fill.dst=work;FrameSample unrelated;run(fill,draw,true,false,&snapshot,unrelated);copied(unrelated);assert(((uint8_t *)source->contents)[66]==39);
 ring.release(uploads,false,[&](Buffer *p){b.destroy(p);});uploads.clear([&](Buffer *p){b.destroy(p);});b.destroy(pipeline);b.dropPipelineCacheForTest();b.destroy(sampler);b.destroy(sample);b.destroy(target);b.destroy(source);b.destroy(indices);b.destroy(work);
 puts("PASS: upload/readonly indexed draws overlap, GPU-written/attachment-alias indices and undeclared prepare drains, declared prepare waits only on conflicting memory (disjoint overlaps), packed shadows copy (unchanged/dirty/GPU-written/unrelated); two slots");
}

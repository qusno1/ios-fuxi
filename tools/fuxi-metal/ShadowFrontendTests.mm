/* Native selector coverage with paused submit; CPU model, no shader translation. */
#define FUXI_MTL_HOST_TEST
#define FUXI_MTL_FRONTEND_TEST
#define FUXI_MTL_BACKEND_HEADER "TestBackend.hpp"
#include "Frontend.mm"
#include <cassert>
@interface FuxiShadowBase : NSObject {uint8_t bytes[520];}
@end
@implementation FuxiShadowBase
@end
int main(){@autoreleasepool{
 assert(fuxi::systemTranslationPolicy().overlap);fuxi::Backend::modelTextures=true;
 Class cls=makeDeviceClass([FuxiShadowBase class]);id device=[cls alloc];FuxiOwner *state=[FuxiOwner new];state->backend=new fuxi::Backend;
 objc_setAssociatedObject(device,&ownerKey,state,OBJC_ASSOCIATION_RETAIN_NONATOMIC);[state release];
 FuxiQueue *queue=(FuxiQueue *)[device newCommandQueue];uint8_t cpu[4096];memset(cpu,19,sizeof(cpu));
 FuxiBuffer *buffer=(FuxiBuffer *)[device newBufferWithBytesNoCopy:cpu length:sizeof(cpu) options:0 deallocator:nil];assert(buffer->_shadow);
 MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:16 height:16 mipmapped:NO];td.usage=MTLTextureUsageRenderTarget;td.storageMode=MTLStorageModePrivate;
 id target=[device newTextureWithDescriptor:td];MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=target;pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
 FuxiPipeline *pipeline=[FuxiPipeline new];pipeline->_device=[device retain];pipeline->_pipeline=new fuxi::Pipeline{};
 auto *p=pipeline->_pipeline;for(auto &stage:p->shadowWindows)for(auto &window:stage)window.kind=2;
 p->options.strides[0]=32;p->options.attributes[0]={31,0,0};p->options.attributes[1]={31,16,0};p->shadowWindows[1][1]={1,0,0,64};
 unsigned seen=0;fuxi::Backend::modelDraw=[&](const fuxi::Operation &op){auto v=op.buffers[0][0],f=op.buffers[1][1];assert(v.buffer==buffer->_buffer && f.buffer==buffer->_buffer);
  for(unsigned i=0;i<192;++i)assert(((uint8_t *)v.buffer->contents)[v.offset+i]==19);for(unsigned i=0;i<64;++i)assert(((uint8_t *)f.buffer->contents)[f.offset+i]==19);++seen;};
 for(unsigned mode=0;mode<7;++mode){
  memset(cpu,19,sizeof(cpu));memset(buffer->_buffer->contents,3,sizeof(cpu));FuxiCommandBuffer *c=[(FuxiCommandBuffer *)[queue commandBuffer] retain];c->_frameTrace=YES;c->_traceCreated=fuxi::FrameClock::now();c->_traceThread=std::this_thread::get_id();
  FuxiRender *e=(FuxiRender *)[c renderCommandEncoderWithDescriptor:pass];[e setRenderPipelineState:(id)pipeline];
  id<MTLBuffer> buffers[]={(id)buffer};NSUInteger offsets[]={0};[e setVertexBuffers:buffers offsets:offsets withRange:NSMakeRange(0,1)];[e setVertexBufferOffset:256 atIndex:0];
  [e setFragmentBuffers:buffers offsets:offsets withRange:NSMakeRange(1,1)];[e setFragmentBufferOffset:448 atIndex:1];uint8_t inlineBytes[16]={};[e setVertexBytes:inlineBytes length:16 atIndex:3];
  if(mode==1)p->shadowWindows[1][1]={}; // Unknown shader/argument access is sticky for this command.
  if(mode!=6)[e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
  if(mode==2){[e setVertexBufferOffset:1024 atIndex:0];[e setFragmentBufferOffset:1216 atIndex:1];[e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];}
  if(mode==3)[e useResource:(id)buffer usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
  if(mode==4){uint16_t indices[]={0,1,2};id index=[device newBufferWithBytes:indices length:sizeof(indices) options:0];[e drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:3 indexType:MTLIndexTypeUInt16 indexBuffer:index indexBufferOffset:0];[index release];}
  [e endEncoding];if(mode==5){id output=[device newBufferWithLength:16 options:0];id<MTLBlitCommandEncoder> blit=[c blitCommandEncoder];[blit copyFromBuffer:(id)buffer sourceOffset:3000 toBuffer:output destinationOffset:0 size:16];[blit endEncoding];[output release];}
  assert(!c->_rejected);size_t expected=mode==6?0:(mode==1 || mode>=3)?4096:mode==2?512:256;
  dispatch_suspend(queue->_submitQueue);[c commit];assert(c->_shadows.bytes()==expected && c->_flightTrace.snapshotBytes==expected);assert(c->_flightTrace.shadowFullReasons[size_t(fuxi::ShadowFullReason::wholeWindow)]==(mode==1));assert(c->_flightTrace.shadowFullReasons[size_t(fuxi::ShadowFullReason::useResource)]==(mode==3));assert(c->_flightTrace.shadowFullReasons[size_t(fuxi::ShadowFullReason::indexed)]==(mode==4));assert(c->_flightTrace.shadowFullReasons[size_t(fuxi::ShadowFullReason::blit)]==(mode==5));memset(cpu,77,sizeof(cpu));dispatch_resume(queue->_submitQueue);[c waitUntilCompleted];
  assert(c.status==MTLCommandBufferStatusCompleted && c->_flightTrace.shadowCopyBytes==expected);assert(((uint8_t *)buffer->_buffer->contents)[3000]==(expected==4096?19:3));[c release];p->shadowWindows[1][1]={1,0,0,64};
 }
 assert(seen==8);
 for(unsigned width:{2u})for(unsigned mode=0;mode<8;++mode){ // UInt32 draws are refused by the frontend.
  uint8_t indexCPU[128]={};uint32_t quad[]={0,1,2,2,1,3},sentinel=width==2?UINT16_MAX:UINT32_MAX;if(mode==1 || mode==2 || mode==7)quad[3]=sentinel;if(mode==3)quad[3]=2000;if(mode==5)for(auto &i:quad)i=sentinel;
  for(unsigned i=0;i<6;++i)memcpy(indexCPU+16+i*width,&quad[i],width);
  if(mode==0){uint32_t bad=2000;memcpy(indexCPU+16,&bad,width);}
  FuxiBuffer *index=(FuxiBuffer *)(mode==4?[device newBufferWithBytes:indexCPU length:sizeof(indexCPU) options:MTLResourceStorageModeShared]:[device newBufferWithBytesNoCopy:indexCPU length:sizeof(indexCPU) options:MTLResourceStorageModeShared deallocator:nil]);assert(index->_shadow==(mode!=4));
  memset(cpu,19,sizeof(cpu));memset(buffer->_buffer->contents,3,sizeof(cpu));if(mode==6)memcpy(cpu+3072,indexCPU+16,6*width);
  FuxiCommandBuffer *c=[(FuxiCommandBuffer *)[queue commandBuffer] retain];c->_frameTrace=YES;c->_traceCreated=fuxi::FrameClock::now();c->_traceThread=std::this_thread::get_id();
  FuxiRender *e=(FuxiRender *)[c renderCommandEncoderWithDescriptor:pass];[e setRenderPipelineState:(id)pipeline];[e setVertexBuffer:(id)buffer offset:256 atIndex:0];[e setFragmentBuffer:(id)buffer offset:384 atIndex:1];
  auto topology=mode==7?MTLPrimitiveTypeLineStrip:mode==1 || mode==5?MTLPrimitiveTypeTriangleStrip:MTLPrimitiveTypeTriangle;id ib=mode==6?(id)buffer:(id)index;NSUInteger offset=mode==6?3072:16;
  [e drawIndexedPrimitives:topology indexCount:6 indexType:width==2?MTLIndexTypeUInt16:MTLIndexTypeUInt32 indexBuffer:ib indexBufferOffset:offset];
  [e drawIndexedPrimitives:topology indexCount:6 indexType:width==2?MTLIndexTypeUInt16:MTLIndexTypeUInt32 indexBuffer:ib indexBufferOffset:offset];[e endEncoding];assert(!c->_rejected);
  // Encoding-time values cannot supply the bound; commit must inspect these bytes.
  if(mode==0)memcpy(indexCPU+16,&quad[0],width);
  size_t vertexBytes=mode==5?64:mode>=2 && mode<=4?sizeof(cpu):192;size_t expected=vertexBytes+(mode==4?0:6*width);
  unsigned draws=0;fuxi::Backend::modelDraw=[&](const fuxi::Operation &op){assert(op.indexWidth==width);for(unsigned i=0;i<6;++i){uint32_t value=0;memcpy(&value,(uint8_t *)op.indices->contents+op.indexOffset+i*width,width);assert(value==quad[i]);}
   if(mode!=5)for(unsigned i=0;i<128;++i)assert(((uint8_t *)buffer->_buffer->contents)[256+i]==19);for(unsigned i=0;i<64;++i)assert(((uint8_t *)buffer->_buffer->contents)[384+i]==19);++draws;};
  dispatch_suspend(queue->_submitQueue);[c commit];assert(c->_shadows.bytes()==expected && c->_flightTrace.snapshotBytes==expected);assert(c->_flightTrace.shadowScannedIndices==(mode==4?0:6));
  assert(c->_flightTrace.shadowFullReasons[size_t(fuxi::ShadowFullReason::bounds)]==(mode==2 || mode==3));assert(c->_flightTrace.shadowFullReasons[size_t(fuxi::ShadowFullReason::indexed)]==(mode==4));
  memset(cpu,77,sizeof(cpu));if(mode!=4)memset(indexCPU,255,sizeof(indexCPU));dispatch_resume(queue->_submitQueue);[c waitUntilCompleted];assert(c.status==MTLCommandBufferStatusCompleted && draws==2 && c->_flightTrace.shadowCopyBytes==expected);
  [c release];[index release];
 }
 fuxi::Backend::modelDraw={};
 MTLTextureDescriptor *linear=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG8Unorm width:7 height:5 mipmapped:NO];linear.storageMode=MTLStorageModeShared;linear.usage=MTLTextureUsageShaderRead;
 FuxiTexture *texture=(FuxiTexture *)[buffer newTextureWithDescriptor:linear offset:256 bytesPerRow:32],*view=(FuxiTexture *)[texture newTextureViewWithPixelFormat:linear.pixelFormat];assert(texture && view);
 FuxiSampler *sampler=(FuxiSampler *)[device newSamplerStateWithDescriptor:[[[MTLSamplerDescriptor alloc] init] autorelease]];
 for(unsigned mode=0;mode<5;++mode){
  memset(cpu,19,sizeof(cpu));memset(buffer->_buffer->contents,3,sizeof(cpu));uint8_t replacement[14];memset(replacement,29,sizeof(replacement));
  [view replaceRegion:MTLRegionMake2D(0,0,7,1) mipmapLevel:0 withBytes:replacement bytesPerRow:14];assert(cpu[256]==29 && ((uint8_t *)buffer->_buffer->contents)[256]==3);
  uint8_t output[14]={};[view getBytes:output bytesPerRow:14 fromRegion:MTLRegionMake2D(0,0,7,1) mipmapLevel:0];assert(!memcmp(output,replacement,14));
  FuxiCommandBuffer *c=[(FuxiCommandBuffer *)[queue commandBuffer] retain];c->_frameTrace=YES;c->_traceCreated=fuxi::FrameClock::now();c->_traceThread=std::this_thread::get_id();
  FuxiRender *e=(FuxiRender *)[c renderCommandEncoderWithDescriptor:pass];[e setRenderPipelineState:(id)pipeline];[e setFragmentTexture:(id)texture atIndex:0];[e setFragmentTexture:(id)view atIndex:1];[e setFragmentSamplerState:(id)sampler atIndex:0];
  if(mode!=4){[e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];}
  if(mode==1)[e useResource:(id)texture usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
  if(mode==2)[e useResource:(id)buffer usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
  [e endEncoding];if(mode==3)retainCommandTexture(c,texture,false);
  size_t expected=mode==4?0:mode?sizeof(cpu):142;assert(!c->_rejected);
  unsigned draws=0;fuxi::Backend::modelDraw=[&](const fuxi::Operation &op){auto *b=op.samples[0]->backingBuffer;assert(b==buffer->_buffer && op.samples[1]==texture->_texture);assert(((uint8_t *)b->contents)[256]==29 && ((uint8_t *)b->contents)[288]==19);++draws;};
  dispatch_suspend(queue->_submitQueue);[c commit];assert(c->_flightTrace.snapshotBytes==expected && c->_flightTrace.textureShadowBytes==expected);assert(c->_flightTrace.shadowFullReasons[size_t(fuxi::ShadowFullReason::other)]==(mode==3));assert(c->_flightTrace.textureShadowUses[0]==(mode!=4) && c->_flightTrace.textureShadowUses[1]==(mode==3) && c->_flightTrace.textureShadowUses[2]==(mode==1));
  memset(cpu,77,sizeof(cpu));dispatch_resume(queue->_submitQueue);[c waitUntilCompleted];assert(c.status==MTLCommandBufferStatusCompleted && c->_flightTrace.shadowCopyBytes==expected && draws==(mode==4?0:2));
  assert(((uint8_t *)buffer->_buffer->contents)[2000]==(mode && mode!=4?19:3));[c release];
 }
 memset(cpu,19,sizeof(cpu));uint8_t row[14];memset(row,29,sizeof(row));[view replaceRegion:MTLRegionMake2D(0,0,7,1) mipmapLevel:0 withBytes:row bytesPerRow:14];
 FuxiCommandBuffer *versions[3]{};unsigned versionDraws=0;
 fuxi::Backend::modelDraw=[&](const fuxi::Operation &op){auto *bytes=(uint8_t *)op.samples[0]->backingBuffer->contents;assert(bytes[256]==29 && bytes[288]==(versionDraws?39:19));++versionDraws;};
 dispatch_suspend(queue->_submitQueue);
 for(unsigned i=0;i<3;++i){if(i==2){dispatch_resume(queue->_submitQueue);[versions[0] waitUntilCompleted];[versions[1] waitUntilCompleted];dispatch_suspend(queue->_submitQueue);}if(i==1)cpu[288]=39;auto *c=versions[i]=[(FuxiCommandBuffer *)[queue commandBuffer] retain];c->_frameTrace=YES;
  FuxiRender *e=(FuxiRender *)[c renderCommandEncoderWithDescriptor:pass];[e setRenderPipelineState:(id)pipeline];[e setFragmentTexture:(id)view atIndex:0];[e setFragmentSamplerState:(id)sampler atIndex:0];[e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[e endEncoding];[c commit];
  assert(c->_flightTrace.snapshotBytes==142 && !c->_flightTrace.snapshotReusedBytes);
 }
 memset(cpu,77,sizeof(cpu));dispatch_resume(queue->_submitQueue);
 for(unsigned i=0;i<3;++i){auto *c=versions[i];[c waitUntilCompleted];assert(c.status==MTLCommandBufferStatusCompleted && c->_flightTrace.shadowCopyBytes==142 && !c->_flightTrace.shadowSkippedBytes);[c release];}assert(versionDraws==3);
 [sampler release];[view release];[texture release];
 fuxi::Backend::modelDraw={};[pipeline release];[target release];[buffer release];[queue release];[device release];
 puts("PASS: shadow bindings/offsets, frozen indexed u16 quads, strips/restarts, bounds and mutable-index fallback, aliased indices, scan cache and reason/byte trace");
}}

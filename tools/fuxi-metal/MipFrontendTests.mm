/* Native Mac ownership/descriptor/encoder tests; CPU model, GPU tested by M9k. */
#define FUXI_MTL_HOST_TEST
#define FUXI_MTL_FRONTEND_TEST
#define FUXI_MTL_BACKEND_HEADER "TestBackend.hpp"
#include "Frontend.mm"
#include "IOS16Defaults.hpp"
#include <cassert>
@interface FuxiMipTestBase : NSObject {uint8_t bytes[520];}
@end
@implementation FuxiMipTestBase
@end
int main(){@autoreleasepool {
 Class cls=makeDeviceClass([FuxiMipTestBase class]);id device=[cls alloc];FuxiOwner *state=[FuxiOwner new];state->backend=new fuxi::Backend;
 objc_setAssociatedObject(device,&ownerKey,state,OBJC_ASSOCIATION_RETAIN_NONATOMIC);[state release];fuxi::Backend::modelTextures=true;
 FuxiQueue *q=(FuxiQueue *)[device newCommandQueue];
 MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:13 height:7 mipmapped:YES];
 d.storageMode=MTLStorageModeShared;d.cpuCacheMode=MTLCPUCacheModeWriteCombined;d.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget|MTLTextureUsagePixelFormatView;
 FuxiTexture *t=(FuxiTexture *)[device newTextureWithDescriptor:d];assert(t && t.mipmapLevelCount==4 && t.cpuCacheMode==MTLCPUCacheModeWriteCombined);
 uint8_t input[512],output[512];
 for(unsigned l=0;l<4;++l){unsigned w=std::max(1u,13u>>l),h=std::max(1u,7u>>l),row=w*4+8;for(unsigned i=0;i<sizeof(input);++i)input[i]=(uint8_t)(i+43*l);memset(output,0xa5,sizeof(output));
  [t replaceRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:l withBytes:input bytesPerRow:row];
  [t getBytes:output bytesPerRow:row fromRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:l];
  for(unsigned y=0;y<h;++y){assert(!memcmp(output+y*row,input+y*row,w*4));for(unsigned x=w*4;x<row;++x)assert(output[y*row+x]==0xa5);}
 }
 auto errors=failureCount.load();
 FuxiTexture *view=(FuxiTexture *)[t newTextureViewWithPixelFormat:t.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(1,3) slices:NSMakeRange(0,1)];
 FuxiTexture *nested=(FuxiTexture *)[view newTextureViewWithPixelFormat:t.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(1,1) slices:NSMakeRange(0,1)];
 assert(view && view.width==6 && view.height==3 && view.mipmapLevelCount==3 && view.parentRelativeLevel==1);
 assert(nested.width==3 && nested.height==1 && nested.parentRelativeLevel==1 && nested->_baseLevel==2);
 uint8_t patch[4]={7,31,97,255};[nested replaceRegion:MTLRegionMake2D(1,0,1,1) mipmapLevel:0 withBytes:patch bytesPerRow:4];
 [t getBytes:output bytesPerRow:12 fromRegion:MTLRegionMake2D(0,0,3,1) mipmapLevel:2];assert(!memcmp(output+4,patch,4));
 FuxiCommandBuffer *c=(FuxiCommandBuffer *)[q commandBuffer];FuxiBlit *blit=(FuxiBlit *)[c blitCommandEncoder];[blit generateMipmapsForTexture:(id)view];
 assert(c->_ops.size()==1 && c->_ops[0].generateMips && c->_ops[0].mipBase==1 && c->_ops[0].mipCount==3);[blit endEncoding]; // Model deliberately has no GPU filtering.
 d.cpuCacheMode=MTLCPUCacheModeDefaultCache;FuxiTexture *copy=(FuxiTexture *)[device newTextureWithDescriptor:d];
 c=(FuxiCommandBuffer *)[q commandBuffer];blit=(FuxiBlit *)[c blitCommandEncoder];[blit copyFromTexture:(id)t toTexture:(id)copy];[blit endEncoding];assert(c->_ops.size()==4 && c->_ops[3].srcLevel==3 && c->_ops[3].dstLevel==3);[c commit];assert(!c.error);
 [copy getBytes:output bytesPerRow:12 fromRegion:MTLRegionMake2D(0,0,3,1) mipmapLevel:2];assert(!memcmp(output+4,patch,4));
 // All three blit directions translate view-relative levels and retain owners.
 FuxiBuffer *buffer=(FuxiBuffer *)[device newBufferWithLength:64 options:0];memcpy(buffer.contents,patch,4);
 c=(FuxiCommandBuffer *)[q commandBuffer];blit=(FuxiBlit *)[c blitCommandEncoder];
 [blit copyFromBuffer:buffer sourceOffset:0 sourceBytesPerRow:4 sourceBytesPerImage:0 sourceSize:MTLSizeMake(1,1,1) toTexture:nested destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
 [blit copyFromTexture:nested sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(1,1,1) toBuffer:buffer destinationOffset:16 destinationBytesPerRow:4 destinationBytesPerImage:0];
 assert(c->_ops[0].dstLevel==2 && c->_ops[1].srcLevel==2);[blit endEncoding];[c commit];assert(!c.error && !memcmp((uint8_t *)buffer.contents+16,patch,4));
 // A level-one attachment on a rebased view targets absolute level two.
 MTLRenderPassDescriptor *pass=ios16PassDefaults();pass.colorAttachments[0].texture=(id)view;pass.colorAttachments[0].level=1;
 pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;pass.colorAttachments[0].clearColor=MTLClearColorMake(1,0,0,1);
 c=(FuxiCommandBuffer *)[q commandBuffer];FuxiRender *r=(FuxiRender *)[c renderCommandEncoderWithDescriptor:pass];assert(r && r->_state.dstLevel==2 && r->_state.areaWidth==3 && r->_state.areaHeight==1);[r endEncoding];[c commit];assert(!c.error);
 [t getBytes:output bytesPerRow:12 fromRegion:MTLRegionMake2D(0,0,3,1) mipmapLevel:2];for(unsigned x=0;x<3;++x){uint8_t red[]={0,0,255,255};assert(!memcmp(output+4*x,red,4));}
 // The supplied Spotlight shape binds its full mip view before draw-time
 // access analysis. Object identity alone must not fail this encoder.
 auto blurDescriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:704 height:96 mipmapped:YES];blurDescriptor.storageMode=MTLStorageModePrivate;blurDescriptor.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
 FuxiTexture *blur=(FuxiTexture *)[device newTextureWithDescriptor:blurDescriptor];assert(blur.mipmapLevelCount==10);
 FuxiPipeline *modelPipeline=[FuxiPipeline new];modelPipeline->_device=[device retain];modelPipeline->_pipeline=new fuxi::Pipeline{};
 auto blurPass=ios16PassDefaults();blurPass.colorAttachments[0].texture=(id)blur;blurPass.colorAttachments[0].level=1;blurPass.colorAttachments[0].loadAction=MTLLoadActionClear;blurPass.colorAttachments[0].storeAction=MTLStoreActionStore;
 c=(FuxiCommandBuffer *)[q commandBuffer];r=(FuxiRender *)[c renderCommandEncoderWithDescriptor:blurPass];assert(r && r->_state.areaWidth==352 && r->_state.areaHeight==48);
 [r setFragmentTexture:blur atIndex:0];assert(!c->_rejected);[r setRenderPipelineState:modelPipeline];[r drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[r endEncoding];
 assert(!c->_rejected && c->_ops.size()==1 && c->_ops[0].dstLevel==1 && c->_ops[0].samples[0]==blur->_texture && c->_ops[0].sampleBase[0]==0 && c->_ops[0].sampleCount[0]==10);[blur release];
 // Resource classes: public query values, private heap budgets, volatile/empty.
 for(MTLPixelFormat format:{MTLPixelFormatBGRA8Unorm,MTLPixelFormatRGBA8Unorm,MTLPixelFormatRGBA16Float,MTLPixelFormatR8Unorm,MTLPixelFormatRG8Unorm,MTLPixelFormatA8Unorm,MTLPixelFormatBGR10A2Unorm}){
  d.pixelFormat=format;d.usage=format==MTLPixelFormatA8Unorm?MTLTextureUsageShaderRead:MTLTextureUsageUnknown;d.storageMode=MTLStorageModePrivate;
  FuxiTexture *resource=(FuxiTexture *)[device newTextureWithDescriptor:d];assert(resource && resource.usage==d.usage && resource.storageMode==MTLStorageModePrivate && resource.mipmapLevelCount==4);[resource release];
 }
 d.pixelFormat=MTLPixelFormatBGRA8Unorm;d.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
 MTLHeapDescriptor *hd=[MTLHeapDescriptor new];hd.storageMode=MTLStorageModePrivate;hd.size=4096;
 FuxiHeap *heap=(FuxiHeap *)[device newHeapWithDescriptor:hd];FuxiTexture *ht=(FuxiTexture *)[heap newTextureWithDescriptor:d];
 id<MTLBuffer> hb=[heap newBufferWithLength:64 options:MTLResourceStorageModePrivate];assert(heap && ht && hb && !hb.contents && hb.storageMode==MTLStorageModePrivate);
 assert(ht.heap==heap && heap.usedSize==ht.allocatedSize+hb.allocatedSize);[ht release];[hb release];assert(!heap.usedSize);[heap release];[hd release];
 for(MTLSamplerMipFilter filter:{MTLSamplerMipFilterNotMipmapped,MTLSamplerMipFilterNearest,MTLSamplerMipFilterLinear}){MTLSamplerDescriptor *sd=[MTLSamplerDescriptor new];sd.mipFilter=filter;sd.lodMinClamp=1;sd.lodMaxClamp=2.5f;id sampler=[device newSamplerStateWithDescriptor:sd];assert(sampler);[sampler release];[sd release];}
 assert(failureCount==errors);
 [t setPurgeableState:MTLPurgeableStateEmpty];assert(!t->_texture->isDefined(0) && !t->_texture->isDefined(2));
 // Refusals use valid SDK values; do not provoke SDK descriptor-array assertions.
 d.storageMode=MTLStorageModeMemoryless;{FuxiTexture *m=(FuxiTexture *)[device newTextureWithDescriptor:d];assert(m && m.storageMode==MTLStorageModeMemoryless && m.mipmapLevelCount==d.mipmapLevelCount);[m release];}d.storageMode=MTLStorageModeShared;
 d.textureType=MTLTextureType2DArray;d.arrayLength=2;assert(![device newTextureWithDescriptor:d]);d.textureType=MTLTextureType2D;d.arrayLength=1;
 d.pixelFormat=MTLPixelFormatBGRA8Unorm_sRGB;assert(![device newTextureWithDescriptor:d]);d.pixelFormat=MTLPixelFormatDepth32Float;assert(![device newTextureWithDescriptor:d]);d.pixelFormat=MTLPixelFormatBGRA8Unorm;
 d.swizzle=MTLTextureSwizzleChannelsMake(MTLTextureSwizzleBlue,MTLTextureSwizzleGreen,MTLTextureSwizzleRed,MTLTextureSwizzleAlpha);assert(![device newTextureWithDescriptor:d]);d.swizzle=MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed,MTLTextureSwizzleGreen,MTLTextureSwizzleBlue,MTLTextureSwizzleAlpha);
 d.usage=MTLTextureUsageShaderWrite;assert(![device newTextureWithDescriptor:d]);d.usage=MTLTextureUsageShaderRead;
 // Nonplanar plane 1 is invalid; one explicit BGRA plane uses its own offset
 // and row stride while the entire surface allocation crosses HostImport.
 d.width=8;d.height=8;d.mipmapLevelCount=1;
 fuxi::Backend::modelHostImport=true;
 d.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
 IOSurfaceRef surface=IOSurfaceCreate((CFDictionaryRef)@{(id)kIOSurfaceWidth:@8,(id)kIOSurfaceHeight:@8,(id)kIOSurfaceBytesPerElement:@4,(id)kIOSurfacePixelFormat:@0x42475241});
 assert(surface && ![device newTextureWithDescriptor:d iosurface:surface plane:1]);
 FuxiTexture *st=(FuxiTexture *)[device newTextureWithDescriptor:d iosurface:surface plane:0];
 assert(st && fuxi::Backend::importOffset==0 && fuxi::Backend::importRow==IOSurfaceGetBytesPerRow(surface));
 FuxiTexture *surfaceAlias=(FuxiTexture *)[device newTextureWithDescriptor:d iosurface:surface plane:0];assert(surfaceAlias && surfaceAlias!=st);
 auto surfacePass=ios16PassDefaults();surfacePass.colorAttachments[0].texture=(id)st;surfacePass.colorAttachments[0].loadAction=MTLLoadActionClear;surfacePass.colorAttachments[0].storeAction=MTLStoreActionStore;
 c=(FuxiCommandBuffer *)[q commandBuffer];r=(FuxiRender *)[c renderCommandEncoderWithDescriptor:surfacePass];[r setRenderPipelineState:modelPipeline];[r setFragmentTexture:surfaceAlias atIndex:0];assert(!c->_rejected);
 auto beforeSkip=fuxi::skippedSamplingDraws.load();[r drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
 assert(bool(c->_rejected)==!fuxi::systemTranslationPolicy().skipSampling);
 if(fuxi::systemTranslationPolicy().skipSampling){assert(c->_ops.empty() && fuxi::skippedSamplingDraws==beforeSkip+1);[r endEncoding];assert(c->_ops.size()==1 && c->_ops[0].clearOnly);}
 [surfaceAlias release];[st release];CFRelease(surface);
 surface=IOSurfaceCreate((CFDictionaryRef)@{(id)kIOSurfaceWidth:@8,(id)kIOSurfaceHeight:@8,(id)kIOSurfacePixelFormat:@0x42475241,(id)kIOSurfaceAllocSize:@16384,
  (id)kIOSurfacePlaneInfo:@[@{(id)kIOSurfacePlaneWidth:@8,(id)kIOSurfacePlaneHeight:@8,(id)kIOSurfacePlaneBytesPerElement:@4,
   (id)kIOSurfacePlaneBytesPerRow:@128,(id)kIOSurfacePlaneOffset:@4096,(id)kIOSurfacePlaneSize:@1024}]});
 assert(surface && IOSurfaceGetPlaneCount(surface)==1);
 st=(FuxiTexture *)[device newTextureWithDescriptor:d iosurface:surface plane:0];
 assert(st && st.iosurface==surface && st.iosurfacePlane==0);
 assert(fuxi::Backend::importBase==IOSurfaceGetBaseAddress(surface) && fuxi::Backend::importBytes==IOSurfaceGetAllocSize(surface));
 assert(fuxi::Backend::importOffset==4096 && fuxi::Backend::importRow==128);[st release];
 assert(![device newTextureWithDescriptor:d iosurface:surface plane:1]);CFRelease(surface);
 surface=IOSurfaceCreate((CFDictionaryRef)@{(id)kIOSurfaceWidth:@8,(id)kIOSurfaceHeight:@8,(id)kIOSurfacePixelFormat:@0x5f524234,(id)kIOSurfaceAllocSize:@16384,
  (id)kIOSurfacePlaneInfo:@[@{(id)kIOSurfacePlaneWidth:@8,(id)kIOSurfacePlaneHeight:@8,(id)kIOSurfacePlaneBytesPerElement:@1,(id)kIOSurfacePlaneBytesPerRow:@64,(id)kIOSurfacePlaneOffset:@0,(id)kIOSurfacePlaneSize:@512},
   @{(id)kIOSurfacePlaneWidth:@8,(id)kIOSurfacePlaneHeight:@8,(id)kIOSurfacePlaneBytesPerElement:@2,(id)kIOSurfacePlaneBytesPerRow:@64,(id)kIOSurfacePlaneOffset:@4096,(id)kIOSurfacePlaneSize:@512}]});
 assert(surface && IOSurfaceGetPlaneCount(surface)==2);d.pixelFormat=(MTLPixelFormat)542;
 assert(![device newTextureWithDescriptor:d iosurface:surface plane:0]);
 assert(![device newTextureWithDescriptor:d iosurface:surface plane:1]);CFRelease(surface);
 [modelPipeline release];[buffer release];[copy release];[nested release];[t release];assert(view.width==6);[view release];[q release];[device release];
 puts("PASS: native M9k descriptor/level transfers/views/blits/generation encoding/render attachment/private heap/purge/sampler/refusal controls");
}}

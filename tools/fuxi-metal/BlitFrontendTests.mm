/* Native Metal selectors/validation/ownership, with the portable CPU model. */
#define FUXI_MTL_FRONTEND_TEST
#define FUXI_MTL_BACKEND_HEADER "TestBackend.hpp"
#include "Frontend.mm"
#include <cassert>
/* The tests call deprecated sync selectors and pass nil on purpose to check refusal. */
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#pragma clang diagnostic ignored "-Wnonnull"
@interface FuxiBlitTestBase : NSObject {uint8_t bytes[520];}
@end
@implementation FuxiBlitTestBase
@end
static void refuses(FuxiCommandBuffer *c,void (^body)()){
 size_t n=c->_ops.size();body();assert(c->_rejected && c.error && c->_ops.size()==n);
 c->_rejected=NO;[c->_error release];c->_error=nil;
}
int main(){@autoreleasepool {
 static_assert(MTLPixelFormatR16Uint==23 && MTLPixelFormatR16Float==25 && MTLPixelFormatR32Float==55 && MTLPixelFormatRG32Float==105);
 Class cls=makeDeviceClass([FuxiBlitTestBase class]);id device=[cls alloc];FuxiOwner *state=[FuxiOwner new];state->backend=new fuxi::Backend;
 objc_setAssociatedObject(device,&ownerKey,state,OBJC_ASSOCIATION_RETAIN_NONATOMIC);[state release];fuxi::Backend::modelTextures=true;
 FuxiQueue *queue=(FuxiQueue *)[device newCommandQueue];
 auto errors=failureCount.load();
 for(MTLPixelFormat format:{MTLPixelFormatR16Uint,MTLPixelFormatR16Float,MTLPixelFormatR32Float,MTLPixelFormatRG32Float}){
  MTLTextureDescriptor *d=[MTLTextureDescriptor new];d.textureType=MTLTextureType1D;d.pixelFormat=format;d.width=16;d.height=1;d.depth=1;d.mipmapLevelCount=5;d.usage=MTLTextureUsageShaderRead;d.storageMode=MTLStorageModeShared;
  FuxiTexture *t=(FuxiTexture *)[device newTextureWithDescriptor:d];assert(t && t.textureType==MTLTextureType1D && t.height==1 && t.width==16);
  FuxiTexture *view=(FuxiTexture *)[t newTextureViewWithPixelFormat:format];assert(view && view.textureType==MTLTextureType1D);
  FuxiTexture *mip=(FuxiTexture *)[view newTextureViewWithPixelFormat:format textureType:MTLTextureType1D levels:NSMakeRange(1,1) slices:NSMakeRange(0,1)];assert(mip && mip.width==8 && mip.height==1 && mip->_baseLevel==1);
  uint8_t in[128],out[128];for(unsigned i=0;i<128;++i)in[i]=(uint8_t)i;
  auto pixel=fuxi::Backend::texelBytes((uint32_t)format);
  [t replaceRegion:MTLRegionMake1D(0,16) mipmapLevel:0 withBytes:in bytesPerRow:16*pixel];
  FuxiCommandBuffer *c=(FuxiCommandBuffer *)[queue commandBuffer];id<MTLBlitCommandEncoder> e=(id)[c blitCommandEncoder];assert(e && [(id)e conformsToProtocol:@protocol(MTLBlitCommandEncoder)]);
  [e copyFromTexture:(id)t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(8,1,1) toTexture:(id)mip destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
  [e synchronizeTexture:(id)mip slice:0 level:0];[e synchronizeResource:(id)t];[e endEncoding];assert(!c->_rejected && c->_ops[0].dstLevel==1);[c commit];assert(!c.error);
  [mip getBytes:out bytesPerRow:8*pixel fromRegion:MTLRegionMake1D(0,8) mipmapLevel:0];assert(!memcmp(in,out,8*pixel));
  [mip release];[view release];[t release];[d release];
 }
 MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:8 height:6 mipmapped:YES];d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
 FuxiTexture *t=(FuxiTexture *)[device newTextureWithDescriptor:d],*other=(FuxiTexture *)[device newTextureWithDescriptor:d];
 uint8_t input[192],output[192],expected[192];for(unsigned i=0;i<192;++i)input[i]=(uint8_t)i;memcpy(expected,input,192);
 for(unsigned y=0;y<4;++y)memcpy(expected+((y+1)*8+1)*4,input+y*32,24);
 [t replaceRegion:MTLRegionMake2D(0,0,8,6) mipmapLevel:0 withBytes:input bytesPerRow:32];
 for(NSUInteger level=1;level<t.mipmapLevelCount;++level)[t replaceRegion:MTLRegionMake2D(0,0,std::max(NSUInteger(1),t.width>>level),std::max(NSUInteger(1),t.height>>level)) mipmapLevel:level withBytes:input bytesPerRow:std::max(NSUInteger(1),t.width>>level)*4];
 FuxiTexture *view=(FuxiTexture *)[t newTextureViewWithPixelFormat:t.pixelFormat];
 FuxiCommandBuffer *c=(FuxiCommandBuffer *)[queue commandBuffer];id<MTLBlitCommandEncoder> e=(id)[c blitCommandEncoder];
 [e copyFromTexture:(id)t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(6,4,1) toTexture:(id)view destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(1,1,0)];
 [e copyFromTexture:(id)t toTexture:(id)other];[e endEncoding];assert(!c->_rejected);[view release];[t release];[c commit];assert(!c.error);
 [other getBytes:output bytesPerRow:32 fromRegion:MTLRegionMake2D(0,0,8,6) mipmapLevel:0];assert(!memcmp(output,expected,192));
 // Buffer-image overloads accept None and retain resources through completion.
 id<MTLBuffer> buffer=[device newBufferWithLength:256 options:0];
 c=(FuxiCommandBuffer *)[queue commandBuffer];e=(id)[c blitCommandEncoder];
 [e fillBuffer:buffer range:NSMakeRange(0,256) value:0x72];
 [e copyFromBuffer:buffer sourceOffset:16 sourceBytesPerRow:24 sourceBytesPerImage:0 sourceSize:MTLSizeMake(4,3,1) toTexture:(id)other destinationSlice:0 destinationLevel:1 destinationOrigin:MTLOriginMake(0,0,0) options:MTLBlitOptionNone];
 [e copyFromTexture:(id)other sourceSlice:0 sourceLevel:1 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(4,3,1) toBuffer:buffer destinationOffset:128 destinationBytesPerRow:24 destinationBytesPerImage:0 options:MTLBlitOptionNone];
 [e endEncoding];[c commit];assert(!c.error);for(unsigned y=0;y<3;++y)for(unsigned x=0;x<16;++x)assert(((uint8_t *)buffer.contents)[128+y*24+x]==0x72);
 assert(failureCount==errors);
 // Invalid shape, slices, levels, origins, format and stride never append work.
 c=(FuxiCommandBuffer *)[queue commandBuffer];e=(id)[c blitCommandEncoder];
 refuses(c,^{[e copyFromTexture:nil toTexture:nil];});
 auto badCopy=^(NSUInteger slice,NSUInteger level,MTLOrigin origin,MTLSize size){[e copyFromTexture:(id)other sourceSlice:slice sourceLevel:level sourceOrigin:origin sourceSize:size toTexture:(id)other destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];};
 refuses(c,^{badCopy(1,0,MTLOriginMake(0,0,0),MTLSizeMake(1,1,1));});
 refuses(c,^{badCopy(0,99,MTLOriginMake(0,0,0),MTLSizeMake(1,1,1));});
 refuses(c,^{badCopy(0,0,MTLOriginMake(NSUIntegerMax,0,0),MTLSizeMake(1,1,1));});
 refuses(c,^{badCopy(0,0,MTLOriginMake(0,0,1),MTLSizeMake(1,1,1));});
 refuses(c,^{badCopy(0,0,MTLOriginMake(0,0,0),MTLSizeMake(1,1,2));});
 refuses(c,^{[e copyFromBuffer:buffer sourceOffset:0 sourceBytesPerRow:3 sourceBytesPerImage:0 sourceSize:MTLSizeMake(1,1,1) toTexture:(id)other destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];});
 refuses(c,^{[e synchronizeTexture:(id)other slice:1 level:0];});
 refuses(c,^{[e copyFromTexture:(id)other sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(1,1,1) toBuffer:buffer destinationOffset:0 destinationBytesPerRow:4 destinationBytesPerImage:0 options:MTLBlitOptionDepthFromDepthStencil];});
 [e endEncoding];refuses(c,^{[e synchronizeResource:buffer];});
 for(auto value:{550u,554u}){d.pixelFormat=(MTLPixelFormat)value;assert(![device newTextureWithDescriptor:d]);}
 d.pixelFormat=MTLPixelFormatR32Float;d.textureType=MTLTextureType1D;d.height=2;d.usage=MTLTextureUsageShaderRead;assert(![device newTextureWithDescriptor:d]);
 // 3D sampled textures (the CoreAnimation colour lookup table): CPU slices round-trip; render targets, mip chains and linear buffers refuse.
 {MTLTextureDescriptor *v=[MTLTextureDescriptor new];v.textureType=MTLTextureType3D;v.pixelFormat=MTLPixelFormatRGBA8Unorm;v.width=16;v.height=16;v.depth=16;v.usage=MTLTextureUsageShaderRead;v.storageMode=MTLStorageModeShared;
  FuxiTexture *t3=(FuxiTexture *)[device newTextureWithDescriptor:v];assert(t3 && t3.textureType==MTLTextureType3D && t3.depth==16 && t3.width==16 && t3.mipmapLevelCount==1);
  static uint8_t in3[16*16*16*4],out3[16*16*16*4];for(unsigned i=0;i<sizeof(in3);++i)in3[i]=(uint8_t)(i*7);
  [t3 replaceRegion:MTLRegionMake3D(0,0,0,16,16,16) mipmapLevel:0 slice:0 withBytes:in3 bytesPerRow:64 bytesPerImage:1024];
  [t3 getBytes:out3 bytesPerRow:64 bytesPerImage:1024 fromRegion:MTLRegionMake3D(0,0,0,16,16,16) mipmapLevel:0 slice:0];assert(!memcmp(in3,out3,sizeof(in3)));
  uint8_t patch[8]={1,2,3,4,5,6,7,8};[t3 replaceRegion:MTLRegionMake3D(2,1,5,2,1,1) mipmapLevel:0 slice:0 withBytes:patch bytesPerRow:8 bytesPerImage:8];
  [t3 getBytes:out3 bytesPerRow:64 bytesPerImage:1024 fromRegion:MTLRegionMake3D(0,0,0,16,16,16) mipmapLevel:0 slice:0];assert(!memcmp(out3+5*1024+64+8,patch,8));
  @try{[t3 getBytes:out3 bytesPerRow:64 bytesPerImage:1024 fromRegion:MTLRegionMake3D(0,0,15,16,16,2) mipmapLevel:0 slice:0];assert(false);}@catch(NSException *e){(void)e;}
  [t3 release];
  v.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;assert(![device newTextureWithDescriptor:v]);v.usage=MTLTextureUsageShaderRead;
  v.mipmapLevelCount=2;assert(![device newTextureWithDescriptor:v]);v.mipmapLevelCount=1;
  v.depth=0;assert(![device newTextureWithDescriptor:v]);v.depth=16;v.textureType=MTLTextureType2D;assert(![device newTextureWithDescriptor:v]);
  [v release];}
 [buffer release];[other release];[queue release];[device release];
 puts("PASS: M9n native blit protocol/overloads, 1D formats/views, alias snapshot pixels, retained owners, buffer strides/fill/sync, subresource/shape/options/format refusal");
}}

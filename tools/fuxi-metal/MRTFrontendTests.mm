/* Native descriptor/encoding/retention controls. GPU pixels live in M9l. */
#define FUXI_MTL_FRONTEND_TEST
#define FUXI_MTL_BACKEND_HEADER "TestBackend.hpp"
#include "Frontend.mm"
#include "IOS16Defaults.hpp"
#include <cassert>
@interface FuxiMRTTestBase : NSObject {uint8_t bytes[520];}
@end
@implementation FuxiMRTTestBase
@end
int main(){@autoreleasepool {
 Class cls=makeDeviceClass([FuxiMRTTestBase class]);id device=[cls alloc];FuxiOwner *ownerState=[FuxiOwner new];ownerState->backend=new fuxi::Backend;
 objc_setAssociatedObject(device,&ownerKey,ownerState,OBJC_ASSOCIATION_RETAIN_NONATOMIC);[ownerState release];
 fuxi::Backend::modelTextures=true;fuxi::Backend::modelPipelines=true;FuxiQueue *q=(FuxiQueue *)[device newCommandQueue];
 FuxiLibrary *lib=[FuxiLibrary new];lib->_device=[device retain];FuxiFunction *v=[FuxiFunction new],*f=[FuxiFunction new];
 v->_library=[lib retain];v->_name=[@"vertex" copy];v->_stage=1;f->_library=[lib retain];f->_name=[@"fragment" copy];f->_stage=2;
 auto *pd=ios16PipelineDefaults();pd.vertexFunction=(id)v;pd.fragmentFunction=(id)f;
 FuxiTexture *textures[4]{};MTLRenderPassDescriptor *pass=ios16PassDefaults();
 for(unsigned i=0;i<4;++i){auto format=i==2?MTLPixelFormatRGBA16Float:i%2?MTLPixelFormatRGBA8Unorm:MTLPixelFormatBGRA8Unorm;
  auto *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:i==1?8:16 height:i==1?8:16 mipmapped:YES];td.storageMode=MTLStorageModePrivate;td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
  textures[i]=(FuxiTexture *)[device newTextureWithDescriptor:td];assert(textures[i]);auto a=pass.colorAttachments[i];a.texture=(id)textures[i];a.level=i==1?0:1;a.loadAction=(MTLLoadAction)(i%3);a.storeAction=i%2?MTLStoreActionDontCare:MTLStoreActionStore;a.clearColor=MTLClearColorMake(.125*i,.25,.5,1);
  pd.colorAttachments[i].pixelFormat=format;pd.colorAttachments[i].blendingEnabled=i%2;pd.colorAttachments[i].sourceRGBBlendFactor=MTLBlendFactorSourceAlpha;pd.colorAttachments[i].destinationRGBBlendFactor=MTLBlendFactorOneMinusSourceAlpha;pd.colorAttachments[i].writeMask=i==3?MTLColorWriteMaskRed:MTLColorWriteMaskAll;
 }
 NSError *error=nil;FuxiPipeline *pipeline=(FuxiPipeline *)[device newRenderPipelineStateWithDescriptor:pd error:&error];assert(pipeline && !error);
 FuxiCommandBuffer *cb=(FuxiCommandBuffer *)[q commandBuffer];FuxiRender *encoder=(FuxiRender *)[cb renderCommandEncoderWithDescriptor:pass];assert(encoder && encoder->_state.colorCount==4 && encoder->_state.colorMask==15);
 [encoder setRenderPipelineState:(id)pipeline];[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[encoder endEncoding];
 assert(!cb->_rejected && cb->_ops.size()==2);
 for(unsigned i=0;i<4;++i){auto first=cb->_ops[0].color(i),last=cb->_ops[1].color(i);assert(first.texture==textures[i]->_texture && first.level==(i==1?0u:1u) && first.load==i%3 && first.store==1);assert(last.load==1 && last.store==unsigned(i%2==0));assert(strstr(cb->_ops[0].diagnostic.c_str(),"colors=["));}
 assert(cb->_ops[0].encoderID && cb->_ops[0].encoderID==cb->_ops[1].encoderID);
 encoder=(FuxiRender *)[cb renderCommandEncoderWithDescriptor:pass];[encoder setRenderPipelineState:(id)pipeline];[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[encoder endEncoding];
 assert(cb->_ops.size()==3 && cb->_ops[2].encoderID!=cb->_ops[1].encoderID);
 for(unsigned i=0;i<4;++i)assert([cb->_resources containsObject:textures[i]]);
 // Pipeline state at slot 3 affects identity; absent slots' defaults are inert.
 auto misses=fuxi::Backend::pipelineMisses.load();pd.colorAttachments[3].writeMask=MTLColorWriteMaskAll;id changed=[device newRenderPipelineStateWithDescriptor:pd error:&error];assert(changed && fuxi::Backend::pipelineMisses==misses+1);[changed release];
 auto refuses=[&](void (^change)(),void (^restore)()) {change();FuxiCommandBuffer *bad=(FuxiCommandBuffer *)[q commandBuffer];id refused=[bad renderCommandEncoderWithDescriptor:pass];assert(refused && [refused isKindOfClass:[FuxiRefusedRender class]] && bad->_rejected);id<MTLRenderPipelineState> none=nil;[refused setRenderPipelineState:none];[refused drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[refused endEncoding];restore();};
 refuses(^{pass.colorAttachments[2].level=0;},^{pass.colorAttachments[2].level=1;});
 refuses(^{pass.colorAttachments[3].slice=1;},^{pass.colorAttachments[3].slice=0;});
 for(auto action:{MTLStoreActionMultisampleResolve,MTLStoreActionStoreAndMultisampleResolve,MTLStoreActionUnknown})refuses(^{pass.colorAttachments[2].storeAction=action;},^{pass.colorAttachments[2].storeAction=MTLStoreActionStore;});
 id visibility=[device newBufferWithLength:256 options:0];refuses(^{pass.visibilityResultBuffer=visibility;},^{pass.visibilityResultBuffer=nil;});[visibility release];
 id first=(id)textures[0];// blocks cannot capture C arrays
 refuses(^{pass.depthAttachment.texture=first;},^{pass.depthAttachment.texture=nil;});
 refuses(^{pass.stencilAttachment.texture=first;},^{pass.stencilAttachment.texture=nil;});
 refuses(^{pass.defaultRasterSampleCount=4;},^{pass.defaultRasterSampleCount=1;});
 // Slot 7 works without slot 0; mip area derives from the first present slot.
 for(unsigned i=0;i<4;++i)pass.colorAttachments[i].texture=nil;
 pass.colorAttachments[7].texture=(id)textures[1];pass.colorAttachments[7].loadAction=MTLLoadActionClear;pass.colorAttachments[7].storeAction=MTLStoreActionStore;
 cb=(FuxiCommandBuffer *)[q commandBuffer];encoder=(FuxiRender *)[cb renderCommandEncoderWithDescriptor:pass];assert(encoder && encoder->_state.colorCount==8 && encoder->_state.colorMask==128);[encoder endEncoding];assert(cb->_ops.size()==1 && !cb->_ops[0].color(0).texture && cb->_ops[0].color(7).texture==textures[1]->_texture);
 for(auto t:textures)[t release];[pipeline release];[v release];[f release];[lib release];[q release];[device release];
 puts("PASS: native MRT formats/blend/masks, independent actions/levels, sparse slot7, descriptor refusal context and resource retention");
}}

/* Objective-C encoder/ownership tests only. CPU model; no translation/GPU claim. */
#define FUXI_MTL_FRONTEND_TEST
#define FUXI_MTL_BACKEND_HEADER "TestBackend.hpp"
#include <cstddef>
static int testFunctionLookup(void *,const char *,char *,size_t);
#define FUXI_MTL_FUNCTION_LOOKUP testFunctionLookup
#include "Frontend.mm"
#include <cassert>
#include "LabelFrontendTests.hpp"
#include "IOS16Defaults.hpp"
#include "VertexFrontendTests.hpp"
// Only the dynamic translator boundary is modeled; actual Objective-C lookup,
// NSError propagation and specialization wrappers are exercised below.
static int testFunctionLookup(void *,const char *name,char *why,size_t size) {
 if(!strcmp(name,"fixture_fragment"))return 2;
 if(!strcmp(name,"fixture_exception"))throw std::runtime_error("lookup exception");
 snprintf(why,size,"test entry missing: %s",name);return 0;
}
@interface FuxiPipelineTestBase : NSObject {uint8_t bytes[520];}
@end
@implementation FuxiPipelineTestBase
@end
static void refuses(FuxiCommandBuffer *c,void (^body)()) {
 size_t count=c->_ops.size();body();assert(c->_rejected && c.error && c->_ops.size()==count);
 // Isolate each validation case; production never clears a rejected command.
 c->_rejected=NO;[c->_error release];c->_error=nil;
}
int main() {@autoreleasepool {
 Class cls=makeDeviceClass([FuxiPipelineTestBase class]);assert(cls);
 FuxiOwner *state=[FuxiOwner new];state->backend=new fuxi::Backend;state->registry=123;
 id device=[cls alloc];objc_setAssociatedObject(device,&ownerKey,state,OBJC_ASSOCIATION_RETAIN_NONATOMIC);[state release];
 id<MTLCommandQueue>queue=[device newCommandQueue];
 {
  FuxiLibrary *library=[FuxiLibrary new];library->_device=[device retain];
  FuxiFunction *vertex=[FuxiFunction new],*fragment=[FuxiFunction new];
  vertex->_library=[library retain];vertex->_name=[@"fixture_vertex" copy];vertex->_stage=1;
  fragment->_library=[library retain];fragment->_name=[@"fixture_fragment" copy];fragment->_stage=2;
  checkVertexDescriptors(device,vertex,fragment);[vertex release];[fragment release];[library release];
 }
 { // A modeled missing entry exercises real nil/error wrappers exactly once.
  FuxiLibrary *library=[FuxiLibrary new];library->_device=[device retain];checkLabel(library);
  id missing=nil;NSError *lookupError=nil;uint64_t failures=fuxi_metal_failure_count();
  assert(![library newFunctionWithName:@"fixture_missing" constantValues:missing error:&lookupError]);
  assert([lookupError.localizedDescription isEqualToString:@"test entry missing: fixture_missing"]);
  assert(fuxi_metal_failure_count()==++failures);
  MTLFunctionDescriptor *fd=[MTLFunctionDescriptor functionDescriptor];fd.name=@"fixture_missing";
  assert(![library newFunctionWithDescriptor:fd error:&lookupError]);
  assert([lookupError.localizedDescription isEqualToString:@"test entry missing: fixture_missing"]);
  assert(fuxi_metal_failure_count()==++failures);
  assert(![library newFunctionWithName:@"fixture_missing"]);
  assert(fuxi_metal_failure_count()==++failures);
  assert(![library newFunctionWithName:@"fixture_exception" constantValues:missing error:&lookupError]);
  assert([lookupError.localizedDescription isEqualToString:@"lookup exception"]);
  assert(fuxi_metal_failure_count()==++failures);
  assert(![library newFunctionWithName:missing constantValues:missing error:&lookupError] && lookupError);
  assert(fuxi_metal_failure_count()==++failures);
  FuxiFunction *function=(FuxiFunction *)[library newFunctionWithName:@"fixture_fragment" constantValues:missing error:&lookupError];
  assert(function && !lookupError && function.functionType==MTLFunctionTypeFragment);
  checkLabel(function);assert(fuxi_metal_failure_count()==failures);
  MTLFunctionDescriptor *roundtrip=[function functionDescriptor];assert([roundtrip.name isEqualToString:@"fixture_fragment"] && !roundtrip.constantValues && ![function options]);
  __block bool called=false;[library newFunctionWithName:@"fixture_fragment" constantValues:missing completionHandler:^(id<MTLFunction> f,NSError *e){called=true;assert(f && !e && [f.name isEqualToString:@"fixture_fragment"]);}];assert(called);
  [function release];[library release];
 }
 MTLSamplerDescriptor *sd=[MTLSamplerDescriptor new];id<MTLSamplerState>sampler=[device newSamplerStateWithDescriptor:sd];
 assert(sampler && sampler.device==device);checkLabel((id)sampler);[sampler release];sd.maxAnisotropy=8;
 sampler=[device newSamplerStateWithDescriptor:sd];assert(sampler);[sampler release];sd.maxAnisotropy=17;
 assert(![device newSamplerStateWithDescriptor:sd]);[sd release];
 FuxiTexture *t=[FuxiTexture new];t->_device=[device retain];t->_texture=new fuxi::Texture{};t->_texture->allocated=262144;t->_usage=MTLTextureUsageRenderTarget;
 FuxiPipeline *p=[FuxiPipeline new];p->_device=[device retain];p->_pipeline=new fuxi::Pipeline{};
 checkLabel(t);checkLabel(p);
 NSError *error=nil;assert(![device newLibraryWithData:dispatch_data_empty error:&error] && error);
 error=nil;assert(![device newLibraryWithURL:[NSURL URLWithString:@"https://example.invalid/test"] error:&error] && error);
 error=nil;assert(![device newRenderPipelineStateWithDescriptor:[[MTLRenderPipelineDescriptor new] autorelease] error:&error] && error);
 MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:256 height:256 mipmapped:NO];
 assert(![device newTextureWithDescriptor:td]);
 MTLRenderPassDescriptor *pass=ios16PassDefaults();pass.colorAttachments[0].texture=(id)t;
 pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
 FuxiCommandBuffer *c=(id)queue.commandBuffer;
 pass.colorAttachments[0].loadAction=(MTLLoadAction)3;
 refuses(c,^{[c renderCommandEncoderWithDescriptor:pass];});pass.colorAttachments[0].loadAction=MTLLoadActionClear;
 pass.defaultRasterSampleCount=4;refuses(c,^{[c renderCommandEncoderWithDescriptor:pass];});pass.defaultRasterSampleCount=0;
 pass.renderTargetArrayLength=2;refuses(c,^{[c renderCommandEncoderWithDescriptor:pass];});pass.renderTargetArrayLength=0;
 pass.renderTargetWidth=257;refuses(c,^{[c renderCommandEncoderWithDescriptor:pass];});pass.renderTargetWidth=0;
 pass.renderTargetHeight=257;refuses(c,^{[c renderCommandEncoderWithDescriptor:pass];});pass.renderTargetHeight=0;
 { // A smaller renderTargetWidth/Height is a top-left render area: default viewport/scissor, scissor bound.
  FuxiCommandBuffer *a=(id)queue.commandBuffer;pass.renderTargetWidth=53;pass.renderTargetHeight=40;
  FuxiRender *area=(FuxiRender *)[a renderCommandEncoderWithDescriptor:pass];pass.renderTargetWidth=0;pass.renderTargetHeight=0;
  assert(area && area->_state.areaWidth==53 && area->_state.areaHeight==40);
  assert(area->_state.scissor.extent.width==53 && area->_state.scissor.extent.height==40 && area->_state.viewport.width==53 && area->_state.viewport.y==40);
  [area setScissorRect:(MTLScissorRect){10,10,43,30}];assert(!a->_rejected);
  refuses(a,^{[area setScissorRect:(MTLScissorRect){0,0,54,40}];});
  [area endEncoding];assert(a->_ops.size()==1 && a->_ops[0].areaWidth==53 && a->_ops[0].areaHeight==40);
 }
 pass.depthAttachment.texture=(id)t;refuses(c,^{[c renderCommandEncoderWithDescriptor:pass];});pass.depthAttachment.texture=nil;
 pass.colorAttachments[7].resolveTexture=(id)t;refuses(c,^{[c renderCommandEncoderWithDescriptor:pass];});pass.colorAttachments[7].resolveTexture=nil;
 MTLRenderPassDescriptor *samplePass=[[pass copy] autorelease];
 MTLSamplePosition positions[4]={{0.25f,0.25f},{0.75f,0.25f},{0.25f,0.75f},{0.75f,0.75f}};[samplePass setSamplePositions:positions count:4];
 refuses(c,^{[c renderCommandEncoderWithDescriptor:samplePass];});
 FuxiRender *r=(FuxiRender *)[c renderCommandEncoderWithDescriptor:pass];assert(r);
 checkLabel(c);checkLabel(r);assert(!c->_rejected && !c.error);
 {auto dropped=fuxi_metal_dropped_draws();size_t ops=c->_ops.size();
  [r drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  assert(!c->_rejected && c->_ops.size()==ops && fuxi_metal_dropped_draws()==dropped+1);}
 refuses(c,^{[r setRenderPipelineState:(id)t];});[r setRenderPipelineState:(id)p];
 refuses(c,^{[r drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:3];});
 refuses(c,^{[r drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:65535 vertexCount:3];});
 [r setFrontFacingWinding:MTLWindingClockwise];[r setCullMode:MTLCullModeNone];[r setTriangleFillMode:MTLTriangleFillModeFill];
 [r setDepthBias:0 slopeScale:1 clamp:0];[r setBlendColorRed:0 green:0 blue:0 alpha:0];
 [r setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];assert(!c->_rejected);
 refuses(c,^{[r setFrontFacingWinding:MTLWindingCounterClockwise];});
 refuses(c,^{[r setCullMode:MTLCullModeBack];});
 refuses(c,^{[r setTriangleFillMode:MTLTriangleFillModeLines];});
 refuses(c,^{[r setDepthBias:1 slopeScale:1 clamp:0];});
 refuses(c,^{[r setDepthBias:0 slopeScale:NAN clamp:0];});
 refuses(c,^{[r setBlendColorRed:NAN green:0 blue:0 alpha:0];});
 refuses(c,^{[r setVisibilityResultMode:MTLVisibilityResultModeBoolean offset:0];});
 refuses(c,^{[r drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 instanceCount:2];});

 [r drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 instanceCount:1];
 [r drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
 [r endEncoding];refuses(c,^{[r endEncoding];});
 checkLabel(r);checkLabel(c);assert(!c->_rejected && !c.error);
 FuxiBuffer *b=(FuxiBuffer *)[device newBufferWithLength:262656 options:0];FuxiBlit *blit=(FuxiBlit *)[c blitCommandEncoder];
 refuses(c,^{[blit copyFromTexture:(id)t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(256,256,1)
 toBuffer:(id)b destinationOffset:0 destinationBytesPerRow:1000 destinationBytesPerImage:262144];});
 [blit copyFromTexture:(id)t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(256,256,1)
 toBuffer:(id)b destinationOffset:256 destinationBytesPerRow:1024 destinationBytesPerImage:262144];[blit endEncoding];
 assert(c->_ops.size()==3 && c->_ops[0].texture==t->_texture && c->_ops[0].pipeline==p->_pipeline && c->_ops[2].dst==b->_buffer);
 assert(c->_resources.count==3 && c->_resourceSet.size()==3);
 assert(c->_ops[0].diagnostic==c->_ops[1].diagnostic && !c->_ops[0].diagnostic.empty());
 // Recording snapshots the byte data and state; all bound buffers survive callers.
 FuxiBuffer *input=(FuxiBuffer *)[device newBufferWithLength:128 options:0];
 pass.colorAttachments[0].loadAction=MTLLoadActionLoad;pass.colorAttachments[0].storeAction=MTLStoreActionDontCare;
 r=[c renderCommandEncoderWithDescriptor:pass];assert(r);[r setRenderPipelineState:(id)p];
 [r setVertexBuffer:input offset:16 atIndex:0];[r setFragmentBuffer:input offset:32 atIndex:3];
 float uniform[4]={1,0.5,0.25,1};[r setFragmentBytes:uniform length:sizeof(uniform) atIndex:1];memset(uniform,0,sizeof(uniform));
 [r setViewport:(MTLViewport){16,24,128,96,0,1}];[r setScissorRect:(MTLScissorRect){24,32,64,64}];
 refuses(c,^{[r setVertexBuffer:input offset:0 atIndex:16];});
 {float *u=uniform;refuses(c,^{[r setFragmentBytes:u length:4097 atIndex:1];});}
 refuses(c,^{[r drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:3 indexType:MTLIndexTypeUInt32 indexBuffer:input indexBufferOffset:0];});
 [r drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:6 indexType:MTLIndexTypeUInt16 indexBuffer:input indexBufferOffset:4];[r endEncoding];
 const auto &recorded=c->_ops.back();assert(recorded.load==1 && !recorded.store && recorded.indices==input->_buffer && recorded.indexOffset==4);
 assert(recorded.buffers[0][0].offset==16 && recorded.buffers[1][3].offset==32 && recorded.buffers[1][1].bytes);
 assert(((float *)recorded.buffers[1][1].buffer->contents)[1]==0.5f && recorded.viewport.y==120 && recorded.scissor.offset.x==24);
 // M9b: the real adapters preserve snapshots, independent slots and ownership.
 r=[c renderCommandEncoderWithDescriptor:pass];assert(r);[r setRenderPipelineState:(id)p];
 id<MTLBuffer> bindings[2]={(id)input,(id)input};NSUInteger offsets[2]={8,24};
 [r setVertexBuffers:bindings offsets:offsets withRange:NSMakeRange(1,2)];
 [r setFragmentBuffers:bindings offsets:offsets withRange:NSMakeRange(2,2)];
 [r setVertexBufferOffset:48 atIndex:1];[r setFragmentBufferOffset:64 atIndex:3];
 refuses(c,^{[r setVertexBufferOffset:128 atIndex:1];});
 refuses(c,^{[r setVertexBufferOffset:0 atIndex:0];});
 refuses(c,^{[r setFragmentBuffers:nullptr offsets:nullptr withRange:NSMakeRange(NSUIntegerMax,2)];});
 [r setFragmentBytes:uniform length:sizeof(uniform) atIndex:0];
 refuses(c,^{[r setFragmentBufferOffset:4 atIndex:0];});
 FuxiTexture *sample=[FuxiTexture new];sample->_device=[device retain];sample->_texture=new fuxi::Texture{};sample->_texture->allocated=262144;sample->_usage=MTLTextureUsageShaderRead;
 sd=[MTLSamplerDescriptor new];sampler=[device newSamplerStateWithDescriptor:sd];[sd release];
 [r setFragmentTexture:(id)sample atTextureIndex:2 samplerState:sampler atSamplerIndex:3];
 assert(r->_samples[2]==sample && r->_samplers[3]==(id)sampler);
 id<MTLTexture> textures[1]={nil};id<MTLSamplerState> samplers[1]={nil};
 [r setFragmentTextures:textures withRange:NSMakeRange(2,1)];[r setFragmentSamplerStates:samplers withRange:NSMakeRange(3,1)];
 assert(!r->_samples[2] && !r->_samplers[3]);
 // M9f: slot 4 (CA second image) and the last admitted slot survive snapshots.
 [r setVertexBuffer:input offset:8 atIndex:15];[r setFragmentBuffer:input offset:16 atIndex:15];
 [r setFragmentTexture:(id)sample atTextureIndex:4 samplerState:sampler atSamplerIndex:15];
 refuses(c,^{[r setFragmentTexture:(id)sample atIndex:16];});
 MTLDepthStencilDescriptor *depth=[MTLDepthStencilDescriptor new];
 id ds=[device newDepthStencilStateWithDescriptor:depth];assert(ds);checkLabel(ds);[r setDepthStencilState:ds];[r setDepthStencilState:nil];
 depth.depthWriteEnabled=YES;assert(![device newDepthStencilStateWithDescriptor:depth]);depth.depthWriteEnabled=NO;
 depth.depthCompareFunction=MTLCompareFunctionLess;assert(![device newDepthStencilStateWithDescriptor:depth]);
 refuses(c,^{[r setDepthStencilState:(id)input];});
 [r drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
 assert(c->_ops.back().buffers[0][1].offset==48 && c->_ops.back().buffers[1][3].offset==64);
 assert(c->_ops.back().buffers[0][15].offset==8 && c->_ops.back().buffers[1][15].offset==16);
 assert(c->_ops.back().samples[4]==sample->_texture && c->_ops.back().samplers[15]==((FuxiSampler *)sampler)->_sampler);
 // Repeated state/draws retain each live object once, including shared slots.
 size_t retained=c->_resources.count;auto diagnostic=c->_ops.back().diagnostic;
 for(unsigned i=0;i<120;++i){[r setRenderPipelineState:(id)p];[r setFragmentTexture:(id)sample atIndex:4];[r setFragmentSamplerState:sampler atIndex:15];[r setVertexBuffer:input offset:8 atIndex:15];[r drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];}
 assert(c->_resources.count==retained && c->_resourceSet.size()==retained && c->_ops.back().diagnostic==diagnostic);
 [r setFragmentTexture:nil atIndex:4];[r setFragmentSamplerState:nil atIndex:15];
 assert(c->_ops.back().samples[4]==sample->_texture && c->_ops.back().samplers[15]==((FuxiSampler *)sampler)->_sampler);
 [r setVertexBufferOffset:80 atIndex:1];assert(c->_ops.back().buffers[0][1].offset==48);[r endEncoding];
 [ds release];[depth release];[sampler release];[sample release];
 [input release];
 [t release];[p release];[b release]; // Encoder and command references keep resources alive.
 assert([c->_resources[0] device]==device && [c->_resources[1] device]==device);
 // Never submit modeled render operations; translation/pixels are tested on real Vulkan.
 [queue release];[device release];
 }assert(fuxi::Backend::live==0);
 puts("PASS: pipeline frontend CPU-model descriptor refusals, multiple-draw state, readback bounds, retained resource ownership (no submit)");}

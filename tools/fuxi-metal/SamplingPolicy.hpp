/* A sampling refusal can omit one draw, only with the explicit boot opt-in. */
#pragma once
#include "TranslationPolicy.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
namespace fuxi {
inline std::atomic<uint64_t> skippedSamplingDraws{0};
inline bool skipSamplingDraw(const char *reason) {
 if(!systemTranslationPolicy().skipSampling)return false;
 auto count=++skippedSamplingDraws;
 // First eight, then powers of two: bounded log volume, cumulative count.
 if(count<=8 || !(count&(count-1)))fprintf(stderr,"FUXI MTL: sampling skip draw reason=%s count=%llu\n",reason,(unsigned long long)count);
 return true;
}
inline std::atomic<uint64_t> allowedFeedbackDraws{0},undefinedFeedbackReads{0};
// fuxigpufeedback=1: a same-image sample whose read range could not be proven
// disjoint from the attached level runs with the whole view and the attachment
// in GENERAL. Metal leaves a read of the written texel undefined; CoreAnimation's
// blur shaders compute their LOD at runtime and read other levels (guess).
inline bool allowFeedbackDraw(const char *reason) {
 if(!systemTranslationPolicy().allowFeedback)return false;
 auto count=++allowedFeedbackDraws;
 if(count<=8 || !(count&(count-1)))fprintf(stderr,"FUXI MTL: sampling allow feedback reason=%s count=%llu\n",reason,(unsigned long long)count);
 return true;
}
// With the opt-in, a possible read of a never-written level samples undefined
// contents instead of failing the device.
inline bool allowUndefinedRead() {
 if(!systemTranslationPolicy().allowFeedback)return false;
 auto count=++undefinedFeedbackReads;
 if(count<=8 || !(count&(count-1)))fprintf(stderr,"FUXI MTL: sampling undefined mip allowed count=%llu\n",(unsigned long long)count);
 return true;
}
}

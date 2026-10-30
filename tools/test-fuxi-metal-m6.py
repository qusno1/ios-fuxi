#!/usr/bin/env python3
"""Linux software Vulkan M6 resource tests; no Apple SDK or device required."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]

# Original GLSL controls isolate Vulkan binding/layout/sampler behavior. They
# deliberately make no claim about AIR lowering or CA's private shader ABI.
VERTEX = r'''#version 450
void main() {
 int i=gl_VertexIndex;
 bool right=i==1 || i==4 || i==5;
 bool top=i==2 || i==3 || i==5;
 gl_Position=vec4(right?1.0:-1.0,top?1.0:-1.0,0.0,1.0);
}
'''
FRAGMENT = r'''#version 450
layout(set=0,binding=0) uniform sampler2D image;
layout(location=0) out vec4 color;
void main() {color=texture(image,gl_FragCoord.xy/256.0);}
'''
CONTROL = r'''
#include "fuxi-metal/Translator.hpp"
#include <fstream>
#include <iterator>
#include <vector>
static std::vector<uint32_t> shader(const char *name) {
 std::ifstream f(getenv(name),std::ios::binary);std::vector<char> bytes{std::istreambuf_iterator<char>(f),{}};
 if(bytes.size()<20 || bytes.size()%4)throw std::runtime_error("control SPIR-V");
 std::vector<uint32_t> words(bytes.size()/4);memcpy(words.data(),bytes.data(),bytes.size());return words;
}
struct Product {std::vector<uint32_t> v,f;};
extern "C" int fuxi_m2v_sampler(void *,uint32_t,fuxi::BakedSampler *out,char *,size_t){*out={};return 1;}
extern "C" uint32_t fuxi_m2v_abi(){return 8;}
extern "C" void *fuxi_m2v_open(const uint8_t *,size_t,char *,size_t){return nullptr;}
extern "C" void fuxi_m2v_close(void *){}
extern "C" int fuxi_m2v_function(void *,const char *,char *,size_t){return 0;}
extern "C" void fuxi_m2v_free(void *p){delete (Product *)p;}
// This single authored control has one stable input/product identity.
extern "C" int fuxi_m2v_identity(void *,const char *,void *,const char *,const fuxi::CompileOptions *,uint8_t *out,char *,size_t){memset(out,1,32);return 1;}
extern "C" int fuxi_m2v_product_hash(void *,uint8_t *out){memset(out,2,32);return 1;}
extern "C" int fuxi_m2v_compile(void *,const char *,void *,const char *,void *,uint64_t,uint64_t,const fuxi::CompileOptions *options,fuxi::Translation *out,char *,size_t) {
 auto *p=new Product{shader("CONTROL_VERTEX"),shader("CONTROL_FRAGMENT")};
 *out={};out->orderedColorAccess=options->orderedColorAccess;out->handle=p;out->vertex=p->v.data();out->vertexCount=p->v.size();out->vertexEntry="main";
 out->fragment=p->f.data();out->fragmentCount=p->f.size();out->fragmentEntry="main";
 out->mask=out->colors[0].mask=15;out->rangeCount=1;out->ranges[0]={0,4,16};out->pushSize=4;out->pushStages=16;out->descriptorCount=1;out->descriptors[0]={0,1,16};return 1;
}
extern "C" int fuxi_m2v_draw(void *,const fuxi::DrawInput *in,fuxi::DrawOutput *out,char *,size_t){
 if(!in->formats[0] || !in->filters[0])return 0;*out={};out->count=in->count;out->first=in->start;out->descriptorCount=1;out->descriptors[0]={0,1,16,0,0,44};out->pushCount=1;out->pushes[0].size=4;out->pushes[0].stages=16;return 1;
}
extern "C" int fuxi_m2v_variant(void *,const fuxi::DrawInput *,fuxi::Translation *,char *,size_t){return 0;}

'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--headers', type=Path, required=True)
    parser.add_argument('--icd', type=Path, default=Path('/lib/x86_64-linux-gnu/libvulkan_lvp.so'))
    parser.add_argument('--glslang', type=Path, help='optional glslangValidator for actual Vulkan sampled-quad controls')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        directory = Path(tmp)
        binary = directory / 'resources'
        extra = []
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
        if args.glslang:
            for name, source in [('vert', VERTEX), ('frag', FRAGMENT)]:
                text = directory / ('control.' + name)
                text.write_text(source)
                spirv = text.with_suffix('.spv.' + name)
                subprocess.run([str(args.glslang), '-V', '--target-env', 'vulkan1.2', str(text), '-o', str(spirv)], check=True)
                env['CONTROL_' + ('VERTEX' if name == 'vert' else 'FRAGMENT')] = str(spirv)
            control = directory / 'control.cpp'
            control.write_text('#include <cstring>\n' + CONTROL)
            library = directory / 'control.so'
            subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                            '-I' + str(ROOT / 'tools'), str(control), '-o', str(library)], check=True)
            extra = ['-DFUXI_MTL_TRANSLATOR="' + str(library) + '"']
        subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-DFUXI_MTL_HOST_TEST',
                        '-DFUXI_MTL_ICD="' + str(args.icd.resolve()) + '"',
                        *extra, '-I' + str(args.headers), '-I' + str(ROOT / 'tools'),
                        str(ROOT / 'tools/test-fuxi-metal-m6.cpp'), '-ldl', '-pthread', '-o', str(binary)], check=True)
        subprocess.run([str(binary)] + (['sampling'] if args.glslang else []), env=env, check=True, timeout=30)


if __name__ == '__main__':
    main()

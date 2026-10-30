#!/usr/bin/env python3
"""Offline models of the production union vnode policy and present priority.

No supplied reference execution. Complete vfs patch application and kernel
syntax are also checked by test-fuxi-gpu-scanout.py --xnu-buildtree.
"""
import importlib.util
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    spec = importlib.util.spec_from_file_location('display', ROOT / 'tools/test-fuxi-unixnu.py')
    display = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(display)
    source = (ROOT / 'xnu/fuxi-unixnu/SweetFramebuffer.cpp').read_text()
    priority = display.function(source, 'sweet_fb_timer_options')
    prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
static unsigned option, parses, puts;
static int PE_parse_boot_argn(const char *key, void *value, unsigned bytes) {
 assert(!strcmp(key,"fuxiuniondir") || !strcmp(key,"fuxifbprio"));
 assert(bytes==sizeof(option));memcpy(value,&option,bytes);parses++;return option!=0;
}
enum {VDIR=2, VLNK=5, VREG=1};
struct vnode {unsigned v_type,refs;};
typedef struct vnode *vnode_t;
static void vnode_put(vnode_t vp) {assert(vp->refs);vp->refs--;puts++;}
#define kIOTimerEventSourceOptionsPriorityHigh 0
#define kIOTimerEventSourceOptionsDefault 1
#include "fuxi_unixnu_lookup.h"
'''
    test = r'''
int main(void) {
 // Every option value except 1 keeps the old referenced lower vnode.
 const unsigned values[]={0,1,2,~0u};
 for(unsigned value:values)for(unsigned type:{VDIR,VLNK,VREG}) {
  option=value;struct vnode node={type,1};unsigned before=puts;
  int error=fuxi_unixnu_lower_directory(&node);
  bool refuse=value==1 && type!=VDIR;
  assert(error==(refuse?ENOENT:0));
  assert(node.refs==(refuse?0u:1u) && puts==before+refuse);
  // A successful caller owns exactly one iocount, an error caller owns none.
  if(!error)vnode_put(&node);
  assert(!node.refs);
 }
 for(unsigned value:values) {
  option=value;unsigned before=parses;
  unsigned expected=kIOTimerEventSourceOptionsDefault;
#if defined(FUXI)
  if(value==1)expected=kIOTimerEventSourceOptionsPriorityHigh;
  before++;
#endif
  assert(sweet_fb_timer_options()==expected && parses==before);
 }
}
'''
    with tempfile.TemporaryDirectory(prefix='fuxi-port2-') as tmp:
        tmp = Path(tmp)
        (tmp / 'pexpert').mkdir()
        (tmp / 'pexpert/pexpert.h').write_text('/* PE_parse_boot_argn is modeled above. */\n')
        src = tmp / 'test.cpp'
        src.write_text('#include <initializer_list>\n' + prefix + priority + test)
        for mode in ([], ['-DFUXI']):
            exe = tmp / ('fuxi' if mode else 'sweet')
            subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', *mode, '-I'+str(tmp),
                            '-I'+str(ROOT / 'xnu/fuxi-unixnu'), str(src), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
    print('PASS: production lower vnode iocount/errno policy, exact opt-in and default/SWEET/FUXI timer priorities')


if __name__ == '__main__':
    main()

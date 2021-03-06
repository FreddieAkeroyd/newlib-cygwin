//  By aaronwl 2003-01-28 for mingw-msvcrt
//  Public domain: all copyrights disclaimed, absolutely no warranties */

#include <stdarg.h>
#include <wchar.h>


int vswscanf(const wchar_t * __restrict__ s, const wchar_t * __restrict__ format,
  va_list arg) {

  int ret;

  __asm__(

    // allocate stack (esp += frame - arg3 - (8[arg1,2] + 12))
    "movl	%%esp, %%ebx\n\t"
    "lea	0xFFFFFFEC(%%esp, %6), %%esp\n\t"
    "subl	%5, %%esp\n\t"

    // set up stack
    "movl	%1, 0xC(%%esp)\n\t"  // s
    "movl	%2, 0x10(%%esp)\n\t"  // format
    "lea	0x14(%%esp), %%edi\n\t"
    "movl	%%edi, (%%esp)\n\t"  // memcpy dest
    "movl	%5, 0x4(%%esp)\n\t"  // memcpy src
    "movl	%5, 0x8(%%esp)\n\t"
    "subl	%6, 0x8(%%esp)\n\t"  // memcpy len
    "call	_memcpy\n\t"
    "addl	$12, %%esp\n\t"

    // call sscanf
    "call	_swscanf\n\t"

    // restore stack
    "movl	%%ebx, %%esp\n\t"

    : "=a"(ret), "=c"(s), "=d"(format)
    : "1"(s), "2"(format), "S"(arg),
      "a"(&ret)
    : "ebx");

  return ret;
}

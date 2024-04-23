#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

// This implements a simple JIT for x86_64 or arm64 with support for
// symbolization with perfmap. We don't use any JIT framework or assembler for
// the sake of simplicity.

// Some tools have heuristics to unwind the stack even
// with frame pointers *and* unwind information omitted.
#define ENABLE_FRAME_POINTERS_IN_JIT true
// Amount of items to push into the stack to simulate a
// more realistic stack usage. Otherwise stack unwinding with
// frame pointers might work by accident.
#define STACK_ITEMS 32

int __attribute__((noinline)) aot_top() {
  for (int i = 0; i < 1000; i++) {
  }

  return 0;
}

// ahead of time
int __attribute__((noinline)) aot2() { return aot_top(); }

int __attribute__((noinline)) aot1() { return aot2(); }

int __attribute__((noinline)) aot() { return aot1(); }

void add_preamble(char **mem) {
  if (!ENABLE_FRAME_POINTERS_IN_JIT) {
    return;
  }

#if ARM
  *(*mem)++ = 0xfd; // stp x29, x30, [sp, #-16]!
  *(*mem)++ = 0x7b;
  *(*mem)++ = 0xbf;
  *(*mem)++ = 0xa9;
  *(*mem)++ = 0xfd; // mov x29, sp
  *(*mem)++ = 0x03;
  *(*mem)++ = 0x00;
  *(*mem)++ = 0x91;
#else
  *(*mem)++ = 0x55; // push   %rbp
  *(*mem)++ = 0x48; // mov    %rsp,%rbp
  *(*mem)++ = 0x89;
  *(*mem)++ = 0xe5;
#endif
}

void add_epilogue(char **mem) {
  if (!ENABLE_FRAME_POINTERS_IN_JIT) {
    return;
  }
#if ARM
  *(*mem)++ = 0xfd; // ldp x29, x30, [sp], #16
  *(*mem)++ = 0x7b;
  *(*mem)++ = 0xc1;
  *(*mem)++ = 0xa8;
#else
  *(*mem)++ = 0x5d;   // pop    %rbp
#endif
}

// Add arbitrary on the stack to change stack
// pointer.
void push_stuff_to_stack(char **mem) {
  for (int i = 0; i < STACK_ITEMS; i++) {
#if ARM
    *(*mem)++ = 0x40; // mov w0, #0xfafa
    *(*mem)++ = 0x5f;
    *(*mem)++ = 0x9f;
    *(*mem)++ = 0x52;
    // 0xe0, 0x0f, 0x1f, 0xb8,
    *(*mem)++ = 0xe0; // str w0, [sp, #-16]!
    *(*mem)++ = 0x0f;
    *(*mem)++ = 0x1f;
    *(*mem)++ = 0xb8;
#else
    *(*mem)++ = 0x68; // push   0xfafafa
    *(*mem)++ = 0xfa;
    *(*mem)++ = 0xfa;
    *(*mem)++ = 0xfa;
    *(*mem)++ = 0x00;
#endif
  }
}
// Pop the stack and discard
void pop_stuff_from_stack(char **mem) {
  for (int i = 0; i < STACK_ITEMS; i++) {
#if ARM
    *(*mem)++ = 0xff; // add sp, sp, #16
    *(*mem)++ = 0x43;
    *(*mem)++ = 0x00;
    *(*mem)++ = 0x91;
#else
    *(*mem)++ = 0x48; // add    rsp,0x8.
    *(*mem)++ = 0x83;
    *(*mem)++ = 0xc4;
    *(*mem)++ = 0x08;
#endif
  }
}

char *call(char **mem) {
#if ARM
  char *fixup = *mem;
  // bl <relative>
  // We don't know this address yet, will fix up later.
  for (int i = 0; i < 3; i++) {
    *(*mem)++ = 0x00;
  }
  *(*mem)++ = 0x94;
#else
  *(*mem)++ = 0xe8; // call <relative>
  char *fixup = *mem;
  // We don't know this address yet, will fix up later.
  for (int i = 0; i < 4; i++) {
    *(*mem)++ = 0x00;
  }
#endif

  return fixup;
}

void ret(char **mem) {
#if ARM
  *(*mem)++ = 0xc0;
  *(*mem)++ = 0x03;
  *(*mem)++ = 0x5f;
  *(*mem)++ = 0xd6;
#else
  *(*mem)++ = 0xc3;
#endif
}

void fixup(char *fixup, uintptr_t callee) {
#if ARM
  uint32_t offset = (callee - (uintptr_t)fixup) / 4;
#else
  uint32_t offset = (callee - (uintptr_t)fixup) - 4;
#endif
  *fixup++ = (offset & 0xFF) >> 0;
  *fixup++ = (offset & 0xFF00) >> 8;
  *fixup++ = (offset & 0xFF0000) >> 16;
#if !ARM
  *fixup++ = (offset & 0xFF000000) >> 24;
#endif
}

void leaf_body(char **mem) {
#if ARM
  *(*mem)++ = 0x00; // mov w0, #0
  *(*mem)++ = 0x00;
  *(*mem)++ = 0x80;
  *(*mem)++ = 0x52;
  *(*mem)++ = 0x02; // b test
  *(*mem)++ = 0x00;
  *(*mem)++ = 0x00;
  *(*mem)++ = 0x14;
  *(*mem)++ = 0x00; // loop: add w0, w0, #1
  *(*mem)++ = 0x04;
  *(*mem)++ = 0x00;
  *(*mem)++ = 0x11;
  *(*mem)++ = 0x1f; // test: cmp w0, #1000
  *(*mem)++ = 0xa0;
  *(*mem)++ = 0x0f;
  *(*mem)++ = 0x71;
  *(*mem)++ = 0xcd; // ble loop
  *(*mem)++ = 0xff;
  *(*mem)++ = 0xff;
  *(*mem)++ = 0x54;
#else
  *(*mem)++ = 0xc7; // movl   $0x0,-0x4(%rbp)
  *(*mem)++ = 0x45;
  *(*mem)++ = 0xfc;
  *(*mem)++ = 0x00;
  *(*mem)++ = 0x00;
  *(*mem)++ = 0x00;
  *(*mem)++ = 0x00;
  *(*mem)++ = 0xeb; // jmp    <first cmpl below>
  *(*mem)++ = 0x04;
  *(*mem)++ = 0x83; // addl   $0x1,-0x4(%rbp)
  *(*mem)++ = 0x45;
  *(*mem)++ = 0xfc;
  *(*mem)++ = 0x01;
  *(*mem)++ = 0x81; // cmpl   $0x3e7,-0x4(%rbp) // <- 999 (1000 iters - 1)
  *(*mem)++ = 0x7d;
  *(*mem)++ = 0xfc;
  *(*mem)++ = 0xe7;
  *(*mem)++ = 0x03;
  *(*mem)++ = 0x00;
  *(*mem)++ = 0x00;
  *(*mem)++ = 0x7e; // jle    0x401153 <first addl above>
  *(*mem)++ = 0xf3;
#endif
}

int main() {
  size_t jit_size = 5000;
  char *mem = (char *)mmap(NULL, jit_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == (void *)-1) {
    printf("mmap failed: %s\n", strerror(errno));
    return -1;
  }
  char *mem_start = mem;
  printf("jit segment starts at: %p\n", mem_start);
  void (*jit_func)() = (void (*)())mem_start;

  // ===== Entrypoint function =====
  unsigned long long jit1_first_addr = (unsigned long long)mem;
  add_preamble(&mem);
  push_stuff_to_stack(&mem);

  // Body
  char *to_call = call(&mem);

  pop_stuff_from_stack(&mem);
  add_epilogue(&mem);
  ret(&mem);

  unsigned long long jit1_last_addr = (unsigned long long)mem;

  fixup(to_call, (uintptr_t)mem);

  // ===== Leaf func =====
  unsigned long long jit2_first_addr = (unsigned long long)mem;
  add_preamble(&mem);
  push_stuff_to_stack(&mem);

  leaf_body(&mem);

  pop_stuff_from_stack(&mem);
  add_epilogue(&mem);
  ret(&mem);

  for (char *x = mem_start; x <= mem; ++x) {
    printf("0x%02X", *x);
    if ((x + 1 - mem_start) % 8 == 0) {
      printf("\n");
    } else {
      printf(" ");
    }
  }

  unsigned long long jit2_last_addr = (unsigned long long)mem;

  // We are done writing code, let's not make it writable anymore.
  mprotect(mem_start, jit_size, PROT_EXEC);

  // Write perfmap so perf can symbolize the jitted functions.
  //
  // https://github.com/torvalds/linux/blob/master/tools/perf/Documentation/jit-interface.txt
  char path[100];
  sprintf(path, "/tmp/perf-%d.map", getpid());
  printf("path for jitdump: %s\n", path);

  FILE *file = fopen(path, "w+");
  if (file == NULL) {
    printf("fopen failed: %s\n", strerror(errno));
    exit(1);
  }

  fprintf(file, "%llx %llx %s\n", jit1_first_addr,
          jit1_last_addr - jit1_first_addr, "jit_middle");
  fprintf(file, "%llx %llx %s\n", jit2_first_addr,
          jit2_last_addr - jit2_first_addr, "jit_top");

  fclose(file);

  while (true) {
    jit_func();
    aot();
  }
  return 0;
}

// Notes:
//
// - GDB doesn't seem to use perfmap;
// - GDB complains about our stack;
// (gdb) bt
// #0  0x00007ffff7fbe023 in ?? ()
// #1 a 0x00007fffffffd8f0 in ?? ()
// #2  0x00007ffff7fbe010 in ?? ()
// #3 a 0x00007fffffffd9d0 in ?? ()
// #4  0x00000000004016a8 in main () at src/basic-cpp.cpp:135
// Backtrace stopped: previous frame inner to this frame (corrupt stack?)

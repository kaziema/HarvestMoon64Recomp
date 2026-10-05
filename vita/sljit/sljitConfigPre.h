/* sljit configuration for the PS Vita build.
 * The Vita has no mmap, and mods (the only user of the live recompiler) are not supported,
 * so executable memory allocation always fails instead of using sljit's built-in allocator. */
#ifndef HM64_VITA_SLJIT_CONFIG_PRE_H
#define HM64_VITA_SLJIT_CONFIG_PRE_H

#define SLJIT_EXECUTABLE_ALLOCATOR 0
#define SLJIT_UTIL_STACK 0
#define SLJIT_MALLOC_EXEC(size, exec_allocator_data) ((void*)0)
#define SLJIT_FREE_EXEC(ptr, exec_allocator_data) ((void)0)

#endif

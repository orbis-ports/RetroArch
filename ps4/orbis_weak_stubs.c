/* RetroArch - A frontend for libretro.
 *
 * Definitions for symbols a module may reference weakly and never get.
 *
 * ⚠ WHY THIS IS NEEDED AT ALL. ld.lld is happy to leave an UNDEFINED WEAK symbol undefined - that
 * is what weak means, and at run time it would read as NULL. create-fself is not: it maps every
 * remaining undefined symbol to a library NID, finds no library that exports this one, and
 * refuses the whole module with
 *
 *     Failed to build FSELF: missing library for symbol (ZSTD_trace_decompress_end)
 *
 * ⚠ AND IT REFUSES WITH EXIT STATUS 0, so a build script that trusts the exit code sees success
 * and no .prx. The harness checks for the file instead.
 *
 * The alternative - `--defsym=NAME=0` - would satisfy create-fself and leave a call to address
 * zero for anything that does not null-check first. These are zstd's tracing hooks and a no-op is
 * their documented off state, so a real definition is both safer and more honest.
 *
 * This file is compiled into the core-build fallback archive, so a module picks it up only if it
 * has one of these undefined and nothing else provides it.
 */

/* zstd's trace context is an unsigned long long; the trace struct is opaque here because nothing
 * in this file looks inside it. */
unsigned long long ZSTD_trace_compress_begin(const void *cctx);
void               ZSTD_trace_compress_end(unsigned long long ctx, const void *trace);
unsigned long long ZSTD_trace_decompress_begin(const void *dctx);
void               ZSTD_trace_decompress_end(unsigned long long ctx, const void *trace);

unsigned long long ZSTD_trace_compress_begin(const void *cctx) { (void)cctx; return 0; }
void ZSTD_trace_compress_end(unsigned long long ctx, const void *trace)
{ (void)ctx; (void)trace; }
unsigned long long ZSTD_trace_decompress_begin(const void *dctx) { (void)dctx; return 0; }
void ZSTD_trace_decompress_end(unsigned long long ctx, const void *trace)
{ (void)ctx; (void)trace; }

/* ---------------------------------------------------------------- did the constructors run?
 *
 * ⚠ THIS ANSWERS A QUESTION THAT COST A WRONG DIAGNOSIS. Every C++-dominant core crashed with
 *
 *     terminating with uncaught exception of type std::length_error:
 *     allocator<T>::allocate(size_t n) 'n' exceeds maximum supported size
 *
 * which is what an unconstructed global container looks like when something asks it for its
 * size. crtlib.o's `module_start` walks __init_array_start..__init_array_end calling each entry,
 * and those two symbols are BSS VARIABLES IN crtlib.o - eight bytes apart, never filled in - not
 * linker-provided section bounds. OpenOrbis's link.x collects `.init_array` and defines nothing
 * around it, so the walk covers eight bytes of zeroes.
 *
 * Bracketing the section in our own linker script fixed the symbols - they now span the real
 * array, 0x48 bytes, nine entries - and the crash did NOT change. So the symbols were necessary
 * and not sufficient, and the next question is whether module_start runs at all.
 *
 * A constructor that says so is the cheapest possible answer, and it goes to klog because that
 * is the channel that survives a process about to die. It prints once per module.
 */
/* ⚠ IT WRITES A FILE, NOT A LOG LINE, AND THAT DISTINCTION COST A ROUND TRIP. The first version
 * called sceKernelDebugOutText and printed nothing - which could equally mean "the constructor
 * did not run" or "klog from inside a .prx does not reach the host", and those need opposite
 * fixes. A file is answered by looking at the filesystem and depends on no channel.
 *
 * The array itself has been verified correct: ten entries, relocated (this is a -pie link so the
 * file holds zeroes and R_X86_64_RELATIVE fills them at load), and the FIRST relocation points
 * at this function. So if the file is absent, module_start did not walk the array. */
extern int open(const char *, int, ...);
extern long write(int, const void *, unsigned long);
extern int close(int);

__attribute__((constructor(101)))
static void orbis_ctor_probe(void)
{
   /* O_WRONLY|O_CREAT|O_TRUNC = 1|0x200|0x400 on this FreeBSD-derived kernel. */
   const int fd = open("/data/ctor-ran.txt", 1 | 0x200 | 0x400, 0666);
   if (fd >= 0)
   {
      write(fd, "a global constructor ran in a .prx\n", 35);
      close(fd);
   }
}

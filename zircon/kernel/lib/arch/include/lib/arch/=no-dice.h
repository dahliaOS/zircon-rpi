// These C++ macros are exclusively in header files defining assembly macros.
// They wrap assembly code so it's emitted whether the header is used in an
// assembly (`.S`) source file or in a C++ source file.  The assembly macros
// are defined in C++ source files so that inline `__asm__` statements can use
// them just like assembly source files.
//
// Chunks of assembly code are bracketed `ARCH_ASM_BEGIN` and `ARCH_ASM_END`,
// while appear alone on a line (aside from C++ any comments).  Between those
// the only lines that can appear are blank lines, C++ comments, and
// `ARCH_ASM(...)` lines of assembly code.  Note the assembly code is passed
// through C++ macro expansion and then stringified.  It's used in a
// traditional top-level `__asm__` so no characters like `%` are treated
// specially by the C++ compiler.  However, `\` is then special as in C++
// strings.  So use `ARCH_ASM_ARG(name)` in place of `\name` to refer to macro
// arguments inside a `.macro` definition; `ARCH_ASM_ARG_STRING(name)` must
// be used in place of `"\name"`, e.g. in `.ifeqs` directives.
//
// ```
// ARCH_ASM_BEGIN
// ARCH_ASM(.some assembly directives, ...)
// ARCH_ASM_END
// ```
#ifdef __ASSEMBLER__
#define ARCH_ASM_BEGIN
#define ARCH_ASM(...) __VA_ARGS__
#define ARCH_ASM_ARG(name) \name
#define ARCH_ASM_ARG_STRING(name) "\name"
#define ARCH_ASM_END
#else
#define ARCH_ASM_BEGIN __asm__ (
#define ARCH_ASM(...) _ARCH_ASM_1(__VA_ARGS__)
#define _ARCH_ASM_1(...) #__VA_ARGS__ "\n\t"
#define ARCH_ASM_ARG(name) \\name
#define ARCH_ASM_ARG_STRING(name) "\\name"
#define ARCH_ASM_END );
#endif

ARCH_ASM_BEGIN

ARCH_ASM_END

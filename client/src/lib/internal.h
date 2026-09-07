#ifndef SHYAKE_INTERNAL_H
#define SHYAKE_INTERNAL_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Short fixed-width type names for internal code. The public header
 * include/shyake.h deliberately keeps the <stdint.h> spellings: these
 * aliases are common identifiers and must not leak to FFI consumers.
 */
typedef uint8_t u8;
typedef int8_t i8;
typedef uint16_t u16;
typedef int16_t i16;
typedef uint32_t u32;
typedef int32_t i32;
typedef uint64_t u64;
typedef int64_t i64;
typedef float f32;
typedef double f64;
typedef size_t usize;
typedef ptrdiff_t isize;
typedef uintptr_t uptr;

/*
 * Byte-oriented crypto and wire-format code throughout the client
 * assumes 8-bit bytes. POSIX mandates it; refuse to build rather than
 * silently miscompute lengths on a platform that does not.
 */
#if CHAR_BIT != 8
#error "shyake requires CHAR_BIT == 8"
#endif

#endif /* SHYAKE_INTERNAL_H */

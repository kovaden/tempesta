/*
 *		Tempesta TLS
 *
 * Multi-precision integer library.
 *
 * The following sources were referenced in the design of this Multi-precision
 * Integer library:
 *
 * [1] Handbook of Applied Cryptography - 1997
 *     Menezes, van Oorschot and Vanstone
 *
 * [2] Multi-Precision Math, Tom St Denis
 *
 * [3] GNU Multi-Precision Arithmetic Library
 *     https://gmplib.org/manual/index.html
 *
 * Based on mbed TLS, https://tls.mbed.org.
 *
 * Copyright (C) 2006-2015, ARM Limited, All Rights Reserved
 * Copyright (C) 2015-2019 Tempesta Technologies, Inc.
 * SPDX-License-Identifier: GPL-2.0
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include <linux/bitops.h>

#include "lib/str.h"
#include "bignum.h"
#include "tls_internal.h"

/*
 * Convert between bits/chars and number of limbs
 * Divide first in order to avoid potential overflows
 */
#define BITS_TO_LIMBS(n)	(((n) + BIL - 1) >> BSHIFT)
#define CHARS_TO_LIMBS(n)	(((n) + CIL - 1) >> LSHIFT)

/* Maximum sliding window size in bits used for modular exponentiation. */
#define MPI_W_SZ		6

static DEFINE_PER_CPU(TlsMpi *, g_buf);

/**
 * Initialize one MPI (make internal references valid).
 * This just makes it ready to be set or freed, but does not define a value
 * for the MPI.
 *
 * Single-limb MPI are quite rare, so we don't try to use TlsMpi->p as an
 * integer value for sigle-limb case.
 */
void
ttls_mpi_init(TlsMpi *X)
{
	if (unlikely(!X))
		return;

	X->s = 1;
	X->used = 0;
	X->limbs = 0;
	X->p = NULL;
}

void
ttls_mpi_free(TlsMpi *X)
{
	if (unlikely(!X))
		return;

	if (X->p) {
		memset(X->p, 0, X->limbs << LSHIFT);
		kfree(X->p);
	}

	ttls_mpi_init(X);
}

/**
 * Reallocate the MPI data area and copy old data if necessary.
 *
 * TODO #1064: use exponential growth or allocate with some step like 2 or 4
 * limbs? Need to explore common memory allocation pattern -> collect histogram.
 * Probably we can allocate all required MPIs on early handshake phase with the
 * key parameters size knowledge, so we could avoid allocations at all.
 * It seems we can not use per-cpu pages to store MPI since we need the MPIs
 * state between handshake messages.
 */
int
__mpi_realloc(TlsMpi *X, size_t nblimbs, unsigned int flags)
{
	unsigned long *p;

	if (unlikely(nblimbs > TTLS_MPI_MAX_LIMBS))
		return -ENOMEM;
	if (unlikely(X->limbs >= nblimbs))
		return 0;

	if (!(p = kmalloc(nblimbs * CIL, GFP_ATOMIC)))
		return -ENOMEM;

	if (X->p) {
		if (flags & MPI_GROW_COPY)
			memcpy(p, X->p, X->used * CIL);
		kfree(X->p);
	}
	if (flags & MPI_GROW_ZERO)
		memset(p + X->used, 0, (nblimbs - X->used) * CIL);

	X->limbs = nblimbs;
	X->p = p;

	return 0;
}

/**
 * Set proper @X->used. Move from @n towards @X->p[0] and throw out all zeros.
 */
void
mpi_fixup_used(TlsMpi *X, size_t n)
{
	if (unlikely(n > X->limbs))
		n = X->limbs;
	/*
	 * Leave the least significant limb even if it's zero to represent
	 * zero valued MPI.
	 */
	for (X->used = n; X->used > 1 && !X->p[X->used - 1]; )
		--X->used;
}

/**
 * Resize down as much as possible, while keeping at least the specified
 * number of limbs.
 */
int
ttls_mpi_shrink(TlsMpi *X, size_t nblimbs)
{
	unsigned long *p;

	if (WARN_ON_ONCE(!X->p || X->limbs < nblimbs))
		return 0;
	if (X->used > nblimbs)
		nblimbs = X->used;

	if (!(p = kmalloc(nblimbs * CIL, GFP_ATOMIC)))
		return -ENOMEM;

	memcpy(p, X->p, X->used * CIL);
	kfree(X->p);

	X->limbs = nblimbs;
	X->p = p;

	return 0;
}

/**
 * Copy @Y to @X.
 */
int
ttls_mpi_copy(TlsMpi *X, const TlsMpi *Y)
{
	if (unlikely(X == Y))
		return 0;
	if (unlikely(!Y->p)) {
		ttls_mpi_free(X);
		return 0;
	}

	if (X->limbs < Y->used)
		if (__mpi_realloc(X, Y->used, 0))
			return -ENOMEM;
	memcpy(X->p, Y->p, Y->used * CIL);
	X->s = Y->s;
	X->used = Y->used;

	return 0;
}

/**
 * Safe conditional assignment X = Y if @assign is 1.
 *
 * This function avoids leaking any information about whether the assignment was
 * done or not (the above code may leak information through branch prediction
 * and/or memory access patterns analysis). Leaking information about the
 * respective sizes of X and Y is ok however.
 */
int
ttls_mpi_safe_cond_assign(TlsMpi *X, const TlsMpi *Y, unsigned char assign)
{
	int i;

	/* Make sure assign is 0 or 1 in a time-constant manner. */
	assign = (assign | (unsigned char)-assign) >> 7;

	if (ttls_mpi_grow(X, Y->used))
		return -ENOMEM;

	X->s = X->s * (1 - assign) + Y->s * assign;
	X->used = X->used * (1 - assign) + Y->used * assign;

	for (i = 0; i < Y->used; i++)
		X->p[i] = X->p[i] * (1 - assign) + Y->p[i] * assign;

	return 0;
}

/**
 * Conditionally swap X and Y, without leaking information about whether the
 * swap was made or not. Here it is not ok to simply swap the pointers, which
 * would lead to different memory access patterns when X and Y are used
 * afterwards.
 */
int
ttls_mpi_safe_cond_swap(TlsMpi *X, TlsMpi *Y, unsigned char swap)
{
	unsigned short used;
	int s, i;

	if (X == Y)
		return 0;

	/* Make sure swap is 0 or 1 in a time-constant manner. */
	swap = (swap | (unsigned char)-swap) >> 7;

	if (ttls_mpi_grow(X, Y->used))
		return -ENOMEM;
	if (ttls_mpi_grow(Y, X->used))
		return -ENOMEM;

	s = X->s;
	X->s = X->s * (1 - swap) + Y->s * swap;
	Y->s = Y->s * (1 - swap) + s * swap;

	used = X->used;
	X->used = X->used * (1 - swap) + Y->used * swap;
	Y->used = Y->used * (1 - swap) + used * swap;

	used = max_t(unsigned short, X->used, Y->used);
	for (i = 0; i < used; i++) {
		unsigned long tmp = X->p[i];
		X->p[i] = X->p[i] * (1 - swap) + Y->p[i] * swap;
		Y->p[i] = Y->p[i] * (1 - swap) + tmp * swap;
	}

	return 0;
}

/**
 * Set value from integer.
 */
int
ttls_mpi_lset(TlsMpi *X, long z)
{
	if (__mpi_realloc(X, 1, 0))
		return -ENOMEM;

	X->used = 1;
	if (z < 0) {
		X->p[0] = -z;
		X->s = -1;
	} else {
		X->p[0] = z;
		X->s = 1;
	}

	return 0;
}

int
ttls_mpi_get_bit(const TlsMpi *X, size_t pos)
{
	if ((X->used << BSHIFT) <= pos)
		return 0;

	return (X->p[pos >> BSHIFT] >> (pos & BMASK)) & 0x01;
}

/**
 * Set a bit to a specific value of 0 or 1.
 *
 * Will grow X if necessary to set a bit to 1 in a not yet existing limb.
 * Will not grow if bit should be set to 0.
 */
int
ttls_mpi_set_bit(TlsMpi *X, size_t pos, unsigned char val)
{
	size_t off = pos >> BSHIFT;
	size_t idx = pos & BMASK;

	WARN_ON_ONCE(val != 0 && val != 1);

	if (X->used << BSHIFT <= pos) {
		if (!val)
			return 0;
		if (unlikely(X->limbs << BSHIFT <= pos))
			if (ttls_mpi_grow(X, off + 1))
				return -ENOMEM;
		memset(&X->p[X->used], 0, (off - X->used + 1) << LSHIFT);
		X->used = off + 1;
	}

	X->p[off] &= ~((unsigned long)0x01 << idx);
	X->p[off] |= (unsigned long)val << idx;

	return 0;
}

/**
 * Return the number of less significant zero-bits, which is equal to the
 * position of the first less significant bit.
 *
 * WARNING: this doesn't work with ttls_mpi_set_bit() called with @pos out of
 * X->used and value 0.
 */
size_t
ttls_mpi_lsb(const TlsMpi *X)
{
	size_t i;

	for (i = 0 ; i < X->used; i++) {
		if (!X->p[i])
			continue;
		return (i * BIL) + __ffs(X->p[i]);
	}

	return 0;
}

size_t
ttls_mpi_bitlen(const TlsMpi *X)
{
	if (!X->used || !X->p[X->used - 1])
		return 0;

	/*
	 * Number of full limbs plus number of less significant non-zero bits.
	 */
	return (X->used - 1) * BIL + fls64(X->p[X->used - 1]);
}

/*
 * Return the total size in bytes
 */
size_t
ttls_mpi_size(const TlsMpi *X)
{
	return (ttls_mpi_bitlen(X) + 7) >> 3;
}

/**
 * Left-shift: X <<= count.
 *
 * TODO #1064 stupid 2*n algorithm, do this in one shot.
 */
int
ttls_mpi_shift_l(TlsMpi *X, size_t count)
{
	size_t v0, t1, old_used = X->used, i = ttls_mpi_bitlen(X);
	unsigned long r0 = 0, r1, *p = X->p;

	if (unlikely(!i))
		return 0;

	v0 = count >> BSHIFT;
	t1 = count & BMASK;
	i += count;

	if ((X->limbs << BSHIFT) < i) {
		if (!(p = kmalloc(BITS_TO_LIMBS(i) * CIL, GFP_ATOMIC)))
			return -ENOMEM;
	}
	X->used = BITS_TO_LIMBS(i);

	/* Shift by count / limb_size. */
	if (v0 > 0) {
		for (i = X->used; i > v0; i--)
			p[i - 1] = X->p[i - v0 - 1];
		for ( ; i > 0; i--)
			p[i - 1] = 0;
	}
	else if (X->p != p) {
		memcpy(p, X->p, old_used * CIL);
		/* Not more than 1 limb. */
		memset(&p[old_used], 0, CIL);
	}

	/* shift by count % limb_size. */
	if (t1 > 0) {
		for (i = v0; i < X->used; i++) {
			r1 = p[i] >> (BIL - t1);
			p[i] <<= t1;
			p[i] |= r0;
			r0 = r1;
		}
	}

	if (X->p != p) {
		kfree(X->p);
		X->p = p;
		X->limbs = X->used;
	}

	return 0;
}

/**
 * Right-shift: X >>= count.
 *
 * TODO #1064 stupid 2*n algorithm, do this in one shot.
 */
int
ttls_mpi_shift_r(TlsMpi *X, size_t count)
{
	size_t i, v0, v1;
	unsigned long r0 = 0, r1;

	if (unlikely(!X->used || !X->p[X->used - 1])) {
		WARN_ON_ONCE(X->used > 1);
		return 0;
	}

	v0 = count >> BSHIFT;
	v1 = count & BMASK;

	if (v0 > X->used || (v0 == X->used && v1 > 0))
		return ttls_mpi_lset(X, 0);

	/*
	 * Shift by count / limb_size - remove least significant limbs.
	 * There could be garbage after last used limb, so be careful.
	 */
	if (v0 > 0) {
		X->used -= v0;
		for (i = 0; i < X->used; i++)
			X->p[i] = X->p[i + v0];
	}

	/* Shift by count % limb_size. */
	if (v1 > 0) {
		for (i = X->used; i > 0; i--) {
			r1 = X->p[i - 1] << (BIL - v1);
			X->p[i - 1] >>= v1;
			X->p[i - 1] |= r0;
			r0 = r1;
		}
		if (!X->p[X->used - 1])
			--X->used;
	}

	return 0;
}

/**
 * Dump MPI content, including unused limbs, for debugging.
 */
bool __mpi_do_dump = false;

void
ttls_mpi_dump(const TlsMpi *X, const char *prefix)
{
	if (!__mpi_do_dump)
		return;

	pr_info("MPI(%pK, p=%pK) %s DUMP: s=%d used=%u limbs=%u\n",
		X, X->p, prefix, X->s, X->used, X->limbs);
	print_hex_dump(KERN_INFO, "    ", DUMP_PREFIX_OFFSET, 16, 1, X->p,
		       X->limbs * sizeof(long), true);
}

/**
 * Import X from unsigned binary data.
 * The bytes are read in reverse order and stored as big endian.
 */
int
ttls_mpi_read_binary(TlsMpi *X, const unsigned char *buf, size_t buflen)
{
	size_t i = buflen, l = 0, j;
	size_t const limbs = CHARS_TO_LIMBS(buflen);

	if (X->limbs < limbs) {
		unsigned long *p;

		if (!(p = kmalloc(limbs * CIL, GFP_ATOMIC)))
			return -ENOMEM;
		kfree(X->p);
		X->p = p;
		X->limbs = limbs;
	}

	X->s = 1;
	while (i >= CIL) {
		i -= CIL;
		X->p[l] = cpu_to_be64(*(long *)(buf + i));
		++l;
	}
	if (i) {
		/* Read last, probably incomplete, limb if any. */
		X->p[l] = 0;
		for (j = 0; i > 0; i--, j += 8)
			X->p[l] |= ((unsigned long)buf[i - 1]) << j;
	}

	mpi_fixup_used(X, limbs);

	return 0;
}

/**
 * Export X into unsigned binary data, big endian.
 * Always fills the whole buffer, which will start with zeros if the number
 * is smaller.
 */
int
ttls_mpi_write_binary(const TlsMpi *X, unsigned char *buf, size_t buflen)
{
	size_t i = buflen, j, n;

	n = ttls_mpi_size(X);

	if (buflen < n)
		return -ENOSPC;

	/* TODO #1064 use cpu_to_be64() for bytes inverted write. */
	for (j = 0; n > 0; i--, j++, n--)
		buf[i - 1] = (unsigned char)(X->p[j / CIL] >> ((j % CIL) << 3));
	memset(buf, 0, i);

	return 0;
}

/**
 * Fill X with @size bytes of random.
 *
 * Use a temporary bytes representation to make sure the result is the same
 * regardless of the platform endianness (useful when f_rng is actually
 * deterministic, eg for tests).
 */
int
ttls_mpi_fill_random(TlsMpi *X, size_t size)
{
	size_t limbs = CHARS_TO_LIMBS(size);
	size_t rem = limbs * CIL - size;

	if (WARN_ON_ONCE(size > TTLS_MPI_MAX_SIZE))
		return -EINVAL;

	if (__mpi_realloc(X, limbs, 0))
		return -ENOMEM;

	ttls_rnd(X->p, size);
	if (rem > 0)
		memset((char *)X->p + size, 0, rem);
	X->used = limbs;
	X->s = 1;

	return 0;
}

/**
 * Compare unsigned values.
 */
int
ttls_mpi_cmp_abs(const TlsMpi *X, const TlsMpi *Y)
{
	int i;

	if (!X->used && !Y->used)
		return 0;

	if (X->used > Y->used)
		return 1;
	if (Y->used > X->used)
		return -1;

	for (i = X->used - 1; i >= 0; i--) {
		if (X->p[i] == Y->p[i])
			continue;
		return X->p[i] > Y->p[i] ? 1 : -1;
	}

	return 0;
}

/*
 * Compare signed values.
 */
int
ttls_mpi_cmp_mpi(const TlsMpi *X, const TlsMpi *Y)
{
	int i;

	if (!X->used && !Y->used)
		return 0;

	if (X->used > Y->used)
		return X->s;
	if (Y->used > X->used)
		return -Y->s;

	if (X->s > 0 && Y->s < 0)
		return 1;
	if (Y->s > 0 && X->s < 0)
		return -1;

	for (i = X->used - 1; i >= 0; i--) {
		if (X->p[i] == Y->p[i])
			continue;
		return X->p[i] > Y->p[i] ? X->s : -X->s;
	}

	return 0;
}

/**
 * Compare MPI with a signed value.
 */
int
ttls_mpi_cmp_int(const TlsMpi *X, long z)
{
	if (X->used > 1)
		return X->s;
	if (!X->used)
		return z == 0 ? 0 : z < 0 ? 1 : -1;

	if (z < 0) {
		if (X->s > 0)
			return 1;
		z = -z;
	} else {
		if (X->s < 0)
			return -1;
	}

	/* Modular comparison. */
	return z == X->p[0] ? 0 : X->p[0] > z ? X->s : -X->s;
}

/**
 * Unsigned addition: X = |A| + |B|
 *
 * @A and @B must be different, but either of them can accept the result @X.
 */
int
ttls_mpi_add_abs(TlsMpi *X, const TlsMpi *A, const TlsMpi *B)
{
	size_t i, n;
	unsigned long *a, *b, *x, c = 0;

	BUG_ON(A == B);
	if (X == B) {
		const TlsMpi *T = A;
		A = X;
		B = T;
	}

	/* X should always be positive as a result of unsigned additions. */
	X->s = 1;

	n = max_t(unsigned short, A->used, B->used);
	if ((A->used > B->used && (A->p[n - 1] & (1UL << 63)))
	    || (B->used > A->used && (B->p[n - 1] & (1UL << 63)))
	    || (A->used == B->used && (A->p[n - 1] | B->p[n - 1])
				      & (1UL << 63)))
	{
		++n;
	}
	if (__mpi_realloc(X, n, X == A ? MPI_GROW_COPY : 0))
		return -ENOMEM;
	X->used = A->used;

	a = A->p;
	b = B->p;
	x = X->p;
	/* TODO #1064 move out condition from under the loop. */
	for (i = 0; i < B->used; i++, a++, b++, x++) {
		if (i == X->used) {
			++X->used;
			*x = c;
		} else {
			*x = *a + c;
		}
		c = *x < c;
		*x += *b;
		c += *x < *b;
	}
	for ( ; c; i++, a++, x++) {
		BUG_ON(i >= X->limbs);
		if (i == X->used) {
			++X->used;
			*x = c;
		} else {
			*x = *a + c;
		}
		c = *x < c;
	}
	if (X != A && X->used > i)
		memcpy(x, a, (X->used - i) * CIL);

	return 0;
}

/**
 * Subtract @b from @a and write result to @r, @a_len > @b_len.
 * Either @a or @b can be referenced by @r.
 */
static void
__mpi_sub(unsigned long *a, size_t a_len, unsigned long *b, size_t b_len,
	  unsigned long *r)
{
	unsigned long c = 0, z, b_tmp, *b_end = b + b_len, *a_end = a + a_len;

	BUG_ON(a_len < b_len);

	for ( ; b < b_end; a++, b++, r++) {
		z = *a < c;
		b_tmp = *b;
		*r = *a - c;
		c = (*r < b_tmp) + z;
		*r -= b_tmp;
	}
	while (c) {
		z = *a < c;
		*r = *a - c;
		c = z;
		a++;
		r++;
	}
	BUG_ON(a > a_end);
	memcpy(r, a, (a_end - a) * CIL);
}

/**
 * Unsigned subtraction: X = |A| - |B| (HAC 14.9).
 * @X may reference either @A or @B.
 */
int
ttls_mpi_sub_abs(TlsMpi *X, const TlsMpi *A, const TlsMpi *B)
{
	unsigned long *p = X->p;

	if (ttls_mpi_cmp_abs(A, B) < 0)
		return -EINVAL;

	if (X->limbs < A->used) {
		if (!(p = kmalloc(A->used * CIL, GFP_ATOMIC)))
			return -ENOMEM;
	}

	__mpi_sub(A->p, A->used, B->p, B->used, p);

	/* X should always be positive as a result of unsigned subtractions. */
	X->s = 1;
	if (p != X->p) {
		kfree(X->p);
		X->p = p;
		X->limbs = A->used;
	}
	mpi_fixup_used(X, A->used);

	return 0;
}

/**
 * Signed addition: X = A + B
 */
int
ttls_mpi_add_mpi(TlsMpi *X, const TlsMpi *A, const TlsMpi *B)
{
	int r, s = A->s;

	if (A->s * B->s < 0) {
		if (ttls_mpi_cmp_abs(A, B) >= 0) {
			if ((r = ttls_mpi_sub_abs(X, A, B)))
				return r;
			X->s = s;
		} else {
			if ((r = ttls_mpi_sub_abs(X, B, A)))
				return r;
			X->s = -s;
		}
	} else {
		if ((r = ttls_mpi_add_abs(X, A, B)))
			return r;
		X->s = s;
	}

	return 0;
}

/**
 * Signed subtraction: X = A - B
 */
int
ttls_mpi_sub_mpi(TlsMpi *X, const TlsMpi *A, const TlsMpi *B)
{
	int r, s = A->s;

	if (A->s * B->s > 0) {
		if (ttls_mpi_cmp_abs(A, B) >= 0) {
			if ((r = ttls_mpi_sub_abs(X, A, B)))
				return r;
			X->s = s;
		} else {
			if ((r = ttls_mpi_sub_abs(X, B, A)))
				return r;
			X->s = -s;
		}
	} else {
		if ((r = ttls_mpi_add_abs(X, A, B)))
			return r;
		X->s = s;
	}

	return 0;
}

/**
 * Signed addition: X = A + b
 */
int
ttls_mpi_add_int(TlsMpi *X, const TlsMpi *A, long b)
{
	TlsMpi _B;
	unsigned long p[1];

	p[0] = (b < 0) ? -b : b;
	_B.s = (b < 0) ? -1 : 1;
	_B.limbs = _B.used = 1;
	_B.p = p;

	return ttls_mpi_add_mpi(X, A, &_B);
}

/**
 * Signed subtraction: X = A - b
 */
int
ttls_mpi_sub_int(TlsMpi *X, const TlsMpi *A, long b)
{
	TlsMpi _B;
	unsigned long p[1];

	p[0] = (b < 0) ? -b : b;
	_B.s = (b < 0) ? -1 : 1;
	_B.limbs = _B.used = 1;
	_B.p = p;

	return ttls_mpi_sub_mpi(X, A, &_B);
}

/*
 * TODO #1064 see MULADDC_HUIT optimization in original mbedTLS; use AVX2.
 */
#define MULADDC_INIT							\
	asm(	"xorq	%%r8, %%r8	\n\t"

#define MULADDC_CORE							\
		"movq	(%%rsi), %%rax	\n\t"				\
		"mulq	%%rbx		\n\t"				\
		"addq	$8, %%rsi	\n\t"				\
		"addq	%%rcx, %%rax	\n\t"				\
		"movq	%%r8, %%rcx	\n\t"				\
		"adcq	$0, %%rdx	\n\t"				\
		"nop			\n\t"				\
		"addq	%%rax, (%%rdi)	\n\t"				\
		"adcq	%%rdx, %%rcx	\n\t"				\
		"addq	$8, %%rdi	\n\t"

#define MULADDC_STOP							\
		: "+c" (c), "+D" (d), "+S" (s)				\
		: "b" (b)						\
		: "rax", "rdx", "r8"					\
	);

/**
 * Multiplies vector @s of size @n by scalar @b and stores result in vector @d.
 */
static void
__mpi_mul(size_t n, const unsigned long *s, unsigned long *d, unsigned long b)
{
	unsigned long c = 0;

	for ( ; n >= 16; n -= 16) {
		MULADDC_INIT
		MULADDC_CORE MULADDC_CORE
		MULADDC_CORE MULADDC_CORE
		MULADDC_CORE MULADDC_CORE
		MULADDC_CORE MULADDC_CORE

		MULADDC_CORE MULADDC_CORE
		MULADDC_CORE MULADDC_CORE
		MULADDC_CORE MULADDC_CORE
		MULADDC_CORE MULADDC_CORE
		MULADDC_STOP
	}
	for ( ; n >= 8; n -= 8) {
		MULADDC_INIT
		MULADDC_CORE MULADDC_CORE
		MULADDC_CORE MULADDC_CORE

		MULADDC_CORE MULADDC_CORE
		MULADDC_CORE MULADDC_CORE
		MULADDC_STOP
	}
	for ( ; n > 0; n--) {
		MULADDC_INIT
		MULADDC_CORE
		MULADDC_STOP
	}

	do {
		*d += c;
		c = *d < c;
		d++;
	} while (c);
}

/**
 * Baseline multiplication: X = A * B  (HAC 14.12).
 *
 * All the arguments may reference the same MPI.
 */
int
ttls_mpi_mul_mpi(TlsMpi *X, const TlsMpi *A, const TlsMpi *B)
{
	int r = 0;
	size_t i = A->used, j = B->used;
	TlsMpi T;

	if (X == A) {
		ttls_mpi_init(&T);
		if (ttls_mpi_copy(&T, A))
			return -ENOMEM;
		if (A == B)
			B = &T;
		A = &T;
	}
	else if (X == B) {
		ttls_mpi_init(&T);
		if (ttls_mpi_copy(&T, B))
			return -ENOMEM;
		B = &T;
	}

	if ((r = __mpi_realloc(X, i + j, 0)))
		goto cleanup;
	memset(X->p, 0, CIL * (i + j));
	X->used = i + j;

	for ( ; j > 0; j--)
		__mpi_mul(i, A->p, X->p + j - 1, B->p[j - 1]);

	mpi_fixup_used(X, X->used);

	X->s = A->s * B->s;

cleanup:
	if (A == &T || B == &T)
		ttls_mpi_free(&T);

	return r;
}

/*
 * Baseline multiplication: X = A * b
 */
int
ttls_mpi_mul_uint(TlsMpi *X, const TlsMpi *A, unsigned long b)
{
	TlsMpi _B;
	unsigned long p[1];

	_B.s = 1;
	_B.limbs = _B.used = 1;
	_B.p = p;
	p[0] = b;

	return ttls_mpi_mul_mpi(X, A, &_B);
}

/**
 * Unsigned integer divide - double unsigned long dividend, @u1/@u0,
 * and unsigned long divisor, @d.
 */
static unsigned long
ttls_int_div_int(unsigned long u1, unsigned long u0, unsigned long d,
		 unsigned long *r)
{
	const unsigned long radix = 1UL << BIH;
	const unsigned long uint_halfword_mask = (1UL << BIH) - 1;
	unsigned long d0, d1, q0, q1, rAX, r0;
	unsigned long u0_msw, u0_lsw;
	size_t s;

	/* Check for overflow. */
	if (!d || u1 >= d) {
		if (r)
			*r = ~0UL;
		return ~0UL;
	}

	/*
	 * Algorithm D, Section 4.3.1 - The Art of Computer Programming
	 *   Vol. 2 - Seminumerical Algorithms, Knuth.
	 */

	/* Normalize the divisor, d, and dividend, u0, u1. */
	s = BIL - fls64(d);
	d = d << s;

	u1 = u1 << s;
	u1 |= (u0 >> (BIL - s)) & (-(long)s >> (BIL - 1));
	u0 =  u0 << s;

	d1 = d >> BIH;
	d0 = d & uint_halfword_mask;

	u0_msw = u0 >> BIH;
	u0_lsw = u0 & uint_halfword_mask;

	/* Find the first quotient and remainder. */
	q1 = u1 / d1;
	r0 = u1 - d1 * q1;

	while (q1 >= radix || (q1 * d0 > radix * r0 + u0_msw)) {
		--q1;
		r0 += d1;
		if (r0 >= radix)
			break;
	}

	rAX = (u1 * radix) + (u0_msw - q1 * d);
	q0 = rAX / d1;
	r0 = rAX - q0 * d1;

	while (q0 >= radix || (q0 * d0 > radix * r0 + u0_lsw)) {
		--q0;
		r0 += d1;
		if (r0 >= radix)
			break;
	}

	if (r)
		*r = (rAX * radix + u0_lsw - q0 * d) >> s;

	return q1 * radix + q0;
}

/**
 * Division by TlsMpi: A = Q * B + R  (HAC 14.20).
 *
 * @Q - destination MPI for the quotient.
 * @R - destination MPI for the rest value.
 * @A - left-hand MPI.
 * @B - right-hand MPI.
 */
int
ttls_mpi_div_mpi(TlsMpi *Q, TlsMpi *R, const TlsMpi *A, const TlsMpi *B)
{
	int r = -ENOMEM;
	size_t i, n, t, k;
	TlsMpi X, Y, Z, T1, T2;
	unsigned long __tp2[3];

	if (!ttls_mpi_cmp_int(B, 0)) {
		T_DBG_MPI1("Division by zero", B);
		TTLS_MPI_DUMP_ONCE(B, "B/zero");
		return -EINVAL;
	}
	if (!ttls_mpi_cmp_int(B, 1)) {
		if (Q)
			if (ttls_mpi_copy(Q, A))
				return -ENOMEM;
		if (R)
			if (ttls_mpi_lset(R, 0))
				return -ENOMEM;
		return 0;
	}
	if (ttls_mpi_cmp_abs(A, B) < 0) {
		if (Q)
			if (ttls_mpi_lset(Q, 0))
				return -ENOMEM;
		if (R)
			if (ttls_mpi_copy(R, A))
				return -ENOMEM;
		return 0;
	}

	if (!Q) {
		ttls_mpi_init(&Z);
		Q = &Z;
	}

	ttls_mpi_init(&X);
	ttls_mpi_init(&Y);
	ttls_mpi_init(&T1);
	/* Avoid dynamic memory allocations for T2. */
	T2.p = __tp2;
	T2.limbs = 3;

	if (ttls_mpi_copy(&X, A) || ttls_mpi_copy(&Y, B)
	    || __mpi_realloc(&T1, 2, 0))
		goto cleanup;
	X.s = Y.s = 1;

	/* Initialize Q after copying A to X in case of Q == A. */
	if (__mpi_realloc(Q, A->used, 0))
		goto cleanup;
	Q->used = A->used;
	memset(Q->p, 0, Q->used * CIL);

	k = ttls_mpi_bitlen(&Y) & BMASK;
	if (k < BIL - 1) {
		k = BIL - 1 - k;
		if (ttls_mpi_shift_l(&X, k) || ttls_mpi_shift_l(&Y, k))
			goto cleanup;
	}
	else {
		k = 0;
	}

	n = X.used - 1;
	t = Y.used - 1;

	if (ttls_mpi_shift_l(&Y, BIL * (n - t)))
		goto cleanup;
	while (ttls_mpi_cmp_mpi(&X, &Y) >= 0) {
		Q->p[n - t]++;
		if (ttls_mpi_sub_mpi(&X, &X, &Y))
			goto cleanup;
	}
	if (ttls_mpi_shift_r(&Y, BIL * (n - t)))
		goto cleanup;

	for (i = n; i > t; i--) {
		Q->p[i - t - 1] = X.p[i] >= Y.p[t]
				  ? 0
				  : ttls_int_div_int(X.p[i], X.p[i - 1],
						     Y.p[t], NULL) + 1;

		T2.s = 1;
		T2.p[0] = (i < 2) ? 0 : X.p[i - 2];
		T2.p[1] = (i < 1) ? 0 : X.p[i - 1];
		T2.p[2] = X.p[i];
		mpi_fixup_used(&T2, 3);

		/*
		 * TODO #1064 inadequately many iterations - use binary search
		 * for Q->p[i - t - 1] value.
		 */
		do {
			Q->p[i - t - 1]--;

			T1.s = 1;
			T1.used = 2; /* overwrite previous multiplication */
			T1.p[0] = (t < 1) ? 0 : Y.p[t - 1];
			T1.p[1] = Y.p[t];
			mpi_fixup_used(&T1, 2);
			if (ttls_mpi_mul_uint(&T1, &T1, Q->p[i - t - 1]))
				goto cleanup;
		} while (ttls_mpi_cmp_mpi(&T1, &T2) > 0);

		if (ttls_mpi_mul_uint(&T1, &Y, Q->p[i - t - 1])
		    || ttls_mpi_shift_l(&T1,  BIL * (i - t - 1))
		    || ttls_mpi_sub_mpi(&X, &X, &T1))
			goto cleanup;

		if (ttls_mpi_cmp_int(&X, 0) < 0) {
			if (ttls_mpi_copy(&T1, &Y)
			    || ttls_mpi_shift_l(&T1, BIL * (i - t - 1))
			    || ttls_mpi_add_mpi(&X, &X, &T1))
				goto cleanup;
			Q->p[i - t - 1]--;
		}
	}

	if (Q != &Z) {
		Q->s = A->s * B->s;
		mpi_fixup_used(Q, Q->used);
	}
	if (R) {
		if (ttls_mpi_shift_r(&X, k))
			goto cleanup;
		mpi_fixup_used(&X, X.used);
		X.s = A->s;
		if (ttls_mpi_copy(R, &X))
			goto cleanup;
		if (ttls_mpi_cmp_int(R, 0) == 0)
			R->s = 1;
	}

	r = 0;
cleanup:
	ttls_mpi_free(&X);
	ttls_mpi_free(&Y);
	ttls_mpi_free(&T1);
	if (Q == &Z)
		ttls_mpi_free(&Z); /* caller don't need the quotient */

	return r;
}

/**
 * Modulo: R = A mod B.
 *
 * @R - destination MPI for the rest value.
 * @A - left-hand MPI.
 * @B - right-hand MPI.
 */
int
ttls_mpi_mod_mpi(TlsMpi *R, const TlsMpi *A, const TlsMpi *B)
{
	int r;

	if (ttls_mpi_cmp_int(B, 0) < 0) {
		T_DBG_MPI1("Negative modulo", B);
		return -EINVAL;
	}

	if ((r = ttls_mpi_div_mpi(NULL, R, A, B)))
		return r;

	while (ttls_mpi_cmp_int(R, 0) < 0)
		if ((r = ttls_mpi_add_mpi(R, R, B)))
			return r;

	while (ttls_mpi_cmp_mpi(R, B) >= 0)
		if ((r = ttls_mpi_sub_mpi(R, R, B)))
			return r;

	return 0;
}

/**
 * Fast Montgomery initialization (thanks to Tom St Denis).
 */
static void
__mpi_montg_init(unsigned long *mm, const TlsMpi *N)
{
	unsigned long x, m0 = N->p[0];
	unsigned int i;

	x = m0;
	x += ((m0 + 2) & 4) << 1;

	for (i = BIL; i >= 8; i /= 2)
		x *= 2 - (m0 * x);

	*mm = ~x + 1;
}

/**
 * Montgomery multiplication: A = A * B * R^-1 mod N  (HAC 14.36).
 */
static int
__mpi_montmul(TlsMpi *A, const TlsMpi *B, const TlsMpi *N, unsigned long mm,
	      TlsMpi *T)
{
	size_t i, n, m;
	unsigned long u0, u1, *d;

	BUG_ON(T->limbs < N->used + 1 || !T->p);
	memset(T->p, 0, T->limbs * CIL);

	d = T->p;
	n = N->used;
	m = (B->used < n) ? B->used : n;

	for (i = 0; i < n; i++) {
		/* T = (T + u0*B + u1*N) / 2^BIL */
		u0 = A->p[i];
		u1 = (d[0] + u0 * B->p[0]) * mm;

		__mpi_mul(m, B->p, d, u0);
		__mpi_mul(n, N->p, d, u1);

		*d++ = u0;
		d[n + 1] = 0;
	}
	mpi_fixup_used(T, T->limbs);

	memcpy(A->p, d, (n + 1) * CIL);
	mpi_fixup_used(A, n + 1);

	if (ttls_mpi_cmp_abs(A, N) >= 0) {
		__mpi_sub(A->p, A->used, N->p, N->used, A->p);
		mpi_fixup_used(A, A->used);
	} else {
		/* Prevent timing attacks. */
		__mpi_sub(T->p, T->used, A->p, A->used, T->p);
		mpi_fixup_used(T, T->used);
	}

	return 0;
}

/**
 * Montgomery reduction: A = A * R^-1 mod N
 */
static int
__mpi_montred(TlsMpi *A, const TlsMpi *N, unsigned long mm, TlsMpi *T)
{
	unsigned long z = 1;
	TlsMpi U;

	U.s = 1;
	U.limbs = U.used = 1;
	U.p = &z;

	return __mpi_montmul(A, &U, N, mm, T);
}

/**
 * Sliding-window exponentiation: X = A^E mod N  (HAC 14.85).
 *
 * @X	- destination MPI;
 * @A	- left-hand MPI
 * @E	- exponent MPI;
 * @N	- modular MPI
 * @RR	- speed-up MPI used for recalculations.
 *
 * @RR is used to avoid re-computing R * R mod N across multiple calls,
 * which speeds up things a bit.
 */
int
ttls_mpi_exp_mod(TlsMpi *X, const TlsMpi *A, const TlsMpi *E, const TlsMpi *N,
		 TlsMpi *RR)
{
	int r = -ENOMEM, neg;
	size_t i, j, nblimbs, bufsize = 0, nbits = 0, wbits = 0, wsize, one = 1;
	unsigned long ei, mm, state = 0;
	TlsMpi T, Apos, *W = *this_cpu_ptr(&g_buf);

	BUILD_BUG_ON(MPI_W_SZ < 6);
	if (ttls_mpi_cmp_int(N, 0) <= 0 || !(N->p[0] & 1))
		return -EINVAL;
	if (ttls_mpi_cmp_int(E, 0) < 0)
		return -EINVAL;

	/* Init temps and window size. */
	__mpi_montg_init(&mm, N);
	ttls_mpi_init(&T);
	ttls_mpi_init(&Apos);
	memset(W, 0, sizeof(TlsMpi) << MPI_W_SZ);

	i = ttls_mpi_bitlen(E);
	wsize = (i > 671) ? 6
		: (i > 239) ? 5
		  : (i >  79) ? 4
		    : (i >  23) ? 3
		      : 1;

	j = N->used + 1;
	if (ttls_mpi_grow(X, j)
	    || __mpi_realloc(&W[1], j, 0)
	    || __mpi_realloc(&T, j * 2, 0))
		goto cleanup;

	/* Compensate for negative A (and correct at the end). */
	neg = (A->s == -1);
	if (neg) {
		if ((r = ttls_mpi_copy(&Apos, A)))
			goto cleanup;
		Apos.s = 1;
		A = &Apos;
	}

	/*
	 * If 1st call, pre-compute R^2 mod N
	 */
	BUG_ON(!RR);
	if (!RR->p) {
		if ((r = ttls_mpi_lset(RR, 1))
		    || (r = ttls_mpi_shift_l(RR, N->used * 2 * BIL))
		    || (r = ttls_mpi_mod_mpi(RR, RR, N)))
			goto cleanup;
	}

	/* W[1] = A * R^2 * R^-1 mod N = A * R mod N */
	if (ttls_mpi_cmp_mpi(A, N) >= 0) {
		if ((r = ttls_mpi_mod_mpi(&W[1], A, N)))
			goto cleanup;
	} else {
		if ((r = ttls_mpi_copy(&W[1], A)))
			goto cleanup;
	}

	if ((r = __mpi_montmul(&W[1], RR, N, mm, &T)))
		goto cleanup;

	/* X = R^2 * R^-1 mod N = R mod N */
	if ((r = ttls_mpi_copy(X, RR))
	    || (r = __mpi_montred(X, N, mm, &T)))
		goto cleanup;

	if (wsize > 1) {
		/* W[1 << (wsize - 1)] = W[1] ^ (wsize - 1) */
		j =  one << (wsize - 1);

		if ((r = ttls_mpi_grow(&W[j], N->used + 1))
		    || (r = ttls_mpi_copy(&W[j], &W[1])))
			goto cleanup;

		for (i = 0; i < wsize - 1; i++)
			if ((r = __mpi_montmul(&W[j], &W[j], N, mm, &T)))
				goto cleanup;

		/* W[i] = W[i - 1] * W[1] */
		for (i = j + 1; i < (one << wsize); i++) {
			if ((r = ttls_mpi_grow(&W[i], N->used + 1))
			    || (r = ttls_mpi_copy(&W[i], &W[i - 1]))
			    || (r = __mpi_montmul(&W[i], &W[1], N, mm, &T)))
				goto cleanup;
		}
	}

	nblimbs = E->used;
	while (1) {
		if (!bufsize) {
			if (!nblimbs)
				break;
			nblimbs--;
			bufsize = sizeof(unsigned long) << 3;
		}

		bufsize--;

		ei = (E->p[nblimbs] >> bufsize) & 1;

		/* Skip leading 0s. */
		if (!ei && !state)
			continue;

		if (!ei && state == 1) {
			/* Out of window, square X. */
			if ((r = __mpi_montmul(X, X, N, mm, &T)))
				goto cleanup;
			continue;
		}

		/* Add ei to current window. */
		state = 2;

		nbits++;
		wbits |= (ei << (wsize - nbits));

		if (nbits == wsize) {
			/* X = X^wsize R^-1 mod N . */
			for (i = 0; i < wsize; i++)
				if ((r = __mpi_montmul(X, X, N, mm, &T)))
					goto cleanup;

			/* X = X * W[wbits] R^-1 mod N. */
			if ((r = __mpi_montmul(X, &W[wbits], N, mm, &T)))
				goto cleanup;

			state--;
			nbits = 0;
			wbits = 0;
		}
	}

	/* Process the remaining bits. */
	for (i = 0; i < nbits; i++) {
		if ((r = __mpi_montmul(X, X, N, mm, &T)))
			goto cleanup;

		wbits <<= 1;
		if (wbits & (one << wsize))
			if ((r = __mpi_montmul(X, &W[1], N, mm, &T)))
				goto cleanup;
	}

	/* X = A^E * R * R^-1 mod N = A^E mod N. */
	if ((r = __mpi_montred(X, N, mm, &T)))
		goto cleanup;

	if (neg && E->used && (E->p[0] & 1)) {
		X->s = -1;
		if ((r = ttls_mpi_add_mpi(X, N, X)))
			goto cleanup;
	}

cleanup:
	for (i = (one << (wsize - 1)); i < (one << wsize); i++)
		ttls_mpi_free(&W[i]);
	ttls_mpi_free(&W[1]);
	ttls_mpi_free(&T);
	ttls_mpi_free(&Apos);

	return r;
}

/**
 * Greatest common divisor: G = gcd(A, B)  (HAC 14.54)
 */
int
ttls_mpi_gcd(TlsMpi *G, const TlsMpi *A, const TlsMpi *B)
{
	int r;
	size_t lz, lzt;
	TlsMpi TA, TB;

	ttls_mpi_init(&TA);
	ttls_mpi_init(&TB);
	if (ttls_mpi_copy(&TA, A)
	    || ttls_mpi_copy(&TB, B))
		return -ENOMEM;

	lz = ttls_mpi_lsb(A);
	lzt = ttls_mpi_lsb(B);
	if (lzt < lz)
		lz = lzt;

	if ((r = ttls_mpi_shift_r(&TA, lz))
	    || (r = ttls_mpi_shift_r(&TB, lz)))
		goto cleanup;

	TA.s = TB.s = 1;

	while (ttls_mpi_cmp_int(&TA, 0)) {
		if ((r = ttls_mpi_shift_r(&TA, ttls_mpi_lsb(&TA)))
		    || (r = ttls_mpi_shift_r(&TB, ttls_mpi_lsb(&TB))))
			goto cleanup;

		if (ttls_mpi_cmp_mpi(&TA, &TB) >= 0) {
			if ((r = ttls_mpi_sub_abs(&TA, &TA, &TB))
			    || (r = ttls_mpi_shift_r(&TA, 1)))
				goto cleanup;
		} else {
			if ((r = ttls_mpi_sub_abs(&TB, &TB, &TA))
			    || (r = ttls_mpi_shift_r(&TB, 1)))
				goto cleanup;
		}
	}

	if ((r = ttls_mpi_shift_l(&TB, lz)))
		goto cleanup;
	r = ttls_mpi_copy(G, &TB);

cleanup:
	ttls_mpi_free(&TA);
	ttls_mpi_free(&TB);

	return r;
}

/**
 * Modular inverse: X = A^-1 mod N  (HAC 14.61 / 14.64)
 */
int
ttls_mpi_inv_mod(TlsMpi *X, const TlsMpi *A, const TlsMpi *N)
{
	int r;
	TlsMpi G, TA, TU, U1, U2, TB, TV, V1, V2;

	if (ttls_mpi_cmp_int(N, 1) <= 0)
		return -EINVAL;

	ttls_mpi_init(&TA);
	ttls_mpi_init(&TU);
	ttls_mpi_init(&U1);
	ttls_mpi_init(&U2);
	ttls_mpi_init(&G);
	ttls_mpi_init(&TB);
	ttls_mpi_init(&TV);
	ttls_mpi_init(&V1);
	ttls_mpi_init(&V2);

	if ((r = ttls_mpi_gcd(&G, A, N)))
		goto cleanup;

	if (ttls_mpi_cmp_int(&G, 1)) {
		r = -EINVAL;
		goto cleanup;
	}

	if ((r = ttls_mpi_mod_mpi(&TA, A, N))
	    || (r = ttls_mpi_copy(&TU, &TA))
	    || (r = ttls_mpi_copy(&TB, N))
	    || (r = ttls_mpi_copy(&TV, N)))
		goto cleanup;

	if ((r = ttls_mpi_lset(&U1, 1))
	    || (r = ttls_mpi_lset(&U2, 0))
	    || (r = ttls_mpi_lset(&V1, 0))
	    || (r = ttls_mpi_lset(&V2, 1)))
		goto cleanup;

	do {
		while (!(TU.p[0] & 1)) {
			if ((r = ttls_mpi_shift_r(&TU, 1)))
				goto cleanup;

			if ((U1.p[0] & 1) || (U2.p[0] & 1)) {
				if ((r = ttls_mpi_add_mpi(&U1, &U1, &TB))
				    || (r = ttls_mpi_sub_mpi(&U2, &U2, &TA)))
					goto cleanup;
			}
			if ((r = ttls_mpi_shift_r(&U1, 1))
			    || (r = ttls_mpi_shift_r(&U2, 1)))
				goto cleanup;
		}

		while (!(TV.p[0] & 1)) {
			if ((r = ttls_mpi_shift_r(&TV, 1)))
				goto cleanup;

			if ((V1.p[0] & 1) || (V2.p[0] & 1)) {
				if ((r = ttls_mpi_add_mpi(&V1, &V1, &TB))
				    || (r = ttls_mpi_sub_mpi(&V2, &V2, &TA)))
					goto cleanup;
			}

			if ((r = ttls_mpi_shift_r(&V1, 1))
			    || (r = ttls_mpi_shift_r(&V2, 1)))
				goto cleanup;
		}

		if (ttls_mpi_cmp_mpi(&TU, &TV) >= 0) {
			if ((r = ttls_mpi_sub_mpi(&TU, &TU, &TV))
			    || (r = ttls_mpi_sub_mpi(&U1, &U1, &V1))
			    || (r = ttls_mpi_sub_mpi(&U2, &U2, &V2)))
				goto cleanup;
		} else {
			if ((r = ttls_mpi_sub_mpi(&TV, &TV, &TU))
			    || (r = ttls_mpi_sub_mpi(&V1, &V1, &U1))
			    || (r = ttls_mpi_sub_mpi(&V2, &V2, &U2)))
				goto cleanup;
		}
	} while (ttls_mpi_cmp_int(&TU, 0));

	while (ttls_mpi_cmp_int(&V1, 0) < 0)
		if ((r = ttls_mpi_add_mpi(&V1, &V1, N)))
			goto cleanup;

	while (ttls_mpi_cmp_mpi(&V1, N) >= 0)
		if ((r = ttls_mpi_sub_mpi(&V1, &V1, N)))
			goto cleanup;

	r = ttls_mpi_copy(X, &V1);

cleanup:
	ttls_mpi_free(&TA);
	ttls_mpi_free(&TU);
	ttls_mpi_free(&U1);
	ttls_mpi_free(&U2);
	ttls_mpi_free(&G);
	ttls_mpi_free(&TB);
	ttls_mpi_free(&TV);
	ttls_mpi_free(&V1);
	ttls_mpi_free(&V2);

	return r;
}

void
ttls_mpi_modexit(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		TlsMpi **ptr = per_cpu_ptr(&g_buf, cpu);
		kfree(*ptr);
	}
}

int __init
ttls_mpi_modinit(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		TlsMpi **ptr = per_cpu_ptr(&g_buf, cpu);
		*ptr = kmalloc(sizeof(TlsMpi) << MPI_W_SZ, GFP_KERNEL);
		if (!*ptr)
			goto err_cleanup;
	}

	return 0;
err_cleanup:
	ttls_mpi_modexit();
	return -ENOMEM;
}

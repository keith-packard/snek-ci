/*
 * Copyright © 2018 Keith Packard <keithp@keithp.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */


#include "snek.h"

#ifdef SNEK_DYNAMIC
uint8_t 	*snek_pool  __attribute__((aligned(SNEK_ALLOC_ROUND)));
uint32_t	snek_pool_size;
#else
uint8_t	snek_pool[SNEK_POOL + SNEK_POOL_EXTRA] __attribute__((aligned(SNEK_ALLOC_ROUND)));
#endif

struct snek_root {
	const snek_mem_t	*type;
	void			**addr;
};

#define SNEK_NUM_STASH	5
static snek_poly_t		stash_poly[SNEK_NUM_STASH];
static uint8_t			stash_poly_ptr;
static snek_code_t		*stash_code;

#ifndef SNEK_ROOT_DECLARE
#define SNEK_ROOT_DECLARE(n) n
#define SNEK_ROOT_TYPE(n) ((n)->type)
#define SNEK_ROOT_ADDR(n) ((n)->addr)
#endif

static const struct snek_root	SNEK_ROOT_DECLARE(snek_root)[] = {
	{
		.type = &snek_name_mem,
		.addr = (void **) (void *) &snek_names,
	},
	{
		.type = &snek_frame_mem,
		.addr = (void **) (void *) &snek_globals,
	},
	{
		.type = &snek_frame_mem,
		.addr = (void **) (void *) &snek_frame,
	},
	{
		.type = &snek_code_mem,
		.addr = (void **) (void *) &stash_code,
	},
	{
		.type = &snek_compile_mem,
		.addr = (void **) (void *) &snek_compile,
	},
	{
		.type = NULL,
		.addr = (void **) (void *) &stash_poly[0]
	},
	{
		.type = NULL,
		.addr = (void **) (void *) &stash_poly[1]
	},
	{
		.type = NULL,
		.addr = (void **) (void *) &stash_poly[2]
	},
	{
		.type = NULL,
		.addr = (void **) (void *) &stash_poly[3]
	},
	{
		.type = NULL,
		.addr = (void **) (void *) &stash_poly[4]
	},
};

#define SNEK_ROOT	(sizeof (snek_root) / sizeof (snek_root[0]))

#define SNEK_BUSY_SIZE		((SNEK_POOL + 31) / 32)
#define SNEK_NCHUNK_EST(pool)	((pool) / 64)

struct snek_chunk {
	snek_offset_t		old_offset;
	union {
		snek_offset_t	size;
		snek_offset_t	new_offset;
	};
};

#ifdef SNEK_DYNAMIC
static uint8_t	*snek_busy;
static struct snek_chunk *snek_chunk;
static snek_offset_t	SNEK_NCHUNK;

typedef snek_soffset_t snek_chunk_t;

bool
snek_mem_alloc(snek_poly_t pool_size)
{
	snek_poly_t	busy_size = (pool_size + 31) / 32;

	snek_pool = malloc(pool_size + SNEK_POOL_EXTRA +
				busy_size +
				busy_size +
				busy_size +
				SNEK_NCHUNK_EST(pool_size) * sizeof (struct snek_chunk));
	if (!snek_pool)
		return false;
	snek_busy = snek_pool + pool_size + SNEK_POOL_EXTRA;
	snek_chunk = (struct snek_chunk *) (((uintptr_t)(snek_busy + busy_size) + 7) & ~7);
	snek_pool_size = pool_size;
	SNEK_NCHUNK = SNEK_NCHUNK_EST(pool_size);
	return true;
}

#else

#define SNEK_NCHUNK SNEK_NCHUNK_EST(SNEK_POOL)

static uint8_t			snek_busy[SNEK_BUSY_SIZE];
static struct snek_chunk	snek_chunk[SNEK_NCHUNK];

#if SNEK_NCHUNK < 127
typedef int8_t snek_chunk_t;
typedef uint8_t snek_uchunk_t;
#else
typedef snek_soffset_t snek_chunk_t;
typedef snek_offset_t snek_uchunk_t;
#endif

#endif

static snek_offset_t	snek_note_list;

static snek_offset_t	snek_top;

/* Offset of an address within the pool. */
static inline snek_offset_t pool_offset(void *addr) {
	return ((uint8_t *) addr) - snek_pool;
}

/* Address of an offset within the pool */
static inline void *pool_addr(snek_offset_t offset) {
	return snek_pool + offset;
}

static inline snek_offset_t tag_byte(snek_offset_t offset) {
	return offset >> (SNEK_ALLOC_SHIFT + 3);
}

static inline uint8_t tag_bit(snek_offset_t offset) {
	return (offset >> SNEK_ALLOC_SHIFT) & 7;
}

static inline void mark(snek_offset_t offset) {
	snek_busy[tag_byte(offset)] |= (1 << tag_bit(offset));
}

static inline void clear(snek_offset_t offset) {
	snek_busy[tag_byte(offset)] &= ~(1 << tag_bit(offset));
}

static inline bool busy(snek_offset_t offset) {
	return (snek_busy[tag_byte(offset)] >> tag_bit(offset)) & 1;
}

static inline int min(int a, int b) { return a < b ? a : b; }
static inline int max(int a, int b) { return a > b ? a : b; }

static inline bool
note_list(snek_list_t *list_old, snek_list_t *list_new)
{
	if (!snek_list_noted(list_new)) {
		debug_memory("\tnote list %d -> %d\n", pool_offset(list_old), pool_offset(list_new));
		snek_list_set_note_next(list_new, snek_note_list);
		snek_list_set_noted(list_new, true);
		snek_note_list = snek_pool_offset(list_old);
		return false;
	}
	debug_memory("\tnote list %d -> %d already noted (noted %d busy %d)\n",
		     pool_offset(list_old), pool_offset(list_new),
		     snek_list_noted(list_new), busy(pool_offset(list_old)));
	return true;
}

static snek_offset_t	chunk_low, chunk_high;
static snek_chunk_t	chunk_first, chunk_last;

static snek_chunk_t
find_chunk(snek_offset_t offset)
{
	snek_chunk_t l, r;

	/* Binary search for the location */
	l = chunk_first;
	r = chunk_last - 1;
	while (l <= r) {
		snek_chunk_t m = ((snek_uchunk_t) (l + r)) >> 1;
		if (snek_chunk[m].old_offset < offset)
			l = m + 1;
		else
			r = m - 1;
	}
	return l;
}

static void
note_chunk(snek_offset_t offset, snek_offset_t size)
{
	snek_chunk_t chunk;
	snek_offset_t end;

	if (offset < chunk_low || chunk_high <= offset)
		return;

	chunk = find_chunk(offset);

#if SNEK_DEBUG
	if (chunk >= SNEK_NCHUNK)
		snek_error("note chunk failed");
#endif

	/*
	 * The correct location is always in 'l', with r = l-1 being
	 * the entry before the right one
	 */

	/* Shuffle existing entries right */
	end = min(SNEK_NCHUNK, chunk_last + 1);

	memmove(&snek_chunk[chunk+1],
		&snek_chunk[chunk],
		(end - (chunk+1)) * sizeof (struct snek_chunk));

	/* Add new entry */
	snek_chunk[chunk].old_offset = offset;
	snek_chunk[chunk].size = size;

	/* Increment the number of elements up to the size of the array */
	if (chunk_last < SNEK_NCHUNK)
		chunk_last++;

	debug_memory("add chunk %d offset %d size %d chunk_last %d\n", chunk, offset, size, chunk_last);

	/* Set the top address if the array is full */
	if (chunk_last == SNEK_NCHUNK) {
		chunk_high = snek_chunk[SNEK_NCHUNK-1].old_offset +
			snek_chunk[SNEK_NCHUNK-1].size;
		debug_memory("reset chunk_high to %d\n", chunk_high);
	}
}

static void
reset_chunks(void)
{
	chunk_high = snek_top;
	chunk_last = 0;
	chunk_first = 0;
}

/*
 * Walk all referenced objects calling functions on each one
 */

static void
walk(bool (*visit_addr)(const struct snek_mem *type, void **addr),
     bool (*visit_poly)(snek_poly_t *p),
     void (*visit_run)(void))
{
	snek_offset_t i;

	memset(snek_busy, '\0', SNEK_BUSY_SIZE);
	snek_note_list = 0;
	visit_run();
	for (i = 0; i < (int) SNEK_ROOT; i++) {
		if (SNEK_ROOT_TYPE(&snek_root[i])) {
			void **a = SNEK_ROOT_ADDR(&snek_root[i]), *v;
			if (a == NULL || (v = *a) != NULL) {
				visit_addr(SNEK_ROOT_TYPE(&snek_root[i]), a);
			}
		} else {
			snek_poly_t *a = (snek_poly_t *) SNEK_ROOT_ADDR(&snek_root[i]), p;
			if (a && !snek_is_null(p = *a)) {
				visit_poly(a);
			}
		}
	}
	while (snek_note_list) {
		snek_offset_t note = snek_note_list;
		snek_note_list = 0;
		debug_memory("processing list %d\n", pool_offset(snek_pool_ref(note)));
		while (note) {
			snek_list_t *list = snek_pool_ref(note);
			debug_memory("\tprocess list %d\n", pool_offset(list));
			visit_addr(&snek_list_mem, (void **) &list);
			note = snek_list_note_next(list);
			debug_memory("\t\tprocessed, list is now %d next now %d\n",
				     pool_offset(list), note);
			snek_list_set_note_next(list, 0);
			snek_list_set_noted(list, false);
		}
		debug_memory("done procesing list\n");
	}
}


static const struct snek_mem * const snek_mems[4] = {
	[snek_list] = &snek_list_mem,
	[snek_string] = &snek_string_mem,
	[snek_func] = &snek_func_mem,
	[snek_builtin] = &snek_null_mem,
};

static bool
snek_mark_ref(const struct snek_mem *type, void **ref)
{
	return snek_mark_addr(type, *ref);
}

bool
snek_poly_mark_ref(snek_poly_t *p)
{
	return snek_poly_mark(*p);
}

snek_offset_t snek_last_top;
uint8_t snek_collect_counts;

#ifdef DEBUG_MEMORY
static void dump_busy(void)
{
	snek_offset_t i;

	for (i = 0; i < snek_top; i += 4)
		if (busy(i))
			debug_memory("\tBusy %d\n", i);
}
#else
#define dump_busy()
#endif

snek_offset_t
snek_collect(uint8_t style)
{
	snek_offset_t	i;
	snek_offset_t	top;

	debug_memory("Collect...\n");
	/* The first time through, we're doing a full collect */
	if (snek_last_top == 0)
		style = SNEK_COLLECT_FULL;

	/* One in a while, just do a full collect */

	if (snek_collect_counts >= 128)
		style = SNEK_COLLECT_FULL;

	if (style == SNEK_COLLECT_FULL)
		snek_collect_counts = 0;

	if (style == SNEK_COLLECT_FULL) {
		chunk_low = top = 0;
	} else {
		chunk_low = top = snek_last_top;
	}
	for (;;) {
		/* Find the sizes of the first chunk of objects to move */
		reset_chunks();
		debug_memory("mark\n");
		walk(snek_mark_ref, snek_poly_mark_ref, snek_run_mark);
		dump_busy();
		debug_memory("done\n");

		debug_memory("chunk_last %d\n", chunk_last);
		/* Find the first moving object */
		for (i = 0; i < chunk_last; i++) {
			snek_offset_t	size = snek_chunk[i].size;

			debug_memory("size %d old_offset %d top %d\n", size, snek_chunk[i].old_offset, top);
			if (snek_chunk[i].old_offset > top)
				break;

			top += size;
		}

		/* Short-circuit the rest of the loop when all of the
		 * found objects aren't moving. This isn't strictly
		 * necessary as the rest of the loop is structured to
		 * work in this case, but GCC 7.2.0 with optimization
		 * greater than 2 generates incorrect code for this...
		 */
		if (i == SNEK_NCHUNK) {
			chunk_low = chunk_high;
			continue;
		}

		/*
		 * Limit amount of chunk array used in mapping moves
		 * to the active region
		 */
		chunk_first = i;
		chunk_low = snek_chunk[i].old_offset;

		/* Copy all of the objects */
		for (; i < chunk_last; i++) {
			snek_offset_t	size = snek_chunk[i].size;

			debug_memory("  moving %d -> %d (%d)\n",
			       snek_chunk[i].old_offset, top, size);

			snek_chunk[i].new_offset = top;

			memmove(&snek_pool[top],
				&snek_pool[snek_chunk[i].old_offset],
				size);

			top += size;
		}

		if (chunk_first < chunk_last) {
			/* Relocate all references to the objects */
			debug_memory("move\n");
			walk(snek_move_addr, snek_poly_move, snek_run_move);
			debug_memory("done\n");
		}

		/* If we ran into the end of the heap, then
		 * there's no need to keep walking
		 */
		if (chunk_last != SNEK_NCHUNK)
			break;

		/* Next loop starts right above this loop */
		chunk_low = chunk_high;
	}

	snek_top = top;
	if (style == SNEK_COLLECT_FULL)
		snek_last_top = top;

	debug_memory("%d free\n", SNEK_POOL - snek_top);
	return SNEK_POOL - snek_top;
}

/*
 * Mark interfaces for objects
 */


/*
 * Note a reference to memory and collect information about a few
 * object sizes at a time
 */

bool
snek_mark_blob(void *addr, snek_offset_t size)
{
	snek_offset_t offset;

#if SNEK_DEBUG
	if (!snek_is_pool_addr(addr))
		return true;
#endif

	offset = pool_offset(addr);
	if (busy(offset))
		return true;
	debug_memory("\tmark %d size %d\n", offset, size);
	mark(offset);
	note_chunk(offset, size);
	return false;
}

bool
snek_mark_block_addr(const struct snek_mem *type, void *addr)
{
	bool ret;
	ret = snek_mark_blob(addr, snek_size(type, addr));
	if (!ret)
		debug_memory("\tmark %s %d %d\n", type->name, pool_offset(addr), snek_size(type, addr));
	return ret;
}

bool
snek_mark_block_offset(const struct snek_mem *type, snek_offset_t offset)
{
	return snek_mark_block_addr(type, snek_pool_ref(offset));
}

/*
 * Mark an object and all that it refereces
 */
bool
snek_mark_addr(const struct snek_mem *type, void *addr)
{
	bool ret;
	ret = snek_mark_block_addr(type, addr);
	if (!ret)
		SNEK_MEM_MARK(type)(addr);
	return ret;
}

bool
snek_mark_offset(const struct snek_mem *type, snek_offset_t offset)
{
	return snek_mark_addr(type, snek_pool_ref(offset));
}

/*
 * Mark an object, unless it is a list. In that case, just set a bit
 * in the list note array; those will be marked in a separate pass to
 * avoid deep recursion in the collector
 */

bool
snek_poly_mark(snek_poly_t p)
{
	snek_type_t	type;
	void		*addr;
	bool		ret;

	type = snek_poly_type(p);

	if (type == snek_float || type == snek_builtin)
		return true;

	addr = snek_ref(p);

	if (type == snek_list)
		debug_memory("\tmark list %d\n", pool_offset(addr));

#if SNEK_DEBUG
	if (!snek_is_pool_addr(addr))
		snek_panic("non-pool addr in heap");
#endif

	const struct snek_mem *mem;

	mem = snek_mems[type];
	ret = snek_mark_block_addr(mem, addr);
	if (!ret) {
		SNEK_MEM_MARK(mem)(addr);
		if (type == snek_list)
			note_list(addr, addr);
	}
	return ret;
}

/*
 * Find the current location of an object
 * based on the original location. For unmoved
 * objects, this is simple. For moved objects,
 * go search for it
 */

static snek_offset_t
move_map(snek_offset_t offset)
{
	snek_chunk_t	chunk;

	if (offset < chunk_low || chunk_high <= offset)
		return offset;

	chunk = find_chunk(offset);

#if DBG_MEM
	if (snek_chunk[chunk].old_offset != offset)
		abort();
#endif
	return snek_chunk[chunk].new_offset;
}

bool
snek_move_block_offset(snek_offset_t *ref)
{
	snek_offset_t	orig_offset = *ref - 1;
	snek_offset_t	offset;

	offset = move_map(orig_offset);
	if (offset != orig_offset) {
		debug_memory("\tmove %d -> %d\n", orig_offset, offset);
		*ref = offset + 1;
	}

	if (busy(offset))
		return true;

	mark(offset);
	return false;
}

bool
snek_move_block_addr(void **ref)
{
	void		*addr = *ref;
	snek_offset_t	offset, orig_offset;
	bool		ret;

#if SNEK_DEBUG
	if (!snek_is_pool_addr(addr))
		snek_panic("non-pool address");
#endif

	orig_offset = snek_pool_offset(addr);
	offset = orig_offset;
	ret = snek_move_block_offset(&offset);
	if (offset != orig_offset)
		*ref = snek_pool_ref(offset);

	return ret;
}

bool
snek_move_addr(const struct snek_mem *type, void **ref)
{
	bool ret;
	ret = snek_move_block_addr(ref);
	if (!ret)
		SNEK_MEM_MOVE(type)(*ref);

	return ret;
}

bool
snek_move_offset(const struct snek_mem *type, snek_offset_t *ref)
{
	bool ret;
	ret = snek_move_block_offset(ref);
	if (!ret)
		SNEK_MEM_MOVE(type)(snek_pool_ref(*ref));
	return ret;
}

bool
snek_poly_move(snek_poly_t *ref)
{
	snek_poly_t	p = *ref;
	bool		ret;
	void		*addr;
	snek_offset_t	offset, orig_offset;
	snek_type_t	type = snek_poly_type(p);

	if (type == snek_float || type == snek_builtin)
		return true;

	addr = snek_ref(p);

	if (type == snek_list)
		debug_memory("\tmove list %d\n", pool_offset(addr));

#if SNEK_DEBUG
	if (!snek_is_pool_addr(addr))
		snek_panic("non-pool address");
#endif

	orig_offset = pool_offset(addr);
	offset = move_map(orig_offset);

	const struct snek_mem *mem;

	mem = snek_mems[type];

	/* inline snek_move to save stack space */
	ret = snek_move_block_addr(&addr);
	if (!ret) {
		SNEK_MEM_MOVE(mem)(addr);
		if (type == snek_list)
			note_list(pool_addr(orig_offset), addr);
	}

	/* Re-write the poly value */
	if (offset != orig_offset) {
		snek_poly_t np = snek_poly(pool_addr(offset), snek_poly_type(p));
		*ref = np;
	}
	return ret;
}

void *
snek_alloc(snek_offset_t size)
{
	void	*addr;

	size = snek_size_round(size);
	if (SNEK_POOL - snek_top < size &&
	    snek_collect(SNEK_COLLECT_INCREMENTAL) < size &&
	    snek_collect(SNEK_COLLECT_FULL) < size)
	{
		snek_error("out of memory");
		return NULL;
	}
	addr = pool_addr(snek_top);
	memset(addr, '\0', size);
	debug_memory("Alloc %d size %d\n", snek_top, size);
	snek_top += size;
	return addr;
}

void
snek_poly_stash(snek_poly_t p)
{
	stash_poly[stash_poly_ptr++] = p;
}

snek_poly_t
snek_poly_fetch(void)
{
	snek_poly_t	p;

	p = stash_poly[--stash_poly_ptr];
	stash_poly[stash_poly_ptr] = SNEK_NULL;
	return p;
}

void
snek_string_stash(const char *s)
{
	snek_poly_stash(snek_string_to_poly((char *) s));
}

char *
snek_string_fetch(void)
{
	return snek_poly_to_string(snek_poly_fetch());
}

void
snek_list_stash(snek_list_t *l)
{
	snek_poly_stash(snek_list_to_poly(l));
}

snek_list_t *
snek_list_fetch(void)
{
	return snek_poly_to_list(snek_poly_fetch());
}

void
snek_code_stash(snek_code_t *code)
{
	stash_code = code;
}

snek_code_t *
snek_code_fetch(void)
{
	snek_code_t	*code;
	code = stash_code;
	stash_code = 0;
	return code;
}
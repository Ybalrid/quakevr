/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2010-2014 QuakeSpasm developers
Copyright (C) 2020-2020 Vittorio Romeo

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// zone.c

#include "zone.hpp"

#include "quakedef.hpp"
#include "gl_model.hpp"
#include "cmd.hpp"
#include "common.hpp"
#include "console.hpp"
#include "sys.hpp"
#include "gl_texmgr.hpp"

#define DYNAMIC_SIZE \
    (4 * 1024 * 1024) // ericw -- was 512KB (64-bit) / 384KB (32-bit)

#define ZONEID 0x1d4a11
#define MINFRAGMENT 64

typedef struct memblock_s
{
    int size; // including the header and possibly tiny fragments
    int tag;  // a tag of 0 is a free block
    int id;   // should be ZONEID
    int pad;  // pad to 64 bit boundary
    struct memblock_s *next, *prev;
} memblock_t;

typedef struct
{
    int size;             // total bytes malloced, including header
    memblock_t blocklist; // start / end cap for linked list
    memblock_t* rover;
} memzone_t;

void Cache_FreeLow(int new_low_hunk);
void Cache_FreeHigh(int new_high_hunk);


/*
==============================================================================

                        ZONE MEMORY ALLOCATION

There is never any space between memblocks, and there will never be two
contiguous free memblocks.

The rover can be left pointing at a non-empty block

The zone calls are pretty much only used for small strings and structures,
all big things are allocated on the hunk.
==============================================================================
*/

static memzone_t* mainzone;


/*
========================
Z_Free
========================
*/
void Z_Free(void* ptr)
{
    memblock_t* block;
    memblock_t* other;

    if(!ptr)
    {
        Sys_Error("Z_Free: NULL pointer");
    }

    block = (memblock_t*)((byte*)ptr - sizeof(memblock_t));
    if(block->id != ZONEID)
    {
        Sys_Error("Z_Free: freed a pointer without ZONEID");
    }
    if(block->tag == 0)
    {
        Sys_Error("Z_Free: freed a freed pointer");
    }

    block->tag = 0; // mark as free

    other = block->prev;
    if(!other->tag)
    {
        // merge with previous free block
        other->size += block->size;
        other->next = block->next;
        other->next->prev = other;
        if(block == mainzone->rover)
        {
            mainzone->rover = other;
        }
        block = other;
    }

    other = block->next;
    if(!other->tag)
    {
        // merge the next free block onto the end
        block->size += other->size;
        block->next = other->next;
        block->next->prev = block;
        if(other == mainzone->rover)
        {
            mainzone->rover = block;
        }
    }
}


static void* Z_TagMalloc(int size, int tag)
{
    int extra;
    memblock_t* start;
    memblock_t* rover;
    memblock_t* newblock;
    memblock_t* base;

    if(!tag)
    {
        Sys_Error("Z_TagMalloc: tried to use a 0 tag");
    }

    //
    // scan through the block list looking for the first free block
    // of sufficient size
    //
    size += sizeof(memblock_t); // account for size of block header
    size += 4;                  // space for memory trash tester
    size = (size + 7) & ~7;     // align to 8-byte boundary

    base = rover = mainzone->rover;
    start = base->prev;

    do
    {
        if(rover == start)
        {
            // scaned all the way around the list
            return nullptr;
        }
        if(rover->tag)
        {
            base = rover = rover->next;
        }
        else
        {
            rover = rover->next;
        }
    } while(base->tag || base->size < size);

    //
    // found a block big enough
    //
    extra = base->size - size;
    if(extra > MINFRAGMENT)
    {
        // there will be a free fragment after the allocated block
        newblock = (memblock_t*)((byte*)base + size);
        newblock->size = extra;
        newblock->tag = 0; // free block
        newblock->prev = base;
        newblock->id = ZONEID;
        newblock->next = base->next;
        newblock->next->prev = newblock;
        base->next = newblock;
        base->size = size;
    }

    base->tag = tag; // no longer a free block

    mainzone->rover = base->next; // next allocation will start looking here

    base->id = ZONEID;

    // marker for memory trash testing
    *(int*)((byte*)base + base->size - 4) = ZONEID;

    return (void*)((byte*)base + sizeof(memblock_t));
}

/*
========================
Z_CheckHeap
========================
*/
[[maybe_unused]] static void Z_CheckHeap()
{
    for(memblock_t* block = mainzone->blocklist.next;; block = block->next)
    {
        if(block->next == &mainzone->blocklist)
        {
            break; // all blocks have been hit
        }

        if((byte*)block + block->size != (byte*)block->next)
        {
            Sys_Error(
                "Z_CheckHeap: block size does not touch the next block\n");
        }

        if(block->next->prev != block)
        {
            Sys_Error(
                "Z_CheckHeap: next block doesn't have proper back link\n");
        }

        if(!block->tag && !block->next->tag)
        {
            Sys_Error("Z_CheckHeap: two consecutive free blocks\n");
        }
    }
}


/*
========================
Z_Malloc
========================
*/
void* Z_Malloc(int size)
{
#ifdef PARANOID
    Z_CheckHeap(); // DEBUG
#endif

    void* buf = Z_TagMalloc(size, 1);
    if(!buf)
    {
        Sys_Error("Z_Malloc: failed on allocation of %i bytes", size);
    }
    Q_memset(buf, 0, size);

    return buf;
}

/*
========================
Z_Realloc
========================
*/
void* Z_Realloc(void* ptr, int size)
{
    int old_size;
    void* old_ptr;
    memblock_t* block;

    if(!ptr)
    {
        return Z_Malloc(size);
    }

    block = (memblock_t*)((byte*)ptr - sizeof(memblock_t));
    if(block->id != ZONEID)
    {
        Sys_Error("Z_Realloc: realloced a pointer without ZONEID");
    }
    if(block->tag == 0)
    {
        Sys_Error("Z_Realloc: realloced a freed pointer");
    }

    old_size = block->size;
    old_size -= (4 + (int)sizeof(memblock_t)); /* see Z_TagMalloc() */
    old_ptr = ptr;

    Z_Free(ptr);
    ptr = Z_TagMalloc(size, 1);

    if(!ptr)
    {
        Sys_Error("Z_Realloc: failed on allocation of %i bytes", size);
    }

    // QSS
    // Spike -- fix a bug where alignment resulted in no 0-initialisation
    block = (memblock_t*)((byte*)ptr - sizeof(memblock_t));
    size = block->size;
    size -= (4 + (int)sizeof(memblock_t)); /* see Z_TagMalloc() */
                                           // Spike -- end fix

    if(ptr != old_ptr)
    {
        memmove(ptr, old_ptr, q_min(old_size, size));
    }

    if(old_size < size)
    {
        memset((byte*)ptr + old_size, 0, size - old_size);
    }

    return ptr;
}

char* Z_Strdup(const char* s)
{
    size_t sz = strlen(s) + 1;
    char* ptr = (char*)Z_Malloc(sz);
    memcpy(ptr, s, sz);
    return ptr;
}


/*
========================
Z_Print
========================
*/
void Z_Print(memzone_t* zone)
{
    memblock_t* block;

    Con_Printf("zone size: %i  location: %p\n", mainzone->size,
        static_cast<void*>(mainzone));

    for(block = zone->blocklist.next;; block = block->next)
    {
        Con_Printf("block:%p    size:%7i    tag:%3i\n",
            static_cast<void*>(block), block->size, block->tag);

        if(block->next == &zone->blocklist)
        {
            break; // all blocks have been hit
        }
        if((byte*)block + block->size != (byte*)block->next)
        {
            Con_Printf("ERROR: block size does not touch the next block\n");
        }
        if(block->next->prev != block)
        {
            Con_Printf("ERROR: next block doesn't have proper back link\n");
        }
        if(!block->tag && !block->next->tag)
        {
            Con_Printf("ERROR: two consecutive free blocks\n");
        }
    }
}


//============================================================================

#define HUNK_SENTINAL 0x1df001ed

#define HUNKNAME_LEN 24
typedef struct
{
    int sentinal;
    int size; // including sizeof(hunk_t), -1 = not allocated
    char name[HUNKNAME_LEN];
} hunk_t;

byte* hunk_base;
int hunk_size;

int hunk_low_used;
int hunk_high_used;

bool hunk_tempactive;
int hunk_tempmark;

/*
==============
Hunk_Check

Run consistancy and sentinal trahing checks
==============
*/
void Hunk_Check()
{
    hunk_t* h;

    for(h = (hunk_t*)hunk_base; (byte*)h != hunk_base + hunk_low_used;)
    {
        if(h->sentinal != HUNK_SENTINAL)
        {
            Sys_Error("Hunk_Check: trahsed sentinal");
        }
        if(h->size < (int)sizeof(hunk_t) ||
            h->size + (byte*)h - hunk_base > hunk_size)
        {
            Sys_Error("Hunk_Check: bad size");
        }
        h = (hunk_t*)((byte*)h + h->size);
    }
}

/*
==============
Hunk_Print

If "all" is specified, every single allocation is printed.
Otherwise, allocations with the same name will be totaled up before printing.
==============
*/
void Hunk_Print(bool all)
{
    hunk_t* h;
    hunk_t* next;
    hunk_t* endlow;
    hunk_t* starthigh;
    hunk_t* endhigh;
    int count;
    int sum;
    int totalblocks;
    char name[HUNKNAME_LEN];

    count = 0;
    sum = 0;
    totalblocks = 0;

    h = (hunk_t*)hunk_base;
    endlow = (hunk_t*)(hunk_base + hunk_low_used);
    starthigh = (hunk_t*)(hunk_base + hunk_size - hunk_high_used);
    endhigh = (hunk_t*)(hunk_base + hunk_size);

    Con_Printf("          :%8i total hunk size\n", hunk_size);
    Con_Printf("-------------------------\n");

    while(true)
    {
        //
        // skip to the high hunk if done with low hunk
        //
        if(h == endlow)
        {
            Con_Printf("-------------------------\n");
            Con_Printf("          :%8i REMAINING\n",
                hunk_size - hunk_low_used - hunk_high_used);
            Con_Printf("-------------------------\n");
            h = starthigh;
        }

        //
        // if totally done, break
        //
        if(h == endhigh)
        {
            break;
        }

        //
        // run consistancy checks
        //
        if(h->sentinal != HUNK_SENTINAL)
        {
            Sys_Error("Hunk_Check: trahsed sentinal");
        }
        if(h->size < (int)sizeof(hunk_t) ||
            h->size + (byte*)h - hunk_base > hunk_size)
        {
            Sys_Error("Hunk_Check: bad size");
        }

        next = (hunk_t*)((byte*)h + h->size);
        count++;
        totalblocks++;
        sum += h->size;

        //
        // print the single block
        //
        memcpy(name, h->name, HUNKNAME_LEN);
        if(all)
        {
            Con_Printf("%8p :%8i %8s\n", static_cast<void*>(h), h->size, name);
        }

        //
        // print the total
        //
        if(next == endlow || next == endhigh ||
            strncmp(h->name, next->name, HUNKNAME_LEN - 1))
        {
            if(!all)
            {
                Con_Printf("          :%8i %8s (TOTAL)\n", sum, name);
            }
            count = 0;
            sum = 0;
        }

        h = next;
    }

    Con_Printf("-------------------------\n");
    Con_Printf("%8i total blocks\n", totalblocks);
}

/*
===================
Hunk_Print_f -- johnfitz -- console command to call hunk_print
===================
*/
void Hunk_Print_f()
{
    Hunk_Print(false);
}

/*
===================
Hunk_AllocName
===================
*/
void* Hunk_AllocName(int size, const char* name) noexcept
{
#ifdef PARANOID
    Hunk_Check();
#endif

    if(size < 0)
    {
        Sys_Error("Hunk_Alloc: bad size: %i", size);
    }

    size = sizeof(hunk_t) + ((size + 15) & ~15);

    if(hunk_size - hunk_low_used - hunk_high_used < size)
    {
        Sys_Error("Hunk_Alloc: failed on %i bytes", size);
    }

    hunk_t* h = (hunk_t*)(hunk_base + hunk_low_used);
    hunk_low_used += size;

    Cache_FreeLow(hunk_low_used);

    memset(h, 0, size);

    h->size = size;
    h->sentinal = HUNK_SENTINAL;
    q_strlcpy(h->name, name, HUNKNAME_LEN);

    return (void*)(h + 1);
}

/*
===================
Hunk_Alloc
===================
*/
[[nodiscard]] void* Hunk_Alloc(const int size) noexcept
{
    return Hunk_AllocName(size, "unknown");
}

[[nodiscard]] int Hunk_LowMark() noexcept
{
    return hunk_low_used;
}

void Hunk_FreeToLowMark(const int mark) noexcept
{
    if(mark < 0 || mark > hunk_low_used)
    {
        Sys_Error("Hunk_FreeToLowMark: bad mark %i", mark);
    }
    memset(hunk_base + mark, 0, hunk_low_used - mark);
    hunk_low_used = mark;
}

[[nodiscard]] int Hunk_HighMark() noexcept
{
    if(hunk_tempactive)
    {
        hunk_tempactive = false;
        Hunk_FreeToHighMark(hunk_tempmark);
    }

    return hunk_high_used;
}

void Hunk_FreeToHighMark(const int mark) noexcept
{
    if(hunk_tempactive)
    {
        hunk_tempactive = false;
        Hunk_FreeToHighMark(hunk_tempmark);
    }
    if(mark < 0 || mark > hunk_high_used)
    {
        Sys_Error("Hunk_FreeToHighMark: bad mark %i", mark);
    }
    memset(hunk_base + hunk_size - hunk_high_used, 0, hunk_high_used - mark);
    hunk_high_used = mark;
}


/*
===================
Hunk_HighAllocName
===================
*/
[[nodiscard]] void* Hunk_HighAllocName(int size, const char* name) noexcept
{
    hunk_t* h;

    if(size < 0)
    {
        Sys_Error("Hunk_HighAllocName: bad size: %i", size);
    }

    if(hunk_tempactive)
    {
        Hunk_FreeToHighMark(hunk_tempmark);
        hunk_tempactive = false;
    }

#ifdef PARANOID
    Hunk_Check();
#endif

    size = sizeof(hunk_t) + ((size + 15) & ~15);

    if(hunk_size - hunk_low_used - hunk_high_used < size)
    {
        Con_Printf("Hunk_HighAlloc: failed on %i bytes\n", size);
        return nullptr;
    }

    hunk_high_used += size;
    Cache_FreeHigh(hunk_high_used);

    h = (hunk_t*)(hunk_base + hunk_size - hunk_high_used);

    memset(h, 0, size);
    h->size = size;
    h->sentinal = HUNK_SENTINAL;
    q_strlcpy(h->name, name, HUNKNAME_LEN);

    return (void*)(h + 1);
}


/*
=================
Hunk_TempAlloc

Return space from the top of the hunk
=================
*/
void* Hunk_TempAlloc(int size)
{
    void* buf;

    size = (size + 15) & ~15;

    if(hunk_tempactive)
    {
        Hunk_FreeToHighMark(hunk_tempmark);
        hunk_tempactive = false;
    }

    hunk_tempmark = Hunk_HighMark();

    buf = Hunk_HighAllocName(size, "temp");

    hunk_tempactive = true;

    return buf;
}

[[nodiscard]] char* Hunk_Strdup(const char* s, const char* name) noexcept
{
    size_t sz = strlen(s) + 1;
    char* ptr = (char*)(Hunk_AllocName(sz, name));
    memcpy(ptr, s, sz);
    return ptr;
}

/*
===============================================================================

CACHE MEMORY

===============================================================================
*/

#define CACHENAME_LEN 32
typedef struct cache_system_s
{
    int size; // including this header
    cache_user_t* user;
    char name[CACHENAME_LEN];
    struct cache_system_s *prev, *next;
    struct cache_system_s *lru_prev, *lru_next; // for LRU flushing
} cache_system_t;

cache_system_t* Cache_TryAlloc(int size, bool nobottom);

cache_system_t cache_head;

/*
===========
Cache_Move
===========
*/
void Cache_Move(cache_system_t* c)
{
    cache_system_t* new_cs;

    // we are clearing up space at the bottom, so only allocate it late
    new_cs = Cache_TryAlloc(c->size, true);
    if(new_cs)
    {
        //		Con_Printf ("cache_move ok\n");

        Q_memcpy(new_cs + 1, c + 1, c->size - sizeof(cache_system_t));
        new_cs->user = c->user;
        Q_memcpy(new_cs->name, c->name, sizeof(new_cs->name));
        Cache_Free(c->user, false); // johnfitz -- added second argument
        new_cs->user->data = (void*)(new_cs + 1);
    }
    else
    {
        //		Con_Printf ("cache_move failed\n");

        Cache_Free(
            c->user, true); // tough luck... //johnfitz -- added second argument
    }
}

/*
============
Cache_FreeLow

Throw things out until the hunk can be expanded to the given point
============
*/
void Cache_FreeLow(int new_low_hunk)
{
    cache_system_t* c;

    while(true)
    {
        c = cache_head.next;
        if(c == &cache_head)
        {
            return; // nothing in cache at all
        }
        if((byte*)c >= hunk_base + new_low_hunk)
        {
            return; // there is space to grow the hunk
        }
        Cache_Move(c); // reclaim the space
    }
}

/*
============
Cache_FreeHigh

Throw things out until the hunk can be expanded to the given point
============
*/
void Cache_FreeHigh(int new_high_hunk)
{
    cache_system_t* c;
    cache_system_t* prev;

    prev = nullptr;
    while(true)
    {
        c = cache_head.prev;
        if(c == &cache_head)
        {
            return; // nothing in cache at all
        }
        if((byte*)c + c->size <= hunk_base + hunk_size - new_high_hunk)
        {
            return; // there is space to grow the hunk
        }
        if(c == prev)
        {
            Cache_Free(c->user, true); // didn't move out of the way //johnfitz
                                       // -- added second argument
        }
        else
        {
            Cache_Move(c); // try to move it
            prev = c;
        }
    }
}

void Cache_UnlinkLRU(cache_system_t* cs)
{
    if(!cs->lru_next || !cs->lru_prev)
    {
        Sys_Error("Cache_UnlinkLRU: NULL link");
    }

    cs->lru_next->lru_prev = cs->lru_prev;
    cs->lru_prev->lru_next = cs->lru_next;

    cs->lru_prev = cs->lru_next = nullptr;
}

void Cache_MakeLRU(cache_system_t* cs)
{
    if(cs->lru_next || cs->lru_prev)
    {
        Sys_Error("Cache_MakeLRU: active link");
    }

    cache_head.lru_next->lru_prev = cs;
    cs->lru_next = cache_head.lru_next;
    cs->lru_prev = &cache_head;
    cache_head.lru_next = cs;
}

/*
============
Cache_TryAlloc

Looks for a free block of memory between the high and low hunk marks
Size should already include the header and padding
============
*/
cache_system_t* Cache_TryAlloc(int size, bool nobottom)
{
    cache_system_t* cs;
    cache_system_t* new_cs;

    // is the cache completely empty?

    if(!nobottom && cache_head.prev == &cache_head)
    {
        if(hunk_size - hunk_high_used - hunk_low_used < size)
        {
            Sys_Error("Cache_TryAlloc: %i is greater than free hunk", size);
        }

        new_cs = (cache_system_t*)(hunk_base + hunk_low_used);
        memset(new_cs, 0, sizeof(*new_cs));
        new_cs->size = size;

        cache_head.prev = cache_head.next = new_cs;
        new_cs->prev = new_cs->next = &cache_head;

        Cache_MakeLRU(new_cs);
        return new_cs;
    }

    // search from the bottom up for space

    new_cs = (cache_system_t*)(hunk_base + hunk_low_used);
    cs = cache_head.next;

    do
    {
        if(!nobottom || cs != cache_head.next)
        {
            if((byte*)cs - (byte*)new_cs >= size)
            {
                // found space
                memset(new_cs, 0, sizeof(*new_cs));
                new_cs->size = size;

                new_cs->next = cs;
                new_cs->prev = cs->prev;
                cs->prev->next = new_cs;
                cs->prev = new_cs;

                Cache_MakeLRU(new_cs);

                return new_cs;
            }
        }

        // continue looking
        new_cs = (cache_system_t*)((byte*)cs + cs->size);
        cs = cs->next;

    } while(cs != &cache_head);

    // try to allocate one at the very end
    if(hunk_base + hunk_size - hunk_high_used - (byte*)new_cs >= size)
    {
        memset(new_cs, 0, sizeof(*new_cs));
        new_cs->size = size;

        new_cs->next = &cache_head;
        new_cs->prev = cache_head.prev;
        cache_head.prev->next = new_cs;
        cache_head.prev = new_cs;

        Cache_MakeLRU(new_cs);

        return new_cs;
    }

    return nullptr; // couldn't allocate
}

/*
============
Cache_Flush

Throw everything out, so new data will be demand cached
============
*/
void Cache_Flush()
{
    while(cache_head.next != &cache_head)
    {
        Cache_Free(cache_head.next->user,
            true); // reclaim the space //johnfitz -- added second argument
    }
}

/*
============
Cache_Print

============
*/
void Cache_Print()
{
    cache_system_t* cd;

    for(cd = cache_head.next; cd != &cache_head; cd = cd->next)
    {
        Con_Printf("%8i : %s\n", cd->size, cd->name);
    }
}

/*
============
Cache_Report

============
*/
void Cache_Report()
{
    Con_DPrintf("%4.1f megabyte data cache\n",
        (hunk_size - hunk_high_used - hunk_low_used) / (float)(1024 * 1024));
}

/*
============
Cache_Init

============
*/
void Cache_Init()
{
    cache_head.next = cache_head.prev = &cache_head;
    cache_head.lru_next = cache_head.lru_prev = &cache_head;

    Cmd_AddCommand("flush", Cache_Flush);
}

/*
==============
Cache_Free

Frees the memory and removes it from the LRU list
==============
*/
void Cache_Free(
    cache_user_t* c, bool freetextures) // johnfitz -- added second argument
{
    cache_system_t* cs;

    if(!c->data)
    {
        Sys_Error("Cache_Free: not allocated");
    }

    cs = ((cache_system_t*)c->data) - 1;

    cs->prev->next = cs->next;
    cs->next->prev = cs->prev;
    cs->next = cs->prev = nullptr;

    c->data = nullptr;

    Cache_UnlinkLRU(cs);

    // johnfitz -- if a model becomes uncached, free the gltextures.  This only
    // works becuase the cache_user_t is the last component of the qmodel_t
    // struct.  Should fail harmlessly if *c is actually part of an sfx_t
    // struct. I FEEL DIRTY
    if(freetextures)
    {
        TexMgr_FreeTexturesForOwner((qmodel_t*)(c + 1) - 1);
    }
}



/*
==============
Cache_Check
==============
*/
void* Cache_Check(cache_user_t* c)
{
    cache_system_t* cs;

    if(!c->data)
    {
        return nullptr;
    }

    cs = ((cache_system_t*)c->data) - 1;

    // move to head of LRU
    Cache_UnlinkLRU(cs);
    Cache_MakeLRU(cs);

    return c->data;
}


/*
==============
Cache_Alloc
==============
*/
void* Cache_Alloc(cache_user_t* c, int size, const char* name)
{
    cache_system_t* cs;

    if(c->data)
    {
        Sys_Error("Cache_Alloc: allready allocated");
    }

    if(size <= 0)
    {
        Sys_Error("Cache_Alloc: size %i", size);
    }

    size = (size + sizeof(cache_system_t) + 15) & ~15;

    // find memory for it
    while(true)
    {
        cs = Cache_TryAlloc(size, false);
        if(cs)
        {
            q_strlcpy(cs->name, name, CACHENAME_LEN);
            c->data = (void*)(cs + 1);
            cs->user = c;
            break;
        }

        // free the least recently used cahedat
        if(cache_head.lru_prev == &cache_head)
        {
            Sys_Error("Cache_Alloc: out of memory"); // not enough memory at all
        }

        Cache_Free(cache_head.lru_prev->user,
            true); // johnfitz -- added second argument
    }

    return Cache_Check(c);
}

//============================================================================


static void Memory_InitZone(memzone_t* zone, int size)
{
    memblock_t* block;

    // set the entire zone to one free block

    zone->blocklist.next = zone->blocklist.prev = block =
        (memblock_t*)((byte*)zone + sizeof(memzone_t));
    zone->blocklist.tag = 1; // in use block
    zone->blocklist.id = 0;
    zone->blocklist.size = 0;
    zone->rover = block;

    block->prev = block->next = &zone->blocklist;
    block->tag = 0; // free block
    block->id = ZONEID;
    block->size = size - sizeof(memzone_t);
}

/*
========================
Memory_Init
========================
*/
void Memory_Init(void* buf, int size)
{
    int p;
    int zonesize = DYNAMIC_SIZE;

    hunk_base = (byte*)buf;
    hunk_size = size;
    hunk_low_used = 0;
    hunk_high_used = 0;

    Cache_Init();
    p = COM_CheckParm("-zone");
    if(p)
    {
        if(p < com_argc - 1)
        {
            zonesize = Q_atoi(com_argv[p + 1]) * 1024;
        }
        else
        {
            Sys_Error("Memory_Init: you must specify a size in KB after -zone");
        }
    }
    mainzone = (memzone_t*)Hunk_AllocName(zonesize, "zone");
    Memory_InitZone(mainzone, zonesize);

    Cmd_AddCommand("hunk_print", Hunk_Print_f); // johnfitz
}

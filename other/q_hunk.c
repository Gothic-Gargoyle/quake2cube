#include <stdio.h>
#include <ctype.h>

#include "../qcommon/qcommon.h"

//===============================================================================

int		hunkcount;


byte	*membase;
int		hunkmaxsize;
int		cursize;

void *Hunk_Begin (int maxsize)
{
	cursize = 0;

#ifdef HW_DOL
	/*
	 * Quake II's PC renderer treats these as generous virtual-memory
	 * reservations. GameCube must allocate them from physical MEM1.
	 *
	 * Runtime measurements on base2:
	 *   16 MiB BSP request used about 2.07 MiB.
	 *
	 * Give the renderer practical fixed budgets instead of consuming
	 * the original PC reservation sizes.
	 */
	if (maxsize == 0x1000000)
		hunkmaxsize = 0x270000; /* 2.4375 MiB BSP */
	else
		hunkmaxsize = maxsize;

	Com_Printf (
		"Hunk_Begin: GameCube %i-byte request -> %i-byte hunk\n",
		maxsize,
		hunkmaxsize);

	membase = malloc (hunkmaxsize);

	if (!membase)
	{
#ifdef HW_DOL
		int probe;
		int largest = 0;
		int count = 0;
		int total = 0;
		void *probe_mem = NULL;
		void *pieces[128];

		/*
		 * First find the precise contiguous-allocation ceiling near
		 * the failed request.
		 */
		for (probe = hunkmaxsize - 32; probe >= 32; probe -= 32)
		{
			probe_mem = malloc(probe);

			if (probe_mem)
			{
				largest = probe;
				free(probe_mem);
				break;
			}
		}

		/*
		 * Then measure fragmented free space by consuming it in
		 * small pieces. Everything is released before the fatal.
		 */
		while (count < 128)
		{
			pieces[count] = malloc(16384);

			if (!pieces[count])
				break;

			total += 16384;
			count++;
		}

		while (count > 0)
			free(pieces[--count]);

		Com_Printf(
			"Hunk_Begin heap probe: request=%i largest=%i "
			"fragmented16k=%i bytes\\n",
			hunkmaxsize,
			largest,
			total);
#endif

		Sys_Error ("reserve failed");
	}

	memset (membase, 0, hunkmaxsize);
	return (void *)membase;
#endif

	hunkmaxsize = maxsize;

	Com_Printf (
		"Hunk_Begin: reserving %i bytes\n",
		hunkmaxsize);

	membase = malloc (hunkmaxsize);

	if (!membase)
		Sys_Error ("reserve failed");

	memset (membase, 0, hunkmaxsize);

	return (void *)membase;
}

void *Hunk_Alloc (int size)
{
	// round to cacheline
	size = (size+31)&~31;

	cursize += size;
	if (cursize > hunkmaxsize)
	{
#ifdef HW_DOL
		Com_Printf (
			"Hunk_Alloc overflow: need %i bytes, max %i, "
			"last allocation %i bytes\n",
			cursize,
			hunkmaxsize,
			size);
#endif
		Sys_Error ("Hunk_Alloc overflow");
	}

	return (void *)(membase+cursize-size);
}

int Hunk_End (void)
{
#ifdef HW_DOL
	int compact_size = cursize > 0 ? cursize : 32;

	if (compact_size < hunkmaxsize)
	{
		byte *shrunk;

		shrunk = realloc (membase, compact_size);

		if (!shrunk)
			Sys_Error ("Hunk_End: shrink failed");

		/*
		 * All renderer pointers already reference locations inside
		 * membase. Shrinking is safe only if realloc keeps the block
		 * at the same address.
		 */
		if (shrunk != membase)
			Sys_Error ("Hunk_End: realloc moved hunk");

		membase = shrunk;

		Com_Printf (
			"Hunk_End: shrunk %i -> %i bytes\n",
			hunkmaxsize,
			compact_size);

		hunkmaxsize = compact_size;
	}
#endif

	hunkcount++;

	Com_Printf (
		"Hunk_End: used %i of %i bytes\n",
		cursize,
		hunkmaxsize);

	return cursize;
}

void Hunk_Free (void *base)
{
	if ( base )
		free (base);

	hunkcount--;
}



//============================================


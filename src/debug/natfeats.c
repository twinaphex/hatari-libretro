/*
 * Hatari - natfeats.c
 * 
 * Copyright (C) 2012-2014 by Eero Tamminen
 *
 * This file is distributed under the GNU General Public License, version 2
 * or at your option any later version. Read the file gpl.txt for details.
 *
 * natfeats.c - Hatari Native features identification and call forwarding,
 * modeleted after similar code in Aranym (written by Petr Stehlik),
 * specified here:
 * 	http://wiki.aranym.org/natfeats/proposal
 */
const char Natfeats_fileid[] = "Hatari natfeats.c : " __DATE__ " " __TIME__;

#include <stdio.h>
#include "main.h"
#include "version.h"
#include "configuration.h"
#include "stMemory.h"
#include "m68000.h"
#include "natfeats.h"
#include "control.h"

#define NF_DEBUG 1
#if NF_DEBUG
# define Dprintf(a) printf a
#else
# define Dprintf(a)
#endif

/* whether to allow XBIOS(255) style
 * Hatari command line parsing with "command" NF
 */
#define NF_COMMAND 0

/* TODO:
 * - supervisor vs. user stack handling?
 * - clipboard and hostfs native features?
 */


/*
 * Native Features shared with Aranym
 */

static bool nf_name(Uint32 stack, Uint32 subid, Uint32 *retval)
{
	Uint32 ptr, len;
	const char *str;
	char *buf;

	ptr = STMemory_ReadLong(stack);
	len = STMemory_ReadLong(stack + SIZE_LONG);
	Dprintf(("NF_NAME[%d](0x%x, %d)\n", subid, ptr, len));

	if (!STMemory_ValidArea(ptr, len)) {
		M68000_BusError(ptr, BUS_ERROR_WRITE);
		return false;
	}
	if (subid) {
		str = PROG_NAME;
	} else {
		str = "Hatari";
	}
	buf = (char *)STRAM_ADDR(ptr);
	*retval = snprintf(buf, len, "%s", str);
	return true;
}

static bool nf_version(Uint32 stack, Uint32 subid, Uint32 *retval)
{
	Dprintf(("NF_VERSION() -> 0x00010000\n"));
	*retval = 0x00010000;
	return true;
}

static bool nf_stderr(Uint32 stack, Uint32 subid, Uint32 *retval)
{
	const char *str;
	Uint32 ptr;

	ptr = STMemory_ReadLong(stack);
	//Dprintf(("NF stderr(0x%x)\n", ptr));

	if (!STMemory_ValidArea(ptr, 1)) {
		M68000_BusError(ptr, BUS_ERROR_READ);
		return false;
	}
	str = (const char *)STRAM_ADDR(ptr);
	*retval = fprintf(stderr, "%s", str);
	fflush(stderr);
	return true;
}

static bool nf_shutdown(Uint32 stack, Uint32 subid, Uint32 *retval)
{
	Dprintf(("NF_SHUTDOWN()\n"));
	ConfigureParams.Log.bConfirmQuit = false;
	Main_RequestQuit();
	return true;
}

/*
 * Native Features specific to Hatari
 */

/**
 * invoke debugger
 */
static bool nf_debugger(Uint32 stack, Uint32 subid, Uint32 *retval)
{
	Dprintf(("NF_DEBUGGER()\n"));
	M68000_SetSpecial(SPCFLAG_DEBUGGER);
	return true;
}

#if NF_COMMAND
/**
 * execute Hatari (command line / debugger) command
 */
static bool nf_command(Uint32 stack, Uint32 subid, Uint32 *retval)
{
	const char *buffer;
	Uint32 ptr;

	ptr = STMemory_ReadLong(stack);

	if (!STMemory_ValidArea(ptr, 1)) {
		M68000_BusError(ptr, BUS_ERROR_READ);
		return false;
	}
	buffer = (const char *)STRAM_ADDR(ptr);
	Dprintf(("NF_COMMAND(0x%x \"%s\")\n", ptr, buffer));

	Control_ProcessBuffer(buffer);
	return true;
}
#endif

/* ---------------------------- */

#define FEATNAME_MAX 16

static const struct {
	const char *name;	/* feature name */
	bool super;		/* should be called only in supervisor mode */
	bool (*cb)(Uint32 stack, Uint32 subid, Uint32 *retval);
} features[] = {
#if NF_COMMAND
	{ "NF_COMMAND",  false, nf_command },
#endif
	{ "NF_NAME",     false, nf_name },
	{ "NF_VERSION",  false, nf_version },
	{ "NF_STDERR",   false, nf_stderr },
	{ "NF_SHUTDOWN", true,  nf_shutdown },
	{ "NF_DEBUGGER", false, nf_debugger }
};

/* macros from Aranym */
#define ID_SHIFT        20
#define IDX2MASTERID(idx)       (((idx)+1) << ID_SHIFT)
#define MASTERID2IDX(id)        (((id) >> ID_SHIFT)-1)
#define MASKOUTMASTERID(id)     ((id) & ((1L << ID_SHIFT)-1))


/**
 * Set retval to internal ID for requested Native Feature,
 * or zero if feature is unknown/unsupported.
 * 
 * Return true if caller is to proceed normally,
 * false if there was an exception.
 */
bool NatFeat_ID(Uint32 stack, Uint32 *retval)
{
	const char *name;
	Uint32 ptr;
	int i;

	ptr = STMemory_ReadLong(stack);
	if (!STMemory_ValidArea(ptr, FEATNAME_MAX)) {
		M68000_BusError(ptr, BUS_ERROR_READ);
		return false;
	}

	name = (const char *)STRAM_ADDR(ptr);
	Dprintf(("NF ID(0x%x \"%s\")\n", ptr, name));

	for (i = 0; i < ARRAYSIZE(features); i++) {
		if (strcmp(features[i].name, name) == 0) {
			*retval = IDX2MASTERID(i);
			return true;
		}
	}
	/* unknown feature */
	*retval = 0;
	return true;
}

/**
 * Do given Native Feature, if it is supported
 * and set 'retval' accordingly.
 * 
 * Return true if caller is to proceed normally,
 * false if there was an exception.
 */
bool NatFeat_Call(Uint32 stack, bool super, Uint32 *retval)
{
	Uint32 subid = STMemory_ReadLong(stack);
	unsigned int idx = MASTERID2IDX(subid);
	subid = MASKOUTMASTERID(subid);

	if (idx >= ARRAYSIZE(features)) {
		Dprintf(("ERROR: invalid NF ID %d requested\n", idx));
		return true; /* undefined */
	}
	if (features[idx].super && !super) {
		Dprintf(("ERROR: NF function %d called without supervisor mode\n", idx));
		Exception(8, 0, M68000_EXC_SRC_CPU);
		return false;
	}
	stack += SIZE_LONG;
	return features[idx].cb(stack, subid, retval);
}

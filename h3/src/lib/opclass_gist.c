/*
 * Copyright 2019-2020 Bytes & Brains
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *	   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <postgres.h>		 // Datum, etc.
#include <fmgr.h>			 // PG_FUNCTION_ARGS, etc.
#include <utils/geo_decls.h> // making native points
#include <access/stratnum.h> // RTOverlapStrategyNumber, etc.
#include <access/gist.h>	 // GiST

#include <h3api.h> // Main H3 include
#include "extension.h"
#include "upstream/h3Index.h"

#define H3_ROOT_INDEX -1

#define LOG_NOTICE(X)                                                                     \
	do                                                                                    \
	{                                                                                     \
		if (false)                                                                        \
			ereport(NOTICE, (errmsg("[%s]: (%d).", (const char *)__FUNCTION__, (int)X))); \
	} while (0)

#define debug_func(x)                                                                                                \
	do                                                                                                               \
	{                                                                                                                \
		if (false)                                                                                                   \
		{                                                                                                            \
			ereport(NOTICE, (                                                                                        \
								errmsg("[%s]: Returned nonzero result (%d).", (const char *)__FUNCTION__, (int)x))); \
		}                                                                                                            \
	} while (0)

static int
gist_cmp(H3Index a, H3Index b)
{
	int aRes;
	int bRes;

	uint64_t cellMask = (1LL << 45) - 1; /* rightmost 45 bits */
	uint64_t aCell;
	uint64_t bCell;

	/* identity */
	if (a == b)
	{
		return 1;
	}

	/* no shared basecell */
	if (H3_GET_BASE_CELL(a) != H3_GET_BASE_CELL(b))
	{
		return 0;
	}

	aRes = H3_GET_RESOLUTION(a);
	bRes = H3_GET_RESOLUTION(b);

	//----

	H3Index big, sml;
	int maxRes;

	/* a contains b */
	if (a == H3_ROOT_INDEX || (aRes < bRes && h3ToParent(b, aRes) == a))
		return 1;
	/* a contained by b */
	if (b == H3_ROOT_INDEX || (aRes > bRes && h3ToParent(a, bRes) == b))
		return -1;

	/* no overlap */
	return 0;
}

/**
 * GiST support
 */

static H3Index
common_ancestor(H3Index a, H3Index b)
{
	int aRes;
	int bRes;
	int maxRes, bigRes;
	uint64_t cellMask = (1LL << 45) - 1; /* rightmost 45 bits */
	uint64_t abCell;
	uint64_t mask;
	H3Index masked;

	if (a == b)
	{
		return a;
	}

	/* do not even share the basecell */
	if (H3_GET_BASE_CELL(a) != H3_GET_BASE_CELL(b))
	{
		return H3_ROOT_INDEX;
	}

	aRes = H3_GET_RESOLUTION(a);
	bRes = H3_GET_RESOLUTION(b);
	bigRes = (aRes > bRes) ? aRes : bRes;
	for (int i = bigRes; i > 0; i--) // iterate back basecells
	{
		if (h3ToParent(a, i) == h3ToParent(b, i))
			return h3ToParent(a, i);
	}

	LOG_NOTICE(0);

	return H3_ROOT_INDEX;
}

/**
 * The GiST Union method for H3 indexes
 * returns the minimal H3 index that encloses all the entries in entryvec
 */
PG_FUNCTION_INFO_V1(h3index_gist_union);
Datum
	h3index_gist_union(PG_FUNCTION_ARGS)
{
	LOG_NOTICE(0);

	GistEntryVector *entryvec = (GistEntryVector *)PG_GETARG_POINTER(0);
	GISTENTRY *entries = entryvec->vector;
	int n = entryvec->n;
	H3Index out = DatumGetH3Index(entries[0].key);
	H3Index tmp;

	// build smallest common parent
	for (int i = 1; i < n; i++)
	{
		tmp = DatumGetH3Index(entries[i].key);
		out = common_ancestor(out, tmp);
	}

	debug_func(n);

	PG_RETURN_H3INDEX(out);
}

/**
 * The GiST Consistent method for H3 indexes
 * Should return false if for all data items x below entry,
 * the predicate x op query == false, where op is the operation
 * corresponding to strategy in the pg_amop table.
 */
PG_FUNCTION_INFO_V1(h3index_gist_consistent);
Datum
	h3index_gist_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY *entry = (GISTENTRY *)PG_GETARG_POINTER(0);
	H3Index query = PG_GETARG_H3INDEX(1);
	StrategyNumber strategy = (StrategyNumber)PG_GETARG_UINT16(2);

	/* Oid subtype = PG_GETARG_OID(3); */
	bool *recheck = (bool *)PG_GETARG_POINTER(4);
	H3Index key = DatumGetH3Index(entry->key);

	/* When the result is true, a recheck flag must also be returned. */
	*recheck = true;

	switch (strategy)
	{
	case RTOverlapStrategyNumber:
		PG_RETURN_BOOL(gist_cmp(key, query) != 0);
	case RTContainsStrategyNumber:
		LOG_NOTICE(4);
		PG_RETURN_BOOL(gist_cmp(key, query) > 0);
	case RTContainedByStrategyNumber:
		LOG_NOTICE(5);
		if (GIST_LEAF(entry))
		{
			PG_RETURN_BOOL(gist_cmp(key, query) < 0);
		}
		LOG_NOTICE(6);
		LOG_NOTICE(gist_cmp(key, query));
		/* internal nodes, just check if we overlap */
		PG_RETURN_BOOL(gist_cmp(key, query) != 0);
	default:
		ereport(ERROR, (
						   errcode(ERRCODE_INTERNAL_ERROR),
						   errmsg("unrecognized StrategyNumber: %d", strategy)));
	}
}

/**
 * GiST Compress and Decompress methods for H3Indexes
 * do not do anything. We *could* use compact/uncompact?
 */
PG_FUNCTION_INFO_V1(h3index_gist_compress);
Datum
	h3index_gist_compress(PG_FUNCTION_ARGS)
{
	LOG_NOTICE(0);
	PG_RETURN_DATUM(PG_GETARG_DATUM(0));
}

PG_FUNCTION_INFO_V1(h3index_gist_decompress);
Datum
	h3index_gist_decompress(PG_FUNCTION_ARGS)
{
	LOG_NOTICE(0);
	PG_RETURN_POINTER(PG_GETARG_POINTER(0));
}

/*
** The GiST Penalty method for H3 indexes
** We use change resolution as our penalty metric
*/
PG_FUNCTION_INFO_V1(h3index_gist_penalty);
Datum
	h3index_gist_penalty(PG_FUNCTION_ARGS)
{
	LOG_NOTICE(0);
	GISTENTRY *origentry = (GISTENTRY *)PG_GETARG_POINTER(0);
	GISTENTRY *newentry = (GISTENTRY *)PG_GETARG_POINTER(1);
	float *penalty = (float *)PG_GETARG_POINTER(2);

	H3Index orig = DatumGetH3Index(origentry->key);
	H3Index new = DatumGetH3Index(newentry->key);

	H3Index ancestor = common_ancestor(orig, new);

	*penalty = (float)h3GetResolution(orig) - h3GetResolution(ancestor);

	debug_func(*penalty);

	PG_RETURN_POINTER(penalty);
}

/**
 * The GiST PickSplit method for H3 indexes
 * 
 * given a full page;
 * split into two new pages, each with a new
 */
PG_FUNCTION_INFO_V1(h3index_gist_picksplit);
Datum
	h3index_gist_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *)PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *)PG_GETARG_POINTER(1);

	LOG_NOTICE(entryvec->n);
	OffsetNumber maxoff = entryvec->n - 1;
	GISTENTRY *ent = entryvec->vector;
	int i,
		nbytes;
	OffsetNumber *left,
		*right;
	H3Index tmp_union,
		unionL,
		unionR;
	GISTENTRY **raw_entryvec;

	bool lset = false, rset = false;

	nbytes = (maxoff + 1) * sizeof(OffsetNumber);

	v->spl_left = (OffsetNumber *)palloc(nbytes);
	left = v->spl_left;
	v->spl_nleft = 0;

	v->spl_right = (OffsetNumber *)palloc(nbytes);
	right = v->spl_right;
	v->spl_nright = 0;

	unionL = NULL;
	unionR = NULL;

	/* Initialize the raw entry vector. */
	raw_entryvec = (GISTENTRY **)malloc(entryvec->n * sizeof(void *));
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
		raw_entryvec[i] = &(ent[i]);

	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		LOG_NOTICE(i);
		int real_index = raw_entryvec[i] - ent;

		tmp_union = DatumGetH3Index(ent[real_index].key);
		//PRINT_H3INDEX(tmp_union);
		//Assert(tmp_union != NULL);
		/*
		 * Choose where to put the index entries and update unionL and unionR
		 * accordingly. Append the entries to either v_spl_left or
		 * v_spl_right, and care about the counters.
		 */

		if (v->spl_nleft < v->spl_nright)
		{
			LOG_NOTICE(5);
			if (lset == false)
			{
				lset = true;
				unionL = tmp_union;
			}
			else
			{
				unionL = common_ancestor(unionL, tmp_union);
			}
			*left = real_index;
			++left;
			++(v->spl_nleft);
		}
		else
		{
			LOG_NOTICE(6);
			if (rset == false)
			{
				rset = true;
				LOG_NOTICE(7);
				//PRINT_H3INDEX(tmp_union);
				unionR = tmp_union;
			}
			else
			{
				unionR = common_ancestor(unionR, tmp_union);
			}
			LOG_NOTICE(8);
			*right = real_index;
			LOG_NOTICE(9);
			++right;
			LOG_NOTICE(10);
			++(v->spl_nright);
			LOG_NOTICE(11);
		}
	}

	debug_func(maxoff);

	v->spl_ldatum = H3IndexGetDatum(unionL);
	v->spl_rdatum = H3IndexGetDatum(unionR);
	PG_RETURN_POINTER(v);
}

/**
 * Returns true if two index entries are identical, false otherwise.
 * (An “index entry” is a value of the index's storage type, not necessarily
 * the original indexed column's type.)
 */
PG_FUNCTION_INFO_V1(h3index_gist_same);
Datum
	h3index_gist_same(PG_FUNCTION_ARGS)
{
	LOG_NOTICE(0);

	H3Index a = PG_GETARG_H3INDEX(0);
	H3Index b = PG_GETARG_H3INDEX(1);
	bool *result = (bool *)PG_GETARG_POINTER(2);

	debug_func(*result);

	*result = a == b;
	PG_RETURN_POINTER(result);
}

/**
 * Given an index entry p and a query value q, this function determines the
 * index entry's “distance” from the query value. This function must be
 * supplied if the operator class contains any ordering operators. A query
 * using the ordering operator will be implemented by returning index entries
 * with the smallest “distance” values first, so the results must be consistent
 * with the operator's semantics. For a leaf index entry the result just
 * represents the distance to the index entry; for an internal tree node, the
 * result must be the smallest distance that any child entry could have.
 */
PG_FUNCTION_INFO_V1(h3index_gist_distance);
Datum
	h3index_gist_distance(PG_FUNCTION_ARGS)
{
	LOG_NOTICE(0);

	GISTENTRY *entry = (GISTENTRY *)PG_GETARG_POINTER(0);
	H3Index query = PG_GETARG_H3INDEX(1);
	StrategyNumber strategy = (StrategyNumber)PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	/* bool    *recheck = (bool *) PG_GETARG_POINTER(4); */
	H3Index key = DatumGetH3Index(entry->key);
	double retval;

	switch (strategy)
	{
	case RTKNNSearchStrategyNumber:
		retval = h3Distance(query, key);
	default:
		retval = -1;
	}

	debug_func(retval);

	PG_RETURN_FLOAT8(retval);
}

/*-------------------------------------------------------------------------
 *
 * cms_topn.c
 *
 * This file contains the function definitions to perform top-n, point and
 * union queries by using the count-min sketch structure.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"

#include <math.h>
#include <limits.h>

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "MurmurHash3.h"
#include "utils/array.h"
#include "utils/bytea.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/typcache.h"


#define DEFAULT_ERROR_BOUND 0.001
#define DEFAULT_CONFIDENCE_INTERVAL 0.99
#define MURMUR_SEED 304837963
#define MAX_FREQUENCY ULONG_MAX
#define DEFAULT_TOPN_ITEM_SIZE 16
#define TOPN_ARRAY_OVERHEAD ARR_OVERHEAD_NONULLS(1)


/*
 * Frequency can be set to different data types to specify upper limit of counter
 * and required space for each counter. If we change type of Frequency, we also
 * need to update MAX_FREQUENCY above. Such as if Frequency is defined as uint32,
 * then MAX_FREQUENCY has to be UINT_MAX.
 */
typedef uint64 Frequency;

/*
 * CmsTopn is the main struct for the count-min sketch top-n implementation and
 * it has two variable length fields. First one is an array for keeping the sketch
 * which is pointed by "sketch" of the struct and second one is an ArrayType for
 * keeping the most frequent n items. It is not possible to lay out two variable
 * width fields consecutively in memory. So we are using pointer arithmetic to
 * reach and handle ArrayType for the most frequent n items.
 */
typedef struct CmsTopn
{
	char length[4];
	uint32 sketchDepth;
	uint32 sketchWidth;
	uint32 topnItemCount;
	uint32 topnItemSize;
	Frequency minFrequencyOfTopnItems;
	Frequency sketch[1];
} CmsTopn;

/*
 * FrequentTopnItem is the struct to keep frequent items and their frequencies
 * together.
 * It is useful to sort the top-n items before returning in topn() function.
 */
typedef struct FrequentTopnItem
{
	Datum topnItem;
	Frequency topnItemFrequency;
} FrequentTopnItem;


/* local functions forward declarations */
static CmsTopn * CreateCmsTopn(int32 topnItemCount, float8 errorBound,
                               float8 confidenceInterval);
static CmsTopn * UpdateCmsTopn(CmsTopn *currentCmsTopn, Datum newItem,
                               TypeCacheEntry *newItemTypeCacheEntry);
static ArrayType * TopnArray(CmsTopn *cmsTopn);
static Frequency UpdateSketchInPlace(CmsTopn *cmsTopn, Datum newItem,
                                     TypeCacheEntry *newItemTypeCacheEntry);
static void ConvertDatumToBytes(Datum datum, TypeCacheEntry *datumTypeCacheEntry,
                                StringInfo datumString);
static Frequency CmsTopnEstimateHashedItemFrequency(CmsTopn *cmsTopn,
                                                    uint64 *hashValueArray);
static ArrayType * UpdateTopnArray(CmsTopn *cmsTopn, Datum candidateItem,
                                   TypeCacheEntry *itemTypeCacheEntry,
                                   Frequency itemFrequency, bool *topnArrayUpdated);
static Frequency CmsTopnEstimateItemFrequency(CmsTopn *cmsTopn, Datum item,
                                              TypeCacheEntry *itemTypeCacheEntry);
static CmsTopn * FormCmsTopn(CmsTopn *cmsTopn, ArrayType *newTopnArray);
static CmsTopn * CmsTopnUnion(CmsTopn *firstCmsTopn, CmsTopn *secondCmsTopn,
                              TypeCacheEntry *itemTypeCacheEntry);
void SortTopnItems(FrequentTopnItem *topnItemArray, int topnItemCount);


/* declarations for dynamic loading */
PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(cms_topn_in);
PG_FUNCTION_INFO_V1(cms_topn_out);
PG_FUNCTION_INFO_V1(cms_topn_recv);
PG_FUNCTION_INFO_V1(cms_topn_send);
PG_FUNCTION_INFO_V1(cms_topn);
PG_FUNCTION_INFO_V1(cms_topn_add);
PG_FUNCTION_INFO_V1(cms_topn_add_agg);
PG_FUNCTION_INFO_V1(cms_topn_add_agg_with_parameters);
PG_FUNCTION_INFO_V1(cms_topn_union);
PG_FUNCTION_INFO_V1(cms_topn_union_agg);
PG_FUNCTION_INFO_V1(cms_topn_frequency);
PG_FUNCTION_INFO_V1(cms_topn_info);
PG_FUNCTION_INFO_V1(topn);


/*
 * cms_topn_in creates cms_topn from printable representation.
 */
Datum
cms_topn_in(PG_FUNCTION_ARGS)
{
	Datum datum = DirectFunctionCall1(byteain, PG_GETARG_DATUM(0));

	return datum;
}


/*
 *  cms_topn_out converts cms_topn to printable representation.
 */
Datum
cms_topn_out(PG_FUNCTION_ARGS)
{
	Datum datum = DirectFunctionCall1(byteaout, PG_GETARG_DATUM(0));

	PG_RETURN_CSTRING(datum);
}


/*
 * cms_topn_recv creates cms_topn from external binary format.
 */
Datum
cms_topn_recv(PG_FUNCTION_ARGS)
{
	Datum datum = DirectFunctionCall1(bytearecv, PG_GETARG_DATUM(0));

	return datum;
}


/*
 * cms_topn_send converts cms_topn to external binary format.
 */
Datum
cms_topn_send(PG_FUNCTION_ARGS)
{
	Datum datum = DirectFunctionCall1(byteasend, PG_GETARG_DATUM(0));

	return datum;
}


/*
 * cms_topn is a user-facing UDF which creates new cms_topn with given parameters.
 * The first parameter is for the number of top items which will be kept, the others
 * are for error bound(e) and confidence interval(p) respectively. Given e and p,
 * estimated frequency can be at most (e*||a||) more than real frequency with the
 * probability p while ||a|| is the sum of frequencies of all items according to
 * this paper: http://dimacs.rutgers.edu/~graham/pubs/papers/cm-full.pdf.
 */
Datum
cms_topn(PG_FUNCTION_ARGS)
{
	int32 topnItemCount = PG_GETARG_UINT32(0);
	float8 errorBound = PG_GETARG_FLOAT8(1);
	float8 confidenceInterval =  PG_GETARG_FLOAT8(2);

	CmsTopn *cmsTopn = CreateCmsTopn(topnItemCount, errorBound, confidenceInterval);

	PG_RETURN_POINTER(cmsTopn);
}


/*
 * CmsTopnCreate creates CmsTopn structure with given parameters. The first parameter
 * is for the number of frequent items, other two specifies error bound and confidence
 * interval for this error bound. Size of the sketch is determined with the given
 * error bound and confidence interval according to formula in this paper:
 * http://dimacs.rutgers.edu/~graham/pubs/papers/cm-full.pdf.
 *
 * Note that here we also allocate initial memory for the ArrayType which keeps
 * the frequent items. This allocation includes ArrayType overhead for one dimensional
 * array and additional memory according to number of top-n items and default size
 * for an individual item.
 */
static CmsTopn *
CreateCmsTopn(int32 topnItemCount, float8 errorBound, float8 confidenceInterval)
{
	CmsTopn *cmsTopn = NULL;
	uint32 sketchWidth = 0;
	uint32 sketchDepth = 0;
	Size staticStructSize = 0;
	Size sketchSize = 0;
	Size topnItemsReservedSize = 0;
	Size topnArrayReservedSize = 0;
	Size totalCmsTopnSize = 0;

	if (topnItemCount <= 0)
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		                errmsg("invalid parameters for cms_topn"),
		                errhint("Number of top items has to be positive")));
	}
	else if (errorBound <= 0 || errorBound >= 1)
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		                errmsg("invalid parameters for cms_topn"),
		                errhint("Error bound has to be between 0 and 1")));
	}
	else if (confidenceInterval <= 0 || confidenceInterval >= 1)
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		                errmsg("invalid parameters for cms_topn"),
		                errhint("Confidence interval has to be between 0 and 1")));
	}

	sketchWidth = (uint32) ceil(exp(1) / errorBound);
	sketchDepth = (uint32) ceil(log(1 / (1 - confidenceInterval)));
	sketchSize =  sizeof(Frequency) * sketchDepth * sketchWidth;
	staticStructSize = sizeof(CmsTopn);
	topnItemsReservedSize = topnItemCount * DEFAULT_TOPN_ITEM_SIZE;
	topnArrayReservedSize = TOPN_ARRAY_OVERHEAD + topnItemsReservedSize;
	totalCmsTopnSize = staticStructSize + sketchSize + topnArrayReservedSize;

	cmsTopn = palloc0(totalCmsTopnSize);
	cmsTopn->sketchDepth = sketchDepth;
	cmsTopn->sketchWidth = sketchWidth;
	cmsTopn->topnItemCount = topnItemCount;
	cmsTopn->topnItemSize = DEFAULT_TOPN_ITEM_SIZE;
	cmsTopn->minFrequencyOfTopnItems = 0;

	SET_VARSIZE(cmsTopn, totalCmsTopnSize);

	return cmsTopn;
}


/*
 * cms_topn_add is a user-facing UDF which inserts new item to the given CmsTopn.
 * The first parameter is for the CmsTopn to add the new item and second is for
 * the new item. This function returns updated CmsTopn.
 */
Datum
cms_topn_add(PG_FUNCTION_ARGS)
{
	CmsTopn *currentCmsTopn = NULL;
	CmsTopn *updatedCmsTopn = NULL;
	Datum newItem = 0;
	TypeCacheEntry *newItemTypeCacheEntry = NULL;
	Oid newItemType = InvalidOid;

	/* check whether cms_topn is null */
	if (PG_ARGISNULL(0))
	{
		PG_RETURN_NULL();
	}
	else
	{
		currentCmsTopn = (CmsTopn *) PG_GETARG_VARLENA_P(0);
	}

	/* if new item is null, then return current CmsTopn */
	if (PG_ARGISNULL(1))
	{
		PG_RETURN_POINTER(currentCmsTopn);
	}

	/* get item type and check if it is valid */
	newItemType = get_fn_expr_argtype(fcinfo->flinfo, 1);
	if (newItemType == InvalidOid)
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		                errmsg("could not determine input data type")));
	}

	newItem = PG_GETARG_DATUM(1);
	newItemTypeCacheEntry = lookup_type_cache(newItemType, 0);
	updatedCmsTopn = UpdateCmsTopn(currentCmsTopn, newItem, newItemTypeCacheEntry);

	PG_RETURN_POINTER(updatedCmsTopn);
}


/*
 * UpdateCmsTopn is a helper function to add new item to CmsTopn structure. It
 * adds the item to the sketch, calculates its frequency, and updates the top-n
 * array. Finally it forms new CmsTopn from updated sketch and updated top-n array.
 */
static CmsTopn *
UpdateCmsTopn(CmsTopn *currentCmsTopn, Datum newItem,
              TypeCacheEntry *newItemTypeCacheEntry)
{
	CmsTopn *updatedCmsTopn = NULL;
	ArrayType *currentTopnArray = NULL;
	ArrayType *updatedTopnArray = NULL;
	Datum detoastedItem = 0;
	Oid newItemType = newItemTypeCacheEntry->type_id;
	Oid currentItemType = InvalidOid;
	Frequency newItemFrequency = 0;
	bool topnArrayUpdated = false;
	Size currentTopnArrayLength = 0;

	currentTopnArray = TopnArray(currentCmsTopn);
	currentTopnArrayLength = ARR_DIMS(currentTopnArray)[0];
	currentItemType = ARR_ELEMTYPE(currentTopnArray);

	if (currentTopnArrayLength != 0 && currentItemType != newItemType)
	{
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
		                errmsg("not proper type for this cms_topn")));
	}

	/* if datum is toasted, detoast it */
	if (newItemTypeCacheEntry->typlen == -1)
	{
		detoastedItem = PointerGetDatum(PG_DETOAST_DATUM(newItem));
	}
	else
	{
		detoastedItem = newItem;
	}

	newItemFrequency = UpdateSketchInPlace(currentCmsTopn, detoastedItem,
	                                       newItemTypeCacheEntry);
	updatedTopnArray = UpdateTopnArray(currentCmsTopn, detoastedItem,
	                                   newItemTypeCacheEntry, newItemFrequency,
	                                   &topnArrayUpdated);

	/* we only form a new CmsTopn only if top-n array is updated */
	if (topnArrayUpdated)
	{
		updatedCmsTopn = FormCmsTopn(currentCmsTopn, updatedTopnArray);
	}
	else
	{
		updatedCmsTopn = currentCmsTopn;
	}

	return updatedCmsTopn;
}


/*
 * TopnArray returns pointer for the ArrayType which is kept in CmsTopn structure
 * by calculating its place with pointer arithmetic.
 */
static ArrayType *
TopnArray(CmsTopn *cmsTopn)
{
	Size staticSize = sizeof(CmsTopn);
	Size sketchSize = sizeof(Frequency) * cmsTopn->sketchDepth * cmsTopn->sketchWidth;
	Size sizeWithoutTopnArray = staticSize + sketchSize;
	ArrayType *topnArray = (ArrayType *) (((char *) cmsTopn) + sizeWithoutTopnArray);

	return topnArray;
}


/*
 * UpdateSketchInPlace updates sketch inside CmsTopn in-place with given item
 * and returns new estimated frequency for the given item.
 */
static Frequency
UpdateSketchInPlace(CmsTopn *cmsTopn, Datum newItem,
                    TypeCacheEntry *newItemTypeCacheEntry)
{
	uint32 hashIndex = 0;
	uint64 hashValueArray[2] = {0, 0};
	StringInfo newItemString = makeStringInfo();
	Frequency newFrequency = 0;
	Frequency minFrequency = MAX_FREQUENCY;

	/* get hashed values for the given item */
	ConvertDatumToBytes(newItem, newItemTypeCacheEntry, newItemString);
	MurmurHash3_x64_128(newItemString->data, newItemString->len, MURMUR_SEED,
	                    &hashValueArray);

	/*
	 * Estimate frequency of the given item from hashed values and calculate new
	 * frequency for this item.
	 */
	minFrequency = CmsTopnEstimateHashedItemFrequency(cmsTopn, hashValueArray);
	newFrequency = minFrequency + 1;

	/*
	 * We can create an independent hash function for each index by using two hash
	 * values from the Murmur Hash function. This is a standard technique from the
	 * hashing literature for the additional hash functions of the form
	 * g(x) = h1(x) + i * h2(x) and does not hurt the independence between hash
	 * function. For more information you can check this paper:
	 * https://www.eecs.harvard.edu/~michaelm/postscripts/tr-02-05.pdf
	 */
	for (hashIndex = 0; hashIndex < cmsTopn->sketchDepth; hashIndex++)
	{
		uint64 hashValue = hashValueArray[0] + (hashIndex * hashValueArray[1]);
		uint32 widthIndex = hashValue % cmsTopn->sketchWidth;
		uint32 depthOffset = hashIndex * cmsTopn->sketchWidth;
		uint32 counterIndex = depthOffset + widthIndex;

		/*
		 * Selective update to decrease effect of collisions. We only update
		 * counters less than new frequency because other counters are bigger
		 * due to collisions.
		 */
		Frequency counterFrequency = cmsTopn->sketch[counterIndex];
		if (newFrequency > counterFrequency)
		{
			cmsTopn->sketch[counterIndex] = newFrequency;
		}
	}

	return newFrequency;
}


/*
 * ConvertDatumToBytes converts datum to byte array and saves it in the given
 * datum string.
 */
static void
ConvertDatumToBytes(Datum datum, TypeCacheEntry *datumTypeCacheEntry,
                    StringInfo datumString)
{
	int16 datumTypeLength = datumTypeCacheEntry->typlen;
	bool datumTypeByValue = datumTypeCacheEntry->typbyval;
	Size datumSize = 0;

	if (datumTypeLength == -1)
	{
		datumSize = VARSIZE_ANY_EXHDR(DatumGetPointer(datum));
	}
	else
	{
		datumSize = datumGetSize(datum, datumTypeByValue, datumTypeLength);
	}

	if (datumTypeByValue)
	{
		appendBinaryStringInfo(datumString, (char *) &datum, datumSize);
	}
	else
	{
		appendBinaryStringInfo(datumString, VARDATA_ANY(datum), datumSize);
	}
}


/*
 * CmsTopnEstimateHashedItemFrequency is a helper function to get frequency
 * estimate of an item from it's hashed values.
 */
static Frequency
CmsTopnEstimateHashedItemFrequency(CmsTopn *cmsTopn, uint64 *hashValueArray)
{
	uint32 hashIndex = 0;
	Frequency minFrequency = MAX_FREQUENCY;

	for (hashIndex = 0; hashIndex < cmsTopn->sketchDepth; hashIndex++)
	{
		uint64 hashValue = hashValueArray[0] + (hashIndex * hashValueArray[1]);
		uint32 widthIndex = hashValue % cmsTopn->sketchWidth;
		uint32 depthOffset = hashIndex * cmsTopn->sketchWidth;
		uint32 counterIndex = depthOffset + widthIndex;

		Frequency counterFrequency = cmsTopn->sketch[counterIndex];
		if (counterFrequency < minFrequency)
		{
			minFrequency = counterFrequency;
		}
	}

	return minFrequency;
}


/*
 * UpdateTopnArray is a helper function for the unions and inserts. It takes
 * given item and its frequency. If the item is not in the top-n array, it tries
 * to insert new item. If there is place in the top-n array, it insert directly.
 * Otherwise, it compares its frequency with the minimum of current items in the
 * array and updates top-n array if new frequency is bigger.
 */
static ArrayType *
UpdateTopnArray(CmsTopn *cmsTopn, Datum candidateItem, TypeCacheEntry *itemTypeCacheEntry,
                Frequency itemFrequency, bool *topnArrayUpdated)
{
	ArrayType *currentTopnArray = TopnArray(cmsTopn);
	ArrayType *updatedTopnArray = NULL;
	int16 itemTypeLength = itemTypeCacheEntry->typlen;
	bool itemTypeByValue = itemTypeCacheEntry->typbyval;
	char itemTypeAlignment = itemTypeCacheEntry->typalign;
	Frequency minOfNewTopnItems = MAX_FREQUENCY;
	bool candidateAlreadyInArray = false;
	int candidateIndex = -1;

	int currentArrayLength = ARR_DIMS(currentTopnArray)[0];
	if (currentArrayLength == 0)
	{
		Oid itemType = itemTypeCacheEntry->type_id;
		if (itemTypeCacheEntry->typtype == TYPTYPE_COMPOSITE)
		{
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			                errmsg("composite types are not supported")));
		}

		currentTopnArray = construct_empty_array(itemType);
	}

	/*
	 * If item frequency is smaller than min frequency of old top-n items,
	 * it cannot be in the top-n items and we insert it only if there is place.
	 * Otherwise, we need to check new minimum and whether it is in the top-n.
	 */
	if (itemFrequency <= cmsTopn->minFrequencyOfTopnItems)
	{
		if (currentArrayLength < cmsTopn->topnItemCount)
		{
			candidateIndex =  currentArrayLength + 1;
			minOfNewTopnItems = itemFrequency;
		}
	}
	else
	{
		ArrayIterator iterator = array_create_iterator(currentTopnArray, 0);
		Datum topnItem = 0;
		int topnItemIndex = 1;
		int minItemIndex = 1;
		bool hasMoreItem = false;
		bool isNull = false;

		/*
		 * Find the top-n item with minimum frequency to replace it with candidate
		 * item.
		 */
		hasMoreItem = array_iterate(iterator, &topnItem, &isNull);
		while (hasMoreItem)
		{
			Frequency topnItemFrequency = 0;

			/* check if we already have this item in top-n array */
			candidateAlreadyInArray = datumIsEqual(topnItem, candidateItem,
			                                       itemTypeByValue, itemTypeLength);
			if (candidateAlreadyInArray)
			{
				minItemIndex = -1;
				break;
			}

			/* keep track of minumum frequency item in the top-n array */
			topnItemFrequency = CmsTopnEstimateItemFrequency(cmsTopn, topnItem,
			                                                 itemTypeCacheEntry);
			if (topnItemFrequency < minOfNewTopnItems)
			{
				minOfNewTopnItems = topnItemFrequency;
				minItemIndex = topnItemIndex;
			}

			hasMoreItem = array_iterate(iterator, &topnItem, &isNull);
			topnItemIndex++;
		}

		/* if new item is not in the top-n and there is place, insert the item */
		if (!candidateAlreadyInArray && currentArrayLength < cmsTopn->topnItemCount)
		{
			minItemIndex = currentArrayLength + 1;
			minOfNewTopnItems = Min(minOfNewTopnItems, itemFrequency);
		}

		candidateIndex = minItemIndex;
	}

	/*
	 * If it is not in the top-n structure and its frequency bigger than minimum
	 * put into top-n instead of the item which has minimum frequency. If it is
	 * in top-n or not frequent items, do not change anything.
	 */
	if (!candidateAlreadyInArray && minOfNewTopnItems <= itemFrequency)
	{
		updatedTopnArray = array_set(currentTopnArray, 1, &candidateIndex,
		                             candidateItem, false, -1, itemTypeLength,
		                             itemTypeByValue, itemTypeAlignment);
		cmsTopn->minFrequencyOfTopnItems = minOfNewTopnItems;
		*topnArrayUpdated = true;
	}
	else
	{
		updatedTopnArray = currentTopnArray;
		*topnArrayUpdated = false;
	}

	return updatedTopnArray;
}


/*
 * CmsTopnEstimateItemFrequency calculates estimated frequency for the given
 * item and returns it.
 */
static Frequency
CmsTopnEstimateItemFrequency(CmsTopn *cmsTopn, Datum item,
                             TypeCacheEntry *itemTypeCacheEntry)
{
	uint64 hashValueArray[2] = {0, 0};
	StringInfo itemString = makeStringInfo();
	Frequency frequency = 0;

	/* if datum is toasted, detoast it */
	if (itemTypeCacheEntry->typlen == -1)
	{
		Datum detoastedItem =  PointerGetDatum(PG_DETOAST_DATUM(item));
		ConvertDatumToBytes(detoastedItem, itemTypeCacheEntry, itemString);
	}
	else
	{
		ConvertDatumToBytes(item, itemTypeCacheEntry, itemString);
	}

	/*
	 * Calculate hash values for the given item and then get frequency estimate
	 * with these hashed values.
	 */
	MurmurHash3_x64_128(itemString->data, itemString->len, MURMUR_SEED, &hashValueArray);
	frequency = CmsTopnEstimateHashedItemFrequency(cmsTopn, hashValueArray);

	return frequency;
}


/*
 * FormCmsTopn copies current count-min sketch and new top-n array into a new
 * CmsTopn. This function is called only when there is an update in top-n and it
 * only copies ArrayType part if allocated memory is enough for new ArrayType.
 * Otherwise, it allocates new memory and copies all CmsTopn.
 */
static CmsTopn *
FormCmsTopn(CmsTopn *cmsTopn, ArrayType *newTopnArray)
{
	Size staticSize = sizeof(CmsTopn);
	Size sketchSize = cmsTopn->sketchDepth * cmsTopn->sketchWidth * sizeof(Frequency);
	Size sizeWithoutTopnArray = staticSize + sketchSize;
	Size topnArrayReservedSize = VARSIZE(cmsTopn) - sizeWithoutTopnArray;
	Size newTopnArraySize = ARR_SIZE(newTopnArray);
	Size newCmsTopnSize = 0;
	char *newCmsTopn = NULL;
	char *topnArrayOffset = NULL;

	/*
	 * Check whether we have enough memory for new top-n array. If not, we need
	 * to allocate more memory and copy CmsTopn and new top-n array.
	 */
	if (newTopnArraySize > topnArrayReservedSize)
	{
		Size newItemsReservedSize = 0;
		Size newTopnArrayReservedSize = 0;
		uint32 topnItemCount = cmsTopn->topnItemCount;

		/*
		 * Calculate average top-n item size in the new top-n array and use
		 * two times of it for each top-n item while allocating new memory.
		 */
		Size averageTopnItemSize = (Size) (newTopnArraySize / topnItemCount);
		Size topnItemSize = averageTopnItemSize * 2;
		cmsTopn->topnItemSize = topnItemSize;

		newItemsReservedSize = topnItemCount * topnItemSize;
		newTopnArrayReservedSize = TOPN_ARRAY_OVERHEAD + newItemsReservedSize;
		newCmsTopnSize = sizeWithoutTopnArray + newTopnArrayReservedSize;
		newCmsTopn = palloc0(newCmsTopnSize);

		/* first copy until to top-n array */
		memcpy(newCmsTopn, (char *)cmsTopn, sizeWithoutTopnArray);

		/* set size of new CmsTopn */
		SET_VARSIZE(newCmsTopn, newCmsTopnSize);
	}
	else
	{
		newCmsTopn = (char *)cmsTopn;
	}

	/* finally copy new top-n array */
	topnArrayOffset = ((char *) newCmsTopn) + sizeWithoutTopnArray;
	memcpy(topnArrayOffset, newTopnArray, ARR_SIZE(newTopnArray));

	return (CmsTopn *) newCmsTopn;
}


/*
 * cms_topn_add_agg is aggregate function to add items in a cms_topn. It uses
 * default values for error bound, confidence interval and given top-n item
 * count to create initial cms_topn. The first parameter for the CmsTopn which
 * is updated during the aggregation, the second one is for the items to add and
 * third one specifies number of items to be kept in top-n array.
 */
Datum
cms_topn_add_agg(PG_FUNCTION_ARGS)
{
	CmsTopn *currentCmsTopn = NULL;
	CmsTopn *updatedCmsTopn = NULL;
	uint32 topnItemCount = PG_GETARG_UINT32(2);
	float8 errorBound = DEFAULT_ERROR_BOUND;
	float8 confidenceInterval = DEFAULT_CONFIDENCE_INTERVAL;
	Datum newItem = 0;
	TypeCacheEntry *newItemTypeCacheEntry = NULL;
	Oid newItemType = InvalidOid;

	if (!AggCheckCallContext(fcinfo, NULL))
	{
		ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
		                errmsg("cms_topn_add_agg called in non-aggregate context")));
	}

	/* check whether cms_topn is null and create if it is */
	if (PG_ARGISNULL(0))
	{
		currentCmsTopn = CreateCmsTopn(topnItemCount, errorBound, confidenceInterval);
	}
	else
	{
		currentCmsTopn = (CmsTopn *) PG_GETARG_VARLENA_P(0);
	}

	/* if new item is null, return current CmsTopn */
	if (PG_ARGISNULL(1))
	{
		PG_RETURN_POINTER(currentCmsTopn);
	}

	/*
	 * Keep type cache entry between subsequent calls in order to get rid of
	 * cache lookup overhead.
	 */
	newItem = PG_GETARG_DATUM(1);
	if (fcinfo->flinfo->fn_extra == NULL)
	{
		newItemType = get_fn_expr_argtype(fcinfo->flinfo, 1);
		newItemTypeCacheEntry = lookup_type_cache(newItemType, 0);
		fcinfo->flinfo->fn_extra = newItemTypeCacheEntry;
	}
	else
	{
		newItemTypeCacheEntry = fcinfo->flinfo->fn_extra;
	}

	updatedCmsTopn = UpdateCmsTopn(currentCmsTopn, newItem, newItemTypeCacheEntry);

	PG_RETURN_POINTER(updatedCmsTopn);
}


/*
 * cms_topn_add_agg_with_parameters is a aggregate function to add items. It
 * allows to specify parameters of created CmsTopn structure. In addition to
 * cms_topn_add_agg function, it takes error bound and confidence interval
 * parameters as the forth and fifth parameters.
 */
Datum
cms_topn_add_agg_with_parameters(PG_FUNCTION_ARGS)
{
	CmsTopn *currentCmsTopn = NULL;
	CmsTopn *updatedCmsTopn = NULL;
	uint32 topnItemCount = PG_GETARG_UINT32(2);
	float8 errorBound = PG_GETARG_FLOAT8(3);
	float8 confidenceInterval = PG_GETARG_FLOAT8(4);
	Datum newItem = 0;
	TypeCacheEntry *newItemTypeCacheEntry = NULL;
	Oid newItemType = InvalidOid;

	if (!AggCheckCallContext(fcinfo, NULL))
	{
		ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
		                errmsg("cms_topn_add_agg_with_parameters called in "
		                       "non-aggregate context")));
	}

	/* check whether cms_topn is null and create if it is */
	if (PG_ARGISNULL(0))
	{
		currentCmsTopn = CreateCmsTopn(topnItemCount, errorBound, confidenceInterval);
	}
	else
	{
		currentCmsTopn = (CmsTopn *) PG_GETARG_VARLENA_P(0);
	}

	/* if new item is null, return current CmsTopn */
	if (PG_ARGISNULL(1))
	{
		PG_RETURN_POINTER(currentCmsTopn);
	}

	/*
	 * Keep type cache entry between subsequent calls in order to get rid of
	 * cache lookup overhead.
	 */
	newItem = PG_GETARG_DATUM(1);
	if (fcinfo->flinfo->fn_extra == NULL)
	{
		newItemType = get_fn_expr_argtype(fcinfo->flinfo, 1);
		newItemTypeCacheEntry = lookup_type_cache(newItemType, 0);
		fcinfo->flinfo->fn_extra = newItemTypeCacheEntry;
	}
	else
	{
		newItemTypeCacheEntry = fcinfo->flinfo->fn_extra;
	}

	updatedCmsTopn = UpdateCmsTopn(currentCmsTopn, newItem, newItemTypeCacheEntry);

	PG_RETURN_POINTER(updatedCmsTopn);
}


/*
 * cms_topn_union is a user-facing UDF which takes two cms_topn and returns
 * their union.
 */
Datum
cms_topn_union(PG_FUNCTION_ARGS)
{
	CmsTopn *firstCmsTopn = NULL;
	CmsTopn *secondCmsTopn = NULL;
	CmsTopn *newCmsTopn = NULL;
	ArrayType *firstTopnArray = NULL;
	Size firstTopnArrayLength = 0;
	TypeCacheEntry *itemTypeCacheEntry = NULL;

	/*
	 * If both cms_topn is null, it returns null. If one of the cms_topn's is
	 * null, it returns other cms_topn.
	 */
	if (PG_ARGISNULL(0) && PG_ARGISNULL(1))
	{
		PG_RETURN_NULL();
	}
	else if (PG_ARGISNULL(0))
	{
		secondCmsTopn = (CmsTopn *) PG_GETARG_VARLENA_P(1);
		PG_RETURN_POINTER(secondCmsTopn);
	}
	else if (PG_ARGISNULL(1))
	{
		firstCmsTopn = (CmsTopn *) PG_GETARG_VARLENA_P(0);
		PG_RETURN_POINTER(firstCmsTopn);
	}

	firstCmsTopn = (CmsTopn *) PG_GETARG_VARLENA_P(0);
	secondCmsTopn = (CmsTopn *) PG_GETARG_VARLENA_P(1);
	firstTopnArray = TopnArray(firstCmsTopn);
	firstTopnArrayLength = ARR_DIMS(firstTopnArray)[0];

	if (firstTopnArrayLength != 0)
	{
		Oid itemType = firstTopnArray->elemtype;
		itemTypeCacheEntry = lookup_type_cache(itemType, 0);
	}

	newCmsTopn = CmsTopnUnion(firstCmsTopn, secondCmsTopn, itemTypeCacheEntry);

	PG_RETURN_POINTER(newCmsTopn);
}


/*
 * CmsTopnUnion is a helper function for union operations. It first sums up two
 * sketchs and iterates through the top-n array of the second cms_topn to update
 * the top-n of union.
 */
static CmsTopn *
CmsTopnUnion(CmsTopn *firstCmsTopn, CmsTopn *secondCmsTopn,
             TypeCacheEntry *itemTypeCacheEntry)
{
	ArrayType *firstTopnArray = TopnArray(firstCmsTopn);
	ArrayType *secondTopnArray = TopnArray(secondCmsTopn);
	ArrayType *newTopnArray = NULL;
	Size firstTopnArrayLength = ARR_DIMS(firstTopnArray)[0];
	Size secondTopnArrayLength = ARR_DIMS(secondTopnArray)[0];
	Size sketchSize = 0;
	CmsTopn *newCmsTopn = NULL;
	uint32 topnItemIndex = 0;
	Datum topnItem = 0;
	ArrayIterator secondTopnArrayIterator = NULL;
	bool isNull = false;
	bool hasMoreItem = false;

	if (firstCmsTopn->sketchDepth != secondCmsTopn->sketchDepth ||
	    firstCmsTopn->sketchWidth != secondCmsTopn->sketchWidth ||
	    firstCmsTopn->topnItemCount != secondCmsTopn->topnItemCount)
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		                errmsg("cannot merge cms_topns with different parameters")));
	}

	if (firstTopnArrayLength == 0)
	{
		newCmsTopn = secondCmsTopn;
	}
	else if (secondTopnArrayLength == 0)
	{
		newCmsTopn = firstCmsTopn;
	}
	else if (ARR_ELEMTYPE(firstTopnArray) != ARR_ELEMTYPE(secondTopnArray))
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		                errmsg("cannot merge cms_topns of different types")));
	}
	else
	{
		/* for performance reasons we use first cms_topn to merge two cms_topn's */
		newCmsTopn = firstCmsTopn;
		newTopnArray = firstTopnArray;

		sketchSize = newCmsTopn->sketchDepth * newCmsTopn->sketchWidth;
		for (topnItemIndex = 0; topnItemIndex < sketchSize; topnItemIndex++)
		{
			newCmsTopn->sketch[topnItemIndex] += secondCmsTopn->sketch[topnItemIndex];
		}

		secondTopnArrayIterator = array_create_iterator(secondTopnArray, 0);

		/*
		 * One by one we add top-n items in second top-n array to top-n array of
		 * first(new) top-n array. As a result only top-n elements of two item
		 * list are kept.
		 */
		hasMoreItem = array_iterate(secondTopnArrayIterator, &topnItem, &isNull);
		while (hasMoreItem)
		{
			Frequency newItemFrequency = 0;
			bool topnArrayUpdated = false;

			newItemFrequency = CmsTopnEstimateItemFrequency(newCmsTopn, topnItem,
			                                                itemTypeCacheEntry);
			newTopnArray = UpdateTopnArray(newCmsTopn, topnItem, itemTypeCacheEntry,
			                               newItemFrequency, &topnArrayUpdated);
			if (topnArrayUpdated)
			{
				newCmsTopn = FormCmsTopn(newCmsTopn, newTopnArray);
			}

			hasMoreItem = array_iterate(secondTopnArrayIterator, &topnItem, &isNull);
		}
	}

	return newCmsTopn;
}


/*
 * cms_topn_union_agg is aggregate function to create union of cms_topns.
 */
Datum
cms_topn_union_agg(PG_FUNCTION_ARGS)
{
	CmsTopn *firstCmsTopn = NULL;
	CmsTopn *secondCmsTopn = NULL;
	CmsTopn *newCmsTopn = NULL;
	ArrayType *firstTopnArray = NULL;
	Size firstTopnArrayLength = 0;
	TypeCacheEntry *itemTypeCacheEntry = NULL;

	if (!AggCheckCallContext(fcinfo, NULL))
	{
		ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
		                errmsg("cmsketch_merge_agg_trans called in non-aggregate "
		                       "context")));
	}

	/*
	 * If both cms_topn is null, it returns null. If one of the cms_topn's is
	 * null, it returns other cms_topn.
	 */
	if (PG_ARGISNULL(0) && PG_ARGISNULL(1))
	{
		PG_RETURN_NULL();
	}
	else if (PG_ARGISNULL(0))
	{
		secondCmsTopn = (CmsTopn *) PG_GETARG_VARLENA_P(1);
		PG_RETURN_POINTER(secondCmsTopn);
	}
	else if (PG_ARGISNULL(1))
	{
		firstCmsTopn = (CmsTopn *) PG_GETARG_VARLENA_P(0);
		PG_RETURN_POINTER(firstCmsTopn);
	}

	firstCmsTopn = (CmsTopn *) PG_GETARG_VARLENA_P(0);
	secondCmsTopn = (CmsTopn *) PG_GETARG_VARLENA_P(1);
	firstTopnArray = TopnArray(firstCmsTopn);
	firstTopnArrayLength = ARR_DIMS(firstTopnArray)[0];

	/*
	 * Keep type cache entry between subsequent calls in order to get rid of
	 * cache lookup overhead.
	 */
	if (fcinfo->flinfo->fn_extra == NULL && firstTopnArrayLength != 0)
	{
		Oid itemType = firstTopnArray->elemtype;

		itemTypeCacheEntry = lookup_type_cache(itemType, 0);
		fcinfo->flinfo->fn_extra = itemTypeCacheEntry;
	}
	else
	{
		itemTypeCacheEntry = fcinfo->flinfo->fn_extra;
	}

	newCmsTopn = CmsTopnUnion(firstCmsTopn, secondCmsTopn, itemTypeCacheEntry);

	PG_RETURN_POINTER(newCmsTopn);
}


/*
 * cms_topn_frequency is a user-facing UDF which returns the estimated frequency
 * of an item. The first parameter is for CmsTopn and second is for the item to
 * return the frequency.
 */
Datum
cms_topn_frequency(PG_FUNCTION_ARGS)
{
	CmsTopn *cmsTopn = (CmsTopn *) PG_GETARG_VARLENA_P(0);
	ArrayType *topnArray = TopnArray(cmsTopn);
	Datum item = PG_GETARG_DATUM(1);
	Oid itemType = get_fn_expr_argtype(fcinfo->flinfo, 1);
	TypeCacheEntry *itemTypeCacheEntry = NULL;
	Frequency frequency = 0;

	if (itemType == InvalidOid)
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		                errmsg("could not determine input data types")));
	}

	if (topnArray != NULL && itemType != ARR_ELEMTYPE(topnArray))
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		                errmsg("Not proper type for this cms_topn")));
	}

	itemTypeCacheEntry = lookup_type_cache(itemType, 0);
	frequency = CmsTopnEstimateItemFrequency(cmsTopn, item, itemTypeCacheEntry);

	PG_RETURN_INT32(frequency);
}


/*
 * cms_topn_info returns summary about the given CmsTopn structure.
 */
Datum
cms_topn_info(PG_FUNCTION_ARGS)
{
	CmsTopn *cmsTopn = NULL;
	StringInfo cmsTopnInfoString = makeStringInfo();

	cmsTopn = (CmsTopn *) PG_GETARG_VARLENA_P(0);
	appendStringInfo(cmsTopnInfoString, "Sketch depth = %d, Sketch width = %d, "
	                 "Size = %ukB", cmsTopn->sketchDepth, cmsTopn->sketchWidth,
	                 VARSIZE(cmsTopn) / 1024);

	PG_RETURN_TEXT_P(CStringGetTextDatum(cmsTopnInfoString->data));
}


/*
 * topn is a user-facing UDF which returns the top items and their frequencies.
 * It first gets the top-n structure and converts it into the ordered array of
 * FrequentTopnItem which keeps Datums and the frequencies in the first call.
 * Then, it returns an item and its frequency according to call counter. This
 * function requires a parameter for the type because PostgreSQL has strongly
 * typed system and the type of frequent items in returning rows has to be given.
 */
Datum
topn(PG_FUNCTION_ARGS)
{
	FuncCallContext *functionCallContext = NULL;
	TupleDesc tupleDescriptor = NULL;
	Oid returningItemType = get_fn_expr_argtype(fcinfo->flinfo, 1);
	CmsTopn *cmsTopn = NULL;
	ArrayType *topnArray = NULL;
	int topnArrayLength = 0;
	Datum topnItem = 0;
	bool isNull = false;
	ArrayIterator topnIterator = NULL;
	bool hasMoreItem = false;
	int callCounter = 0;
	int maxCallCounter = 0;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext = NULL;
		Size topnArraySize = 0;
		TypeCacheEntry *itemTypeCacheEntry = NULL;
		int topnIndex = 0;
		Oid itemType = InvalidOid;
		FrequentTopnItem *sortedTopnArray = NULL;
		TupleDesc completeTupleDescriptor = NULL;

		functionCallContext = SRF_FIRSTCALL_INIT();
		if (PG_ARGISNULL(0))
		{
			SRF_RETURN_DONE(functionCallContext);
		}

		cmsTopn = (CmsTopn *)  PG_GETARG_VARLENA_P(0);
		topnArray = TopnArray(cmsTopn);
		topnArrayLength = ARR_DIMS(topnArray)[0];

		/* if there is not any element in the array just return */
		if (topnArrayLength == 0)
		{
			SRF_RETURN_DONE(functionCallContext);
		}

		itemType = ARR_ELEMTYPE(topnArray);
		if (itemType != returningItemType)
		{
			elog(ERROR, "not a proper cms_topn for the result type");
		}

		/* switch to consistent context for multiple calls */
		oldcontext = MemoryContextSwitchTo(functionCallContext->multi_call_memory_ctx);
		itemTypeCacheEntry = lookup_type_cache(itemType, 0);
		functionCallContext->max_calls = topnArrayLength;

		/* create an array to copy top-n items and sort them later */
		topnArraySize = sizeof(FrequentTopnItem) * topnArrayLength;
		sortedTopnArray = palloc0(topnArraySize);

		topnIterator = array_create_iterator(topnArray, 0);
		hasMoreItem = array_iterate(topnIterator, &topnItem, &isNull);
		while (hasMoreItem)
		{
			FrequentTopnItem frequentTopnItem;
			frequentTopnItem.topnItem = topnItem;
			frequentTopnItem.topnItemFrequency =
			        CmsTopnEstimateItemFrequency(cmsTopn, topnItem, itemTypeCacheEntry);
			sortedTopnArray[topnIndex] = frequentTopnItem;

			hasMoreItem = array_iterate(topnIterator, &topnItem, &isNull);
			topnIndex++;
		}

		SortTopnItems(sortedTopnArray, topnArrayLength);
		functionCallContext->user_fctx = sortedTopnArray;

		get_call_result_type(fcinfo, &returningItemType, &tupleDescriptor);
		completeTupleDescriptor = BlessTupleDesc(tupleDescriptor);
		functionCallContext->tuple_desc = completeTupleDescriptor;
		MemoryContextSwitchTo(oldcontext);
	}

	functionCallContext = SRF_PERCALL_SETUP();
	maxCallCounter = functionCallContext->max_calls;
	callCounter = functionCallContext->call_cntr;

	if (callCounter < maxCallCounter)
	{
		Datum *tupleValues = (Datum *) palloc(2 * sizeof(Datum));
		HeapTuple topnItemTuple;
		Datum topnItemDatum = 0;
		char *tupleNulls = (char *) palloc0(2 * sizeof(char));
		FrequentTopnItem *sortedTopnArray = NULL;
		TupleDesc completeTupleDescriptor = NULL;

		sortedTopnArray = (FrequentTopnItem *) functionCallContext->user_fctx;
		tupleValues[0] = sortedTopnArray[callCounter].topnItem;
		tupleValues[1] = sortedTopnArray[callCounter].topnItemFrequency;

		/* non-null attributes are indicated by a ' ' (space) */
		tupleNulls[0] = ' ';
		tupleNulls[1] = ' ';

		completeTupleDescriptor = functionCallContext->tuple_desc;
		topnItemTuple = heap_formtuple(completeTupleDescriptor, tupleValues, tupleNulls);

		topnItemDatum = HeapTupleGetDatum(topnItemTuple);
		SRF_RETURN_NEXT(functionCallContext, topnItemDatum);
	}
	else
	{
		SRF_RETURN_DONE(functionCallContext);
	}
}


/*
 * SortTopnItems sorts the top-n items in place according to their frequencies
 * by using selection sort.
 */
void
SortTopnItems(FrequentTopnItem *topnItemArray, int topnItemCount)
{
	int currentItemIndex = 0;
	for (currentItemIndex = 0; currentItemIndex < topnItemCount; currentItemIndex++)
	{
		FrequentTopnItem swapFrequentTopnItem;
		int candidateItemIndex = 0;

		/* use current top-n item as default max */
		Frequency maxItemFrequency = topnItemArray[currentItemIndex].topnItemFrequency;
		int maxItemIndex = currentItemIndex;

		for (candidateItemIndex = currentItemIndex + 1;
		     candidateItemIndex < topnItemCount; candidateItemIndex++)
		{
			Frequency candidateItemFrequency =
			        topnItemArray[candidateItemIndex].topnItemFrequency;
			if(candidateItemFrequency > maxItemFrequency)
			{
				maxItemFrequency = candidateItemFrequency;
				maxItemIndex = candidateItemIndex;
			}
		}

		swapFrequentTopnItem = topnItemArray[maxItemIndex];
		topnItemArray[maxItemIndex] = topnItemArray[currentItemIndex];
		topnItemArray[currentItemIndex] = swapFrequentTopnItem;
	}
}

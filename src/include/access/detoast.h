/*-------------------------------------------------------------------------
 *
 * detoast.h
 *	  Access to compressed and external varlena values.
 *
 * Copyright (c) 2000-2019, PostgreSQL Global Development Group
 *
 * src/include/access/detoast.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DETOAST_H
#define DETOAST_H

/*
 * Testing whether an externally-stored value is compressed now requires
 * comparing extsize (the actual length of the external data) to rawsize
 * (the original uncompressed datum's size).  The latter includes VARHDRSZ
 * overhead, the former doesn't.  We never use compression unless it actually
 * saves space, so we expect either equality or less-than.
 */
#define VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer) \
	((toast_pointer).va_extsize < (toast_pointer).va_rawsize - VARHDRSZ)

/*
 * Macro to fetch the possibly-unaligned contents of an EXTERNAL datum
 * into a local "struct varatt_external" toast pointer.  This should be
 * just a memcpy, but some versions of gcc seem to produce broken code
 * that assumes the datum contents are aligned.  Introducing an explicit
 * intermediate "varattrib_1b_e *" variable seems to fix it.
 */
#define VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr) \
do { \
	varattrib_1b_e *attre = (varattrib_1b_e *) (attr); \
	Assert(VARATT_IS_EXTERNAL(attre)); \
	Assert(VARSIZE_EXTERNAL(attre) == sizeof(toast_pointer) + VARHDRSZ_EXTERNAL); \
	memcpy(&(toast_pointer), VARDATA_EXTERNAL(attre), sizeof(toast_pointer)); \
} while (0)

/* Size of an EXTERNAL datum that contains a standard TOAST pointer */
#define TOAST_POINTER_SIZE (VARHDRSZ_EXTERNAL + sizeof(varatt_external))

/* Size of an EXTERNAL datum that contains an indirection pointer */
#define INDIRECT_POINTER_SIZE (VARHDRSZ_EXTERNAL + sizeof(varatt_indirect))

/* ----------
 * heap_tuple_fetch_attr() -
 *
 *		Fetches an external stored attribute from the toast
 *		relation. Does NOT decompress it, if stored external
 *		in compressed format.
 * ----------
 */
extern struct varlena *heap_tuple_fetch_attr(struct varlena *attr);

/* ----------
 * heap_tuple_untoast_attr() -
 *
 *		Fully detoasts one attribute, fetching and/or decompressing
 *		it as needed.
 * ----------
 */
extern struct varlena *heap_tuple_untoast_attr(struct varlena *attr);

/* ----------
 * heap_tuple_untoast_attr_slice() -
 *
 *		Fetches only the specified portion of an attribute.
 *		(Handles all cases for attribute storage)
 * ----------
 */
extern struct varlena *heap_tuple_untoast_attr_slice(struct varlena *attr,
							  int32 sliceoffset,
							  int32 slicelength);

/* ----------
 * toast_raw_datum_size -
 *
 *	Return the raw (detoasted) size of a varlena datum
 * ----------
 */
extern Size toast_raw_datum_size(Datum value);

/* ----------
 * toast_datum_size -
 *
 *	Return the storage size of a varlena datum
 * ----------
 */
extern Size toast_datum_size(Datum value);

#endif							/* DETOAST_H */
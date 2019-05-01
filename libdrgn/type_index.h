// Copyright 2018-2019 - Omar Sandoval
// SPDX-License-Identifier: GPL-3.0+

/**
 * @file
 *
 * Type lookup and caching.
 *
 * See @ref TypeIndex.
 */

#ifndef DRGN_TYPE_INDEX_H
#define DRGN_TYPE_INDEX_H

#include <elfutils/libdw.h>

#include "drgn.h"
#include "hash_table.h"
#include "language.h"
#include "type.h"

/**
 * @ingroup Internals
 *
 * @defgroup TypeIndex Type index
 *
 * Type lookup and caching.
 *
 * @ref drgn_type_index provides a common interface for finding types in a
 * program.
 *
 * @{
 */

DEFINE_HASH_SET_TYPES(drgn_pointer_type_set, struct drgn_type *)
DEFINE_HASH_SET_TYPES(drgn_array_type_set, struct drgn_type *)

/** <tt>(type, member name)</tt> pair. */
struct drgn_member_key {
	struct drgn_type *type;
	const char *name;
	size_t name_len;
};

/** Type, offset, and bit field size of a type member. */
struct drgn_member_value {
	struct drgn_lazy_type *type;
	uint64_t bit_offset, bit_field_size;
};

#ifdef DOXYGEN
/**
 * @struct drgn_member_map
 *
 * Map of compound type members.
 *
 * The key is a @ref drgn_member_key, and the value is a @ref drgn_member_value.
 *
 * @struct drgn_type_set
 *
 * Set of types compared by address.
 */
#else
DEFINE_HASH_MAP_TYPES(drgn_member_map, struct drgn_member_key,
		      struct drgn_member_value)
DEFINE_HASH_SET_TYPES(drgn_type_set, struct drgn_type *)
#endif

/**
 * Callback for finding a type.
 *
 * If the type is found, this should fill in @p ret and return @c NULL. If not,
 * this should set <tt>ret->type</tt> to @c NULL return @c NULL.
 *
 * @param[in] kind Kind of type.
 * @param[in] name Name of type (or tag, for structs, unions, and enums). This
 * is @em not null-terminated.
 * @param[in] name_len Length of @p name.
 * @param[in] filename Filename containing the type definition or @c NULL. This
 * should be matched with @ref path_ends_with().
 * @param[in] arg Argument passed to @ref drgn_type_index_add_finder().
 * @param[out] ret Returned type.
 * @return @c NULL on success, non-@c NULL on error.
 */
typedef struct drgn_error *
(*drgn_type_find_fn)(enum drgn_type_kind kind, const char *name,
		     size_t name_len, const char *filename, void *arg,
		     struct drgn_qualified_type *ret);

/** Registered callback in a @ref drgn_type_index. */
struct drgn_type_finder {
	/** The callback. */
	drgn_type_find_fn fn;
	/** Argument to pass to @ref drgn_type_finder::fn. */
	void *arg;
	/** Next callback to try. */
	struct drgn_type_finder *next;
};

/**
 * Type index.
 *
 * A type index is used to find types by name and cache the results. The types
 * are found using callbacks which are registered with @ref
 * drgn_type_index_add_finder().
 *
 * @ref drgn_type_index_find() searches for a type. @ref
 * drgn_type_index_pointer_type(), @ref drgn_type_index_array_type(), and @ref
 * drgn_type_index_incomplete_array_type() create derived types. Any type
 * returned by these is valid until the type index is destroyed with @ref
 * drgn_type_index_destroy().
 */
struct drgn_type_index {
	/** Callbacks for finding types. */
	struct drgn_type_finder *finders;
	/** Cache of primitive types. */
	struct drgn_type *primitive_types[DRGN_PRIMITIVE_TYPE_NUM];
	/** Cache of created pointer types. */
	struct drgn_pointer_type_set pointer_types;
	/** Cache of created array types. */
	struct drgn_array_type_set array_types;
	/** Cache for @ref drgn_type_index_find_member(). */
	struct drgn_member_map members;
	/**
	 * Set of types which have been already cached in @ref
	 * drgn_type_index::members.
	 */
	struct drgn_type_set members_cached;
	/**
	 * Size of a pointer in bytes.
	 *
	 * This is zero if it has not been set yet.
	 */
	uint8_t word_size;
};

/**
 * Initialize a @ref drgn_type_index.
 *
 * @param[in] tindex Type index to initialize.
 */
void drgn_type_index_init(struct drgn_type_index *tindex);

/** Deinitialize a @ref drgn_type_index. */
void drgn_type_index_deinit(struct drgn_type_index *tindex);

/**
 * Register a type finding callback.
 *
 * Callbacks are called in reverse order of the order they were added in until
 * the type is found. So, more recently added callbacks take precedence.
 *
 * @param[in] fn The callback.
 * @param[in] arg Argument to pass to @p fn.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *drgn_type_index_add_finder(struct drgn_type_index *tindex,
					      drgn_type_find_fn fn, void *arg);

/** Find a primitive type in a @ref drgn_type_index. */
struct drgn_error *
drgn_type_index_find_primitive(struct drgn_type_index *tindex,
			       enum drgn_primitive_type type,
			       struct drgn_type **ret);

/**
 * Find a parsed type in a @ref drgn_type_index.
 *
 * This should only be called by implementations of @ref
 * drgn_language::find_type().
 *
 * @param[in] kind Kind of type to find. Must be @ref DRGN_TYPE_STRUCT, @ref
 * DRGN_TYPE_UNION, @ref DRGN_TYPE_ENUM, or @ref DRGN_TYPE_TYPEDEF.
 * @param[in] name Name of the type.
 * @param[in] name_len Length of @p name in bytes.
 * @param[in] filename See @ref drgn_type_index_find().
 * @param[out] ret Returned type.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *
drgn_type_index_find_parsed(struct drgn_type_index *tindex,
			    enum drgn_type_kind kind, const char *name,
			    size_t name_len, const char *filename,
			    struct drgn_qualified_type *ret);

/**
 * Find a type in a @ref drgn_type_index.
 *
 * The returned type is valid for the lifetime of the @ref drgn_type_index.
 *
 * @param[in] tindex Type index.
 * @param[in] name Name of the type.
 * @param[in] filename Exact filename containing the type definition, or @c NULL
 * for any definition.
 * @param[in] lang Language to use to parse @p name.
 * @param[out] ret Returned type.
 * @return @c NULL on success, non-@c NULL on error.
 */
static inline struct drgn_error *
drgn_type_index_find(struct drgn_type_index *tindex, const char *name,
		     const char *filename, const struct drgn_language *lang,
		     struct drgn_qualified_type *ret)
{
	return lang->find_type(tindex, name, filename, ret);
}

/**
 * Create a pointer type.
 *
 * The created type is cached for the lifetime of the @ref drgn_type_index. If
 * the same @p referenced_type is passed, the same type will be returned.
 *
 * If this succeeds, @p referenced_type must remain valid until @p tindex is
 * destroyed.
 *
 * @param[in] tindex Type index.
 * @param[in] referenced_type Type referenced by the pointer type.
 * @param[out] ret Returned type.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *
drgn_type_index_pointer_type(struct drgn_type_index *tindex,
			     struct drgn_qualified_type referenced_type,
			     struct drgn_type **ret);

/**
 * Create an array type.
 *
 * The created type is cached for the lifetime of the @ref drgn_type_index. If
 * the same @p length and @p element_type are passed, the same type will be
 * returned.
 *
 * If this succeeds, @p element_type must remain valid until @p tindex is
 * destroyed.
 *
 * @param[in] tindex Type index.
 * @param[in] length Number of elements in the array type.
 * @param[in] element_type Type of an element in the array type.
 * @param[out] ret Returned type.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *
drgn_type_index_array_type(struct drgn_type_index *tindex, uint64_t length,
			   struct drgn_qualified_type element_type,
			   struct drgn_type **ret);

/**
 * Create an incomplete array type.
 *
 * The created type is cached for the lifetime of the @ref drgn_type_index. If
 * the same @p element_type is passed, the same type will be returned.
 *
 * If this succeeds, @p element_type must remain valid until @p tindex is
 * destroyed.
 *
 * @param[in] tindex Type index.
 * @param[in] element_type Type of an element in the array type.
 * @param[out] ret Returned type.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *
drgn_type_index_incomplete_array_type(struct drgn_type_index *tindex,
				      struct drgn_qualified_type element_type,
				      struct drgn_type **ret);

/**
 * Find the type, offset, and bit field size of a type member.
 *
 * This matches the members of the type itself as well as the members of any
 * unnamed members of the type.
 *
 * This caches all members of @p type for subsequent calls.
 *
 * @param[in] tindex Type index.
 * @param[in] type Compound type to search in.
 * @param[in] member_name Name of member.
 * @param[in] member_name_len Length of @p member_name
 * @param[out] ret Returned member information.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *drgn_type_index_find_member(struct drgn_type_index *tindex,
					       struct drgn_type *type,
					       const char *member_name,
					       size_t member_name_len,
					       struct drgn_member_value **ret);

/** Type index entry for testing. */
struct drgn_mock_type {
	/** Type. */
	struct drgn_type *type;
	/**
	 * Name of the file that the type is defined in.
	 *
	 * This may be @c NULL, in which case no filename will match it.
	 */
	const char *filename;
};

/** @} */

#endif /* DRGN_TYPE_INDEX_H */
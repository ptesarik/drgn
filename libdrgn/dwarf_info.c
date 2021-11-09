// Copyright (c) Facebook, Inc. and its affiliates.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <assert.h>
#include <byteswap.h>
#include <dwarf.h>
#include <elf.h>
#include <elfutils/known-dwarf.h>
#include <elfutils/libdw.h>
#include <gelf.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "array.h"
#include "debug_info.h" // IWYU pragma: associated
#include "dwarf_index.h"
#include "error.h"
#include "language.h"
#include "lazy_object.h"
#include "minmax.h"
#include "object.h"
#include "path.h"
#include "program.h"
#include "register_state.h"
#include "serialize.h"
#include "type.h"
#include "util.h"

DEFINE_HASH_MAP_FUNCTIONS(drgn_dwarf_type_map, ptr_key_hash_pair, scalar_key_eq)

void drgn_dwarf_module_info_deinit(struct drgn_debug_info_module *module)
{
	free(module->dwarf.fdes);
	free(module->dwarf.cies);
}

void drgn_dwarf_info_init(struct drgn_debug_info *dbinfo)
{
	drgn_dwarf_index_init(&dbinfo->dwarf.index);
	drgn_dwarf_type_map_init(&dbinfo->dwarf.types);
	drgn_dwarf_type_map_init(&dbinfo->dwarf.cant_be_incomplete_array_types);
	dbinfo->dwarf.depth = 0;
}

void drgn_dwarf_info_deinit(struct drgn_debug_info *dbinfo)
{
	drgn_dwarf_type_map_deinit(&dbinfo->dwarf.cant_be_incomplete_array_types);
	drgn_dwarf_type_map_deinit(&dbinfo->dwarf.types);
	drgn_dwarf_index_deinit(&dbinfo->dwarf.index);
}

/*
 * Diagnostics.
 */

#define DW_TAG_UNKNOWN_FORMAT "unknown DWARF tag 0x%02x"
#define DW_TAG_BUF_LEN (sizeof(DW_TAG_UNKNOWN_FORMAT) - 4 + 2 * sizeof(int))

/**
 * Get the name of a DWARF tag.
 *
 * @return Static string if the tag is known or @p buf if the tag is unknown
 * (populated with a description).
 */
static const char *dw_tag_str(int tag, char buf[DW_TAG_BUF_LEN])
{
	switch (tag) {
#define DWARF_ONE_KNOWN_DW_TAG(name, value) case value: return "DW_TAG_" #name;
	DWARF_ALL_KNOWN_DW_TAG
#undef DWARF_ONE_KNOWN_DW_TAG
	default:
		sprintf(buf, DW_TAG_UNKNOWN_FORMAT, tag);
		return buf;
	}
}

/** Like @ref dw_tag_str(), but takes a @c Dwarf_Die. */
static const char *dwarf_tag_str(Dwarf_Die *die, char buf[DW_TAG_BUF_LEN])
{
	return dw_tag_str(dwarf_tag(die), buf);
}

static struct drgn_error *
drgn_error_debug_info(struct drgn_debug_info_module *module, const char *ptr,
		      const char *message)
{
	uintptr_t p = (uintptr_t)ptr;
	int end_match = -1;
	for (int i = 0; i < array_size(module->scn_data); i++) {
		if (!module->scn_data[i])
			continue;
		uintptr_t start = (uintptr_t)module->scn_data[i]->d_buf;
		uintptr_t end = start + module->scn_data[i]->d_size;
		if (start <= p) {
			if (p < end) {
				return drgn_error_debug_info_scn(module, i, ptr,
								 message);
			} else if (p == end) {
				end_match = i;
			}
		}
	}
	if (end_match != -1) {
		/*
		 * The pointer doesn't lie within a section, but it does point
		 * to the end of a section.
		 */
		return drgn_error_debug_info_scn(module, end_match, ptr,
						 message);
	}
	/* We couldn't find the section containing the pointer. */
	const char *name = dwfl_module_info(module->dwfl_module, NULL, NULL,
					    NULL, NULL, NULL, NULL, NULL);
	return drgn_error_format(DRGN_ERROR_OTHER, "%s: %s", name, message);
}

static inline struct drgn_error *drgn_check_address_size(uint8_t address_size)
{
	if (address_size < 1 || address_size > 8) {
		return drgn_error_format(DRGN_ERROR_OTHER,
					 "unsupported address size %" PRIu8,
					 address_size);
	}
	return NULL;
}

/*
 * Language support.
 */

/**
 * Return the @ref drgn_language of the CU of the given DIE.
 *
 * @param[in] fall_back Whether to fall back if the language is not found or
 * unknown. If @c true, @ref drgn_default_language is returned in this case. If
 * @c false, @c NULL is returned.
 * @param[out] ret Returned language.
 * @return @c NULL on success, non-@c NULL on error.
 */
static struct drgn_error *drgn_language_from_die(Dwarf_Die *die, bool fall_back,
						 const struct drgn_language **ret)
{
	Dwarf_Die cudie;
	if (!dwarf_cu_die(die->cu, &cudie, NULL, NULL, NULL, NULL, NULL, NULL))
		return drgn_error_libdw();
	switch (dwarf_srclang(&cudie)) {
	case DW_LANG_C:
	case DW_LANG_C89:
	case DW_LANG_C99:
	case DW_LANG_C11:
		*ret = &drgn_language_c;
		break;
	case DW_LANG_C_plus_plus:
	case DW_LANG_C_plus_plus_03:
	case DW_LANG_C_plus_plus_11:
	case DW_LANG_C_plus_plus_14:
		*ret = &drgn_language_cpp;
		break;
	default:
		*ret = fall_back ? &drgn_default_language : NULL;
		break;
	}
	return NULL;
}

struct drgn_error *
drgn_debug_info_main_language(struct drgn_debug_info *dbinfo,
			      const struct drgn_language **ret)
{
	struct drgn_error *err;
	struct drgn_dwarf_index_iterator it;
	const uint64_t tag = DW_TAG_subprogram;
	err = drgn_dwarf_index_iterator_init(&it, &dbinfo->dwarf.index.global,
					     "main", strlen("main"), &tag, 1);
	if (err)
		return err;
	struct drgn_dwarf_index_die *index_die;
	while ((index_die = drgn_dwarf_index_iterator_next(&it))) {
		Dwarf_Die die;
		err = drgn_dwarf_index_get_die(index_die, &die);
		if (err) {
			drgn_error_destroy(err);
			continue;
		}

		err = drgn_language_from_die(&die, false, ret);
		if (err) {
			drgn_error_destroy(err);
			continue;
		}
		if (*ret)
			return NULL;
	}
	*ret = NULL;
	return NULL;
}

/*
 * DIE iteration.
 */

DEFINE_VECTOR(dwarf_die_vector, Dwarf_Die)

/** Iterator over DWARF DIEs in a @ref drgn_debug_info_module. */
struct drgn_dwarf_die_iterator {
	/** Stack of current DIE and its ancestors. */
	struct dwarf_die_vector dies;
	Dwarf *dwarf;
	/** End of current CU (for bounds checking). */
	const char *cu_end;
	/** Offset of next CU. */
	Dwarf_Off next_cu_off;
	/** Whether current CU is from .debug_types. */
	bool debug_types;
};

static void drgn_dwarf_die_iterator_init(struct drgn_dwarf_die_iterator *it,
					 Dwarf *dwarf)
{
	dwarf_die_vector_init(&it->dies);
	it->dwarf = dwarf;
	it->next_cu_off = 0;
	it->debug_types = false;
}

static void drgn_dwarf_die_iterator_deinit(struct drgn_dwarf_die_iterator *it)
{
	dwarf_die_vector_deinit(&it->dies);
}

/**
 * Return the next DWARF DIE in a @ref drgn_dwarf_die_iterator.
 *
 * The first call returns the top-level DIE for the first unit in the module.
 * Subsequent calls return children, siblings, and unit DIEs.
 *
 * This includes the .debug_types section.
 *
 * @param[in,out] it Iterator containing the returned DIE and its ancestors. The
 * last entry in `it->dies` is the DIE itself, the entry before that is its
 * parent, the entry before that is its grandparent, etc.
 * @param[in] children If @c true and the last returned DIE has children, return
 * its first child (this is a pre-order traversal). Otherwise, return the next
 * DIE at the level less than or equal to the last returned DIE, i.e., the last
 * returned DIE's sibling, or its ancestor's sibling, or the next top-level unit
 * DIE.
 * @param[in] subtree If zero, iterate over all DIEs in all units. If non-zero,
 * stop after returning all DIEs in the subtree rooted at the DIE that was
 * returned in the last call as entry `subtree - 1` in `it->dies`.
 * @return @c NULL on success, `&drgn_stop` if there are no more DIEs, in which
 * case the size of `it->dies` equals @p subtree and `it->dies` refers to the
 * root of the iterated subtree, non-@c NULL on error, in which case this should
 * not be called again.
 */
static struct drgn_error *
drgn_dwarf_die_iterator_next(struct drgn_dwarf_die_iterator *it, bool children,
			     size_t subtree)
{
#define TOP() (&it->dies.data[it->dies.size - 1])
	int r;
	Dwarf_Die die;
	assert(subtree <= it->dies.size);
	if (it->dies.size == 0) {
		/* This is the first call. Get the first unit DIE. */
		if (!dwarf_die_vector_append_entry(&it->dies))
			return &drgn_enomem;
	} else {
		if (children) {
			r = dwarf_child(TOP(), &die);
			if (r == 0) {
				/* The previous DIE has a child. Return it. */
				if (!dwarf_die_vector_append(&it->dies, &die))
					return &drgn_enomem;
				return NULL;
			} else if (r < 0) {
				return drgn_error_libdw();
			}
			/* The previous DIE has no children. */
		}

		if (it->dies.size == subtree) {
			/*
			 * The previous DIE is the root of the subtree. We're
			 * done.
			 */
			return &drgn_stop;
		}

		if (it->dies.size > 1) {
			r = dwarf_siblingof(TOP(), &die);
			if (r == 0) {
				/* The previous DIE has a sibling. Return it. */
				*TOP() = die;
				return NULL;
			} else if (r > 0) {
				if (!die.addr)
					goto next_unit;
				/*
				 * The previous DIE is the last child of its
				 * parent.
				 */
				char *addr = die.addr;
				do {
					/*
					 * addr points to the null terminator
					 * for the list of siblings. Go back up
					 * to its parent. The next byte is
					 * either the parent's sibling or
					 * another null terminator.
					 */
					it->dies.size--;
					addr++;
					if (it->dies.size == subtree) {
						/*
						 * We're back to the root of the
						 * subtree. We're done.
						 */
						return &drgn_stop;
					}
					if (it->dies.size == 1 ||
					    addr >= it->cu_end)
						goto next_unit;
				} while (*addr == '\0');
				/*
				 * addr now points to the next DIE. Return it.
				 */
				*TOP() = (Dwarf_Die){
					.cu = it->dies.data[0].cu,
					.addr = addr,
				};
				return NULL;
			} else {
				return drgn_error_libdw();
			}
		}
	}

next_unit:;
	/* There are no more DIEs in the current unit.  */
	Dwarf_Off cu_off = it->next_cu_off;
	size_t cu_header_size;
	uint64_t type_signature;
	r = dwarf_next_unit(it->dwarf, cu_off, &it->next_cu_off,
			    &cu_header_size, NULL, NULL, NULL, NULL,
			    it->debug_types ? &type_signature : NULL, NULL);
	if (r == 0) {
		/* Got the next unit. Return the unit DIE. */
		if (it->debug_types) {
			r = !dwarf_offdie_types(it->dwarf,
						cu_off + cu_header_size, TOP());
		} else {
			r = !dwarf_offdie(it->dwarf, cu_off + cu_header_size,
					  TOP());
		}
		if (r)
			return drgn_error_libdw();
		it->cu_end = ((const char *)TOP()->addr
			      - dwarf_dieoffset(TOP())
			      + it->next_cu_off);
		return NULL;
	} else if (r > 0) {
		if (!it->debug_types) {
			it->next_cu_off = 0;
			it->debug_types = true;
			goto next_unit;
		}
		/* There are no more units. */
		return &drgn_stop;
	} else {
		return drgn_error_libdw();
	}
#undef TOP
}

struct drgn_error *
drgn_debug_info_module_find_dwarf_scopes(struct drgn_debug_info_module *module,
					 uint64_t pc, uint64_t *bias_ret,
					 Dwarf_Die **dies_ret,
					 size_t *length_ret)
{
	struct drgn_error *err;

	Dwarf_Addr bias;
	Dwarf *dwarf = dwfl_module_getdwarf(module->dwfl_module, &bias);
	if (!dwarf)
		return drgn_error_libdw();
	*bias_ret = bias;
	pc -= bias;

	/* First, try to get the CU containing the PC. */
	Dwarf_Aranges *aranges;
	size_t naranges;
	if (dwarf_getaranges(dwarf, &aranges, &naranges) < 0)
		return drgn_error_libdw();

	struct drgn_dwarf_die_iterator it;
	bool children;
	size_t subtree;
	Dwarf_Off offset;
	if (dwarf_getarangeinfo(dwarf_getarange_addr(aranges, pc), NULL, NULL,
				&offset) >= 0) {
		drgn_dwarf_die_iterator_init(&it, dwarf);
		Dwarf_Die *cu_die = dwarf_die_vector_append_entry(&it.dies);
		if (!cu_die) {
			err = &drgn_enomem;
			goto err;
		}
		if (!dwarf_offdie(dwarf, offset, cu_die)) {
			err = drgn_error_libdw();
			goto err;
		}
		if (dwarf_next_unit(dwarf, offset - dwarf_cuoffset(cu_die),
				    &it.next_cu_off, NULL, NULL, NULL, NULL,
				    NULL, NULL, NULL)) {
			err = drgn_error_libdw();
			goto err;
		}
		it.cu_end = ((const char *)cu_die->addr
			     - dwarf_dieoffset(cu_die)
			     + it.next_cu_off);
		children = true;
		subtree = 1;
	} else {
		/*
		 * Range was not found. .debug_aranges could be missing or
		 * incomplete, so fall back to checking each CU.
		 */
		drgn_dwarf_die_iterator_init(&it, dwarf);
		children = false;
		subtree = 0;
	}

	/* Now find DIEs containing the PC. */
	while (!(err = drgn_dwarf_die_iterator_next(&it, children, subtree))) {
		int r = dwarf_haspc(&it.dies.data[it.dies.size - 1], pc);
		if (r > 0) {
			children = true;
			subtree = it.dies.size;
		} else if (r < 0) {
			err = drgn_error_libdw();
			goto err;
		}
	}
	if (err != &drgn_stop)
		goto err;

	*dies_ret = it.dies.data;
	*length_ret = it.dies.size;
	return NULL;

err:
	drgn_dwarf_die_iterator_deinit(&it);
	return err;
}

struct drgn_error *drgn_find_die_ancestors(Dwarf_Die *die, Dwarf_Die **dies_ret,
					   size_t *length_ret)
{
	struct drgn_error *err;

	Dwarf *dwarf = dwarf_cu_getdwarf(die->cu);
	if (!dwarf)
		return drgn_error_libdw();

	struct dwarf_die_vector dies = VECTOR_INIT;
	Dwarf_Die *cu_die = dwarf_die_vector_append_entry(&dies);
	if (!cu_die) {
		err = &drgn_enomem;
		goto err;
	}

	Dwarf_Half cu_version;
	Dwarf_Off type_offset;
	if (!dwarf_cu_die(die->cu, cu_die, &cu_version, NULL, NULL, NULL, NULL,
			  &type_offset)) {
		err = drgn_error_libdw();
		goto err;
	}
	Dwarf_Off cu_die_offset = dwarf_dieoffset(cu_die);
	bool debug_types = cu_version == 4 && type_offset != 0;
	Dwarf_Off next_cu_offset;
	uint64_t type_signature;
	if (dwarf_next_unit(dwarf, cu_die_offset - dwarf_cuoffset(cu_die),
			    &next_cu_offset, NULL, NULL, NULL, NULL, NULL,
			    debug_types ? &type_signature : NULL, NULL)) {
		err = drgn_error_libdw();
		goto err;
	}
	const unsigned char *cu_end =
		(unsigned char *)cu_die->addr - cu_die_offset + next_cu_offset;

#define TOP() (&dies.data[dies.size - 1])
	while ((char *)TOP()->addr <= (char *)die->addr) {
		if (TOP()->addr == die->addr) {
			*dies_ret = dies.data;
			*length_ret = dies.size - 1;
			return NULL;
		}

		Dwarf_Attribute attr;
		if (dwarf_attr(TOP(), DW_AT_sibling, &attr)) {
			/* The top DIE has a DW_AT_sibling attribute. */
			Dwarf_Die sibling;
			if (!dwarf_formref_die(&attr, &sibling)) {
				err = drgn_error_libdw();
				goto err;
			}
			if (sibling.cu != TOP()->cu ||
			    (char *)sibling.addr <= (char *)TOP()->addr) {
				err = drgn_error_create(DRGN_ERROR_OTHER,
							"invalid DW_AT_sibling");
				goto err;
			}

			if ((char *)sibling.addr > (char *)die->addr) {
				/*
				 * The top DIE's sibling is after the target
				 * DIE. Therefore, the target DIE must be a
				 * descendant of the top DIE.
				 */
				Dwarf_Die *child =
					dwarf_die_vector_append_entry(&dies);
				if (!child) {
					err = &drgn_enomem;
					goto err;
				}
				int r = dwarf_child(TOP() - 1, child);
				if (r < 0) {
					err = drgn_error_libdw();
					goto err;
				} else if (r > 0) {
					/*
					 * The top DIE didn't have any children,
					 * which should be impossible.
					 */
					goto not_found;
				}
			} else {
				/*
				 * The top DIE's sibling is before or equal to
				 * the target DIE. Therefore, the target DIE
				 * isn't a descendant of the top DIE. Skip to
				 * the sibling.
				 */
				*TOP() = sibling;
			}
		} else {
			/*
			 * The top DIE does not have a DW_AT_sibling attribute.
			 * Instead, we found the end of the top DIE.
			 */
			unsigned char *addr = attr.valp;
			if (!addr || addr >= cu_end)
				goto not_found;

			/*
			 * If the top DIE has children, then addr is its first
			 * child. Otherwise, then addr is its sibling. (Unless
			 * it is a null terminator.)
			 */
			size_t new_size = dies.size;
			if (dwarf_haschildren(TOP()) > 0)
				new_size++;

			while (*addr == '\0') {
				/*
				 * addr points to the null terminator for the
				 * list of siblings. Go back up to its parent.
				 * The next byte is either the parent's sibling
				 * or another null terminator.
				 */
				new_size--;
				addr++;
				if (new_size <= 1 || addr >= cu_end)
					goto not_found;
			}

			/* addr now points to the next DIE. Go to it. */
			if (new_size > dies.size) {
				if (!dwarf_die_vector_append_entry(&dies)) {
					err = &drgn_enomem;
					goto err;
				}
			} else {
				dies.size = new_size;
			}
			*TOP() = (Dwarf_Die){
				.cu = dies.data[0].cu,
				.addr = addr,
			};
		}
	}
#undef TOP

not_found:
	err = drgn_error_create(DRGN_ERROR_OTHER,
				"could not find DWARF DIE ancestors");
err:
	dwarf_die_vector_deinit(&dies);
	return err;
}

/*
 * Location lists.
 */

static struct drgn_error *
drgn_dwarf_next_addrx(struct binary_buffer *bb,
		      struct drgn_debug_info_module *module, Dwarf_Die *cu_die,
		      uint8_t address_size, const char **addr_base,
		      uint64_t *ret)
{
	struct drgn_error *err;

	if (!*addr_base) {
		Dwarf_Attribute attr_mem, *attr;
		if (!(attr = dwarf_attr(cu_die, DW_AT_addr_base, &attr_mem))) {
			return drgn_error_create(DRGN_ERROR_OTHER,
						 "indirect address without DW_AT_addr_base");
		}
		Dwarf_Word base;
		if (dwarf_formudata(attr, &base))
			return drgn_error_libdw();

		if (!module->scns[DRGN_SCN_DEBUG_ADDR]) {
			return drgn_error_create(DRGN_ERROR_OTHER,
						 "indirect address without .debug_addr section");
		}
		err = drgn_debug_info_module_cache_section(module,
							   DRGN_SCN_DEBUG_ADDR);
		if (err)
			return err;

		if (base > module->scn_data[DRGN_SCN_DEBUG_ADDR]->d_size ||
		    base == 0) {
			return drgn_error_create(DRGN_ERROR_OTHER,
						 "DW_AT_addr_base is out of bounds");
		}

		*addr_base = (char *)module->scn_data[DRGN_SCN_DEBUG_ADDR]->d_buf + base;
		uint8_t segment_selector_size = ((uint8_t *)*addr_base)[-1];
		if (segment_selector_size != 0) {
			return drgn_error_format(DRGN_ERROR_OTHER,
						 "unsupported segment selector size %" PRIu8,
						 segment_selector_size);
		}
	}

	uint64_t index;
	if ((err = binary_buffer_next_uleb128(bb, &index)))
		return err;

	Elf_Data *data = module->scn_data[DRGN_SCN_DEBUG_ADDR];
	if (index >=
	    ((char *)data->d_buf + data->d_size - *addr_base) / address_size) {
		return binary_buffer_error(bb,
					   "address index is out of bounds");
	}
	copy_lsbytes(ret, sizeof(*ret), HOST_LITTLE_ENDIAN,
		     *addr_base + index * address_size, address_size,
		     drgn_platform_is_little_endian(&module->platform));
	return NULL;
}

static struct drgn_error *
drgn_dwarf_read_loclistx(struct drgn_debug_info_module *module,
			 Dwarf_Die *cu_die, uint8_t offset_size,
			 Dwarf_Word index, Dwarf_Word *ret)
{
	struct drgn_error *err;

	Dwarf_Attribute attr_mem, *attr;
	if (!(attr = dwarf_attr(cu_die, DW_AT_loclists_base, &attr_mem))) {
		return drgn_error_create(DRGN_ERROR_OTHER,
					 "DW_FORM_loclistx without DW_AT_loclists_base");
	}
	Dwarf_Word base;
	if (dwarf_formudata(attr, &base))
		return drgn_error_libdw();

	if (!module->scns[DRGN_SCN_DEBUG_LOCLISTS]) {
		return drgn_error_create(DRGN_ERROR_OTHER,
					 "DW_FORM_loclistx without .debug_loclists section");
	}
	err = drgn_debug_info_module_cache_section(module,
						   DRGN_SCN_DEBUG_LOCLISTS);
	if (err)
		return err;
	Elf_Data *data = module->scn_data[DRGN_SCN_DEBUG_LOCLISTS];

	if (base > data->d_size) {
		return drgn_error_create(DRGN_ERROR_OTHER,
					 "DW_AT_loclists_base is out of bounds");
	}
	assert(offset_size == 4 || offset_size == 8);
	if (index >= (data->d_size - base) / offset_size) {
		return drgn_error_create(DRGN_ERROR_OTHER,
					 "DW_FORM_loclistx is out of bounds");
	}
	const char *basep = (char *)data->d_buf + base;
	if (offset_size == 8) {
		uint64_t offset;
		memcpy(&offset, (uint64_t *)basep + index, sizeof(offset));
		if (drgn_platform_bswap(&module->platform))
			offset = bswap_64(offset);
		*ret = base + offset;
	} else {
		uint32_t offset;
		memcpy(&offset, (uint32_t *)basep + index, sizeof(offset));
		if (drgn_platform_bswap(&module->platform))
			offset = bswap_32(offset);
		*ret = base + offset;
	}
	return NULL;
}

static struct drgn_error *
drgn_dwarf5_location_list(struct drgn_debug_info_module *module,
			  Dwarf_Word offset, Dwarf_Die *cu_die,
			  uint8_t address_size, uint64_t pc,
			  const char **expr_ret, size_t *expr_size_ret)
{
	struct drgn_error *err;

	if (!module->scns[DRGN_SCN_DEBUG_LOCLISTS]) {
		return drgn_error_create(DRGN_ERROR_OTHER,
					 "loclist without .debug_loclists section");
	}
	err = drgn_debug_info_module_cache_section(module,
						   DRGN_SCN_DEBUG_LOCLISTS);
	if (err)
		return err;
	struct drgn_debug_info_buffer buffer;
	drgn_debug_info_buffer_init(&buffer, module, DRGN_SCN_DEBUG_LOCLISTS);
	if (offset > buffer.bb.end - buffer.bb.pos) {
		return drgn_error_create(DRGN_ERROR_OTHER,
					 "loclist is out of bounds");
	}
	buffer.bb.pos += offset;

	const char *addr_base = NULL;
	uint64_t base;
	bool base_valid = false;
	/* Default is unknown. May be overridden by DW_LLE_default_location. */
	*expr_ret = NULL;
	*expr_size_ret = 0;
	for (;;) {
		uint8_t kind;
		if ((err = binary_buffer_next_u8(&buffer.bb, &kind)))
			return err;
		uint64_t start, length, expr_size;
		switch (kind) {
		case DW_LLE_end_of_list:
			return NULL;
		case DW_LLE_base_addressx:
			if ((err = drgn_dwarf_next_addrx(&buffer.bb, module,
							 cu_die, address_size,
							 &addr_base, &base)))
				return err;
			base_valid = true;
			break;
		case DW_LLE_startx_endx:
			if ((err = drgn_dwarf_next_addrx(&buffer.bb, module,
							 cu_die, address_size,
							 &addr_base, &start)) ||
			    (err = drgn_dwarf_next_addrx(&buffer.bb, module,
							 cu_die, address_size,
							 &addr_base, &length)))
				return err;
			length -= start;
counted_location_description:
			if ((err = binary_buffer_next_uleb128(&buffer.bb,
							      &expr_size)))
				return err;
			if (expr_size > buffer.bb.end - buffer.bb.pos) {
				return binary_buffer_error(&buffer.bb,
							   "location description size is out of bounds");
			}
			if (pc >= start && pc - start < length) {
				*expr_ret = buffer.bb.pos;
				*expr_size_ret = expr_size;
				return NULL;
			}
			buffer.bb.pos += expr_size;
			break;
		case DW_LLE_startx_length:
			if ((err = drgn_dwarf_next_addrx(&buffer.bb, module,
							 cu_die, address_size,
							 &addr_base, &start)) ||
			    (err = binary_buffer_next_uleb128(&buffer.bb,
							      &length)))
				return err;
			goto counted_location_description;
		case DW_LLE_offset_pair:
			if ((err = binary_buffer_next_uleb128(&buffer.bb,
							      &start)) ||
			    (err = binary_buffer_next_uleb128(&buffer.bb,
							      &length)))
				return err;
			length -= start;
			if (!base_valid) {
				Dwarf_Addr low_pc;
				if (dwarf_lowpc(cu_die, &low_pc))
					return drgn_error_libdw();
				base = low_pc;
				base_valid = true;
			}
			start += base;
			goto counted_location_description;
		case DW_LLE_default_location:
			if ((err = binary_buffer_next_uleb128(&buffer.bb,
							      &expr_size)))
				return err;
			if (expr_size > buffer.bb.end - buffer.bb.pos) {
				return binary_buffer_error(&buffer.bb,
							   "location description size is out of bounds");
			}
			*expr_ret = buffer.bb.pos;
			*expr_size_ret = expr_size;
			buffer.bb.pos += expr_size;
			break;
		case DW_LLE_base_address:
			if ((err = binary_buffer_next_uint(&buffer.bb,
							   address_size,
							   &base)))
				return err;
			base_valid = true;
			break;
		case DW_LLE_start_end:
			if ((err = binary_buffer_next_uint(&buffer.bb,
							   address_size,
							   &start)) ||
			    (err = binary_buffer_next_uint(&buffer.bb,
							   address_size,
							   &length)))
				return err;
			length -= start;
			goto counted_location_description;
		case DW_LLE_start_length:
			if ((err = binary_buffer_next_uint(&buffer.bb,
							   address_size,
							   &start)) ||
			    (err = binary_buffer_next_uleb128(&buffer.bb,
							      &length)))
				return err;
			goto counted_location_description;
		default:
			return binary_buffer_error(&buffer.bb,
						   "unknown location list entry kind %#" PRIx8,
						   kind);
		}
	}
}

static struct drgn_error *
drgn_dwarf4_location_list(struct drgn_debug_info_module *module,
			  Dwarf_Word offset, Dwarf_Die *cu_die,
			  uint8_t address_size, uint64_t pc,
			  const char **expr_ret, size_t *expr_size_ret)
{
	struct drgn_error *err;

	if (!module->scns[DRGN_SCN_DEBUG_LOC]) {
		return drgn_error_create(DRGN_ERROR_OTHER,
					 "loclistptr without .debug_loc section");
	}
	err = drgn_debug_info_module_cache_section(module, DRGN_SCN_DEBUG_LOC);
	if (err)
		return err;
	struct drgn_debug_info_buffer buffer;
	drgn_debug_info_buffer_init(&buffer, module, DRGN_SCN_DEBUG_LOC);
	if (offset > buffer.bb.end - buffer.bb.pos) {
		return drgn_error_create(DRGN_ERROR_OTHER,
					 "loclistptr is out of bounds");
	}
	buffer.bb.pos += offset;

	uint64_t address_max = uint_max(address_size);
	uint64_t base;
	bool base_valid = false;
	for (;;) {
		uint64_t start, end;
		if ((err = binary_buffer_next_uint(&buffer.bb, address_size,
						   &start)) ||
		    (err = binary_buffer_next_uint(&buffer.bb, address_size,
						   &end)))
			return err;
		if (start == 0 && end == 0) {
			*expr_ret = NULL;
			*expr_size_ret = 0;
			return NULL;
		} else if (start == address_max) {
			base = end;
			base_valid = true;
		} else {
			if (!base_valid) {
				Dwarf_Addr low_pc;
				if (dwarf_lowpc(cu_die, &low_pc))
					return drgn_error_libdw();
				base = low_pc;
				base_valid = true;
			}
			uint16_t expr_size;
			if ((err = binary_buffer_next_u16(&buffer.bb,
							  &expr_size)))
				return err;
			if (expr_size > buffer.bb.end - buffer.bb.pos) {
				return binary_buffer_error(&buffer.bb,
							   "location description size is out of bounds");
			}
			if (base + start <= pc && pc < base + end) {
				*expr_ret = buffer.bb.pos;
				*expr_size_ret = expr_size;
				return NULL;
			}
			buffer.bb.pos += expr_size;
		}
	}
}

static struct drgn_error *
drgn_dwarf_location(struct drgn_debug_info_module *module,
		    Dwarf_Attribute *attr,
		    const struct drgn_register_state *regs,
		    const char **expr_ret, size_t *expr_size_ret)
{
	struct drgn_error *err;
	switch (attr->form) {
	/* DWARF 3 */
	case DW_FORM_data4:
	case DW_FORM_data8:
	/* DWARF 4-5 */
	case DW_FORM_sec_offset:
	/* DWARF 5 */
	case DW_FORM_loclistx: {
		Dwarf_Die cu_die;
		Dwarf_Half cu_version;
		uint8_t address_size;
		uint8_t offset_size;
		if (!dwarf_cu_die(attr->cu, &cu_die, &cu_version, NULL,
				  &address_size, &offset_size, NULL, NULL))
			return drgn_error_libdw();
		if ((err = drgn_check_address_size(address_size)))
			return err;

		Dwarf_Word offset;
		if (dwarf_formudata(attr, &offset))
			return drgn_error_libdw();
		if (attr->form == DW_FORM_loclistx &&
		    ((err = drgn_dwarf_read_loclistx(module, &cu_die,
						     offset_size, offset,
						     &offset))))
			return err;

		struct optional_uint64 pc;
		if (!regs ||
		    !(pc = drgn_register_state_get_pc(regs)).has_value) {
			*expr_ret = NULL;
			*expr_size_ret = 0;
			return NULL;
		}
		Dwarf_Addr bias;
		dwfl_module_info(module->dwfl_module, NULL, NULL, NULL, &bias,
				 NULL, NULL, NULL);
		pc.value = pc.value - !regs->interrupted - bias;

		if (cu_version >= 5) {
			return drgn_dwarf5_location_list(module, offset,
							 &cu_die, address_size,
							 pc.value, expr_ret,
							 expr_size_ret);
		} else {
			return drgn_dwarf4_location_list(module, offset,
							 &cu_die, address_size,
							 pc.value, expr_ret,
							 expr_size_ret);
		}
	}
	default: {
		Dwarf_Block block;
		if (dwarf_formblock(attr, &block))
			return drgn_error_libdw();
		*expr_ret = (char *)block.data;
		*expr_size_ret = block.length;
		return NULL;
	}
	}
}

/*
 * DWARF expressions.
 */

/**
 * Arbitrary limit for number of operations to execute in a DWARF expression to
 * avoid infinite loops.
 */
static const int MAX_DWARF_EXPR_OPS = 10000;

DEFINE_VECTOR(uint64_vector, uint64_t)

/* A DWARF expression and the context it is being evaluated in. */
struct drgn_dwarf_expression_context {
	struct binary_buffer bb;
	const char *start;
	struct drgn_program *prog;
	struct drgn_debug_info_module *module;
	uint8_t address_size;
	Dwarf_Die cu_die;
	const char *cu_addr_base;
	Dwarf_Die *function;
	const struct drgn_register_state *regs;
};

static struct drgn_error *
drgn_dwarf_expression_buffer_error(struct binary_buffer *bb, const char *pos,
				   const char *message)
{
	struct drgn_dwarf_expression_context *ctx =
		container_of(bb, struct drgn_dwarf_expression_context, bb);
	return drgn_error_debug_info(ctx->module, pos, message);
}

static inline struct drgn_error *
drgn_dwarf_expression_context_init(struct drgn_dwarf_expression_context *ctx,
				   struct drgn_program *prog,
				   struct drgn_debug_info_module *module,
				   Dwarf_CU *cu, Dwarf_Die *function,
				   const struct drgn_register_state *regs,
				   const char *expr, size_t expr_size)
{
	struct drgn_error *err;
	binary_buffer_init(&ctx->bb, expr, expr_size,
			   drgn_platform_is_little_endian(&module->platform),
			   drgn_dwarf_expression_buffer_error);
	ctx->start = expr;
	ctx->prog = prog;
	ctx->module = module;
	if (cu) {
		if (!dwarf_cu_die(cu, &ctx->cu_die, NULL, NULL,
				  &ctx->address_size, NULL, NULL, NULL))
			return drgn_error_libdw();
		if ((err = drgn_check_address_size(ctx->address_size)))
			return err;
	} else {
		ctx->cu_die.addr = NULL;
		ctx->address_size =
			drgn_platform_address_size(&module->platform);
	}
	ctx->cu_addr_base = NULL;
	ctx->function = function;
	ctx->regs = regs;
	return NULL;
}

static struct drgn_error *
drgn_dwarf_frame_base(struct drgn_program *prog,
		      struct drgn_debug_info_module *module, Dwarf_Die *die,
		      const struct drgn_register_state *regs,
		      int *remaining_ops, uint64_t *ret);

/*
 * Evaluate a DWARF expression up to the next location description operation or
 * operation that can't be evaluated in the given context.
 *
 * Returns &drgn_not_found if it tried to use an unknown register value.
 */
static struct drgn_error *
drgn_eval_dwarf_expression(struct drgn_dwarf_expression_context *ctx,
			   struct uint64_vector *stack,
			   int *remaining_ops)
{
	struct drgn_error *err;
	const struct drgn_platform *platform = &ctx->module->platform;
	bool little_endian = drgn_platform_is_little_endian(platform);
	uint8_t address_size = ctx->address_size;
	uint8_t address_bits = address_size * CHAR_BIT;
	uint64_t address_mask = uint_max(address_size);
	drgn_register_number (*dwarf_regno_to_internal)(uint64_t) =
		platform->arch->dwarf_regno_to_internal;

#define CHECK(n) do {								\
	size_t _n = (n);							\
	if (stack->size < _n) {							\
		return binary_buffer_error(&ctx->bb,				\
					   "DWARF expression stack underflow");	\
	}									\
} while (0)

#define ELEM(i) stack->data[stack->size - 1 - (i)]

#define PUSH(x) do {					\
	uint64_t push = (x);				\
	if (!uint64_vector_append(stack, &push))	\
		return &drgn_enomem;			\
} while (0)

#define PUSH_MASK(x) PUSH((x) & address_mask)

	while (binary_buffer_has_next(&ctx->bb)) {
		if (*remaining_ops <= 0) {
			return binary_buffer_error(&ctx->bb,
						   "DWARF expression executed too many operations");
		}
		(*remaining_ops)--;
		uint8_t opcode;
		if ((err = binary_buffer_next_u8(&ctx->bb, &opcode)))
			return err;
		uint64_t uvalue;
		uint64_t dwarf_regno;
		uint8_t deref_size;
		switch (opcode) {
		/* Literal encodings. */
		case DW_OP_lit0 ... DW_OP_lit31:
			PUSH(opcode - DW_OP_lit0);
			break;
		case DW_OP_addr:
			if ((err = binary_buffer_next_uint(&ctx->bb,
							   address_size,
							   &uvalue)))
				return err;
			PUSH(uvalue);
			break;
		case DW_OP_const1u:
			if ((err = binary_buffer_next_u8_into_u64(&ctx->bb,
								  &uvalue)))
				return err;
			PUSH(uvalue);
			break;
		case DW_OP_const2u:
			if ((err = binary_buffer_next_u16_into_u64(&ctx->bb,
								   &uvalue)))
				return err;
			PUSH_MASK(uvalue);
			break;
		case DW_OP_const4u:
			if ((err = binary_buffer_next_u32_into_u64(&ctx->bb,
								   &uvalue)))
				return err;
			PUSH_MASK(uvalue);
			break;
		case DW_OP_const8u:
			if ((err = binary_buffer_next_u64(&ctx->bb, &uvalue)))
				return err;
			PUSH_MASK(uvalue);
			break;
		case DW_OP_const1s:
			if ((err = binary_buffer_next_s8_into_u64(&ctx->bb,
								  &uvalue)))
				return err;
			PUSH_MASK(uvalue);
			break;
		case DW_OP_const2s:
			if ((err = binary_buffer_next_s16_into_u64(&ctx->bb,
								   &uvalue)))
				return err;
			PUSH_MASK(uvalue);
			break;
		case DW_OP_const4s:
			if ((err = binary_buffer_next_s32_into_u64(&ctx->bb,
								   &uvalue)))
				return err;
			PUSH_MASK(uvalue);
			break;
		case DW_OP_const8s:
			if ((err = binary_buffer_next_s64_into_u64(&ctx->bb,
								   &uvalue)))
				return err;
			PUSH_MASK(uvalue);
			break;
		case DW_OP_constu:
			if ((err = binary_buffer_next_uleb128(&ctx->bb,
							      &uvalue)))
				return err;
			PUSH_MASK(uvalue);
			break;
		case DW_OP_consts:
			if ((err = binary_buffer_next_sleb128_into_u64(&ctx->bb,
								       &uvalue)))
				return err;
			PUSH_MASK(uvalue);
			break;
		case DW_OP_addrx:
		case DW_OP_constx:
			if (!ctx->cu_die.addr) {
				ctx->bb.pos = ctx->bb.prev;
				return NULL;
			}
			if ((err = drgn_dwarf_next_addrx(&ctx->bb, ctx->module,
							 &ctx->cu_die,
							 address_size,
							 &ctx->cu_addr_base,
							 &uvalue)))
				return err;
			PUSH(uvalue);
			break;
		/* Register values. */
		case DW_OP_fbreg: {
			err = drgn_dwarf_frame_base(ctx->prog, ctx->module,
						    ctx->function, ctx->regs,
						    remaining_ops, &uvalue);
			if (err)
				return err;
			int64_t svalue;
			if ((err = binary_buffer_next_sleb128(&ctx->bb,
							      &svalue)))
				return err;
			PUSH_MASK(uvalue + svalue);
			break;
		}
		case DW_OP_breg0 ... DW_OP_breg31:
			dwarf_regno = opcode - DW_OP_breg0;
			goto breg;
		case DW_OP_bregx:
			if ((err = binary_buffer_next_uleb128(&ctx->bb,
							      &dwarf_regno)))
				return err;
breg:
		{
			if (!ctx->regs)
				return &drgn_not_found;
			drgn_register_number regno =
				dwarf_regno_to_internal(dwarf_regno);
			if (!drgn_register_state_has_register(ctx->regs, regno))
				return &drgn_not_found;
			const struct drgn_register_layout *layout =
				&platform->arch->register_layout[regno];
			copy_lsbytes(&uvalue, sizeof(uvalue),
				     HOST_LITTLE_ENDIAN,
				     &ctx->regs->buf[layout->offset],
				     layout->size, little_endian);
			int64_t svalue;
			if ((err = binary_buffer_next_sleb128(&ctx->bb,
							      &svalue)))
				return err;
			PUSH_MASK(uvalue + svalue);
			break;
		}
		/* Stack operations. */
		case DW_OP_dup:
			CHECK(1);
			PUSH(ELEM(0));
			break;
		case DW_OP_drop:
			CHECK(1);
			stack->size--;
			break;
		case DW_OP_pick: {
			uint8_t index;
			if ((err = binary_buffer_next_u8(&ctx->bb, &index)))
				return err;
			CHECK(index + 1);
			PUSH(ELEM(index));
			break;
		}
		case DW_OP_over:
			CHECK(2);
			PUSH(ELEM(1));
			break;
		case DW_OP_swap:
			CHECK(2);
			uvalue = ELEM(0);
			ELEM(0) = ELEM(1);
			ELEM(1) = uvalue;
			break;
		case DW_OP_rot:
			CHECK(3);
			uvalue = ELEM(0);
			ELEM(0) = ELEM(1);
			ELEM(1) = ELEM(2);
			ELEM(2) = uvalue;
			break;
		case DW_OP_deref:
			deref_size = address_size;
			goto deref;
		case DW_OP_deref_size:
			if ((err = binary_buffer_next_u8(&ctx->bb,
							 &deref_size)))
				return err;
			if (deref_size > address_size) {
				return binary_buffer_error(&ctx->bb,
							   "DW_OP_deref_size has invalid size");
			}
deref:
		{
			CHECK(1);
			char deref_buf[8];
			err = drgn_program_read_memory(ctx->prog, deref_buf,
						       ELEM(0), deref_size,
						       false);
			if (err)
				return err;
			copy_lsbytes(&ELEM(0), sizeof(ELEM(0)),
				     HOST_LITTLE_ENDIAN, deref_buf, deref_size,
				     little_endian);
			break;
		}
		case DW_OP_call_frame_cfa: {
			if (!ctx->regs)
				return &drgn_not_found;
			/*
			 * The DWARF 5 specification says that
			 * DW_OP_call_frame_cfa cannot be used for CFI. For
			 * DW_CFA_def_cfa_expression, it is clearly invalid to
			 * define the CFA in terms of the CFA, and it will fail
			 * naturally below. This restriction doesn't make sense
			 * for DW_CFA_expression and DW_CFA_val_expression, as
			 * they push the CFA and thus depend on it anyways, so
			 * we don't bother enforcing it.
			 */
			struct optional_uint64 cfa =
				drgn_register_state_get_cfa(ctx->regs);
			if (!cfa.has_value)
				return &drgn_not_found;
			PUSH(cfa.value);
			break;
		}
		/* Arithmetic and logical operations. */
#define UNOP_MASK(op) do {			\
	CHECK(1);				\
	ELEM(0) = (op ELEM(0)) & address_mask;	\
} while (0)
#define BINOP(op) do {			\
	CHECK(2);			\
	ELEM(1) = ELEM(1) op ELEM(0);	\
	stack->size--;			\
} while (0)
#define BINOP_MASK(op) do {				\
	CHECK(2);					\
	ELEM(1) = (ELEM(1) op ELEM(0)) & address_mask;	\
	stack->size--;					\
} while (0)
		case DW_OP_abs:
			CHECK(1);
			if (ELEM(0) & (UINT64_C(1) << (address_bits - 1)))
				ELEM(0) = -ELEM(0) & address_mask;
			break;
		case DW_OP_and:
			BINOP(&);
			break;
		case DW_OP_div:
			CHECK(2);
			if (ELEM(0) == 0) {
				return binary_buffer_error(&ctx->bb,
							   "division by zero in DWARF expression");
			}
			ELEM(1) = ((truncate_signed(ELEM(1), address_bits)
				    / truncate_signed(ELEM(0), address_bits))
				   & address_mask);
			stack->size--;
			break;
		case DW_OP_minus:
			BINOP_MASK(-);
			break;
		case DW_OP_mod:
			CHECK(2);
			if (ELEM(0) == 0) {
				return binary_buffer_error(&ctx->bb,
							   "modulo by zero in DWARF expression");
			}
			ELEM(1) = ELEM(1) % ELEM(0);
			stack->size--;
			break;
		case DW_OP_mul:
			BINOP_MASK(*);
			break;
		case DW_OP_neg:
			UNOP_MASK(-);
			break;
		case DW_OP_not:
			UNOP_MASK(~);
			break;
		case DW_OP_or:
			BINOP(|);
			break;
		case DW_OP_plus:
			BINOP_MASK(+);
			break;
		case DW_OP_plus_uconst:
			CHECK(1);
			if ((err = binary_buffer_next_uleb128(&ctx->bb,
							      &uvalue)))
				return err;
			ELEM(0) = (ELEM(0) + uvalue) & address_mask;
			break;
		case DW_OP_shl:
			CHECK(2);
			if (ELEM(0) < address_bits)
				ELEM(1) = (ELEM(1) << ELEM(0)) & address_mask;
			else
				ELEM(1) = 0;
			stack->size--;
			break;
		case DW_OP_shr:
			CHECK(2);
			if (ELEM(0) < address_bits)
				ELEM(1) >>= ELEM(0);
			else
				ELEM(1) = 0;
			stack->size--;
			break;
		case DW_OP_shra:
			CHECK(2);
			if (ELEM(0) < address_bits) {
				ELEM(1) = ((truncate_signed(ELEM(1), address_bits)
					    >> ELEM(0))
					   & address_mask);
			} else if (ELEM(1) & (UINT64_C(1) << (address_bits - 1))) {
				ELEM(1) = -INT64_C(1) & address_mask;
			} else {
				ELEM(1) = 0;
			}
			stack->size--;
			break;
		case DW_OP_xor:
			BINOP(^);
			break;
#undef BINOP_MASK
#undef BINOP
#undef UNOP_MASK
		/* Control flow operations. */
#define RELOP(op) do {						\
	CHECK(2);						\
	ELEM(1) = (truncate_signed(ELEM(1), address_bits) op	\
		   truncate_signed(ELEM(0), address_bits));	\
	stack->size--;						\
} while (0)
		case DW_OP_le:
			RELOP(<=);
			break;
		case DW_OP_ge:
			RELOP(>=);
			break;
		case DW_OP_eq:
			RELOP(==);
			break;
		case DW_OP_lt:
			RELOP(<);
			break;
		case DW_OP_gt:
			RELOP(>);
			break;
		case DW_OP_ne:
			RELOP(!=);
			break;
#undef RELOP
		case DW_OP_skip:
branch:
		{
			int16_t skip;
			if ((err = binary_buffer_next_s16(&ctx->bb, &skip)))
				return err;
			if ((skip >= 0 && skip > ctx->bb.end - ctx->bb.pos) ||
			    (skip < 0 && -skip > ctx->bb.pos - ctx->start)) {
				return binary_buffer_error(&ctx->bb,
							   "DWARF expression branch is out of bounds");
			}
			ctx->bb.pos += skip;
			break;
		}
		case DW_OP_bra:
			CHECK(1);
			if (ELEM(0)) {
				stack->size--;
				goto branch;
			} else {
				stack->size--;
				if ((err = binary_buffer_skip(&ctx->bb, 2)))
					return err;
			}
			break;
		/* Special operations. */
		case DW_OP_nop:
			break;
		/* Location description operations. */
		case DW_OP_reg0 ... DW_OP_reg31:
		case DW_OP_regx:
		case DW_OP_implicit_value:
		case DW_OP_stack_value:
		case DW_OP_piece:
		case DW_OP_bit_piece:
			/* The caller must handle it. */
			ctx->bb.pos = ctx->bb.prev;
			return NULL;
		/*
		 * We don't yet support:
		 *
		 * - DW_OP_push_object_address
		 * - DW_OP_form_tls_address
		 * - DW_OP_entry_value
		 *   DW_OP_implicit_pointer
		 * - Procedure calls: DW_OP_call2, DW_OP_call4, DW_OP_call_ref.
		 * - Typed operations: DW_OP_const_type, DW_OP_regval_type,
		 *   DW_OP_deref_type, DW_OP_convert, DW_OP_reinterpret.
		 * - Operations for multiple address spaces: DW_OP_xderef,
		 *   DW_OP_xderef_size, DW_OP_xderef_type.
		 */
		default:
			return binary_buffer_error(&ctx->bb,
						   "unknown DWARF expression opcode %#" PRIx8,
						   opcode);
		}
	}

#undef PUSH_MASK
#undef PUSH
#undef ELEM
#undef CHECK

	return NULL;
}

static struct drgn_error *
drgn_dwarf_frame_base(struct drgn_program *prog,
		      struct drgn_debug_info_module *module, Dwarf_Die *die,
		      const struct drgn_register_state *regs,
		      int *remaining_ops, uint64_t *ret)
{
	struct drgn_error *err;
	bool little_endian = drgn_platform_is_little_endian(&module->platform);
	drgn_register_number (*dwarf_regno_to_internal)(uint64_t) =
		module->platform.arch->dwarf_regno_to_internal;

	if (!die)
		return &drgn_not_found;
	Dwarf_Attribute attr_mem, *attr;
	if (!(attr = dwarf_attr_integrate(die, DW_AT_frame_base, &attr_mem)))
		return &drgn_not_found;
	const char *expr;
	size_t expr_size;
	err = drgn_dwarf_location(module, attr, regs, &expr, &expr_size);
	if (err)
		return err;

	struct drgn_dwarf_expression_context ctx;
	if ((err = drgn_dwarf_expression_context_init(&ctx, prog, module,
						      die->cu, NULL, regs, expr,
						      expr_size)))
		return err;
	struct uint64_vector stack = VECTOR_INIT;
	for (;;) {
		err = drgn_eval_dwarf_expression(&ctx, &stack, remaining_ops);
		if (err)
			goto out;
		if (binary_buffer_has_next(&ctx.bb)) {
			uint8_t opcode;
			if ((err = binary_buffer_next_u8(&ctx.bb, &opcode)))
				goto out;

			uint64_t dwarf_regno;
			switch (opcode) {
			case DW_OP_reg0 ... DW_OP_reg31:
				dwarf_regno = opcode - DW_OP_reg0;
				goto reg;
			case DW_OP_regx:
				if ((err = binary_buffer_next_uleb128(&ctx.bb,
								      &dwarf_regno)))
					goto out;
reg:
			{
				if (!regs) {
					err = &drgn_not_found;
					goto out;
				}
				drgn_register_number regno =
					dwarf_regno_to_internal(dwarf_regno);
				if (!drgn_register_state_has_register(regs,
								      regno)) {
					err = &drgn_not_found;
					goto out;
				}
				const struct drgn_register_layout *layout =
					&prog->platform.arch->register_layout[regno];
				/*
				 * Note that this doesn't mask the address since
				 * the caller does that.
				 */
				copy_lsbytes(ret, sizeof(*ret),
					     HOST_LITTLE_ENDIAN,
					     &regs->buf[layout->offset],
					     layout->size, little_endian);
				if (binary_buffer_has_next(&ctx.bb)) {
					err = binary_buffer_error(&ctx.bb,
								  "stray operations in DW_AT_frame_base expression");
				} else {
					err = NULL;
				}
				goto out;
			}
			default:
				err = binary_buffer_error(&ctx.bb,
							  "invalid opcode %#" PRIx8 " for DW_AT_frame_base expression",
							  opcode);
				goto out;
			}
		} else if (stack.size) {
			*ret = stack.data[stack.size - 1];
			err = NULL;
			break;
		} else {
			err = &drgn_not_found;
			break;
		}
	}
out:
	uint64_vector_deinit(&stack);
	return err;
}

/*
 * Type and object parsing.
 */

/**
 * Return whether a DWARF DIE is little-endian.
 *
 * @param[in] check_attr Whether to check the DW_AT_endianity attribute. If @c
 * false, only the ELF header is checked and this function cannot fail.
 * @return @c NULL on success, non-@c NULL on error.
 */
static struct drgn_error *dwarf_die_is_little_endian(Dwarf_Die *die,
						     bool check_attr, bool *ret)
{
	Dwarf_Attribute endianity_attr_mem, *endianity_attr;
	Dwarf_Word endianity;
	if (check_attr &&
	    (endianity_attr = dwarf_attr_integrate(die, DW_AT_endianity,
						   &endianity_attr_mem))) {
		if (dwarf_formudata(endianity_attr, &endianity)) {
			return drgn_error_create(DRGN_ERROR_OTHER,
						 "invalid DW_AT_endianity");
		}
	} else {
		endianity = DW_END_default;
	}
	switch (endianity) {
	case DW_END_default: {
		Elf *elf = dwarf_getelf(dwarf_cu_getdwarf(die->cu));
		*ret = elf_getident(elf, NULL)[EI_DATA] == ELFDATA2LSB;
		return NULL;
	}
	case DW_END_little:
		*ret = true;
		return NULL;
	case DW_END_big:
		*ret = false;
		return NULL;
	default:
		return drgn_error_create(DRGN_ERROR_OTHER,
					 "unknown DW_AT_endianity");
	}
}

/** Like dwarf_die_is_little_endian(), but returns a @ref drgn_byte_order. */
static struct drgn_error *dwarf_die_byte_order(Dwarf_Die *die, bool check_attr,
					       enum drgn_byte_order *ret)
{
	bool little_endian;
	struct drgn_error *err = dwarf_die_is_little_endian(die, check_attr,
							    &little_endian);
	/*
	 * dwarf_die_is_little_endian() can't fail if check_attr is false, so
	 * the !check_attr test suppresses maybe-uninitialized warnings.
	 */
	if (!err || !check_attr)
		*ret = drgn_byte_order_from_little_endian(little_endian);
	return err;
}

static int dwarf_type(Dwarf_Die *die, Dwarf_Die *ret)
{
	Dwarf_Attribute attr_mem;
	Dwarf_Attribute *attr;

	if (!(attr = dwarf_attr_integrate(die, DW_AT_type, &attr_mem)))
		return 1;

	return dwarf_formref_die(attr, ret) ? 0 : -1;
}

static int dwarf_flag(Dwarf_Die *die, unsigned int name, bool *ret)
{
	Dwarf_Attribute attr_mem;
	Dwarf_Attribute *attr;

	if (!(attr = dwarf_attr(die, name, &attr_mem))) {
		*ret = false;
		return 0;
	}
	return dwarf_formflag(attr, ret);
}

static int dwarf_flag_integrate(Dwarf_Die *die, unsigned int name, bool *ret)
{
	Dwarf_Attribute attr_mem;
	Dwarf_Attribute *attr;

	if (!(attr = dwarf_attr_integrate(die, name, &attr_mem))) {
		*ret = false;
		return 0;
	}
	return dwarf_formflag(attr, ret);
}

/**
 * Parse a type from a DWARF debugging information entry.
 *
 * This is the same as @ref drgn_type_from_dwarf() except that it can be used to
 * work around a bug in GCC < 9.0 that zero length array types are encoded the
 * same as incomplete array types. There are a few places where GCC allows
 * zero-length arrays but not incomplete arrays:
 *
 * - As the type of a member of a structure with only one member.
 * - As the type of a structure member other than the last member.
 * - As the type of a union member.
 * - As the element type of an array.
 *
 * In these cases, we know that what appears to be an incomplete array type must
 * actually have a length of zero. In other cases, a subrange DIE without
 * DW_AT_count or DW_AT_upper_bound is ambiguous; we return an incomplete array
 * type.
 *
 * @param[in] dbinfo Debugging information.
 * @param[in] module Module containing @p die.
 * @param[in] die DIE to parse.
 * @param[in] can_be_incomplete_array Whether the type can be an incomplete
 * array type. If this is @c false and the type appears to be an incomplete
 * array type, its length is set to zero instead.
 * @param[out] is_incomplete_array_ret Whether the encoded type is an incomplete
 * array type or a typedef of an incomplete array type (regardless of @p
 * can_be_incomplete_array).
 * @param[out] ret Returned type.
 * @return @c NULL on success, non-@c NULL on error.
 */
static struct drgn_error *
drgn_type_from_dwarf_internal(struct drgn_debug_info *dbinfo,
			      struct drgn_debug_info_module *module,
			      Dwarf_Die *die, bool can_be_incomplete_array,
			      bool *is_incomplete_array_ret,
			      struct drgn_qualified_type *ret);

/**
 * Parse a type from a DWARF debugging information entry.
 *
 * @param[in] dbinfo Debugging information.
 * @param[in] module Module containing @p die.
 * @param[in] die DIE to parse.
 * @param[out] ret Returned type.
 * @return @c NULL on success, non-@c NULL on error.
 */
static inline struct drgn_error *
drgn_type_from_dwarf(struct drgn_debug_info *dbinfo,
		     struct drgn_debug_info_module *module, Dwarf_Die *die,
		     struct drgn_qualified_type *ret)
{
	return drgn_type_from_dwarf_internal(dbinfo, module, die, true, NULL,
					     ret);
}

/**
 * Parse a type from the @c DW_AT_type attribute of a DWARF debugging
 * information entry.
 *
 * @param[in] dbinfo Debugging information.
 * @param[in] module Module containing @p die.
 * @param[in] die DIE with @c DW_AT_type attribute.
 * @param[in] lang Language of @p die if it is already known, @c NULL if it
 * should be determined from @p die.
 * @param[in] can_be_void Whether the @c DW_AT_type attribute may be missing,
 * which is interpreted as a void type. If this is false and the @c DW_AT_type
 * attribute is missing, an error is returned.
 * @param[in] can_be_incomplete_array See @ref drgn_type_from_dwarf_internal().
 * @param[in] is_incomplete_array_ret See @ref drgn_type_from_dwarf_internal().
 * @param[out] ret Returned type.
 * @return @c NULL on success, non-@c NULL on error.
 */
static struct drgn_error *
drgn_type_from_dwarf_attr(struct drgn_debug_info *dbinfo,
			  struct drgn_debug_info_module *module, Dwarf_Die *die,
			  const struct drgn_language *lang,
			  bool can_be_void, bool can_be_incomplete_array,
			  bool *is_incomplete_array_ret,
			  struct drgn_qualified_type *ret)
{
	struct drgn_error *err;
	char tag_buf[DW_TAG_BUF_LEN];

	Dwarf_Attribute attr_mem;
	Dwarf_Attribute *attr;
	if (!(attr = dwarf_attr_integrate(die, DW_AT_type, &attr_mem))) {
		if (can_be_void) {
			if (!lang) {
				err = drgn_language_from_die(die, true, &lang);
				if (err)
					return err;
			}
			ret->type = drgn_void_type(dbinfo->prog, lang);
			ret->qualifiers = 0;
			return NULL;
		} else {
			return drgn_error_format(DRGN_ERROR_OTHER,
						 "%s is missing DW_AT_type",
						 dwarf_tag_str(die, tag_buf));
		}
	}

	Dwarf_Die type_die;
	if (!dwarf_formref_die(attr, &type_die)) {
		return drgn_error_format(DRGN_ERROR_OTHER,
					 "%s has invalid DW_AT_type",
					 dwarf_tag_str(die, tag_buf));
	}

	return drgn_type_from_dwarf_internal(dbinfo, module, &type_die,
					     can_be_incomplete_array,
					     is_incomplete_array_ret, ret);
}

static struct drgn_error *
drgn_object_from_dwarf_enumerator(struct drgn_debug_info *dbinfo,
				  struct drgn_debug_info_module *module,
				  Dwarf_Die *die, const char *name,
				  struct drgn_object *ret)
{
	struct drgn_error *err;
	struct drgn_qualified_type qualified_type;
	err = drgn_type_from_dwarf(dbinfo, module, die, &qualified_type);
	if (err)
		return err;
	const struct drgn_type_enumerator *enumerators =
		drgn_type_enumerators(qualified_type.type);
	size_t num_enumerators = drgn_type_num_enumerators(qualified_type.type);
	for (size_t i = 0; i < num_enumerators; i++) {
		if (strcmp(enumerators[i].name, name) != 0)
			continue;

		if (drgn_enum_type_is_signed(qualified_type.type)) {
			return drgn_object_set_signed(ret, qualified_type,
						      enumerators[i].svalue, 0);
		} else {
			return drgn_object_set_unsigned(ret, qualified_type,
							enumerators[i].uvalue,
							0);
		}
	}
	UNREACHABLE();
}

static struct drgn_error *
drgn_object_from_dwarf_subprogram(struct drgn_debug_info *dbinfo,
				  struct drgn_debug_info_module *module,
				  Dwarf_Die *die, struct drgn_object *ret)
{
	struct drgn_qualified_type qualified_type;
	struct drgn_error *err = drgn_type_from_dwarf(dbinfo, module, die,
						      &qualified_type);
	if (err)
		return err;
	Dwarf_Addr low_pc;
	if (dwarf_lowpc(die, &low_pc) == -1)
		return drgn_object_set_absent(ret, qualified_type, 0);
	Dwarf_Addr bias;
	dwfl_module_info(module->dwfl_module, NULL, NULL, NULL, &bias, NULL,
			 NULL, NULL);
	return drgn_object_set_reference(ret, qualified_type, low_pc + bias, 0,
					 0);
}

static struct drgn_error *read_bits(struct drgn_program *prog, void *dst,
				    unsigned int dst_bit_offset, uint64_t src,
				    unsigned int src_bit_offset,
				    uint64_t bit_size, bool lsb0)
{
	struct drgn_error *err;

	assert(dst_bit_offset < 8);
	assert(src_bit_offset < 8);

	if (bit_size == 0)
		return NULL;

	if (dst_bit_offset == src_bit_offset) {
		/*
		 * We can read directly into the the destination buffer, but we
		 * may have to preserve some bits at the start and/or end.
		 */
		uint8_t *d = dst;
		uint64_t last_bit = dst_bit_offset + bit_size - 1;
		uint8_t first_byte = d[0];
		uint8_t last_byte = d[last_bit / 8];
		err = drgn_program_read_memory(prog, d, src, last_bit / 8 + 1,
					       false);
		if (err)
			return err;
		if (dst_bit_offset != 0) {
			uint8_t mask =
				copy_bits_first_mask(dst_bit_offset, lsb0);
			d[0] = (first_byte & ~mask) | (d[0] & mask);
		}
		if (last_bit % 8 != 7) {
			uint8_t mask = copy_bits_last_mask(last_bit, lsb0);
			d[last_bit / 8] = ((last_byte & ~mask)
					   | (d[last_bit / 8] & mask));
		}
		return NULL;
	} else {
		/*
		 * If the source and destination have different offsets, then
		 * depending on the size and source offset, we may have to read
		 * one more byte than is available in the destination. To keep
		 * things simple, we always read into a temporary buffer (rather
		 * than adding a special case for reading directly into the
		 * destination and shifting bits around).
		 */
		uint64_t src_bytes = (src_bit_offset + bit_size - 1) / 8 + 1;
		char stack_tmp[16], *tmp;
		if (src_bytes <= sizeof(stack_tmp)) {
			tmp = stack_tmp;
		} else {
			tmp = malloc64(src_bytes);
			if (!tmp)
				return &drgn_enomem;
		}
		err = drgn_program_read_memory(prog, tmp, src, src_bytes,
					       false);
		if (!err) {
			copy_bits(dst, dst_bit_offset, tmp, src_bit_offset,
				  bit_size, lsb0);
		}
		if (src_bytes > sizeof(stack_tmp))
			free(tmp);
		return err;
	}
}

static struct drgn_error *
drgn_object_from_dwarf_location(struct drgn_program *prog,
				struct drgn_debug_info_module *module,
				Dwarf_Die *die,
				struct drgn_qualified_type qualified_type,
				const char *expr, size_t expr_size,
				Dwarf_Die *function_die,
				const struct drgn_register_state *regs,
				struct drgn_object *ret)
{
	struct drgn_error *err;
	bool little_endian = drgn_platform_is_little_endian(&module->platform);
	uint64_t address_mask = drgn_platform_address_mask(&module->platform);
	drgn_register_number (*dwarf_regno_to_internal)(uint64_t) =
		module->platform.arch->dwarf_regno_to_internal;

	struct drgn_object_type type;
	err = drgn_object_type(qualified_type, 0, &type);
	if (err)
		return err;

	union drgn_value value;
	char *value_buf = NULL;

	uint64_t address = 0; /* GCC thinks this may be used uninitialized. */
	int bit_offset = -1; /* -1 means that we don't have an address. */

	uint64_t bit_pos = 0;

	int remaining_ops = MAX_DWARF_EXPR_OPS;
	struct drgn_dwarf_expression_context ctx;
	if ((err = drgn_dwarf_expression_context_init(&ctx, prog, module,
						      die->cu, function_die,
						      regs, expr, expr_size)))
		return err;
	struct uint64_vector stack = VECTOR_INIT;
	do {
		stack.size = 0;
		err = drgn_eval_dwarf_expression(&ctx, &stack, &remaining_ops);
		if (err == &drgn_not_found)
			goto absent;
		else if (err)
			goto out;

		const void *src = NULL;
		size_t src_size;

		if (binary_buffer_has_next(&ctx.bb)) {
			uint8_t opcode;
			if ((err = binary_buffer_next_u8(&ctx.bb, &opcode)))
				goto out;

			uint64_t uvalue;
			uint64_t dwarf_regno;
			drgn_register_number regno;
			switch (opcode) {
			case DW_OP_reg0 ... DW_OP_reg31:
				dwarf_regno = opcode - DW_OP_reg0;
				goto reg;
			case DW_OP_regx:
				if ((err = binary_buffer_next_uleb128(&ctx.bb,
								      &dwarf_regno)))
					goto out;
reg:
				if (!regs)
					goto absent;
				regno = dwarf_regno_to_internal(dwarf_regno);
				if (!drgn_register_state_has_register(regs,
								      regno))
					goto absent;
				const struct drgn_register_layout *layout =
					&prog->platform.arch->register_layout[regno];
				src = &regs->buf[layout->offset];
				src_size = layout->size;
				break;
			case DW_OP_implicit_value:
				if ((err = binary_buffer_next_uleb128(&ctx.bb,
								      &uvalue)))
					goto out;
				if (uvalue > ctx.bb.end - ctx.bb.pos) {
					err = binary_buffer_error(&ctx.bb,
								  "DW_OP_implicit_value size is out of bounds");
					goto out;
				}
				src = ctx.bb.pos;
				src_size = uvalue;
				ctx.bb.pos += uvalue;
				break;
			case DW_OP_stack_value:
				if (!stack.size)
					goto absent;
				if (little_endian != HOST_LITTLE_ENDIAN) {
					stack.data[stack.size - 1] =
						bswap_64(stack.data[stack.size - 1]);
				}
				src = &stack.data[stack.size - 1];
				src_size = sizeof(stack.data[0]);
				break;
			default:
				ctx.bb.pos = ctx.bb.prev;
				break;
			}
		}

		uint64_t piece_bit_size;
		uint64_t piece_bit_offset;
		if (binary_buffer_has_next(&ctx.bb)) {
			uint8_t opcode;
			if ((err = binary_buffer_next_u8(&ctx.bb, &opcode)))
				goto out;

			switch (opcode) {
			case DW_OP_piece:
				if ((err = binary_buffer_next_uleb128(&ctx.bb,
								      &piece_bit_size)))
					goto out;
				/*
				 * It's probably bogus for the piece size to be
				 * larger than the remaining value size, but
				 * that's not explicitly stated in the DWARF 5
				 * specification, so clamp it instead.
				 */
				if (__builtin_mul_overflow(piece_bit_size, 8U,
							   &piece_bit_size) ||
				    piece_bit_size > type.bit_size - bit_pos)
					piece_bit_size = type.bit_size - bit_pos;
				piece_bit_offset = 0;
				break;
			case DW_OP_bit_piece:
				if ((err = binary_buffer_next_uleb128(&ctx.bb,
								      &piece_bit_size)) ||
				    (err = binary_buffer_next_uleb128(&ctx.bb,
								      &piece_bit_offset)))
					goto out;
				if (piece_bit_size > type.bit_size - bit_pos)
					piece_bit_size = type.bit_size - bit_pos;
				break;
			default:
				err = binary_buffer_error(&ctx.bb,
							  "unknown DWARF expression opcode %#" PRIx8 " after simple location description",
							  opcode);
				goto out;
			}
		} else {
			piece_bit_size = type.bit_size - bit_pos;
			piece_bit_offset = 0;
		}

		/*
		 * TODO: there are a few cases that a DWARF location can
		 * describe that can't be represented in drgn's object model:
		 *
		 * 1. An object that is partially known and partially unknown.
		 * 2. An object that is partially in memory and partially a
		 *    value.
		 * 3. An object that is in memory at non-contiguous addresses.
		 * 4. A pointer object whose pointer value is not known but
		 *    whose referenced value is known (DW_OP_implicit_pointer).
		 *
		 * For case 1, we consider the whole object as absent. For cases
		 * 2 and 3, we convert the whole object to a value. Case 4 is
		 * not supported at all. We should add a way to represent all of
		 * these situations precisely.
		 */
		if (src && piece_bit_size == 0) {
			/* Ignore empty value. */
		} else if (src) {
			if (!value_buf &&
			    !drgn_value_zalloc(drgn_value_size(type.bit_size),
					       &value, &value_buf)) {
				err = &drgn_enomem;
				goto out;
			}
			if (bit_offset >= 0) {
				/*
				 * We previously had an address. Read it into
				 * the value.
				 */
				err = read_bits(prog, value_buf, 0, address,
						bit_offset, bit_pos,
						little_endian);
				if (err)
					goto out;
				bit_offset = -1;
			}
			/*
			 * It's probably safe to assume that we don't have an
			 * implicit value larger than 2 exabytes.
			 */
			assert(src_size <= UINT64_MAX / 8);
			uint64_t src_bit_size = UINT64_C(8) * src_size;
			if (piece_bit_offset > src_bit_size)
				piece_bit_offset = src_bit_size;
			uint64_t copy_bit_size =
				min(piece_bit_size,
				    src_bit_size - piece_bit_offset);
			uint64_t copy_bit_offset = bit_pos;
			if (!little_endian) {
				copy_bit_offset += piece_bit_size - copy_bit_size;
				piece_bit_offset = (src_bit_size
						    - copy_bit_size
						    - piece_bit_offset);
			}
			copy_bits(&value_buf[copy_bit_offset / 8],
				  copy_bit_offset % 8,
				  (const char *)src + (piece_bit_offset / 8),
				  piece_bit_offset % 8, copy_bit_size,
				  little_endian);
		} else if (stack.size) {
			uint64_t piece_address =
				((stack.data[stack.size - 1] + piece_bit_offset / 8)
				 & address_mask);
			piece_bit_offset %= 8;
			if (bit_pos > 0 && bit_offset >= 0) {
				/*
				 * We already had an address. Merge the pieces
				 * if the addresses are contiguous, otherwise
				 * convert to a value.
				 *
				 * The obvious way to write this is
				 * (address + (bit_pos + bit_offset) / 8), but
				 * (bit_pos + bit_offset) can overflow uint64_t.
				 */
				uint64_t end_address =
					((address
					  + bit_pos / 8
					  + (bit_pos % 8 + bit_offset) / 8)
					 & address_mask);
				unsigned int end_bit_offset =
					(bit_offset + bit_pos) % 8;
				if (piece_bit_size == 0 ||
				    (piece_address == end_address &&
				     piece_bit_offset == end_bit_offset)) {
					/* Piece is contiguous. */
					piece_address = address;
					piece_bit_offset = bit_offset;
				} else {
					if (!drgn_value_zalloc(drgn_value_size(type.bit_size),
							       &value,
							       &value_buf)) {
						err = &drgn_enomem;
						goto out;
					}
					err = read_bits(prog, value_buf, 0,
							address, bit_offset,
							bit_pos, little_endian);
					if (err)
						goto out;
					bit_offset = -1;
				}
			}
			if (value_buf) {
				/* We already have a value. Read into it. */
				err = read_bits(prog, &value_buf[bit_pos / 8],
						bit_pos % 8, piece_address,
						piece_bit_offset,
						piece_bit_size, little_endian);
				if (err)
					goto out;
			} else {
				address = piece_address;
				bit_offset = piece_bit_offset;
			}
		} else if (piece_bit_size > 0) {
			goto absent;
		}
		bit_pos += piece_bit_size;
	} while (binary_buffer_has_next(&ctx.bb));

	if (bit_pos < type.bit_size || (bit_offset < 0 && !value_buf)) {
absent:
		if (dwarf_tag(die) == DW_TAG_template_value_parameter) {
			return drgn_error_create(DRGN_ERROR_OTHER,
						 "DW_AT_template_value_parameter is missing value");
		}
		drgn_object_reinit(ret, &type, DRGN_OBJECT_ABSENT);
		err = NULL;
	} else if (bit_offset >= 0) {
		Dwarf_Addr start, end, bias;
		dwfl_module_info(module->dwfl_module, NULL, &start, &end, &bias,
				 NULL, NULL, NULL);
		/*
		 * If the address is not in the module's address range, then
		 * it's probably something special like a Linux per-CPU variable
		 * (which isn't actually a variable address but an offset).
		 * Don't apply the bias in that case.
		 */
		if (start <= address + bias && address + bias < end)
			address += bias;
		err = drgn_object_set_reference_internal(ret, &type, address,
							 bit_offset);
	} else if (type.encoding == DRGN_OBJECT_ENCODING_BUFFER) {
		drgn_object_reinit(ret, &type, DRGN_OBJECT_VALUE);
		ret->value = value;
		value_buf = NULL;
		err = NULL;
	} else {
		err = drgn_object_set_from_buffer_internal(ret, &type,
							   value_buf, 0);
	}

out:
	if (value_buf != value.ibuf)
		free(value_buf);
	uint64_vector_deinit(&stack);
	return err;
}

static struct drgn_error *
drgn_object_from_dwarf_constant(struct drgn_debug_info *dbinfo, Dwarf_Die *die,
				struct drgn_qualified_type qualified_type,
				Dwarf_Attribute *attr, struct drgn_object *ret)
{
	struct drgn_object_type type;
	struct drgn_error *err = drgn_object_type(qualified_type, 0, &type);
	if (err)
		return err;
	Dwarf_Block block;
	if (dwarf_formblock(attr, &block) == 0) {
		if (block.length < drgn_value_size(type.bit_size)) {
			return drgn_error_create(DRGN_ERROR_OTHER,
						 "DW_AT_const_value block is too small");
		}
		return drgn_object_set_from_buffer_internal(ret, &type,
							    block.data, 0);
	} else if (type.encoding == DRGN_OBJECT_ENCODING_SIGNED) {
		Dwarf_Sword svalue;
		if (dwarf_formsdata(attr, &svalue)) {
			return drgn_error_create(DRGN_ERROR_OTHER,
						 "invalid DW_AT_const_value");
		}
		drgn_object_set_signed_internal(ret, &type, svalue);
		return NULL;
	} else if (type.encoding == DRGN_OBJECT_ENCODING_UNSIGNED) {
		Dwarf_Word uvalue;
		if (dwarf_formudata(attr, &uvalue)) {
			return drgn_error_create(DRGN_ERROR_OTHER,
						 "invalid DW_AT_const_value");
		}
		drgn_object_set_unsigned_internal(ret, &type, uvalue);
		return NULL;
	} else {
		return drgn_error_create(DRGN_ERROR_OTHER,
					 "unknown DW_AT_const_value form");
	}
}

struct drgn_error *
drgn_object_from_dwarf(struct drgn_debug_info *dbinfo,
		       struct drgn_debug_info_module *module,
		       Dwarf_Die *die, Dwarf_Die *type_die,
		       Dwarf_Die *function_die,
		       const struct drgn_register_state *regs,
		       struct drgn_object *ret)
{
	struct drgn_error *err;
	if (dwarf_tag(die) == DW_TAG_subprogram) {
		return drgn_object_from_dwarf_subprogram(dbinfo, module, die,
							 ret);
	}
	/*
	 * The DWARF 5 specifications mentions that data object entries can have
	 * DW_AT_endianity, but that doesn't seem to be used in practice. It
	 * would be inconvenient to support, so ignore it for now.
	 */
	struct drgn_qualified_type qualified_type;
	if (type_die) {
		err = drgn_type_from_dwarf(dbinfo, module, type_die,
					   &qualified_type);
	} else {
		err = drgn_type_from_dwarf_attr(dbinfo, module, die, NULL, true,
						true, NULL, &qualified_type);
	}
	if (err)
		return err;
	Dwarf_Attribute attr_mem, *attr;
	const char *expr;
	size_t expr_size;
	if ((attr = dwarf_attr_integrate(die, DW_AT_location, &attr_mem))) {
		err = drgn_dwarf_location(module, attr, regs, &expr,
					  &expr_size);
		if (err)
			return err;
	} else if ((attr = dwarf_attr_integrate(die, DW_AT_const_value,
						&attr_mem))) {
		return drgn_object_from_dwarf_constant(dbinfo, die,
						       qualified_type, attr,
						       ret);
	} else {
		expr = NULL;
		expr_size = 0;
	}
	return drgn_object_from_dwarf_location(dbinfo->prog, module, die,
					       qualified_type, expr, expr_size,
					       function_die, regs, ret);
}

static struct drgn_error *find_dwarf_enumerator(Dwarf_Die *enumeration_type,
						const char *name,
						Dwarf_Die *ret)
{
	int r = dwarf_child(enumeration_type, ret);
	while (r == 0) {
		if (dwarf_tag(ret) == DW_TAG_enumerator &&
		    strcmp(dwarf_diename(ret), name) == 0)
			return NULL;
		r = dwarf_siblingof(ret, ret);
	}
	if (r < 0)
		return drgn_error_libdw();
	ret->addr = NULL;
	return NULL;
}

struct drgn_error *drgn_find_in_dwarf_scopes(Dwarf_Die *scopes,
					     size_t num_scopes,
					     const char *name,
					     Dwarf_Die *die_ret,
					     Dwarf_Die *type_ret)
{
	struct drgn_error *err;
	Dwarf_Die die;
	for (size_t scope = num_scopes; scope--;) {
		bool have_declaration = false;
		if (dwarf_child(&scopes[scope], &die) != 0)
			continue;
		do {
			switch (dwarf_tag(&die)) {
			case DW_TAG_variable:
			case DW_TAG_formal_parameter:
			case DW_TAG_subprogram:
				if (strcmp(dwarf_diename(&die), name) == 0) {
					*die_ret = die;
					bool declaration;
					if (dwarf_flag(&die, DW_AT_declaration,
						       &declaration))
						return drgn_error_libdw();
					if (declaration)
						have_declaration = true;
					else
						return NULL;
				}
				break;
			case DW_TAG_enumeration_type: {
				bool enum_class;
				if (dwarf_flag_integrate(&die, DW_AT_enum_class,
							 &enum_class))
					return drgn_error_libdw();
				if (!enum_class) {
					Dwarf_Die enumerator;
					err = find_dwarf_enumerator(&die, name,
								    &enumerator);
					if (err)
						return err;
					if (enumerator.addr) {
						*die_ret = enumerator;
						*type_ret = die;
						return NULL;
					}
				}
				break;
			}
			default:
				continue;
			}
		} while (dwarf_siblingof(&die, &die) == 0);
		if (have_declaration)
			return NULL;
	}
	die_ret->addr = NULL;
	return NULL;
}

static struct drgn_error *
drgn_base_type_from_dwarf(struct drgn_debug_info *dbinfo,
			  struct drgn_debug_info_module *module, Dwarf_Die *die,
			  const struct drgn_language *lang,
			  struct drgn_type **ret)
{
	struct drgn_error *err;

	const char *name = dwarf_diename(die);
	if (!name) {
		return drgn_error_create(DRGN_ERROR_OTHER,
					 "DW_TAG_base_type has missing or invalid DW_AT_name");
	}

	Dwarf_Attribute attr;
	Dwarf_Word encoding;
	if (!dwarf_attr_integrate(die, DW_AT_encoding, &attr) ||
	    dwarf_formudata(&attr, &encoding)) {
		return drgn_error_create(DRGN_ERROR_OTHER,
					 "DW_TAG_base_type has missing or invalid DW_AT_encoding");
	}
	int size = dwarf_bytesize(die);
	if (size == -1) {
		return drgn_error_create(DRGN_ERROR_OTHER,
					 "DW_TAG_base_type has missing or invalid DW_AT_byte_size");
	}

	enum drgn_byte_order byte_order;
	err = dwarf_die_byte_order(die, true, &byte_order);
	if (err)
		return err;

	switch (encoding) {
	case DW_ATE_boolean:
		return drgn_bool_type_create(dbinfo->prog, name, size,
					     byte_order, lang, ret);
	case DW_ATE_float:
		return drgn_float_type_create(dbinfo->prog, name, size,
					      byte_order, lang, ret);
	case DW_ATE_signed:
	case DW_ATE_signed_char:
		return drgn_int_type_create(dbinfo->prog, name, size, true,
					    byte_order, lang, ret);
	case DW_ATE_unsigned:
	case DW_ATE_unsigned_char:
		return drgn_int_type_create(dbinfo->prog, name, size, false,
					    byte_order, lang, ret);
	/* We don't support complex types yet. */
	case DW_ATE_complex_float:
	default:
		return drgn_error_format(DRGN_ERROR_OTHER,
					 "DW_TAG_base_type has unknown DWARF encoding 0x%llx",
					 (unsigned long long)encoding);
	}
}

/*
 * DW_TAG_structure_type, DW_TAG_union_type, DW_TAG_class_type, and
 * DW_TAG_enumeration_type can be incomplete (i.e., have a DW_AT_declaration of
 * true). This tries to find the complete type. If it succeeds, it returns NULL.
 * If it can't find a complete type, it returns &drgn_not_found. Otherwise, it
 * returns an error.
 */
static struct drgn_error *
drgn_debug_info_find_complete(struct drgn_debug_info *dbinfo, uint64_t tag,
			      const char *name, struct drgn_type **ret)
{
	struct drgn_error *err;

	struct drgn_dwarf_index_iterator it;
	err = drgn_dwarf_index_iterator_init(&it, &dbinfo->dwarf.index.global,
					     name, strlen(name), &tag, 1);
	if (err)
		return err;

	/*
	 * Find a matching DIE. Note that drgn_dwarf_index does not contain DIEs
	 * with DW_AT_declaration, so this will always be a complete type.
	 */
	struct drgn_dwarf_index_die *index_die =
		drgn_dwarf_index_iterator_next(&it);
	if (!index_die)
		return &drgn_not_found;
	/*
	 * Look for another matching DIE. If there is one, then we can't be sure
	 * which type this is, so leave it incomplete rather than guessing.
	 */
	if (drgn_dwarf_index_iterator_next(&it))
		return &drgn_not_found;

	Dwarf_Die die;
	err = drgn_dwarf_index_get_die(index_die, &die);
	if (err)
		return err;
	struct drgn_qualified_type qualified_type;
	err = drgn_type_from_dwarf(dbinfo, index_die->module, &die,
				   &qualified_type);
	if (err)
		return err;
	*ret = qualified_type.type;
	return NULL;
}

struct drgn_dwarf_member_thunk_arg {
	struct drgn_debug_info_module *module;
	Dwarf_Die die;
	bool can_be_incomplete_array;
};

static struct drgn_error *
drgn_dwarf_member_thunk_fn(struct drgn_object *res, void *arg_)
{
	struct drgn_error *err;
	struct drgn_dwarf_member_thunk_arg *arg = arg_;
	if (res) {
		struct drgn_qualified_type qualified_type;
		err = drgn_type_from_dwarf_attr(drgn_object_program(res)->dbinfo,
						arg->module, &arg->die, NULL,
						false,
						arg->can_be_incomplete_array,
						NULL, &qualified_type);
		if (err)
			return err;

		Dwarf_Attribute attr_mem, *attr;
		uint64_t bit_field_size;
		if ((attr = dwarf_attr_integrate(&arg->die, DW_AT_bit_size,
						 &attr_mem))) {
			Dwarf_Word bit_size;
			if (dwarf_formudata(attr, &bit_size)) {
				return drgn_error_create(DRGN_ERROR_OTHER,
							 "DW_TAG_member has invalid DW_AT_bit_size");
			}
			bit_field_size = bit_size;
		} else {
			bit_field_size = 0;
		}

		err = drgn_object_set_absent(res, qualified_type,
					     bit_field_size);
		if (err)
			return err;
	}
	free(arg);
	return NULL;
}

static inline bool drgn_dwarf_attribute_is_block(Dwarf_Attribute *attr)
{
	switch (attr->form) {
	case DW_FORM_block1:
	case DW_FORM_block2:
	case DW_FORM_block4:
	case DW_FORM_block:
		return true;
	default:
		return false;
	}
}

static inline bool drgn_dwarf_attribute_is_ptr(Dwarf_Attribute *attr)
{
	switch (attr->form) {
	case DW_FORM_sec_offset:
		return true;
	case DW_FORM_data4:
	case DW_FORM_data8: {
		/*
		 * dwarf_cu_die() always returns the DIE. We should use
		 * dwarf_cu_info(), but that requires elfutils >= 0.171.
		 */
		Dwarf_Die unused;
		Dwarf_Half cu_version;
		dwarf_cu_die(attr->cu, &unused, &cu_version, NULL, NULL, NULL,
			     NULL, NULL);
		return cu_version <= 3;
	}
	default:
		return false;
	}
}

static struct drgn_error *invalid_data_member_location(struct binary_buffer *bb,
						       const char *pos,
						       const char *message)
{
	return drgn_error_create(DRGN_ERROR_OTHER,
				 "DW_TAG_member has invalid DW_AT_data_member_location");
}

static struct drgn_error *
drgn_parse_dwarf_data_member_location(Dwarf_Attribute *attr, uint64_t *ret)
{
	struct drgn_error *err;

	if (drgn_dwarf_attribute_is_block(attr)) {
		Dwarf_Block block;
		if (dwarf_formblock(attr, &block))
			return drgn_error_libdw();
		/*
		 * In DWARF 2, DW_AT_data_member_location is always a location
		 * description. We can translate a DW_OP_plus_uconst expression
		 * into a constant offset; other expressions aren't supported
		 * yet.
		 */
		struct binary_buffer bb;
		/*
		 * Right now we only parse u8 and ULEB128, so the byte order
		 * doesn't matter.
		 */
		binary_buffer_init(&bb, block.data, block.length,
				   HOST_LITTLE_ENDIAN,
				   invalid_data_member_location);
		uint8_t opcode;
		err = binary_buffer_next_u8(&bb, &opcode);
		if (err)
			return err;
		if (opcode != DW_OP_plus_uconst) {
unsupported:
			return drgn_error_create(DRGN_ERROR_OTHER,
						 "DW_TAG_member has unsupported DW_AT_data_member_location");
		}
		err = binary_buffer_next_uleb128(&bb, ret);
		if (err)
			return err;
		if (binary_buffer_has_next(&bb))
			goto unsupported;
	} else if (drgn_dwarf_attribute_is_ptr(attr)) {
		goto unsupported;
	} else {

		Dwarf_Word word;
		if (dwarf_formudata(attr, &word))
			return invalid_data_member_location(NULL, NULL, NULL);
		*ret = word;
	}
	return NULL;
}

static struct drgn_error *
parse_member_offset(Dwarf_Die *die, union drgn_lazy_object *member_object,
		    bool little_endian, uint64_t *ret)
{
	struct drgn_error *err;
	Dwarf_Attribute attr_mem;
	Dwarf_Attribute *attr;

	/*
	 * The simplest case is when we have DW_AT_data_bit_offset, which is
	 * already the offset in bits from the beginning of the containing
	 * object to the beginning of the member (which may be a bit field).
	 */
	attr = dwarf_attr_integrate(die, DW_AT_data_bit_offset, &attr_mem);
	if (attr) {
		Dwarf_Word bit_offset;
		if (dwarf_formudata(attr, &bit_offset)) {
			return drgn_error_create(DRGN_ERROR_OTHER,
						 "DW_TAG_member has invalid DW_AT_data_bit_offset");
		}
		*ret = bit_offset;
		return NULL;
	}

	/*
	 * Otherwise, we might have DW_AT_data_member_location, which is the
	 * offset in bytes from the beginning of the containing object.
	 */
	attr = dwarf_attr_integrate(die, DW_AT_data_member_location, &attr_mem);
	if (attr) {
		err = drgn_parse_dwarf_data_member_location(attr, ret);
		if (err)
			return err;
		*ret *= 8;
	} else {
		*ret = 0;
	}

	/*
	 * In addition to DW_AT_data_member_location, a bit field might have
	 * DW_AT_bit_offset, which is the offset in bits of the most significant
	 * bit of the bit field from the most significant bit of the containing
	 * object.
	 */
	attr = dwarf_attr_integrate(die, DW_AT_bit_offset, &attr_mem);
	if (attr) {
		Dwarf_Word bit_offset;
		if (dwarf_formudata(attr, &bit_offset)) {
			return drgn_error_create(DRGN_ERROR_OTHER,
						 "DW_TAG_member has invalid DW_AT_bit_offset");
		}

		/*
		 * If the architecture is little-endian, then we must compute
		 * the location of the most significant bit from the size of the
		 * member, then subtract the bit offset and bit size to get the
		 * location of the beginning of the bit field.
		 *
		 * If the architecture is big-endian, then the most significant
		 * bit of the bit field is the beginning.
		 */
		if (little_endian) {
			err = drgn_lazy_object_evaluate(member_object);
			if (err)
				return err;

			attr = dwarf_attr_integrate(die, DW_AT_byte_size,
						    &attr_mem);
			/*
			 * If the member has an explicit byte size, we can use
			 * that. Otherwise, we have to get it from the member
			 * type.
			 */
			uint64_t byte_size;
			if (attr) {
				Dwarf_Word word;
				if (dwarf_formudata(attr, &word)) {
					return drgn_error_create(DRGN_ERROR_OTHER,
								 "DW_TAG_member has invalid DW_AT_byte_size");
				}
				byte_size = word;
			} else {
				if (!drgn_type_has_size(member_object->obj.type)) {
					return drgn_error_create(DRGN_ERROR_OTHER,
								 "DW_TAG_member bit field type does not have size");
				}
				err = drgn_type_sizeof(member_object->obj.type,
						       &byte_size);
				if (err)
					return err;
			}
			*ret += 8 * byte_size - bit_offset - member_object->obj.bit_size;
		} else {
			*ret += bit_offset;
		}
	}

	return NULL;
}

static struct drgn_error *
parse_member(struct drgn_debug_info *dbinfo,
	     struct drgn_debug_info_module *module, Dwarf_Die *die,
	     bool little_endian, bool can_be_incomplete_array,
	     struct drgn_compound_type_builder *builder)
{
	struct drgn_error *err;

	Dwarf_Attribute attr_mem, *attr;
	const char *name;
	if ((attr = dwarf_attr_integrate(die, DW_AT_name, &attr_mem))) {
		name = dwarf_formstring(attr);
		if (!name) {
			return drgn_error_create(DRGN_ERROR_OTHER,
						 "DW_TAG_member has invalid DW_AT_name");
		}
	} else {
		name = NULL;
	}

	struct drgn_dwarf_member_thunk_arg *thunk_arg =
		malloc(sizeof(*thunk_arg));
	if (!thunk_arg)
		return &drgn_enomem;
	thunk_arg->module = module;
	thunk_arg->die = *die;
	thunk_arg->can_be_incomplete_array = can_be_incomplete_array;

	union drgn_lazy_object member_object;
	drgn_lazy_object_init_thunk(&member_object, dbinfo->prog,
				    drgn_dwarf_member_thunk_fn, thunk_arg);

	uint64_t bit_offset;
	err = parse_member_offset(die, &member_object, little_endian,
				  &bit_offset);
	if (err)
		goto err;

	err = drgn_compound_type_builder_add_member(builder, &member_object,
						    name, bit_offset);
	if (err)
		goto err;
	return NULL;

err:
	drgn_lazy_object_deinit(&member_object);
	return err;
}

struct drgn_dwarf_die_thunk_arg {
	struct drgn_debug_info_module *module;
	Dwarf_Die die;
};

static struct drgn_error *
drgn_dwarf_template_type_parameter_thunk_fn(struct drgn_object *res, void *arg_)
{
	struct drgn_error *err;
	struct drgn_dwarf_die_thunk_arg *arg = arg_;
	if (res) {
		struct drgn_qualified_type qualified_type;
		err = drgn_type_from_dwarf_attr(drgn_object_program(res)->dbinfo,
						arg->module, &arg->die, NULL,
						true, true, NULL,
						&qualified_type);
		if (err)
			return err;

		err = drgn_object_set_absent(res, qualified_type, 0);
		if (err)
			return err;
	}
	free(arg);
	return NULL;
}

static struct drgn_error *
drgn_dwarf_template_value_parameter_thunk_fn(struct drgn_object *res,
					     void *arg_)
{
	struct drgn_error *err;
	struct drgn_dwarf_die_thunk_arg *arg = arg_;
	if (res) {
		err = drgn_object_from_dwarf(drgn_object_program(res)->dbinfo,
					     arg->module, &arg->die, NULL, NULL,
					     NULL, res);
		if (err)
			return err;
	}
	free(arg);
	return NULL;
}

static struct drgn_error *
parse_template_parameter(struct drgn_debug_info *dbinfo,
			 struct drgn_debug_info_module *module, Dwarf_Die *die,
			 drgn_object_thunk_fn *thunk_fn,
			 struct drgn_template_parameters_builder *builder)
{
	char tag_buf[DW_TAG_BUF_LEN];

	Dwarf_Attribute attr_mem, *attr;
	const char *name;
	if ((attr = dwarf_attr_integrate(die, DW_AT_name, &attr_mem))) {
		name = dwarf_formstring(attr);
		if (!name) {
			return drgn_error_format(DRGN_ERROR_OTHER,
						 "%s has invalid DW_AT_name",
						 dwarf_tag_str(die, tag_buf));
		}
	} else {
		name = NULL;
	}

	bool defaulted;
	if (dwarf_flag_integrate(die, DW_AT_default_value, &defaulted)) {
		return drgn_error_format(DRGN_ERROR_OTHER,
					 "%s has invalid DW_AT_default_value",
					 dwarf_tag_str(die, tag_buf));
	}

	struct drgn_dwarf_die_thunk_arg *thunk_arg =
		malloc(sizeof(*thunk_arg));
	if (!thunk_arg)
		return &drgn_enomem;
	thunk_arg->module = module;
	thunk_arg->die = *die;

	union drgn_lazy_object argument;
	drgn_lazy_object_init_thunk(&argument, dbinfo->prog, thunk_fn,
				    thunk_arg);

	struct drgn_error *err =
		drgn_template_parameters_builder_add(builder, &argument, name,
						     defaulted);
	if (err)
		drgn_lazy_object_deinit(&argument);
	return err;
}

static struct drgn_error *
drgn_compound_type_from_dwarf(struct drgn_debug_info *dbinfo,
			      struct drgn_debug_info_module *module,
			      Dwarf_Die *die, const struct drgn_language *lang,
			      enum drgn_type_kind kind, struct drgn_type **ret)
{
	struct drgn_error *err;
	char tag_buf[DW_TAG_BUF_LEN];

	Dwarf_Attribute attr_mem;
	Dwarf_Attribute *attr = dwarf_attr_integrate(die, DW_AT_name,
						     &attr_mem);
	const char *tag;
	if (attr) {
		tag = dwarf_formstring(attr);
		if (!tag) {
			return drgn_error_format(DRGN_ERROR_OTHER,
						 "%s has invalid DW_AT_name",
						 dwarf_tag_str(die, tag_buf));
		}
	} else {
		tag = NULL;
	}

	bool declaration;
	if (dwarf_flag(die, DW_AT_declaration, &declaration)) {
		return drgn_error_format(DRGN_ERROR_OTHER,
					 "%s has invalid DW_AT_declaration",
					 dwarf_tag_str(die, tag_buf));
	}
	if (declaration && tag) {
		err = drgn_debug_info_find_complete(dbinfo, dwarf_tag(die), tag,
						    ret);
		if (err != &drgn_not_found)
			return err;
	}

	struct drgn_compound_type_builder builder;
	drgn_compound_type_builder_init(&builder, dbinfo->prog, kind);

	int size;
	bool little_endian;
	if (declaration) {
		size = 0;
	} else {
		size = dwarf_bytesize(die);
		if (size == -1) {
			return drgn_error_format(DRGN_ERROR_OTHER,
						 "%s has missing or invalid DW_AT_byte_size",
						 dwarf_tag_str(die, tag_buf));
		}
		dwarf_die_is_little_endian(die, false, &little_endian);
	}

	Dwarf_Die member = {}, child;
	int r = dwarf_child(die, &child);
	while (r == 0) {
		switch (dwarf_tag(&child)) {
		case DW_TAG_member:
			if (!declaration) {
				if (member.addr) {
					err = parse_member(dbinfo, module,
							   &member,
							   little_endian, false,
							   &builder);
					if (err)
						goto err;
				}
				member = child;
			}
			break;
		case DW_TAG_template_type_parameter:
			err = parse_template_parameter(dbinfo, module, &child,
						       drgn_dwarf_template_type_parameter_thunk_fn,
						       &builder.template_builder);
			if (err)
				goto err;
			break;
		case DW_TAG_template_value_parameter:
			err = parse_template_parameter(dbinfo, module, &child,
						       drgn_dwarf_template_value_parameter_thunk_fn,
						       &builder.template_builder);
			if (err)
				goto err;
			break;
		default:
			break;
		}
		r = dwarf_siblingof(&child, &child);
	}
	if (r == -1) {
		err = drgn_error_create(DRGN_ERROR_OTHER,
					"libdw could not parse DIE children");
		goto err;
	}
	/*
	 * Flexible array members are only allowed as the last member of a
	 * structure with at least one other member.
	 */
	if (member.addr) {
		err = parse_member(dbinfo, module, &member, little_endian,
				   kind != DRGN_TYPE_UNION &&
				   builder.members.size > 0,
				   &builder);
		if (err)
			goto err;
	}

	err = drgn_compound_type_create(&builder, tag, size, !declaration, lang,
					ret);
	if (err)
		goto err;
	return NULL;

err:
	drgn_compound_type_builder_deinit(&builder);
	return err;
}

static struct drgn_error *
parse_enumerator(Dwarf_Die *die, struct drgn_enum_type_builder *builder,
		 bool *is_signed)
{
	const char *name = dwarf_diename(die);
	if (!name) {
		return drgn_error_create(DRGN_ERROR_OTHER,
					 "DW_TAG_enumerator has missing or invalid DW_AT_name");
	}

	Dwarf_Attribute attr_mem, *attr;
	if (!(attr = dwarf_attr_integrate(die, DW_AT_const_value, &attr_mem))) {
		return drgn_error_create(DRGN_ERROR_OTHER,
					 "DW_TAG_enumerator is missing DW_AT_const_value");
	}
	struct drgn_error *err;
	if (attr->form == DW_FORM_sdata ||
	    attr->form == DW_FORM_implicit_const) {
		Dwarf_Sword svalue;
		if (dwarf_formsdata(attr, &svalue))
			goto invalid;
		err = drgn_enum_type_builder_add_signed(builder, name,
							svalue);
		/*
		 * GCC before 7.1 didn't include DW_AT_encoding for
		 * DW_TAG_enumeration_type DIEs, so we have to guess the sign
		 * for enum_compatible_type_fallback().
		 */
		if (!err && svalue < 0)
			*is_signed = true;
	} else {
		Dwarf_Word uvalue;
		if (dwarf_formudata(attr, &uvalue))
			goto invalid;
		err = drgn_enum_type_builder_add_unsigned(builder, name,
							  uvalue);
	}
	return err;

invalid:
	return drgn_error_create(DRGN_ERROR_OTHER,
				 "DW_TAG_enumerator has invalid DW_AT_const_value");
}

/*
 * GCC before 5.1 did not include DW_AT_type for DW_TAG_enumeration_type DIEs,
 * so we have to fabricate the compatible type.
 */
static struct drgn_error *
enum_compatible_type_fallback(struct drgn_debug_info *dbinfo,
			      Dwarf_Die *die, bool is_signed,
			      const struct drgn_language *lang,
			      struct drgn_type **ret)
{
	int size = dwarf_bytesize(die);
	if (size == -1) {
		return drgn_error_create(DRGN_ERROR_OTHER,
					 "DW_TAG_enumeration_type has missing or invalid DW_AT_byte_size");
	}
	enum drgn_byte_order byte_order;
	dwarf_die_byte_order(die, false, &byte_order);
	return drgn_int_type_create(dbinfo->prog, "<unknown>", size, is_signed,
				    byte_order, lang, ret);
}

static struct drgn_error *
drgn_enum_type_from_dwarf(struct drgn_debug_info *dbinfo,
			  struct drgn_debug_info_module *module, Dwarf_Die *die,
			  const struct drgn_language *lang,
			  struct drgn_type **ret)
{
	struct drgn_error *err;

	Dwarf_Attribute attr_mem;
	Dwarf_Attribute *attr = dwarf_attr_integrate(die, DW_AT_name,
						     &attr_mem);
	const char *tag;
	if (attr) {
		tag = dwarf_formstring(attr);
		if (!tag)
			return drgn_error_create(DRGN_ERROR_OTHER,
						 "DW_TAG_enumeration_type has invalid DW_AT_name");
	} else {
		tag = NULL;
	}

	bool declaration;
	if (dwarf_flag(die, DW_AT_declaration, &declaration)) {
		return drgn_error_create(DRGN_ERROR_OTHER,
					 "DW_TAG_enumeration_type has invalid DW_AT_declaration");
	}
	if (declaration && tag) {
		err = drgn_debug_info_find_complete(dbinfo,
						    DW_TAG_enumeration_type,
						    tag, ret);
		if (err != &drgn_not_found)
			return err;
	}

	if (declaration) {
		return drgn_incomplete_enum_type_create(dbinfo->prog, tag, lang,
							ret);
	}

	struct drgn_enum_type_builder builder;
	drgn_enum_type_builder_init(&builder, dbinfo->prog);
	bool is_signed = false;
	Dwarf_Die child;
	int r = dwarf_child(die, &child);
	while (r == 0) {
		if (dwarf_tag(&child) == DW_TAG_enumerator) {
			err = parse_enumerator(&child, &builder, &is_signed);
			if (err)
				goto err;
		}
		r = dwarf_siblingof(&child, &child);
	}
	if (r == -1) {
		err = drgn_error_create(DRGN_ERROR_OTHER,
					"libdw could not parse DIE children");
		goto err;
	}

	struct drgn_type *compatible_type;
	r = dwarf_type(die, &child);
	if (r == -1) {
		err = drgn_error_create(DRGN_ERROR_OTHER,
					"DW_TAG_enumeration_type has invalid DW_AT_type");
		goto err;
	} else if (r) {
		err = enum_compatible_type_fallback(dbinfo, die, is_signed,
						    lang, &compatible_type);
		if (err)
			goto err;
	} else {
		struct drgn_qualified_type qualified_compatible_type;
		err = drgn_type_from_dwarf(dbinfo, module, &child,
					   &qualified_compatible_type);
		if (err)
			goto err;
		compatible_type =
			drgn_underlying_type(qualified_compatible_type.type);
		if (drgn_type_kind(compatible_type) != DRGN_TYPE_INT) {
			err = drgn_error_create(DRGN_ERROR_OTHER,
						"DW_AT_type of DW_TAG_enumeration_type is not an integer type");
			goto err;
		}
	}

	err = drgn_enum_type_create(&builder, tag, compatible_type, lang, ret);
	if (err)
		goto err;
	return NULL;

err:
	drgn_enum_type_builder_deinit(&builder);
	return err;
}

static struct drgn_error *
drgn_typedef_type_from_dwarf(struct drgn_debug_info *dbinfo,
			     struct drgn_debug_info_module *module,
			     Dwarf_Die *die, const struct drgn_language *lang,
			     bool can_be_incomplete_array,
			     bool *is_incomplete_array_ret,
			     struct drgn_type **ret)
{
	const char *name = dwarf_diename(die);
	if (!name) {
		return drgn_error_create(DRGN_ERROR_OTHER,
					 "DW_TAG_typedef has missing or invalid DW_AT_name");
	}

	struct drgn_qualified_type aliased_type;
	struct drgn_error *err = drgn_type_from_dwarf_attr(dbinfo, module, die,
							   lang, true,
							   can_be_incomplete_array,
							   is_incomplete_array_ret,
							   &aliased_type);
	if (err)
		return err;

	return drgn_typedef_type_create(dbinfo->prog, name, aliased_type, lang,
					ret);
}

static struct drgn_error *
drgn_pointer_type_from_dwarf(struct drgn_debug_info *dbinfo,
			     struct drgn_debug_info_module *module,
			     Dwarf_Die *die, const struct drgn_language *lang,
			     struct drgn_type **ret)
{
	struct drgn_qualified_type referenced_type;
	struct drgn_error *err = drgn_type_from_dwarf_attr(dbinfo, module, die,
							   lang, true, true,
							   NULL,
							   &referenced_type);
	if (err)
		return err;

	Dwarf_Attribute attr_mem, *attr;
	uint64_t size;
	if ((attr = dwarf_attr_integrate(die, DW_AT_byte_size, &attr_mem))) {
		Dwarf_Word word;
		if (dwarf_formudata(attr, &word)) {
			return drgn_error_format(DRGN_ERROR_OTHER,
						 "DW_TAG_pointer_type has invalid DW_AT_byte_size");
		}
		size = word;
	} else {
		uint8_t address_size;
		err = drgn_program_address_size(dbinfo->prog, &address_size);
		if (err)
			return err;
		size = address_size;
	}

	/*
	 * The DWARF 5 specification doesn't mention DW_AT_endianity for
	 * DW_TAG_pointer_type DIEs, and GCC as of version 10.2 doesn't emit it
	 * even for pointers stored in the opposite byte order (e.g., when using
	 * scalar_storage_order), but it probably should.
	 */
	enum drgn_byte_order byte_order;
	dwarf_die_byte_order(die, false, &byte_order);
	return drgn_pointer_type_create(dbinfo->prog, referenced_type, size,
					byte_order, lang, ret);
}

struct array_dimension {
	uint64_t length;
	bool is_complete;
};

DEFINE_VECTOR(array_dimension_vector, struct array_dimension)

static struct drgn_error *subrange_length(Dwarf_Die *die,
					  struct array_dimension *dimension)
{
	Dwarf_Attribute attr_mem;
	Dwarf_Attribute *attr;
	Dwarf_Word word;

	if (!(attr = dwarf_attr_integrate(die, DW_AT_upper_bound, &attr_mem)) &&
	    !(attr = dwarf_attr_integrate(die, DW_AT_count, &attr_mem))) {
		dimension->is_complete = false;
		return NULL;
	}

	if (dwarf_formudata(attr, &word)) {
		return drgn_error_format(DRGN_ERROR_OTHER,
					 "DW_TAG_subrange_type has invalid %s",
					 attr->code == DW_AT_upper_bound ?
					 "DW_AT_upper_bound" :
					 "DW_AT_count");
	}

	dimension->is_complete = true;
	/*
	 * GCC emits a DW_FORM_sdata DW_AT_upper_bound of -1 for empty array
	 * variables without an explicit size (e.g., `int arr[] = {};`).
	 */
	if (attr->code == DW_AT_upper_bound && attr->form == DW_FORM_sdata &&
	    word == (Dwarf_Word)-1) {
		dimension->length = 0;
	} else if (attr->code == DW_AT_upper_bound) {
		if (word >= UINT64_MAX) {
			return drgn_error_create(DRGN_ERROR_OVERFLOW,
						 "DW_AT_upper_bound is too large");
		}
		dimension->length = (uint64_t)word + 1;
	} else {
		if (word > UINT64_MAX) {
			return drgn_error_create(DRGN_ERROR_OVERFLOW,
						 "DW_AT_count is too large");
		}
		dimension->length = word;
	}
	return NULL;
}

static struct drgn_error *
drgn_array_type_from_dwarf(struct drgn_debug_info *dbinfo,
			   struct drgn_debug_info_module *module,
			   Dwarf_Die *die, const struct drgn_language *lang,
			   bool can_be_incomplete_array,
			   bool *is_incomplete_array_ret,
			   struct drgn_type **ret)
{
	struct drgn_error *err;
	struct array_dimension_vector dimensions = VECTOR_INIT;
	struct array_dimension *dimension;
	Dwarf_Die child;
	int r = dwarf_child(die, &child);
	while (r == 0) {
		if (dwarf_tag(&child) == DW_TAG_subrange_type) {
			dimension = array_dimension_vector_append_entry(&dimensions);
			if (!dimension)
				goto out;
			err = subrange_length(&child, dimension);
			if (err)
				goto out;
		}
		r = dwarf_siblingof(&child, &child);
	}
	if (r == -1) {
		err = drgn_error_create(DRGN_ERROR_OTHER,
					"libdw could not parse DIE children");
		goto out;
	}
	if (!dimensions.size) {
		dimension = array_dimension_vector_append_entry(&dimensions);
		if (!dimension)
			goto out;
		dimension->is_complete = false;
	}

	struct drgn_qualified_type element_type;
	err = drgn_type_from_dwarf_attr(dbinfo, module, die, lang, false, false,
					NULL, &element_type);
	if (err)
		goto out;

	*is_incomplete_array_ret = !dimensions.data[0].is_complete;
	struct drgn_type *type;
	do {
		dimension = array_dimension_vector_pop(&dimensions);
		if (dimension->is_complete) {
			err = drgn_array_type_create(dbinfo->prog, element_type,
						     dimension->length, lang,
						     &type);
		} else if (dimensions.size || !can_be_incomplete_array) {
			err = drgn_array_type_create(dbinfo->prog, element_type,
						     0, lang, &type);
		} else {
			err = drgn_incomplete_array_type_create(dbinfo->prog,
								element_type,
								lang, &type);
		}
		if (err)
			goto out;

		element_type.type = type;
		element_type.qualifiers = 0;
	} while (dimensions.size);

	*ret = type;
	err = NULL;
out:
	array_dimension_vector_deinit(&dimensions);
	return err;
}

static struct drgn_error *
drgn_dwarf_formal_parameter_thunk_fn(struct drgn_object *res, void *arg_)
{
	struct drgn_error *err;
	struct drgn_dwarf_die_thunk_arg *arg = arg_;
	if (res) {
		struct drgn_qualified_type qualified_type;
		err = drgn_type_from_dwarf_attr(drgn_object_program(res)->dbinfo,
						arg->module, &arg->die, NULL,
						false, true, NULL,
						&qualified_type);
		if (err)
			return err;

		err = drgn_object_set_absent(res, qualified_type, 0);
		if (err)
			return err;
	}
	free(arg);
	return NULL;
}

static struct drgn_error *
parse_formal_parameter(struct drgn_debug_info *dbinfo,
		       struct drgn_debug_info_module *module, Dwarf_Die *die,
		       struct drgn_function_type_builder *builder)
{
	Dwarf_Attribute attr_mem, *attr;
	const char *name;
	if ((attr = dwarf_attr_integrate(die, DW_AT_name, &attr_mem))) {
		name = dwarf_formstring(attr);
		if (!name) {
			return drgn_error_create(DRGN_ERROR_OTHER,
						 "DW_TAG_formal_parameter has invalid DW_AT_name");
		}
	} else {
		name = NULL;
	}

	struct drgn_dwarf_die_thunk_arg *thunk_arg =
		malloc(sizeof(*thunk_arg));
	if (!thunk_arg)
		return &drgn_enomem;
	thunk_arg->module = module;
	thunk_arg->die = *die;

	union drgn_lazy_object default_argument;
	drgn_lazy_object_init_thunk(&default_argument, dbinfo->prog,
				    drgn_dwarf_formal_parameter_thunk_fn,
				    thunk_arg);

	struct drgn_error *err =
		drgn_function_type_builder_add_parameter(builder,
							 &default_argument,
							 name);
	if (err)
		drgn_lazy_object_deinit(&default_argument);
	return err;
}

static struct drgn_error *
drgn_function_type_from_dwarf(struct drgn_debug_info *dbinfo,
			      struct drgn_debug_info_module *module,
			      Dwarf_Die *die, const struct drgn_language *lang,
			      struct drgn_type **ret)
{
	struct drgn_error *err;
	char tag_buf[DW_TAG_BUF_LEN];

	struct drgn_function_type_builder builder;
	drgn_function_type_builder_init(&builder, dbinfo->prog);
	bool is_variadic = false;
	Dwarf_Die child;
	int r = dwarf_child(die, &child);
	while (r == 0) {
		switch (dwarf_tag(&child)) {
		case DW_TAG_formal_parameter:
			if (is_variadic) {
				err = drgn_error_format(DRGN_ERROR_OTHER,
							"%s has DW_TAG_formal_parameter child after DW_TAG_unspecified_parameters child",
							dwarf_tag_str(die,
								      tag_buf));
				goto err;
			}
			err = parse_formal_parameter(dbinfo, module, &child,
						     &builder);
			if (err)
				goto err;
			break;
		case DW_TAG_unspecified_parameters:
			if (is_variadic) {
				err = drgn_error_format(DRGN_ERROR_OTHER,
							"%s has multiple DW_TAG_unspecified_parameters children",
							dwarf_tag_str(die,
								      tag_buf));
				goto err;
			}
			is_variadic = true;
			break;
		case DW_TAG_template_type_parameter:
			err = parse_template_parameter(dbinfo, module, &child,
						       drgn_dwarf_template_type_parameter_thunk_fn,
						       &builder.template_builder);
			if (err)
				goto err;
			break;
		case DW_TAG_template_value_parameter:
			err = parse_template_parameter(dbinfo, module, &child,
						       drgn_dwarf_template_value_parameter_thunk_fn,
						       &builder.template_builder);
			if (err)
				goto err;
			break;
		default:
			break;
		}
		r = dwarf_siblingof(&child, &child);
	}
	if (r == -1) {
		err = drgn_error_create(DRGN_ERROR_OTHER,
					"libdw could not parse DIE children");
		goto err;
	}

	struct drgn_qualified_type return_type;
	err = drgn_type_from_dwarf_attr(dbinfo, module, die, lang, true, true,
					NULL, &return_type);
	if (err)
		goto err;

	err = drgn_function_type_create(&builder, return_type, is_variadic,
					lang, ret);
	if (err)
		goto err;
	return NULL;

err:
	drgn_function_type_builder_deinit(&builder);
	return err;
}

static struct drgn_error *
drgn_type_from_dwarf_internal(struct drgn_debug_info *dbinfo,
			      struct drgn_debug_info_module *module,
			      Dwarf_Die *die, bool can_be_incomplete_array,
			      bool *is_incomplete_array_ret,
			      struct drgn_qualified_type *ret)
{
	if (dbinfo->dwarf.depth >= 1000) {
		return drgn_error_create(DRGN_ERROR_RECURSION,
					 "maximum DWARF type parsing depth exceeded");
	}

	/* If the DIE has a type unit signature, follow it. */
	Dwarf_Die definition_die;
	{
		Dwarf_Attribute attr_mem, *attr;
		if ((attr = dwarf_attr_integrate(die, DW_AT_signature,
						 &attr_mem))) {
			if (!dwarf_formref_die(attr, &definition_die))
				return drgn_error_libdw();
			die = &definition_die;
		}
	}

	/* If we got a declaration, try to find the definition. */
	bool declaration;
	if (dwarf_flag(die, DW_AT_declaration, &declaration))
		return drgn_error_libdw();
	if (declaration) {
		uintptr_t die_addr;
		if (drgn_dwarf_index_find_definition(&dbinfo->dwarf.index,
						     (uintptr_t)die->addr,
						     &module, &die_addr)) {
			Dwarf_Addr bias;
			Dwarf *dwarf = dwfl_module_getdwarf(module->dwfl_module,
							    &bias);
			if (!dwarf)
				return drgn_error_libdwfl();
			uintptr_t start =
				(uintptr_t)module->scn_data[DRGN_SCN_DEBUG_INFO]->d_buf;
			size_t size =
				module->scn_data[DRGN_SCN_DEBUG_INFO]->d_size;
			if (die_addr >= start && die_addr < start + size) {
				if (!dwarf_offdie(dwarf, die_addr - start,
						  &definition_die))
					return drgn_error_libdw();
			} else {
				start = (uintptr_t)module->scn_data[DRGN_SCN_DEBUG_TYPES]->d_buf;
				/* Assume .debug_types */
				if (!dwarf_offdie_types(dwarf, die_addr - start,
							&definition_die))
					return drgn_error_libdw();
			}
			die = &definition_die;
		}
	}

	struct drgn_dwarf_type_map_entry entry = {
		.key = die->addr,
	};
	struct hash_pair hp = drgn_dwarf_type_map_hash(&entry.key);
	struct drgn_dwarf_type_map_iterator it =
		drgn_dwarf_type_map_search_hashed(&dbinfo->dwarf.types,
						  &entry.key, hp);
	if (it.entry) {
		if (!can_be_incomplete_array &&
		    it.entry->value.is_incomplete_array) {
			it = drgn_dwarf_type_map_search_hashed(&dbinfo->dwarf.cant_be_incomplete_array_types,
							       &entry.key, hp);
		}
		if (it.entry) {
			ret->type = it.entry->value.type;
			ret->qualifiers = it.entry->value.qualifiers;
			return NULL;
		}
	}

	const struct drgn_language *lang;
	struct drgn_error *err = drgn_language_from_die(die, true, &lang);
	if (err)
		return err;

	ret->qualifiers = 0;
	dbinfo->dwarf.depth++;
	entry.value.is_incomplete_array = false;
	switch (dwarf_tag(die)) {
	case DW_TAG_const_type:
		err = drgn_type_from_dwarf_attr(dbinfo, module, die, lang, true,
						can_be_incomplete_array,
						&entry.value.is_incomplete_array,
						ret);
		ret->qualifiers |= DRGN_QUALIFIER_CONST;
		break;
	case DW_TAG_restrict_type:
		err = drgn_type_from_dwarf_attr(dbinfo, module, die, lang, true,
						can_be_incomplete_array,
						&entry.value.is_incomplete_array,
						ret);
		ret->qualifiers |= DRGN_QUALIFIER_RESTRICT;
		break;
	case DW_TAG_volatile_type:
		err = drgn_type_from_dwarf_attr(dbinfo, module, die, lang, true,
						can_be_incomplete_array,
						&entry.value.is_incomplete_array,
						ret);
		ret->qualifiers |= DRGN_QUALIFIER_VOLATILE;
		break;
	case DW_TAG_atomic_type:
		err = drgn_type_from_dwarf_attr(dbinfo, module, die, lang, true,
						can_be_incomplete_array,
						&entry.value.is_incomplete_array,
						ret);
		ret->qualifiers |= DRGN_QUALIFIER_ATOMIC;
		break;
	case DW_TAG_base_type:
		err = drgn_base_type_from_dwarf(dbinfo, module, die, lang,
						&ret->type);
		break;
	case DW_TAG_structure_type:
		err = drgn_compound_type_from_dwarf(dbinfo, module, die, lang,
						    DRGN_TYPE_STRUCT,
						    &ret->type);
		break;
	case DW_TAG_union_type:
		err = drgn_compound_type_from_dwarf(dbinfo, module, die, lang,
						    DRGN_TYPE_UNION,
						    &ret->type);
		break;
	case DW_TAG_class_type:
		err = drgn_compound_type_from_dwarf(dbinfo, module, die, lang,
						    DRGN_TYPE_CLASS,
						    &ret->type);
		break;
	case DW_TAG_enumeration_type:
		err = drgn_enum_type_from_dwarf(dbinfo, module, die, lang,
						&ret->type);
		break;
	case DW_TAG_typedef:
		err = drgn_typedef_type_from_dwarf(dbinfo, module, die, lang,
						   can_be_incomplete_array,
						   &entry.value.is_incomplete_array,
						   &ret->type);
		break;
	case DW_TAG_pointer_type:
		err = drgn_pointer_type_from_dwarf(dbinfo, module, die, lang,
						   &ret->type);
		break;
	case DW_TAG_array_type:
		err = drgn_array_type_from_dwarf(dbinfo, module, die, lang,
						 can_be_incomplete_array,
						 &entry.value.is_incomplete_array,
						 &ret->type);
		break;
	case DW_TAG_subroutine_type:
	case DW_TAG_subprogram:
		err = drgn_function_type_from_dwarf(dbinfo, module, die, lang,
						    &ret->type);
		break;
	default:
		err = drgn_error_format(DRGN_ERROR_OTHER,
					"unknown DWARF type tag 0x%x",
					dwarf_tag(die));
		break;
	}
	dbinfo->dwarf.depth--;
	if (err)
		return err;

	entry.value.type = ret->type;
	entry.value.qualifiers = ret->qualifiers;
	struct drgn_dwarf_type_map *map;
	if (!can_be_incomplete_array && entry.value.is_incomplete_array)
		map = &dbinfo->dwarf.cant_be_incomplete_array_types;
	else
		map = &dbinfo->dwarf.types;
	if (drgn_dwarf_type_map_insert_searched(map, &entry, hp, NULL) == -1) {
		/*
		 * This will "leak" the type we created, but it'll still be
		 * cleaned up when the program is freed.
		 */
		return &drgn_enomem;
	}
	if (is_incomplete_array_ret)
		*is_incomplete_array_ret = entry.value.is_incomplete_array;
	return NULL;
}

struct drgn_error *drgn_debug_info_find_type(enum drgn_type_kind kind,
					     const char *name, size_t name_len,
					     const char *filename, void *arg,
					     struct drgn_qualified_type *ret)
{
	struct drgn_error *err;
	struct drgn_debug_info *dbinfo = arg;

	uint64_t tag;
	switch (kind) {
	case DRGN_TYPE_INT:
	case DRGN_TYPE_BOOL:
	case DRGN_TYPE_FLOAT:
		tag = DW_TAG_base_type;
		break;
	case DRGN_TYPE_STRUCT:
		tag = DW_TAG_structure_type;
		break;
	case DRGN_TYPE_UNION:
		tag = DW_TAG_union_type;
		break;
	case DRGN_TYPE_CLASS:
		tag = DW_TAG_class_type;
		break;
	case DRGN_TYPE_ENUM:
		tag = DW_TAG_enumeration_type;
		break;
	case DRGN_TYPE_TYPEDEF:
		tag = DW_TAG_typedef;
		break;
	default:
		UNREACHABLE();
	}

	struct drgn_dwarf_index_iterator it;
	err = drgn_dwarf_index_iterator_init(&it, &dbinfo->dwarf.index.global,
					     name, name_len, &tag, 1);
	if (err)
		return err;
	struct drgn_dwarf_index_die *index_die;
	while ((index_die = drgn_dwarf_index_iterator_next(&it))) {
		Dwarf_Die die;
		err = drgn_dwarf_index_get_die(index_die, &die);
		if (err)
			return err;
		if (die_matches_filename(&die, filename)) {
			err = drgn_type_from_dwarf(dbinfo, index_die->module,
						   &die, ret);
			if (err)
				return err;
			/*
			 * For DW_TAG_base_type, we need to check that the type
			 * we found was the right kind.
			 */
			if (drgn_type_kind(ret->type) == kind)
				return NULL;
		}
	}
	return &drgn_not_found;
}

struct drgn_error *
drgn_debug_info_find_object(const char *name, size_t name_len,
			    const char *filename,
			    enum drgn_find_object_flags flags, void *arg,
			    struct drgn_object *ret)
{
	struct drgn_error *err;
	struct drgn_debug_info *dbinfo = arg;

	struct drgn_dwarf_index_namespace *ns = &dbinfo->dwarf.index.global;
	if (name_len >= 2 && memcmp(name, "::", 2) == 0) {
		/* Explicit global namespace. */
		name_len -= 2;
		name += 2;
	}
	const char *colons;
	while ((colons = memmem(name, name_len, "::", 2))) {
		struct drgn_dwarf_index_iterator it;
		uint64_t ns_tag = DW_TAG_namespace;
		err = drgn_dwarf_index_iterator_init(&it, ns, name,
						     colons - name, &ns_tag, 1);
		if (err)
			return err;
		struct drgn_dwarf_index_die *index_die =
			drgn_dwarf_index_iterator_next(&it);
		if (!index_die)
			return &drgn_not_found;
		ns = index_die->namespace;
		name_len -= colons + 2 - name;
		name = colons + 2;
	}

	uint64_t tags[3];
	size_t num_tags = 0;
	if (flags & DRGN_FIND_OBJECT_CONSTANT)
		tags[num_tags++] = DW_TAG_enumerator;
	if (flags & DRGN_FIND_OBJECT_FUNCTION)
		tags[num_tags++] = DW_TAG_subprogram;
	if (flags & DRGN_FIND_OBJECT_VARIABLE)
		tags[num_tags++] = DW_TAG_variable;

	struct drgn_dwarf_index_iterator it;
	err = drgn_dwarf_index_iterator_init(&it, ns, name, name_len, tags,
					     num_tags);
	if (err)
		return err;
	struct drgn_dwarf_index_die *index_die;
	while ((index_die = drgn_dwarf_index_iterator_next(&it))) {
		Dwarf_Die die;
		err = drgn_dwarf_index_get_die(index_die, &die);
		if (err)
			return err;
		if (!die_matches_filename(&die, filename))
			continue;
		if (dwarf_tag(&die) == DW_TAG_enumeration_type) {
			return drgn_object_from_dwarf_enumerator(dbinfo,
								 index_die->module,
								 &die, name,
								 ret);
		} else {
			return drgn_object_from_dwarf(dbinfo, index_die->module,
						      &die, NULL, NULL, NULL,
						      ret);
		}
	}
	return &drgn_not_found;
}

/*
 * Call frame information.
 */

struct drgn_dwarf_cie {
	/* Whether this CIE is from .eh_frame. */
	bool is_eh;
	/* Size of an address in this CIE in bytes. */
	uint8_t address_size;
	/* DW_EH_PE_* encoding of addresses in this CIE. */
	uint8_t address_encoding;
	/* Whether this CIE has a 'z' augmentation. */
	bool have_augmentation_length;
	/* Whether this CIE is for a signal handler ('S' augmentation). */
	bool signal_frame;
	drgn_register_number return_address_register;
	uint64_t code_alignment_factor;
	int64_t data_alignment_factor;
	const char *initial_instructions;
	size_t initial_instructions_size;
};

DEFINE_VECTOR(drgn_dwarf_fde_vector, struct drgn_dwarf_fde)
DEFINE_VECTOR(drgn_dwarf_cie_vector, struct drgn_dwarf_cie)
DEFINE_HASH_MAP(drgn_dwarf_cie_map, size_t, size_t, int_key_hash_pair,
		scalar_key_eq)

static struct drgn_error *
drgn_dwarf_cfi_next_encoded(struct drgn_debug_info_buffer *buffer,
			    uint8_t address_size, uint8_t encoding,
			    uint64_t func_addr, uint64_t *ret)
{
	struct drgn_error *err;

	/* Not currently used for CFI. */
	if (encoding & DW_EH_PE_indirect) {
unknown_fde_encoding:
		return binary_buffer_error(&buffer->bb,
					   "unknown EH encoding %#" PRIx8,
					   encoding);
	}

	size_t pos = (buffer->bb.pos -
		      (char *)buffer->module->scn_data[buffer->scn]->d_buf);
	uint64_t base;
	switch (encoding & 0x70) {
	case DW_EH_PE_absptr:
		base = 0;
		break;
	case DW_EH_PE_pcrel:
		base = buffer->module->dwarf.pcrel_base + pos;
		break;
	case DW_EH_PE_textrel:
		base = buffer->module->dwarf.textrel_base;
		break;
	case DW_EH_PE_datarel:
		base = buffer->module->dwarf.datarel_base;
		break;
	case DW_EH_PE_funcrel:
		/* Relative to the FDE's initial location. */
		base = func_addr;
		break;
	case DW_EH_PE_aligned:
		base = 0;
		if (pos % address_size != 0 &&
		    (err = binary_buffer_skip(&buffer->bb,
					      address_size - pos % address_size)))
			return err;
		break;
	default:
		goto unknown_fde_encoding;
	}

	uint64_t offset;
	switch (encoding & 0xf) {
	case DW_EH_PE_absptr:
		if ((err = binary_buffer_next_uint(&buffer->bb, address_size,
						   &offset)))
			return err;
		break;
	case DW_EH_PE_uleb128:
		if ((err = binary_buffer_next_uleb128(&buffer->bb, &offset)))
			return err;
		break;
	case DW_EH_PE_udata2:
		if ((err = binary_buffer_next_u16_into_u64(&buffer->bb,
							   &offset)))
			return err;
		break;
	case DW_EH_PE_udata4:
		if ((err = binary_buffer_next_u32_into_u64(&buffer->bb,
							   &offset)))
			return err;
		break;
	case DW_EH_PE_udata8:
		if ((err = binary_buffer_next_u64(&buffer->bb, &offset)))
			return err;
		break;
	case DW_EH_PE_sleb128:
		if ((err = binary_buffer_next_sleb128_into_u64(&buffer->bb,
							       &offset)))
			return err;
		break;
	case DW_EH_PE_sdata2:
		if ((err = binary_buffer_next_s16_into_u64(&buffer->bb,
							   &offset)))
			return err;
		break;
	case DW_EH_PE_sdata4:
		if ((err = binary_buffer_next_s32_into_u64(&buffer->bb,
							   &offset)))
			return err;
		break;
	case DW_EH_PE_sdata8:
		if ((err = binary_buffer_next_s64_into_u64(&buffer->bb,
							   &offset)))
			return err;
		break;
	default:
		goto unknown_fde_encoding;
	}
	*ret = (base + offset) & uint_max(address_size);

	return NULL;
}

static struct drgn_error *
drgn_parse_dwarf_cie(struct drgn_debug_info_module *module,
		     enum drgn_debug_info_scn scn, size_t cie_pointer,
		     struct drgn_dwarf_cie *cie)
{
	bool is_eh = scn == DRGN_SCN_EH_FRAME;
	struct drgn_error *err;

	cie->is_eh = is_eh;

	struct drgn_debug_info_buffer buffer;
	drgn_debug_info_buffer_init(&buffer, module, scn);
	buffer.bb.pos += cie_pointer;

	uint32_t tmp;
	if ((err = binary_buffer_next_u32(&buffer.bb, &tmp)))
		return err;
	bool is_64_bit = tmp == UINT32_C(0xffffffff);
	uint64_t length;
	if (is_64_bit) {
		if ((err = binary_buffer_next_u64(&buffer.bb, &length)))
			return err;
	} else {
		length = tmp;
	}
	if (length > buffer.bb.end - buffer.bb.pos) {
		return binary_buffer_error(&buffer.bb,
					   "entry length is out of bounds");
	}
	buffer.bb.end = buffer.bb.pos + length;

	uint64_t cie_id, expected_cie_id;
	if (is_64_bit) {
		if ((err = binary_buffer_next_u64(&buffer.bb, &cie_id)))
			return err;
		expected_cie_id = is_eh ? 0 : UINT64_C(0xffffffffffffffff);
	} else {
		if ((err = binary_buffer_next_u32_into_u64(&buffer.bb,
							   &cie_id)))
			return err;
		expected_cie_id = is_eh ? 0 : UINT64_C(0xffffffff);
	}
	if (cie_id != expected_cie_id)
		return binary_buffer_error(&buffer.bb, "invalid CIE ID");

	uint8_t version;
	if ((err = binary_buffer_next_u8(&buffer.bb, &version)))
		return err;
	if (version < 1 || version == 2 || version > 4) {
		return binary_buffer_error(&buffer.bb,
					   "unknown CIE version %" PRIu8,
					   version);
	}

	const char *augmentation;
	size_t augmentation_len;
	if ((err = binary_buffer_next_string(&buffer.bb, &augmentation,
					     &augmentation_len)))
		return err;
	cie->have_augmentation_length = augmentation[0] == 'z';
	cie->signal_frame = false;
	for (size_t i = 0; i < augmentation_len; i++) {
		switch (augmentation[i]) {
		case 'z':
			if (i != 0)
				goto unknown_augmentation;
			break;
		case 'L':
		case 'P':
		case 'R':
			if (augmentation[0] != 'z')
				goto unknown_augmentation;
			break;
		case 'S':
			cie->signal_frame = true;
			break;
		default:
unknown_augmentation:
			/*
			 * We could ignore this CIE and all FDEs that reference
			 * it or skip the augmentation if we have its length,
			 * but let's fail loudly so that we find out about
			 * missing support.
			 */
			return binary_buffer_error_at(&buffer.bb,
						      &augmentation[i],
						      "unknown CFI augmentation %s",
						      augmentation);
		}
	}

	if (version >= 4) {
		if ((err = binary_buffer_next_u8(&buffer.bb,
						 &cie->address_size)))
			return err;
		if (cie->address_size < 1 || cie->address_size > 8) {
			return binary_buffer_error(&buffer.bb,
						   "unsupported address size %" PRIu8,
						   cie->address_size);
		}
		uint8_t segment_selector_size;
		if ((err = binary_buffer_next_u8(&buffer.bb,
						 &segment_selector_size)))
			return err;
		if (segment_selector_size) {
			return binary_buffer_error(&buffer.bb,
						   "unsupported segment selector size %" PRIu8,
						   segment_selector_size);
		}
	} else {
		cie->address_size =
			drgn_platform_address_size(&module->platform);
	}
	if ((err = binary_buffer_next_uleb128(&buffer.bb,
					      &cie->code_alignment_factor)) ||
	    (err = binary_buffer_next_sleb128(&buffer.bb,
					      &cie->data_alignment_factor)))
		return err;
	uint64_t return_address_register;
	if (version >= 3) {
		if ((err = binary_buffer_next_uleb128(&buffer.bb,
						      &return_address_register)))
			return err;
	} else {
		if ((err = binary_buffer_next_u8_into_u64(&buffer.bb,
							  &return_address_register)))
			return err;
	}
	cie->return_address_register =
		module->platform.arch->dwarf_regno_to_internal(return_address_register);
	if (cie->return_address_register == DRGN_REGISTER_NUMBER_UNKNOWN) {
		return binary_buffer_error(&buffer.bb,
					   "unknown return address register");
	}
	cie->address_encoding = DW_EH_PE_absptr;
	if (augmentation[0] == 'z') {
		for (size_t i = 0; i < augmentation_len; i++) {
			switch (augmentation[i]) {
			case 'z':
				if ((err = binary_buffer_skip_leb128(&buffer.bb)))
					return err;
				break;
			case 'L':
				if ((err = binary_buffer_skip(&buffer.bb, 1)))
					return err;
				break;
			case 'P': {
				uint8_t encoding;
				if ((err = binary_buffer_next_u8(&buffer.bb, &encoding)))
					return err;
				/*
				 * We don't need the result, so don't bother
				 * dereferencing.
				 */
				encoding &= ~DW_EH_PE_indirect;
				uint64_t unused;
				if ((err = drgn_dwarf_cfi_next_encoded(&buffer,
								       cie->address_size,
								       encoding,
								       0,
								       &unused)))
					return err;
				break;
			}
			case 'R':
				if ((err = binary_buffer_next_u8(&buffer.bb,
								 &cie->address_encoding)))
					return err;
				break;
			}
		}
	}
	cie->initial_instructions = buffer.bb.pos;
	cie->initial_instructions_size = buffer.bb.end - buffer.bb.pos;
	return NULL;
}

static struct drgn_error *
drgn_parse_dwarf_frames(struct drgn_debug_info_module *module,
			enum drgn_debug_info_scn scn,
			struct drgn_dwarf_cie_vector *cies,
			struct drgn_dwarf_fde_vector *fdes)
{
	bool is_eh = scn == DRGN_SCN_EH_FRAME;
	struct drgn_error *err;

	if (!module->scns[scn])
		return NULL;
	err = drgn_debug_info_module_cache_section(module, scn);
	if (err)
		return err;
	Elf_Data *data = module->scn_data[scn];
	struct drgn_debug_info_buffer buffer;
	drgn_debug_info_buffer_init(&buffer, module, scn);

	struct drgn_dwarf_cie_map cie_map = HASH_TABLE_INIT;
	while (binary_buffer_has_next(&buffer.bb)) {
		uint32_t tmp;
		if ((err = binary_buffer_next_u32(&buffer.bb, &tmp)))
			goto out;
		bool is_64_bit = tmp == UINT32_C(0xffffffff);
		uint64_t length;
		if (is_64_bit) {
			if ((err = binary_buffer_next_u64(&buffer.bb, &length)))
				goto out;
		} else {
			length = tmp;
		}
		/*
		 * Technically, a length of zero is only a terminator in
		 * .eh_frame, but other consumers (binutils, elfutils, GDB)
		 * handle it the same way in .debug_frame.
		 */
		if (length == 0)
			break;
		if (length > buffer.bb.end - buffer.bb.pos) {
			err = binary_buffer_error(&buffer.bb,
						  "entry length is out of bounds");
			goto out;
		}
		buffer.bb.end = buffer.bb.pos + length;

		/*
		 * The Linux Standard Base Core Specification [1] states that
		 * the CIE ID in .eh_frame is always 4 bytes. However, other
		 * consumers handle it the same as in .debug_frame (8 bytes for
		 * the 64-bit format).
		 *
		 * 1: https://refspecs.linuxfoundation.org/LSB_5.0.0/LSB-Core-generic/LSB-Core-generic/ehframechpt.html
		 */
		uint64_t cie_pointer, cie_id;
		if (is_64_bit) {
			if ((err = binary_buffer_next_u64(&buffer.bb,
							  &cie_pointer)))
				goto out;
			cie_id = is_eh ? 0 : UINT64_C(0xffffffffffffffff);
		} else {
			if ((err = binary_buffer_next_u32_into_u64(&buffer.bb,
								   &cie_pointer)))
				goto out;
			cie_id = is_eh ? 0 : UINT64_C(0xffffffff);
		}

		if (cie_pointer != cie_id) {
			if (is_eh) {
				size_t pointer_offset =
					(buffer.bb.pos
					 - (is_64_bit ? 8 : 4)
					 - (char *)data->d_buf);
				if (cie_pointer > pointer_offset) {
					err = binary_buffer_error(&buffer.bb,
								  "CIE pointer is out of bounds");
					goto out;
				}
				cie_pointer = pointer_offset - cie_pointer;
			} else if (cie_pointer > data->d_size) {
				err = binary_buffer_error(&buffer.bb,
							  "CIE pointer is out of bounds");
				goto out;
			}
			struct drgn_dwarf_fde *fde =
				drgn_dwarf_fde_vector_append_entry(fdes);
			if (!fde) {
				err = &drgn_enomem;
				goto out;
			}
			struct drgn_dwarf_cie_map_entry entry = {
				.key = cie_pointer,
				.value = cies->size,
			};
			struct drgn_dwarf_cie_map_iterator it;
			int r = drgn_dwarf_cie_map_insert(&cie_map, &entry,
							  &it);
			struct drgn_dwarf_cie *cie;
			if (r > 0) {
				cie = drgn_dwarf_cie_vector_append_entry(cies);
				if (!cie) {
					err = &drgn_enomem;
					goto out;
				}
				err = drgn_parse_dwarf_cie(module, scn,
							   cie_pointer, cie);
				if (err)
					goto out;
			} else if (r == 0) {
				cie = &cies->data[it.entry->value];
			} else {
				err = &drgn_enomem;
				goto out;
			}
			if ((err = drgn_dwarf_cfi_next_encoded(&buffer,
							       cie->address_size,
							       cie->address_encoding,
							       0,
							       &fde->initial_location)) ||
			    (err = drgn_dwarf_cfi_next_encoded(&buffer,
							       cie->address_size,
							       cie->address_encoding & 0xf,
							       0,
							       &fde->address_range)))
				goto out;
			if (cie->have_augmentation_length) {
				uint64_t augmentation_length;
				if ((err = binary_buffer_next_uleb128(&buffer.bb,
								      &augmentation_length)))
					goto out;
				if (augmentation_length >
				    buffer.bb.end - buffer.bb.pos) {
					err = binary_buffer_error(&buffer.bb,
								  "augmentation length is out of bounds");
					goto out;
				}
				buffer.bb.pos += augmentation_length;
			}
			fde->cie = it.entry->value;
			fde->instructions = buffer.bb.pos;
			fde->instructions_size = buffer.bb.end - buffer.bb.pos;
		}

		buffer.bb.pos = buffer.bb.end;
		buffer.bb.end = (const char *)data->d_buf + data->d_size;
	}

	err = NULL;
out:
	drgn_dwarf_cie_map_deinit(&cie_map);
	return err;
}

static void drgn_debug_info_cache_sh_addr(struct drgn_debug_info_module *module,
					  enum drgn_debug_info_scn scn,
					  uint64_t *addr)
{
	if (module->scns[scn]) {
		GElf_Shdr shdr_mem;
		GElf_Shdr *shdr = gelf_getshdr(module->scns[scn], &shdr_mem);
		if (shdr)
			*addr = shdr->sh_addr;
	}
}

static int drgn_dwarf_fde_compar(const void *_a, const void *_b, void *arg)
{
	const struct drgn_dwarf_fde *a = _a;
	const struct drgn_dwarf_fde *b = _b;
	const struct drgn_dwarf_cie *cies = arg;
	if (a->initial_location < b->initial_location)
		return -1;
	else if (a->initial_location > b->initial_location)
		return 1;
	else
		return cies[a->cie].is_eh - cies[b->cie].is_eh;
}

static struct drgn_error *
drgn_debug_info_parse_frames(struct drgn_debug_info_module *module)
{
	struct drgn_error *err;

	drgn_debug_info_cache_sh_addr(module, DRGN_SCN_EH_FRAME,
				      &module->dwarf.pcrel_base);
	drgn_debug_info_cache_sh_addr(module, DRGN_SCN_TEXT,
				      &module->dwarf.textrel_base);
	drgn_debug_info_cache_sh_addr(module, DRGN_SCN_GOT,
				      &module->dwarf.datarel_base);

	struct drgn_dwarf_cie_vector cies = VECTOR_INIT;
	struct drgn_dwarf_fde_vector fdes = VECTOR_INIT;

	err = drgn_parse_dwarf_frames(module, DRGN_SCN_DEBUG_FRAME, &cies,
				      &fdes);
	if (err)
		goto err;
	err = drgn_parse_dwarf_frames(module, DRGN_SCN_EH_FRAME, &cies, &fdes);
	if (err)
		goto err;

	drgn_dwarf_cie_vector_shrink_to_fit(&cies);

	/*
	 * Sort FDEs and remove duplicates, preferring .debug_frame over
	 * .eh_frame.
	 */
	qsort_r(fdes.data, fdes.size, sizeof(fdes.data[0]),
		drgn_dwarf_fde_compar, cies.data);
	if (fdes.size > 0) {
		size_t src = 1, dst = 1;
		for (; src < fdes.size; src++) {
			if (fdes.data[src].initial_location !=
			    fdes.data[dst - 1].initial_location) {
				if (src != dst)
					fdes.data[dst] = fdes.data[src];
				dst++;
			}
		}
		fdes.size = dst;
	}
	drgn_dwarf_fde_vector_shrink_to_fit(&fdes);

	module->dwarf.cies = cies.data;
	module->dwarf.fdes = fdes.data;
	module->dwarf.num_fdes = fdes.size;
	return NULL;

err:
	drgn_dwarf_fde_vector_deinit(&fdes);
	drgn_dwarf_cie_vector_deinit(&cies);
	return err;
}

static struct drgn_error *
drgn_debug_info_find_fde(struct drgn_debug_info_module *module,
			 uint64_t unbiased_pc, struct drgn_dwarf_fde **ret)
{
	struct drgn_error *err;

	if (!module->parsed_frames) {
		err = drgn_debug_info_parse_frames(module);
		if (err)
			return err;
		module->parsed_frames = true;
	}

	/* Binary search for the containing FDE. */
	size_t lo = 0, hi = module->dwarf.num_fdes;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		struct drgn_dwarf_fde *fde = &module->dwarf.fdes[mid];
		if (unbiased_pc < fde->initial_location) {
			hi = mid;
		} else if (unbiased_pc - fde->initial_location >=
			   fde->address_range) {
			lo = mid + 1;
		} else {
			*ret = fde;
			return NULL;
		}
	}
	*ret = NULL;
	return NULL;
}

static struct drgn_error *
drgn_dwarf_cfi_next_offset(struct drgn_debug_info_buffer *buffer, int64_t *ret)
{
	struct drgn_error *err;
	uint64_t offset;
	if ((err = binary_buffer_next_uleb128(&buffer->bb, &offset)))
		return err;
	if (offset > INT64_MAX)
		return binary_buffer_error(&buffer->bb, "offset is too large");
	*ret = offset;
	return NULL;
}

static struct drgn_error *
drgn_dwarf_cfi_next_offset_sf(struct drgn_debug_info_buffer *buffer,
			      struct drgn_dwarf_cie *cie, int64_t *ret)
{
	struct drgn_error *err;
	int64_t factored;
	if ((err = binary_buffer_next_sleb128(&buffer->bb, &factored)))
		return err;
	if (__builtin_mul_overflow(factored, cie->data_alignment_factor, ret))
		return binary_buffer_error(&buffer->bb, "offset is too large");
	return NULL;
}

static struct drgn_error *
drgn_dwarf_cfi_next_offset_f(struct drgn_debug_info_buffer *buffer,
			     struct drgn_dwarf_cie *cie, int64_t *ret)
{
	struct drgn_error *err;
	uint64_t factored;
	if ((err = binary_buffer_next_uleb128(&buffer->bb, &factored)))
		return err;
	if (__builtin_mul_overflow(factored, cie->data_alignment_factor, ret))
		return binary_buffer_error(&buffer->bb, "offset is too large");
	return NULL;
}

static struct drgn_error *
drgn_dwarf_cfi_next_block(struct drgn_debug_info_buffer *buffer,
			  const char **buf_ret, size_t *size_ret)
{
	struct drgn_error *err;
	uint64_t size;
	if ((err = binary_buffer_next_uleb128(&buffer->bb, &size)))
		return err;
	if (size > buffer->bb.end - buffer->bb.pos) {
		return binary_buffer_error(&buffer->bb,
					   "block is out of bounds");
	}
	*buf_ret = buffer->bb.pos;
	buffer->bb.pos += size;
	*size_ret = size;
	return NULL;
}

DEFINE_VECTOR(drgn_cfi_row_vector, struct drgn_cfi_row *)

static struct drgn_error *
drgn_eval_dwarf_cfi(struct drgn_debug_info_module *module,
		    struct drgn_dwarf_fde *fde,
		    const struct drgn_cfi_row *initial_row, uint64_t target,
		    const char *instructions, size_t instructions_size,
		    struct drgn_cfi_row **row)
{
	struct drgn_error *err;
	drgn_register_number (*dwarf_regno_to_internal)(uint64_t) =
		module->platform.arch->dwarf_regno_to_internal;
	struct drgn_dwarf_cie *cie = &module->dwarf.cies[fde->cie];
	uint64_t pc = fde->initial_location;

	struct drgn_cfi_row_vector state_stack = VECTOR_INIT;
	struct drgn_debug_info_buffer buffer;
	drgn_debug_info_buffer_init(&buffer, module,
				    cie->is_eh ?
				    DRGN_SCN_EH_FRAME : DRGN_SCN_DEBUG_FRAME);
	buffer.bb.pos = instructions;
	buffer.bb.end = instructions + instructions_size;
	while (binary_buffer_has_next(&buffer.bb)) {
		uint8_t opcode;
		if ((err = binary_buffer_next_u8(&buffer.bb, &opcode)))
			goto out;

		uint64_t dwarf_regno;
		drgn_register_number regno;
		struct drgn_cfi_rule rule;
		uint64_t tmp;
		switch ((opcode & 0xc0) ? (opcode & 0xc0) : opcode) {
		case DW_CFA_set_loc:
			if (!initial_row)
				goto invalid_for_initial;
			if ((err = drgn_dwarf_cfi_next_encoded(&buffer,
							       cie->address_size,
							       cie->address_encoding,
							       fde->initial_location,
							       &tmp)))
				goto out;
			if (tmp <= pc) {
				err = binary_buffer_error(&buffer.bb,
							  "DW_CFA_set_loc location is not greater than current location");
				goto out;
			}
			pc = tmp;
			if (pc > target)
				goto found;
			break;
		case DW_CFA_advance_loc:
			if (!initial_row)
				goto invalid_for_initial;
			tmp = opcode & 0x3f;
			goto advance_loc;
		case DW_CFA_advance_loc1:
			if (!initial_row)
				goto invalid_for_initial;
			if ((err = binary_buffer_next_u8_into_u64(&buffer.bb,
								  &tmp)))
				goto out;
			goto advance_loc;
		case DW_CFA_advance_loc2:
			if (!initial_row)
				goto invalid_for_initial;
			if ((err = binary_buffer_next_u16_into_u64(&buffer.bb,
								   &tmp)))
				goto out;
			goto advance_loc;
		case DW_CFA_advance_loc4:
			if (!initial_row)
				goto invalid_for_initial;
			if ((err = binary_buffer_next_u32_into_u64(&buffer.bb,
								   &tmp)))
				goto out;
advance_loc:
			if (__builtin_mul_overflow(tmp,
						   cie->code_alignment_factor,
						   &tmp) ||
			    __builtin_add_overflow(pc, tmp, &pc) ||
			    pc > uint_max(cie->address_size)) {
				err = drgn_error_create(DRGN_ERROR_OTHER,
							"DW_CFA_advance_loc* overflows location");
				goto out;
			}
			if (pc > target)
				goto found;
			break;
		case DW_CFA_def_cfa:
			rule.kind = DRGN_CFI_RULE_REGISTER_PLUS_OFFSET;
			if ((err = binary_buffer_next_uleb128(&buffer.bb,
							      &dwarf_regno)) ||
			    (err = drgn_dwarf_cfi_next_offset(&buffer, &rule.offset)))
				goto out;
			if ((rule.regno = dwarf_regno_to_internal(dwarf_regno)) ==
			    DRGN_REGISTER_NUMBER_UNKNOWN)
				rule.kind = DRGN_CFI_RULE_UNDEFINED;
			goto set_cfa;
		case DW_CFA_def_cfa_sf:
			rule.kind = DRGN_CFI_RULE_REGISTER_PLUS_OFFSET;
			if ((err = binary_buffer_next_uleb128(&buffer.bb,
							      &dwarf_regno)) ||
			    (err = drgn_dwarf_cfi_next_offset_sf(&buffer, cie,
								 &rule.offset)))
				goto out;
			if ((rule.regno = dwarf_regno_to_internal(dwarf_regno)) ==
			    DRGN_REGISTER_NUMBER_UNKNOWN)
				rule.kind = DRGN_CFI_RULE_UNDEFINED;
			goto set_cfa;
		case DW_CFA_def_cfa_register:
			drgn_cfi_row_get_cfa(*row, &rule);
			if (rule.kind != DRGN_CFI_RULE_REGISTER_PLUS_OFFSET) {
				err = binary_buffer_error(&buffer.bb,
							  "DW_CFA_def_cfa_register with incompatible CFA rule");
				goto out;
			}
			if ((err = binary_buffer_next_uleb128(&buffer.bb,
							      &dwarf_regno)))
				goto out;
			if ((rule.regno = dwarf_regno_to_internal(dwarf_regno)) ==
			    DRGN_REGISTER_NUMBER_UNKNOWN)
				rule.kind = DRGN_CFI_RULE_UNDEFINED;
			goto set_cfa;
		case DW_CFA_def_cfa_offset:
			drgn_cfi_row_get_cfa(*row, &rule);
			if (rule.kind != DRGN_CFI_RULE_REGISTER_PLUS_OFFSET) {
				err = binary_buffer_error(&buffer.bb,
							  "DW_CFA_def_cfa_offset with incompatible CFA rule");
				goto out;
			}
			if ((err = drgn_dwarf_cfi_next_offset(&buffer,
							      &rule.offset)))
				goto out;
			goto set_cfa;
		case DW_CFA_def_cfa_offset_sf:
			drgn_cfi_row_get_cfa(*row, &rule);
			if (rule.kind != DRGN_CFI_RULE_REGISTER_PLUS_OFFSET) {
				err = binary_buffer_error(&buffer.bb,
							  "DW_CFA_def_cfa_offset_sf with incompatible CFA rule");
				goto out;
			}
			if ((err = drgn_dwarf_cfi_next_offset_sf(&buffer, cie,
								 &rule.offset)))
				goto out;
			goto set_cfa;
		case DW_CFA_def_cfa_expression:
			rule.kind = DRGN_CFI_RULE_DWARF_EXPRESSION;
			rule.push_cfa = false;
			if ((err = drgn_dwarf_cfi_next_block(&buffer,
							     &rule.expr,
							     &rule.expr_size)))
				goto out;
set_cfa:
			if (!drgn_cfi_row_set_cfa(row, &rule)) {
				err = &drgn_enomem;
				goto out;
			}
			break;
		case DW_CFA_undefined:
			rule.kind = DRGN_CFI_RULE_UNDEFINED;
			if ((err = binary_buffer_next_uleb128(&buffer.bb,
							      &dwarf_regno)))
				goto out;
			if ((regno = dwarf_regno_to_internal(dwarf_regno)) ==
			    DRGN_REGISTER_NUMBER_UNKNOWN)
				break;
			goto set_reg;
		case DW_CFA_same_value:
			rule.kind = DRGN_CFI_RULE_REGISTER_PLUS_OFFSET;
			rule.offset = 0;
			if ((err = binary_buffer_next_uleb128(&buffer.bb,
							      &dwarf_regno)))
				goto out;
			if ((regno = dwarf_regno_to_internal(dwarf_regno)) ==
			    DRGN_REGISTER_NUMBER_UNKNOWN)
				break;
			rule.regno = regno;
			goto set_reg;
		case DW_CFA_offset:
			rule.kind = DRGN_CFI_RULE_AT_CFA_PLUS_OFFSET;
			if ((err = drgn_dwarf_cfi_next_offset_f(&buffer, cie,
								&rule.offset)))
				goto out;
			if ((regno = dwarf_regno_to_internal(opcode & 0x3f)) ==
			    DRGN_REGISTER_NUMBER_UNKNOWN)
				break;
			goto set_reg;
		case DW_CFA_offset_extended:
			rule.kind = DRGN_CFI_RULE_AT_CFA_PLUS_OFFSET;
			goto reg_offset_f;
		case DW_CFA_offset_extended_sf:
			rule.kind = DRGN_CFI_RULE_AT_CFA_PLUS_OFFSET;
			goto reg_offset_sf;
		case DW_CFA_val_offset:
			rule.kind = DRGN_CFI_RULE_CFA_PLUS_OFFSET;
reg_offset_f:
			if ((err = binary_buffer_next_uleb128(&buffer.bb,
							      &dwarf_regno)) ||
			    (err = drgn_dwarf_cfi_next_offset_f(&buffer, cie,
								&rule.offset)))
				goto out;
			if ((regno = dwarf_regno_to_internal(dwarf_regno)) ==
			    DRGN_REGISTER_NUMBER_UNKNOWN)
				break;
			goto set_reg;
		case DW_CFA_val_offset_sf:
			rule.kind = DRGN_CFI_RULE_CFA_PLUS_OFFSET;
reg_offset_sf:
			if ((err = binary_buffer_next_uleb128(&buffer.bb,
							      &dwarf_regno)) ||
			    (err = drgn_dwarf_cfi_next_offset_sf(&buffer, cie,
								 &rule.offset)))
				goto out;
			if ((regno = dwarf_regno_to_internal(dwarf_regno)) ==
			    DRGN_REGISTER_NUMBER_UNKNOWN)
				break;
			goto set_reg;
		case DW_CFA_register: {
			rule.kind = DRGN_CFI_RULE_REGISTER_PLUS_OFFSET;
			rule.offset = 0;
			uint64_t dwarf_regno2;
			if ((err = binary_buffer_next_uleb128(&buffer.bb,
							      &dwarf_regno)) ||
			    (err = binary_buffer_next_uleb128(&buffer.bb,
							      &dwarf_regno2)))
				goto out;
			if ((regno = dwarf_regno_to_internal(dwarf_regno)) ==
			    DRGN_REGISTER_NUMBER_UNKNOWN)
				break;
			if ((rule.regno = dwarf_regno_to_internal(dwarf_regno2)) ==
			    DRGN_REGISTER_NUMBER_UNKNOWN)
				rule.kind = DRGN_CFI_RULE_UNDEFINED;
			goto set_reg;
		}
		case DW_CFA_expression:
			rule.kind = DRGN_CFI_RULE_AT_DWARF_EXPRESSION;
			goto reg_expression;
		case DW_CFA_val_expression:
			rule.kind = DRGN_CFI_RULE_DWARF_EXPRESSION;
reg_expression:
			rule.push_cfa = true;
			if ((err = binary_buffer_next_uleb128(&buffer.bb,
							      &dwarf_regno)) ||
			    (err = drgn_dwarf_cfi_next_block(&buffer,
							     &rule.expr,
							     &rule.expr_size)))
				goto out;
			if ((regno = dwarf_regno_to_internal(dwarf_regno)) ==
			    DRGN_REGISTER_NUMBER_UNKNOWN)
				break;
			goto set_reg;
		case DW_CFA_restore:
			if (!initial_row)
				goto invalid_for_initial;
			dwarf_regno = opcode & 0x3f;
			goto restore;
		case DW_CFA_restore_extended:
			if (!initial_row) {
invalid_for_initial:
				err = binary_buffer_error(&buffer.bb,
							  "invalid initial DWARF CFI opcode %#" PRIx8,
							  opcode);
				goto out;
			}
			if ((err = binary_buffer_next_uleb128(&buffer.bb,
							      &dwarf_regno)))
				goto out;
restore:
			if ((regno = dwarf_regno_to_internal(dwarf_regno)) ==
			    DRGN_REGISTER_NUMBER_UNKNOWN)
				break;
			drgn_cfi_row_get_register(initial_row, regno, &rule);
set_reg:
			if (!drgn_cfi_row_set_register(row, regno, &rule)) {
				err = &drgn_enomem;
				goto out;
			}
			break;
		case DW_CFA_remember_state: {
			struct drgn_cfi_row **state =
				drgn_cfi_row_vector_append_entry(&state_stack);
			if (!state) {
				err = &drgn_enomem;
				goto out;
			}
			*state = drgn_empty_cfi_row;
			if (!drgn_cfi_row_copy(state, *row)) {
				err = &drgn_enomem;
				goto out;
			}
			break;
		}
		case DW_CFA_restore_state:
			if (state_stack.size == 0) {
				err = binary_buffer_error(&buffer.bb,
							  "DW_CFA_restore_state with empty state stack");
				goto out;
			}
			drgn_cfi_row_destroy(*row);
			*row = state_stack.data[--state_stack.size];
			break;
		case DW_CFA_nop:
			break;
		default:
			err = binary_buffer_error(&buffer.bb,
						  "unknown DWARF CFI opcode %#" PRIx8,
						  opcode);
			goto out;
		}
	}
found:
	err = NULL;
out:
	for (size_t i = 0; i < state_stack.size; i++)
		drgn_cfi_row_destroy(state_stack.data[i]);
	drgn_cfi_row_vector_deinit(&state_stack);
	return err;
}

static struct drgn_error *
drgn_debug_info_find_cfi_in_fde(struct drgn_debug_info_module *module,
				struct drgn_dwarf_fde *fde,
				uint64_t unbiased_pc, struct drgn_cfi_row **ret)
{
	struct drgn_error *err;
	struct drgn_dwarf_cie *cie = &module->dwarf.cies[fde->cie];
	struct drgn_cfi_row *initial_row =
		(struct drgn_cfi_row *)module->platform.arch->default_dwarf_cfi_row;
	err = drgn_eval_dwarf_cfi(module, fde, NULL, unbiased_pc,
				  cie->initial_instructions,
				  cie->initial_instructions_size, &initial_row);
	if (err)
		goto out;
	if (!drgn_cfi_row_copy(ret, initial_row)) {
		err = &drgn_enomem;
		goto out;
	}
	err = drgn_eval_dwarf_cfi(module, fde, initial_row, unbiased_pc,
				  fde->instructions, fde->instructions_size,
				  ret);
out:
	drgn_cfi_row_destroy(initial_row);
	return err;
}

struct drgn_error *
drgn_debug_info_find_dwarf_cfi(struct drgn_debug_info_module *module,
			       uint64_t unbiased_pc,
			       struct drgn_cfi_row **row_ret,
			       bool *interrupted_ret,
			       drgn_register_number *ret_addr_regno_ret)
{
	struct drgn_error *err;
	struct drgn_dwarf_fde *fde;
	err = drgn_debug_info_find_fde(module, unbiased_pc, &fde);
	if (err)
		return err;
	if (!fde)
		return &drgn_not_found;
	err = drgn_debug_info_find_cfi_in_fde(module, fde, unbiased_pc,
					      row_ret);
	if (err)
		return err;
	*interrupted_ret = module->dwarf.cies[fde->cie].signal_frame;
	*ret_addr_regno_ret =
		module->dwarf.cies[fde->cie].return_address_register;
	return NULL;
}

struct drgn_error *
drgn_eval_cfi_dwarf_expression(struct drgn_program *prog,
			       const struct drgn_cfi_rule *rule,
			       const struct drgn_register_state *regs,
			       void *buf, size_t size)
{
	struct drgn_error *err;
	struct uint64_vector stack = VECTOR_INIT;

	if (rule->push_cfa) {
		struct optional_uint64 cfa = drgn_register_state_get_cfa(regs);
		if (!cfa.has_value) {
			err = &drgn_not_found;
			goto out;
		}
		if (!uint64_vector_append(&stack, &cfa.value)) {
			err = &drgn_enomem;
			goto out;
		}
	}

	int remaining_ops = MAX_DWARF_EXPR_OPS;
	struct drgn_dwarf_expression_context ctx;
	drgn_dwarf_expression_context_init(&ctx, prog, regs->module, NULL, NULL,
					   regs, rule->expr, rule->expr_size);
	err = drgn_eval_dwarf_expression(&ctx, &stack, &remaining_ops);
	if (err)
		goto out;
	if (binary_buffer_has_next(&ctx.bb)) {
		uint8_t opcode;
		err = binary_buffer_next_u8(&ctx.bb, &opcode);
		if (!err) {
			err = binary_buffer_error(&ctx.bb,
						  "invalid opcode %#" PRIx8 " for CFI expression",
						  opcode);
		}
		goto out;
	}
	if (stack.size == 0) {
		err = &drgn_not_found;
	} else if (rule->kind == DRGN_CFI_RULE_AT_DWARF_EXPRESSION) {
		err = drgn_program_read_memory(prog, buf,
					       stack.data[stack.size - 1], size,
					       false);
	} else {
		copy_lsbytes(buf, size,
			     drgn_platform_is_little_endian(&prog->platform),
			     &stack.data[stack.size - 1], sizeof(uint64_t),
			     HOST_LITTLE_ENDIAN);
		err = NULL;
	}

out:
	uint64_vector_deinit(&stack);
	return err;
}
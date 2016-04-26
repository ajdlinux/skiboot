/* Copyright 2013-2014 IBM Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * 	http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <skiboot.h>
#include <stdarg.h>
#include <libfdt.h>
#include <device.h>
#include <cpu.h>
#include <opal.h>
#include <interrupts.h>
#include <fsp.h>
#include <cec.h>
#include <vpd.h>
#include <ccan/str/str.h>

static int fdt_error;

#undef DEBUG_FDT

static void __save_err(int err, const char *str)
{
#ifdef DEBUG_FDT
	printf("FDT: rc: %d from \"%s\"\n", err, str);
#endif
	if (err && !fdt_error) {
		prerror("FDT: Error %d from \"%s\"\n", err, str);
		fdt_error = err;
	}
}

#define save_err(...) __save_err(__VA_ARGS__, #__VA_ARGS__)

static void dt_property_cell(void *fdt, const char *name, u32 cell)
{
	save_err(fdt_property_cell(fdt, name, cell));
}

static void dt_begin_node(void *fdt, const struct dt_node *dn)
{
	save_err(fdt_begin_node(fdt, dn->name));

	/*
	 * We add both the new style "phandle" and the legacy
	 * "linux,phandle" properties
	 */
	dt_property_cell(fdt, "linux,phandle", dn->phandle);
	dt_property_cell(fdt, "phandle", dn->phandle);
}

static void dt_property(void *fdt, const struct dt_property *p)
{
	save_err(fdt_property(fdt, p->name, p->prop, p->len));
}

static void dt_end_node(void *fdt)
{
	save_err(fdt_end_node(fdt));
}

#ifdef DEBUG_FDT
static void dump_fdt(void *fdt)
{
	int i, off, depth, err;

	printf("Device tree %u@%p\n", fdt_totalsize(fdt), fdt);

	err = fdt_check_header(fdt);
	if (err) {
		prerror("fdt_check_header: %s\n", fdt_strerror(err));
		return;
	}
	printf("fdt_check_header passed\n");

	printf("fdt_num_mem_rsv = %u\n", fdt_num_mem_rsv(fdt));
	for (i = 0; i < fdt_num_mem_rsv(fdt); i++) {
		u64 addr, size;

		err = fdt_get_mem_rsv(fdt, i, &addr, &size);
		if (err) {
			printf(" ERR %s\n", fdt_strerror(err));
			return;
		}
		printf("  mem_rsv[%i] = %lu@%#lx\n", i, (long)addr, (long)size);
	}

	for (off = fdt_next_node(fdt, 0, &depth);
	     off > 0;
	     off = fdt_next_node(fdt, off, &depth)) {
		int len;
		const char *name;

		name = fdt_get_name(fdt, off, &len);
		if (!name) {
			prerror("fdt: offset %i no name!\n", off);
			return;
		}
		printf("name: %s [%u]\n", name, off);
	}
}
#endif

static void flatten_dt_properties(void *fdt, const struct dt_node *dn)
{
	const struct dt_property *p;

	list_for_each(&dn->properties, p, list) {
		if (strstarts(p->name, DT_PRIVATE))
			continue;
#ifdef DEBUG_FDT
		printf("FDT: prop: %s size: %ld\n", p->name, p->len);
#endif
		dt_property(fdt, p);
	}
}

static void flatten_dt_node(void *fdt,
			    const struct dt_node *root,
			    bool exclusive)
{
	const struct dt_node *i;

	if (!exclusive) {
#ifdef DEBUG_FDT
		printf("FDT: node: %s\n", root->name);
#endif
		dt_begin_node(fdt, root);
		flatten_dt_properties(fdt, root);
	}

	list_for_each(&root->children, i, list)
		flatten_dt_node(fdt, i, false);

	if (!exclusive)
		dt_end_node(fdt);
}

static void create_dtb_reservemap(void *fdt, const struct dt_node *root)
{
	uint64_t base, size;
	const uint64_t *ranges;
	const struct dt_property *prop;
	int i;

	/* Duplicate the reserved-ranges property into the fdt reservemap */
	prop = dt_find_property(root, "reserved-ranges");
	if (prop) {
		ranges = (const void *)prop->prop;

		for (i = 0; i < prop->len / (sizeof(uint64_t) * 2); i++) {
			base = *(ranges++);
			size = *(ranges++);
			save_err(fdt_add_reservemap_entry(fdt, base, size));
		}
	}

	save_err(fdt_finish_reservemap(fdt));
}

static int __create_dtb(void *fdt, size_t len,
			const struct dt_node *root,
			bool exclusive)
{
	uint32_t old_last_phandle = last_phandle;

	fdt_create(fdt, len);

	if (root == dt_root && !exclusive)
		create_dtb_reservemap(fdt, root);

	/* Unflatten our live tree */
	flatten_dt_node(fdt, root, exclusive);

	save_err(fdt_finish(fdt));
	if (fdt_error) {
		last_phandle = old_last_phandle;
		prerror("dtb: error %s\n", fdt_strerror(fdt_error));
		return fdt_error;
	}

#ifdef DEBUG_FDT
	dump_fdt(fdt);
#endif
	return 0;
}

static int64_t opal_get_device_tree(uint32_t phandle,
				    uint64_t buf,
				    uint64_t len)
{
	struct dt_node *root;
	void *fdt = (void *)buf;
	int ret;

	if (!fdt || !len)
		return OPAL_PARAMETER;

	root = dt_find_by_phandle(dt_root, phandle);
	if (!root)
		return OPAL_PARAMETER;

	ret = __create_dtb(fdt, len, root, true);
	if (ret == -FDT_ERR_NOSPACE)
		return OPAL_NO_MEM;
	else if (ret)
		return OPAL_EMPTY;

	return OPAL_SUCCESS;
}
opal_call(OPAL_GET_DEVICE_TREE, opal_get_device_tree, 3);

void *create_dtb(const struct dt_node *root)
{
	void *fdt = NULL;
	size_t len = DEVICE_TREE_MAX_SIZE;
	int ret;

	do {
		fdt = malloc(len);
		if (!fdt) {
			prerror("dtb: cannot allocate FDT blob (%lu bytes)\n",
				(long)len);
			break;
		}

		ret = __create_dtb(fdt, len, root, false);
		if (ret) {
			free(fdt);
			fdt = NULL;
		}

		len *= 2;
	} while (ret == -FDT_ERR_NOSPACE);

	return fdt;
}

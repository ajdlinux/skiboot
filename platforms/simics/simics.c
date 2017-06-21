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
#include <device.h>
#include <lpc.h>
#include <console.h>
#include <opal.h>
#include <psi.h>
#include <bt.h>
#include <platforms/astbmc/astbmc.h>

bool simics = false;

static const struct slot_table_entry simics_phb0_0_slot[] = {
	{
		.etype = st_pluggable_slot,
		.location = ST_LOC_DEVFN(0,0),
		.name = "GPU0",
	},
	{ .etype = st_end },
};

static const struct slot_table_entry simics_phb0_1_slot[] = {
	{
		.etype = st_pluggable_slot,
		.location = ST_LOC_DEVFN(0,0),
		.name = "Slot2",
	},
	{ .etype = st_end },
};

static const struct slot_table_entry simics_npu0_slots[] = {
	{
		.etype = st_npu_slot,
		.location = ST_LOC_NPU_GROUP(0),
		.name = "GPU0",
	},
	{
		.etype = st_npu_slot,
		.location = ST_LOC_NPU_GROUP(1),
		.name = "NO_GPU",
	},
	{ .etype = st_end },
};

static const struct slot_table_entry simics_phb_table[] = {
	{
		.etype = st_phb,
		.location = ST_LOC_PHB(0,0),
		.children = simics_phb0_0_slot,
	},
	{
		.etype = st_phb,
		.location = ST_LOC_PHB(0,1),
		.children = simics_phb0_1_slot,
	},
	{
		.etype = st_phb,
		.location = ST_LOC_PHB(0,2),
		.children = simics_npu0_slots,
	},
};

#define NPU_BASE 0x5011000 // TODO check
#define NPU_SIZE 0x2c // TODO check
#define NPU_INDIRECT0	0x8000000009010c3f
#define NPU_INDIRECT1	0x800000000c010c3f

static void create_link(struct dt_node *npu, int group, int index, bool opencapi)
{
	struct dt_node *link;
	uint32_t lane_mask;
	uint64_t phy;
	char namebuf[32];

	snprintf(namebuf, sizeof(namebuf), "link@%x", index);
	link = dt_new(npu, namebuf);

	dt_add_property_string(link, "compatible", "ibm,npu-link");
	if (opencapi)
		dt_add_property_string(link, "ibm,npu-link-type", "opencapi");
	else
		dt_add_property_string(link, "ibm,npu-link-type", "nvlink");

	dt_add_property_cells(link, "ibm,npu-link-index", index);

	switch (index) {
	case 2:
	case 3:
		phy = NPU_INDIRECT0; /* OB0 */
		break;
	case 4:
	case 5:
		phy = NPU_INDIRECT1; /* OB3 */
		break;
	default:
		assert(false);
	}

	// TODO: a) figure out how this corresponds to fabric workbook, looks wrong
	// b) figure out correct method for OCAPI
	switch (index % 3) {
	case 0: /* NTL0.0 / NTL1.1 */
		lane_mask = 0xf1e000; /* 13-16 20-23 */
		break;

	case 1: /* NTL0.1 / NTL2.0 */
		lane_mask = 0x0e1870; /* 4-6 11-12 17-19 */
		break;

	case 2: /* NTL1.0 / NTL2.1 */
		lane_mask = 0x00078f; /* 0-3 7-10 */
		break;

	default:
		assert(0);
	}

	dt_add_property_u64s(link, "ibm,npu-phy", phy);
	dt_add_property_cells(link, "ibm,npu-lane-mask", lane_mask);
	dt_add_property_cells(link, "ibm,npu-group-id", group);
}

static void simics_create_npu(void)
{
	struct dt_node *xscom, *npu;
	char namebuf[32];
	prlog(PR_INFO, "OCAPI: Simics detected, adding NPU device nodes\n");
	dt_for_each_compatible(dt_root, xscom, "ibm,xscom") {
		snprintf(namebuf, sizeof(namebuf), "npu@%x", NPU_BASE);
		npu = dt_new(xscom, namebuf);
		dt_add_property_cells(npu, "reg", NPU_BASE, NPU_SIZE);
		dt_add_property_strings(npu, "compatible", "ibm,power9-npu");

		dt_add_property_cells(npu, "ibm,phb-index", 4); // arbitrary, see garrison.c
		dt_add_property_cells(npu, "ibm,npu-index", 0); // TODO
		dt_add_property_cells(npu, "ibm,npu-links", 1);

		create_link(npu, 1, 2, true);
		/* Create an NVLink link just for testing */
		create_link(npu, 2, 4, false);
	}
}

static void simics_init(void)
{
	if (uart_enabled())
		set_opal_console(&uart_opal_con);
	bt_init();
	fake_rtc_init();
	simics_create_npu();
}

static bool simics_probe(void)
{
	if (!dt_find_by_path(dt_root, "/simics"))
		return false;

	simics = true;
	uart_init();

	slot_table_init(simics_phb_table);

	return true;
}

DECLARE_PLATFORM(simics) = {
	.name			= "Simics",
	.probe			= simics_probe,
	.init			= simics_init,
	.pci_get_slot_info	= slot_table_get_slot_info,
};

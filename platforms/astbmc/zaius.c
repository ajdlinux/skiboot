/* Copyright 2017 IBM Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
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
#include <console.h>
#include <chip.h>
#include <ipmi.h>
#include <psi.h>
#include <npu-regs.h>

#include "astbmc.h"

#define NPU_BASE 0x5011000
#define NPU_SIZE 0x2c
#define NPU_INDIRECT0	0x8000000009010c3f
#define NPU_INDIRECT1	0x800000000c010c3f

/* FIXME: Hack until we get HDAT-derived NPU nodes */
#define NPU_BASE 0x5011000 // TODO check
#define NPU_SIZE 0x2c // TODO check
#define NPU_INDIRECT0	0x8000000009010c3f
#define NPU_INDIRECT1	0x800000000c010c3f

static void create_link(struct dt_node *npu, int group, int index)
{
	struct dt_node *link;
	uint32_t lane_mask;
	uint64_t phy;
	char namebuf[32];

	snprintf(namebuf, sizeof(namebuf), "link@%x", index);
	link = dt_new(npu, namebuf);

	dt_add_property_string(link, "compatible", "ibm,npu-link");
	dt_add_property_string(link, "ibm,npu-link-type", "opencapi");
	
	dt_add_property_cells(link, "ibm,npu-link-index", index);

	/* Wrong for nvlink */
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

	/* Wrong for nvlink */
	switch (index) {
	case 2:
		lane_mask = 0x00078f;
		break;
	case 3: /* WIP so wrong */
		//lane_mask = 0x0e1870;
		lane_mask = 0xf1e000;
		break;

	default:
		assert(0);
	}

	dt_add_property_u64s(link, "ibm,npu-phy", phy);
	dt_add_property_cells(link, "ibm,npu-lane-mask", lane_mask);
	dt_add_property_cells(link, "ibm,npu-group-id", group);
}

static void zaius_create_npu(void)
{
	struct dt_node *xscom, *npu;
	int phb_index = 7;
	int npu_index = 0;
	char namebuf[32];
	prlog(PR_INFO, "OCAPI: Adding NPU device nodes\n");
	dt_for_each_compatible(dt_root, xscom, "ibm,xscom") {
		snprintf(namebuf, sizeof(namebuf), "npu@%x", NPU_BASE);
		npu = dt_new(xscom, namebuf);
		dt_add_property_cells(npu, "reg", NPU_BASE, NPU_SIZE);
		dt_add_property_strings(npu, "compatible", "ibm,power9-npu");

		dt_add_property_cells(npu, "ibm,phb-index", phb_index++);
		dt_add_property_cells(npu, "ibm,npu-index", npu_index++);
		//dt_add_property_cells(npu, "ibm,npu-links", 2);
		dt_add_property_cells(npu, "ibm,npu-links", 1);
		//create_link(npu, 1, 2);
		create_link(npu, 1, 3);
	}
}

static void zaius_create_ocapi_i2c_bus(void)
{
	struct dt_node *xscom, *i2cm, *i2c_bus;
	prlog(PR_INFO, "OCAPI: Faking e1p4 I2C bus\n");
	dt_for_each_compatible(dt_root, xscom, "ibm,xscom") {
		i2cm = dt_find_by_name(xscom, "i2cm@a1000");
		if (!i2cm) {
			prlog(PR_INFO, "OCAPI: hack failed\n");
			continue;
		}
		i2c_bus = dt_new_addr(i2cm, "i2c-bus", 4);
		dt_add_property_cells(i2c_bus, "reg", 4);
		dt_add_property_cells(i2c_bus, "bus-frequency", 0x61a80);
		dt_add_property_strings(i2c_bus, "compatible",
					"ibm,opal-i2c", "ibm,power8-i2c-port",
					"ibm,power9-i2c-port");
	}
}

static bool zaius_probe(void)
{
	if (!dt_node_is_compatible(dt_root, "ingrasys,zaius"))
		return false;

	/* Lot of common early inits here */
	astbmc_early_init();

	/* Setup UART for direct use by Linux */
	uart_set_console_policy(UART_CONSOLE_OS);

	zaius_create_npu();
	zaius_create_ocapi_i2c_bus();

	return true;
}

DECLARE_PLATFORM(zaius_platform) = {
	.name			= "Zaius",
	.probe			= zaius_probe,
	.init			= astbmc_init,
	.start_preload_resource	= flash_start_preload_resource,
	.resource_loaded	= flash_resource_loaded,
	.bmc			= NULL, /* FIXME: Add openBMC */
	.pci_get_slot_info	= slot_table_get_slot_info,
	.pci_probe_complete	= check_all_slot_table,
	.cec_power_down         = astbmc_ipmi_power_down,
	.cec_reboot             = astbmc_ipmi_reboot,
	.elog_commit		= ipmi_elog_commit,
	.exit			= ipmi_wdt_final_reset,
	.terminate		= ipmi_terminate,
};

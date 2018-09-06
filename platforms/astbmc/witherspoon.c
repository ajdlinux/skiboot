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
#include <xscom.h>
#include <xscom-p9-regs.h>
#include <timebase.h>
#include <pci.h>
#include <pci-slot.h>
#include <phb4.h>
#include <npu2.h>
#include <occ.h>
#include <i2c.h>

#include "astbmc.h"
#include "ast.h"

static enum {
	WITHERSPOON_TYPE_UNKNOWN,
	WITHERSPOON_TYPE_SEQUOIA,
	WITHERSPOON_TYPE_REDBUD
} witherspoon_type;

/*
 * HACK: Hostboot doesn't export the correct data for the system VPD EEPROM
 *       for this system. So we need to work around it here.
 */
static void vpd_dt_fixup(void)
{
	struct dt_node *n = dt_find_by_path(dt_root,
		"/xscom@603fc00000000/i2cm@a2000/i2c-bus@0/eeprom@50");

	if (n) {
		dt_check_del_prop(n, "compatible");
		dt_add_property_string(n, "compatible", "atmel,24c512");

		dt_check_del_prop(n, "label");
		dt_add_property_string(n, "label", "system-vpd");
	}
}

static void witherspoon_create_ocapi_i2c_bus(void)
{
	struct dt_node *xscom, *i2cm, *i2c_bus;
	prlog(PR_DEBUG, "OCAPI: Adding I2C bus device node for OCAPI reset\n");
	dt_for_each_compatible(dt_root, xscom, "ibm,xscom") {
		i2cm = dt_find_by_name(xscom, "i2cm@a1000");
		if (!i2cm) {
			prlog(PR_ERR, "OCAPI: Failed to add I2C bus device node\n");
			continue;
		}

		if (dt_find_by_name(i2cm, "i2c-bus@4"))
			continue;

		i2c_bus = dt_new_addr(i2cm, "i2c-bus", 4);
		dt_add_property_cells(i2c_bus, "reg", 4);
		dt_add_property_cells(i2c_bus, "bus-frequency", 0x61a80);
		dt_add_property_strings(i2c_bus, "compatible",
					"ibm,opal-i2c", "ibm,power8-i2c-port",
					"ibm,power9-i2c-port");
	}
}

static bool witherspoon_probe(void)
{
	struct dt_node *np;
	int highest_gpu_group_id = 0;
	int gpu_group_id;

	if (!dt_node_is_compatible(dt_root, "ibm,witherspoon"))
		return false;

	/* Lot of common early inits here */
	astbmc_early_init();

	/* Setup UART for use by OPAL (Linux hvc) */
	uart_set_console_policy(UART_CONSOLE_OPAL);

	vpd_dt_fixup();

	witherspoon_create_ocapi_i2c_bus();

	dt_for_each_compatible(dt_root, np, "ibm,npu-link") {
		gpu_group_id = dt_prop_get_u32(np, "ibm,npu-group-id");
		if (gpu_group_id > highest_gpu_group_id)
			highest_gpu_group_id = gpu_group_id;
	};

	switch (highest_gpu_group_id) {
	case 1:
		witherspoon_type = WITHERSPOON_TYPE_REDBUD;
		break;
	case 2:
		witherspoon_type = WITHERSPOON_TYPE_SEQUOIA;
		break;
	default:
		witherspoon_type = WITHERSPOON_TYPE_UNKNOWN;
		prlog(PR_NOTICE, "PLAT: Unknown Witherspoon variant detected\n");
	}

	return true;
}

static void phb4_activate_shared_slot_witherspoon(struct proc_chip *chip)
{
	uint64_t val;

	/*
	 * Shared slot activation is done by raising a GPIO line on the
	 * chip with the secondary slot. It will somehow activate the
	 * sideband signals between the slots.
	 * Need to wait 100us for stability.
	 */
	xscom_read(chip->id, P9_GPIO_DATA_OUT_ENABLE, &val);
	val |= PPC_BIT(2);
	xscom_write(chip->id, P9_GPIO_DATA_OUT_ENABLE, val);

	xscom_read(chip->id, P9_GPIO_DATA_OUT, &val);
	val |= PPC_BIT(2);
	xscom_write(chip->id, P9_GPIO_DATA_OUT, val);
	time_wait_us(100);
	prlog(PR_INFO, "Shared PCI slot activated\n");
}

static void phb4_pre_pci_fixup_witherspoon(void)
{
	struct pci_slot *slot0, *slot1;
	struct proc_chip *chip0, *chip1;
	uint8_t p0 = 0, p1 = 0;

	/*
	 * Detect if a x16 card is present on the shared slot and
	 * do some extra configuration if it is.
	 *
	 * The shared slot, a.k.a "Slot 2" in the documentation, is
	 * connected to PEC2 phb index 3 on both chips. From skiboot,
	 * it looks like two x8 slots, each with its own presence bit.
	 *
	 * Here is the matrix of possibilities for the presence bits:
	 *
	 * slot0 presence     slot1 presence
	 *    0                  0               => no card
	 *    1                  0               => x8 or less card detected
	 *    1                  1               => x16 card detected
	 *    0                  1               => invalid combination
	 *
	 * We only act if a x16 card is detected ('1 1' combination above).
	 *
	 * One issue is that we don't really know if it is a
	 * shared-slot-compatible card (such as Mellanox CX5) or
	 * a 'normal' x16 PCI card. We activate the shared slot in both cases,
	 * as it doesn't seem to hurt.
	 *
	 * If the card is a normal x16 PCI card, the link won't train on the
	 * second slot (nothing to do with the shared slot activation), the
	 * procedure will timeout, thus adding some delay to the boot time.
	 * Therefore the recommendation is that we shouldn't use a normal
	 * x16 card on the shared slot of a witherspoon.
	 *
	 * Plugging a x8 or less adapter on the shared slot should work
	 * like any other physical slot.
	 */
	chip0 = next_chip(NULL);
	chip1 = next_chip(chip0);
	if (!chip1 || next_chip(chip1)) {
		prlog(PR_WARNING,
			"Unexpected number of chips, skipping shared slot detection\n");
		return;
	}

	/* the shared slot is connected to PHB3 on both chips */
	slot0 = pci_slot_find(phb4_get_opal_id(chip0->id, 3));
	slot1 = pci_slot_find(phb4_get_opal_id(chip1->id, 3));
	if (slot0 && slot1) {
		if (slot0->ops.get_presence_state)
			slot0->ops.get_presence_state(slot0, &p0);
		if (slot1->ops.get_presence_state)
			slot1->ops.get_presence_state(slot1, &p1);
		if (p0 == 1 && p1 == 1) {
			phb4_activate_shared_slot_witherspoon(chip1);
			slot0->peer_slot = slot1;
			slot1->peer_slot = slot0;
		}
	}
}

static void witherspoon_pre_pci_fixup(void)
{
	phb4_pre_pci_fixup_witherspoon();
}

static void set_link_details(struct npu2 *npu, uint32_t link_index,
			     uint32_t brick_index, enum npu2_dev_type type)
{
	struct npu2_dev *dev = NULL;
	for (int i = 0; i < npu->total_devices; i++) {
		if (npu->devices[i].link_index == link_index) {
			dev = &npu->devices[i];
			break;
		}
	}
	if (!dev) {
		prlog(PR_ERR, "PLAT: Could not find NPU link index %d\n",
		      link_index);
		return;
	}
	dev->brick_index = brick_index;
	dev->type = type;
}

static void witherspoon_npu2_device_detect(struct npu2 *npu)
{
	struct proc_chip *chip;
	uint8_t state;
	uint64_t i2c_port_id = 0;
	char port_name[17];
	struct dt_node *dn;
	int rc;

	enum npu2_dev_type gpu0_type = NPU2_DEV_TYPE_UNKNOWN;
	enum npu2_dev_type gpu1_type = NPU2_DEV_TYPE_UNKNOWN;

	if (witherspoon_type != WITHERSPOON_TYPE_REDBUD) {
		prlog(PR_DEBUG, "PLAT: Setting all NPU links to NVLink, OpenCAPI only supported on Redbud\n");
	        for (int i = 0; i < npu->total_devices; i++) {
			npu->devices[i].type = NPU2_DEV_TYPE_NVLINK;
		}
		return;
	}
	assert(npu->total_devices == 6);

	chip = get_chip(npu->chip_id);

	/* Find I2C port */
	snprintf(port_name, sizeof(port_name), "p8_%08x_e%dp%d",
		 chip->id, platform.ocapi->i2c_engine,
		 platform.ocapi->i2c_port);
	dt_for_each_compatible(dt_root, dn, "ibm,power9-i2c-port") {
		if (streq(port_name, dt_prop_get(dn, "ibm,port-name"))) {
			i2c_port_id = dt_prop_get_u32(dn, "ibm,opal-id");
			break;
		}
	}

	if (!i2c_port_id) {
		prlog(PR_ERR, "PLAT: Could not find NPU presence I2C port\n");
		return;
	}

	/* Set pins to input */
	state = 0xff;
	rc = i2c_request_send(i2c_port_id,
			      platform.ocapi->i2c_presence_addr, SMBUS_WRITE, 3,
			      1, &state, 1, 120);
	if (rc)
		goto i2c_failed;

	/* Read the presence value */
	state = 0x00;
	rc = i2c_request_send(i2c_port_id,
			      platform.ocapi->i2c_presence_addr, SMBUS_READ, 0,
			      1, &state, 1, 120);
	if (rc)
		goto i2c_failed;

	if (occ_get_gpu_presence(chip, 0)) {
		prlog(PR_DEBUG, "PLAT: Chip %d GPU#0 slot present\n", chip->id);
		if (state & (1 << 0)) {
			prlog(PR_DEBUG, "PLAT: Chip %d GPU#0 is OpenCAPI\n",
			      chip->id);
			gpu0_type = NPU2_DEV_TYPE_OPENCAPI;
		} else {
			prlog(PR_DEBUG, "PLAT: Chip %d GPU#0 is NVLink\n",
			      chip->id);
			gpu0_type = NPU2_DEV_TYPE_NVLINK;
		}
	}

	if (occ_get_gpu_presence(chip, 1)) {
		prlog(PR_DEBUG, "PLAT: Chip %d GPU#1 slot present\n", chip->id);
		if (state & (1 << 1)) {
			prlog(PR_DEBUG, "PLAT: Chip %d GPU#1 is OpenCAPI\n",
			      chip->id);
			gpu1_type = NPU2_DEV_TYPE_OPENCAPI;
		} else {
			prlog(PR_DEBUG, "PLAT: Chip %d GPU#1 is NVLink\n",
			      chip->id);
			gpu1_type = NPU2_DEV_TYPE_NVLINK;
		}
	}

	if (gpu0_type == NPU2_DEV_TYPE_OPENCAPI) {
		/*
		 * On witherspoon, bricks 2 and 3 are connected to
		 * the lanes matching links 1 and 0 in OpenCAPI mode.
		 */
		set_link_details(npu, 0, 3, NPU2_DEV_TYPE_OPENCAPI);
		/* We current don't support using the second link */
		set_link_details(npu, 1, 2, NPU2_DEV_TYPE_UNKNOWN);
	}

	if (gpu0_type == NPU2_DEV_TYPE_NVLINK) {
		set_link_details(npu, 0, 0, NPU2_DEV_TYPE_NVLINK);
		set_link_details(npu, 1, 1, NPU2_DEV_TYPE_NVLINK);
		set_link_details(npu, 2, 2, NPU2_DEV_TYPE_NVLINK);
	}

	if (gpu1_type == NPU2_DEV_TYPE_OPENCAPI) {
		set_link_details(npu, 4, 4, NPU2_DEV_TYPE_OPENCAPI);
		/* We current don't support using the second link */
		set_link_details(npu, 5, 5, NPU2_DEV_TYPE_UNKNOWN);
	}

	if (gpu1_type == NPU2_DEV_TYPE_NVLINK) {
		if (gpu0_type == NPU2_DEV_TYPE_OPENCAPI) {
			prlog(PR_WARNING, "PLAT: Chip %d GPU#1 will operate at reduced performance due to presence of OpenCAPI device. For optimal performance, swap device locations\n", chip->id);
		} else {
			set_link_details(npu, 3, 3, NPU2_DEV_TYPE_NVLINK);
		}
		set_link_details(npu, 4, 4, NPU2_DEV_TYPE_NVLINK);
		set_link_details(npu, 5, 5, NPU2_DEV_TYPE_NVLINK);
	}

	return;

i2c_failed:
	prlog(PR_ERR, "PLAT: NPU device type detection failed, rc=%d\n", rc);
	return;
}

const struct platform_ocapi witherspoon_ocapi = {
       .i2c_engine          = 1,
       .i2c_port            = 4,
       .odl_phy_swap        = false,
       .i2c_reset_addr      = 0x20,
       /*
	* Witherspoon uses SXM2 connectors, carrying 2 OCAPI links
	* over a single connector - hence each pair of bricks shares
	* the same pin for resets. We currently only support using
	* bricks 3 and 4, among other reasons because we can't handle
	* a reset on one link causing the other link to reset as
	* well.
	*/
       .i2c_reset_brick2    = 1 << 0,
       .i2c_reset_brick3    = 1 << 0,
       .i2c_reset_brick4    = 1 << 1,
       .i2c_reset_brick5    = 1 << 1,
       .i2c_presence_addr   = 0x20,
       /* unused, we do this in custom presence detect */
       .i2c_presence_brick2 = 0,
       .i2c_presence_brick3 = 0,
       .i2c_presence_brick4 = 0,
       .i2c_presence_brick5 = 0,
};

/* The only difference between these is the PCI slot handling */

DECLARE_PLATFORM(witherspoon) = {
	.name			= "Witherspoon",
	.probe			= witherspoon_probe,
	.init			= astbmc_init,
	.pre_pci_fixup		= witherspoon_pre_pci_fixup,
	.start_preload_resource	= flash_start_preload_resource,
	.resource_loaded	= flash_resource_loaded,
	.bmc			= &astbmc_openbmc,
	.cec_power_down         = astbmc_ipmi_power_down,
	.cec_reboot             = astbmc_ipmi_reboot,
	.elog_commit		= ipmi_elog_commit,
	.exit			= astbmc_exit,
	.terminate		= ipmi_terminate,

	.pci_get_slot_info	= dt_slot_get_slot_info,
	.ocapi                  = &witherspoon_ocapi,
	.npu2_device_detect	= witherspoon_npu2_device_detect,
};

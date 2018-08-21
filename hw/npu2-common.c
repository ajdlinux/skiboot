/* Copyright 2013-2018 IBM Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <skiboot.h>
#include <xscom.h>
#include <pci.h>
#include <npu2.h>
#include <npu2-regs.h>
#include <bitutils.h>
#include <nvram.h>
#include <i2c.h>

/*
 * We use the indirect method because it uses the same addresses as
 * the MMIO offsets (NPU RING)
 */
static void npu2_scom_set_addr(uint64_t gcid, uint64_t scom_base,
			       uint64_t addr, uint64_t size)
{
	addr = SETFIELD(NPU2_MISC_DA_ADDR, 0ull, addr);
	addr = SETFIELD(NPU2_MISC_DA_LEN, addr, size);
	xscom_write(gcid, scom_base + NPU2_MISC_SCOM_IND_SCOM_ADDR, addr);
}

void npu2_scom_write(uint64_t gcid, uint64_t scom_base,
		     uint64_t reg, uint64_t size,
		     uint64_t val)
{
	npu2_scom_set_addr(gcid, scom_base, reg, size);
	xscom_write(gcid, scom_base + NPU2_MISC_SCOM_IND_SCOM_DATA, val);
}

uint64_t npu2_scom_read(uint64_t gcid, uint64_t scom_base,
			uint64_t reg, uint64_t size)
{
	uint64_t val;

	npu2_scom_set_addr(gcid, scom_base, reg, size);
	xscom_read(gcid, scom_base + NPU2_MISC_SCOM_IND_SCOM_DATA, &val);

	return val;
}

void npu2_write_4b(struct npu2 *p, uint64_t reg, uint32_t val)
{
	npu2_scom_write(p->chip_id, p->xscom_base, reg, NPU2_MISC_DA_LEN_4B,
			(uint64_t)val << 32);
}

uint32_t npu2_read_4b(struct npu2 *p, uint64_t reg)
{
	return npu2_scom_read(p->chip_id, p->xscom_base, reg,
			      NPU2_MISC_DA_LEN_4B) >> 32;
}

void npu2_write(struct npu2 *p, uint64_t reg, uint64_t val)
{
	npu2_scom_write(p->chip_id, p->xscom_base, reg, NPU2_MISC_DA_LEN_8B, val);
}

uint64_t npu2_read(struct npu2 *p, uint64_t reg)
{
	return npu2_scom_read(p->chip_id, p->xscom_base, reg, NPU2_MISC_DA_LEN_8B);
}

void npu2_write_mask(struct npu2 *p, uint64_t reg, uint64_t val, uint64_t mask)
{
	uint64_t new_val;

	new_val = npu2_read(p, reg);
	new_val &= ~mask;
	new_val |= val & mask;
	npu2_scom_write(p->chip_id, p->xscom_base, reg, NPU2_MISC_DA_LEN_8B, new_val);
}

void npu2_write_mask_4b(struct npu2 *p, uint64_t reg, uint32_t val, uint32_t mask)
{
	uint32_t new_val;

	new_val = npu2_read_4b(p, reg);
	new_val &= ~mask;
	new_val |= val & mask;
	npu2_scom_write(p->chip_id, p->xscom_base, reg, NPU2_MISC_DA_LEN_4B,
			(uint64_t)new_val << 32);
}

static bool _i2c_presence_detect(struct npu2_dev *dev)
{
	uint8_t state, data;
	int rc;

	rc = i2c_request_send(dev->npu->i2c_port_id_ocapi,
			platform.ocapi->i2c_presence_addr,
			SMBUS_READ, 0, 1,
			&state, 1, 120);
	if (rc) {
		OCAPIERR(dev, "error detecting link presence: %d\n", rc);
		return true; /* assume link exists */
	}

	OCAPIDBG(dev, "I2C presence detect: 0x%x\n", state);

	switch (dev->link_index) {
	case 2:
		data = platform.ocapi->i2c_presence_brick2;
		break;
	case 3:
		data = platform.ocapi->i2c_presence_brick3;
		break;
	case 4:
		data = platform.ocapi->i2c_presence_brick4;
		break;
	case 5:
		data = platform.ocapi->i2c_presence_brick5;
		break;
	default:
		OCAPIERR(dev, "presence detection on invalid link\n");
		return true;
	}
	/* Presence detect bits are active low */
	return !(state & data);
}

/*
 * A default presence detection implementation for platforms like ZZ and Zaius
 * that don't implement their own. Assumes all devices found will be OpenCAPI.
 */
void npu2_i2c_presence_detect(struct npu2 *npu)
{
	struct npu2_dev *dev;
	assert(platform.ocapi);
	for (int i = 0; i < npu->total_devices; i++) {
		dev = &npu->devices[i];
		if (platform.ocapi->force_presence ||
		    _i2c_presence_detect(dev))
			dev->type = NPU2_DEV_TYPE_OPENCAPI;
		else
			dev->type = NPU2_DEV_TYPE_UNKNOWN;
	}
}

/* Procedure 13.1.3.1, OpenCAPI NPU Workbook - Select OCAPI vs NVLink */
static void set_brick_config(struct npu2 *npu)
{
	/* Step 1 - Set Transport MUX controls to select correct OTL or NTL */
	uint64_t reg;
	uint64_t ndlmux_brk0to2, ocmux_brk0to1, ocmux_brk4to5;
	uint64_t phy_config_scom;
	struct npu2_dev *dev;

	prlog(PR_DEBUG, "NPU: %s: Setting transport mux controls\n", __func__);

	/* Optical IO Transport Mux Config for Bricks 0-2 and 4-5 */
	reg = npu2_scom_read(npu->chip_id, npu->xscom_base, NPU2_MISC_OPTICAL_IO_CFG0,
			     NPU2_MISC_DA_LEN_8B);
	ndlmux_brk0to2 = GETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_NDLMUX_BRK0TO2, reg);
	ocmux_brk0to1 = GETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_OCMUX_BRK0TO1, reg);
	ocmux_brk4to5 = GETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_OCMUX_BRK4TO5, reg);
	for (int i = 0; i < npu->total_devices; i++) {
	        dev = &npu->devices[i];
		if (dev->type == NPU2_DEV_TYPE_UNKNOWN)
			continue;

		switch (dev->brick_index) {
		case 0:  /* NTL0.0 */
			assert(dev->type == NPU2_DEV_TYPE_NVLINK);
			ndlmux_brk0to2 |= 0b100;
			break;
		case 1:  /* NTL0.1 */
			assert(dev->type == NPU2_DEV_TYPE_NVLINK);
			ndlmux_brk0to2 |= 0b010;
			break;
		case 2:	 /* NTL1.0 / OTL1.0 */
			if (dev->type == NPU2_DEV_TYPE_OPENCAPI) {
				ndlmux_brk0to2 &= ~0b100;
				ocmux_brk0to1 |= 0b10;
			} else {
				ndlmux_brk0to2 |= 0b001;
			}
			break;
		case 3:	 /* NTL1.1 / OTL1.1 */
			if (dev->type == NPU2_DEV_TYPE_OPENCAPI) {
				ndlmux_brk0to2 &= ~0b010;
				ocmux_brk0to1 |= 0b01;
			}
			break;
		case 4:	 /* NTL2.0 / OTL2.0 */
			if (dev->type == NPU2_DEV_TYPE_OPENCAPI) {
				ocmux_brk4to5 |= 0b10;
			} else {
				ocmux_brk4to5 &= ~0b10;
			}
			break;
		case 5:	 /* NTL2.1 / OTL2.1 */
			if (dev->type == NPU2_DEV_TYPE_OPENCAPI) {
				ocmux_brk4to5 |= 0b01;
			} else {
				ocmux_brk4to5 &= ~0b01;
			}
			break;
		default:
			assert(false);
		}
	}

	reg = SETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_NDLMUX_BRK0TO2, reg, ndlmux_brk0to2);
	reg = SETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_OCMUX_BRK0TO1, reg, ocmux_brk0to1);
	reg = SETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_OCMUX_BRK4TO5, reg, ocmux_brk4to5);
	npu2_scom_write(npu->chip_id, npu->xscom_base, NPU2_MISC_OPTICAL_IO_CFG0,
			NPU2_MISC_DA_LEN_8B, reg);

	/*
	 * PowerBus Optical Miscellaneous Config Register
	 */
	xscom_read(npu->chip_id, PU_IOE_PB_MISC_CFG, &reg);
	for (int i = 0; i < npu->total_devices; i++) {
		dev = &npu->devices[i];
		switch (dev->brick_index) {
		case 3:
			if (dev->type == NPU2_DEV_TYPE_NVLINK)
				reg = SETFIELD(PU_IOE_PB_MISC_CFG_SEL_03_NPU_NOT_PB, reg, 1);
			break;
		case 4:
			reg = SETFIELD(PU_IOE_PB_MISC_CFG_SEL_04_NPU_NOT_PB, reg, 1);
			break;
		case 5:
			reg = SETFIELD(PU_IOE_PB_MISC_CFG_SEL_05_NPU_NOT_PB, reg, 1);
			break;
		default:
			break;
		}
	}
	xscom_write(npu->chip_id, PU_IOE_PB_MISC_CFG, reg);


	/* Some... other step from the OpenCAPI workbook. TODO */

	for (int i = 0; i < npu->total_devices; i++) {
		dev = &npu->devices[i];
		if (dev->type != NPU2_DEV_TYPE_OPENCAPI)
			continue;
		switch (dev->brick_index) {
		case 2:
		case 3:
			phy_config_scom = OBUS_LL0_IOOL_PHY_CONFIG;
			break;
		case 4:
		case 5:
			phy_config_scom = OBUS_LL3_IOOL_PHY_CONFIG;
			break;
		default:
			assert(false);
		}
		/* Disable NV-Link link layers */
		xscom_read(npu->chip_id, phy_config_scom, &reg);
		reg &= ~OBUS_IOOL_PHY_CONFIG_NV0_NPU_ENABLED;
		reg &= ~OBUS_IOOL_PHY_CONFIG_NV1_NPU_ENABLED;
		reg &= ~OBUS_IOOL_PHY_CONFIG_NV2_NPU_ENABLED;
		xscom_write(npu->chip_id, phy_config_scom, reg);
	}

}

static struct npu2 *setup_npu(struct dt_node *dn)
{
	struct npu2 *npu;
	struct npu2_dev *dev;
	struct dt_node *np;
	uint32_t num_links;
	char port_name[17];
	void *npumem;
	char *path;
	int gcid;
	struct proc_chip *chip;
	int i = 0;

	/* Retrieve chip ID */
	path = dt_get_path(dn);
	gcid = dt_get_chip_id(dn);
	chip = get_chip(gcid);
	assert(chip);

	num_links = dt_prop_get_u32(dn, "ibm,npu-links");
	npumem = zalloc(sizeof(struct npu2) + num_links *
			sizeof(struct npu2_dev));
	assert(npumem);
	npu = npumem;

	npu->dt_node = dn;
	npu->index = dt_prop_get_u32(dn, "ibm,npu-index");
	npu->chip_id = gcid;
	npu->xscom_base = dt_get_address(dn, 0, NULL);
	npu->phb_index = dt_prop_get_u32(dn, "ibm,phb-index");

	if (platform.ocapi) {
		/* Find I2C port for handling device presence/reset */
		snprintf(port_name, sizeof(port_name), "p8_%08x_e%dp%d",
			 gcid, platform.ocapi->i2c_engine,
			 platform.ocapi->i2c_port);
		prlog(PR_DEBUG, "NPU: Looking for I2C port %s\n", port_name);

		dt_for_each_compatible(dt_root, np, "ibm,power9-i2c-port") {
			if (streq(port_name, dt_prop_get(np, "ibm,port-name"))) {
				npu->i2c_port_id_ocapi = dt_prop_get_u32(np, "ibm,opal-id");
				break;
			}
		}

		if (!npu->i2c_port_id_ocapi) {
			prlog(PR_ERR, "NPU: Couldn't find I2C port %s\n",
			      port_name);
			goto failed;
		}
	}

	npu->devices = npumem + sizeof(struct npu2);

	dt_for_each_compatible(dn, np, "ibm,npu-link") {
		assert(i < num_links);
		dev = &npu->devices[i];
		dev->link_index = dt_prop_get_u32(np, "ibm,npu-link-index");
		/* May be overridden by platform presence detection */
		dev->brick_index = dev->link_index;
		/* Will be overridden by presence detection */
		dev->type = NPU2_DEV_TYPE_UNKNOWN;
		dev->npu = npu;
		dev->dt_node = np;
		dev->pl_xscom_base = dt_prop_get_u64(np, "ibm,npu-phy");
		dev->lane_mask = dt_prop_get_u32(np, "ibm,npu-lane-mask");
		dev->link_speed = dt_prop_get_u64(np, "ibm,link-speed");
		i++;
	};
	npu->total_devices = i;

	prlog(PR_INFO, "NPU: Chip %d Found NPU2#%d (%d links) at %s\n",
	      npu->chip_id, npu->index, npu->total_devices, path);
	prlog(PR_INFO, "   SCOM Base:  %08llx\n", npu->xscom_base);
	free(path);
	return npu;

failed:
	prlog(PR_ERR, "NPU: Chip %d NPU setup failed\n", gcid);
	free(path);
	free(npu);
	return NULL;
}

static void setup_devices(struct npu2 *npu)
{
	bool nvlink_detected = false, ocapi_detected = false;
	struct npu2_dev *dev;

	/*
	 * TODO: In future, we'll do brick configuration here to support mixed
	 * setups.
	 */
	for (int i = 0; i < npu->total_devices; i++) {
		dev = &npu->devices[i];
		switch (dev->type) {
		case NPU2_DEV_TYPE_NVLINK:
			nvlink_detected = true;
			dt_add_property_strings(dev->dt_node,
						"ibm,npu-link-type",
						"nvlink");
			break;
		case NPU2_DEV_TYPE_OPENCAPI:
			ocapi_detected = true;
			dt_add_property_strings(dev->dt_node,
						"ibm,npu-link-type",
						"opencapi");
			break;
		default:
			prlog(PR_INFO, "NPU: Link %d device not present\n",
			      npu->devices[i].link_index);
			dt_add_property_strings(dev->dt_node,
						"ibm,npu-link-type",
						"unknown");
		}
	}

	if (nvlink_detected && ocapi_detected) {
		prlog(PR_ERR, "NPU: NVLink and OpenCAPI devices on same chip not supported, aborting NPU init\n");
		return;
	}

	if (nvlink_detected)
		npu2_nvlink_init_npu(npu);
	else if (ocapi_detected)
		npu2_opencapi_init_npu(npu);
}

void probe_npu2(void)
{
	struct proc_chip *chip = next_chip(NULL);
	struct npu2 *npu;
	struct dt_node *np;
	const char *zcal;

	/* Abort if we're running on DD1 */
	if (chip &&
	    (chip->type == PROC_CHIP_P9_NIMBUS ||
	     chip->type == PROC_CHIP_P9_CUMULUS) &&
	    (chip->ec_level & 0xf0) == 0x10) {
		prlog(PR_INFO, "NPU2: DD1 not supported\n");
		return;
	}

	/* Check for a zcal override */
	zcal = nvram_query("nv_zcal_override");
	if (zcal) {
		nv_zcal_nominal = atoi(zcal);
		prlog(PR_WARNING, "NPU2: Using ZCAL impedance override = %d\n", nv_zcal_nominal);
	}

	if (!platform.npu2_device_detect) {
		prlog(PR_INFO, "NPU: Platform does not support NPU\n");
		return;
	}

	dt_for_each_compatible(dt_root, np, "ibm,power9-npu") {
	        npu = setup_npu(np);
		if (!npu)
			continue;
		platform.npu2_device_detect(npu);
		set_brick_config(npu);
		setup_devices(npu);
	}
}

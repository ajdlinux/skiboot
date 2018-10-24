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

/*
 * Support for OpenCAPI on POWER9 NPUs
 *
 * This file provides support for OpenCAPI as implemented on POWER9.
 *
 * At present, we initialise the NPU separately from the NVLink code in npu2.c.
 * As such, we don't currently support mixed NVLink and OpenCAPI configurations
 * on the same NPU for machines such as Witherspoon.
 *
 * Procedure references in this file are to the POWER9 OpenCAPI NPU Workbook
 * (IBM internal document).
 *
 * TODO:
 *   - Support for mixed NVLink and OpenCAPI on the same NPU
 *   - Support for link ganging (one AFU using multiple links)
 *   - Link reset and error handling
 *   - Presence detection
 *   - Consume HDAT NPU information
 *   - LPC Memory support
 */

#include <skiboot.h>
#include <xscom.h>
#include <io.h>
#include <timebase.h>
#include <pci.h>
#include <pci-cfg.h>
#include <pci-slot.h>
#include <interrupts.h>
#include <opal.h>
#include <opal-api.h>
#include <npu2.h>
#include <npu2-regs.h>
#include <phys-map.h>
#include <xive.h>
#include <i2c.h>
#include <nvram.h>

#define NPU_IRQ_LEVELS		35
#define NPU_IRQ_LEVELS_XSL	23
#define MAX_PE_HANDLE		((1 << 15) - 1)
#define TL_MAX_TEMPLATE		63
#define TL_RATE_BUF_SIZE	32

#define OCAPI_SLOT_NORMAL                   PCI_SLOT_STATE_NORMAL
#define OCAPI_SLOT_LINK                     PCI_SLOT_STATE_LINK
#define   OCAPI_SLOT_LINK_START             (OCAPI_SLOT_LINK + 1)
#define   OCAPI_SLOT_LINK_WAIT              (OCAPI_SLOT_LINK + 2)
#define   OCAPI_SLOT_LINK_TRAINED           (OCAPI_SLOT_LINK + 3)
#define OCAPI_SLOT_FRESET                   PCI_SLOT_STATE_FRESET
#define   OCAPI_SLOT_FRESET_START           (OCAPI_SLOT_FRESET + 1)
#define   OCAPI_SLOT_FRESET_INIT            (OCAPI_SLOT_FRESET + 2)
#define   OCAPI_SLOT_FRESET_ASSERT_DELAY    (OCAPI_SLOT_FRESET + 3)
#define   OCAPI_SLOT_FRESET_DEASSERT_DELAY  (OCAPI_SLOT_FRESET + 4)
#define   OCAPI_SLOT_FRESET_DEASSERT_DELAY2 (OCAPI_SLOT_FRESET + 5)
#define   OCAPI_SLOT_FRESET_INIT_DELAY      (OCAPI_SLOT_FRESET + 6)

#define OCAPI_LINK_TRAINING_RETRIES	2
#define OCAPI_LINK_TRAINING_TIMEOUT	3000 /* ms */
#define OCAPI_LINK_STATE_TRAINED        0x7

enum npu2_link_training_state {
	NPU2_TRAIN_DEFAULT, /* fully train the link */
	NPU2_TRAIN_PRBS31,  /* used for Signal Integrity testing */
	NPU2_TRAIN_NONE,    /* used for testing with loopback cable */
};
static enum npu2_link_training_state npu2_ocapi_training_state = NPU2_TRAIN_DEFAULT;

static const struct phb_ops npu2_opencapi_ops;

static inline uint64_t index_to_stack(uint64_t index) {
	switch (index) {
	case 2:
	case 3:
		return NPU2_STACK_STCK_1;
		break;
	case 4:
	case 5:
		return NPU2_STACK_STCK_2;
		break;
	default:
		assert(false);
	}
}

static inline uint64_t index_to_stacku(uint64_t index) {
	switch (index) {
	case 2:
	case 3:
		return NPU2_STACK_STCK_1U;
		break;
	case 4:
	case 5:
		return NPU2_STACK_STCK_2U;
		break;
	default:
		assert(false);
	}
}

static inline uint64_t index_to_block(uint64_t index) {
	switch (index) {
	case 2:
	case 4:
		return NPU2_BLOCK_OTL0;
		break;
	case 3:
	case 5:
		return NPU2_BLOCK_OTL1;
		break;
	default:
		assert(false);
	}
}

static uint64_t get_odl_status(uint32_t gcid, uint64_t index)
{
	uint64_t reg, status_xscom;

	status_xscom = OB_ODL_STATUS(index);
	xscom_read(gcid, status_xscom, &reg);
	return reg;
}

static uint64_t get_odl_training_status(uint32_t gcid, uint64_t index)
{
	uint64_t status_xscom, reg;

	status_xscom = OB_ODL_TRAINING_STATUS(index);
	xscom_read(gcid, status_xscom, &reg);
	return reg;
}

static uint64_t get_odl_endpoint_info(uint32_t gcid, uint64_t index)
{
	uint64_t status_xscom, reg;

	status_xscom = OB_ODL_ENDPOINT_INFO(index);
	xscom_read(gcid, status_xscom, &reg);
	return reg;
}

static void disable_nvlink(uint32_t gcid, int index)
{
	uint64_t phy_config_scom, reg;

	switch (index) {
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
	xscom_read(gcid, phy_config_scom, &reg);
	reg &= ~OBUS_IOOL_PHY_CONFIG_NV0_NPU_ENABLED;
	reg &= ~OBUS_IOOL_PHY_CONFIG_NV1_NPU_ENABLED;
	reg &= ~OBUS_IOOL_PHY_CONFIG_NV2_NPU_ENABLED;
	xscom_write(gcid, phy_config_scom, reg);
}

/* Procedure 13.1.3.1 - select OCAPI vs NVLink for bricks 2-3/4-5 */

static void set_transport_mux_controls(uint32_t gcid, uint32_t scom_base,
				       int index, enum npu2_dev_type type)
{
	/* Step 1 - Set Transport MUX controls to select correct OTL or NTL */
	uint64_t reg;
	uint64_t field;

	/* TODO: Rework this to select for NVLink too */
	assert(type == NPU2_DEV_TYPE_OPENCAPI);

	prlog(PR_DEBUG, "OCAPI: %s: Setting transport mux controls\n", __func__);

	/* Optical IO Transport Mux Config for Bricks 0-2 and 4-5 */
	reg = npu2_scom_read(gcid, scom_base, NPU2_MISC_OPTICAL_IO_CFG0,
			     NPU2_MISC_DA_LEN_8B);
	switch (index) {
	case 0:
	case 1:
		/* not valid for OpenCAPI */
		assert(false);
		break;
	case 2:	 /* OTL1.0 */
		field = GETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_NDLMUX_BRK0TO2, reg);
		field &= ~0b100;
		reg = SETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_NDLMUX_BRK0TO2, reg,
			       field);
		field = GETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_OCMUX_BRK0TO1, reg);
		field |= 0b10;
		reg = SETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_OCMUX_BRK0TO1, reg,
			       field);
		break;
	case 3:	 /* OTL1.1 */
		field = GETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_NDLMUX_BRK0TO2, reg);
		field &= ~0b010;
		reg = SETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_NDLMUX_BRK0TO2, reg,
			       field);
		field = GETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_OCMUX_BRK0TO1, reg);
		field |= 0b01;
		reg = SETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_OCMUX_BRK0TO1, reg,
			       field);
		break;
	case 4:	 /* OTL2.0 */
		field = GETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_OCMUX_BRK4TO5, reg);
		field |= 0b10;
		reg = SETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_OCMUX_BRK4TO5, reg,
			       field);
		break;
	case 5:	 /* OTL2.1 */
		field = GETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_OCMUX_BRK4TO5, reg);
		field |= 0b01;
		reg = SETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_OCMUX_BRK4TO5, reg,
			       field);
		break;
	default:
		assert(false);
	}
	npu2_scom_write(gcid, scom_base, NPU2_MISC_OPTICAL_IO_CFG0,
			NPU2_MISC_DA_LEN_8B, reg);

	/*
	 * PowerBus Optical Miscellaneous Config Register - select
	 * OpenCAPI for b4/5 and A-Link for b3
	 */
	xscom_read(gcid, PU_IOE_PB_MISC_CFG, &reg);
	switch (index) {
	case 0:
	case 1:
	case 2:
	case 3:
		break;
	case 4:
		reg = SETFIELD(PU_IOE_PB_MISC_CFG_SEL_04_NPU_NOT_PB, reg, 1);
		break;
	case 5:
		reg = SETFIELD(PU_IOE_PB_MISC_CFG_SEL_05_NPU_NOT_PB, reg, 1);
		break;
	}
	xscom_write(gcid, PU_IOE_PB_MISC_CFG, reg);
}

static void assert_odl_reset(uint32_t gcid, int index)
{
	uint64_t reg, config_xscom;

	config_xscom = OB_ODL_CONFIG(index);
	/* Reset ODL */
	reg = OB_ODL_CONFIG_RESET;
	reg = SETFIELD(OB_ODL_CONFIG_VERSION, reg, 0b000001);
	reg = SETFIELD(OB_ODL_CONFIG_TRAIN_MODE, reg, 0b0110);
	reg = SETFIELD(OB_ODL_CONFIG_SUPPORTED_MODES, reg, 0b0010);
	reg |= OB_ODL_CONFIG_X4_BACKOFF_ENABLE;
	reg = SETFIELD(OB_ODL_CONFIG_PHY_CNTR_LIMIT, reg, 0b1111);
	reg |= OB_ODL_CONFIG_DEBUG_ENABLE;
	reg = SETFIELD(OB_ODL_CONFIG_FWD_PROGRESS_TIMER, reg, 0b0110);
	xscom_write(gcid, config_xscom, reg);
}

static void deassert_odl_reset(uint32_t gcid, int index)
{
	uint64_t reg, config_xscom;

	config_xscom = OB_ODL_CONFIG(index);
	xscom_read(gcid, config_xscom, &reg);
	reg &= ~OB_ODL_CONFIG_RESET;
	xscom_write(gcid, config_xscom, reg);
}

static void enable_odl_phy_mux(uint32_t gcid, int index)
{
	uint64_t reg;
	uint64_t phy_config_scom;
	prlog(PR_DEBUG, "OCAPI: %s: Enabling ODL to PHY MUXes\n", __func__);
	/* Step 2 - Enable MUXes for ODL to PHY connection */
	switch (index) {
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

	/*
	 * ODL must be in reset when enabling.
	 * It stays in reset until the link is trained
	 */
	assert_odl_reset(gcid, index);

	/* PowerBus OLL PHY Training Config Register */
	xscom_read(gcid, phy_config_scom, &reg);

	/*
	 * Enable ODL to use shared PHYs
	 *
	 * On obus3, OTL0 is connected to ODL1 (and OTL1 to ODL0), so
	 * even if it may look odd at first, we do want to enable ODL0
	 * for links 2 and 5
	 */
	switch (index) {
	case 2:
	case 5:
		reg |= OBUS_IOOL_PHY_CONFIG_ODL0_ENABLED;
		break;
	case 3:
	case 4:
		reg |= OBUS_IOOL_PHY_CONFIG_ODL1_ENABLED;
		break;
	}

	/*
	 * Based on the platform, we may have to activate an extra mux
	 * to connect the ODL to the right set of lanes.
	 *
	 * FIXME: to be checked once we have merged with nvlink
	 * code. Need to verify that it's a platform parameter and not
	 * slot-dependent
	 */
	if (platform.ocapi->odl_phy_swap)
		reg |= OBUS_IOOL_PHY_CONFIG_ODL_PHY_SWAP;
	else
		reg &= ~OBUS_IOOL_PHY_CONFIG_ODL_PHY_SWAP;

	/* Disable A-Link link layers */
	reg &= ~OBUS_IOOL_PHY_CONFIG_LINK0_OLL_ENABLED;
	reg &= ~OBUS_IOOL_PHY_CONFIG_LINK1_OLL_ENABLED;

	xscom_write(gcid, phy_config_scom, reg);
}

static void disable_alink_fp(uint32_t gcid)
{
	uint64_t reg = 0;

	prlog(PR_DEBUG, "OCAPI: %s: Disabling A-Link framer/parsers\n", __func__);
	/* Step 3 - Disable A-Link framers/parsers */
	/* TODO: Confirm if needed on OPAL system */

	reg |= PU_IOE_PB_FP_CFG_FP0_FMR_DISABLE;
	reg |= PU_IOE_PB_FP_CFG_FP0_PRS_DISABLE;
	reg |= PU_IOE_PB_FP_CFG_FP1_FMR_DISABLE;
	reg |= PU_IOE_PB_FP_CFG_FP1_PRS_DISABLE;
	xscom_write(gcid, PU_IOE_PB_FP01_CFG, reg);
	xscom_write(gcid, PU_IOE_PB_FP23_CFG, reg);
	xscom_write(gcid, PU_IOE_PB_FP45_CFG, reg);
	xscom_write(gcid, PU_IOE_PB_FP67_CFG, reg);
}

static void enable_xsl_clocks(uint32_t gcid, uint32_t scom_base, int index)
{
	/* Step 5 - Enable Clocks in XSL */

	prlog(PR_DEBUG, "OCAPI: %s: Enable clocks in XSL\n", __func__);

	npu2_scom_write(gcid, scom_base, NPU2_REG_OFFSET(index_to_stack(index),
							 NPU2_BLOCK_XSL,
							 NPU2_XSL_WRAP_CFG),
			NPU2_MISC_DA_LEN_8B, NPU2_XSL_WRAP_CFG_XSLO_CLOCK_ENABLE);
}

#define CQ_CTL_STATUS_TIMEOUT	10 /* milliseconds */

static int set_fence_control(uint32_t gcid, uint32_t scom_base,
			     int index, uint8_t status)
{
	int stack, block;
	uint64_t reg, status_field;
	uint8_t status_val;
	uint64_t fence_control;
	uint64_t timeout = mftb() + msecs_to_tb(CQ_CTL_STATUS_TIMEOUT);

	stack = index_to_stack(index);
	block = index_to_block(index);

	fence_control = NPU2_REG_OFFSET(stack, NPU2_BLOCK_CTL,
					block == NPU2_BLOCK_OTL0 ?
					NPU2_CQ_CTL_FENCE_CONTROL_0 :
					NPU2_CQ_CTL_FENCE_CONTROL_1);

	reg = SETFIELD(NPU2_CQ_CTL_FENCE_CONTROL_REQUEST_FENCE, 0ull, status);
	npu2_scom_write(gcid, scom_base, fence_control,
			NPU2_MISC_DA_LEN_8B, reg);

	/* Wait for fence status to update */
	if (index_to_block(index) == NPU2_BLOCK_OTL0)
		status_field = NPU2_CQ_CTL_STATUS_BRK0_AM_FENCED;
	else
		status_field = NPU2_CQ_CTL_STATUS_BRK1_AM_FENCED;

	do {
		reg = npu2_scom_read(gcid, scom_base,
				     NPU2_REG_OFFSET(index_to_stack(index),
						     NPU2_BLOCK_CTL,
						     NPU2_CQ_CTL_STATUS),
				     NPU2_MISC_DA_LEN_8B);
		status_val = GETFIELD(status_field, reg);
		if (status_val == status)
			return OPAL_SUCCESS;
		time_wait_ms(1);
	} while (tb_compare(mftb(), timeout) == TB_ABEFOREB);

	/**
	 * @fwts-label OCAPIFenceStatusTimeout
	 * @fwts-advice The NPU fence status did not update as expected. This
	 * could be the result of a firmware or hardware bug. OpenCAPI
	 * functionality could be broken.
	 */
	prlog(PR_ERR,
	      "OCAPI: Fence status for brick %d stuck: expected 0x%x, got 0x%x\n",
	      index, status, status_val);
	return OPAL_HARDWARE;
}

static void set_npcq_config(uint32_t gcid, uint32_t scom_base, int index)
{
	uint64_t reg, stack, block;

	prlog(PR_DEBUG, "OCAPI: %s: Set NPCQ Config\n", __func__);
	/* Step 6 - Set NPCQ configuration */
	/* CQ_CTL Misc Config Register #0 */
	stack = index_to_stack(index);
	block = index_to_block(index);

	/* Enable OTL */
	npu2_scom_write(gcid, scom_base, NPU2_OTL_CONFIG0(stack, block),
			NPU2_MISC_DA_LEN_8B, NPU2_OTL_CONFIG0_EN);
	set_fence_control(gcid, scom_base, index, 0b01);
	reg = npu2_scom_read(gcid, scom_base,
			     NPU2_REG_OFFSET(stack, NPU2_BLOCK_CTL,
					     NPU2_CQ_CTL_MISC_CFG),
			     NPU2_MISC_DA_LEN_8B);
	/* Set OCAPI mode */
	reg |= NPU2_CQ_CTL_MISC_CFG_CONFIG_OCAPI_MODE;
	if (block == NPU2_BLOCK_OTL0)
		reg |= NPU2_CQ_CTL_MISC_CFG_CONFIG_OTL0_ENABLE;
	else
		reg |= NPU2_CQ_CTL_MISC_CFG_CONFIG_OTL1_ENABLE;
	npu2_scom_write(gcid, scom_base,
			NPU2_REG_OFFSET(stack, NPU2_BLOCK_CTL,
					NPU2_CQ_CTL_MISC_CFG),
			NPU2_MISC_DA_LEN_8B, reg);

	/* NPU Fenced */
	set_fence_control(gcid, scom_base, index, 0b11);

	/* NPU Half Fenced */
	set_fence_control(gcid, scom_base, index, 0b10);

	/* CQ_DAT Misc Config Register #1 */
	reg = npu2_scom_read(gcid, scom_base,
			     NPU2_REG_OFFSET(stack, NPU2_BLOCK_DAT,
					     NPU2_CQ_DAT_MISC_CFG),
			     NPU2_MISC_DA_LEN_8B);
	/* Set OCAPI mode for bricks 2-5 */
	reg |= NPU2_CQ_DAT_MISC_CFG_CONFIG_OCAPI_MODE;
	npu2_scom_write(gcid, scom_base,
			NPU2_REG_OFFSET(stack, NPU2_BLOCK_DAT,
					NPU2_CQ_DAT_MISC_CFG),
			NPU2_MISC_DA_LEN_8B, reg);

	/* CQ_SM Misc Config Register #0 */
	for (block = NPU2_BLOCK_SM_0; block <= NPU2_BLOCK_SM_3; block++) {
		reg = npu2_scom_read(gcid, scom_base,
				     NPU2_REG_OFFSET(stack, block,
						     NPU2_CQ_SM_MISC_CFG0),
				     NPU2_MISC_DA_LEN_8B);
		/* Set OCAPI mode for bricks 2-5 */
		reg |= NPU2_CQ_SM_MISC_CFG0_CONFIG_OCAPI_MODE;
		npu2_scom_write(gcid, scom_base,
				NPU2_REG_OFFSET(stack, block,
						NPU2_CQ_SM_MISC_CFG0),
				NPU2_MISC_DA_LEN_8B, reg);
	}
}

static void enable_xsl_xts_interfaces(uint32_t gcid, uint32_t scom_base, int index)
{
	uint64_t reg;

	prlog(PR_DEBUG, "OCAPI: %s: Enable XSL-XTS Interfaces\n", __func__);
	/* Step 7 - Enable XSL-XTS interfaces */
	/* XTS Config Register - Enable XSL-XTS interface */
	reg = npu2_scom_read(gcid, scom_base, NPU2_XTS_CFG, NPU2_MISC_DA_LEN_8B);
	reg |= NPU2_XTS_CFG_OPENCAPI;
	npu2_scom_write(gcid, scom_base, NPU2_XTS_CFG, NPU2_MISC_DA_LEN_8B, reg);

	/* XTS Config2 Register - Enable XSL1/2 */
	reg = npu2_scom_read(gcid, scom_base, NPU2_XTS_CFG2, NPU2_MISC_DA_LEN_8B);
	switch (index_to_stack(index)) {
	case NPU2_STACK_STCK_1:
		reg |= NPU2_XTS_CFG2_XSL1_ENA;
		break;
	case NPU2_STACK_STCK_2:
		reg |= NPU2_XTS_CFG2_XSL2_ENA;
		break;
	}
	npu2_scom_write(gcid, scom_base, NPU2_XTS_CFG2, NPU2_MISC_DA_LEN_8B, reg);
}

static void enable_sm_allocation(uint32_t gcid, uint32_t scom_base, int index)
{
	uint64_t reg, block;
	int stack = index_to_stack(index);

	prlog(PR_DEBUG, "OCAPI: %s: Enable State Machine Allocation\n", __func__);
	/* Step 8 - Enable state-machine allocation */
	/* Low-Water Marks Registers - Enable state machine allocation */
	for (block = NPU2_BLOCK_SM_0; block <= NPU2_BLOCK_SM_3; block++) {
		reg = npu2_scom_read(gcid, scom_base,
				     NPU2_REG_OFFSET(stack, block,
						     NPU2_LOW_WATER_MARKS),
				     NPU2_MISC_DA_LEN_8B);
		reg |= NPU2_LOW_WATER_MARKS_ENABLE_MACHINE_ALLOC;
		npu2_scom_write(gcid, scom_base,
				NPU2_REG_OFFSET(stack, block,
						NPU2_LOW_WATER_MARKS),
				NPU2_MISC_DA_LEN_8B, reg);
	}
}

static void enable_pb_snooping(uint32_t gcid, uint32_t scom_base, int index)
{
	uint64_t reg, block;
	int stack = index_to_stack(index);

	prlog(PR_DEBUG, "OCAPI: %s: Enable PowerBus snooping\n", __func__);
	/* Step 9 - Enable PowerBus snooping */
	/* CQ_SM Misc Config Register #0 - Enable PowerBus snooping */
	for (block = NPU2_BLOCK_SM_0; block <= NPU2_BLOCK_SM_3; block++) {
		reg = npu2_scom_read(gcid, scom_base,
				     NPU2_REG_OFFSET(stack, block,
						     NPU2_CQ_SM_MISC_CFG0),
				     NPU2_MISC_DA_LEN_8B);
		reg |= NPU2_CQ_SM_MISC_CFG0_CONFIG_ENABLE_PBUS;
		npu2_scom_write(gcid, scom_base,
				NPU2_REG_OFFSET(stack, block,
						NPU2_CQ_SM_MISC_CFG0),
				NPU2_MISC_DA_LEN_8B, reg);
	}
}

static void brick_config(uint32_t gcid, uint32_t scom_base, int index)
{
	/*
	 * We assume at this point that the PowerBus Hotplug Mode Control
	 * register is correctly set by Hostboot
	 */
	disable_nvlink(gcid, index);
	set_transport_mux_controls(gcid, scom_base, index,
				   NPU2_DEV_TYPE_OPENCAPI);
	enable_odl_phy_mux(gcid, index);
	disable_alink_fp(gcid);
	enable_xsl_clocks(gcid, scom_base, index);
	set_npcq_config(gcid, scom_base, index);
	enable_xsl_xts_interfaces(gcid, scom_base, index);
	enable_sm_allocation(gcid, scom_base, index);
	enable_pb_snooping(gcid, scom_base, index);
}

/* Procedure 13.1.3.5 - TL Configuration */
static void tl_config(uint32_t gcid, uint32_t scom_base, uint64_t index)
{
	uint64_t reg;
	uint64_t stack = index_to_stack(index);
	uint64_t block = index_to_block(index);

	prlog(PR_DEBUG, "OCAPI: %s: TL Configuration\n", __func__);
	/* OTL Config 0 Register */
	reg = 0;
	/* OTL Enable */
	reg |= NPU2_OTL_CONFIG0_EN;
	/* Block PE Handle from ERAT Index */
	reg |= NPU2_OTL_CONFIG0_BLOCK_PE_HANDLE;
	/* OTL Brick ID */
	reg = SETFIELD(NPU2_OTL_CONFIG0_BRICKID, reg, index - 2);
	/* ERAT Hash 0 */
	reg = SETFIELD(NPU2_OTL_CONFIG0_ERAT_HASH_0, reg, 0b011001);
	/* ERAT Hash 1 */
	reg = SETFIELD(NPU2_OTL_CONFIG0_ERAT_HASH_1, reg, 0b000111);
	/* ERAT Hash 2 */
	reg = SETFIELD(NPU2_OTL_CONFIG0_ERAT_HASH_2, reg, 0b101100);
	/* ERAT Hash 3 */
	reg = SETFIELD(NPU2_OTL_CONFIG0_ERAT_HASH_3, reg, 0b100110);
	npu2_scom_write(gcid, scom_base, NPU2_OTL_CONFIG0(stack, block),
			NPU2_MISC_DA_LEN_8B, reg);

	/* OTL Config 1 Register */
	reg = 0;
	/*
	 * We leave Template 1-3 bits at 0 to force template 0 as required
	 * for unknown devices.
	 *
	 * Template 0 Transmit Rate is set to most conservative setting which
	 * will always be supported. Other Template Transmit rates are left
	 * unset and will be set later by OS.
	 */
	reg = SETFIELD(NPU2_OTL_CONFIG1_TX_TEMP0_RATE, reg, 0b1111);
	/* Extra wait cycles TXI-TXO */
	reg = SETFIELD(NPU2_OTL_CONFIG1_TX_DRDY_WAIT, reg, 0b001);
	/* Minimum Frequency to Return TLX Credits to AFU */
	reg = SETFIELD(NPU2_OTL_CONFIG1_TX_CRET_FREQ, reg, 0b001);
	/* Frequency to add age to Transmit Requests */
	reg = SETFIELD(NPU2_OTL_CONFIG1_TX_AGE_FREQ, reg, 0b11000);
	/* Response High Priority Threshold */
	reg = SETFIELD(NPU2_OTL_CONFIG1_TX_RS2_HPWAIT, reg, 0b011011);
	/* 4-slot Request High Priority Threshold */
	reg = SETFIELD(NPU2_OTL_CONFIG1_TX_RQ4_HPWAIT, reg, 0b011011);
	/* 6-slot Request High Priority */
	reg = SETFIELD(NPU2_OTL_CONFIG1_TX_RQ6_HPWAIT, reg, 0b011011);
	/* Stop the OCAPI Link on Uncorrectable Error
	 * TODO: Confirm final value - disabled for debug */

	npu2_scom_write(gcid, scom_base, NPU2_OTL_CONFIG1(stack, block),
			NPU2_MISC_DA_LEN_8B, reg);

	/* TLX Credit Configuration Register */
	reg = 0;
	/* VC0/VC3/DCP0/DCP1 credits to send to AFU */
	reg = SETFIELD(NPU2_OTL_TLX_CREDITS_VC0_CREDITS, reg, 0x40);
	reg = SETFIELD(NPU2_OTL_TLX_CREDITS_VC3_CREDITS, reg, 0x40);
	reg = SETFIELD(NPU2_OTL_TLX_CREDITS_DCP0_CREDITS, reg, 0x80);
	reg = SETFIELD(NPU2_OTL_TLX_CREDITS_DCP1_CREDITS, reg, 0x80);
	npu2_scom_write(gcid, scom_base, NPU2_OTL_TLX_CREDITS(stack, block),
			NPU2_MISC_DA_LEN_8B, reg);
}

/* Detect Nimbus DD2.0 and DD2.01 */
static int get_nimbus_level(void)
{
	struct proc_chip *chip = next_chip(NULL);

	if (chip && chip->type == PROC_CHIP_P9_NIMBUS)
		return chip->ec_level & 0xff;
	return -1;
}

/* Procedure 13.1.3.6 - Address Translation Configuration */
static void address_translation_config(uint32_t gcid, uint32_t scom_base,
				       uint64_t index)
{
	int chip_level;
	uint64_t reg;
	uint64_t stack = index_to_stack(index);

	prlog(PR_DEBUG, "OCAPI: %s: Address Translation Configuration\n", __func__);
	/* PSL_SCNTL_A0 Register */
	/*
	 * ERAT shared between multiple AFUs
	 *
	 * The workbook has this bit around the wrong way from the hardware.
	 *
	 * TODO: handle correctly with link ganging
	 */
	reg = npu2_scom_read(gcid, scom_base,
			     NPU2_REG_OFFSET(stack, NPU2_BLOCK_XSL,
					     NPU2_XSL_PSL_SCNTL_A0),
			     NPU2_MISC_DA_LEN_8B);
	reg |= NPU2_XSL_PSL_SCNTL_A0_MULTI_AFU_DIAL;
	npu2_scom_write(gcid, scom_base,
			NPU2_REG_OFFSET(stack, NPU2_BLOCK_XSL,
					NPU2_XSL_PSL_SCNTL_A0),
			NPU2_MISC_DA_LEN_8B, reg);

	chip_level = get_nimbus_level();
	if (chip_level == 0x20) {
		/*
		 * Errata HW408041 (section 15.1.10 of NPU workbook)
		 * "RA mismatch when both tlbie and checkout response
		 * are seen in same cycle"
		 */
		/* XSL_GP Register - Bloom Filter Disable */
		reg = npu2_scom_read(gcid, scom_base,
				NPU2_REG_OFFSET(stack, NPU2_BLOCK_XSL, NPU2_XSL_GP),
				NPU2_MISC_DA_LEN_8B);
		/* To update XSL_GP, we must first write a magic value to it */
		npu2_scom_write(gcid, scom_base,
				NPU2_REG_OFFSET(stack, NPU2_BLOCK_XSL, NPU2_XSL_GP),
				NPU2_MISC_DA_LEN_8B, 0x0523790323000000UL);
		reg &= ~NPU2_XSL_GP_BLOOM_FILTER_ENABLE;
		npu2_scom_write(gcid, scom_base,
				NPU2_REG_OFFSET(stack, NPU2_BLOCK_XSL, NPU2_XSL_GP),
				NPU2_MISC_DA_LEN_8B, reg);
	}

	if (chip_level == 0x20 || chip_level == 0x21) {
		/*
		 * DD2.0/2.1 EOA Bug. Fixed in DD2.2
		 */
		reg = 0x32F8000000000001UL;
		npu2_scom_write(gcid, scom_base,
				NPU2_REG_OFFSET(stack, NPU2_BLOCK_XSL,
						NPU2_XSL_DEF),
				NPU2_MISC_DA_LEN_8B, reg);
	}
}

static void setup_global_mmio_bar(uint32_t gcid, uint32_t scom_base,
				  uint64_t reg[])
{
	struct npu2_bar *bar;
	struct npu2_bar phy_bars[] = {
		{ .type = NPU_PHY, .index = 0,
		  .reg = NPU2_REG_OFFSET(NPU2_STACK_STCK_2, 0, NPU2_PHY_BAR),
		  .enabled = true },
		{ .type = NPU_PHY, .index = 1,
		  .reg = NPU2_REG_OFFSET(NPU2_STACK_STCK_1, 0, NPU2_PHY_BAR),
		  .enabled = true },
		{ .type = NPU_REGS, .index = 0,
		  .reg = NPU2_REG_OFFSET(NPU2_STACK_STCK_0, 0, NPU2_PHY_BAR),
		  .enabled = true },
	};

	for (int i = 0; i < ARRAY_SIZE(phy_bars); i++) {
		bar = &phy_bars[i];
		npu2_get_bar(gcid, bar);
		npu2_write_bar(NULL, bar, gcid, scom_base);
	}
	reg[0] = phy_bars[2].base;
	reg[1] = phy_bars[2].size;
}

/* Procedure 13.1.3.8 - AFU MMIO Range BARs */
static void setup_afu_mmio_bars(uint32_t gcid, uint32_t scom_base,
				struct npu2_dev *dev)
{
	uint64_t stack = index_to_stack(dev->brick_index);
	uint64_t offset = index_to_block(dev->brick_index) == NPU2_BLOCK_OTL0 ?
		NPU2_NTL0_BAR : NPU2_NTL1_BAR;
	uint64_t pa_offset = index_to_block(dev->brick_index) == NPU2_BLOCK_OTL0 ?
		NPU2_CQ_CTL_MISC_MMIOPA0_CONFIG :
		NPU2_CQ_CTL_MISC_MMIOPA1_CONFIG;
	uint64_t reg;

	prlog(PR_DEBUG, "OCAPI: %s: Setup AFU MMIO BARs\n", __func__);
	dev->bars[0].type = NPU_OCAPI_MMIO;
	dev->bars[0].index = dev->brick_index;
	dev->bars[0].reg = NPU2_REG_OFFSET(stack, 0, offset);
	dev->bars[0].enabled = true;
	npu2_get_bar(gcid, &dev->bars[0]);

	prlog(PR_DEBUG, "OCAPI: AFU MMIO set to %llx, size %llx\n",
	      dev->bars[0].base, dev->bars[0].size);
	npu2_write_bar(NULL, &dev->bars[0], gcid, scom_base);

	reg = SETFIELD(NPU2_CQ_CTL_MISC_MMIOPA_ADDR, 0ull, dev->bars[0].base >> 16);
	reg = SETFIELD(NPU2_CQ_CTL_MISC_MMIOPA_SIZE, reg, ilog2(dev->bars[0].size >> 16));
	prlog(PR_DEBUG, "OCAPI: PA translation %llx\n", reg);
	npu2_scom_write(gcid, scom_base,
			NPU2_REG_OFFSET(stack, NPU2_BLOCK_CTL,
					pa_offset),
			NPU2_MISC_DA_LEN_8B, reg);
}

/* Procedure 13.1.3.9 - AFU Config BARs */
static void setup_afu_config_bars(uint32_t gcid, uint32_t scom_base,
				  struct npu2_dev *dev)
{
	uint64_t stack = index_to_stack(dev->brick_index);
	int stack_num = stack - NPU2_STACK_STCK_0;

	prlog(PR_DEBUG, "OCAPI: %s: Setup AFU Config BARs\n", __func__);
	dev->bars[1].type = NPU_GENID;
	dev->bars[1].index = stack_num;
	dev->bars[1].reg = NPU2_REG_OFFSET(stack, 0, NPU2_GENID_BAR);
	dev->bars[1].enabled = true;
	npu2_get_bar(gcid, &dev->bars[1]);
	prlog(PR_DEBUG, "OCAPI: Assigning GENID BAR: %016llx\n", dev->bars[1].base);
	npu2_write_bar(NULL, &dev->bars[1], gcid, scom_base);
}

static void otl_enabletx(uint32_t gcid, uint32_t scom_base,
			struct npu2_dev *dev)
{
	uint64_t stack = index_to_stack(dev->brick_index);
	uint64_t block = index_to_block(dev->brick_index);
	uint64_t reg;

	/* OTL Config 2 Register */
	/* Transmit Enable */
	OCAPIDBG(dev, "Enabling TX\n");
	reg = 0;
	reg |= NPU2_OTL_CONFIG2_TX_SEND_EN;
	npu2_scom_write(gcid, scom_base, NPU2_OTL_CONFIG2(stack, block),
			NPU2_MISC_DA_LEN_8B, reg);

	reg = npu2_scom_read(gcid, scom_base, NPU2_OTL_VC_CREDITS(stack, block),
			     NPU2_MISC_DA_LEN_8B);
	OCAPIDBG(dev, "credit counter: %llx\n", reg);
	/* TODO: Abort if credits are zero */
}

static uint8_t get_reset_pin(struct npu2_dev *dev)
{
	uint8_t pin;

	switch (dev->brick_index) {
	case 2:
		pin = platform.ocapi->i2c_reset_brick2;
		break;
	case 3:
		pin = platform.ocapi->i2c_reset_brick3;
		break;
	case 4:
		pin = platform.ocapi->i2c_reset_brick4;
		break;
	case 5:
		pin = platform.ocapi->i2c_reset_brick5;
		break;
	default:
		assert(false);
	}
	return pin;
}

static void assert_adapter_reset(struct npu2_dev *dev)
{
	uint8_t pin, data;
	int rc;

	pin = get_reset_pin(dev);
	/*
	 * set the i2c reset pin in output mode
	 *
	 * On the 9554 device, register 3 is the configuration
	 * register and a pin is in output mode if its value is 0
	 */
	lock(&dev->npu->i2c_lock);
	dev->npu->i2c_pin_mode &= ~pin;
	data = dev->npu->i2c_pin_mode;

	rc = i2c_request_send(dev->npu->i2c_port_id_ocapi,
			platform.ocapi->i2c_reset_addr, SMBUS_WRITE,
			0x3, 1,
			&data, sizeof(data), 120);
	if (rc)
		goto err;

	/* register 1 controls the signal, reset is active low */
	dev->npu->i2c_pin_wr_state &= ~pin;
	data = dev->npu->i2c_pin_wr_state;

	rc = i2c_request_send(dev->npu->i2c_port_id_ocapi,
			platform.ocapi->i2c_reset_addr, SMBUS_WRITE,
			0x1, 1,
			&data, sizeof(data), 120);
	if (rc)
		goto err;
	unlock(&dev->npu->i2c_lock);
	return;

err:
	unlock(&dev->npu->i2c_lock);
	/**
	 * @fwts-label OCAPIDeviceResetFailed
	 * @fwts-advice There was an error attempting to send
	 * a reset signal over I2C to the OpenCAPI device.
	 */
	OCAPIERR(dev, "Error writing I2C reset signal: %d\n", rc);
}

static void deassert_adapter_reset(struct npu2_dev *dev)
{
	uint8_t pin, data;
	int rc;

	pin = get_reset_pin(dev);

	lock(&dev->npu->i2c_lock);
	dev->npu->i2c_pin_wr_state |= pin;
	data = dev->npu->i2c_pin_wr_state;

	rc = i2c_request_send(dev->npu->i2c_port_id_ocapi,
			platform.ocapi->i2c_reset_addr, SMBUS_WRITE,
			0x1, 1,
			&data, sizeof(data), 120);
	unlock(&dev->npu->i2c_lock);
	if (rc) {
		/**
		 * @fwts-label OCAPIDeviceResetFailed
		 * @fwts-advice There was an error attempting to send
		 * a reset signal over I2C to the OpenCAPI device.
		 */
		OCAPIERR(dev, "Error writing I2C reset signal: %d\n", rc);
	}
}

static void setup_perf_counters(struct npu2_dev *dev)
{
	uint64_t addr, reg, link;

	/*
	 * setup the DLL perf counters to check CRC errors detected by
	 * the NPU or the adapter.
	 *
	 * Counter 0: link 0/ODL0, CRC error detected by ODL
	 * Counter 1: link 0/ODL0, CRC error detected by DLx
	 * Counter 2: link 1/ODL1, CRC error detected by ODL
	 * Counter 3: link 1/ODL1, CRC error detected by DLx
	 */
	if ((dev->brick_index == 2) || (dev->brick_index == 5))
		link = 0;
	else
		link = 1;

	addr = OB_DLL_PERF_MONITOR_CONFIG(dev->brick_index);
	xscom_read(dev->npu->chip_id, addr, &reg);
	if (link == 0) {
		reg = SETFIELD(OB_DLL_PERF_MONITOR_CONFIG_ENABLE, reg,
			OB_DLL_PERF_MONITOR_CONFIG_LINK0);
		reg = SETFIELD(OB_DLL_PERF_MONITOR_CONFIG_ENABLE >> 2, reg,
			OB_DLL_PERF_MONITOR_CONFIG_LINK0);
	} else {
		reg = SETFIELD(OB_DLL_PERF_MONITOR_CONFIG_ENABLE >> 4, reg,
			OB_DLL_PERF_MONITOR_CONFIG_LINK1);
		reg = SETFIELD(OB_DLL_PERF_MONITOR_CONFIG_ENABLE >> 6, reg,
			OB_DLL_PERF_MONITOR_CONFIG_LINK1);
	}
	reg = SETFIELD(OB_DLL_PERF_MONITOR_CONFIG_SIZE, reg,
		OB_DLL_PERF_MONITOR_CONFIG_SIZE16);
	xscom_write(dev->npu->chip_id,
		OB_DLL_PERF_MONITOR_CONFIG(dev->brick_index), reg);
	OCAPIDBG(dev, "perf counter config %llx = %llx\n", addr, reg);

	addr = OB_DLL_PERF_MONITOR_SELECT(dev->brick_index);
	xscom_read(dev->npu->chip_id, addr, &reg);
	reg = SETFIELD(OB_DLL_PERF_MONITOR_SELECT_COUNTER >> (link * 16),
		reg, OB_DLL_PERF_MONITOR_SELECT_CRC_ODL);
	reg = SETFIELD(OB_DLL_PERF_MONITOR_SELECT_COUNTER >> ((link * 16) + 8),
		reg, OB_DLL_PERF_MONITOR_SELECT_CRC_DLX);
	xscom_write(dev->npu->chip_id, addr, reg);
	OCAPIDBG(dev, "perf counter select %llx = %llx\n", addr, reg);
}

static void check_perf_counters(struct npu2_dev *dev)
{
	uint64_t addr, reg, link0, link1;

	addr = OB_DLL_PERF_COUNTER0(dev->brick_index);
	xscom_read(dev->npu->chip_id, addr, &reg);
	link0 = GETFIELD(PPC_BITMASK(0, 31), reg);
	link1 = GETFIELD(PPC_BITMASK(32, 63), reg);
	if (link0 || link1)
		OCAPIERR(dev, "CRC error count link0=%08llx link1=%08llx\n",
			link0, link1);
}

static void set_init_pattern(uint32_t gcid, struct npu2_dev *dev)
{
	uint64_t reg, config_xscom;

	config_xscom = OB_ODL_CONFIG(dev->brick_index);
	/* Transmit Pattern A */
	xscom_read(gcid, config_xscom, &reg);
	reg = SETFIELD(OB_ODL_CONFIG_TRAIN_MODE, reg, 0b0001);
	xscom_write(gcid, config_xscom, reg);
}

static void start_training(uint32_t gcid, struct npu2_dev *dev)
{
	uint64_t reg, config_xscom;

	config_xscom = OB_ODL_CONFIG(dev->brick_index);
	/* Start training */
	xscom_read(gcid, config_xscom, &reg);
	reg = SETFIELD(OB_ODL_CONFIG_TRAIN_MODE, reg, 0b1000);
	xscom_write(gcid, config_xscom, reg);
}

static int64_t npu2_opencapi_get_presence_state(struct pci_slot __unused *slot,
						uint8_t *val)
{
	/*
	 * Presence detection for OpenCAPI is currently done at the start of
	 * NPU initialisation, and we only create slots if a device is present.
	 * As such we will never be asked to get the presence of a slot that's
	 * empty.
	 *
	 * This may change if we ever support hotplug down the track.
	 */
	*val = true;
	return OPAL_SUCCESS;
}

static enum OpalShpcLinkState get_link_width(uint64_t odl_status)
{
	uint64_t tx_lanes, rx_lanes, state;

	/*
	 * On P9, the 'trained mode' field of the ODL status is
	 * hard-coded to x8 and is useless for us. We need to look at
	 * the status of the individual lanes.
	 * The link trains at x8, x4 or not at all.
	 */
	state = GETFIELD(OB_ODL_STATUS_TRAINING_STATE_MACHINE, odl_status);
	if (state != OCAPI_LINK_STATE_TRAINED)
		return OPAL_SHPC_LINK_DOWN;

	rx_lanes = GETFIELD(OB_ODL_STATUS_RX_TRAINED_LANES, odl_status);
	tx_lanes = GETFIELD(OB_ODL_STATUS_TX_TRAINED_LANES, odl_status);
	if ((rx_lanes != 0xFF) || (tx_lanes != 0xFF))
		return OPAL_SHPC_LINK_UP_x4;
	else
		return OPAL_SHPC_LINK_UP_x8;
}

static int64_t npu2_opencapi_get_link_state(struct pci_slot *slot, uint8_t *val)
{
	struct npu2_dev *dev = phb_to_npu2_dev_ocapi(slot->phb);
	uint64_t reg;

	reg = get_odl_status(dev->npu->chip_id, dev->brick_index);
	*val = get_link_width(reg);
	return OPAL_SUCCESS;
}

static void check_trained_link(struct npu2_dev *dev, uint64_t odl_status)
{
	if (get_link_width(odl_status) != OPAL_SHPC_LINK_UP_x8) {
		OCAPIERR(dev, "Link trained in degraded mode (%016llx)\n",
			odl_status);
		OCAPIDBG(dev, "Link endpoint info: %016llx\n",
			get_odl_endpoint_info(dev->npu->chip_id, dev->brick_index));
	}
}

static int64_t npu2_opencapi_retry_state(struct pci_slot *slot,
					 uint64_t odl_status)
{
	struct npu2_dev *dev = phb_to_npu2_dev_ocapi(slot->phb);
	uint32_t chip_id = dev->npu->chip_id;

	if (!slot->link_retries--) {
		/**
		 * @fwts-label OCAPILinkTrainingFailed
		 * @fwts-advice The OpenCAPI link training procedure failed.
		 * This indicates a hardware or firmware bug. OpenCAPI
		 * functionality will not be available on this link.
		 */
		OCAPIERR(dev,
			"Link failed to train, final link status: %016llx\n",
			odl_status);
		OCAPIDBG(dev, "Final link training status: %016llx\n",
			get_odl_training_status(chip_id, dev->brick_index));
		return OPAL_HARDWARE;
	}

	OCAPIERR(dev, "Link failed to train, retrying\n");
	OCAPIDBG(dev, "Link status: %016llx, training status: %016llx\n",
		odl_status,
		get_odl_training_status(chip_id, dev->brick_index));

	pci_slot_set_state(slot, OCAPI_SLOT_FRESET_INIT);
	return pci_slot_set_sm_timeout(slot, msecs_to_tb(1));
}

static int64_t npu2_opencapi_poll_link(struct pci_slot *slot)
{
	struct npu2_dev *dev = phb_to_npu2_dev_ocapi(slot->phb);
	uint32_t chip_id = dev->npu->chip_id;
	uint64_t reg;

	switch (slot->state) {
	case OCAPI_SLOT_NORMAL:
	case OCAPI_SLOT_LINK_START:
		OCAPIDBG(dev, "Start polling\n");
		pci_slot_set_state(slot, OCAPI_SLOT_LINK_WAIT);
		/* fall-through */
	case OCAPI_SLOT_LINK_WAIT:
		reg = get_odl_status(chip_id, dev->brick_index);
		if (GETFIELD(OB_ODL_STATUS_TRAINING_STATE_MACHINE, reg) ==
			OCAPI_LINK_STATE_TRAINED) {
			OCAPIINF(dev, "link trained in %lld ms\n",
				OCAPI_LINK_TRAINING_TIMEOUT - slot->retries);
			check_trained_link(dev, reg);
			pci_slot_set_state(slot, OCAPI_SLOT_LINK_TRAINED);
			return pci_slot_set_sm_timeout(slot, msecs_to_tb(1));
		}
		if (slot->retries-- == 0)
			return npu2_opencapi_retry_state(slot, reg);

		return pci_slot_set_sm_timeout(slot, msecs_to_tb(1));

	case OCAPI_SLOT_LINK_TRAINED:
		otl_enabletx(chip_id, dev->npu->xscom_base, dev);
		pci_slot_set_state(slot, OCAPI_SLOT_NORMAL);
		check_perf_counters(dev);
		dev->phb_ocapi.scan_map = 1;
		return OPAL_SUCCESS;

	default:
		OCAPIERR(dev, "unexpected slot state %08x\n", slot->state);

	}
	pci_slot_set_state(slot, OCAPI_SLOT_NORMAL);
	return OPAL_HARDWARE;
}

static int64_t npu2_opencapi_creset(struct pci_slot *slot __unused)
{
	struct npu2_dev *dev = phb_to_npu2_dev_ocapi(slot->phb);

	OCAPIERR(dev, "creset not supported\n");
	return OPAL_UNSUPPORTED;
}

static int64_t npu2_opencapi_freset(struct pci_slot *slot)
{
	struct npu2_dev *dev = phb_to_npu2_dev_ocapi(slot->phb);
	uint32_t chip_id = dev->npu->chip_id;
	uint8_t presence = 1;

	switch (slot->state) {
	case OCAPI_SLOT_NORMAL:
	case OCAPI_SLOT_FRESET_START:
		OCAPIDBG(dev, "FRESET starts\n");

		if (slot->ops.get_presence_state)
			slot->ops.get_presence_state(slot, &presence);
		if (!presence) {
			/*
			 * FIXME: if there's no card on the link, we
			 * should consider powering off the unused
			 * lanes to save energy
			 */
			OCAPIINF(dev, "no card detected\n");
			return OPAL_SUCCESS;
		}
		if (dev->train_need_fence) {
			OCAPIDBG(dev, "Fencing OTL during reset\n");
			set_fence_control(chip_id, dev->npu->xscom_base,
					dev->brick_index, 0b11);
			npu2_write(dev->npu, NPU2_MISC_FENCE_STATE,
				PPC_BIT(dev->brick_index + 6));
			dev->train_fenced = true;
		}
		dev->train_need_fence = true;
		slot->link_retries = OCAPI_LINK_TRAINING_RETRIES;
		npu2_opencapi_phy_setup(dev);
		/* fall-through */
	case OCAPI_SLOT_FRESET_INIT:
		assert_odl_reset(chip_id, dev->brick_index);
		assert_adapter_reset(dev);
		pci_slot_set_state(slot,
				OCAPI_SLOT_FRESET_ASSERT_DELAY);
		/* assert for 5ms */
		return pci_slot_set_sm_timeout(slot, msecs_to_tb(5));

	case OCAPI_SLOT_FRESET_ASSERT_DELAY:
		deassert_odl_reset(chip_id, dev->brick_index);
		pci_slot_set_state(slot,
				OCAPI_SLOT_FRESET_DEASSERT_DELAY);
		/*
		 * Minimal delay before taking adapter out of
		 * reset. Could be useless, but doesn't hurt
		 */
		return pci_slot_set_sm_timeout(slot, msecs_to_tb(1));

	case OCAPI_SLOT_FRESET_DEASSERT_DELAY:
		deassert_adapter_reset(dev);
		pci_slot_set_state(slot,
				OCAPI_SLOT_FRESET_DEASSERT_DELAY2);
		/* give 250ms to device to be ready */
		return pci_slot_set_sm_timeout(slot, msecs_to_tb(250));

	case OCAPI_SLOT_FRESET_DEASSERT_DELAY2:
		if (dev->train_fenced) {
			OCAPIDBG(dev, "Unfencing OTL after reset\n");
			npu2_write(dev->npu, NPU2_MISC_FENCE_STATE,
				   PPC_BIT(dev->brick_index));
			set_fence_control(chip_id, dev->npu->xscom_base,
					  dev->brick_index, 0b00);
			dev->train_fenced = false;
		}

		set_init_pattern(chip_id, dev);
		pci_slot_set_state(slot,
				OCAPI_SLOT_FRESET_INIT_DELAY);
		return pci_slot_set_sm_timeout(slot, msecs_to_tb(5));

	case OCAPI_SLOT_FRESET_INIT_DELAY:
		/* Bump lanes - this improves training reliability */
		npu2_opencapi_bump_ui_lane(dev);
		start_training(chip_id, dev);
		slot->retries = OCAPI_LINK_TRAINING_TIMEOUT;
		pci_slot_set_state(slot, OCAPI_SLOT_LINK_START);
		return slot->ops.poll_link(slot);

	default:
		OCAPIERR(dev, "FRESET: unexpected slot state %08x\n",
			slot->state);
	}
	pci_slot_set_state(slot, OCAPI_SLOT_NORMAL);
	return OPAL_HARDWARE;
}

static int64_t npu2_opencapi_hreset(struct pci_slot *slot __unused)
{
	struct npu2_dev *dev = phb_to_npu2_dev_ocapi(slot->phb);

	OCAPIERR(dev, "hreset not supported\n");
	return OPAL_UNSUPPORTED;
}

static struct pci_slot *npu2_opencapi_slot_create(struct phb *phb)
{
	struct pci_slot *slot;

	slot = pci_slot_alloc(phb, NULL);
	if (!slot)
		return slot;

	/* TODO: Figure out other slot functions */
	slot->ops.get_presence_state  = npu2_opencapi_get_presence_state;
	slot->ops.get_link_state      = npu2_opencapi_get_link_state;
	slot->ops.get_power_state     = NULL;
	slot->ops.get_attention_state = NULL;
	slot->ops.get_latch_state     = NULL;
	slot->ops.set_power_state     = NULL;
	slot->ops.set_attention_state = NULL;

	slot->ops.poll_link           = npu2_opencapi_poll_link;
	slot->ops.creset              = npu2_opencapi_creset;
	slot->ops.freset              = npu2_opencapi_freset;
	slot->ops.hreset              = npu2_opencapi_hreset;

	return slot;
}

static int64_t npu2_opencapi_pcicfg_check(struct npu2_dev *dev, uint32_t offset,
					  uint32_t size)
{
	if (!dev || offset > 0xfff || (offset & (size - 1)))
		return OPAL_PARAMETER;

	return OPAL_SUCCESS;
}

static int64_t npu2_opencapi_pcicfg_read(struct phb *phb, uint32_t bdfn,
					 uint32_t offset, uint32_t size,
					 void *data)
{
	uint64_t cfg_addr;
	struct npu2_dev *dev = phb_to_npu2_dev_ocapi(phb);
	uint64_t genid_base;
	int64_t rc;

	rc = npu2_opencapi_pcicfg_check(dev, offset, size);
	if (rc)
		return rc;

	genid_base = dev->bars[1].base +
		(index_to_block(dev->brick_index) == NPU2_BLOCK_OTL1 ? 256 : 0);

	cfg_addr = NPU2_CQ_CTL_CONFIG_ADDR_ENABLE;
	cfg_addr = SETFIELD(NPU2_CQ_CTL_CONFIG_ADDR_BUS_NUMBER |
			    NPU2_CQ_CTL_CONFIG_ADDR_DEVICE_NUMBER |
			    NPU2_CQ_CTL_CONFIG_ADDR_FUNCTION_NUMBER,
			    cfg_addr, bdfn);
	cfg_addr = SETFIELD(NPU2_CQ_CTL_CONFIG_ADDR_REGISTER_NUMBER,
			    cfg_addr, offset & ~3u);

	out_be64((uint64_t *)genid_base, cfg_addr);
	sync();

	switch (size) {
	case 1:
		*((uint8_t *)data) =
			in_8((volatile uint8_t *)(genid_base + 128 + (offset & 3)));
		break;
	case 2:
		*((uint16_t *)data) =
			in_le16((volatile uint16_t *)(genid_base + 128 + (offset & 2)));
		break;
	case 4:
		*((uint32_t *)data) = in_le32((volatile uint32_t *)(genid_base + 128));
		break;
	default:
		return OPAL_PARAMETER;
	}

	return OPAL_SUCCESS;
}

#define NPU2_OPENCAPI_PCI_CFG_READ(size, type)				\
static int64_t npu2_opencapi_pcicfg_read##size(struct phb *phb,		\
					       uint32_t bdfn,		\
					       uint32_t offset,		\
					       type *data)		\
{									\
	/* Initialize data in case of error */				\
	*data = (type)0xffffffff;					\
	return npu2_opencapi_pcicfg_read(phb, bdfn, offset,		\
					 sizeof(type), data);		\
}

static int64_t npu2_opencapi_pcicfg_write(struct phb *phb, uint32_t bdfn,
					  uint32_t offset, uint32_t size,
					  uint32_t data)
{
	uint64_t cfg_addr;
	struct npu2_dev *dev = phb_to_npu2_dev_ocapi(phb);
	uint64_t genid_base;
	int64_t rc;

	rc = npu2_opencapi_pcicfg_check(dev, offset, size);
	if (rc)
		return rc;

	genid_base = dev->bars[1].base +
		(index_to_block(dev->brick_index) == NPU2_BLOCK_OTL1 ? 256 : 0);

	cfg_addr = NPU2_CQ_CTL_CONFIG_ADDR_ENABLE;
	cfg_addr = SETFIELD(NPU2_CQ_CTL_CONFIG_ADDR_BUS_NUMBER |
			    NPU2_CQ_CTL_CONFIG_ADDR_DEVICE_NUMBER |
			    NPU2_CQ_CTL_CONFIG_ADDR_FUNCTION_NUMBER,
			    cfg_addr, bdfn);
	cfg_addr = SETFIELD(NPU2_CQ_CTL_CONFIG_ADDR_REGISTER_NUMBER,
			    cfg_addr, offset & ~3u);

	out_be64((uint64_t *)genid_base, cfg_addr);
	sync();

	switch (size) {
	case 1:
		out_8((volatile uint8_t *)(genid_base + 128 + (offset & 3)),
		      data);
		break;
	case 2:
		out_le16((volatile uint16_t *)(genid_base + 128 + (offset & 2)),
					       data);
		break;
	case 4:
		out_le32((volatile uint32_t *)(genid_base + 128), data);
		break;
	default:
		return OPAL_PARAMETER;
	}

	return OPAL_SUCCESS;
}

#define NPU2_OPENCAPI_PCI_CFG_WRITE(size, type)				\
static int64_t npu2_opencapi_pcicfg_write##size(struct phb *phb,	\
						uint32_t bdfn,		\
						uint32_t offset,	\
						type data)		\
{									\
	return npu2_opencapi_pcicfg_write(phb, bdfn, offset,		\
					  sizeof(type), data);		\
}

NPU2_OPENCAPI_PCI_CFG_READ(8, u8)
NPU2_OPENCAPI_PCI_CFG_READ(16, u16)
NPU2_OPENCAPI_PCI_CFG_READ(32, u32)
NPU2_OPENCAPI_PCI_CFG_WRITE(8, u8)
NPU2_OPENCAPI_PCI_CFG_WRITE(16, u16)
NPU2_OPENCAPI_PCI_CFG_WRITE(32, u32)

static int64_t npu2_opencapi_ioda_reset(struct phb __unused *phb,
				    bool __unused purge)
{
	/* Not relevant to OpenCAPI - we do this just to silence the error */
	return OPAL_SUCCESS;
}

static int64_t npu2_opencapi_set_pe(struct phb *phb,
				    uint64_t pe_num,
				    uint64_t bdfn,
				    uint8_t bcompare,
				    uint8_t dcompare,
				    uint8_t fcompare,
				    uint8_t action)
{
	struct npu2 *p;
	struct npu2_dev *dev;
	uint64_t reg, val, pe_bdfn;

	/* Sanity check */
	if (action != OPAL_MAP_PE && action != OPAL_UNMAP_PE)
		return OPAL_PARAMETER;
	if (pe_num >= NPU2_MAX_PE_NUM)
		return OPAL_PARAMETER;
	if (bdfn >> 8)
		return OPAL_PARAMETER;
	if (bcompare != OpalPciBusAll ||
	    dcompare != OPAL_COMPARE_RID_DEVICE_NUMBER ||
	    fcompare != OPAL_COMPARE_RID_FUNCTION_NUMBER)
		return OPAL_UNSUPPORTED;

	/* Get the NPU2 device */
	dev = phb_to_npu2_dev_ocapi(phb);
	if (!dev)
		return OPAL_PARAMETER;

	p = dev->npu;

	pe_bdfn = dev->bdfn;

	val = NPU2_MISC_BRICK_BDF2PE_MAP_ENABLE;
	val = SETFIELD(NPU2_MISC_BRICK_BDF2PE_MAP_PE, val, pe_num);
	val = SETFIELD(NPU2_MISC_BRICK_BDF2PE_MAP_BDF, val, pe_bdfn);
	reg = NPU2_REG_OFFSET(NPU2_STACK_MISC, NPU2_BLOCK_MISC,
			      NPU2_MISC_BRICK0_BDF2PE_MAP0 +
			      (dev->brick_index * 0x18));
	npu2_write(p, reg, val);

	return OPAL_SUCCESS;
}

static int npu2_add_mmio_regs(struct phb *phb, struct pci_device *pd,
			      void *data __unused)
{
	uint32_t irq;
	struct npu2_dev *dev = phb_to_npu2_dev_ocapi(phb);
	uint64_t block = index_to_block(dev->brick_index);
	uint64_t stacku = index_to_stacku(dev->brick_index);
	uint64_t dsisr, dar, tfc, handle;

	/*
	 * Pass the hw irq number for the translation fault irq
	 * irq levels 23 -> 26 are for translation faults, 1 per brick
	 */
	irq = dev->npu->irq_base + NPU_IRQ_LEVELS_XSL;
	if (stacku == NPU2_STACK_STCK_2U)
		irq += 2;
	if (block == NPU2_BLOCK_OTL1)
		irq++;

	/*
	 * Add the addresses of the registers needed by the OS to handle
	 * faults. The OS accesses them by mmio.
	 */
	dsisr  = (uint64_t) dev->npu->regs + NPU2_OTL_OSL_DSISR(stacku, block);
	dar    = (uint64_t) dev->npu->regs + NPU2_OTL_OSL_DAR(stacku, block);
	tfc    = (uint64_t) dev->npu->regs + NPU2_OTL_OSL_TFC(stacku, block);
	handle = (uint64_t) dev->npu->regs + NPU2_OTL_OSL_PEHANDLE(stacku,
								   block);
	dt_add_property_cells(pd->dn, "ibm,opal-xsl-irq", irq);
	dt_add_property_cells(pd->dn, "ibm,opal-xsl-mmio",
			hi32(dsisr), lo32(dsisr),
			hi32(dar), lo32(dar),
			hi32(tfc), lo32(tfc),
			hi32(handle), lo32(handle));
	return 0;
}

static void npu2_opencapi_final_fixup(struct phb *phb)
{
	pci_walk_dev(phb, NULL, npu2_add_mmio_regs, NULL);
}

static void mask_nvlink_fir(struct npu2 *p)
{
	uint64_t reg;

	/*
	 * From section 13.1.3.10 of the NPU workbook: "the NV-Link
	 * Datalink Layer Stall and NoStall signals are used for a
	 * different purpose when the link is configured for
	 * OpenCAPI. Therefore, the corresponding bits in NPU FIR
	 * Register 1 must be masked and configured to NOT cause the
	 * NPU to go into Freeze or Fence mode or send an Interrupt."
	 *
	 * FIXME: will need to revisit when mixing nvlink with
	 * opencapi. Assumes an opencapi-only setup on both PHYs for
	 * now.
	 */

	/* Mask FIRs */
	xscom_read(p->chip_id, p->xscom_base + NPU2_MISC_FIR_MASK1, &reg);
	reg = SETFIELD(PPC_BITMASK(0, 11), reg, 0xFFF);
	xscom_write(p->chip_id, p->xscom_base + NPU2_MISC_FIR_MASK1, reg);

	/* freeze disable */
	reg = npu2_scom_read(p->chip_id, p->xscom_base,
			NPU2_MISC_FREEZE_ENABLE1, NPU2_MISC_DA_LEN_8B);
	reg = SETFIELD(PPC_BITMASK(0, 11), reg, 0);
	npu2_scom_write(p->chip_id, p->xscom_base,
			NPU2_MISC_FREEZE_ENABLE1, NPU2_MISC_DA_LEN_8B, reg);

	/* fence disable */
	reg = npu2_scom_read(p->chip_id, p->xscom_base,
			NPU2_MISC_FENCE_ENABLE1, NPU2_MISC_DA_LEN_8B);
	reg = SETFIELD(PPC_BITMASK(0, 11), reg, 0);
	npu2_scom_write(p->chip_id, p->xscom_base,
			NPU2_MISC_FENCE_ENABLE1, NPU2_MISC_DA_LEN_8B, reg);

	/* irq disable */
	reg = npu2_scom_read(p->chip_id, p->xscom_base,
			NPU2_MISC_IRQ_ENABLE1, NPU2_MISC_DA_LEN_8B);
	reg = SETFIELD(PPC_BITMASK(0, 11), reg, 0);
	npu2_scom_write(p->chip_id, p->xscom_base,
			NPU2_MISC_IRQ_ENABLE1, NPU2_MISC_DA_LEN_8B, reg);
}

static int setup_irq(struct npu2 *p)
{
	uint64_t reg, mmio_addr;
	uint32_t base;

	base = xive_alloc_ipi_irqs(p->chip_id, NPU_IRQ_LEVELS, 64);
	if (base == XIVE_IRQ_ERROR) {
		/**
		 * @fwts-label OCAPIIRQAllocationFailed
		 * @fwts-advice OpenCAPI IRQ setup failed. This is probably
		 * a firmware bug. OpenCAPI functionality will be broken.
		 */
		prlog(PR_ERR, "OCAPI: Couldn't allocate interrupts for NPU\n");
		return -1;
	}
	p->irq_base = base;

	xive_register_ipi_source(base, NPU_IRQ_LEVELS, NULL, NULL);
	mmio_addr = (uint64_t ) xive_get_trigger_port(base);
	prlog(PR_DEBUG, "OCAPI: NPU base irq %d @%llx\n", base, mmio_addr);
	reg = (mmio_addr & NPU2_MISC_IRQ_BASE_MASK) << 13;
	npu2_scom_write(p->chip_id, p->xscom_base, NPU2_MISC_IRQ_BASE,
			NPU2_MISC_DA_LEN_8B, reg);
	/*
	 * setup page size = 64k
	 *
	 * OS type is set to AIX: opal also runs with 2 pages per interrupt,
	 * so to cover the max offset for 35 levels of interrupt, we need
	 * bits 41 to 46, which is what the AIX setting does. There's no
	 * other meaning for that AIX setting.
	 */
	reg = npu2_scom_read(p->chip_id, p->xscom_base, NPU2_MISC_CFG,
			NPU2_MISC_DA_LEN_8B);
	reg |= NPU2_MISC_CFG_IPI_PS;
	reg &= ~NPU2_MISC_CFG_IPI_OS;
	npu2_scom_write(p->chip_id, p->xscom_base, NPU2_MISC_CFG,
			NPU2_MISC_DA_LEN_8B, reg);

	/* enable translation interrupts for all bricks */
	reg = npu2_scom_read(p->chip_id, p->xscom_base, NPU2_MISC_IRQ_ENABLE2,
			     NPU2_MISC_DA_LEN_8B);
	reg |= PPC_BIT(0) | PPC_BIT(1) | PPC_BIT(2) | PPC_BIT(3);
	npu2_scom_write(p->chip_id, p->xscom_base, NPU2_MISC_IRQ_ENABLE2,
			NPU2_MISC_DA_LEN_8B, reg);

	mask_nvlink_fir(p);
	return 0;
}

static void setup_debug_training_state(struct npu2_dev *dev)
{
	npu2_opencapi_phy_setup(dev);

	switch (npu2_ocapi_training_state) {
	case NPU2_TRAIN_PRBS31:
		OCAPIINF(dev, "sending PRBS31 pattern per NVRAM setting\n");
		npu2_opencapi_phy_prbs31(dev);
		break;

	case NPU2_TRAIN_NONE:
		OCAPIINF(dev, "link not trained per NVRAM setting\n");
		break;
	default:
		assert(false);
	}
}

static void setup_device(struct npu2_dev *dev)
{
	struct dt_node *dn_phb;
	struct pci_slot *slot;
	uint64_t mm_win[2];

	/* Populate PHB device node */
	phys_map_get(dev->npu->chip_id, NPU_OCAPI_MMIO, dev->brick_index, &mm_win[0],
		     &mm_win[1]);
	prlog(PR_DEBUG, "OCAPI: Setting MMIO window to %016llx + %016llx\n",
	      mm_win[0], mm_win[1]);
	dn_phb = dt_new_addr(dt_root, "pciex", mm_win[0]);
	assert(dn_phb);
	dt_add_property_strings(dn_phb,
				"compatible",
				"ibm,power9-npu-opencapi-pciex",
				"ibm,ioda2-npu2-opencapi-phb");

	dt_add_property_cells(dn_phb, "#address-cells", 3);
	dt_add_property_cells(dn_phb, "#size-cells", 2);
	dt_add_property_cells(dn_phb, "#interrupt-cells", 1);
	dt_add_property_cells(dn_phb, "bus-range", 0, 0xff);
	dt_add_property_cells(dn_phb, "clock-frequency", 0x200, 0);
        dt_add_property_cells(dn_phb, "interrupt-parent", get_ics_phandle());

	dt_add_property_strings(dn_phb, "device_type", "pciex");
	dt_add_property(dn_phb, "reg", mm_win, sizeof(mm_win));
	dt_add_property_cells(dn_phb, "ibm,npu-index", dev->npu->index);
	dt_add_property_cells(dn_phb, "ibm,chip-id", dev->npu->chip_id);
	dt_add_property_cells(dn_phb, "ibm,xscom-base", dev->npu->xscom_base);
	dt_add_property_cells(dn_phb, "ibm,npcq", dev->npu->dt_node->phandle);
	dt_add_property_cells(dn_phb, "ibm,links", 1);
	dt_add_property(dn_phb, "ibm,mmio-window", mm_win, sizeof(mm_win));
	dt_add_property_cells(dn_phb, "ibm,phb-diag-data-size", 0);
	dt_add_property_cells(dn_phb, "ibm,opal-num-pes", NPU2_MAX_PE_NUM);

	dt_add_property_cells(dn_phb, "ranges", 0x02000000,
			      hi32(mm_win[0]), lo32(mm_win[0]),
			      hi32(mm_win[0]), lo32(mm_win[0]),
			      hi32(mm_win[1]), lo32(mm_win[1]));

	dev->phb_ocapi.dt_node = dn_phb;
	dev->phb_ocapi.ops = &npu2_opencapi_ops;
	dev->phb_ocapi.phb_type = phb_type_npu_v2_opencapi;
	dev->phb_ocapi.scan_map = 0;

	dev->bdfn = 0;
	dev->train_need_fence = false;
	dev->train_fenced = false;

	/* TODO: Procedure 13.1.3.7 - AFU Memory Range BARs */
	/* Procedure 13.1.3.8 - AFU MMIO Range BARs */
	setup_afu_mmio_bars(dev->npu->chip_id, dev->npu->xscom_base, dev);
	/* Procedure 13.1.3.9 - AFU Config BARs */
	setup_afu_config_bars(dev->npu->chip_id, dev->npu->xscom_base, dev);
	setup_perf_counters(dev);

	set_fence_control(dev->npu->chip_id, dev->npu->xscom_base, dev->brick_index, 0b00);

	if (npu2_ocapi_training_state != NPU2_TRAIN_DEFAULT) {
		setup_debug_training_state(dev);
	} else {
		slot = npu2_opencapi_slot_create(&dev->phb_ocapi);
		if (!slot) {
			/**
			 * @fwts-label OCAPICannotCreatePHBSlot
			 * @fwts-advice Firmware probably ran out of memory creating
			 * NPU slot. OpenCAPI functionality could be broken.
			 */
			prlog(PR_ERR, "OCAPI: Cannot create PHB slot\n");
		}
	}
	pci_register_phb(&dev->phb_ocapi, OPAL_DYNAMIC_PHB_ID);
	return;
}

static void read_nvram_training_state(void)
{
	const char *state;

	state = nvram_query("opencapi-link-training");
	if (state) {
		if (!strcmp(state, "prbs31"))
			npu2_ocapi_training_state = NPU2_TRAIN_PRBS31;
		else if (!strcmp(state, "none"))
			npu2_ocapi_training_state = NPU2_TRAIN_NONE;
		else
			prlog(PR_WARNING,
			      "OCAPI: invalid training state in NVRAM: %s\n",
			      state);
	}
}

int npu2_opencapi_init_npu(struct npu2 *npu)
{
	struct npu2_dev *dev;
	uint64_t reg[2];
	int rc;

	assert(platform.ocapi);
	read_nvram_training_state();

	/* TODO: Test OpenCAPI with fast reboot and make it work */
	disable_fast_reboot("OpenCAPI device enabled");

	setup_global_mmio_bar(npu->chip_id, npu->xscom_base, reg);

	npu->regs = (void *)reg[0];

	for (int i = 0; i < npu->total_devices; i++) {
		dev = &npu->devices[i];
		if (dev->type != NPU2_DEV_TYPE_OPENCAPI)
			continue;

		prlog(PR_INFO, "OCAPI: Configuring link index %d, brick %d\n",
		      dev->link_index, dev->brick_index);

		/* Procedure 13.1.3.1 - Select OCAPI vs NVLink */
		brick_config(npu->chip_id, npu->xscom_base, dev->brick_index);

		/* Procedure 13.1.3.5 - Transaction Layer Configuration */
		tl_config(npu->chip_id, npu->xscom_base, dev->brick_index);

		/* Procedure 13.1.3.6 - Address Translation Configuration */
		address_translation_config(npu->chip_id, npu->xscom_base, dev->brick_index);
	}

	/* Procedure 13.1.3.10 - Interrupt Configuration */
	rc = setup_irq(npu);
	if (rc)
		goto failed;

	for (int i = 0; i < npu->total_devices; i++) {
		dev = &npu->devices[i];
		if (dev->type != NPU2_DEV_TYPE_OPENCAPI)
			continue;
		setup_device(dev);
	}

	return 0;
failed:
	return -1;
}

static const struct phb_ops npu2_opencapi_ops = {
	.cfg_read8		= npu2_opencapi_pcicfg_read8,
	.cfg_read16		= npu2_opencapi_pcicfg_read16,
	.cfg_read32		= npu2_opencapi_pcicfg_read32,
	.cfg_write8		= npu2_opencapi_pcicfg_write8,
	.cfg_write16		= npu2_opencapi_pcicfg_write16,
	.cfg_write32		= npu2_opencapi_pcicfg_write32,
	.choose_bus		= NULL,
	.device_init		= NULL,
	.phb_final_fixup	= npu2_opencapi_final_fixup,
	.ioda_reset		= npu2_opencapi_ioda_reset,
	.papr_errinjct_reset	= NULL,
	.pci_reinit		= NULL,
	.set_phb_mem_window	= NULL,
	.phb_mmio_enable	= NULL,
	.map_pe_mmio_window	= NULL,
	.map_pe_dma_window	= NULL,
	.map_pe_dma_window_real	= NULL,
	.pci_msi_eoi		= NULL,
	.set_xive_pe		= NULL,
	.get_msi_32		= NULL,
	.get_msi_64		= NULL,
	.set_pe			= npu2_opencapi_set_pe,
	.set_peltv		= NULL,
	.eeh_freeze_status	= npu2_freeze_status,  /* TODO */
	.eeh_freeze_clear	= NULL,
	.eeh_freeze_set		= NULL,
	.next_error		= NULL,
	.err_inject		= NULL,
	.get_diag_data		= NULL,
	.get_diag_data2		= NULL,
	.set_capi_mode		= NULL,
	.set_capp_recovery	= NULL,
	.tce_kill		= NULL,
};

static int64_t opal_npu_spa_setup(uint64_t phb_id, uint32_t __unused bdfn,
				uint64_t addr, uint64_t PE_mask)
{
	uint64_t stack, block, offset, reg;
	struct phb *phb = pci_get_phb(phb_id);
	struct npu2_dev *dev;
	int rc;

	if (!phb || phb->phb_type != phb_type_npu_v2_opencapi)
		return OPAL_PARAMETER;

	/* 4k aligned */
	if (addr & 0xFFF)
		return OPAL_PARAMETER;

	if (PE_mask > 15)
		return OPAL_PARAMETER;

	dev = phb_to_npu2_dev_ocapi(phb);
	if (!dev)
		return OPAL_PARAMETER;

	block = index_to_block(dev->brick_index);
	stack = index_to_stack(dev->brick_index);
	if (block == NPU2_BLOCK_OTL1)
		offset = NPU2_XSL_PSL_SPAP_A1;
	else
		offset = NPU2_XSL_PSL_SPAP_A0;


	lock(&dev->npu->lock);
	/*
	 * set the SPAP used by the device
	 */
	reg = npu2_scom_read(dev->npu->chip_id, dev->npu->xscom_base,
			NPU2_REG_OFFSET(stack, NPU2_BLOCK_XSL, offset),
			NPU2_MISC_DA_LEN_8B);
	if ((addr && (reg & NPU2_XSL_PSL_SPAP_EN)) ||
		(!addr && !(reg & NPU2_XSL_PSL_SPAP_EN))) {
		rc = OPAL_BUSY;
		goto out;
	}
	/* SPA is disabled by passing a NULL address */
	reg = addr;
	if (addr)
		reg = addr | NPU2_XSL_PSL_SPAP_EN;

	npu2_scom_write(dev->npu->chip_id, dev->npu->xscom_base,
			NPU2_REG_OFFSET(stack, NPU2_BLOCK_XSL, offset),
			NPU2_MISC_DA_LEN_8B, reg);

	/*
	 * set the PE mask that the OS uses for PASID -> PE handle
	 * conversion
	 */
	reg = npu2_scom_read(dev->npu->chip_id, dev->npu->xscom_base,
			NPU2_OTL_CONFIG0(stack, block), NPU2_MISC_DA_LEN_8B);
	reg &= ~NPU2_OTL_CONFIG0_PE_MASK;
	reg |= (PE_mask << (63-7));
	npu2_scom_write(dev->npu->chip_id, dev->npu->xscom_base,
			NPU2_OTL_CONFIG0(stack, block), NPU2_MISC_DA_LEN_8B,
			reg);
	rc = OPAL_SUCCESS;
out:
	unlock(&dev->npu->lock);
	return rc;
}
opal_call(OPAL_NPU_SPA_SETUP, opal_npu_spa_setup, 4);

static int64_t opal_npu_spa_clear_cache(uint64_t phb_id, uint32_t __unused bdfn,
					uint64_t PE_handle)
{
	uint64_t cc_inv, stack, block, reg, rc;
	uint32_t retries = 5;
	struct phb *phb = pci_get_phb(phb_id);
	struct npu2_dev *dev;

	if (!phb || phb->phb_type != phb_type_npu_v2_opencapi)
		return OPAL_PARAMETER;

	if (PE_handle > MAX_PE_HANDLE)
		return OPAL_PARAMETER;

	dev = phb_to_npu2_dev_ocapi(phb);
	if (!dev)
		return OPAL_PARAMETER;

	block = index_to_block(dev->brick_index);
	stack = index_to_stack(dev->brick_index);
	cc_inv = NPU2_REG_OFFSET(stack, NPU2_BLOCK_XSL, NPU2_XSL_PSL_LLCMD_A0);

	lock(&dev->npu->lock);
	reg = npu2_scom_read(dev->npu->chip_id, dev->npu->xscom_base, cc_inv,
			NPU2_MISC_DA_LEN_8B);
	if (reg & PPC_BIT(16)) {
		rc = OPAL_BUSY;
		goto out;
	}

	reg = PE_handle | PPC_BIT(15);
	if (block == NPU2_BLOCK_OTL1)
		reg |= PPC_BIT(48);
	npu2_scom_write(dev->npu->chip_id, dev->npu->xscom_base, cc_inv,
			NPU2_MISC_DA_LEN_8B, reg);

	rc = OPAL_HARDWARE;
	while (retries--) {
		reg = npu2_scom_read(dev->npu->chip_id, dev->npu->xscom_base,
				     cc_inv, NPU2_MISC_DA_LEN_8B);
		if (!(reg & PPC_BIT(16))) {
			rc = OPAL_SUCCESS;
			break;
		}
		/* the bit expected to flip in less than 200us */
		time_wait_us(200);
	}
out:
	unlock(&dev->npu->lock);
	return rc;
}
opal_call(OPAL_NPU_SPA_CLEAR_CACHE, opal_npu_spa_clear_cache, 3);

static int get_template_rate(unsigned int templ, char *rate_buf)
{
	int shift, idx, val;

	/*
	 * Each rate is encoded over 4 bits (0->15), with 15 being the
	 * slowest. The buffer is a succession of rates for all the
	 * templates. The first 4 bits are for template 63, followed
	 * by 4 bits for template 62, ... etc. So the rate for
	 * template 0 is at the very end of the buffer.
	 */
	idx = (TL_MAX_TEMPLATE - templ) / 2;
	shift = 4 * (1 - ((TL_MAX_TEMPLATE - templ) % 2));
	val = rate_buf[idx] >> shift;
	return val;
}

static bool is_template_supported(unsigned int templ, long capabilities)
{
	return !!(capabilities & (1ull << templ));
}

static int64_t opal_npu_tl_set(uint64_t phb_id, uint32_t __unused bdfn,
			long capabilities, uint64_t rate_phys, int rate_sz)
{
	struct phb *phb = pci_get_phb(phb_id);
	struct npu2_dev *dev;
	uint64_t stack, block, reg, templ_rate;
	int i, rate_pos;
	char *rate = (char *) rate_phys;

	if (!phb || phb->phb_type != phb_type_npu_v2_opencapi)
		return OPAL_PARAMETER;
	if (!opal_addr_valid(rate) || rate_sz != TL_RATE_BUF_SIZE)
		return OPAL_PARAMETER;

	dev = phb_to_npu2_dev_ocapi(phb);
	if (!dev)
		return OPAL_PARAMETER;

	block = index_to_block(dev->brick_index);
	stack = index_to_stack(dev->brick_index);
	/*
	 * The 'capabilities' argument defines what TL template the
	 * device can receive. OpenCAPI 3.0 and 4.0 define 64 templates, so
	 * that's one bit per template.
	 *
	 * For each template, the device processing time may vary, so
	 * the device advertises at what rate a message of a given
	 * template can be sent. That's encoded in the 'rate' buffer.
	 *
	 * On P9, NPU only knows about TL templates 0 -> 3.
	 * Per the spec, template 0 must be supported.
	 */
	if (!is_template_supported(0, capabilities))
		return OPAL_PARAMETER;

	reg = npu2_scom_read(dev->npu->chip_id, dev->npu->xscom_base,
			     NPU2_OTL_CONFIG1(stack, block),
			     NPU2_MISC_DA_LEN_8B);
	reg &= ~(NPU2_OTL_CONFIG1_TX_TEMP1_EN | NPU2_OTL_CONFIG1_TX_TEMP2_EN |
		 NPU2_OTL_CONFIG1_TX_TEMP3_EN);
	for (i = 0; i < 4; i++) {
		/* Skip template 0 as it is implicitly enabled */
		if (i && is_template_supported(i, capabilities))
			reg |= PPC_BIT(i);
		/* The tx rate should still be set for template 0 */
		templ_rate = get_template_rate(i, rate);
		rate_pos = 8 + i * 4;
		reg = SETFIELD(PPC_BITMASK(rate_pos, rate_pos + 3), reg,
			       templ_rate);
	}
	npu2_scom_write(dev->npu->chip_id, dev->npu->xscom_base,
			NPU2_OTL_CONFIG1(stack, block), NPU2_MISC_DA_LEN_8B,
			reg);
	OCAPIDBG(dev, "OTL configuration 1 register set to %llx\n", reg);
	return OPAL_SUCCESS;
}
opal_call(OPAL_NPU_TL_SET, opal_npu_tl_set, 5);

/* Copyright 2013-2017 IBM Corp.
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
#include <io.h>
#include <timebase.h>
#include <pci.h>
#include <pci-cfg.h>
#include <pci-slot.h>
#include <interrupts.h>
#include <opal.h>
#include <opal-api.h>
#include <npu2-opencapi.h>
#include <npu2.h>
#include <npu2-regs.h>
#include <phys-map.h>
#include <xive.h>
#include <p9-adu.h>
#include <i2c.h>

#define MAX_PE_HANDLE		((1 << 15) - 1)
#define NPU_IRQ_LEVELS		35
#define NPU_IRQ_LEVELS_XSL	23
#define TL_MAX_TEMPLATE		63
#define TL_RATE_BUF_SIZE	32

extern bool simics;

static const struct phb_ops npu2_opencapi_ops;

// TODO: merge with nvlink equivalent
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

static uint64_t get_odl_status(uint32_t gcid, uint64_t index) {
	uint64_t reg, status_xscom;
	switch (index) {
	case 2:
		status_xscom = OB0_ODL0_STATUS;
		break;
	case 3:
		status_xscom = OB0_ODL1_STATUS;
		break;
	case 4:
		status_xscom = OB3_ODL0_STATUS;
		break;
	case 5:
		status_xscom = OB3_ODL1_STATUS;
		break;
	default:
		assert(false);
	}
	xscom_read(gcid, status_xscom, &reg);
	return reg;
}

/* Procedure 13.1.3.1 - select OCAPI vs NVLink for bricks 2-3/4-5 */

/*
 * set_transport_mux_controls() - set Transport MUX controls to select correct
 *   OTL or NTL
 */
static void set_transport_mux_controls(uint32_t gcid, uint32_t scom_base,
				       int index, enum npu2_dev_type type)
{
	uint64_t reg;
	uint64_t field;

	/* TODO: Rework this to select for NVLink too */
	assert(type == NPU2_DEV_TYPE_OPENCAPI);

	prlog(PR_DEBUG, "OCAPI: %s: Setting transport mux controls\n", __func__);
	// Optical IO Transport Mux Config for Bricks 0-2 and 4-5
	reg = npu2_scom_read(gcid, scom_base, NPU2_MISC_OPTICAL_IO_CFG0,
			     NPU2_MISC_DA_LEN_8B);
	switch (index) {
	case 0:
	case 1:
		/* not valid for OpenCAPI */
		assert(false);
		break;
	case 2:  /* OTL1.0 */
		field = GETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_NDLMUX_BRK0TO2, reg);
		field &= ~0b100;
		reg = SETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_NDLMUX_BRK0TO2, reg,
			       field);
		field = GETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_OCMUX_BRK0TO1, reg);
		field |= 0b10;
		reg = SETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_OCMUX_BRK0TO1, reg,
			       field);
		break;
	case 3:  /* OTL1.1 */
		field = GETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_NDLMUX_BRK0TO2, reg);
		field &= ~0b010;
		reg = SETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_NDLMUX_BRK0TO2, reg,
			       field);
		field = GETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_OCMUX_BRK0TO1, reg);
		field |= 0b01;
		reg = SETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_OCMUX_BRK0TO1, reg,
			       field);
		break;
	case 4:  /* OTL2.0 */
		field = GETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_OCMUX_BRK4TO5, reg);
		field |= 0b10;
		reg = SETFIELD(NPU2_MISC_OPTICAL_IO_CFG0_OCMUX_BRK4TO5, reg,
			       field);
		break;
	case 5:  /* OTL2.1 */
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

	// PowerBus Optical Miscellaneous Config Register - select
	// OpenCAPI for b4/5 and A-Link for b3
	xscom_read(gcid, PU_IOE_PB_MISC_CFG, &reg);
	//reg = SETFIELD(PU_IOE_PB_MISC_CFG_SEL_03_NPU_NOT_PB, reg, 0);
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

static void enable_odl_phy_mux(uint32_t gcid, int index)
{
	uint64_t reg;
	uint64_t phy_config_scom;
	prlog(PR_DEBUG, "OCAPI: %s: Enabling ODL to PHY MUXes\n", __func__);
	// 2 - Enable MUXes for ODL to PHY connection
	// TODO: This step must take place at least 10 cycles after step 1. will this be a problem for us?
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
	// PowerBus OLL PHY Training Config Register - OB0
	xscom_read(gcid, phy_config_scom, &reg);
	// enable ODLs to use shared PHYs.
	//TODO: It should be fine to just enable both ODL0 and ODL1 here. Test anyway
	reg |= OBUS_IOOL_PHY_CONFIG_ODL0_ENABLED;
	reg |= OBUS_IOOL_PHY_CONFIG_ODL1_ENABLED;
	// swap ODL1 to use brick 2 lanes instead of brick 1 lanes if using a 22-pin cable for OpenCAPI connection
	reg |= OBUS_IOOL_PHY_CONFIG_ODL_PHY_SWAP;
	// disable A-Link Link Layers
	reg &= ~OBUS_IOOL_PHY_CONFIG_LINK0_OLL_ENABLED;
	reg &= ~OBUS_IOOL_PHY_CONFIG_LINK1_OLL_ENABLED;
	// disable NV-Link Link layers
	reg &= ~OBUS_IOOL_PHY_CONFIG_NV0_NPU_ENABLED;
	reg &= ~OBUS_IOOL_PHY_CONFIG_NV1_NPU_ENABLED;
	reg &= ~OBUS_IOOL_PHY_CONFIG_NV2_NPU_ENABLED;
	xscom_write(gcid, phy_config_scom, reg);
}

static void disable_alink_fp(uint32_t gcid)
{
	uint64_t reg = 0;

	prlog(PR_DEBUG, "OCAPI: %s: Disabling A-Link framer/parsers\n", __func__);
	// TODO: This is probably not necessary on an OPAL system!
	// 3 - Disable A-Link framers/parsers

	reg |= PU_IOE_PB_FP_CFG_FP0_FMR_DISABLE;
	reg |= PU_IOE_PB_FP_CFG_FP0_PRS_DISABLE;
	reg |= PU_IOE_PB_FP_CFG_FP1_FMR_DISABLE;
	reg |= PU_IOE_PB_FP_CFG_FP1_PRS_DISABLE;
	xscom_write(gcid, PU_IOE_PB_FP01_CFG, reg);
	xscom_write(gcid, PU_IOE_PB_FP23_CFG, reg);
	xscom_write(gcid, PU_IOE_PB_FP45_CFG, reg);
	xscom_write(gcid, PU_IOE_PB_FP67_CFG, reg);
}

static void set_pb_hp_opencapi(uint32_t gcid, int index)
{
	// 4 - Set PowerBus HotPlug Mode Registers
	uint64_t reg;

	prlog(PR_DEBUG, "OCAPI: %s: Setting PowerBus Hotplug Mode registers\n", __func__);
	// Not supported on simics
	if (simics)
		return;

	// Read current HP Mode
	xscom_read(gcid, PB_WEST_HP_MODE_CURR, &reg);
	switch (index) {
	case 2:
	case 3:
		/* Configure OPT0 as an OpenCAPI link */
		reg = SETFIELD(PPC_BITMASK(32, 33), reg, 0b01);
		break;
	case 4:
	case 5:
		/* Configure OPT3 as an OpenCAPI link */
		reg = SETFIELD(PPC_BITMASK(38, 39), reg, 0b01);
		break;
	default:
		assert(false);
	}

	p9_adu_set_pb_hp_mode(gcid, reg);
}

static void enable_xsl_clocks(uint32_t gcid, uint32_t scom_base, int index)
{
	// 5 - Enable Clocks in XSL
	// xsl_wrap_cfg Register
	// enable XSL clocks

	prlog(PR_DEBUG, "OCAPI: %s: Enable clocks in XSL\n", __func__);

	npu2_scom_write(gcid, scom_base, NPU2_REG_OFFSET(index_to_stack(index),
							 NPU2_BLOCK_XSL,
							 NPU2_XSL_WRAP_CFG),
			NPU2_MISC_DA_LEN_8B, NPU2_XSL_WRAP_CFG_XSLO_CLOCK_ENABLE);
}

#define CQ_CTL_STATUS_TIMEOUT	10 /* milliseconds */

/* TODO: We might just factor out the whole REQUEST_FENCE into this function... */
static int wait_cq_ctl_fence_status(uint32_t gcid, uint32_t scom_base, int index, uint8_t status)
{
	uint64_t reg, field;
	uint8_t field_val;
	uint64_t timeout = mftb() + msecs_to_tb(CQ_CTL_STATUS_TIMEOUT);

	if (simics)
		return OPAL_SUCCESS;

	if (index_to_block(index) == NPU2_BLOCK_OTL0)
		field = NPU2_CQ_CTL_STATUS_BRK0_AM_FENCED;
	else
		field = NPU2_CQ_CTL_STATUS_BRK1_AM_FENCED;

	do {
		reg = npu2_scom_read(gcid, scom_base,
				     NPU2_REG_OFFSET(index_to_stack(index),
						     NPU2_BLOCK_CTL,
						     NPU2_CQ_CTL_STATUS),
				     NPU2_MISC_DA_LEN_8B);
		field_val = GETFIELD(field, reg);
		if (field_val == status)
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
	      index, status, field_val);
	return OPAL_HARDWARE;
}

static void set_npcq_config(uint32_t gcid, uint32_t scom_base, int index)
{
	uint64_t reg, stack, block, fence_control;

	prlog(PR_DEBUG, "OCAPI: %s: Set NPCQ Config\n", __func__);
	// 6 - Set NPCQ configuration
	// CQ_CTL Misc Config Register #0
	stack = index_to_stack(index);
	block = index_to_block(index);

	fence_control = NPU2_REG_OFFSET(stack, NPU2_BLOCK_CTL,
					block == NPU2_BLOCK_OTL0 ?
					NPU2_CQ_CTL_FENCE_CONTROL_0 :
					NPU2_CQ_CTL_FENCE_CONTROL_1);

	// Enable OTL
	npu2_scom_write(gcid, scom_base, NPU2_OTL_CONFIG0(stack, block),
			NPU2_MISC_DA_LEN_8B, NPU2_OTL_CONFIG0_EN);

	reg = SETFIELD(NPU2_CQ_CTL_FENCE_CONTROL_REQUEST_FENCE, 0ull, 0b01);
	npu2_scom_write(gcid, scom_base, fence_control,
			NPU2_MISC_DA_LEN_8B, reg);

	
	reg = npu2_scom_read(gcid, scom_base,
			     NPU2_REG_OFFSET(stack, NPU2_BLOCK_CTL,
					     NPU2_CQ_CTL_MISC_CFG),
			     NPU2_MISC_DA_LEN_8B);
	// set OCAPI mode
	reg |= NPU2_CQ_CTL_MISC_CFG_CONFIG_OCAPI_MODE;
	// Enable OTL0 or OTL1 respectively
	if (block == NPU2_BLOCK_OTL0)
		reg |= NPU2_CQ_CTL_MISC_CFG_CONFIG_OTL0_ENABLE;
	else
		reg |= NPU2_CQ_CTL_MISC_CFG_CONFIG_OTL1_ENABLE;
	npu2_scom_write(gcid, scom_base,
			NPU2_REG_OFFSET(stack, NPU2_BLOCK_CTL,
					NPU2_CQ_CTL_MISC_CFG),
			NPU2_MISC_DA_LEN_8B, reg);

	reg = SETFIELD(NPU2_CQ_CTL_FENCE_CONTROL_REQUEST_FENCE, 0ull, 0b11); // NPU Fenced
	npu2_scom_write(gcid, scom_base, fence_control,
			NPU2_MISC_DA_LEN_8B, reg);

	// Check status
	wait_cq_ctl_fence_status(gcid, scom_base, index, 0b11);

	reg = SETFIELD(NPU2_CQ_CTL_FENCE_CONTROL_REQUEST_FENCE, 0ull, 0b10); // NPU Half Fenced
	npu2_scom_write(gcid, scom_base, fence_control,
			NPU2_MISC_DA_LEN_8B, reg);

	// Check status
	wait_cq_ctl_fence_status(gcid, scom_base, index, 0b10);

	// CQ_DAT Misc Config Register #1
	reg = npu2_scom_read(gcid, scom_base,
			     NPU2_REG_OFFSET(stack, NPU2_BLOCK_DAT,
					     NPU2_CQ_DAT_MISC_CFG),
			     NPU2_MISC_DA_LEN_8B);
	// set OCAPI mode for bricks 2-5
	reg |= NPU2_CQ_DAT_MISC_CFG_CONFIG_OCAPI_MODE;
	npu2_scom_write(gcid, scom_base,
			NPU2_REG_OFFSET(stack, NPU2_BLOCK_DAT,
					NPU2_CQ_DAT_MISC_CFG),
			NPU2_MISC_DA_LEN_8B, reg);

	// CQ_SM Misc Config Register #0
	for (uint64_t block = NPU2_BLOCK_SM_0;
	     block <= NPU2_BLOCK_SM_3; block++) {
		reg = npu2_scom_read(gcid, scom_base,
				     NPU2_REG_OFFSET(stack, block,
						     NPU2_CQ_SM_MISC_CFG0),
				     NPU2_MISC_DA_LEN_8B);
		// set OCAPI mode for bricks 2-5
		reg |= NPU2_CQ_SM_MISC_CFG0_CONFIG_OCAPI_MODE;
		npu2_scom_write(gcid, scom_base,
				NPU2_REG_OFFSET(stack, block,
						NPU2_CQ_SM_MISC_CFG0),
				NPU2_MISC_DA_LEN_8B, reg);
	}
}

static void enable_xsl_xts_interfaces(uint32_t gcid, uint32_t scom_base, int index)
{
	// TODO: Confirm that XSL1 == stack1, XSL2== stack2
	uint64_t reg;

	prlog(PR_DEBUG, "OCAPI: %s: Enable XSL-XTS Interfaces\n", __func__);
	// 7 - Enable XSL-XTS interfaces
	// XTS Config Register
	reg = npu2_scom_read(gcid, scom_base, NPU2_XTS_CFG, NPU2_MISC_DA_LEN_8B);
	// enable XSL-XTS interface
	reg |= NPU2_XTS_CFG_OPENCAPI;
	npu2_scom_write(gcid, scom_base, NPU2_XTS_CFG, NPU2_MISC_DA_LEN_8B, reg);

	// XTS Config2 Register
	reg = npu2_scom_read(gcid, scom_base, NPU2_XTS_CFG2, NPU2_MISC_DA_LEN_8B);
	switch (index_to_stack(index)) {
	case NPU2_STACK_STCK_1:
		// enable XSL1 interface
		reg |= NPU2_XTS_CFG2_XSL1_ENA;
		break;
	case NPU2_STACK_STCK_2:
		// enable XSL2 interface
		reg |= NPU2_XTS_CFG2_XSL2_ENA;
		break;
	}
	npu2_scom_write(gcid, scom_base, NPU2_XTS_CFG2, NPU2_MISC_DA_LEN_8B, reg);
}

static void enable_sm_allocation(uint32_t gcid, uint32_t scom_base, int index)
{
	uint64_t reg;
	int stack = index_to_stack(index);

	prlog(PR_DEBUG, "OCAPI: %s: Enable State Machine Allocation\n", __func__);
	// 8 - Enable state-machine allocation for bricks 2-5
	// Low-Water Marks Registers
	for (uint64_t block = NPU2_BLOCK_SM_0; block <= NPU2_BLOCK_SM_3; block++) {
		reg = npu2_scom_read(gcid, scom_base,
				     NPU2_REG_OFFSET(stack, block,
						     NPU2_LOW_WATER_MARKS),
				     NPU2_MISC_DA_LEN_8B);
		// enable state-machine allocation
		reg |= NPU2_LOW_WATER_MARKS_ENABLE_MACHINE_ALLOC;
		npu2_scom_write(gcid, scom_base,
				NPU2_REG_OFFSET(stack, block,
						NPU2_LOW_WATER_MARKS),
				NPU2_MISC_DA_LEN_8B, reg);
	}
}

static void enable_pb_snooping(uint32_t gcid, uint32_t scom_base, int index)
{
	uint64_t reg;
	int stack = index_to_stack(index);

	prlog(PR_DEBUG, "OCAPI: %s: Enable PowerBus snooping\n", __func__);
	// 9 - Enable PowerBus snooping for bricks 2-5
	// CQ_SM Misc Config Register #0
	for (uint64_t block = NPU2_BLOCK_SM_0; block <= NPU2_BLOCK_SM_3; block++) {
		reg = npu2_scom_read(gcid, scom_base,
				     NPU2_REG_OFFSET(stack, block,
						     NPU2_CQ_SM_MISC_CFG0),
				     NPU2_MISC_DA_LEN_8B);
		// enable powerbus snooping for bricks 2-5
		reg |= NPU2_CQ_SM_MISC_CFG0_CONFIG_ENABLE_PBUS;
		npu2_scom_write(gcid, scom_base,
				NPU2_REG_OFFSET(stack, block,
						NPU2_CQ_SM_MISC_CFG0),
				NPU2_MISC_DA_LEN_8B, reg);
	}
}

static void brick_config(uint32_t gcid, uint32_t scom_base, int index)
{
	set_transport_mux_controls(gcid, scom_base, index, NPU2_DEV_TYPE_OPENCAPI);
	enable_odl_phy_mux(gcid, index);
	disable_alink_fp(gcid);
	set_pb_hp_opencapi(gcid, index);
	enable_xsl_clocks(gcid, scom_base, index);
	set_npcq_config(gcid, scom_base, index);
	enable_xsl_xts_interfaces(gcid, scom_base, index);
	enable_sm_allocation(gcid, scom_base, index);
	enable_pb_snooping(gcid, scom_base, index);
}

// Procedure 13.1.3.5 TL Configuration
static void tl_config(uint32_t gcid, uint32_t scom_base, uint64_t index)
{
	uint64_t reg;
	uint64_t stack = index_to_stack(index);
	uint64_t block = index_to_block(index);

	prlog(PR_DEBUG, "OCAPI: %s: TL Configuration\n", __func__);
	// OTL Config 0 Register
	reg = 0;
	// OTL Enable
	reg |= NPU2_OTL_CONFIG0_EN;
	// Block PE Handle from ERAT Index
	reg |= NPU2_OTL_CONFIG0_BLOCK_PE_HANDLE;
	// OTL Brick ID
	reg = SETFIELD(NPU2_OTL_CONFIG0_BRICKID, reg, index - 2);
	// PE Handle BDF vs PASID bit select mask - ZEROES
	// ERAT Hash 0
	reg = SETFIELD(NPU2_OTL_CONFIG0_ERAT_HASH_0, reg, 0b011001);
	// ERAT Hash 1
	reg = SETFIELD(NPU2_OTL_CONFIG0_ERAT_HASH_1, reg, 0b000111);
	// ERAT Hash 2
	reg = SETFIELD(NPU2_OTL_CONFIG0_ERAT_HASH_2, reg, 0b101100);
	// ERAT Hash 3
	reg = SETFIELD(NPU2_OTL_CONFIG0_ERAT_HASH_3, reg, 0b100110);
	// Array Error Inject Bits - ZEROES
	// Block AFU TID in Wake_Host_Thread - ZEROES
	npu2_scom_write(gcid, scom_base, NPU2_OTL_CONFIG0(stack, block), NPU2_MISC_DA_LEN_8B, reg);

	// OTL Config 1 Register
	reg = 0;
	// Transmit Template 1-3 - ZEROES
	// per fbarrat - we're setting all these to 0 to force template 0,
	// which must be supported by all devices
	// Extra wait cycles for data between TXI and TXO - ZEROES
	// Template 0 Transmit Rate - most conservative - always supported
	reg = SETFIELD(NPU2_OTL_CONFIG1_TX_TEMP0_RATE, reg, 0b1111);
	// Other Template Transmit Rates don't need to be configured since
	// the Templates are disabled. The OS will set it later based
	// on the device.
	// Extra wait cycles TXI-TXO - varied from workbook per JT's sequence
	reg = SETFIELD(NPU2_OTL_CONFIG1_TX_DRDY_WAIT, reg, 0b001);
	// Minimum Frequency to Return TLX Credits to AFU
	reg = SETFIELD(NPU2_OTL_CONFIG1_TX_CRET_FREQ, reg, 0b001);
	// Frequency to add age to Transmit Requests
	reg = SETFIELD(NPU2_OTL_CONFIG1_TX_AGE_FREQ, reg, 0b11000);
	// Response High Priority Threshold
	reg = SETFIELD(NPU2_OTL_CONFIG1_TX_RS2_HPWAIT, reg, 0b011011);
	// 4-slot Request High Priority Threshold
	reg = SETFIELD(NPU2_OTL_CONFIG1_TX_RQ4_HPWAIT, reg, 0b011011);
	// 6-slot Request High Priority
	reg = SETFIELD(NPU2_OTL_CONFIG1_TX_RQ6_HPWAIT, reg, 0b011011);
	// Disable Command Buffer ECC correction - ZEROES
	// Stop the OCAPI Link - ZEROES
	// Stop the OCAPI Link on Uncorrectable Error - ZEROES - DISABLED FOR DEBUGGING PURPOSES
	// Zero Out Template 0 TLX Credits - ZEROES
	// Zero Out Template 1,2,3 TLX Credits - ZEROES
	npu2_scom_write(gcid, scom_base, NPU2_OTL_CONFIG1(stack, block), NPU2_MISC_DA_LEN_8B, reg);

	// TLX Credit Configuration Register
	reg = 0;
	// VC0 credits to send to AFU
	reg = SETFIELD(NPU2_OTL_TLX_CREDITS_VC0_CREDITS, reg, 0x40);
	// VC3 credits to send to AFU
	reg = SETFIELD(NPU2_OTL_TLX_CREDITS_VC3_CREDITS, reg, 0x40);
	// DCP0 credits to send to AFU
	reg = SETFIELD(NPU2_OTL_TLX_CREDITS_DCP0_CREDITS, reg, 0x80);
	// DCP1 credits to send to AFU
	reg = SETFIELD(NPU2_OTL_TLX_CREDITS_DCP1_CREDITS, reg, 0x80);
	npu2_scom_write(gcid, scom_base, NPU2_OTL_TLX_CREDITS(stack, block), NPU2_MISC_DA_LEN_8B, reg);
}

/* Procedure 13.1.3.6 - Address Translation Configuration */
static void address_translation_config(uint32_t gcid, uint32_t scom_base, uint64_t index)
{
	uint64_t reg;
	uint64_t stack = index_to_stack(index);

	prlog(PR_DEBUG, "OCAPI: %s: Address Translation Configuration\n", __func__);
	// PSL_SCNTL_A0 Register
	// ERAT shared between multiple AFUs [TODO: This has some special cases]
	reg = npu2_scom_read(gcid, scom_base,
			     NPU2_REG_OFFSET(stack, NPU2_BLOCK_XSL,
					     NPU2_XSL_PSL_SCNTL_A0),
			     NPU2_MISC_DA_LEN_8B);
	reg &= ~NPU2_XSL_PSL_SCNTL_A0_MULTI_AFU_DIAL;
	npu2_scom_write(gcid, scom_base,
			NPU2_REG_OFFSET(stack, NPU2_BLOCK_XSL,
					NPU2_XSL_PSL_SCNTL_A0),
			NPU2_MISC_DA_LEN_8B, reg);
	
	// XSL_GP Register
	reg = npu2_scom_read(gcid, scom_base,
			     NPU2_REG_OFFSET(stack, NPU2_BLOCK_XSL, NPU2_XSL_GP),
			     NPU2_MISC_DA_LEN_8B);

	// TODO: Page Protection Check Disable - ZEROES
	// TODO: Context Cache Disable - ZEROES
	// TODO: ERAT Cache Disable - ZEROES
	// TODO: PE Handle ERAT Check Disable - ZEROES
	// TODO: Context Valid Check Disable - ZEROES
	// TODO: Context Cache Duplicate Check Disable - ZEROES
	// TODO: ERAT Duplicate Check Disable - ZEROES
	// TODO: ERAT Index Calculated by OTL/XSL - ZEROES
	// TODO: TLB Invalidate Disable - ZEROES
	// TODO: Itag Check Disable - ZEROES
	// TODO: Itag Tracker Disable - ZEROES
	// TODO: Tracker Filter Disable - ZEROES
	// Bloom Filter Disable
	reg |= NPU2_XSL_GP_BLOOM_FILTER_DISABLE;
	npu2_scom_write(gcid, scom_base, NPU2_REG_OFFSET(stack, NPU2_BLOCK_XSL, NPU2_XSL_GP), NPU2_MISC_DA_LEN_8B, reg);

	/*
	 * DD2.0/2.1 EOA Bug. Fixed in DD2.2
	 * XXX: need to skip when running on dd2.2. Need to detect that.
	 */
	reg = 0x32F8000000000001;
	npu2_scom_write(gcid, scom_base,
			NPU2_REG_OFFSET(stack, NPU2_BLOCK_XSL,
					NPU2_XSL_DEF),
			NPU2_MISC_DA_LEN_8B, reg);
}

// TODO: see if this code can be merged with NVLink bar code
static void write_bar(uint32_t gcid, uint32_t scom_base, uint64_t reg,
		uint64_t addr, uint64_t size)
{
	uint64_t val;
	int block;
	switch (NPU2_REG(reg)) {
	case NPU2_PHY_BAR:
		val = SETFIELD(NPU2_PHY_BAR_ADDR, 0ul, addr >> 21);
		val = SETFIELD(NPU2_PHY_BAR_ENABLE, val, 1);
		break;
	case NPU2_NTL0_BAR:
	case NPU2_NTL1_BAR:
		val = SETFIELD(NPU2_NTL_BAR_ADDR, 0ul, addr >> 16);
		val = SETFIELD(NPU2_NTL_BAR_SIZE, val, ilog2(size >> 16));
		val = SETFIELD(NPU2_NTL_BAR_ENABLE, val, 1);
		break;
	case NPU2_GENID_BAR:
		val = SETFIELD(NPU2_GENID_BAR_ADDR, 0ul, addr >> 16);
		val = SETFIELD(NPU2_GENID_BAR_ENABLE, val, 1);
		break;
	default:
		val = 0ul;
	}

	for (block = NPU2_BLOCK_SM_0; block <= NPU2_BLOCK_SM_3; block++) {
		npu2_scom_write(gcid, scom_base, NPU2_REG_OFFSET(0, block, reg),
				NPU2_MISC_DA_LEN_8B, val);
		prlog(PR_DEBUG, "OCAPI: Setting BAR %llx to %llx\n",
		      NPU2_REG_OFFSET(0, block, reg), val);
	}
}

static void setup_global_mmio_bar(uint32_t gcid, uint32_t scom_base,
				  uint64_t reg[])
{
	// TODO: Merge with NVLink bar assignment
	uint64_t addr, size;

	prlog(PR_DEBUG, "OCAPI: patching up PHY0 bar, %s\n", __func__);
	phys_map_get(gcid, NPU_PHY, 0, &addr, &size);
	write_bar(gcid, scom_base,
		  NPU2_REG_OFFSET(NPU2_STACK_STCK_2, 0, NPU2_PHY_BAR),
		addr, size);
	prlog(PR_DEBUG, "OCAPI: patching up PHY1 bar, %s\n", __func__);
	phys_map_get(gcid, NPU_PHY, 1, &addr, &size);
	write_bar(gcid, scom_base,
		  NPU2_REG_OFFSET(NPU2_STACK_STCK_1, 0, NPU2_PHY_BAR),
		addr, size);

	prlog(PR_DEBUG, "OCAPI: setup global mmio, %s\n", __func__);
	phys_map_get(gcid, NPU_REGS, 0, &addr, &size);
	write_bar(gcid, scom_base,
		  NPU2_REG_OFFSET(NPU2_STACK_STCK_0, 0, NPU2_PHY_BAR),
		addr, size);
	reg[0] = addr;
	reg[1] = size;
}

/* Procedure 13.1.3.8 - AFU MMIO Range BARs
 * TODO - merge with NVLink BAR assignment code */
static void setup_afu_mmio_bars(uint32_t gcid, uint32_t scom_base,
				struct npu2_dev *dev)
{
	uint64_t stack = index_to_stack(dev->index);
	uint64_t offset = index_to_block(dev->index) == NPU2_BLOCK_OTL0 ?
		NPU2_NTL0_BAR : NPU2_NTL1_BAR;
	uint64_t pa_offset = index_to_block(dev->index) == NPU2_BLOCK_OTL0 ?
		NPU2_CQ_CTL_MISC_MMIOPA0_CONFIG :
		NPU2_CQ_CTL_MISC_MMIOPA1_CONFIG;
	uint64_t addr, size, reg;

	prlog(PR_DEBUG, "OCAPI: %s: Setup AFU MMIO BARs\n", __func__);
	/* FIXME: This is a temporary hack that works for bricks 2 and 3 only - 
	 * we need to rework how we do MMIO windows completely to properly
	 * support all bricks. */
	phys_map_get(gcid, NPU_OCAPI_MMIO, dev->index == 3 ? 2 : dev->index, &addr, &size);

	prlog(PR_DEBUG, "OCAPI: AFU MMIO set to %llx, size %llx\n", addr, size);
	write_bar(gcid, scom_base, NPU2_REG_OFFSET(stack, 0, offset), addr,
		size);
	dev->bars[0].npu2_bar.base = addr;
	dev->bars[0].npu2_bar.size = size;

	reg = SETFIELD(NPU2_CQ_CTL_MISC_MMIOPA_ADDR, 0ull, addr >> 16);
	reg = SETFIELD(NPU2_CQ_CTL_MISC_MMIOPA_SIZE, reg, ilog2(size >> 16));
	prlog(PR_DEBUG, "OCAPI: PA translation %llx\n", reg);
	npu2_scom_write(gcid, scom_base,
			NPU2_REG_OFFSET(stack, NPU2_BLOCK_CTL,
					pa_offset),
			NPU2_MISC_DA_LEN_8B, reg);
}

/* Procedure 13.1.3.9 - AFU Config BARs
 * TODO - merge with NVLink BAR assignment code */
static void setup_afu_config_bars(uint32_t gcid, uint32_t scom_base,
				  struct npu2_dev *dev)
{
	uint64_t stack = index_to_stack(dev->index);
	int stack_num = stack - NPU2_STACK_STCK_0;
	uint64_t addr, size;

	prlog(PR_DEBUG, "OCAPI: %s: Setup AFU Config BARs\n", __func__);
	phys_map_get(gcid, NPU_GENID, stack_num, &addr, &size);
	prlog(PR_DEBUG, "OCAPI: Assigning GENID BAR: %016llx\n", addr);
	write_bar(gcid, scom_base, NPU2_REG_OFFSET(stack, 0, NPU2_GENID_BAR),
		addr, size);
	dev->bars[1].npu2_bar.base = addr;
	dev->bars[1].npu2_bar.size = size;
}

static void otl_unfence(uint32_t gcid, uint32_t scom_base, uint64_t index)
{
	uint64_t stack = index_to_stack(index);
	uint64_t block = index_to_block(index);
	uint32_t offset;
	uint64_t reg;

	prlog(PR_DEBUG, "OCAPI: %s: Unfencing OTL\n", __func__);
	if (block == NPU2_BLOCK_OTL0)
		offset = NPU2_CQ_CTL_FENCE_CONTROL_0;
	else
		offset = NPU2_CQ_CTL_FENCE_CONTROL_1;
	reg = SETFIELD(NPU2_CQ_CTL_FENCE_CONTROL_REQUEST_FENCE, 0ull, 0b00);
	npu2_scom_write(gcid, scom_base,
			NPU2_REG_OFFSET(stack, NPU2_BLOCK_CTL, offset),
			NPU2_MISC_DA_LEN_8B, reg);
	// TODO: may need status check here
}

static void otl_enabletx(uint32_t gcid, uint32_t scom_base, uint64_t index)
{
	uint64_t stack = index_to_stack(index);
	uint64_t block = index_to_block(index);
	uint64_t reg;
	// OTL Config 2 Register - Moved after link up per JT
	// Transmit Enable

	prlog(PR_DEBUG, "OCAPI: %s: Enabling TX\n", __func__);
	reg = 0;
	reg |= NPU2_OTL_CONFIG2_TX_SEND_EN;
	npu2_scom_write(gcid, scom_base, NPU2_OTL_CONFIG2(stack, block),
			NPU2_MISC_DA_LEN_8B, reg);

	reg = npu2_scom_read(gcid, scom_base, NPU2_OTL_VC_CREDITS(stack, block),
			     NPU2_MISC_DA_LEN_8B);
	prlog(PR_DEBUG, "OCAPI: credit counter: %llx\n", reg);
}

static void reset_ocapi_device_zz(uint32_t gcid);
static int otl_train(uint32_t gcid, uint32_t index, struct npu2_dev *dev)
{
	uint64_t reg, config_xscom;
	int timeout = 3000;
	prlog(PR_DEBUG, "OCAPI: %s: Training OTL\n", __func__);

	switch (index) {
	case 2:
		config_xscom = OB0_ODL0_CONFIG;
		break;
	case 3:
		config_xscom = OB0_ODL1_CONFIG;
		break;
	case 4:
		config_xscom = OB3_ODL0_CONFIG;
		break;
	case 5:
		config_xscom = OB3_ODL1_CONFIG;
		break;
	default:
		assert(false);
	}

	/* reset first */
	//xscom_read(gcid, config_xscom, &reg);
	//reg |= PPC_BIT(0);
	reg = 0x81628F0040600000; /* From JT script - need further work */
	xscom_write(gcid, config_xscom, reg);
	reg &= ~PPC_BIT(0);
	xscom_write(gcid, config_xscom, reg);

	/* TODO: There may be machines which need a different reset mechanism
	 * from ZZ. This should be fine for ZZ and Zaius at least. */
	reset_ocapi_device_zz(gcid);

	// Transmit Pattern A
	reg = 0x01128F0040600000;
	xscom_write(gcid, config_xscom, reg);
	time_wait_ms(1000);

	/* rx_pr_bump_sl_1ui */
	npu2_opencapi_bump_ui_lane(dev);

	/* start training */
	reg = 0x01828F0040600000; // to be spelled out once stable
	xscom_write(gcid, config_xscom, reg);

	do {
		reg = get_odl_status(gcid, index);
		if (GETFIELD(OB_ODL_STATUS_TRAINING_STATE_MACHINE, reg) == 0x7) {
			prlog(PR_DEBUG, "OCAPI: Link %d on chip %u trained in %dms\n",
			      index, gcid, 3000 - timeout);
			return OPAL_SUCCESS;
		}
		time_wait_ms(1);
	} while (timeout--);
	prlog(PR_ERR, "OCAPI: Link %d on chip %u failed to train - link status %016llx\n",
	      index, gcid, reg);
	return OPAL_HARDWARE;
}

static void npu2_opencapi_probe_phb(struct dt_node *dn)
{
        struct dt_node *np, *link;
	char *path;
	const char *link_type;
	uint32_t gcid, index, links, scom_base, phb_index;
	uint64_t addr, size;
	uint64_t reg[2], mm_win[2];
	uint64_t dev_index;

	/* Retrieve chip id */
	path = dt_get_path(dn);
	gcid = dt_get_chip_id(dn);
	index = dt_prop_get_u32(dn, "ibm,npu-index");
	phb_index = dt_prop_get_u32(dn, "ibm,phb-index");
	links = dt_prop_get_u32(dn, "ibm,npu-links");
	prlog(PR_INFO, "Chip %d Found OpenCAPI NPU%d (%d links) at %s\n",
	      gcid, index, links, path);
	free(path);

	/* Retrieve scom base address */
	scom_base = dt_get_address(dn, 0, NULL);
	prlog(PR_INFO, "   SCOM Base:  %08x\n", scom_base);

// TODO
	reset_ocapi_device_zz(gcid);
	
	dt_for_each_compatible(dn, link, "ibm,npu-link") {
		link_type = dt_prop_get_def(link, "ibm,npu-link-type", NULL);
		if (!link_type || !streq(link_type, "opencapi"))
			continue;
		dev_index = dt_prop_get_u32(link, "ibm,npu-link-index");
		prlog(PR_INFO, "OCAPI: Configuring link index %lld\n", dev_index);
		// Procedure 13.1.3.1 - select OCAPI vs NVLink for bricks 2-3/4-5
		brick_config(gcid, scom_base, dev_index);
	
		// TODO: Procedure 13.1.3.2 - Enterprise/Brazos Mode selection (necessary?)

		// TODO: Procedure 13.1.3.3 - Brick Groupings for Error Handling

		// TODO: Procedure 13.1.3.4 - Brick to PE Mapping
		/* TODO: "If multiple links are attached to the same AFU, they
		 * should be assigned to the same PE" - uncertain how to
		 * implement this */

		// Procedure 13.1.3.5 - Transaction Layer Configuration
		// per discussion with fbarrat - assume templates 1-3 are not supported
		tl_config(gcid, scom_base, dev_index);
	
		// Procedure 13.1.3.6 - Address Translation Configuration
		address_translation_config(gcid, scom_base, dev_index);
	}

	// Global MMIO BAR
	setup_global_mmio_bar(gcid, scom_base, reg);
 	
	/* Populate PHB device node - TODO: Double check and verify all the properties here */
	// TODO: double check mmio window
	phys_map_get(gcid, NPU_OCAPI_MMIO, 2, &mm_win[0], NULL);
	phys_map_get(gcid, NPU_OCAPI_MMIO, 5, &addr, &size);
	mm_win[1] = (addr + size) - mm_win[0];
	prlog(PR_DEBUG, "OCAPI: setting mmio window to %016llx + %016llx\n", mm_win[0], mm_win[1]);
	np = dt_new_addr(dt_root, "pciex", reg[0]);
	assert(np);
	dt_add_property_strings(np,
				"compatible",
				"ibm,power9-npu-opencapi-pciex",
				"ibm,ioda2-npu2-opencapi-phb");
	dt_add_property_strings(np, "device_type", "pciex");
	dt_add_property(np, "reg", reg, sizeof(reg));
	dt_add_property_cells(np, "ibm,phb-index", phb_index);
	dt_add_property_cells(np, "ibm,npu-index", index);
	dt_add_property_cells(np, "ibm,chip-id", gcid);
	dt_add_property_cells(np, "ibm,xscom-base", scom_base);
	dt_add_property_cells(np, "ibm,npcq", dn->phandle);
	dt_add_property_cells(np, "ibm,links", links);
	dt_add_property(np, "ibm,mmio-window", mm_win, sizeof(mm_win));
	dt_add_property_cells(np, "ibm,phb-diag-data-size", 0);
}

static int64_t npu2_opencapi_get_link_state(struct pci_slot *slot, uint8_t *val)
{
	struct npu2 *p = phb_to_npu2(slot->phb);
	uint64_t reg;
	int64_t link_width, rc = OPAL_SUCCESS;

	/* TODO: Currently, this only returns link state based on brick 2.
	 * The PCI slot subsystem isn't well adapted for the case of multiple
	 * links behind one "PHB", and so we may need to rethink our design
	 * to get around that. */
	reg = get_odl_status(p->chip_id, 2);
	link_width = (reg & OB_ODL_STATUS_WIDTH_MASK) >>
		OB_ODL_STATUS_WIDTH_SHIFT;
	switch (link_width) {
	case 0b0001:
		*val = OPAL_SHPC_LINK_UP_x4;
		break;
	case 0b0010:
		*val = OPAL_SHPC_LINK_UP_x8;
		break;
	default:
		rc = OPAL_HARDWARE;
	}
	return rc;
}

static struct pci_slot *npu2_opencapi_slot_create(struct phb *phb)
{
	struct pci_slot *slot;

	slot = pci_slot_alloc(phb, NULL);
	if (!slot)
		return slot;

	// TODO: set slot functions
	slot->ops.get_presence_state = NULL;
	slot->ops.get_link_state = npu2_opencapi_get_link_state;
	slot->ops.get_power_state = NULL;
	slot->ops.get_attention_state = NULL;
	slot->ops.get_latch_state     = NULL;
	slot->ops.set_power_state     = NULL;
	slot->ops.set_attention_state = NULL;

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
	/* TODO: Validate error status handling */
        uint64_t cfg_addr;
	struct npu2 *p = phb_to_npu2(phb);
	struct npu2_dev *dev = npu2_bdf_to_dev(p, bdfn);
	uint64_t genid_base;
	int64_t rc;

	rc = npu2_opencapi_pcicfg_check(dev, offset, size);
	if (rc)
		return rc;

	genid_base = dev->bars[1].npu2_bar.base +
		(index_to_block(dev->index) == NPU2_BLOCK_OTL1 ? 256 : 0);

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
        /* TODO: Validate error status handling */

	uint64_t cfg_addr;
	struct npu2 *p = phb_to_npu2(phb);
	struct npu2_dev *dev = npu2_bdf_to_dev(p, bdfn);
	uint64_t genid_base;
	int64_t rc;

	rc = npu2_opencapi_pcicfg_check(dev, offset, size);
	if (rc)
		return rc;

	genid_base = dev->bars[1].npu2_bar.base +
		(index_to_block(dev->index) == NPU2_BLOCK_OTL1 ? 256 : 0);

	cfg_addr = NPU2_CQ_CTL_CONFIG_ADDR_ENABLE;
	cfg_addr = SETFIELD(NPU2_CQ_CTL_CONFIG_ADDR_BUS_NUMBER |
			    NPU2_CQ_CTL_CONFIG_ADDR_DEVICE_NUMBER |
			    NPU2_CQ_CTL_CONFIG_ADDR_FUNCTION_NUMBER,
			    cfg_addr, bdfn);
	cfg_addr = SETFIELD(NPU2_CQ_CTL_CONFIG_ADDR_REGISTER_NUMBER,
			    cfg_addr, offset & ~3u);

	out_be64((uint64_t *)genid_base, cfg_addr);
	sync(); // TODO: needed?

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

static int npu2_add_mmio_regs(struct phb *phb, struct pci_device *pd,
			      void *data __unused)
{
	uint32_t irq;
	struct npu2 *p = phb_to_npu2(phb);
	struct npu2_dev *dev = npu2_bdf_to_dev(p, pd->bdfn);
	uint64_t block = index_to_block(dev->index);
	uint64_t stack = index_to_stack(dev->index);
	uint64_t dsisr, dar, tfc, handle;

	/*
	 * Pass the hw irq number for the translation fault irq
	 * irq levels 23 -> 26 are for translation faults, 1 per brick
	 */
	irq = p->irq_base + NPU_IRQ_LEVELS_XSL;

	/* this is ugly, we'll need to rework our stack numbering */
	stack = stack - 4;

	if (stack == NPU2_STACK_STCK_2)
		irq += 2;
	if (block == NPU2_BLOCK_OTL1)
		irq ++;
	/*
	 * Add the addresses of the registers needed by the OS to handle
	 * faults. The OS accesses them by mmio.
	 */
	dsisr  = (uint64_t) p->regs + NPU2_OTL_OSL_DSISR(stack, block);
	dar    = (uint64_t) p->regs + NPU2_OTL_OSL_DAR(stack, block);
	tfc    = (uint64_t) p->regs + NPU2_OTL_OSL_TFC(stack, block);
	handle = (uint64_t) p->regs + NPU2_OTL_OSL_PEHANDLE(stack, block);
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

static void set_debug_mode(uint32_t gcid, uint32_t scom_base)
{
	uint64_t reg, block;

	/*
	 * fxb this is all temporary and to freeze the adapter so
	 * that it can be debugged
	 */
	prlog(PR_DEBUG, "OCAPI: %s: Enabling debug settings\n", __func__);
	reg = npu2_scom_read(gcid, scom_base,
			NPU2_REG_OFFSET(NPU2_STACK_STCK_1, NPU2_BLOCK_CTL,
					NPU2_CQ_CTL_MISC_CFG1),
			     NPU2_MISC_DA_LEN_8B);
	reg |= PPC_BIT(36) | PPC_BIT(37);
	npu2_scom_write(gcid, scom_base,
			NPU2_REG_OFFSET(NPU2_STACK_STCK_1, NPU2_BLOCK_CTL,
					NPU2_CQ_CTL_MISC_CFG1),
			NPU2_MISC_DA_LEN_8B, reg);

	for (block = NPU2_BLOCK_SM_0; block <= NPU2_BLOCK_SM_3; block++) {
		reg = npu2_scom_read(gcid, scom_base,
				     NPU2_REG_OFFSET(NPU2_STACK_STCK_1, block,
						     NPU2_CQ_SM_MISC_CFG2),
				     NPU2_MISC_DA_LEN_8B);
		reg |= PPC_BIT(3) | PPC_BIT(9) | PPC_BIT(14) | PPC_BIT(15);
		npu2_scom_write(gcid, scom_base,
				NPU2_REG_OFFSET(NPU2_STACK_STCK_1, block,
						NPU2_CQ_SM_MISC_CFG2),
				NPU2_MISC_DA_LEN_8B, reg);
	}

}

static void populate_devices(struct npu2 *p, struct dt_node *dn)
{
	int i = 0;
	int num_links = dt_prop_get_u32(dn, "ibm,links");
	struct dt_node *npu2_dn, *link;
	struct npu2_dev *dev;
	uint32_t npu_phandle;
	uint32_t index;
	const char *link_type;

	npu_phandle = dt_prop_get_u32(dn, "ibm,npcq");
	npu2_dn = dt_find_by_phandle(dt_root, npu_phandle);
	assert(npu2_dn);

	dt_for_each_compatible(npu2_dn, link, "ibm,npu-link") {
		assert(i < num_links);
		link_type = dt_prop_get_def(link, "ibm,npu-link-type", NULL);
		if (!link_type || !streq(link_type, "opencapi"))
			continue;
		index = dt_prop_get_u32(link, "ibm,npu-link-index");
		dev = &p->devices[i];
	        dev->type = NPU2_DEV_TYPE_OPENCAPI;
		dev->npu = p;
		dev->dt_node = link;
		dev->index = index;
		dev->pl_xscom_base = dt_prop_get_u64(link, "ibm,npu-phy");
		dev->lane_mask = dt_prop_get_u32(link, "ibm,npu-lane-mask");
		dev->bdfn = i << 3;
		p->total_devices++;
		p->phb.scan_map |= 1 << i;
		i++;
	}
}

static int setup_devices(struct npu2 *p) {
	struct npu2_dev *dev;
	int i, rc;
	int retries = 20;

	for (i = 0; i < p->total_devices; i++) {
		dev = &p->devices[i];
		// TODO: Procedure 13.1.3.7 - AFU Memory Range BARs
		// Procedure 13.1.3.8 - AFU MMIO Range BARs
		setup_afu_mmio_bars(p->chip_id, p->xscom_base, dev);
		// Procedure 13.1.3.9 - AFU Config BARs
		setup_afu_config_bars(p->chip_id, p->xscom_base, dev);

		// OTL unfence
		otl_unfence(p->chip_id, p->xscom_base, dev->index);

		if (simics)
			prlog(PR_INFO, "OCAPI: Simics detected, skipping PHY setup\n");
		else
			npu2_opencapi_phy_setup(dev);

		do {
			rc = otl_train(p->chip_id, dev->index, dev);
		} while (rc != OPAL_SUCCESS && --retries);

		if (rc != OPAL_SUCCESS && retries == 0) {
			prlog(PR_ERR, "OCAPI: Link failed to train!\n");
			return OPAL_HARDWARE; /* TODO: this function tries all links... */
		}

		// enable TX - per JT
		otl_enabletx(p->chip_id, p->xscom_base, dev->index);
	}
	return OPAL_SUCCESS;
}

static int setup_irq(struct npu2 *p)
{
	uint64_t reg, mmio_addr;
	uint32_t base;

	base = xive_alloc_ipi_irqs(p->chip_id, NPU_IRQ_LEVELS, 64);
	if (base == XIVE_IRQ_ERROR) {
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
	reg = npu2_scom_read(p->chip_id, p->xscom_base,
			NPU2_MISC_IRQ_ON_ERROR_EN_FIR2, NPU2_MISC_DA_LEN_8B);
	reg |= PPC_BIT(0) | PPC_BIT(1) | PPC_BIT(2) | PPC_BIT(3);
	npu2_scom_write(p->chip_id, p->xscom_base,
			NPU2_MISC_IRQ_ON_ERROR_EN_FIR2, NPU2_MISC_DA_LEN_8B,
			reg);
	return 0;
}

static void add_phb_properties(struct npu2 *p)
{
	struct dt_node *np = p->phb.dt_node;
	uint64_t mm_base, mm_size;

	dt_add_property_cells(np, "bus-range", 0, 0xff);
	dt_add_property_cells(np, "ibm,opal-num-pes",
			NPU2_MAX_PE_NUM);
	mm_base = p->mm_base;
	mm_size = p->mm_size;
	// TODO probably totally wrong
	dt_add_property_cells(np, "ranges", 0x02000000, // memory space
			      hi32(mm_base), lo32(mm_base),
			      hi32(mm_base), lo32(mm_base),
			      hi32(mm_size), lo32(mm_size));
}

static void npu2_opencapi_create_phb(struct dt_node *dn)
{
	const struct dt_property *prop;
	struct npu2 *p;
	struct pci_slot *slot;
	uint32_t links, rc;

	links = dt_prop_get_u32(dn, "ibm,links");
	p = zalloc(sizeof(struct npu2) +
		   links * sizeof(struct npu2_dev));
	assert(p);

	p->phb.dt_node = dn;
	p->phb.ops = &npu2_opencapi_ops;
	p->phb.phb_type = phb_type_npu_v2_opencapi;
	p->index = dt_prop_get_u32(dn, "ibm,phb-index");
	p->chip_id = dt_prop_get_u32(dn, "ibm,chip-id");
	p->xscom_base = dt_prop_get_u32(dn, "ibm,xscom-base");
	p->regs = (void *) dt_get_address(dn, 0, NULL);
	p->devices = (struct npu2_dev *)(p + 1);

	prop = dt_require_property(dn, "ibm,mmio-window", -1);
	assert(prop->len >= (2 * sizeof(uint64_t)));
	p->mm_base = ((const uint64_t *)prop->prop)[0];
	p->mm_size = ((const uint64_t *)prop->prop)[1];

	set_debug_mode(p->chip_id, p->xscom_base); /* TODO: Remove */

	rc = setup_irq(p);
	if (rc)
		goto failed;

	add_phb_properties(p);
	populate_devices(p, dn);
	rc = setup_devices(p);
	if (rc)
		goto failed;

	slot = npu2_opencapi_slot_create(&p->phb);
	if (!slot)
	{
		/**
		 * @fwts-label OCAPICannotCreatePHBSlot
		 * @fwts-advice Firmware probably ran out of memory creating
		 * NPU slot. OpenCAPI functionality could be broken.
		 */
		prlog(PR_ERR, "OCAPI: Cannot create PHB slot\n");
	}

	pci_register_phb(&p->phb, OPAL_DYNAMIC_PHB_ID);

	//TODO: npu2_opencapi_init_ioda_cache(p);
	return;
failed:
	dt_add_property_string(dn, "status", "error");
	return;
}

void probe_npu2_opencapi(void)
{
	struct dt_node *np;

	/* Scan NPU XSCOM nodes */
	dt_for_each_compatible(dt_root, np, "ibm,power9-npu")
		npu2_opencapi_probe_phb(np);

	/* Scan newly created PHB nodes */
	dt_for_each_compatible(dt_root, np, "ibm,power9-npu-opencapi-pciex")
		npu2_opencapi_create_phb(np);
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
	.ioda_reset		= NULL,
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
	.set_pe			= npu2_set_pe,
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

static int64_t opal_npu_spa_setup(uint64_t phb_id, uint32_t bdfn,
				uint64_t addr, uint64_t PE_mask)
{
	uint64_t stack, block, offset, reg;
	struct phb *phb = pci_get_phb(phb_id);
	struct npu2 *p = phb_to_npu2(phb);
	struct npu2_dev *dev = npu2_bdf_to_dev(p, bdfn);
	int rc;

	if (!phb || phb->phb_type != phb_type_npu_v2_opencapi || !dev)
		return OPAL_PARAMETER;

	/* 4k aligned */
	if (addr & 0xFFF)
		return OPAL_PARAMETER;

	if (PE_mask > 15)
		return OPAL_PARAMETER;

	block = index_to_block(dev->index);
	stack = index_to_stack(dev->index);
	if (block == NPU2_BLOCK_OTL1)
		offset = NPU2_XSL_PSL_SPAP_A1;
	else
		offset = NPU2_XSL_PSL_SPAP_A0;


	lock(&p->lock);
	/*
	 * set the SPAP used by the device
	 */
	reg = npu2_scom_read(p->chip_id, p->xscom_base,
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

	npu2_scom_write(p->chip_id, p->xscom_base,
			NPU2_REG_OFFSET(stack, NPU2_BLOCK_XSL, offset),
			NPU2_MISC_DA_LEN_8B, reg);

	/*
	 * set the PE mask that the OS uses for PASID -> PE handle
	 * conversion
	 */
 	reg = npu2_scom_read(p->chip_id, p->xscom_base,
			NPU2_OTL_CONFIG0(stack, block), NPU2_MISC_DA_LEN_8B);
	reg &= ~NPU2_OTL_CONFIG0_PE_MASK;
	reg |= (PE_mask << (63-7));
	npu2_scom_write(p->chip_id, p->xscom_base,
			NPU2_OTL_CONFIG0(stack, block), NPU2_MISC_DA_LEN_8B,
			reg);
	rc = OPAL_SUCCESS;
out:
	unlock(&p->lock);
	return rc;
}
opal_call(OPAL_NPU_SPA_SETUP, opal_npu_spa_setup, 4);

static int64_t opal_npu_spa_clear_cache(uint64_t phb_id, uint32_t bdfn,
					uint64_t PE_handle)
{
	uint64_t cc_inv, stack, block, reg, rc;
	uint32_t retries = 5;
	struct phb *phb = pci_get_phb(phb_id);
	struct npu2 *p = phb_to_npu2(phb);
	struct npu2_dev *dev = npu2_bdf_to_dev(p, bdfn);

	if (!phb || phb->phb_type != phb_type_npu_v2_opencapi || !dev)
		return OPAL_PARAMETER;

	if (PE_handle > MAX_PE_HANDLE)
		return OPAL_PARAMETER;

	block = index_to_block(dev->index);
	stack = index_to_stack(dev->index);
	cc_inv = NPU2_REG_OFFSET(stack, NPU2_BLOCK_XSL, NPU2_XSL_PSL_LLCMD_A0);

	lock(&p->lock);
	reg = npu2_scom_read(p->chip_id, p->xscom_base, cc_inv,
			NPU2_MISC_DA_LEN_8B);
	if (reg & PPC_BIT(16)) {
		rc = OPAL_HARDWARE;
		goto out;
	}

	reg = PE_handle | PPC_BIT(15);
	if (block == NPU2_BLOCK_OTL1)
		reg |= PPC_BIT(48);
	npu2_scom_write(p->chip_id, p->xscom_base, cc_inv, NPU2_MISC_DA_LEN_8B,
			reg);

	rc = OPAL_HARDWARE;
	while (retries--) {
		reg = npu2_scom_read(p->chip_id, p->xscom_base, cc_inv,
				NPU2_MISC_DA_LEN_8B);
		if (!(reg & PPC_BIT(16))) {
			rc = OPAL_SUCCESS;
			break;
		}
		/* the bit expected to flip in less than 200us */
		time_wait_us(200);
	}
out:
	unlock(&p->lock);
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

static int64_t opal_npu_tl_set(uint64_t phb_id, uint32_t bdfn,
			long capabilities, uint64_t rate_phys, int rate_sz)
{
	struct phb *phb = pci_get_phb(phb_id);
	struct npu2 *p = phb_to_npu2(phb);
	struct npu2_dev *dev = npu2_bdf_to_dev(p, bdfn);
	uint64_t stack, block, reg, templ_rate;
	int i, rate_pos;
	char *rate = (char *) rate_phys;

	if (!phb || phb->phb_type != phb_type_npu_v2_opencapi || !dev)
		return OPAL_PARAMETER;
	if (!opal_addr_valid(rate) || rate_sz != TL_RATE_BUF_SIZE)
		return OPAL_PARAMETER;

	block = index_to_block(dev->index);
	stack = index_to_stack(dev->index);
	/*
	 * The 'capabilities' argument defines what TL temlpate the
	 * device can receive. Opencapi 3 and 4 define 64 templates, so
	 * that's one bit per template.
	 *
	 * For each template, the device processing time may vary, so
	 * the device advertises at what rate a message of a given
	 * template can be sent. That's encoded in the 'rate' buffer.
	 *
	 * On p9, NPU only knows about TL templates 0 -> 3.
	 * per the spec, template 0 must be supported.
	 */
	if (!is_template_supported(0, capabilities))
		return OPAL_PARAMETER;

	reg = npu2_scom_read(p->chip_id, p->xscom_base,
			NPU2_OTL_CONFIG1(stack, block), NPU2_MISC_DA_LEN_8B);
	reg &= ~(NPU2_OTL_CONFIG1_TX_TEMP1_EN | NPU2_OTL_CONFIG1_TX_TEMP3_EN |
		NPU2_OTL_CONFIG1_TX_TEMP1_EN);
	for (i = 0; i < 4; i++) {
		/* skip templ 0 as it is implicity enabled */
		if (i && is_template_supported(i, capabilities))
			reg |= PPC_BIT(i);
		/* the tx rate should still be set for templ 0 */
		templ_rate = get_template_rate(i, rate);
		rate_pos = 8 + i * 4;
		reg = SETFIELD(PPC_BITMASK(rate_pos, rate_pos + 3), reg, templ_rate);
	}
	npu2_scom_write(p->chip_id, p->xscom_base,
			NPU2_OTL_CONFIG1(stack, block), NPU2_MISC_DA_LEN_8B, reg);
	prlog(PR_DEBUG, "ocapi link %llx:%x, TL conf1 register set to %llx\n",
		phb_id, bdfn, reg);
	return OPAL_SUCCESS;
}
opal_call(OPAL_NPU_TL_SET, opal_npu_tl_set, 5);

static void reset_ocapi_device_zz(uint32_t gcid)
{
	struct dt_node *dn;
	char port_name[17];
	uint32_t opal_id = 0;
	uint8_t data[] = { 0xFD, 0xFD, 0xFF };
	uint32_t offset[] = { 0x3, 0x1, 0x1 };
	int rc;
	int i;

	// TODO: Figure out a canonical way to find this port
	snprintf(port_name, sizeof(port_name), "p8_%08x_e1p4", gcid);
	prlog(PR_DEBUG, "OCAPI: Looking for I2C port %s\n", port_name);
	dt_for_each_compatible(dt_root, dn, "ibm,power9-i2c-port") {
		if (streq(port_name, dt_prop_get(dn, "ibm,port-name"))) {
			opal_id = dt_prop_get_u32(dn, "ibm,opal-id");
			break;
		}
	}

	if (!opal_id) {
		prlog(PR_ERR, "OCAPI: Couldn't find I2C port %s\n", port_name);
		return;
	}

	for (i = 0; i < 3; i++) {
		rc = i2c_request_send(opal_id, 0x20, SMBUS_WRITE, offset[i], 1,
				      &data[i], sizeof(data[i]), 120);
		if (rc) {
			/**
			 * @fwts-label OCAPIZZDeviceResetFailed
			 * @fwts-advice There was an error attempting to send
			 * a reset signal over I2C to the OpenCAPI device.
			 */
			prlog(PR_ERR, "OCAPI: Error writing I2C reset signal: %d\n", rc);
			break;
		}
		if (i != 0) /* to mimic JT's */
			time_wait_ms(1000);
	}
}

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

/*
 * Support code for POWER9 Fabric Alter/Display Unit (ADU) hotplug functionality
 *
 * The ADU acts as a bridge between the PowerBus and the Pervasive Interconnect
 * Bus (PIB).
 *
 * Among other things, the ADU is used to support PowerBus Hotplug. Skiboot
 * needs to set the PowerBus Hotplug Mode Control register as part of OpenCAPI
 * and NVLink initialisation.
 */

#include <skiboot.h>
#include <timebase.h>
#include <xscom.h>
#include <chip.h>
#include <p9-adu.h>

/*
 * Lock or unlock ADU
 */
static int p9_adu_manage_lock(bool lock)
{
	int rc;
	uint64_t val = 0;
	struct proc_chip *chip = NULL;
	if (lock) {
		val |= PU_ALTD_CMD_REG_FBC_LOCKED;
		val |= PU_ALTD_CMD_REG_FBC_ALTD_RESET_FSM;
		val |= PU_ALTD_CMD_REG_FBC_ALTD_CLEAR_STATUS;
	}

	while ((chip = next_chip(chip))) {
		rc = xscom_write(chip->id, PU_ALTD_CMD_REG, val);
		if (rc != OPAL_SUCCESS) {
			/* TODO: Lock picking support */
			prlog(PR_ERR,
			      "ADU: Error %d writing command (chip %d)\n",
			      rc, chip->id);
			return OPAL_HARDWARE;
		}
	}
	return OPAL_SUCCESS;
}

static uint32_t find_master_chip(void)
{
	uint64_t reg;
	struct proc_chip *chip = NULL;
	while ((chip = next_chip(chip))) {
		xscom_read(chip->id, PB_CENT_HP_MODE_CURR, &reg);
		if (reg & PB_CFG_MASTER_CHIP)
			break;
	}
	return chip->id;
}

/*
 * Trigger a SWITCH_AB pulse to switch the current PowerBus Hotplug Mode Control
 * register set.
 *
 * Overview of sequence:
 *
 *  - acquire lock and reset all ADUs
 *  - configure all ADUs for AB switch
 *  - configure one ADU (on the fabric master chip) to issue a
 *    quiesce/switch/reinit
 *  - check status
 *  - clear switch selectors
 *  - reset all ADUs
 *  - unlock
 */
static void p9_adu_switch_ab(void)
{
	uint32_t gcid = find_master_chip();
	struct proc_chip *chip = NULL;
	uint64_t reg;
	uint64_t val;
	int rc = OPAL_SUCCESS;

	/*
	 * There's a performance issue on P9DD1 that requires a workaround:
	 * see IBM defect HW397129. However, this code isn't expected to be
	 * used on DD1 machines.
	 */

	rc = p9_adu_manage_lock(true);
	if (rc)
		goto err;

	/* Set PB_SWITCH_AB on all ADUs */
	while ((chip = next_chip(chip))) {
		xscom_read(chip->id, PU_SND_MODE_REG, &reg);
		reg |= PU_SND_MODE_REG_ENABLE_PB_SWITCH_AB;
		rc = xscom_write(chip->id, PU_SND_MODE_REG, reg);
		if (rc)
			goto err_switch;
	}

	/* Set address 0 */
	rc = xscom_write(gcid, PU_ALTD_ADDR_REG, 0);
	if (rc)
		goto err_switch;

	/* Configure ADU to issue quiesce + switch + reinit */
	val = PU_ALTD_OPTION_REG_FBC_ALTD_WITH_PRE_QUIESCE;
	val = SETFIELD(PU_ALTD_OPTION_REG_FBC_ALTD_AFTER_QUIESCE_WAIT_COUNT,
		       val, QUIESCE_SWITCH_WAIT_COUNT);
	val |= PU_ALTD_OPTION_REG_FBC_ALTD_WITH_POST_INIT;
	val = SETFIELD(PU_ALTD_OPTION_REG_FBC_ALTD_BEFORE_INIT_WAIT_COUNT,
		       val, INIT_SWITCH_WAIT_COUNT);
	val |= PU_ALTD_OPTION_REG_FBC_ALTD_WITH_FAST_PATH; /* see HW397129, DD2 */
	rc = xscom_write(gcid, PU_ALTD_OPTION_REG, val);
	if (rc)
		goto err_switch;

	/* Set up command */
	val = PU_ALTD_CMD_REG_FBC_LOCKED;
	val |= PU_ALTD_CMD_REG_FBC_ALTD_START_OP;
	val = SETFIELD(PU_ALTD_CMD_REG_FBC_ALTD_SCOPE, val,
		       PU_ALTD_CMD_REG_FBC_ALTD_SCOPE_SYSTEM);
	val |= PU_ALTD_CMD_REG_FBC_ALTD_DROP_PRIORITY;
	val |= PU_ALTD_CMD_REG_FBC_ALTD_AXTYPE;
	val = SETFIELD(PU_ALTD_CMD_REG_FBC_ALTD_TTYPE, val,
		       PU_ALTD_CMD_REG_FBC_ALTD_TTYPE_PMISC_OPER);
	val |= PU_ALTD_CMD_REG_FBC_ALTD_WITH_TM_QUIESCE;
	val = SETFIELD(PU_ALTD_CMD_REG_FBC_ALTD_TSIZE, val,
		       PU_ALTD_CMD_REG_FBC_ALTD_TSIZE_PMISC_1);
	xscom_write(gcid, PU_ALTD_CMD_REG, val);

	/*
	 * TODO: check ADU status is consistent, see
	 * p9_build_smp_adu_check_status() in hostboot
	 */

err_switch:
	/* Reset switch controls */
	chip = NULL;
	while ((chip = next_chip(chip))) {
		xscom_read(chip->id, PU_SND_MODE_REG, &reg);
		reg &= ~PU_SND_MODE_REG_ENABLE_PB_SWITCH_AB;
		xscom_write(chip->id, PU_SND_MODE_REG, reg);
	}

	/* Reset ADUs */
	p9_adu_manage_lock(true);

err:
	/* Unlock */
	p9_adu_manage_lock(false);
}

void p9_adu_set_pb_hp_mode(uint32_t gcid, uint64_t val)
{
	/* Write next value */
	xscom_write(gcid, PB_WEST_HP_MODE_NEXT, val);
	xscom_write(gcid, PB_CENT_HP_MODE_NEXT, val);
	xscom_write(gcid, PB_EAST_HP_MODE_NEXT, val);

	/* Send switch pulse */
	p9_adu_switch_ab();

	/* Now that switch is complete, overwrite old value */
	xscom_write(gcid, PB_WEST_HP_MODE_NEXT, val);
	xscom_write(gcid, PB_CENT_HP_MODE_NEXT, val);
	xscom_write(gcid, PB_EAST_HP_MODE_NEXT, val);
}

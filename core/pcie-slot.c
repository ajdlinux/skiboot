/* Copyright 2013-2016 IBM Corp.
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
#include <opal-msg.h>
#include <pci-cfg.h>
#include <pci.h>
#include <pci-slot.h>

/* Debugging options */
#define PCIE_SLOT_PREFIX	"PCIE-SLOT-%016llx "
#define PCIE_SLOT_DBG(s, fmt, a...)		  \
	prlog(PR_DEBUG, PCIE_SLOT_PREFIX fmt, (s)->id, ##a)

static int64_t pcie_slot_get_presence_status(struct pci_slot *slot,
					     uint8_t *val)
{
	struct phb *phb = slot->phb;
	struct pci_device *pd = slot->pd;
	uint32_t ecap;
	int16_t slot_cap, slot_sts;

	/*
	 * We assume the downstream ports are always connected
	 * to the upstream port. Hard code the presence bit for
	 * the case.
	 */
	if (pd->dev_type == PCIE_TYPE_SWITCH_UPPORT) {
		*val = OPAL_PCI_SLOT_PRESENT;
		return OPAL_SUCCESS;
	}

	/*
	 * If downstream port doesn't support slot capability,
	 * we need hardcode it as "presence" according PCIE
	 * spec.
	 *
	 * Note that the power should be supplied to the slot
	 * before detecting the presence bit.
	 */
	ecap = pci_cap(pd, PCI_CFG_CAP_ID_EXP, false);
	pci_cfg_read16(phb, pd->bdfn, ecap + PCICAP_EXP_CAPABILITY_REG,
		       &slot_cap);
	if (pd->dev_type == PCIE_TYPE_SWITCH_DNPORT &&
	    !(slot_cap & PCICAP_EXP_CAP_SLOT)) {
		*val = OPAL_PCI_SLOT_PRESENT;
		return OPAL_SUCCESS;
	}

	/* Check presence bit */
	pci_cfg_read16(phb, pd->bdfn, ecap + PCICAP_EXP_SLOTSTAT,
		       &slot_sts);
	if (slot_sts & PCICAP_EXP_SLOTSTAT_PDETECTST)
		*val = OPAL_PCI_SLOT_PRESENT;
	else
		*val = OPAL_PCI_SLOT_EMPTY;

	return OPAL_SUCCESS;
}

static int64_t pcie_slot_get_link_status(struct pci_slot *slot,
					 uint8_t *val)
{
	struct phb *phb = slot->phb;
	struct pci_device *pd = slot->pd;
	uint32_t ecap;
	int16_t lstat;

	/*
	 * Upstream port doesn't have valid link indicator in
	 * data-link status. Lets hardcode it as link-up as
	 * the downtream ports are always connected to the
	 * upstream port.
	 */
	if (pd->dev_type == PCIE_TYPE_SWITCH_UPPORT) {
		*val = 1;
		return OPAL_SUCCESS;
	}

	ecap = pci_cap(pd, PCI_CFG_CAP_ID_EXP, false);
	pci_cfg_read16(phb, pd->bdfn, ecap + PCICAP_EXP_LSTAT, &lstat);
	if (!(lstat & PCICAP_EXP_LSTAT_DLLL_ACT)) {
		*val = 0;
		return OPAL_SUCCESS;
	}

	*val = GETFIELD(PCICAP_EXP_LSTAT_WIDTH, lstat);
	return OPAL_SUCCESS;
}

static int64_t pcie_slot_get_power_status(struct pci_slot *slot,
					  uint8_t *val)
{
	struct phb *phb = slot->phb;
	struct pci_device *pd = slot->pd;
	uint32_t ecap;
	uint16_t slot_ctl;

	/*
	 * We assume the power is always on if we don't have
	 * power control functionality.
	 */
	ecap = pci_cap(pd, PCI_CFG_CAP_ID_EXP, false);
	if (!(slot->slot_cap & PCICAP_EXP_SLOTCAP_PWCTRL)) {
		*val = PCI_SLOT_POWER_ON;
		return OPAL_SUCCESS;
	}

	/* Check power supply bit */
	*val = PCI_SLOT_POWER_OFF;
	pci_cfg_read16(phb, pd->bdfn, ecap + PCICAP_EXP_SLOTCTL,
		       &slot_ctl);
	if (!(slot_ctl & PCICAP_EXP_SLOTCTL_PWRCTLR))
		*val = PCI_SLOT_POWER_ON;

	return OPAL_SUCCESS;
}

static int64_t pcie_slot_get_attention_status(struct pci_slot *slot,
					      uint8_t *val)
{
	struct phb *phb = slot->phb;
	struct pci_device *pd = slot->pd;
	uint32_t ecap;
	uint16_t slot_ctl;

	/* Attention is assumed as off if we don't have the capability */
	ecap = pci_cap(pd, PCI_CFG_CAP_ID_EXP, false);
	if (!(slot->slot_cap & PCICAP_EXP_SLOTCAP_ATTNI)) {
		*val = 0;
		return OPAL_SUCCESS;
	}

	/* Check the attention bits */
	pci_cfg_read16(phb, pd->bdfn, ecap + PCICAP_EXP_SLOTCTL, &slot_ctl);
	switch (GETFIELD(PCICAP_EXP_SLOTCTL_ATTNI, slot_ctl)) {
	case PCIE_INDIC_ON:
	case PCIE_INDIC_BLINK:
		*val = GETFIELD(PCICAP_EXP_SLOTCTL_ATTNI, slot_ctl);
		break;
	case PCIE_INDIC_OFF:
	default:
		*val = 0;
	}

	return OPAL_SUCCESS;
}

static int64_t pcie_slot_get_latch_status(struct pci_slot *slot,
					  uint8_t *val)
{
	struct phb *phb = slot->phb;
	struct pci_device *pd = slot->pd;
	uint32_t ecap;
	uint16_t slot_sts;

	/* We assume latch is always off if MRL sensor not existing */
	ecap = pci_cap(pd, PCI_CFG_CAP_ID_EXP, false);
	if (!(slot->slot_cap & PCICAP_EXP_SLOTCAP_MRLSENS)) {
		*val = 0;
		return OPAL_SUCCESS;
	}

	/* Check state of MRL sensor */
	*val = 0;
	pci_cfg_read16(phb, pd->bdfn, ecap + PCICAP_EXP_SLOTSTAT, &slot_sts);
	if (slot_sts & PCICAP_EXP_SLOTSTAT_MRLSENSST)
		*val = 1;

	return OPAL_SUCCESS;
}

static int64_t pcie_slot_set_attention_status(struct pci_slot *slot,
					      uint8_t val)
{
	struct phb *phb = slot->phb;
	struct pci_device *pd = slot->pd;
	uint32_t ecap;
	uint16_t slot_ctl;

	/*
	 * Drop the request if the slot doesn't support
	 * attention capability
	 */
	ecap = pci_cap(pd, PCI_CFG_CAP_ID_EXP, false);
	if (!(slot->slot_cap & PCICAP_EXP_SLOTCAP_ATTNI))
		return OPAL_SUCCESS;

	/* Write the attention bits */
	pci_cfg_read16(phb, pd->bdfn, ecap + PCICAP_EXP_SLOTCTL, &slot_ctl);
	switch (val) {
	case PCI_SLOT_ATTN_LED_OFF:
		slot_ctl = SETFIELD(PCICAP_EXP_SLOTCTL_ATTNI,
				    slot_ctl, PCIE_INDIC_OFF);
		break;
	case PCI_SLOT_ATTN_LED_ON:
		slot_ctl = SETFIELD(PCICAP_EXP_SLOTCTL_ATTNI,
				    slot_ctl, PCIE_INDIC_ON);
		break;
	case PCI_SLOT_ATTN_LED_BLINK:
		slot_ctl = SETFIELD(PCICAP_EXP_SLOTCTL_ATTNI,
				    slot_ctl, PCIE_INDIC_BLINK);
		break;
	default:
		prlog(PR_ERR, PCIE_SLOT_PREFIX
		      "Invalid attention state (0x%x)\n", slot->id, val);
	}

	pci_cfg_write16(phb, pd->bdfn, ecap + PCICAP_EXP_SLOTCTL, slot_ctl);
	return OPAL_SUCCESS;
}

static int64_t pcie_slot_set_power_status(struct pci_slot *slot,
					  uint8_t val)
{
	struct phb *phb = slot->phb;
	struct pci_device *pd = slot->pd;
	uint32_t ecap;
	uint16_t slot_ctl;

	/* Drop the request in case power control isn't supported */
	if (!(slot->slot_cap & PCICAP_EXP_SLOTCAP_PWCTRL))
		return OPAL_SUCCESS;

	ecap = pci_cap(pd, PCI_CFG_CAP_ID_EXP, false);
	pci_cfg_read16(phb, pd->bdfn, ecap + PCICAP_EXP_SLOTCTL, &slot_ctl);
	switch (val) {
	case PCI_SLOT_POWER_OFF:
		slot_ctl |= PCICAP_EXP_SLOTCTL_PWRCTLR;
		slot_ctl = SETFIELD(PCICAP_EXP_SLOTCTL_PWRI, slot_ctl,
				    PCIE_INDIC_OFF);
		break;
	case PCI_SLOT_POWER_ON:
		slot_ctl &= ~PCICAP_EXP_SLOTCTL_PWRCTLR;
		slot_ctl = SETFIELD(PCICAP_EXP_SLOTCTL_PWRI, slot_ctl,
				    PCIE_INDIC_ON);
		break;
	}

	pci_cfg_write16(phb, pd->bdfn, ecap + PCICAP_EXP_SLOTCTL,
			slot_ctl);
	return OPAL_SUCCESS;
}

static int64_t pcie_slot_sm_poll_link(struct pci_slot *slot)
{
	struct phb *phb = slot->phb;
	struct pci_device *pd = slot->pd;
	uint32_t ecap;
	uint16_t val;
	uint8_t presence = 0;

	switch (slot->state) {
	case PCI_SLOT_STATE_LINK_START_POLL:
		PCIE_SLOT_DBG(slot, "LINK: Start polling\n");

		/* Link is down for ever without devices attached */
		if (slot->ops.get_presence_status)
			slot->ops.get_presence_status(slot, &presence);
		if (!presence) {
			PCIE_SLOT_DBG(slot, "LINK: No adapter, end polling\n");
			pci_slot_set_state(slot, PCI_SLOT_STATE_NORMAL);
			return OPAL_SUCCESS;
		}

		/* Enable the link without check */
		ecap = pci_cap(pd, PCI_CFG_CAP_ID_EXP, false);
		pci_cfg_read16(phb, pd->bdfn, ecap + PCICAP_EXP_LCTL, &val);
		val &= ~PCICAP_EXP_LCTL_LINK_DIS;
		pci_cfg_write16(phb, pd->bdfn, ecap + PCICAP_EXP_LCTL, val);

		/*
		 * If the link change report isn't supported, we expect
		 * the link is up and stabilized after one second.
		 */
		if (!(slot->link_cap & PCICAP_EXP_LCAP_DL_ACT_REP)) {
			pci_slot_set_state(slot,
					   PCI_SLOT_STATE_LINK_DELAY_FINALIZED);
			return pci_slot_set_sm_timeout(slot, secs_to_tb(1));
		}

		/*
		 * Poll the link state if link state change report is
		 * supported on the link.
		 */
		pci_slot_set_state(slot, PCI_SLOT_STATE_LINK_POLLING);
		slot->retries = 250;
		return pci_slot_set_sm_timeout(slot, msecs_to_tb(20));
	case PCI_SLOT_STATE_LINK_DELAY_FINALIZED:
		PCIE_SLOT_DBG(slot, "LINK: No link report, end polling\n");
		if (slot->ops.prepare_link_change)
			slot->ops.prepare_link_change(slot, true);
		pci_slot_set_state(slot, PCI_SLOT_STATE_NORMAL);
		return OPAL_SUCCESS;
	case PCI_SLOT_STATE_LINK_POLLING:
		ecap = pci_cap(pd, PCI_CFG_CAP_ID_EXP, false);
		pci_cfg_read16(phb, pd->bdfn, ecap + PCICAP_EXP_LSTAT, &val);
		if (val & PCICAP_EXP_LSTAT_DLLL_ACT) {
			PCIE_SLOT_DBG(slot, "LINK: Link is up, end polling\n");
			if (slot->ops.prepare_link_change)
				slot->ops.prepare_link_change(slot, true);
			pci_slot_set_state(slot, PCI_SLOT_STATE_NORMAL);
			return OPAL_SUCCESS;
		}

		/* Check link state again until timeout */
		if (slot->retries-- == 0) {
			prlog(PR_ERR, PCIE_SLOT_PREFIX
			      "LINK: Timeout waiting for up (%04x)\n",
			      slot->id, val);
			pci_slot_set_state(slot, PCI_SLOT_STATE_NORMAL);
			return OPAL_SUCCESS;
		}

		return pci_slot_set_sm_timeout(slot, msecs_to_tb(20));
	default:
		prlog(PR_ERR, PCIE_SLOT_PREFIX
		      "Link: Unexpected slot state %08x\n",
		      slot->id, slot->state);
	}

	pci_slot_set_state(slot, PCI_SLOT_STATE_NORMAL);
	return OPAL_HARDWARE;
}

static void pcie_slot_reset(struct pci_slot *slot, bool assert)
{
	struct phb *phb = slot->phb;
	struct pci_device *pd = slot->pd;
	uint16_t ctl;

	pci_cfg_read16(phb, pd->bdfn, PCI_CFG_BRCTL, &ctl);
	if (assert)
		ctl |= PCI_CFG_BRCTL_SECONDARY_RESET;
	else
		ctl &= ~PCI_CFG_BRCTL_SECONDARY_RESET;
	pci_cfg_write16(phb, pd->bdfn, PCI_CFG_BRCTL, ctl);
}

static int64_t pcie_slot_sm_hreset(struct pci_slot *slot)
{
	switch (slot->state) {
	case PCI_SLOT_STATE_NORMAL:
		PCIE_SLOT_DBG(slot, "HRESET: Starts\n");
		if (slot->ops.prepare_link_change) {
			PCIE_SLOT_DBG(slot, "HRESET: Prepare for link down\n");
			slot->ops.prepare_link_change(slot, false);
		}
		/* fall through */
	case PCI_SLOT_STATE_HRESET_START:
		PCIE_SLOT_DBG(slot, "HRESET: Assert\n");
		pcie_slot_reset(slot, true);
		pci_slot_set_state(slot, PCI_SLOT_STATE_HRESET_HOLD);
		return pci_slot_set_sm_timeout(slot, msecs_to_tb(250));
	case PCI_SLOT_STATE_HRESET_HOLD:
		PCIE_SLOT_DBG(slot, "HRESET: Deassert\n");
		pcie_slot_reset(slot, false);
		pci_slot_set_state(slot, PCI_SLOT_STATE_LINK_START_POLL);
		return pci_slot_set_sm_timeout(slot, msecs_to_tb(1800));
	default:
		PCIE_SLOT_DBG(slot, "HRESET: Unexpected slot state %08x\n",
			      slot->state);
	}

	pci_slot_set_state(slot, PCI_SLOT_STATE_NORMAL);
	return OPAL_HARDWARE;
}

/*
 * Usually, individual platforms need override the power
 * management methods for fundamental reset because the
 * hot reset is commonly shared.
 */
static int64_t pcie_slot_sm_freset(struct pci_slot *slot)
{
	uint8_t power_state = PCI_SLOT_POWER_ON;

	switch (slot->state) {
	case PCI_SLOT_STATE_NORMAL:
		PCIE_SLOT_DBG(slot, "FRESET: Starts\n");
		if (slot->ops.prepare_link_change)
			slot->ops.prepare_link_change(slot, false);

		/* Retrieve power state */
		if (slot->ops.get_power_status) {
			PCIE_SLOT_DBG(slot, "FRESET: Retrieve power state\n");
			slot->ops.get_power_status(slot, &power_state);
		}

		/* In power on state, power it off */
		if (power_state == PCI_SLOT_POWER_ON &&
		    slot->ops.set_power_status) {
			PCIE_SLOT_DBG(slot, "FRESET: Power is on, turn off\n");
			slot->ops.set_power_status(slot,
						   PCI_SLOT_POWER_OFF);
			pci_slot_set_state(slot,
					   PCI_SLOT_STATE_FRESET_POWER_OFF);
			return pci_slot_set_sm_timeout(slot, msecs_to_tb(50));
		}
		/* No power state change, fall through */
	case PCI_SLOT_STATE_FRESET_POWER_OFF:
		PCIE_SLOT_DBG(slot, "FRESET: Power is off, turn on\n");
		if (slot->ops.set_power_status)
			slot->ops.set_power_status(slot,
						   PCI_SLOT_POWER_ON);
		pci_slot_set_state(slot, PCI_SLOT_STATE_HRESET_START);
		return pci_slot_set_sm_timeout(slot, msecs_to_tb(50));
	default:
		prlog(PR_ERR, PCIE_SLOT_PREFIX
		      "FRESET: Unexpected slot state %08x\n",
		      slot->id, slot->state);
	}

	pci_slot_set_state(slot, PCI_SLOT_STATE_NORMAL);
	return OPAL_HARDWARE;
}

struct pci_slot *pcie_slot_create(struct phb *phb, struct pci_device *pd)
{
	struct pci_slot *slot;
	uint32_t ecap;

	/* Allocate PCI slot */
	slot = pci_slot_alloc(phb, pd);
	if (!slot)
		return NULL;

	/* Cache the link and slot capabilities */
	if (pd) {
		ecap = pci_cap(pd, PCI_CFG_CAP_ID_EXP, false);
		pci_cfg_read32(phb, pd->bdfn,
			       ecap + PCICAP_EXP_LCAP, &slot->link_cap);
		pci_cfg_read32(phb, pd->bdfn,
			       ecap + PCICAP_EXP_SLOTCAP, &slot->slot_cap);
	}

	/* Slot info */
	if ((slot->slot_cap & PCICAP_EXP_SLOTCAP_HPLUG_SURP) &&
	    (slot->slot_cap & PCICAP_EXP_SLOTCAP_HPLUG_CAP))
		slot->pluggable = 1;
	if (slot->slot_cap & PCICAP_EXP_SLOTCAP_PWCTRL)
		slot->power_ctl = 1;
	if (slot->slot_cap & PCICAP_EXP_SLOTCAP_PWRI)
		slot->power_led_ctl = PCI_SLOT_PWR_LED_CTL_KERNEL;
	if (slot->slot_cap & PCICAP_EXP_SLOTCAP_ATTNI)
		slot->attn_led_ctl = PCI_SLOT_ATTN_LED_CTL_KERNEL;
	slot->wired_lanes = GETFIELD(PCICAP_EXP_LCAP_MAXWDTH, slot->link_cap);

	/* Standard slot operations */
	slot->ops.get_presence_status   = pcie_slot_get_presence_status;
	slot->ops.get_link_status       = pcie_slot_get_link_status;
	slot->ops.get_power_status      = pcie_slot_get_power_status;
	slot->ops.get_attention_status  = pcie_slot_get_attention_status;
	slot->ops.get_latch_status      = pcie_slot_get_latch_status;
	slot->ops.set_power_status      = pcie_slot_set_power_status;
	slot->ops.set_attention_status  = pcie_slot_set_attention_status;

	/*
	 * SM based reset stuff. The poll function is always
	 * unified for all cases.
	 */
	slot->ops.poll_link             = pcie_slot_sm_poll_link;
	slot->ops.hreset                = pcie_slot_sm_hreset;
	slot->ops.freset                = pcie_slot_sm_freset;
	slot->ops.pfreset               = NULL;

	return slot;
}

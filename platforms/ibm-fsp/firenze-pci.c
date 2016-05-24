/* Copyright 2013-2015 IBM Corp.
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
#include <device.h>
#include <fsp.h>
#include <lock.h>
#include <timer.h>
#include <xscom.h>
#include <pci-cfg.h>
#include <pci.h>
#include <pci-slot.h>
#include <phb3.h>
#include <chip.h>
#include <i2c.h>

#include "ibm-fsp.h"
#include "lxvpd.h"

/* Debugging options */
#define FIRENZE_PCI_DBG(fmt, a...)	\
	prlog(PR_DEBUG, "FIRENZE-PCI: " fmt, ##a)
#define FIRENZE_PCI_INFO(fmt, a...)	\
	prlog(PR_INFO, "FIRENZE-PCI: " fmt, ##a)
#define FIRENZE_PCI_ERR(fmt, a...)	\
	prlog(PR_ERR, "FIRENZE-PCI: " fmt, ##a)

/* Dump PCI slots before sending to FSP */
#define FIRENZE_PCI_INVENTORY_DUMP

/*
 * Firenze PCI slot states to override the default set.
 * Refer to pci-slot.h for the default PCI state set
 * when you're going to change below values.
 */
#define FIRENZE_PCI_SLOT_NORMAL			0x00000000
#define FIRENZE_PCI_SLOT_HRESET			0x00000200
#define   FIRENZE_PCI_SLOT_HRESET_START		0x00000201
#define FIRENZE_PCI_SLOT_FRESET			0x00000300
#define FIRENZE_PCI_SLOT_FRESET_START		0x00000301
#define   FIRENZE_PCI_SLOT_FRESET_WAIT_RSP	0x00000302
#define   FIRENZE_PCI_SLOT_FRESET_DELAY		0x00000303
#define   FIRENZE_PCI_SLOT_FRESET_POWER_STATE	0x00000304
#define   FIRENZE_PCI_SLOT_FRESET_POWER_OFF	0x00000305
#define   FIRENZE_PCI_SLOT_FRESET_POWER_ON	0x00000306
#define   FIRENZE_PCI_SLOT_PERST_DEASSERT	0x00000307
#define   FIRENZE_PCI_SLOT_PERST_DELAY		0x00000308
#define FIRENZE_PCI_SLOT_PFRESET		0x00000400
#define   FIRENZE_PCI_SLOT_PFRESET_START	0x00000401
#define FIRENZE_PCI_SLOT_GPOWER			0x00000600
#define   FIRENZE_PCI_SLOT_GPOWER_START		0x00000601
#define FIRENZE_PCI_SLOT_SPOWER			0x00000700
#define   FIRENZE_PCI_SLOT_SPOWER_START		0x00000701
#define   FIRENZE_PCI_SLOT_SPOWER_DONE		0x00000702

/* Timeout for power status */
#define FIRENZE_PCI_SLOT_RETRIES	500
#define FIRENZE_PCI_SLOT_DELAY		10	/* ms */
#define FIRENZE_PCI_I2C_TIMEOUT		500	/* ms */

/*
 * Need figure out more stuff later: LED and presence
 * detection sensors are accessed from PSI/FSP.
 */
struct firenze_pci_slot {
	struct lxvpd_pci_slot	lxvpd_slot;	/* LXVPD slot data    */

	/* Next slot state */
	uint32_t		next_state;

	/* Power management */
	struct i2c_bus		*i2c_bus;	/* Where MAX5961 seats   */
	struct i2c_request	*req;		/* I2C request message   */
	uint8_t			i2c_rw_buf[8];	/* I2C read/write buffer */
	uint8_t			power_mask;	/* Bits for power status */
	uint8_t			power_on;	/* Bits for power on     */
	uint8_t			power_off;	/* Bits for power off    */
	uint8_t			*power_status;	/* Last power status     */
	uint16_t		perst_reg;	/* PERST config register */
	uint16_t		perst_bit;	/* PERST bit             */
};

struct firenze_pci_slot_info {
	uint8_t		index;
	const char	*label;
	uint8_t		external_power_mgt;
	uint8_t		inband_perst;
	uint8_t		chip_id;
	uint8_t		master_id;
	uint8_t		port_id;
	uint8_t		slave_addr;
	uint8_t		channel;
	uint8_t		power_status;
	uint8_t		buddy;
};

struct firenze_pci_inv {
	uint32_t	hw_proc_id;
	uint16_t	slot_idx;
	uint16_t	reserved;
	uint16_t	vendor_id;
	uint16_t	device_id;
	uint16_t	subsys_vendor_id;
	uint16_t	subsys_device_id;
};

struct firenze_pci_inv_data {
	uint32_t                version;	/* currently 1 */
	uint32_t                num_entries;
	uint32_t                entry_size;
	uint32_t                entry_offset;
	struct firenze_pci_inv	entries[];
};

/*
 * Note: According to Tuleta system workbook, I didn't figure
 * out the I2C mapping info for slot C14/C15.
 */
static struct firenze_pci_inv_data *firenze_inv_data;
static uint32_t firenze_inv_cnt;
static struct firenze_pci_slot_info firenze_pci_slots[] = {
	{ 0x0B,  "C7", 1, 1,    0, 1, 0, 0x35, 1, 0xAA,  0 },
	{ 0x11, "C14", 0, 1,    0, 0, 0, 0x00, 0, 0xAA,  1 },
	{ 0x0F, "C11", 1, 1,    0, 1, 0, 0x32, 1, 0xAA,  2 },
	{ 0x10, "C12", 1, 1,    0, 1, 0, 0x39, 0, 0xAA,  3 },
	{ 0x0A,  "C6", 1, 1,    0, 1, 0, 0x35, 0, 0xAA,  0 },
	{ 0x12, "C15", 0, 1,    0, 0, 0, 0x00, 0, 0xAA,  5 },
	{ 0x01, "USB", 0, 0,    0, 0, 0, 0x00, 0, 0xAA,  6 },
	{ 0x0C,  "C8", 1, 1,    0, 1, 0, 0x36, 0, 0xAA,  7 },
	{ 0x0D,  "C9", 1, 1,    0, 1, 0, 0x36, 1, 0xAA,  7 },
	{ 0x0E, "C10", 1, 1,    0, 1, 0, 0x32, 0, 0xAA,  2 },
	{ 0x09,  "C5", 1, 1, 0x10, 1, 0, 0x39, 1, 0xAA, 10 },
	{ 0x08,  "C4", 1, 1, 0x10, 1, 0, 0x39, 0, 0xAA, 10 },
	{ 0x07,  "C3", 1, 1, 0x10, 1, 0, 0x3A, 1, 0xAA, 12 },
	{ 0x06,  "C2", 1, 1, 0x10, 1, 0, 0x3A, 0, 0xAA, 12 }
};

static void firenze_pci_add_inventory(struct phb *phb,
				      struct pci_device *pd)
{
	struct lxvpd_pci_slot *lxvpd_slot;
	struct firenze_pci_inv *entry;
	struct proc_chip *chip;
	size_t size;
	bool need_init = false;

	/*
	 * Do we need to add that to the FSP inventory for power
	 * management?
	 *
	 * For now, we only add devices that:
	 *
	 *  - Are function 0
	 *  - Are not an RC or a downstream bridge
	 *  - Have a direct parent that has a slot entry
	 *  - Slot entry says pluggable
	 *  - Aren't an upstream switch that has slot info
	 */
	if (!pd || !pd->parent)
		return;
	if (pd->bdfn & 7)
		return;
	if (pd->dev_type == PCIE_TYPE_ROOT_PORT ||
	    pd->dev_type == PCIE_TYPE_SWITCH_DNPORT)
		return;
	if (pd->dev_type == PCIE_TYPE_SWITCH_UPPORT &&
	    pd->slot && pd->slot->data)
		return;
	if (!pd->parent->slot ||
	    !pd->parent->slot->data)
		return;
	lxvpd_slot = pd->parent->slot->data;
	if (!lxvpd_slot->pluggable)
		return;

	/* Check if we need to do some (Re)allocation */
	if (!firenze_inv_data ||
            firenze_inv_data->num_entries == firenze_inv_cnt) {
		need_init = !firenze_inv_data;

		/* (Re)allocate the block to the new size */
		firenze_inv_cnt += 4;
		size = sizeof(struct firenze_pci_inv_data) +
		       sizeof(struct firenze_pci_inv) * firenze_inv_cnt;
                firenze_inv_data = realloc(firenze_inv_data, size);
	}

	/* Initialize the header for a new inventory */
	if (need_init) {
		firenze_inv_data->version = 1;
		firenze_inv_data->num_entries = 0;
		firenze_inv_data->entry_size =
			sizeof(struct firenze_pci_inv);
		firenze_inv_data->entry_offset =
			offsetof(struct firenze_pci_inv_data, entries);
	}

	/* Append slot entry */
	entry = &firenze_inv_data->entries[firenze_inv_data->num_entries++];
	chip = get_chip(dt_get_chip_id(phb->dt_node));
	if (!chip) {
		FIRENZE_PCI_ERR("No chip device node for PHB%04x\n",
				phb->opal_id);
                return;
	}

	entry->hw_proc_id = chip->pcid;
	entry->reserved = 0;
	if (pd->parent &&
	    pd->parent->slot &&
	    pd->parent->slot->data) {
		lxvpd_slot = pd->parent->slot->data;
		entry->slot_idx = lxvpd_slot->slot_index;
	}

	pci_cfg_read16(phb, pd->bdfn, PCI_CFG_VENDOR_ID, &entry->vendor_id);
	pci_cfg_read16(phb, pd->bdfn, PCI_CFG_DEVICE_ID, &entry->device_id);
        if (pd->is_bridge) {
                int64_t ssvc = pci_find_cap(phb, pd->bdfn,
					    PCI_CFG_CAP_ID_SUBSYS_VID);
		if (ssvc <= 0) {
			entry->subsys_vendor_id = 0xffff;
			entry->subsys_device_id = 0xffff;
		} else {
			pci_cfg_read16(phb, pd->bdfn,
				       ssvc + PCICAP_SUBSYS_VID_VENDOR,
				       &entry->subsys_vendor_id);
			pci_cfg_read16(phb, pd->bdfn,
				       ssvc + PCICAP_SUBSYS_VID_DEVICE,
				       &entry->subsys_device_id);
		}
        } else {
		pci_cfg_read16(phb, pd->bdfn, PCI_CFG_SUBSYS_VENDOR_ID,
			       &entry->subsys_vendor_id);
		pci_cfg_read16(phb, pd->bdfn, PCI_CFG_SUBSYS_ID,
			       &entry->subsys_device_id);
	}
}

static void firenze_dump_pci_inventory(void)
{
#ifdef FIRENZE_PCI_INVENTORY_DUMP
	struct firenze_pci_inv *e;
	uint32_t i;

	if (!firenze_inv_data)
		return;

	FIRENZE_PCI_INFO("Dumping Firenze PCI inventory\n");
	FIRENZE_PCI_INFO("HWP SLT VDID DVID SVID SDID\n");
	FIRENZE_PCI_INFO("---------------------------\n");
	for (i = 0; i < firenze_inv_data->num_entries; i++) {
		e = &firenze_inv_data->entries[i];

		FIRENZE_PCI_INFO("%03d %03d %04x %04x %04x %04x\n",
				 e->hw_proc_id, e->slot_idx,
				 e->vendor_id, e->device_id,
				 e->subsys_vendor_id, e->subsys_device_id);
	}
#endif /* FIRENZE_PCI_INVENTORY_DUMP */
}

void firenze_pci_send_inventory(void)
{
	uint64_t base, abase, end, aend, offset;
	int64_t rc;

	if (!firenze_inv_data)
		return;

	/* Dump the inventory */
	FIRENZE_PCI_INFO("Sending %d inventory to FSP\n",
			 firenze_inv_data->num_entries);
	firenze_dump_pci_inventory();

	/* Memory location for inventory */
        base = (uint64_t)firenze_inv_data;
        end = base +
	      sizeof(struct firenze_pci_inv_data) +
	      firenze_inv_data->num_entries * firenze_inv_data->entry_size;
	abase = base & ~0xffful;
	aend = (end + 0xffful) & ~0xffful;
	offset = PSI_DMA_PCIE_INVENTORY + (base & 0xfff);

	/* We can only accomodate so many entries in the PSI map */
	if ((aend - abase) > PSI_DMA_PCIE_INVENTORY_SIZE) {
		FIRENZE_PCI_ERR("Inventory (%lld bytes) too large\n",
				aend - abase);
		goto bail;
	}

	/* Map this in the TCEs */
	fsp_tce_map(PSI_DMA_PCIE_INVENTORY, (void *)abase, aend - abase);

	/* Send FSP message */
	rc = fsp_sync_msg(fsp_mkmsg(FSP_CMD_PCI_POWER_CONF, 3,
				    hi32(offset), lo32(offset),
				    end - base), true);
	if (rc)
		FIRENZE_PCI_ERR("Error %lld sending inventory\n",
				rc);

	/* Unmap */
	fsp_tce_unmap(PSI_DMA_PCIE_INVENTORY, aend - abase);
bail:
	/*
	 * We free the inventory. We'll have to redo that on hotplug
	 * when we support it but that isn't the case yet
	 */
	free(firenze_inv_data);
	firenze_inv_data = NULL;
	firenze_inv_cnt = 0;
}

/* The function is called when the I2C request is completed
 * successfully, or with errors.
 */
static void firenze_i2c_req_done(int rc, struct i2c_request *req)
{
	struct pci_slot *slot = req->user_data;
	uint32_t state;

	/* Check if there are errors for the completion */
	if (rc) {
		FIRENZE_PCI_ERR("Error %d from I2C request on slot %016llx\n",
				rc, slot->id);
		return;
	}

	/* Check the request type */
	if (req->op != SMBUS_READ && req->op != SMBUS_WRITE) {
		FIRENZE_PCI_ERR("Invalid I2C request %d on slot %016llx\n",
				req->op, slot->id);
		return;
	}

	/* After writting power status to I2C slave, we need at least
	 * 5ms delay for the slave to settle down. We also have the
	 * delay after reading the power status as well.
	 */
	switch (slot->state) {
	case FIRENZE_PCI_SLOT_FRESET_WAIT_RSP:
		FIRENZE_PCI_DBG("%016llx FRESET: I2C request completed\n",
				slot->id);
		state = FIRENZE_PCI_SLOT_FRESET_DELAY;
		break;
	case FIRENZE_PCI_SLOT_SPOWER_START:
		FIRENZE_PCI_DBG("%016llx SPOWER: I2C request completed\n",
				slot->id);
		state = FIRENZE_PCI_SLOT_SPOWER_DONE;
		break;
	default:
		FIRENZE_PCI_ERR("Wrong state %08x on slot %016llx\n",
				slot->state, slot->id);
		return;
	}

	/* Switch to net state */
	pci_slot_set_state(slot, state);
}

/* This function is called to setup normal PCI device or PHB slot.
 * For the later case, the slot doesn't have the associated PCI
 * device. Besides, the I2C response timeout is set to 5s. We might
 * improve I2C in future to support priorized requests so that the
 * timeout can be shortened.
 */
static int64_t firenze_pci_slot_freset(struct pci_slot *slot)
{
	struct firenze_pci_slot *plat_slot = slot->data;
	uint8_t *pval, presence = 1;

	switch (slot->state) {
	case FIRENZE_PCI_SLOT_NORMAL:
	case FIRENZE_PCI_SLOT_FRESET_START:
		FIRENZE_PCI_DBG("%016llx FRESET: Starts\n",
				slot->id);

		/* Bail if nothing is connected */
		if (slot->ops.get_presence_state)
			slot->ops.get_presence_state(slot, &presence);
		if (!presence) {
			FIRENZE_PCI_DBG("%016llx FRESET: No device\n",
					slot->id);
			return OPAL_SUCCESS;
		}

		/* Prepare link down */
		if (slot->ops.prepare_link_change) {
			FIRENZE_PCI_DBG("%016llx FRESET: Prepares link down\n",
					slot->id);
			slot->ops.prepare_link_change(slot, false);
		}

		/* Send I2C request */
		FIRENZE_PCI_DBG("%016llx FRESET: Check power state\n",
				slot->id);
		plat_slot->next_state =
			FIRENZE_PCI_SLOT_FRESET_POWER_STATE;
		plat_slot->req->op = SMBUS_READ;
		slot->retries = FIRENZE_PCI_SLOT_RETRIES;
		pci_slot_set_state(slot,
			FIRENZE_PCI_SLOT_FRESET_WAIT_RSP);
		if (pci_slot_has_flags(slot, PCI_SLOT_FLAG_BOOTUP))
			i2c_set_req_timeout(plat_slot->req,
					    FIRENZE_PCI_I2C_TIMEOUT);
		else
			i2c_set_req_timeout(plat_slot->req, 0ul);
		i2c_queue_req(plat_slot->req);
		return pci_slot_set_sm_timeout(slot,
				msecs_to_tb(FIRENZE_PCI_SLOT_DELAY));
	case FIRENZE_PCI_SLOT_FRESET_WAIT_RSP:
		if (slot->retries-- == 0) {
			FIRENZE_PCI_DBG("%016llx FRESET: Timeout waiting for %08x\n",
					slot->id, plat_slot->next_state);
			goto out;
		}

		check_timers(false);
		return pci_slot_set_sm_timeout(slot,
				msecs_to_tb(FIRENZE_PCI_SLOT_DELAY));
	case FIRENZE_PCI_SLOT_FRESET_DELAY:
		FIRENZE_PCI_DBG("%016llx FRESET: Delay %dms on I2C completion\n",
				slot->id, FIRENZE_PCI_SLOT_DELAY);
		pci_slot_set_state(slot, plat_slot->next_state);
		return pci_slot_set_sm_timeout(slot,
				msecs_to_tb(FIRENZE_PCI_SLOT_DELAY));
	case FIRENZE_PCI_SLOT_FRESET_POWER_STATE:
		/* Update last power status */
		pval = (uint8_t *)(plat_slot->req->rw_buf);
		*plat_slot->power_status = *pval;

		/* Power is on, turn it off */
		if (((*pval) & plat_slot->power_mask) == plat_slot->power_on) {
			FIRENZE_PCI_DBG("%016llx FRESET: Power (%02x) on, turn off\n",
					slot->id, *pval);
			(*pval) &= ~plat_slot->power_mask;
			(*pval) |= plat_slot->power_off;
			plat_slot->req->op = SMBUS_WRITE;
			slot->retries = FIRENZE_PCI_SLOT_RETRIES;
			plat_slot->next_state =
				FIRENZE_PCI_SLOT_FRESET_POWER_OFF;
			pci_slot_set_state(slot,
				FIRENZE_PCI_SLOT_FRESET_WAIT_RSP);

			if (pci_slot_has_flags(slot, PCI_SLOT_FLAG_BOOTUP))
				i2c_set_req_timeout(plat_slot->req,
						    FIRENZE_PCI_I2C_TIMEOUT);
			else
				i2c_set_req_timeout(plat_slot->req, 0ul);
			i2c_queue_req(plat_slot->req);
			return pci_slot_set_sm_timeout(slot,
					msecs_to_tb(FIRENZE_PCI_SLOT_DELAY));
		}

		/* Fall through: Power is off, turn it on */
	case FIRENZE_PCI_SLOT_FRESET_POWER_OFF:
		/* Update last power status */
		pval = (uint8_t *)(plat_slot->req->rw_buf);
		*plat_slot->power_status = *pval;

		FIRENZE_PCI_DBG("%016llx FRESET: Power (%02x) off, turn on\n",
				slot->id, *pval);
		(*pval) &= ~plat_slot->power_mask;
		(*pval) |= plat_slot->power_on;
		plat_slot->req->op = SMBUS_WRITE;
		plat_slot->next_state =
			FIRENZE_PCI_SLOT_FRESET_POWER_ON;
		slot->retries = FIRENZE_PCI_SLOT_RETRIES;
		pci_slot_set_state(slot,
			FIRENZE_PCI_SLOT_FRESET_WAIT_RSP);

		if (pci_slot_has_flags(slot, PCI_SLOT_FLAG_BOOTUP))
			i2c_set_req_timeout(plat_slot->req,
					    FIRENZE_PCI_I2C_TIMEOUT);
		else
			i2c_set_req_timeout(plat_slot->req, 0ul);
		i2c_queue_req(plat_slot->req);
		return pci_slot_set_sm_timeout(slot,
				msecs_to_tb(FIRENZE_PCI_SLOT_DELAY));
	case FIRENZE_PCI_SLOT_FRESET_POWER_ON:
		/* Update last power status */
		pval = (uint8_t *)(plat_slot->req->rw_buf);
		*plat_slot->power_status = *pval;

		/* PHB3 slot supports post fundamental reset, we switch
		 * to that. For normal PCI slot, we switch to hot reset
		 * instead.
		 */
		if (slot->ops.pfreset) {
			FIRENZE_PCI_DBG("%016llx FRESET: Switch to PFRESET\n",
					slot->id);
			pci_slot_set_state(slot,
				FIRENZE_PCI_SLOT_PFRESET_START);
			return slot->ops.pfreset(slot);
		} else if (slot->ops.hreset) {
			FIRENZE_PCI_DBG("%016llx FRESET: Switch to HRESET\n",
					slot->id);
			pci_slot_set_state(slot,
				FIRENZE_PCI_SLOT_HRESET_START);
			return slot->ops.hreset(slot);
		}

		pci_slot_set_state(slot, FIRENZE_PCI_SLOT_NORMAL);
		return OPAL_SUCCESS;
	default:
		FIRENZE_PCI_DBG("%016llx FRESET: Unexpected state %08x\n",
				slot->id, slot->state);
	}

out:
	pci_slot_set_state(slot, FIRENZE_PCI_SLOT_NORMAL);
	return OPAL_HARDWARE;
}

static int64_t firenze_pci_slot_perst(struct pci_slot *slot)
{
	struct firenze_pci_slot *plat_slot = slot->data;
	uint8_t presence = 1;
	uint16_t ctrl;

	switch (slot->state) {
	case FIRENZE_PCI_SLOT_NORMAL:
	case FIRENZE_PCI_SLOT_FRESET_START:
		FIRENZE_PCI_DBG("%016llx PERST: Starts\n",
				slot->id);

		/* Bail if nothing is connected */
		if (slot->ops.get_presence_state)
			slot->ops.get_presence_state(slot, &presence);
		if (!presence) {
			FIRENZE_PCI_DBG("%016llx PERST: No device\n",
					slot->id);
			return OPAL_SUCCESS;
		}

		/* Prepare link down */
		if (slot->ops.prepare_link_change) {
			FIRENZE_PCI_DBG("%016llx PERST: Prepare link down\n",
					slot->id);
			slot->ops.prepare_link_change(slot, false);
		}

		/* Assert PERST */
		FIRENZE_PCI_DBG("%016llx PERST: Assert\n",
				slot->id);
		pci_cfg_read16(slot->phb, slot->pd->bdfn,
			       plat_slot->perst_reg, &ctrl);
		ctrl |= plat_slot->perst_bit;
		pci_cfg_write16(slot->phb, slot->pd->bdfn,
				plat_slot->perst_reg, ctrl);
		pci_slot_set_state(slot,
			FIRENZE_PCI_SLOT_PERST_DEASSERT);
		return pci_slot_set_sm_timeout(slot, msecs_to_tb(250));
	case FIRENZE_PCI_SLOT_PERST_DEASSERT:
		/* Deassert PERST */
		pci_cfg_read16(slot->phb, slot->pd->bdfn,
			       plat_slot->perst_reg, &ctrl);
		ctrl &= ~plat_slot->perst_bit;
		pci_cfg_write16(slot->phb, slot->pd->bdfn,
				plat_slot->perst_reg, ctrl);
		pci_slot_set_state(slot,
			FIRENZE_PCI_SLOT_PERST_DELAY);
		return pci_slot_set_sm_timeout(slot, msecs_to_tb(1500));
	case FIRENZE_PCI_SLOT_PERST_DELAY:
		/*
		 * Switch to post fundamental reset if the slot supports
		 * that. Otherwise, we issue a proceeding hot reset on
		 * the slot.
		 */
		if (slot->ops.pfreset) {
			FIRENZE_PCI_DBG("%016llx PERST: Switch to PFRESET\n",
					slot->id);
			pci_slot_set_state(slot,
				FIRENZE_PCI_SLOT_PFRESET_START);
			return slot->ops.pfreset(slot);
		} else if (slot->ops.hreset) {
			FIRENZE_PCI_DBG("%016llx PERST: Switch to HRESET\n",
					slot->id);
			pci_slot_set_state(slot,
				FIRENZE_PCI_SLOT_HRESET_START);
			return slot->ops.hreset(slot);
		}

		pci_slot_set_state(slot, FIRENZE_PCI_SLOT_NORMAL);
		return OPAL_SUCCESS;
	default:
		FIRENZE_PCI_DBG("%016llx PERST: Unexpected state %08x\n",
				slot->id, slot->state);
	}

	pci_slot_set_state(slot, FIRENZE_PCI_SLOT_NORMAL);
	return OPAL_HARDWARE;
}

static int64_t firenze_pci_slot_get_power_state(struct pci_slot *slot,
						uint8_t *val)
{
	if (slot->state != FIRENZE_PCI_SLOT_NORMAL)
		FIRENZE_PCI_ERR("%016llx GPOWER: Unexpected state %08x\n",
				slot->id, slot->state);

	*val = slot->power_state;
	return OPAL_SUCCESS;
}

static int64_t firenze_pci_slot_set_power_state(struct pci_slot *slot,
						uint8_t val)
{
	struct firenze_pci_slot *plat_slot = slot->data;
	uint8_t *pval;

	if (slot->state != FIRENZE_PCI_SLOT_NORMAL)
		FIRENZE_PCI_ERR("%016llx SPOWER: Unexpected state %08x\n",
				slot->id, slot->state);

	if (val != PCI_SLOT_POWER_OFF && val != PCI_SLOT_POWER_ON)
		return OPAL_PARAMETER;

	if (slot->power_state == val)
		return OPAL_SUCCESS;

	slot->power_state = val;
	pci_slot_set_state(slot, FIRENZE_PCI_SLOT_SPOWER_START);

	plat_slot->req->op = SMBUS_WRITE;
	pval = (uint8_t *)plat_slot->req->rw_buf;
	if (val == PCI_SLOT_POWER_ON) {
		*pval = *plat_slot->power_status;
		(*pval) &= ~plat_slot->power_mask;
		(*pval) |= plat_slot->power_on;
	} else {
		*pval = *plat_slot->power_status;
		(*pval) &= ~plat_slot->power_mask;
		(*pval) |= plat_slot->power_off;
	}

	if (pci_slot_has_flags(slot, PCI_SLOT_FLAG_BOOTUP))
		i2c_set_req_timeout(plat_slot->req, FIRENZE_PCI_I2C_TIMEOUT);
	else
		i2c_set_req_timeout(plat_slot->req, 0ul);
	i2c_queue_req(plat_slot->req);

	return OPAL_ASYNC_COMPLETION;
}

static struct i2c_bus *firenze_pci_find_i2c_bus(uint8_t chip,
						uint8_t eng,
						uint8_t port)
{
	struct dt_node *np, *child;
	uint32_t reg;

	/* Iterate I2C masters */
	dt_for_each_compatible(dt_root, np, "ibm,power8-i2cm") {
		if (!np->parent ||
		    !dt_node_is_compatible(np->parent, "ibm,power8-xscom"))
			continue;

		/* Check chip index */
		reg = dt_prop_get_u32(np->parent, "ibm,chip-id");
		if (reg != chip)
			continue;

		/* Check I2C master index */
		reg = dt_prop_get_u32(np, "chip-engine#");
		if (reg != eng)
			continue;

		/* Iterate I2C buses */
		dt_for_each_child(np, child) {
			if (!dt_node_is_compatible(child, "ibm,power8-i2c-port"))
				continue;

			/* Check I2C port index */
			reg = dt_prop_get_u32(child, "reg");
			if (reg != port)
				continue;

			reg = dt_prop_get_u32(child, "ibm,opal-id");
			return i2c_find_bus_by_id(reg);
		}
	}

	return NULL;
}

static void firenze_pci_slot_init(struct pci_slot *slot)
{
	struct lxvpd_pci_slot *s = slot->data;
	struct firenze_pci_slot *plat_slot = slot->data;
	struct firenze_pci_slot_info *info = NULL;
	uint32_t vdid;
	uint8_t buddy;
	int i;

	/* Search for PCI slot info */
	for (i = 0; i < ARRAY_SIZE(firenze_pci_slots); i++) {
		if (firenze_pci_slots[i].index == s->slot_index &&
		    !strcmp(firenze_pci_slots[i].label, s->label)) {
			info = &firenze_pci_slots[i];
			break;
		}
	}
	if (!info)
		return;

	/* Search I2C bus for external power mgt */
	buddy = info->buddy;
	plat_slot->i2c_bus = firenze_pci_find_i2c_bus(info->chip_id,
						      info->master_id,
						      info->port_id);
	if (plat_slot->i2c_bus) {
		plat_slot->req = i2c_alloc_req(plat_slot->i2c_bus);
		if (!plat_slot->req)
			plat_slot->i2c_bus = NULL;

		plat_slot->req->dev_addr	= info->slave_addr;
		plat_slot->req->offset_bytes	= 1;
		plat_slot->req->offset		= 0x69;
		plat_slot->req->rw_buf		= plat_slot->i2c_rw_buf;
		plat_slot->req->rw_len		= 1;
		plat_slot->req->completion	= firenze_i2c_req_done;
		plat_slot->req->user_data	= slot;
		switch (info->channel) {
		case 0:
			plat_slot->power_status = &firenze_pci_slots[buddy].power_status;
			plat_slot->power_mask = 0x33;
			plat_slot->power_on   = 0x22;
			plat_slot->power_off  = 0x33;
			break;
		case 1:
			plat_slot->power_status = &firenze_pci_slots[buddy].power_status;
			plat_slot->power_mask = 0xcc;
			plat_slot->power_on   = 0x88;
			plat_slot->power_off  = 0xcc;
			break;
		default:
			FIRENZE_PCI_DBG("%016llx: Invalid channel %d\n",
					slot->id, info->channel);
			plat_slot->i2c_bus = NULL;
		}
	}

	/*
	 * If the slot has external power logic, to override the
	 * default power management methods. Because of the bad
	 * I2C design, the API supplied by I2C is really hard to
	 * be utilized. To figure out power status retrival or
	 * configuration after we have a blocking API for that.
	 */
	if (plat_slot->i2c_bus) {
		slot->ops.freset = firenze_pci_slot_freset;
		slot->ops.get_power_state = firenze_pci_slot_get_power_state;
		slot->ops.set_power_state = firenze_pci_slot_set_power_state;
		FIRENZE_PCI_DBG("%016llx: External power mgt initialized\n",
				slot->id);
	} else if (info->inband_perst) {
		/*
		 * For PLX downstream ports, PCI config register can be
		 * leveraged to do PERST. If the slot doesn't have external
		 * power management stuff, lets try to stick to the PERST
		 * logic if applicable
		 */
		if (slot->pd->dev_type == PCIE_TYPE_SWITCH_DNPORT) {
			pci_cfg_read32(slot->phb, slot->pd->bdfn,
				       PCI_CFG_VENDOR_ID, &vdid);
			switch (vdid) {
			case 0x873210b5:        /* PLX8732 */
			case 0x874810b5:        /* PLX8748 */
				plat_slot->perst_reg = 0x80;
				plat_slot->perst_bit = 0x0400;
				slot->ops.freset = firenze_pci_slot_perst;
				break;
			}
		}
	}

	/*
         * Anyway, the slot has platform specific info. That
         * requires platform specific method to parse it out
         * properly.
         */
	slot->ops.add_properties = lxvpd_add_slot_properties;
}

void firenze_pci_setup_phb(struct phb *phb, unsigned int index)
{
	uint32_t hub_id;
	struct pci_slot *slot;
	struct lxvpd_pci_slot *s;

	/* Grab Hub ID used to parse VPDs */
	hub_id = dt_prop_get_u32_def(phb->dt_node, "ibm,hub-id", 0);

	/* Process the pcie slot entries from the lx vpd lid */
	lxvpd_process_slot_entries(phb, dt_root, hub_id,
				   index, sizeof(struct firenze_pci_slot));

	/* Fixup PHB3 slot */
	slot = phb->slot;
	s = slot ? lxvpd_get_slot(slot) : NULL;
	if (s) {
                lxvpd_extract_info(slot, s);
		firenze_pci_slot_init(slot);
	}
}

static void firenze_pci_i2c_complete(int rc, struct i2c_request *req)
{
	*(int *)req->user_data = rc;
}

static void firenze_pci_do_i2c_byte(uint8_t chip, uint8_t eng, uint8_t port,
				    uint8_t addr, uint8_t reg, uint8_t data)
{
	struct i2c_bus *bus;
	struct i2c_request *req;
	uint8_t verif;
	int rc;

        bus = firenze_pci_find_i2c_bus(chip, eng, port);
        if (!bus) {
                prerror("FIRENZE: Failed to find i2c (%d/%d/%d)\n", chip, eng, port);
                return;
        }
        req = i2c_alloc_req(bus);
        if (!req) {
                prerror("FIRENZE: Failed to allocate i2c request\n");
                return;
        }
        req->op           = SMBUS_WRITE;
        req->dev_addr     = addr >> 1;
        req->offset_bytes = 1;
        req->offset       = reg;
        req->rw_buf       = &data;
        req->rw_len       = 1;
        req->completion   = firenze_pci_i2c_complete;
        req->user_data    = &rc;
        rc = 1;
        i2c_queue_req(req);
        while(rc == 1) {
                time_wait_us(10);
        }
        if (rc != 0) {
                prerror("FIRENZE: I2C error %d writing byte\n", rc);
                return;
        }
        req->op           = SMBUS_READ;
        req->dev_addr     = addr >> 1;
        req->offset_bytes = 1;
        req->offset       = reg;
        req->rw_buf       = &verif;
        req->rw_len       = 1;
        req->completion   = firenze_pci_i2c_complete;
        req->user_data    = &rc;
        rc = 1;
        i2c_queue_req(req);
        while(rc == 1) {
                time_wait_us(10);
        }
        if (rc != 0) {
                prerror("FIRENZE: I2C error %d reading byte\n", rc);
                return;
        }
        if (verif != data) {
                prerror("FIRENZE: I2C miscompare want %02x got %02x\n", data, verif);
        }
}

static void firenze_pci_slot_fixup(struct pci_slot *slot)
{
	uint64_t id;
	const uint32_t *p;
	struct lxvpd_pci_slot *s;

	p = dt_prop_get_def(dt_root, "ibm,vpd-lx-info", NULL);
	if (!p)
		return;

	/* FIXME: support fixup with generic way */
	id = ((uint64_t)p[1] << 32) | p[2];
	id = 0;
	if (id != LX_VPD_2S4U_BACKPLANE &&
	    id != LX_VPD_1S4U_BACKPLANE)
		return;

	s = slot->data;
	if (!s || !s->pluggable)
		return;

	/* Note: We apply the settings twice for C6/C7 but that shouldn't
	 * be a problem
	 */
	if (!strncmp(s->label, "C6 ", 2) ||
	    !strncmp(s->label, "C7 ", 2)) {
		printf("FIRENZE: Fixing power on %s...\n", s->label);
		firenze_pci_do_i2c_byte(0, 1, 0, 0x6a, 0x5e, 0xfa);
		firenze_pci_do_i2c_byte(0, 1, 0, 0x6a, 0x5a, 0xff);
		firenze_pci_do_i2c_byte(0, 1, 0, 0x6a, 0x5b, 0xff);
	} else if (!strncmp(s->label, "C5 ", 2)) {
                printf("FIRENZE: Fixing power on %s...\n", s->label);
		firenze_pci_do_i2c_byte(0, 1, 0, 0x72, 0x5e, 0xfb);
		firenze_pci_do_i2c_byte(0, 1, 0, 0x72, 0x5b, 0xff);
        } else if (!strncmp(s->label, "C3 ", 2)) {
		printf("FIRENZE: Fixing power on %s...\n", s->label);
		firenze_pci_do_i2c_byte(0, 1, 0, 0x74, 0x5e, 0xfb);
		firenze_pci_do_i2c_byte(0, 1, 0, 0x74, 0x5b, 0xff);
        }
}

void firenze_pci_get_slot_info(struct phb *phb, struct pci_device *pd)
{
	struct pci_slot *slot;
	struct lxvpd_pci_slot *s;

	/* Prepare the PCI inventory */
	firenze_pci_add_inventory(phb, pd);

	if (pd->dev_type != PCIE_TYPE_ROOT_PORT &&
	    pd->dev_type != PCIE_TYPE_SWITCH_UPPORT &&
	    pd->dev_type != PCIE_TYPE_SWITCH_DNPORT &&
	    pd->dev_type != PCIE_TYPE_PCIE_TO_PCIX)
		return;

	/* Create PCIe slot */
	slot = pcie_slot_create(phb, pd);
	if (!slot)
		return;

	/* Root complex inherits methods from PHB slot */
	if (!pd->parent && phb->slot)
		memcpy(&slot->ops, &phb->slot->ops, sizeof(struct pci_slot_ops));

	/* Patch PCIe slot */
	s = lxvpd_get_slot(slot);
	if (s) {
		lxvpd_extract_info(slot, s);
		firenze_pci_slot_init(slot);
	}

	/* Fixup the slot's power status */
	firenze_pci_slot_fixup(slot);
}

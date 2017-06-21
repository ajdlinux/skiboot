#include <skiboot.h>
#include <xscom.h>
#include <pci.h>
#include <npu2.h>
#include <npu2-regs.h>
#include <bitutils.h>

bool is_p9dd1(void)
{
	struct proc_chip *chip = next_chip(NULL);

	return chip &&
	       (chip->type == PROC_CHIP_P9_NIMBUS ||
		chip->type == PROC_CHIP_P9_CUMULUS) &&
	       (chip->ec_level & 0xf0) == 0x10;
}

struct npu2_dev *npu2_bdf_to_dev(struct npu2 *p, uint32_t bdfn)
{
	// HACK: verify that this works for function != 0 and doesn't break NVLink
	for (uint32_t i = 0; i < p->total_devices; i++) {
		if (p->devices[i].bdfn >> 3 == bdfn >> 3) {
			return &p->devices[i];
		}
	}
	return NULL;
}

/*
 * We use the indirect method because it uses the same addresses as
 * the MMIO offsets (NPU RING)
 */
static void npu2_scom_set_addr(uint64_t gcid, uint64_t scom_base,
			       uint64_t addr, uint64_t size)
{
	uint64_t isa = is_p9dd1() ? NPU2_DD1_MISC_SCOM_IND_SCOM_ADDR :
				    NPU2_MISC_SCOM_IND_SCOM_ADDR;

	addr = SETFIELD(NPU2_MISC_DA_ADDR, 0ull, addr);
	addr = SETFIELD(NPU2_MISC_DA_LEN, addr, size);
	xscom_write(gcid, scom_base + isa, addr);
}

void npu2_scom_write(uint64_t gcid, uint64_t scom_base,
		     uint64_t reg, uint64_t size,
		     uint64_t val)
{
	uint64_t isd = is_p9dd1() ? NPU2_DD1_MISC_SCOM_IND_SCOM_DATA :
				    NPU2_MISC_SCOM_IND_SCOM_DATA;

	/* prlog(PR_DEBUG, "NPU2: Writing register via SCOM: gcid 0x%llx " */
	/*       "scom_base %llx reg %016llx size %llx val %016llx\n", */
	/*       gcid, scom_base, reg, size, val); */

	npu2_scom_set_addr(gcid, scom_base, reg, size);
	xscom_write(gcid, scom_base + isd, val);
}

uint64_t npu2_scom_read(uint64_t gcid, uint64_t scom_base,
			uint64_t reg, uint64_t size)
{
	uint64_t val;
	uint64_t isd = is_p9dd1() ? NPU2_DD1_MISC_SCOM_IND_SCOM_DATA :
				    NPU2_MISC_SCOM_IND_SCOM_DATA;

	npu2_scom_set_addr(gcid, scom_base, reg, size);
	xscom_read(gcid, scom_base + isd, &val);
	/* prlog(PR_DEBUG, "NPU2: Reading register via SCOM: gcid 0x%llx " */
	/*       "scom_base %llx reg %016llx size %llx val %016llx\n", */
	/*       gcid, scom_base, reg, size, val); */

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

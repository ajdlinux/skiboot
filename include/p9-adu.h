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

void p9_adu_set_pb_hp_mode(uint32_t gcid, uint64_t val);

#define PU_ALTD_ADDR_REG				0x0090000

#define PU_ALTD_CMD_REG					0x0090001
#define   PU_ALTD_CMD_REG_FBC_ALTD_START_OP		PPC_BIT(2)
#define   PU_ALTD_CMD_REG_FBC_ALTD_CLEAR_STATUS		PPC_BIT(3)
#define   PU_ALTD_CMD_REG_FBC_ALTD_RESET_FSM		PPC_BIT(4)
#define   PU_ALTD_CMD_REG_FBC_ALTD_AXTYPE		PPC_BIT(6)
#define   PU_ALTD_CMD_REG_FBC_LOCKED			PPC_BIT(11)
#define   PU_ALTD_CMD_REG_FBC_LOCK_ID			PPC_BITMASK(12, 15)
#define   PU_ALTD_CMD_REG_FBC_ALTD_SCOPE		PPC_BITMASK(16, 18)
#define   PU_ALTD_CMD_REG_FBC_ALTD_SCOPE_SYSTEM		0b101
#define   PU_ALTD_CMD_REG_FBC_ALTD_DROP_PRIORITY	PPC_BIT(20)
#define   PU_ALTD_CMD_REG_FBC_ALTD_WITH_TM_QUIESCE	PPC_BIT(24)
#define   PU_ALTD_CMD_REG_FBC_ALTD_TTYPE		PPC_BITMASK(25, 31)
#define   PU_ALTD_CMD_REG_FBC_ALTD_TTYPE_PMISC_OPER	0b0110001
#define   PU_ALTD_CMD_REG_FBC_ALTD_TSIZE		PPC_BITMASK(32, 39)
#define   PU_ALTD_CMD_REG_FBC_ALTD_TSIZE_PMISC_1	0b00000010

#define PU_ALTD_OPTION_REG				0x0090002
#define   PU_ALTD_OPTION_REG_FBC_ALTD_WITH_PRE_QUIESCE	PPC_BIT(23)
#define   PU_ALTD_OPTION_REG_FBC_ALTD_AFTER_QUIESCE_WAIT_COUNT PPC_BITMASK(28, 47)
#define   PU_ALTD_OPTION_REG_FBC_ALTD_WITH_POST_INIT	PPC_BIT(51)
#define   PU_ALTD_OPTION_REG_FBC_ALTD_WITH_FAST_PATH	PPC_BIT(52)
#define   PU_ALTD_OPTION_REG_FBC_ALTD_BEFORE_INIT_WAIT_COUNT	PPC_BITMASK(54, 63)

#define QUIESCE_SWITCH_WAIT_COUNT			128
#define INIT_SWITCH_WAIT_COUNT				128

#define PU_SND_MODE_REG					0x0090021
#define PU_SND_MODE_REG_ENABLE_PB_SWITCH_AB		PPC_BIT(30)

// Hotplug registers
#define PB_WEST_HP_MODE_NEXT				0x501180B
#define PB_CENT_HP_MODE_NEXT				0x5011C0B
#define PB_EAST_HP_MODE_NEXT				0x501200B
#define PB_WEST_HP_MODE_CURR				0x501180C
#define PB_CENT_HP_MODE_CURR				0x5011C0C
#define PB_EAST_HP_MODE_CURR				0x501200C
#define   PB_HP_MODE_MASTER_CHIP			PPC_BIT(0)

/*
 * Copyright (C) 2019 saybur
 * 
 * This file is part of scuznet.
 * 
 * scuznet is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * scuznet is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with scuznet.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <util/delay.h>
#include "lib/ff/ff.h"
#include "lib/ff/diskio.h"
#include "config.h"
#include "debug.h"
#include "logic.h"
#include "hdd.h"
#include "toolbox.h"

/*
 * Defines the standard response we provide when asked to give INQUIRY data.
 */
#define HDD_INQUIRY_LENGTH 36
static const __flash uint8_t inquiry_data[] = {
	0x00, 0x00, 0x02, 0x02,
	0x1F, 0x00, 0x00, 0x00,
	' ', 's', 'c', 'u', 'z', 'n', 'e', 't',
	' ', 's', 'c', 'u', 'z', 'n', 'e', 't',
	' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', 
	'0', '.', '1', 'a'
};

// track global state of the whole subsystem
static HDDSTATE state = HDD_NOINIT;

// generic buffer for READ/WRITE BUFFER commands
#define MEMORY_BUFFER_OFFSET 600 // from front of global buffer
#define MEMORY_BUFFER_LENGTH 68

/*
 * ============================================================================
 *   UTILITY FUNCTIONS
 * ============================================================================
 */

/*
 * Given a pointer to a four byte array of capacity information, this sets the
 * values to match the current volume.
 */
static void hdd_update_capacity(uint8_t id, uint8_t* arr)
{
	/*
	 * We strip off the low 12 bits to conform with the sizing reported by the
	 * rigid disk geometry page in MODE SENSE, then subtract 1 to get the last
	 * readable block for the command.
	 */
	uint32_t last = (config_hdd[id].size & 0xFFFFF000) - 1;
	arr[0] = (uint8_t) (last >> 24);
	arr[1] = (uint8_t) (last >> 16);
	arr[2] = (uint8_t) (last >> 8);
	arr[3] = (uint8_t) last;
}

/*
 * Seeks to the correct position within a filesystem-backed virtual hard drive
 * unit. This should not be invoked on raw volumes.
 * 
 * Returns true on success and false on failure.
 */
static uint8_t hdd_seek(uint8_t id, uint32_t lba)
{
	FRESULT res = f_lseek(&(config_hdd[id].fp), lba * 512);
	if (res)
	{
		debug_dual(DEBUG_HDD_MEM_SEEK_ERROR, res);
		state = HDD_ERROR;
		logic_set_sense(SENSE_MEDIUM_ERROR, 0);
		logic_status(LOGIC_STATUS_CHECK_CONDITION);
		logic_message_in(LOGIC_MSG_COMMAND_COMPLETE);
		return 0;
	}
	else
	{
		return 1;
	}
}

/*
 * Calls the logic parse function and checks for operation validity.
 * 
 * This bounds-checks versus the known size of the hard drive, either adding
 * the operation length or not depending on whether 'use_length' is true.
 * 
 * Returns true on success or false on failure.
 */
static uint8_t hdd_parse_op(uint8_t id, uint8_t* cmd,
		LogicDataOp* op, uint8_t use_length)
{
	if (! logic_parse_data_op(cmd, op))
	{
		debug(DEBUG_HDD_INVALID_OPERATION);
		logic_status(LOGIC_STATUS_CHECK_CONDITION);
		logic_message_in(LOGIC_MSG_COMMAND_COMPLETE);
		return 0;
	}

	if (use_length)
	{
		if (op->lba + op->length >= config_hdd[id].size)
		{
			debug(DEBUG_HDD_SIZE_EXCEEDED);
			logic_set_sense(SENSE_ILLEGAL_LBA, config_hdd[id].size);
			logic_status(LOGIC_STATUS_CHECK_CONDITION);
			logic_message_in(LOGIC_MSG_COMMAND_COMPLETE);
			return 0;
		}
		else
		{
			return 1;
		}
	}
	else
	{
		if (op->lba >= config_hdd[id].size)
		{
			debug(DEBUG_HDD_SIZE_EXCEEDED);
			logic_set_sense(SENSE_ILLEGAL_LBA, config_hdd[id].size);
			logic_status(LOGIC_STATUS_CHECK_CONDITION);
			logic_message_in(LOGIC_MSG_COMMAND_COMPLETE);
			return 0;
		}
		else
		{
			return 1;
		}
	}
}

/*
 * ============================================================================
 *   OPERATION HANDLERS
 * ============================================================================
 * 
 * Each of these gets called from the _main() function to perform a particular
 * task on either the device or the PHY.
 */

static void hdd_cmd_test_unit_ready()
{
	// no test currently performed, always assume volume is good
	logic_status(LOGIC_STATUS_GOOD);
	logic_message_in(LOGIC_MSG_COMMAND_COMPLETE);
}

static void hdd_cmd_inquiry(uint8_t id, uint8_t* cmd)
{
	(void) id; // silence compiler warning

	// limit size to the requested inquiry length
	uint8_t alloc = cmd[4];
	if (alloc > HDD_INQUIRY_LENGTH)
		alloc = HDD_INQUIRY_LENGTH;

	logic_data_in_pgm(inquiry_data, alloc);
	logic_status(LOGIC_STATUS_GOOD);
	logic_message_in(LOGIC_MSG_COMMAND_COMPLETE);
}

static void hdd_cmd_read_capacity(uint8_t id, uint8_t* cmd)
{
	uint8_t resp[8];

	if (cmd[1] & 1)
	{
		// RelAdr set, we're not playing that game
		logic_cmd_illegal_arg(1);
	}
	else
	{
		// set number of sectors
		hdd_update_capacity(id, resp);
		// sectors fixed at 512 bytes
		resp[4] = 0x00;
		resp[5] = 0x00;
		resp[6] = 0x02;
		resp[7] = 0x00;

		logic_data_in(resp, 8);
		logic_status(LOGIC_STATUS_GOOD);
		logic_message_in(LOGIC_MSG_COMMAND_COMPLETE);
	}
}

/*
 * Minimalistic implementation of the FORMAT UNIT command, supporting only
 * no-arg defect lists.
 * 
 * The flash card handles all this internally so this is likely useless to
 * support anyway.
 */
static void hdd_cmd_format(uint8_t id, uint8_t* cmd)
{
	(void) id; // silence compiler warning

	uint8_t fmt = cmd[1];
	if (fmt == 0x00)
	{
		logic_status(LOGIC_STATUS_GOOD);
		logic_message_in(LOGIC_MSG_COMMAND_COMPLETE);
	}
	else if (fmt == 0x10
			|| fmt == 0x18)
	{
		// read the defect list header
		uint8_t parms[4];
		uint8_t len = logic_data_out(parms, 4);
		if (len != 4)
		{
			// TODO verify if unexpected bus free is appropriate here
			phy_phase(PHY_PHASE_BUS_FREE);
			return;
		}

		// we only support empty lists
		// TODO: should be bother checking the flags?
		if (parms[2] == 0x00 && parms[3] == 0x00)
		{
			logic_status(LOGIC_STATUS_GOOD);
			logic_message_in(LOGIC_MSG_COMMAND_COMPLETE);
		}
		else
		{
			logic_set_sense(SENSE_INVALID_PARAMETER, 2);
			logic_status(LOGIC_STATUS_CHECK_CONDITION);
			logic_message_in(LOGIC_MSG_COMMAND_COMPLETE);
		}
	}
	else
	{
		logic_cmd_illegal_arg(1);
	}
}

static void hdd_cmd_read(uint8_t id, uint8_t* cmd)
{
	LogicDataOp op;
	if (! hdd_parse_op(id, cmd, &op, 1)) return;

	if (op.length > 0)
	{
		if (debug_enabled())
		{
			debug(DEBUG_HDD_READ_STARTING);
			if (debug_verbose())
			{
				debug(DEBUG_HDD_LBA);
				debug(op.lba >> 24);
				debug(op.lba >> 16);
				debug(op.lba >> 8);
				debug(op.lba);
				debug(DEBUG_HDD_LENGTH);
				debug_dual(
					(uint8_t) (op.length >> 8),
					(uint8_t) op.length);
			}
		}
		phy_phase(PHY_PHASE_DATA_IN);

		uint8_t res = 255;
		uint16_t act_len = 0;
		if (config_hdd[id].lba > 0) // low-level access
		{
			uint32_t offset = config_hdd[id].lba + op.lba;
			res = disk_read_multi(0, phy_data_offer_block, offset, op.length);
			if (! res) act_len = op.length;
		}
		else // access via FAT
		{
			// move to correct sector
			if (! hdd_seek(id, op.lba)) return;

			// read from card
			res = f_mread(&(config_hdd[id].fp), phy_data_offer_block,
					op.length, &act_len, 0);
		}

		if (res || act_len != op.length)
		{
			if (debug_enabled())
			{
				debug_dual(DEBUG_HDD_MEM_READ_ERROR, res);
				if (debug_verbose())
				{
					debug(DEBUG_HDD_LENGTH);
					debug_dual(
						(uint8_t) (act_len >> 8),
						(uint8_t) act_len);
				}
			}
			state = HDD_ERROR;
			logic_set_sense(SENSE_MEDIUM_ERROR, 0);
			logic_status(LOGIC_STATUS_CHECK_CONDITION);
			logic_message_in(LOGIC_MSG_COMMAND_COMPLETE);
			return;
		}
	}

	debug(DEBUG_HDD_READ_OKAY);
	logic_status(LOGIC_STATUS_GOOD);
	logic_message_in(LOGIC_MSG_COMMAND_COMPLETE);
}

static void hdd_cmd_write(uint8_t id, uint8_t* cmd)
{
	LogicDataOp op;
	if (! hdd_parse_op(id, cmd, &op, 1)) return;

	if (op.length > 0)
	{
		if (debug_enabled())
		{
			debug(DEBUG_HDD_WRITE_STARTING);
			if (debug_verbose())
			{
				debug(DEBUG_HDD_LBA);
				debug(op.lba >> 24);
				debug(op.lba >> 16);
				debug(op.lba >> 8);
				debug(op.lba);
				debug(DEBUG_HDD_LENGTH);
				debug_dual(
					(uint8_t) (op.length >> 8),
					(uint8_t) op.length);
			}
		}
		phy_phase(PHY_PHASE_DATA_OUT);

		uint8_t res = 255;
		uint16_t act_len = 0;
		if (config_hdd[id].lba > 0) // low-level access
		{
			uint32_t offset = config_hdd[id].lba + op.lba;
			res = disk_write_multi(0, phy_data_ask_block, offset, op.length);
			if (! res) act_len = op.length;
		}
		else // access via FAT
		{
			// move to correct sector
			if (! hdd_seek(id, op.lba)) return;

			// write to card
			res = f_mwrite(&(config_hdd[id].fp), phy_data_ask_block,
					op.length, &act_len);
		}

		if (res || act_len != op.length)
		{
			if (debug_enabled())
			{
				debug_dual(DEBUG_HDD_MEM_WRITE_ERROR, res);
				if (debug_verbose())
				{
					debug(DEBUG_HDD_LENGTH);
					debug_dual(
						(uint8_t) (act_len >> 8),
						(uint8_t) act_len);
				}
			}
			state = HDD_ERROR;
			logic_set_sense(SENSE_MEDIUM_ERROR, 0);
			logic_status(LOGIC_STATUS_CHECK_CONDITION);
			logic_message_in(LOGIC_MSG_COMMAND_COMPLETE);
			return;
		}
	}

	debug(DEBUG_HDD_WRITE_OKAY);
	logic_status(LOGIC_STATUS_GOOD);
	logic_message_in(LOGIC_MSG_COMMAND_COMPLETE);
}

static void hdd_cmd_mode_sense(uint8_t id, uint8_t* cmd)
{
	debug(DEBUG_HDD_MODE_SENSE);

	// extract basic command values
	uint8_t cmd_dbd = cmd[1] & 0x8;
	uint8_t cmd_pc = (cmd[2] & 0xC0) >> 6;
	uint8_t cmd_page = cmd[2] & 0x3F;

	// reset result length values
	uint8_t* buffer = global_buffer;
	buffer[0] = 0;
	buffer[1] = 0;

	// get allocation length and set the block descriptor basics,
	// which vary between the (6) and (10) command variants.
	uint8_t cmd_alloc;
	uint8_t mode_pos;
	if (cmd[0] == 0x5A)
	{
		// allocation length, limit to 8 bits (we never will use more)
		if (cmd[7] > 0)
		{
			cmd_alloc = 255;
		}
		else
		{
			cmd_alloc = cmd[8];
		}

		// header values
		mode_pos = 2;
		buffer[mode_pos++] = 0x00; // default medium
		buffer[mode_pos++] = 0x00; // not write protected

		// reserved
		buffer[mode_pos++] = 0;
		buffer[mode_pos++] = 0;

		// include block descriptor?
		buffer[mode_pos++] = 0;
		if (cmd_dbd)
		{
			buffer[mode_pos++] = 0x00;
		}
		else
		{
			buffer[mode_pos++] = 0x08;
		}
	}
	else
	{
		// allocation length
		cmd_alloc = cmd[4];

		// header values
		mode_pos = 1;
		buffer[mode_pos++] = 0x00; // default medium
		buffer[mode_pos++] = 0x00; // not write protected

		// include block descriptor?
		if (cmd_dbd)
		{
			buffer[mode_pos++] = 0x00;
		}
		else
		{
			buffer[mode_pos++] = 0x08;
		}
	}

	// append block descriptors, if allowed
	if (! cmd_dbd)
	{
		buffer[mode_pos++] = 0x00; // density
		buffer[mode_pos++] = 0x00; // blocks MSB
		buffer[mode_pos++] = 0x00;
		buffer[mode_pos++] = 0x00;
		buffer[mode_pos++] = 0x00; // reserved
		buffer[mode_pos++] = 0x00; // block length MSB
		buffer[mode_pos++] = 0x02;
		buffer[mode_pos++] = 0x00;
	}

	/*
	 * Append pages in descending order as we get to them.
	 */
	uint8_t page_found = 0;

	// R/W error recovery page
	if (cmd_page == 0x01 || cmd_page == 0x3F)
	{
		page_found = 1;

		buffer[mode_pos++] = 0x01;
		buffer[mode_pos++] = 0x0A;
		for (uint8_t i = 0; i < 0x0A; i++)
		{
			buffer[mode_pos++] = 0x00;
		}
	}

	// disconnect/reconnect page
	if (cmd_page == 0x02 || cmd_page == 0x3F)
	{
		buffer[mode_pos++] = 0x02;
		buffer[mode_pos++] = 0x0E;
		for (uint8_t i = 0; i < 0x0E; i++)
		{
			buffer[mode_pos++] = 0x00;
		}
	}

	// format page
	if (cmd_page == 0x03 || cmd_page == 0x3F)
	{
		page_found = 1;

		buffer[mode_pos++] = 0x03;
		buffer[mode_pos++] = 0x16;
		for (uint8_t i = 0; i < 8; i++)
		{
			buffer[mode_pos++] = 0x00;
		}

		// sectors per track, fixed @ 32
		buffer[mode_pos++] = 0x00;
		if (cmd_pc != 0x01)
		{
			buffer[mode_pos++] = 32;
		}
		else
		{
			buffer[mode_pos++] = 0;
		}

		// bytes per sector, fixed @ 512
		if (cmd_pc != 0x01)
		{
			buffer[mode_pos++] = 0x02;
		}
		else
		{
			buffer[mode_pos++] = 0x00;
		}
		buffer[mode_pos++] = 0x00;

		// interleave, fixed @ 1
		buffer[mode_pos++] = 0x00;
		if (cmd_pc != 0x01)
		{
			buffer[mode_pos++] = 0x01;
		}
		else
		{
			buffer[mode_pos++] = 0x00;
		}

		// track skew, cyl skew
		for (uint8_t i = 0; i < 4; i++)
		{
			buffer[mode_pos++] = 0x00;
		}

		// flags in byte 20
		if (cmd_pc != 0x01)
		{
			buffer[mode_pos++] = 0x40; // hard sectors only
		}
		else
		{
			buffer[mode_pos++] = 0x00;
		}

		// remaining reserved bytes
		for (uint8_t i = 0; i < 3; i++)
		{
			buffer[mode_pos++] = 0x00;
		}
	}

	// rigid disk geometry page
	uint8_t cap[4];
	uint8_t cyl[3];
	if (cmd_page == 0x04 || cmd_page == 0x3F)
	{
		page_found = 1;

		/*
		 * We always report 128 heads and 32 sectors per track, so only
		 * cylinder data needs to be reported as a variable based on the
		 * volume capacity. With a fixed 512 byte sector size, this allows
		 * incrementing in 4096 block steps, or 2MB each.
		 */
		hdd_update_capacity(id, cap);
		cyl[0] = cap[0] >> 4;
		cyl[1] = (cap[0] << 4) | (cap[1] >> 4);
		cyl[2] = (cap[1] << 4) | (cap[2] >> 4);

		buffer[mode_pos++] = 0x04;
		buffer[mode_pos++] = 0x16;

		// cylinders
		if (cmd_pc != 0x01)
		{
			buffer[mode_pos++] = cyl[0];
			buffer[mode_pos++] = cyl[1];
			buffer[mode_pos++] = cyl[2];
		}
		else
		{
			buffer[mode_pos++] = 0x00;
			buffer[mode_pos++] = 0x00;
			buffer[mode_pos++] = 0x00;
		}

		// heads
		if (cmd_pc != 0x01)
		{
			buffer[mode_pos++] = 0x80;
		}
		else
		{
			buffer[mode_pos++] = 0x00;
		}

		// disable the next fields by setting to max cyl
		for (uint8_t j = 0; j < 2; j++)
		{
			for (uint8_t i = 0; i < 3; i++)
			{
				if (cmd_pc != 0x01)
				{
					buffer[mode_pos++] = cyl[i];
				}
				else
				{
					buffer[mode_pos++] = 0x00;
				}
			}
		}

		// step rate
		buffer[mode_pos++] = 0x00;
		if (cmd_pc != 0x01)
		{
			buffer[mode_pos++] = 0x01;
		}
		else
		{
			buffer[mode_pos++] = 0x00;
		}

		// defaults for the next values
		for (uint8_t i = 0; i < 6; i++)
		{
			buffer[mode_pos++] = 0x00;
		}

		// medium rotation rate... say, maybe 10,000 RPM?
		if (cmd_pc != 0x01)
		{
			buffer[mode_pos++] = 0x27;
			buffer[mode_pos++] = 0x10;
		}
		else
		{
			buffer[mode_pos++] = 0x00;
			buffer[mode_pos++] = 0x00;
		}

		// defaults for the next values
		for (uint8_t i = 0; i < 2; i++)
		{
			buffer[mode_pos++] = 0x00;
		}
	}

	// cache page
	if (cmd_page == 0x08 || cmd_page == 0x3F)
	{
		page_found = 1;

		buffer[mode_pos++] = 0x08;
		buffer[mode_pos++] = 0x0A;

		if (cmd_pc != 0x01)
		{
			buffer[mode_pos++] = 0x01; // only RCD set, no read cache
		}
		else
		{
			buffer[mode_pos++] = 0x00;
		}

		for (uint8_t i = 1; i < 0x0A; i++)
		{
			buffer[mode_pos++] = 0x00;
		}
	}

#ifdef USE_TOOLBOX
	if (cmd_page == 0x31 || cmd_page == 0x3F)
	{
		page_found = 1;

		buffer[mode_pos++] = 0x31;
		buffer[mode_pos++] = 42;

		for (uint8_t i = 0; i < 42; i++)
		{
			buffer[mode_pos++] = 0x00;
		}
	}
#endif

	// finally, either send or error out, depending on if any page matched.
	if (page_found)
	{
		if (cmd[0] == 0x5A)
		{
			buffer[1] = mode_pos - 2;
		}
		else
		{
			buffer[0] = mode_pos - 1;
		}
		// per spec, do not modify data count based on allocation,
		// just truncate the return
		if (mode_pos > cmd_alloc)
			mode_pos = cmd_alloc;

		logic_data_in(buffer, mode_pos);
		logic_status(LOGIC_STATUS_GOOD);
		logic_message_in(LOGIC_MSG_COMMAND_COMPLETE);
	}
	else
	{
		logic_cmd_illegal_arg(2);
	}
}

static void hdd_cmd_verify(uint8_t id, uint8_t* cmd)
{
	(void) id; // silence compiler warning
	debug(DEBUG_HDD_VERIFY);

	if (cmd[1] & 1)
	{
		// RelAdr set
		logic_cmd_illegal_arg(1);
		return;
	}
	if (cmd[1] & 2)
	{
		/*
		 * This is a dummy operation: we just pretend to care about what the
		 * initiator is asking for, and we don't verify anything: just get
		 * the DATA IN length we need to do and report that everything is OK.
		 */
		phy_phase(PHY_PHASE_DATA_IN);
		uint16_t len = (cmd[7] << 8) | cmd[8];
		for (uint16_t i = 0; i < len; i++)
		{
			for (uint16_t j = 0; j < 512; j++)
			{
				// this will be glacial, but this should be an uncommon
				// operation anyway
				phy_data_ask();
			}
		}
	}

	logic_status(LOGIC_STATUS_GOOD);
	logic_message_in(LOGIC_MSG_COMMAND_COMPLETE);
}

static void hdd_cmd_read_buffer(uint8_t id, uint8_t* cmd)
{
	(void) id; // silence compiler warning

	debug(DEBUG_HDD_READ_BUFFER);
	uint8_t cmd_mode = cmd[1] & 0x7;
	// we only support mode 0
	if (cmd_mode)
	{
		logic_cmd_illegal_arg(1);
		return;
	}

	// figure how long the READ BUFFER needs to be
	uint8_t length;
	if (cmd[6] > 0 || cmd[7] > 0)
	{
		length = 255;
	}
	else
	{
		length = cmd[8];
	}
	if (length > MEMORY_BUFFER_LENGTH)
	{
		length = MEMORY_BUFFER_LENGTH;
	}

	// rewrite the header
	global_buffer[MEMORY_BUFFER_OFFSET] = 0x00;
	global_buffer[MEMORY_BUFFER_OFFSET + 1] = 0x00;
	global_buffer[MEMORY_BUFFER_OFFSET + 2] = 0x00;
	global_buffer[MEMORY_BUFFER_OFFSET + 3] = 0x40;

	// send the data
	logic_data_in(global_buffer + MEMORY_BUFFER_OFFSET, length);
	logic_status(LOGIC_STATUS_GOOD);
	logic_message_in(LOGIC_MSG_COMMAND_COMPLETE);
}

static void hdd_cmd_write_buffer(uint8_t id, uint8_t* cmd)
{
	(void) id; // silence compiler warning

	debug(DEBUG_HDD_WRITE_BUFFER);
	uint8_t cmd_mode = cmd[1] & 0x7;
	// we only support mode 0
	if (cmd_mode)
	{
		logic_cmd_illegal_arg(1);
		return;
	}

	uint8_t length = cmd[8];
	if (cmd[6] > 0
			|| cmd[7] > 0
			|| length > MEMORY_BUFFER_LENGTH - 4)
	{
		// exceeded buffer capacity
		logic_cmd_illegal_arg(6);
		return;
	}
	if (length < 4)
	{
		// too short?
		logic_status(LOGIC_STATUS_GOOD);
		logic_message_in(LOGIC_MSG_COMMAND_COMPLETE);
		return;
	}

	phy_phase(PHY_PHASE_DATA_OUT);
	for (uint8_t i = 0; i < 4; i++)
	{
		phy_data_ask();
	}
	logic_data_out(global_buffer + MEMORY_BUFFER_OFFSET + 4, length);
	logic_status(LOGIC_STATUS_GOOD);
	logic_message_in(LOGIC_MSG_COMMAND_COMPLETE);
}

// hacky version that just accepts without complaint anything
static void hdd_cmd_mode_select(uint8_t id, uint8_t* cmd)
{
	(void) id; // silence compiler warning

	debug(DEBUG_HDD_MODE_SELECT);
	uint8_t length = cmd[4];
	if (length > 0)
	{
		logic_data_out_dummy(length);
	}
	logic_status(LOGIC_STATUS_GOOD);
	logic_message_in(LOGIC_MSG_COMMAND_COMPLETE);
}

static void hdd_cmd_seek(uint8_t id, uint8_t* cmd)
{
	LogicDataOp op;
	if (! hdd_parse_op(id, cmd, &op, 0)) return;

	if (debug_enabled())
	{
		debug(DEBUG_HDD_SEEK);
		if (debug_verbose())
		{
			debug(DEBUG_HDD_LBA);
			debug_dual(
				(uint8_t) (op.lba >> 24),
				(uint8_t) (op.lba >> 16));
			debug_dual(
				(uint8_t) (op.lba >> 8),
				(uint8_t) op.lba);
		}
	}

	if (config_hdd[id].lba > 0) // low-level access
	{
		/*
		 * Consider native access to have "free" seeks due to the very low card
		 * seek time, so just do nothing here and pretend to have seeked.
		 */
	}
	else // access via FAT
	{
		// move to correct sector
		if (! hdd_seek(id, op.lba)) return;
	}

	logic_status(LOGIC_STATUS_GOOD);
	logic_message_in(LOGIC_MSG_COMMAND_COMPLETE);
}

/*
 * ============================================================================
 *   EXTERNAL FUNCTIONS
 * ============================================================================
 */

uint16_t hdd_init(void)
{
	FRESULT res;
	FILINFO fno;
	FIL* fp;
	uint16_t err;

	for (uint8_t i = 0; i < HARD_DRIVE_COUNT; i++)
	{
		err = (i + 1) << 8;
		if (config_hdd[i].id != 255)
		{
			fp = &(config_hdd[i].fp);

			// file should be defined, but double check anyway
			if (config_hdd[i].filename == NULL)
			{
				err += (uint8_t) FR_INT_ERR;
				return err;
			}

			/*
			 * Verify the file exists. If it does not exist, we may have been
			 * asked to create it.
			 */
			res = f_stat(config_hdd[i].filename, &fno);
			if (res == FR_NO_FILE)
			{
				if (config_hdd[i].size > 0)
				{
					/*
					 * File does not exist and we have been asked to create it.
					 * This is done via f_expand() to maximize
					 * sequential-access performance. This will not work well
					 * if the drive is fragmented.
					 */
					config_hdd[i].size &= 0xFFF; // limit to 4GB
					config_hdd[i].size <<= 20; // MB to bytes (for now)
					res = f_open(fp, config_hdd[i].filename,
							FA_CREATE_NEW | FA_WRITE);
					if (res)
					{
						err += (uint8_t) res;
						return err;
					}
					// allocate the space
					res = f_expand(fp, config_hdd[i].size, 1);
					if (res)
					{
						err += (uint8_t) res;
						return err;
					}
					// close the file, we'll be re-opening it later in
					// the normal modes
					f_close(fp);
					if (res)
					{
						err += (uint8_t) res;
						return err;
					}
				}
			}
			else if (res == FR_OK)
			{
				// allow to flow through
			}
			else
			{
				err += (uint8_t) res;
				return err;
			}

			/*
			 * If we flowed through to here, OK to attempt opening the file.
			 */
			res = f_open(fp, config_hdd[i].filename, FA_READ | FA_WRITE);
			if (res)
			{
				err += (uint8_t) res;
				return err;
			}
			config_hdd[i].size = (f_size(fp) >> 9); // store in 512 byte sectors
			if (config_hdd[i].size == 0)
			{
				err += (uint8_t) FR_INVALID_OBJECT;
				return err;
			}
		}
	}

	state = HDD_OK;
	return 0;
}

void hdd_contiguous_check(void)
{
	static uint8_t cont_hdd_id;
	static FSCONTIG cc;
	static FIL fp;

	FRESULT res;

	// block further calls once configured
	if (GLOBAL_CONFIG_REGISTER & GLOBAL_FLAG_HDD_CHECKED) return;

	/*
	 * If this function is called without this flag set, that is a directive
	 * for starting the hard drive check. Set it up.
	 */
	if (! (GLOBAL_CONFIG_REGISTER & GLOBAL_FLAG_HDD_CHECKING))
	{
		GLOBAL_CONFIG_REGISTER |= GLOBAL_FLAG_HDD_CHECKING;
		cont_hdd_id = 0;
		cc.fsz = 0;
	}

	/*
	 * If the remaining filesize is zero, advance to next volume that needs
	 * sizing. Otherwise, perform the per-cycle check.
	 */
	if (cc.fsz == 0)
	{
		while (cont_hdd_id < HARD_DRIVE_COUNT)
		{
			// if this volume is not configured, move to the next one
			if (config_hdd[cont_hdd_id].id == 255)
			{
				cont_hdd_id++;
				continue;
			}

			// otherwise check for fast/forcefast modes
			if (config_hdd[cont_hdd_id].mode == HDD_MODE_FAST)
			{
				/*
				 * Open a new pointer to the file. This violates the FatFs
				 * rules at http://elm-chan.org/fsw/ff/doc/appnote.html#dup by
				 * maintaining more than one open file pointer with FA_WRITE
				 * enabled. However, since all access to the file is via
				 * the special f_mread/f_mwrite objects there should be no use
				 * of the data caches, and the file FAT table entries are never
				 * modified, it *should* be safe to do this here.
				 */
				res = f_open(&fp, config_hdd[cont_hdd_id].filename, FA_READ);
				if (res)
				{
					debug_dual(DEBUG_HDD_CHECK_REJECTED, cont_hdd_id);
					f_close(&fp);
					cont_hdd_id++;
					// allow return, f_open() may have taken too much time
					break;
				}
				res = f_contiguous_setup(&fp, &cc);
				if (res)
				{
					debug_dual(DEBUG_HDD_CHECK_REJECTED, cont_hdd_id);
					f_close(&fp);
					cont_hdd_id++;
				}

				// stop the loop; next time through we'll process or skip
				break;
			}
			else if (config_hdd[cont_hdd_id].mode == HDD_MODE_FORCEFAST)
			{
				// user wants us to enable without checking
				debug_dual(DEBUG_HDD_CHECK_FORCED, cont_hdd_id);
				// find the starting sector for the file
				// see http://elm-chan.org/fsw/ff/doc/expand.html
				FIL* fp_ptr = &(config_hdd[cont_hdd_id].fp);
				config_hdd[cont_hdd_id].lba = fp_ptr->obj.fs->database
						+ fp_ptr->obj.fs->csize * (fp_ptr->obj.sclust - 2);
				if (debug_verbose())
				{
					debug(DEBUG_HDD_LBA);
					debug(config_hdd[cont_hdd_id].lba >> 24);
					debug(config_hdd[cont_hdd_id].lba >> 16);
					debug(config_hdd[cont_hdd_id].lba >> 8);
					debug(config_hdd[cont_hdd_id].lba);
				}
				// and advance to next volume
				cont_hdd_id++;
			}
			else
			{
				// non-fast mode, skip this drive
				cont_hdd_id++;
			}
		}

		// stop further processing once we've exhausted all drives
		if (cont_hdd_id >= HARD_DRIVE_COUNT)
		{
			// checks complete
			GLOBAL_CONFIG_REGISTER &= ~GLOBAL_FLAG_HDD_CHECKING;
			GLOBAL_CONFIG_REGISTER |= GLOBAL_FLAG_HDD_CHECKED;
		}
	}
	else
	{
		res = f_contiguous(&cc);
		if (res)
		{
			// error, file must not be contiguous
			// this will get picked up on during next call
			debug_dual(DEBUG_HDD_CHECK_FAILED, cont_hdd_id);
			cc.fsz = 0;
			// move to next drive
			f_close(&fp);
			cont_hdd_id++;
		}
		else if (cc.fsz == 0)
		{
			// success, file is contiguous!
			debug_dual(DEBUG_HDD_CHECK_SUCCESS, cont_hdd_id);
			// find the starting sector for the file
			// see http://elm-chan.org/fsw/ff/doc/expand.html
			config_hdd[cont_hdd_id].lba = fp.obj.fs->database
					+ fp.obj.fs->csize * (fp.obj.sclust - 2);
			if (debug_verbose())
			{
				debug(DEBUG_HDD_LBA);
				debug(config_hdd[cont_hdd_id].lba >> 24);
				debug(config_hdd[cont_hdd_id].lba >> 16);
				debug(config_hdd[cont_hdd_id].lba >> 8);
				debug(config_hdd[cont_hdd_id].lba);
			}
			// move to next drive
			f_close(&fp);
			cont_hdd_id++;
		}
	}
}

HDDSTATE hdd_state(void)
{
	return state;
}

uint8_t hdd_main(uint8_t id)
{
	if (! logic_ready()) return 0;
	if (id >= HARD_DRIVE_COUNT) return 0;
	if (config_hdd[id].id == 255) return 0;

	uint8_t cmd[10];
	logic_start(id + 1, 1); // logic ID 0 for the link device, hence +1
	if (! logic_command(cmd)) return 1; // takes care of disconnection on fail

	/*
	 * If there is a subsystem problem, we prevent further calls to commands,
	 * except those that are supposed to reply unless there is a critical
	 * problem.
	 */
	if (! (cmd[0] == 0x03 || cmd[0] == 0x12))
	{
		if (state == HDD_OK)
		{
			// no issue, allow flow to continue
		}
		else if (state == HDD_NOINIT)
		{
			// system is still becoming ready
			debug(DEBUG_HDD_NOT_READY);
			logic_set_sense(SENSE_BECOMING_READY, 0);
			logic_status(LOGIC_STATUS_CHECK_CONDITION);
			logic_message_in(LOGIC_MSG_COMMAND_COMPLETE);
			return 1;
		}
		else
		{
			// general error
			logic_set_sense(SENSE_HARDWARE_ERROR, 0);
			logic_status(LOGIC_STATUS_CHECK_CONDITION);
			logic_message_in(LOGIC_MSG_COMMAND_COMPLETE);
			return 1;
		}
	}

#ifdef USE_TOOLBOX
	if (toolbox_main(cmd))
	{
		logic_done();
		return 1;
	}
#endif

	switch (cmd[0])
	{
		case 0x04: // FORMAT UNIT
			hdd_cmd_format(id, cmd);
			break;
		case 0x12: // INQUIRY
			hdd_cmd_inquiry(id, cmd);
			break;
		case 0x08: // READ(6)
		case 0x28: // READ(10)
			hdd_cmd_read(id, cmd);
			break;
		case 0x25: // READ CAPACITY
			hdd_cmd_read_capacity(id, cmd);
			break;
		case 0x17: // RELEASE
			logic_status(LOGIC_STATUS_GOOD);
			logic_message_in(LOGIC_MSG_COMMAND_COMPLETE);
			break;
		case 0x03: // REQUEST SENSE
			logic_request_sense(cmd);
			break;
		case 0x16: // RESERVE
			logic_status(LOGIC_STATUS_GOOD);
			logic_message_in(LOGIC_MSG_COMMAND_COMPLETE);
			break;
		case 0x1D: // SEND DIAGNOSTIC
			logic_send_diagnostic(cmd);
			break;
		case 0x0B: // SEEK(6)
		case 0x2B: // SEEK(10)
			hdd_cmd_seek(id, cmd);
			break;
		case 0x00: // TEST UNIT READY
			hdd_cmd_test_unit_ready(id, cmd);
			break;
		case 0x0A: // WRITE(6)
		case 0x2A: // WRITE(10)
			hdd_cmd_write(id, cmd);
			break;
		case 0x1A: // MODE SENSE(6)
		case 0x5A: // MODE SENSE(10)
			hdd_cmd_mode_sense(id, cmd);
			break;
		case 0x15: // MODE SELECT(6)
			hdd_cmd_mode_select(id, cmd);
			break;
		case 0x2F: // VERIFY
			hdd_cmd_verify(id, cmd);
			break;
		case 0x3C: // READ BUFFER
			hdd_cmd_read_buffer(id, cmd);
			break;
		case 0x3B: // WRITE BUFFER
			hdd_cmd_write_buffer(id, cmd);
			break;
		default:
			logic_cmd_illegal_op(cmd[0]);
	}
	logic_done();
	return 1;
}

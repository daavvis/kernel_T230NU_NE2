/*
 *  Copyright (C) 2014, Samsung Electronics Co. Ltd. All Rights Reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/input/mxts.h>
#include <asm/unaligned.h>
#include <linux/firmware.h>
#include <linux/string.h>
#if defined(CONFIG_PM_RUNTIME)
#include <linux/pm_runtime.h>
#endif
#include <linux/regulator/consumer.h>
#ifdef CONFIG_OF
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#endif
#include <linux/printk.h>
#ifdef CONFIG_MACH_PXA_SAMSUNG
#include <linux/sec-common.h>
#endif

/* PMIC Regulator based supply to TSP */
#define REGULATOR_SUPPLY        1
/* gpio controlled LDO based supply to TSP */
#define LDO_SUPPLY              0

#if CHECK_ANTITOUCH
#define MXT_T61_TIMER_ONESHOT		0
#define MXT_T61_TIMER_REPEAT		1
#define MXT_T61_TIMER_CMD_START		1
#define MXT_T61_TIMER_CMD_STOP		2
#endif


#if ENABLE_TOUCH_KEY
int tsp_keycodes[NUMOFKEYS] = {
	KEY_MENU,
	KEY_BACK,
};
char *tsp_keyname[NUMOFKEYS] = {
	"Menu",
	"Back",
};
static u16 tsp_keystatus;
#endif

static int calibrate_chip(struct mxt_data *data);

static int mxt_read_mem(struct mxt_data *data, u16 reg, u8 len, void *buf)
{
	int ret = 0, i = 0;
	u16 le_reg = cpu_to_le16(reg);
	struct i2c_msg msg[2] = {
		{
			.addr = data->client->addr,
			.flags = 0,
			.len = 2,
			.buf = (u8 *)&le_reg,
		},
		{
			.addr = data->client->addr,
			.flags = I2C_M_RD,
			.len = len,
			.buf = buf,
		},
	};

#if TSP_USE_ATMELDBG
	if (data->atmeldbg.block_access)
		return 0;
#endif

	for (i = 0; i < 3; i++) {
		ret = i2c_transfer(data->client->adapter, msg, 2);
		if (ret < 0)
			pr_err("%s fail[%d] address[0x%x]\n",
					__func__, ret, le_reg);
		else
			break;
	}

	return (ret == 2) ? 0 : -EIO;
}

static int mxt_write_mem(struct mxt_data *data, u16 reg, u8 len, const u8 *buf)
{
	int ret = 0, i = 0;
	u8 tmp[len + 2];

#if TSP_USE_ATMELDBG
	if (data->atmeldbg.block_access)
		return 0;
#endif

	put_unaligned_le16(cpu_to_le16(reg), tmp);
	memcpy(tmp + 2, buf, len);

	for (i = 0; i < 3; i++) {
		ret = i2c_master_send(data->client, tmp, sizeof(tmp));
		if (ret < 0)
			pr_err("%s %d times write error on address[0x%x,0x%x]\n",
					__func__, i, tmp[1], tmp[0]);
		else
			break;
	}

	return (ret == sizeof(tmp)) ? 0 : -EIO;
}

static struct mxt_object *mxt_get_object(struct mxt_data *data, u8 type)
{
	struct mxt_object *object;
	int i;

	if (!data->objects)
		return NULL;

	for (i = 0; i < data->info.object_num; i++) {
		object = data->objects + i;
		if (object->type == type)
			return object;
	}

	pr_err("Invalid object type T%d\n", type);

	return NULL;
}

static int mxt_read_message(struct mxt_data *data, struct mxt_message *message)
{
	struct mxt_object *object;

	object = mxt_get_object(data, MXT_GEN_MESSAGEPROCESSOR_T5);
	if (!object)
		return -EINVAL;

	return mxt_read_mem(data, object->start_address,
			sizeof(struct mxt_message), message);
}

static int mxt_read_message_reportid(struct mxt_data *data,
				struct mxt_message *message, u8 reportid)
{
	int try = 0;
	int error;
	int fail_count;

	fail_count = data->max_reportid * 2;

	while (++try < fail_count) {
		error = mxt_read_message(data, message);
		if (error)
			return error;

		if (message->reportid == 0xff)
			continue;

		if (message->reportid == reportid)
			return 0;
	}

	return -EINVAL;
}

static int mxt_read_object(struct mxt_data *data, u8 type, u8 offset, u8 *val)
{
	struct mxt_object *object;
	int error = 0;

	object = mxt_get_object(data, type);
	if (!object)
		return -EINVAL;

	error = mxt_read_mem(data, object->start_address + offset, 1, val);
	if (error)
		pr_err("Error to read T[%d] offset[%d] val[%d]\n",
				type, offset, *val);

	return error;
}

static int mxt_write_object(struct mxt_data *data,
				 u8 type, u8 offset, u8 val)
{
	struct mxt_object *object;
	int error = 0;
	u16 reg;

	object = mxt_get_object(data, type);
	if (!object)
		return -EINVAL;

	if (offset >= object->size * object->instances) {
		pr_err("Tried to write outside object T%d offset:%d, size:%d\n",
				type, offset, object->size);

		return -EINVAL;
	}

	reg = object->start_address;
	error = mxt_write_mem(data, reg + offset, 1, &val);
	if (error)
		pr_err("Error to write T[%d] offset[%d] val[%d]\n",
				type, offset, val);

	return error;
}

static u32 mxt_make_crc24(u32 crc, u8 byte1, u8 byte2)
{
	static const u32 crcpoly = 0x80001B;
	u32 res;
	u16 data_word;

	data_word = (((u16)byte2) << 8) | byte1;
	res = (crc << 1) ^ (u32)data_word;

	if (res & 0x1000000)
		res ^= crcpoly;

	return res;
}

static int mxt_calculate_infoblock_crc(struct mxt_data *data, u32 *crc_pointer)
{
	u32 crc = 0;
	u8 mem[7 + data->info.object_num * 6];
	int ret;
	int i;

	ret = mxt_read_mem(data, 0, sizeof(mem), mem);

	if (ret)
		return ret;

	for (i = 0; i < sizeof(mem) - 1; i += 2)
		crc = mxt_make_crc24(crc, mem[i], mem[i + 1]);

	*crc_pointer = mxt_make_crc24(crc, mem[i], 0) & 0x00FFFFFF;

	return 0;
}

static int mxt_read_info_crc(struct mxt_data *data, u32 *crc_pointer)
{
	u16 crc_address;
	u8 msg[3];
	int ret;

	/* Read Info block CRC address */
	crc_address = MXT_OBJECT_TABLE_START_ADDRESS +
			data->info.object_num * MXT_OBJECT_TABLE_ELEMENT_SIZE;

	ret = mxt_read_mem(data, crc_address, 3, msg);
	if (ret)
		return ret;

	*crc_pointer = msg[0] | (msg[1] << 8) | (msg[2] << 16);

	return 0;
}

static int mxt_read_config_crc(struct mxt_data *data, u32 *crc)
{
	struct mxt_message message;
	struct mxt_object *object;
	int error;

	object = mxt_get_object(data, MXT_GEN_COMMANDPROCESSOR_T6);
	if (!object)
		return -EIO;

	/* Try to read the config checksum of the existing cfg */
	mxt_write_object(data, MXT_GEN_COMMANDPROCESSOR_T6,
			MXT_COMMAND_REPORTALL, 1);

	/* Read message from command processor, which only has one report ID */
	error = mxt_read_message_reportid(data, &message, object->max_reportid);
	if (error) {
		pr_err("Failed to retrieve CRC\n");
		return error;
	}

	/* Bytes 1-3 are the checksum. */
	*crc = message.message[1] | (message.message[2] << 8) |
		(message.message[3] << 16);

	return 0;
}

#if CHECK_ANTITOUCH
void mxt_t61_timer_set(struct mxt_data *data, u8 mode, u8 cmd, u16 msPeriod)
{
	struct mxt_object *object;
	int ret = 0;
	u16 reg;
	u8 buf[5] = {3, 0, 0, 0, 0};

	buf[1] = cmd;
	buf[2] = mode;
	buf[3] = msPeriod & 0xFF;
	buf[4] = (msPeriod >> 8) & 0xFF;

	object = mxt_get_object(data, MXT_SPT_TIMER_T61);
	reg = object->start_address;
	ret = mxt_write_mem(data, reg+0, 5, (const u8*)&buf);

	pr_info("[TSP] T61 Timer Enabled %d\n", msPeriod);
}

void mxt_t8_cal_set(struct mxt_data *data, u8 mstime)
{

	data->pdata->check_autocal = (mstime) ? 1 : 0;

	mxt_write_object(data, MXT_GEN_ACQUISITIONCONFIG_T8, 4, mstime);
}

static unsigned short diff_two_point(u16 x, u16 y, u16 oldx, u16 oldy)
{
	u16 diffx, diffy;
	u16 distance;

	diffx = x-oldx;
	diffy = y-oldy;
	distance = abs(diffx) + abs(diffy);

	return distance;
}

static void mxt_check_coordinate(struct mxt_data *data, u8 detect, u8 id,
				u16 x, u16 y)
{
	int i;
	if (detect) {
		data->tcount[id] = 0;
		data->distance[id] = 0;
	} else {
		data->distance[id] = diff_two_point(x, y,
		data->touchbx[id], data->touchby[id]);

		if (data->distance[id] < 3) {
			if (data->atch_value >= data->tch_value) {
				data->release_max = 3;

				if (data->tcount[id] < 20000)
					data->tcount[id]++;
			} else if ((data->atch_value + data->tch_value) >= 80) {
				data->release_max = 10;

				if (data->tcount[id] < 20000)
					data->tcount[id]++;
			}
		} else
			data->tcount[id] = 0;
	}

	data->touchbx[id] = x;
	data->touchby[id] = y;

	data->max_id = (id >= data->old_id) ? id : data->old_id;
	data->old_id = id;

	if (!data->Press_Release_check) {
		if (data->Report_touch_number > 0) {
			for (i = 0;  i <= data->max_id; i++) {
				if (data->tcount[i] > data->release_max) {
						data->Press_cnt = 0;
						data->Release_cnt = 0;
						data->Press_Release_check = 1;
						data->release_max = 3;
						calibrate_chip(data);
						pr_info("[TSP] Recal for Pattern tracking\n");
				}
			}
		}
	 }
}

#endif	/* CHECK_ANTITOUCH */

static int mxt_check_instance(struct mxt_data *data, u8 type)
{
	int i;

	for (i = 0; i < data->info.object_num; i++) {
		if (data->objects[i].type == type)
			return data->objects[i].instances;
	}

	return 0;
}

static int mxt_init_write_config(struct mxt_data *data, u8 type, const u8 *cfg)
{
	struct mxt_object *object;
	u8 *temp;
	int ret;

	object = mxt_get_object(data, type);
	if (!object)
		return -EINVAL;

	if ((object->size == 0) || (object->start_address == 0)) {
		pr_err("%s error T%d\n", __func__, type);

		return -ENODEV;
	}

	ret = mxt_write_mem(data, object->start_address, object->size, cfg);
	if (ret) {
		pr_err("%s write error T%d address[0x%x]\n", __func__, type, object->start_address);
		return ret;
	}

	if (mxt_check_instance(data, type)) {
		temp = kzalloc(object->size, GFP_KERNEL);

		if (temp == NULL)
			return -ENOMEM;

		ret |= mxt_write_mem(data, object->start_address + object->size,
			object->size, temp);
		kfree(temp);
	}

	return ret;
}

static int mxt_write_config_from_pdata(struct mxt_data *data)
{
	u8 **tsp_config = (u8 **)data->pdata->config;
	u8 i;
	int ret = 0;

	if (!tsp_config) {
		pr_info("No cfg data in pdata\n");
		return 0;
	}

	for (i = 0; tsp_config[i][0] != MXT_RESERVED_T255; i++) {
		ret = mxt_init_write_config(data, tsp_config[i][0], tsp_config[i] + 1);
		if (ret)
			return ret;
	}
	return ret;
}

#if DUAL_CFG
static int mxt_write_config(struct mxt_fw_info *fw_info)
{
	struct mxt_data *data = fw_info->data;
	struct mxt_object *object;
	struct mxt_cfg_data *cfg_data;
	u32 current_crc;
	u8 i, val = 0;
	u16 reg, index;
	int ret;
	u32 cfg_length = data->cfg_len = fw_info->cfg_len / 2;

	if (!fw_info->ta_cfg_raw_data && !fw_info->batt_cfg_raw_data) {
		pr_info("No cfg data in file\n");
		ret = mxt_write_config_from_pdata(data);
		return ret;
	}

	/* Get config CRC from device */
	ret = mxt_read_config_crc(data, &current_crc);
	if (ret)
		return ret;

	/* Check Version information */
	if (fw_info->fw_ver != data->info.version) {
		pr_err("Warning: version mismatch! %s\n", __func__);
		return 0;
	}
	if (fw_info->build_ver != data->info.build) {
		pr_err("Warning: build num mismatch! %s\n", __func__);
		return 0;
	}

	/* Check config CRC */
	if (current_crc == fw_info->cfg_crc) {
		pr_info("Skip writing Config:[CRC 0x%06X]\n", current_crc);
		return 0;
	}

	pr_info("Writing Config:[CRC 0x%06X!=0x%06X]\n", current_crc,
			fw_info->cfg_crc);

	/* Get the address of configuration data */
	data->batt_cfg_raw_data = fw_info->batt_cfg_raw_data;
	data->ta_cfg_raw_data = fw_info->ta_cfg_raw_data =
		fw_info->batt_cfg_raw_data + cfg_length;

	/* Write config info */
	for (index = 0; index < cfg_length;) {
		if (index + sizeof(struct mxt_cfg_data) >= cfg_length) {
			pr_err("index(%d) of cfg_data exceeded total size(%d)!!\n",
					index + sizeof(struct mxt_cfg_data), cfg_length);
			return -EINVAL;
		}

		/* Get the info about each object */
		cfg_data = (struct mxt_cfg_data *)((data->charging_mode) ?
			(&fw_info->ta_cfg_raw_data[index]) : (&fw_info->batt_cfg_raw_data[index]));

		index += sizeof(struct mxt_cfg_data) + cfg_data->size;
		if (index > cfg_length) {
			pr_err("index(%d) of cfg_data exceeded total size(%d) "
					"in T%d object!!\n", index, cfg_length, cfg_data->type);
			return -EINVAL;
		}

		object = mxt_get_object(data, cfg_data->type);
		if (!object) {
			pr_err("T%d is Invalid object type\n", cfg_data->type);
			return -EINVAL;
		}

		/* Check and compare the size, instance of each object */
		if (cfg_data->size > object->size) {
			pr_err("T%d Object length exceeded!\n", cfg_data->type);
			return -EINVAL;
		}
		if (cfg_data->instance >= object->instances) {
			pr_err("T%d Object instances exceeded!\n", cfg_data->type);
			return -EINVAL;
		}

		pr_info("Writing config for obj %d len %d instance %d (%d/%d)\n",
				cfg_data->type, object->size,
				cfg_data->instance, index, cfg_length);

		reg = object->start_address + object->size * cfg_data->instance;

		/* Write register values of each object */
		ret = mxt_write_mem(data, reg, cfg_data->size, cfg_data->register_val);
		if (ret) {
			pr_err("Write T%d Object failed\n", object->type);
			return ret;
		}

		/*
		 * If firmware is upgraded, new bytes may be added to end of
		 * objects. It is generally forward compatible to zero these
		 * bytes - previous behaviour will be retained. However
		 * this does invalidate the CRC and will force a config
		 * download every time until the configuration is updated.
		 */
		if (cfg_data->size < object->size) {
			pr_err("Warning: zeroing %d byte(s) in T%d\n",
					object->size - cfg_data->size, cfg_data->type);

			for (i = cfg_data->size + 1; i < object->size; i++) {
				ret = mxt_write_mem(data, reg + i, 1, &val);
				if (ret)
					return ret;
			}
		}
	}
	pr_info("Updated configuration\n");

	return ret;
}
#else
static int mxt_write_config(struct mxt_fw_info *fw_info)
{
	struct mxt_data *data = fw_info->data;
	struct device *dev = &data->client->dev;
	struct mxt_object *object;
	struct mxt_cfg_data *cfg_data;
	u32 current_crc;
	u8 i, val = 0;
	u16 reg, index;
	int ret;

	if (!fw_info->cfg_raw_data) {
		pr_info("No cfg data in file\n");
		ret = mxt_write_config_from_pdata(data);
		return ret;
	}

	/* Get config CRC from device */
	ret = mxt_read_config_crc(data, &current_crc);
	if (ret)
		return ret;

	/* Check Version information */
	if (fw_info->fw_ver != data->info.version) {
		pr_err("Warning: version mismatch! %s\n", __func__);
		return 0;
	}
	if (fw_info->build_ver != data->info.build) {
		pr_err("Warning: build num mismatch! %s\n", __func__);
		return 0;
	}

	/* Check config CRC */
	if (current_crc == fw_info->cfg_crc) {
		pr_info("Skip writing Config:[CRC 0x%06X]\n", current_crc);
		return 0;
	}

	pr_info("Writing Config:[CRC 0x%06X!=0x%06X]\n", current_crc,
			fw_info->cfg_crc);

	/* Write config info */
	for (index = 0; index < fw_info->cfg_len;) {

		if (index + sizeof(struct mxt_cfg_data) >= fw_info->cfg_len) {
			pr_err("index(%d) of cfg_data exceeded total size(%d)!!\n",
					index + sizeof(struct mxt_cfg_data), fw_info->cfg_len);
			return -EINVAL;
		}

		/* Get the info about each object */
		cfg_data = (struct mxt_cfg_data *)
					(&fw_info->cfg_raw_data[index]);

		index += sizeof(struct mxt_cfg_data) + cfg_data->size;
		if (index > fw_info->cfg_len) {
			pr_err("index(%d) of cfg_data exceeded total size(%d)"
					" in T%d object!!\n", index, fw_info->cfg_len,
					cfg_data->type);
			return -EINVAL;
		}

		object = mxt_get_object(data, cfg_data->type);
		if (!object) {
			pr_err("T%d is Invalid object type\n", cfg_data->type);
			return -EINVAL;
		}

		/* Check and compare the size, instance of each object */
		if (cfg_data->size > object->size) {
			pr_err("T%d Object length exceeded!\n", cfg_data->type);
			return -EINVAL;
		}
		if (cfg_data->instance >= object->instances) {
			pr_err("T%d Object instances exceeded!\n", cfg_data->type);
			return -EINVAL;
		}

		pr_info("Writing config for obj %d len %d instance %d (%d/%d)\n",
				cfg_data->type, object->size,
				cfg_data->instance, index, fw_info->cfg_len);

		reg = object->start_address + object->size * cfg_data->instance;

		/* Write register values of each object */
		ret = mxt_write_mem(data, reg, cfg_data->size, cfg_data->register_val);
		if (ret) {
			pr_err("Write T%d Object failed\n", object->type);

			return ret;
		}

		/*
		 * If firmware is upgraded, new bytes may be added to end of
		 * objects. It is generally forward compatible to zero these
		 * bytes - previous behaviour will be retained. However
		 * this does invalidate the CRC and will force a config
		 * download every time until the configuration is updated.
		 */
		if (cfg_data->size < object->size) {
			pr_err("Warning: zeroing %d byte(s) in T%d\n",
					object->size - cfg_data->size, cfg_data->type);

			for (i = cfg_data->size + 1; i < object->size; i++) {
				ret = mxt_write_mem(data, reg + i, 1, &val);
				if (ret)
					return ret;
			}
		}
	}

	pr_info("Updated configuration\n");

	return ret;
}
#endif

#if TSP_PATCH
#include "mxts_patch.c"
#endif

/* TODO TEMP_ADONIS: need to inspect below functions */
#if TSP_INFORM_CHARGER

static int set_charger_config(struct mxt_data *data)
{
	struct mxt_object *object;
	struct mxt_cfg_data *cfg_data;
	u8 i, val = 0;
	u16 reg, index;
	int ret;

	pr_info("Current state is %s",
		data->charging_mode ? "Charging mode" : "Battery mode");

/* if you need to change configuration depend on chager detection,
 * please insert below line.
 */

	pr_info("set_charger_config data->cfg_len = %d\n", data->cfg_len);

	for (index = 0; index < data->cfg_len;) {
		if (index + sizeof(struct mxt_cfg_data) >= data->cfg_len) {
			pr_err("index(%d) of cfg_data exceeded total size(%d)!!\n",
					index + sizeof(struct mxt_cfg_data),
					data->cfg_len);
			return -EINVAL;
		}

		/* Get the info about each object */
		cfg_data = (struct mxt_cfg_data *)((data->charging_mode) ?
			(&data->ta_cfg_raw_data[index]) : (&data->batt_cfg_raw_data[index]));

		index += sizeof(struct mxt_cfg_data) + cfg_data->size;
		if (index > data->cfg_len) {
			pr_err("index(%d) of cfg_data exceeded total size(%d)"
					" in T%d object!!\n", index, data->cfg_len,
					cfg_data->type);
			return -EINVAL;
		}

		object = mxt_get_object(data, cfg_data->type);
		if (!object) {
			pr_err("T%d is Invalid object type\n", cfg_data->type);
			return -EINVAL;
		}

		/* Check and compare the size, instance of each object */
		if (cfg_data->size > object->size) {
			pr_err("T%d Object length exceeded!\n", cfg_data->type);
			return -EINVAL;
		}
		if (cfg_data->instance >= object->instances) {
			pr_err("T%d Object instances exceeded!\n", cfg_data->type);
			return -EINVAL;
		}

		pr_info("Writing config for obj %d len %d instance %d (%d/%d)\n",
				cfg_data->type, object->size, cfg_data->instance, index,
				data->cfg_len);

		reg = object->start_address + object->size * cfg_data->instance;

		/* Write register values of each object */
		ret = mxt_write_mem(data, reg, cfg_data->size, cfg_data->register_val);
		if (ret) {
			pr_err("Write T%d Object failed\n", object->type);
			return ret;
		}

		/*
		 * If firmware is upgraded, new bytes may be added to end of
		 * objects. It is generally forward compatible to zero these
		 * bytes - previous behaviour will be retained. However
		 * this does invalidate the CRC and will force a config
		 * download every time until the configuration is updated.
		 */
		if (cfg_data->size < object->size) {
			pr_err("Warning: zeroing %d byte(s) in T%d\n",
					object->size - cfg_data->size, cfg_data->type);

			for (i = cfg_data->size + 1; i < object->size; i++) {
				ret = mxt_write_mem(data, reg + i, 1, &val);
				if (ret)
					return ret;
			}
		}
	}

#if TSP_PATCH
	if (data->charging_mode) {
		if (data->patch.event_cnt)
			mxt_patch_test_event(data, 1);
	}
	else {
		if (data->patch.event_cnt)
			mxt_patch_test_event(data, 0);
	}
#endif
	calibrate_chip(data);
	return ret;
}

static void inform_charger(struct tsp_callbacks *cb, bool en)
{
	struct mxt_data *data = container_of(cb, struct mxt_data, callbacks);

	cancel_delayed_work_sync(&data->noti_dwork);
	data->charging_mode = en;
	schedule_delayed_work(&data->noti_dwork, HZ / 5);
}

static void charger_noti_dwork(struct work_struct *work)
{
	struct mxt_data *data = container_of(work, struct mxt_data,
								noti_dwork.work);

	if (!data->mxt_enabled) {
		schedule_delayed_work(&data->noti_dwork, HZ / 5);
		return;
	}

	pr_info("%s mode\n",
		data->charging_mode ? "charging" : "battery");

#if	CHECK_ANTITOUCH
	data->Press_cnt = 0;
	data->Release_cnt = 0;
	data->Press_Release_check = 1;
#endif

	set_charger_config(data);
}

static void inform_charger_init(struct mxt_data *data)
{
	INIT_DELAYED_WORK(&data->noti_dwork, charger_noti_dwork);
}
#endif

static void mxt_report_input_data(struct mxt_data *data)
{
	int i;
	int count = 0;
	int report_count = 0;

	for (i = 0; i < MXT_MAX_FINGER; i++) {
		if (data->fingers[i].state == MXT_STATE_INACTIVE)
			continue;

		input_mt_slot(data->input_dev, i);
		if (data->fingers[i].state == MXT_STATE_RELEASE) {
			input_mt_report_slot_state(data->input_dev, MT_TOOL_FINGER, false);
		} else {
			input_mt_report_slot_state(data->input_dev, MT_TOOL_FINGER, true);
			input_report_abs(data->input_dev, ABS_MT_POSITION_X,
					data->fingers[i].x);
			input_report_abs(data->input_dev, ABS_MT_POSITION_Y,
					data->fingers[i].y);
			input_report_abs(data->input_dev, ABS_MT_TOUCH_MAJOR,
					data->fingers[i].w);
			input_report_abs(data->input_dev, ABS_MT_PRESSURE,
					 data->fingers[i].z);
#if TSP_USE_SHAPETOUCH
			/* Currently revision G firmware do not support it */
			if (data->pdata->revision == MXT_REVISION_I) {
				input_report_abs(data->input_dev, ABS_MT_COMPONENT,
					data->fingers[i].component);
				input_report_abs(data->input_dev, ABS_MT_SUMSIZE,
					data->sumsize);
			}
#endif
			input_report_key(data->input_dev, BTN_TOOL_FINGER, 1);

			if (data->fingers[i].type == MXT_T100_TYPE_HOVERING_FINGER)
				/* hover is reported */
				input_report_key(data->input_dev, BTN_TOUCH, 0);
			else
				/* finger or passive stylus are reported */
				input_report_key(data->input_dev, BTN_TOUCH, 1);
		}
		report_count++;

#if !defined(CONFIG_SAMSUNG_PRODUCT_SHIP)
		if (data->fingers[i].state == MXT_STATE_PRESS)
			pr_info("[P][%d]: T[%d][%d] X[%d],Y[%d]\n",
				i, data->fingers[i].type, data->fingers[i].event,
				data->fingers[i].x, data->fingers[i].y);
#else
		if (data->fingers[i].state == MXT_STATE_PRESS)
			pr_info("[P][%d]: T[%d][%d]\n",
				i, data->fingers[i].type, data->fingers[i].event);
#endif
		else if (data->fingers[i].state == MXT_STATE_RELEASE)
			pr_info("[R][%d]: T[%d][%d] M[%d]\n",
				i, data->fingers[i].type, data->fingers[i].event,
				data->fingers[i].mcount);


		if (data->fingers[i].state == MXT_STATE_RELEASE) {
			data->fingers[i].state = MXT_STATE_INACTIVE;
			data->fingers[i].mcount = 0;
		} else {
			data->fingers[i].state = MXT_STATE_MOVE;
			count++;
		}
	}

	if (count == 0) {
		input_report_key(data->input_dev, BTN_TOUCH, 0);
		input_report_key(data->input_dev, BTN_TOOL_FINGER, 0);
	}

	if (report_count > 0) {
#if TSP_USE_ATMELDBG
		if (!data->atmeldbg.stop_sync)
#endif
			input_sync(data->input_dev);
	}

#if (TSP_USE_SHAPETOUCH || TSP_BOOSTER)
	/* all fingers are released */
	if (count == 0) {
#if TSP_USE_SHAPETOUCH
		data->sumsize = 0;
#endif
#if TSP_BOOSTER
		mxt_set_dvfs_on(data, false);
#endif
	}
#endif

	data->finger_mask = 0;
}

static void mxt_release_all_finger(struct mxt_data *data)
{
	int i;
	int count = 0;

	for (i = 0; i < MXT_MAX_FINGER; i++) {
		if (data->fingers[i].state == MXT_STATE_INACTIVE)
			continue;
		data->fingers[i].z = 0;
		data->fingers[i].state = MXT_STATE_RELEASE;
		count++;
	}

	if (count) {
		pr_err("%s\n", __func__);
		mxt_report_input_data(data);
	}
}

#if TSP_HOVER_WORKAROUND
static void mxt_current_calibration(struct mxt_data *data)
{
	pr_info("%s\n", __func__);

	mxt_write_object(data, MXT_SPT_SELFCAPHOVERCTECONFIG_T102, 1, 1);
}
#endif

static int calibrate_chip(struct mxt_data *data)
{
	int ret = 0;
	/* send calibration command to the chip */
	if (data->cal_busy)
		return ret;

	ret = mxt_write_object(data, MXT_GEN_COMMANDPROCESSOR_T6,
			MXT_COMMAND_CALIBRATE, 1);

	/* set flag for calibration lockup
	recovery if cal command was successful */
	data->cal_busy = 1;
	if (!ret)
		pr_info("[TSP] calibration success!!!\n");
	return ret;
}

#if CHECK_ANTITOUCH
static unsigned short mxt_dist_check(struct mxt_data *data)
{
	int i;
	u16 dist_sum = 0;

	for (i = 0; i <= data->max_id; i++) {
		if (data->distance[i] < 3)
			dist_sum++;
		else
			dist_sum = 0;
	}

	for (i = data->max_id + 1; i < MAX_USING_FINGER_NUM; i++)
		data->distance[i] = 0;

	return dist_sum;
}

static void mxt_tch_atch_area_check(struct mxt_data *data,
		int tch_area, int atch_area, int touch_area)
{
	u16 dist_sum = 0;
	unsigned char touch_num;

	touch_num = data->Report_touch_number;
	if (tch_area) {
		/* First Touch After Calibration */
		if (data->pdata->check_timer == 0) {
			data->coin_check = 0;
			mxt_t61_timer_set(data, MXT_T61_TIMER_ONESHOT,
				MXT_T61_TIMER_CMD_START, 1000);
			data->pdata->check_timer = 1;
		}
	}

	if ((tch_area == 0) && (atch_area > 0)) {
		pr_info("[TSP] T57_Abnormal Status, tch=%d, atch=%d\n",
			data->tch_value, data->atch_value);
		calibrate_chip(data);
		return;
	}

	dist_sum = mxt_dist_check(data);
	if (touch_num > 1 && tch_area <= 45) {
		if (touch_num == 2) {
			if (tch_area < atch_area-3) {
				pr_info("[TSP] Two Cal_Bad : tch area < atch_area-3, tch=%d, atch=%d\n"
				, data->tch_value
				, data->atch_value);
				calibrate_chip(data);
			}
		}
		else if (tch_area <= (touch_num * 4 + 2)) {

			if (!data->coin_check) {
				if (dist_sum == (data->max_id + 1)) {
					if (touch_area < T_AREA_LOW_MT) {
						if (data->t_area_l_cnt >= 7) {
							pr_info("[TSP] Multi Cal maybe bad contion : Set autocal = 5, tch=%d, atch=%d\n"
							, data->tch_value
							, data->atch_value);
							mxt_t8_cal_set(data, 5);
							data->coin_check = 1;
							data->t_area_l_cnt = 0;
						} else {
							data->t_area_l_cnt++;
						}

						data->t_area_cnt = 0;
					} else {
						data->t_area_cnt = 0;
						data->t_area_l_cnt = 0;
					}
				}
			}

		} else {
			if (tch_area < atch_area-2) {
				pr_info("[TSP] Multi Cal_Bad : tch area < atch_area-2 , tch=%d, atch=%d\n"
				, data->tch_value
				, data->atch_value);
				calibrate_chip(data);
			}
		}
	} else if (touch_num  > 1 && tch_area > 48) {
		if (tch_area > atch_area) {
				pr_info("[TSP] Multi Cal_Bad : tch area > atch_area  , tch=%d, atch=%d\n"
				, data->tch_value
				, data->atch_value);
			calibrate_chip(data);
		}

	} else if (touch_num == 1) {
		/* single Touch */
		dist_sum = data->distance[0];
		if ((tch_area < 7) &&
			(atch_area <= 1)) {
			if (!data->coin_check) {
				if (data->distance[0] < 3) {
					if (touch_area < T_AREA_LOW_ST) {
						if (data->t_area_l_cnt >= 7) {
							pr_info("[TSP] Single Floating metal Wakeup suspection :Set autocal = 5, tch=%d, atch=%d\n"
							, data->tch_value
							, data->atch_value);
							mxt_t8_cal_set(data, 5);
							data->coin_check = 1;
							data->t_area_l_cnt = 0;
						} else {
							data->t_area_l_cnt++;
						}
						data->t_area_cnt = 0;

					} else if (touch_area < \
							T_AREA_HIGH_ST) {
						if (data->t_area_cnt >= 7) {
							pr_info("[TSP] Single Floating metal Wakeup suspection :Set autocal = 5, tch=%d, atch=%d\n"
							, data->tch_value
							, data->atch_value);
							mxt_t8_cal_set(data, 5);
							data->coin_check = 1;
							data->t_area_cnt = 0;
						} else {
							data->t_area_cnt++;
						}
						data->t_area_l_cnt = 0;
					} else {
						data->t_area_cnt = 0;
						data->t_area_l_cnt = 0;
					}
				}
			}
		} else if (tch_area > 25) {
			pr_info("[TSP] tch_area > 25, tch=%d, atch=%d\n"
					, data->tch_value, data->atch_value);
			calibrate_chip(data);
		}
	}
}
#endif

static void mxt_treat_T6_object(struct mxt_data *data,
		struct mxt_message *message)
{
	/* Normal mode */
	if (message->message[0] == 0x00) {
		pr_info("Normal mode\n");
		if (data->cal_busy)
			data->cal_busy = 0;
#if TSP_HOVER_WORKAROUND
/* TODO HOVER : Below commands should be removed.
*/
		if (data->pdata->revision == MXT_REVISION_I
			&& data->cur_cal_status) {
			mxt_current_calibration(data);
			data->cur_cal_status = false;
		}
#endif
	}
	/* I2C checksum error */
	if (message->message[0] & 0x04)
		pr_err("I2C checksum error\n");
	/* Config error */
	if (message->message[0] & 0x08)
		pr_err("Config error\n");
	/* Calibration */
	if (message->message[0] & 0x10) {
		pr_info("Calibration is on going !!\n");

#if CHECK_ANTITOUCH
		/* After Calibration */
		data->coin_check = 0;
		mxt_t8_cal_set(data, 0);
		data->pdata->check_antitouch = 1;
		mxt_t61_timer_set(data,
				MXT_T61_TIMER_ONESHOT,
				MXT_T61_TIMER_CMD_STOP, 0);
		data->pdata->check_timer = 0;
		data->pdata->check_calgood = 0;
		data->cal_busy = 1;
		data->finger_area = 0;
#if PALM_CAL///0730
		data->palm_cnt = 0;
#endif

		if (!data->Press_Release_check) {
			pr_info("[TSP] Second Cal check\n");
			data->Press_Release_check = 1;
			data->Press_cnt = 0;
			data->Release_cnt = 0;
			data->release_max = 3;///0730
		}
#endif
	}
	/* Signal error */
	if (message->message[0] & 0x20)
		pr_err("Signal error\n");
	/* Overflow */
	if (message->message[0] & 0x40)
		pr_err("Overflow detected\n");
	/* Reset */
	if (message->message[0] & 0x80) {
		pr_info("Reset is ongoing\n");
#if TSP_INFORM_CHARGER
		if (data->charging_mode)
			set_charger_config(data);
#endif

#if	CHECK_ANTITOUCH
		data->Press_Release_check = 1;
		data->Press_cnt = 0;
		data->Release_cnt = 0;
		data->release_max = 3;
#endif

#if TSP_HOVER_WORKAROUND
/* TODO HOVER : Below commands should be removed.
 * it added just for hover. Current firmware shoud set the acqusition mode
 * with free-run and run current calibration after receive reset command
 * to support hover functionality.
 * it is bug of firmware. and it will be fixed in firmware level.
 */
		if (data->pdata->revision == MXT_REVISION_I) {
			int error = 0;
			u8 value = 0;

			error = mxt_read_object(data,
				MXT_SPT_TOUCHSCREENHOVER_T101, 0, &value);

			if (error) {
				pr_err("Error read hover enable status[%d]\n"
					, error);
			} else {
				if (value)
					data->cur_cal_status = true;
			}
		}
#endif
	}
}

#if ENABLE_TOUCH_KEY
static void mxt_release_all_keys(struct mxt_data *data)
{
	if (tsp_keystatus != TOUCH_KEY_NULL) {
		switch (tsp_keystatus) {
		case TOUCH_KEY_MENU:
			input_report_key(data->input_dev, KEY_MENU,
								KEY_RELEASE);
			break;
		case TOUCH_KEY_BACK:
			input_report_key(data->input_dev, KEY_BACK,
								KEY_RELEASE);
			break;
		default:
			break;
		}
		pr_info("[TSP_KEY] r %s\n",
						tsp_keyname[tsp_keystatus - 1]);
		tsp_keystatus = TOUCH_KEY_NULL;
	}
}


static void mxt_treat_T15_object(struct mxt_data *data,
						struct mxt_message *message)
{
	struct	input_dev *input;
	input = data->input_dev;

	/* single key configuration*/
	if (message->message[MXT_MSG_T15_STATUS] & MXT_MSGB_T15_DETECT) {

		/* defence code, if there is any Pressed key, force release!! */
		if (tsp_keystatus != TOUCH_KEY_NULL)
			mxt_release_all_keys(data);

		switch (message->message[MXT_MSG_T15_KEYSTATE]) {
		case TOUCH_KEY_MENU:
			input_report_key(data->input_dev, KEY_MENU, KEY_PRESS);
			tsp_keystatus = TOUCH_KEY_MENU;
			break;
		case TOUCH_KEY_BACK:
			input_report_key(data->input_dev, KEY_BACK, KEY_PRESS);
			tsp_keystatus = TOUCH_KEY_BACK;
			break;
		default:
			pr_err("[TSP_KEY] abnormal P [%d %d]\n",
				message->message[0], message->message[1]);
			return;
		}

		pr_info("[TSP_KEY] P %s\n",
						tsp_keyname[tsp_keystatus - 1]);
	} else {
		switch (tsp_keystatus) {
		case TOUCH_KEY_MENU:
			input_report_key(data->input_dev, KEY_MENU,
								KEY_RELEASE);
			break;
		case TOUCH_KEY_BACK:
			input_report_key(data->input_dev, KEY_BACK,
								KEY_RELEASE);
			break;
		default:
			pr_err("[TSP_KEY] abnormal R [%d %d]\n",
				message->message[0], message->message[1]);
			return;
		}
		pr_info("[TSP_KEY] R %s\n",
						tsp_keyname[tsp_keystatus - 1]);
		tsp_keystatus = TOUCH_KEY_NULL;
	}
	input_sync(data->input_dev);
	return;
}
#endif

static void mxt_treat_T9_object(struct mxt_data *data,
		struct mxt_message *message)
{
	int id;
	u8 *msg = message->message;

	id = data->reportids[message->reportid].index;

	/* If not a touch event, return */
	if (id >= MXT_MAX_FINGER) {
		pr_err("MAX_FINGER exceeded!\n");
		return;
	}
	if (msg[0] & MXT_RELEASE_MSG_MASK) {
		data->fingers[id].z = 0;
		data->fingers[id].w = msg[4];
		data->fingers[id].state = MXT_STATE_RELEASE;

#if	CHECK_ANTITOUCH
		data->tcount[id] = 0;
		data->distance[id] = 0;
#endif

		mxt_report_input_data(data);
	} else if ((msg[0] & MXT_DETECT_MSG_MASK)
		&& (msg[0] & (MXT_PRESS_MSG_MASK | MXT_MOVE_MSG_MASK))) {
		data->fingers[id].x = (msg[1] << 4) | (msg[3] >> 4);
		data->fingers[id].y = (msg[2] << 4) | (msg[3] & 0xF);
		data->fingers[id].w = msg[4];
		data->fingers[id].z = msg[5];
#if TSP_USE_SHAPETOUCH
		data->fingers[id].component = msg[6];
#endif

		if (data->pdata->max_x < 1024)
			data->fingers[id].x = data->fingers[id].x >> 2;
		if (data->pdata->max_y < 1024)
			data->fingers[id].y = data->fingers[id].y >> 2;

		data->finger_mask |= 1U << id;

		if (msg[0] & MXT_PRESS_MSG_MASK) {
			data->fingers[id].state = MXT_STATE_PRESS;
			data->fingers[id].mcount = 0;

#if	CHECK_ANTITOUCH
			mxt_check_coordinate(data, 1, id,
				data->fingers[id].x,
				data->fingers[id].y);
#endif

		} else if (msg[0] & MXT_MOVE_MSG_MASK) {
			data->fingers[id].mcount += 1;

#if	CHECK_ANTITOUCH
			mxt_check_coordinate(data, 0, id,
				data->fingers[id].x,
				data->fingers[id].y);
#endif
		}

#if TSP_BOOSTER
		mxt_set_dvfs_on(data, true);
#endif
	} else if ((msg[0] & MXT_SUPPRESS_MSG_MASK)
		&& (data->fingers[id].state != MXT_STATE_INACTIVE)) {
		data->fingers[id].z = 0;
		data->fingers[id].w = msg[4];
		data->fingers[id].state = MXT_STATE_RELEASE;
		data->finger_mask |= 1U << id;
	} else {
		/* ignore changed amplitude and vector messsage */
		if (!((msg[0] & MXT_DETECT_MSG_MASK)
				&& (msg[0] & MXT_AMPLITUDE_MSG_MASK
				 || msg[0] & MXT_VECTOR_MSG_MASK)))
			pr_err("Unknown state %#02x %#02x\n",
				msg[0], msg[1]);
	}
}

static void mxt_treat_T42_object(struct mxt_data *data,
		struct mxt_message *message)
{
	(message->message[0] & 0x01) ? pr_info("palm touch detected\n") : pr_info("palm touch released\n");
}

static void mxt_treat_T57_object(struct mxt_data *data,
		struct mxt_message *message)
{

#if CHECK_ANTITOUCH
	u16 tch_area = 0;
	u16 atch_area = 0;
	u16 touch_area_T57 = 0;
	u8 i;

	touch_area_T57 = message->message[0] | (message->message[1] << 8);
	tch_area = message->message[2] | (message->message[3] << 8);
	atch_area = message->message[4] | (message->message[5] << 8);

	data->tch_value  = tch_area;
	data->atch_value = atch_area;
	data->T57_touch = touch_area_T57;
	data->Report_touch_number = 0;

	for (i = 0; i < MXT_MAX_FINGER; i++) {
		if ((data->fingers[i].state != \
			MXT_STATE_INACTIVE) &&
			(data->fingers[i].state != \
			MXT_STATE_RELEASE))
			data->Report_touch_number++;
	}

	if (data->pdata->check_antitouch) {
		mxt_tch_atch_area_check(data,
		tch_area, atch_area, touch_area_T57);
#if  PALM_CAL
		if ((data->Report_touch_number >= 5) && \
			(touch_area_T57 <\
			(data->Report_touch_number * 2) + 2)) {
			if (data->palm_cnt >= 5) {
				data->palm_cnt = 0;
				pr_info("[TSP] Palm Calibration, tch:%d, atch:%d, t57tch:%d\n"
				, tch_area, atch_area
				, touch_area_T57);
				calibrate_chip(data);
			} else {
				data->palm_cnt++;
			}
		} else {
			data->palm_cnt = 0;
		}
#endif
	}

	if (data->pdata->check_calgood == 1) {
		if ((atch_area - tch_area) > 15) {
			if (tch_area < 25) {
				pr_info("[TSP] Cal Not Good1 ,tch:%d, atch:%d, t57tch:%d\n"
				, tch_area, atch_area
				, touch_area_T57);
				calibrate_chip(data);
			}
		}
		if ((tch_area - atch_area) > 48) {
			pr_info("[TSP] Cal Not Good 2 ,tch:%d, atch:%d, t57tch:%d\n"
			, tch_area, atch_area
			, touch_area_T57);
			calibrate_chip(data);
		}
	}
#endif	/* CHECK_ANTITOUCH */

#if TSP_USE_SHAPETOUCH
	data->sumsize = message->message[0] + (message->message[1] << 8);
#endif	/* TSP_USE_SHAPETOUCH */

}
static void mxt_treat_T61_object(struct mxt_data *data,
		struct mxt_message *message)
{

#if  CHECK_ANTITOUCH

	if ((message->message[0] & 0xa0) == 0xa0) {
		if (data->pdata->check_calgood == 1) {
			if (data->Press_cnt == \
			data->Release_cnt) {
				if ((data->tch_value == 0)\
				&& (data->atch_value == 0)) {
					if (data->FirstCal_tch == 0\
					&& data->FirstCal_atch == 0) {
						if (data->FirstCal_t57tch\
						== data->T57_touch) {
							if (\
						data->T57_touch == 0\
						|| data->T57_touch > 12) {
								pr_info("[TSP] CalFail_1 SPT_TIMER_T61 Stop 3sec, tch=%d, atch=%d, t57tch=%d\n"
							, data->tch_value
							, data->atch_value
							, data->T57_touch);
							calibrate_chip(data);
				} else {
					data->pdata->check_calgood = 0;
					data->Press_Release_check = 0;
					data->pdata->check_afterCalgood = 1;
					pr_info("[TSP] CalGood SPT_TIMER_T61 Stop 3sec, tch=%d, atch=%d, t57tch=%d\n"
					, data->tch_value
					, data->atch_value
					, data->T57_touch);
					}
				} else {
					data->pdata->check_calgood = 0;
					data->Press_Release_check = 0;
					data->pdata->check_afterCalgood = 1;
					pr_info("[TSP] CalGood SPT_TIMER_T61 Stop 3sec, tch=%d, atch=%d, t57tch=%d\n"
					, data->tch_value
					, data->atch_value
					, data->T57_touch);
					}
				} else {
					data->pdata->check_calgood = 0;
					data->Press_Release_check = 0;
					data->pdata->check_afterCalgood = 1;
					pr_info("[TSP] CalGood SPT_TIMER_T61 Stop 3sec, tch=%d, atch=%d, t57tch=%d\n"
					, data->tch_value
					, data->atch_value
					, data->T57_touch);
					}
				} else {
						calibrate_chip(data);
						pr_info("[TSP] CalFail_2 SPT_TIMER_T61 Stop 3sec, tch=%d, atch=%d, t57tch=%d\n"
						, data->tch_value
						, data->atch_value
						, data->T57_touch);
					}
				} else {
				if (data->atch_value == 0) {
					if (data->finger_area < 35) {
						calibrate_chip(data);
				pr_info("[TSP] CalFail_3 Press_cnt Fail, tch=%d, atch=%d, t57tch=%d\n"
				, data->tch_value
				, data->atch_value
				, data->T57_touch);
				} else {
				pr_info("[TSP] CalGood Press_cnt Fail, tch=%d, atch=%d, t57tch=%d\n"
				, data->tch_value
				, data->atch_value
				, data->T57_touch);
				data->pdata->check_afterCalgood = 1;
				data->pdata->check_calgood = 0;
				data->Press_Release_check = 0;
				}
			}
			else if (data->atch_value < data->tch_value \
				&& data->Report_touch_number < 4) {
				if (data->Report_touch_number == 2 && \
					data->tch_value > 12 && \
					data->T57_touch >= 1) {
					pr_info("[TSP] CalGood Press_two touch, tch=%d, atch=%d, num=%d, t57tch=%d\n"
					, data->tch_value
					, data->atch_value
					, data->Report_touch_number
					, data->T57_touch);
					data->pdata->check_calgood = 0;
					data->Press_Release_check = 0;
					data->pdata->check_afterCalgood = 1;
				} else if (data->Report_touch_number == 3 \
					&&	data->tch_value > 18\
					&& data->T57_touch > 8) {
						pr_info("[TSP] CalGood Press_three touch, tch=%d, atch=%d, num=%d, t57tch=%d\n"
				, data->tch_value, data->atch_value
				, data->Report_touch_number, data->T57_touch);
				data->pdata->check_calgood = 0;
				data->Press_Release_check = 0;
				data->pdata->check_afterCalgood = 1;
				} else {
				calibrate_chip(data);
				pr_info("[TSP] CalFail_4 Press_cnt Fail, tch=%d, atch=%d, num=%d, t57tch=%d\n"
				, data->tch_value, data->atch_value, \
				data->Report_touch_number, \
				data->T57_touch);
				}
				} else {
				calibrate_chip(data);
				pr_info("[TSP] CalFail_5 Press_cnt Fail, tch=%d, atch=%d, num=%d, t57tch=%d\n"
				, data->tch_value, data->atch_value,\
				data->Report_touch_number,\
				data->T57_touch);
				}
				}
		} else if (data->pdata->check_antitouch) {
			if (data->pdata->check_autocal == 1) {
				pr_info("[TSP] Auto cal is on going - 1sec time restart, tch=%d, atch=%d, t57tch=%d\n"
				, data->tch_value, data->atch_value\
				, data->T57_touch);
				data->pdata->check_timer = 0;
				data->coin_check = 0;
				mxt_t8_cal_set(data, 0);
				mxt_t61_timer_set(data,
				MXT_T61_TIMER_ONESHOT,
				MXT_T61_TIMER_CMD_START,
				1000);
			} else {
				data->pdata->check_antitouch = 0;
				data->pdata->check_timer = 0;
				mxt_t8_cal_set(data, 0);
				data->pdata->check_calgood = 1;
				data->coin_check = 0;
				pr_info("[TSP] First Check Good, tch=%d, atch=%d, t57tch=%d\n"
				, data->tch_value, data->atch_value
				, data->T57_touch);
				data->FirstCal_tch = data->tch_value;
				data->FirstCal_atch = data->atch_value;
				data->FirstCal_t57tch = data->T57_touch;
				mxt_t61_timer_set(data,
				MXT_T61_TIMER_ONESHOT,
				MXT_T61_TIMER_CMD_START,
				3000);
		}
	}
		if (!data->Press_Release_check) {
			if (data->pdata->check_afterCalgood) {
				pr_info("[TSP] CalGood 3sec START\n");
				data->pdata->check_afterCalgood = 0;
				mxt_t61_timer_set(data,
				MXT_T61_TIMER_ONESHOT,
				MXT_T61_TIMER_CMD_START,
				5000);
			} else {
				if (data->tch_value < data->atch_value) {
					calibrate_chip(data);
					pr_info("[TSP] CalFail_6 5sec End, tch=%d, atch=%d, num=%d, t57tch=%d\n"
					, data->tch_value, data->atch_value
					, data->Report_touch_number
					, data->T57_touch);
				} else {
					pr_info("[TSP] CalGood 5sec STOP & Final\n");
					mxt_t61_timer_set(data,
					MXT_T61_TIMER_ONESHOT,
					MXT_T61_TIMER_CMD_STOP, 0);
				}
			}

		}

	}
#endif
}
static void mxt_treat_T100_object(struct mxt_data *data,
		struct mxt_message *message)
{
	u8 id, index;
	u8 *msg = message->message;
	u8 touch_type = 0, touch_event = 0, touch_detect = 0;

	index = data->reportids[message->reportid].index;

	/* Treate screen messages */
	if (index < MXT_T100_SCREEN_MESSAGE_NUM_RPT_ID) {
		if (index == MXT_T100_SCREEN_MSG_FIRST_RPT_ID)
			/* TODO: Need to be implemeted after fixed protocol
			 * This messages will indicate TCHAREA, ATCHAREA
			 */
			pr_info("SCRSTATUS:[%02X] %02X %04X %04X %04X\n",
				 msg[0], msg[1], (msg[3] << 8) | msg[2],
				 (msg[5] << 8) | msg[4],
				 (msg[7] << 8) | msg[6]);
#if TSP_USE_SHAPETOUCH
			data->sumsize = (msg[3] << 8) | msg[2];
#endif	/* TSP_USE_SHAPETOUCH */
		return;
	}

	/* Treate touch status messages */
	id = index - MXT_T100_SCREEN_MESSAGE_NUM_RPT_ID;
	touch_detect = msg[0] >> MXT_T100_DETECT_MSG_MASK;
	touch_type = (msg[0] & 0x70) >> 4;
	touch_event = msg[0] & 0x0F;

	pr_info("TCHSTATUS [%d] : DETECT[%d] TYPE[%d] EVENT[%d] %d,%d,%d,%d,%d\n",
		id, touch_detect, touch_type, touch_event,
		msg[1] | (msg[2] << 8),	msg[3] | (msg[4] << 8),
		msg[5], msg[6], msg[7]);

	switch (touch_type)	{
	case MXT_T100_TYPE_FINGER:
	case MXT_T100_TYPE_PASSIVE_STYLUS:
	case MXT_T100_TYPE_HOVERING_FINGER:
		/* There are no touch on the screen */
		if (!touch_detect) {
			if (touch_event == MXT_T100_EVENT_UP
				|| touch_event == MXT_T100_EVENT_SUPPESS) {

				data->fingers[id].w = 0;
				data->fingers[id].z = 0;
				data->fingers[id].state = MXT_STATE_RELEASE;
				data->fingers[id].type = touch_type;
				data->fingers[id].event = touch_event;

				mxt_report_input_data(data);
			} else {
				pr_err("Untreated Undetectd touch : type[%d], event[%d]\n",
					touch_type, touch_event);
			}
			break;
		}

		/* There are touch on the screen */
		if (touch_event == MXT_T100_EVENT_DOWN
			|| touch_event == MXT_T100_EVENT_UNSUPPRESS
			|| touch_event == MXT_T100_EVENT_MOVE
			|| touch_event == MXT_T100_EVENT_NONE) {

			data->fingers[id].x = msg[1] | (msg[2] << 8);
			data->fingers[id].y = msg[3] | (msg[4] << 8);

			/* AUXDATA[n]'s order is depended on which values are
			 * enabled or not.
			 */
#if TSP_USE_SHAPETOUCH
			data->fingers[id].component = msg[5];
#endif
			data->fingers[id].z = msg[6];
			data->fingers[id].w = msg[7];

			if (touch_type == MXT_T100_TYPE_HOVERING_FINGER) {
				data->fingers[id].w = 0;
				data->fingers[id].z = 0;
			}

			if (touch_event == MXT_T100_EVENT_DOWN
				|| touch_event == MXT_T100_EVENT_UNSUPPRESS) {
				data->fingers[id].state = MXT_STATE_PRESS;
				data->fingers[id].mcount = 0;
			} else {
				data->fingers[id].state = MXT_STATE_MOVE;
				data->fingers[id].mcount += 1;
			}
			data->fingers[id].type = touch_type;
			data->fingers[id].event = touch_event;

			mxt_report_input_data(data);
		} else {
			pr_err("Untreated Detectd touch : type[%d], event[%d]\n",
				touch_type, touch_event);
		}
		break;
	case MXT_T100_TYPE_ACTIVE_STYLUS:
		break;
	}
}

static irqreturn_t mxt_irq_thread(int irq, void *ptr)
{
	struct mxt_data *data = ptr;
	struct i2c_client * client = data->client;
	struct mxt_message message;
	u8 reportid, type;

	do {
		if (mxt_read_message(data, &message)) {
			dev_err(&client->dev, "Failed to read message\n");
			goto end;
		}

#if TSP_USE_ATMELDBG
		if (data->atmeldbg.display_log) {
			print_hex_dump(KERN_INFO, "MXT MSG:",
				DUMP_PREFIX_NONE, 16, 1,
				&message,
				sizeof(struct mxt_message), false);
		}
#endif
		reportid = message.reportid;

		if (reportid > data->max_reportid)
			goto end;

		type = data->reportids[reportid].type;

		switch (type) {
		case MXT_RESERVED_T0:
			goto end;
			break;
		case MXT_GEN_COMMANDPROCESSOR_T6:
			mxt_treat_T6_object(data, &message);
			break;
		case MXT_TOUCH_MULTITOUCHSCREEN_T9:
			mxt_treat_T9_object(data, &message);
			break;
#if ENABLE_TOUCH_KEY
		case MXT_TOUCH_KEYARRAY_T15:
			mxt_treat_T15_object(data, &message);
			break;
#endif
		case MXT_SPT_SELFTEST_T25:
			dev_err(&client->dev, "Self test fail [0x%x 0x%x 0x%x 0x%x]\n",
				message.message[0], message.message[1],
				message.message[2], message.message[3]);
			break;
		case MXT_PROCI_TOUCHSUPPRESSION_T42:
			mxt_treat_T42_object(data, &message);
			break;
		case MXT_PROCI_EXTRATOUCHSCREENDATA_T57:
			mxt_treat_T57_object(data, &message);
			break;
		case MXT_SPT_TIMER_T61:
			mxt_treat_T61_object(data, &message);
			break;
		case MXT_PROCG_NOISESUPPRESSION_T62:
			break;
		case MXT_TOUCH_MULTITOUCHSCREEN_T100:
			mxt_treat_T100_object(data, &message);
			break;

		default:
			pr_info("Untreated Object type[%d]\tmessage[0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x]\n",
				type, message.message[0],
				message.message[1], message.message[2],
				message.message[3], message.message[4],
				message.message[5], message.message[6]);
			break;
		}
#if TSP_PATCH
		mxt_patch_message(data, &message);
#endif
	} while (!data->pdata->read_chg(client));

	if (data->finger_mask)
		mxt_report_input_data(data);
end:
	return IRQ_HANDLED;
}
static void mxts_register_callback(struct tsp_callbacks *tsp_cb)
{

}

static struct regulator *mxts_vdd_regulator;
static struct regulator *mxts_1v8_regulator;

static int mxts_power_setup(struct i2c_client *client, bool onoff)
{
	struct mxt_platform_data *pdata = client->dev.platform_data;
	int min_uv, max_uv;
	int ret = 0;

	dev_info(&client->dev, "%s : Setting up power for mxts\n", __func__);

	if (onoff) {
		if (pdata->mxts_vdd_type == REGULATOR_SUPPLY) {

			if (!mxts_vdd_regulator) {
				mxts_vdd_regulator = regulator_get(&client->dev, "v_tsp_3v3");
				if (IS_ERR(mxts_vdd_regulator)) {
					ret = PTR_ERR(mxts_vdd_regulator);
					dev_err(&client->dev, "%s :  Failed to get mxts_vdd_regulator (%d)\n",
									__func__, ret);
					goto mxts_vdd_get_error;
				}

				min_uv = max_uv = pdata->mxts_vdd_regulator_volt;
				ret = regulator_set_voltage(mxts_vdd_regulator, min_uv, max_uv);
				if (ret < 0) {
					dev_err(&client->dev, "%s :  Failed to set mxts_mxts_vdd_regulator \
						to %d, %d (%d)\n", __func__, min_uv, max_uv, ret);
					regulator_put(mxts_vdd_regulator);
					goto mxts_vdd_set_error;
				}
			}
		}

		if (pdata->mxts_1v8_type == REGULATOR_SUPPLY) {

			if (!mxts_1v8_regulator) {
				mxts_1v8_regulator = regulator_get(&client->dev, "v_tsp_1v8");
				if (IS_ERR(mxts_1v8_regulator)) {
					ret = PTR_ERR(mxts_1v8_regulator);
					dev_err(&client->dev, "%s :  Failed to get mxts_1v8_regulator (%d)\n",
									__func__, ret);
					goto mxts_1v8_get_error;
				}

				min_uv = max_uv = pdata->mxts_1v8_regulator_volt;
				ret = regulator_set_voltage(mxts_1v8_regulator, min_uv, max_uv);
				if (ret < 0) {
					dev_err(&client->dev, "%s :  Failed to set mxts_mxts_1v8_regulator \
						to %d, %d (%d)\n", __func__, min_uv, max_uv, ret);
					regulator_put(mxts_1v8_regulator);
					goto mxts_1v8_set_error;
				}
			}
		}
	}
	else {
		if (mxts_vdd_regulator)
			regulator_put(mxts_vdd_regulator);
		if (mxts_1v8_regulator)
			regulator_put(mxts_1v8_regulator);
	}

	return 0;

mxts_1v8_set_error:
	regulator_put(mxts_1v8_regulator);
mxts_1v8_get_error:
mxts_vdd_set_error:
	regulator_put(mxts_vdd_regulator);
mxts_vdd_get_error:
	return ret;
}

static int mxts_power_onoff(struct i2c_client *client, bool onoff)
{
	struct mxt_platform_data *pdata = client->dev.platform_data;
	int ret = 0;

	if (pdata->mxts_vdd_type == REGULATOR_SUPPLY) {
		ret = (onoff) ? regulator_enable(mxts_vdd_regulator) : regulator_disable(mxts_vdd_regulator);
	}

	if (pdata->mxts_1v8_type == REGULATOR_SUPPLY) {
		ret = (onoff) ? regulator_enable(mxts_1v8_regulator) : regulator_disable(mxts_1v8_regulator);
	}

	if (pdata->mxts_vdd_type == LDO_SUPPLY) {
		if (pdata->gpio_vdd_en) {
			gpio_direction_output(pdata->gpio_vdd_en, onoff);
		}
	}

	if (pdata->mxts_1v8_type == LDO_SUPPLY) {
		if (pdata->gpio_1v8_en) {
			gpio_direction_output(pdata->gpio_1v8_en, onoff);
		}
	}

	dev_info(&client->dev, "%s :  %s\n", __func__, (onoff) ? "on" : "off");

	return ret;
}

static bool mxts_read_chg(struct i2c_client *client)
{
	struct mxt_platform_data *pdata = client->dev.platform_data;

	return gpio_get_value(pdata->gpio_int);
}

static int mxt_get_bootloader_version(struct i2c_client *client, u8 val)
{
	u8 buf[3];

	if (val & MXT_BOOT_EXTENDED_ID) {
		if (i2c_master_recv(client, buf, sizeof(buf)) != sizeof(buf)) {
			dev_err(&client->dev, "%s :  i2c recv failed\n",
				 __func__);
			return -EIO;
		}
		dev_info(&client->dev, "Bootloader ID:%d Version:%d",
			 buf[1], buf[2]);
	} else {
		dev_info(&client->dev, "Bootloader ID:%d",
			 val & MXT_BOOT_ID_MASK);
	}
	return 0;
}

static int mxt_check_bootloader(struct i2c_client *client,
				     unsigned int state)
{
	u8 val;

recheck:
	if (i2c_master_recv(client, &val, 1) != 1) {
		dev_err(&client->dev, "%s :  i2c recv failed\n", __func__);
		return -EIO;
	}

	switch (state) {
	case MXT_WAITING_BOOTLOAD_CMD:
		if (mxt_get_bootloader_version(client, val))
			return -EIO;
		val &= ~MXT_BOOT_STATUS_MASK;
		break;
	case MXT_WAITING_FRAME_DATA:
	case MXT_APP_CRC_FAIL:
		val &= ~MXT_BOOT_STATUS_MASK;
		break;
	case MXT_FRAME_CRC_PASS:
		if (val == MXT_FRAME_CRC_CHECK)
			goto recheck;
		if (val == MXT_FRAME_CRC_FAIL) {
			dev_err(&client->dev, "Bootloader CRC fail\n");
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}

	if (val != state) {
		dev_err(&client->dev, "Invalid bootloader mode state 0x%X\n", val);
		return -EINVAL;
	}

	return 0;
}

static int mxt_unlock_bootloader(struct i2c_client *client)
{
	u8 buf[2] = {MXT_UNLOCK_CMD_LSB, MXT_UNLOCK_CMD_MSB};

	if (i2c_master_send(client, buf, 2) != 2) {
		dev_err(&client->dev, "%s :  i2c send failed\n", __func__);

		return -EIO;
	}

	return 0;
}

static int mxt_fw_write(struct i2c_client *client,
				const u8 *frame_data, unsigned int frame_size)
{
	if (i2c_master_send(client, frame_data, frame_size) != frame_size) {
		dev_err(&client->dev, "%s :  i2c send failed\n", __func__);
		return -EIO;
	}

	return 0;
}

#if DUAL_CFG
int mxt_verify_fw(struct mxt_fw_info *fw_info, const struct firmware *fw)
{
	struct mxt_data *data = fw_info->data;
	struct mxt_fw_image *fw_img;

	if (!fw) {
		pr_err("could not find firmware file\n");
		return -ENOENT;
	}

	fw_img = (struct mxt_fw_image *)fw->data;

	if (le32_to_cpu(fw_img->magic_code) != MXT_FW_MAGIC) {
		/* In case, firmware file only consist of firmware */
		pr_info("Firmware file only consist of raw firmware\n");
		fw_info->fw_len = fw->size;
		fw_info->fw_raw_data = fw->data;
	} else {
		/*
		 * In case, firmware file consist of header,
		 * configuration, firmware.
		 */
		pr_info("Firmware file consist of header, configuration, firmware\n");
		fw_info->fw_ver = fw_img->fw_ver;
		fw_info->build_ver = fw_img->build_ver;
		fw_info->hdr_len = le32_to_cpu(fw_img->hdr_len);
		fw_info->cfg_len = le32_to_cpu(fw_img->cfg_len);
		fw_info->fw_len = le32_to_cpu(fw_img->fw_len);
		fw_info->cfg_crc = le32_to_cpu(fw_img->cfg_crc);

		/* Check the firmware file with header */
		if (fw_info->hdr_len != sizeof(struct mxt_fw_image)
			|| fw_info->hdr_len + fw_info->cfg_len
				+ fw_info->fw_len != fw->size) {
#if TSP_PATCH
			struct patch_header* ppheader;
			u32 ppos = fw_info->hdr_len + fw_info->cfg_len + fw_info->fw_len;
			ppheader = (struct patch_header*)(fw->data + ppos);
			if (ppheader->magic == MXT_PATCH_MAGIC) {
				pr_info("Firmware file has patch size: %d\n", ppheader->size);
				if (ppheader->size) {
					u8* patch = NULL;
					if (!data->patch.patch) {
						kfree(data->patch.patch);
					}
					patch = kzalloc(ppheader->size, GFP_KERNEL);
					memcpy(patch, (u8*)ppheader, ppheader->size);
					data->patch.patch = patch;
				}
			}
			else
#endif
			{
				pr_err("Firmware file is invaild !!hdr size[%d] cfg,fw size[%d,%d] filesize[%d]\n",
					fw_info->hdr_len, fw_info->cfg_len,
					fw_info->fw_len, fw->size);
				return -EINVAL;
#if TSP_PATCH
			}
#endif
		}

		if (!fw_info->cfg_len) {
			pr_err("Firmware file dose not include configuration data\n");
			return -EINVAL;
		}
		if (!fw_info->fw_len) {
			pr_err("Firmware file dose not include raw firmware data\n");
			return -EINVAL;
		}

		/* Get the address of configuration data */
		data->cfg_len = fw_info->cfg_len / 2;
		data->batt_cfg_raw_data = fw_info->batt_cfg_raw_data
			= fw_img->data;
		data->ta_cfg_raw_data = fw_info->ta_cfg_raw_data
			= fw_img->data +  (fw_info->cfg_len / 2);

		/* Get the address of firmware data */
		fw_info->fw_raw_data = fw_img->data + fw_info->cfg_len;

#if TSP_SEC_FACTORY
		data->fdata->fw_ver = fw_info->fw_ver;
		data->fdata->build_ver = fw_info->build_ver;
#endif
	}

	return 0;
}
#else
int mxt_verify_fw(struct mxt_fw_info *fw_info, const struct firmware *fw)
{
	struct mxt_data *data = fw_info->data;
	struct device *dev = &data->client->dev;
	struct mxt_fw_image *fw_img;

	if (!fw) {
		pr_err("could not find firmware file\n");
		return -ENOENT;
	}

	fw_img = (struct mxt_fw_image *)fw->data;

	if (le32_to_cpu(fw_img->magic_code) != MXT_FW_MAGIC) {
		/* In case, firmware file only consist of firmware */
		pr_info("Firmware file only consist of raw firmware\n");
		fw_info->fw_len = fw->size;
		fw_info->fw_raw_data = fw->data;
	} else {
		/*
		 * In case, firmware file consist of header,
		 * configuration, firmware.
		 */
		pr_info("Firmware file consist of header, configuration, firmware\n");
		fw_info->fw_ver = fw_img->fw_ver;
		fw_info->build_ver = fw_img->build_ver;
		fw_info->hdr_len = le32_to_cpu(fw_img->hdr_len);
		fw_info->cfg_len = le32_to_cpu(fw_img->cfg_len);
		fw_info->fw_len = le32_to_cpu(fw_img->fw_len);
		fw_info->cfg_crc = le32_to_cpu(fw_img->cfg_crc);

		/* Check the firmware file with header */
		if (fw_info->hdr_len != sizeof(struct mxt_fw_image)
			|| fw_info->hdr_len + fw_info->cfg_len
				+ fw_info->fw_len != fw->size) {
			pr_err("Firmware file is invaild !!hdr size[%d] cfg,fw size[%d,%d] filesize[%d]\n",
				fw_info->hdr_len, fw_info->cfg_len,
				fw_info->fw_len, fw->size);
			return -EINVAL;
		}

		if (!fw_info->cfg_len) {
			pr_err("Firmware file dose not include configuration data\n");
			return -EINVAL;
		}
		if (!fw_info->fw_len) {
			pr_err("Firmware file dose not include raw firmware data\n");
			return -EINVAL;
		}

		/* Get the address of configuration data */
		fw_info->cfg_raw_data = fw_img->data;

		/* Get the address of firmware data */
		fw_info->fw_raw_data = fw_img->data + fw_info->cfg_len;

#if TSP_SEC_FACTORY
		data->fdata->fw_ver = fw_info->fw_ver;
		data->fdata->build_ver = fw_info->build_ver;
#endif
	}

	return 0;
}
#endif

static int mxt_wait_for_chg(struct mxt_data *data, u16 time)
{
	int timeout_counter = 0;
	struct i2c_client *client = data->client;
	msleep(time);

	if (data->pdata->read_chg) {
		while (data->pdata->read_chg(client)
			&& timeout_counter++ <= 20) {
			msleep(MXT_RESET_INTEVAL_TIME);
			pr_err("Spend %d time waiting for chg_high\n",
				(MXT_RESET_INTEVAL_TIME * timeout_counter)
				 + time);
		}
	}

	return 0;
}

static int mxt_command_reset(struct mxt_data *data, u8 value)
{
	int error;

	mxt_write_object(data, MXT_GEN_COMMANDPROCESSOR_T6,
			MXT_COMMAND_RESET, value);

	error = mxt_wait_for_chg(data, MXT_SW_RESET_TIME);
	if (error)
		pr_err("Not respond after reset command[%d]\n",
			value);

	return error;
}

static int mxt_command_calibration(struct mxt_data *data)
{
	return mxt_write_object(data, MXT_GEN_COMMANDPROCESSOR_T6,
						MXT_COMMAND_CALIBRATE, 1);
}

static int mxt_command_backup(struct mxt_data *data, u8 value)
{
	mxt_write_object(data, MXT_GEN_COMMANDPROCESSOR_T6,
			MXT_COMMAND_BACKUPNV, value);

	msleep(MXT_BACKUP_TIME);

	return 0;
}

static int mxt_flash_fw(struct mxt_fw_info *fw_info)
{
	struct mxt_data *data = fw_info->data;
	struct i2c_client *client = data->client_boot;
	const u8 *fw_data = fw_info->fw_raw_data;
	size_t fw_size = fw_info->fw_len;
	unsigned int frame_size;
	unsigned int pos = 0;
	int ret;

	if (!fw_data) {
		pr_err("firmware data is Null\n");
		return -ENOMEM;
	}

	ret = mxt_check_bootloader(client, MXT_WAITING_BOOTLOAD_CMD);
	if (ret) {
		/*may still be unlocked from previous update attempt */
		ret = mxt_check_bootloader(client, MXT_WAITING_FRAME_DATA);
		if (ret)
			goto out;
	} else {
		pr_info("Unlocking bootloader\n");
		/* Unlock bootloader */
		mxt_unlock_bootloader(client);
	}
	while (pos < fw_size) {
		ret = mxt_check_bootloader(client,
					MXT_WAITING_FRAME_DATA);
		if (ret) {
			pr_err("Fail updating firmware. wating_frame_data err\n");
			goto out;
		}

		frame_size = ((*(fw_data + pos) << 8) | *(fw_data + pos + 1));

		/*
		* We should add 2 at frame size as the the firmware data is not
		* included the CRC bytes.
		*/

		frame_size += 2;

		/* Write one frame to device */
		mxt_fw_write(client, fw_data + pos, frame_size);

		ret = mxt_check_bootloader(client,
						MXT_FRAME_CRC_PASS);
		if (ret) {
			pr_err("Fail updating firmware. frame_crc err\n");
			goto out;
		}

		pos += frame_size;

		pr_info("Updated %d bytes / %zd bytes\n",
				pos, fw_size);

		msleep(20);
	}

	ret = mxt_wait_for_chg(data, MXT_SW_RESET_TIME);
	if (ret) {
		pr_err("Not respond after F/W  finish reset\n");
		goto out;
	}

	pr_info("success updating firmware\n");
out:
	return ret;
}

static void mxt_handle_init_data(struct mxt_data *data)
{
/*
 * Caution : This function is called before backup NV. So If you write
 * register vaules directly without config file in this function, it can
 * be a cause of that configuration CRC mismatch or unintended values are
 * stored in Non-volatile memory in IC. So I would recommed do not use
 * this function except for bring up case. Please keep this in your mind.
 */
	return;
}

static int mxt_read_id_info(struct mxt_data *data)
{
	int ret = 0;
	u8 id[MXT_INFOMATION_BLOCK_SIZE];

	/* Read IC information */
	ret = mxt_read_mem(data, 0, MXT_INFOMATION_BLOCK_SIZE, id);
	if (ret) {
		pr_err("Read fail. IC information\n");
		goto out;
	} else {
		pr_info("family: 0x%x variant: 0x%x version: 0x%x"
			" build: 0x%x matrix X,Y size:  %d,%d"
			" number of obect: %d\n"
			, id[0], id[1], id[2], id[3], id[4], id[5], id[6]);
		data->info.family_id = id[0];
		data->info.variant_id = id[1];
		data->info.version = id[2];
		data->info.build = id[3];
		data->info.matrix_xsize = id[4];
		data->info.matrix_ysize = id[5];
		data->info.object_num = id[6];
	}

out:
	return ret;
}

static int mxt_get_object_table(struct mxt_data *data)
{
	int error;
	int i;
	u16 reg;
	u8 reportid = 0;
	u8 buf[MXT_OBJECT_TABLE_ELEMENT_SIZE];

	for (i = 0; i < data->info.object_num; i++) {
		struct mxt_object *object = data->objects + i;

		reg = MXT_OBJECT_TABLE_START_ADDRESS +
				MXT_OBJECT_TABLE_ELEMENT_SIZE * i;
		error = mxt_read_mem(data, reg,
				MXT_OBJECT_TABLE_ELEMENT_SIZE, buf);
		if (error)
			return error;

		object->type = buf[0];
		object->start_address = (buf[2] << 8) | buf[1];
		/* the real size of object is buf[3]+1 */
		object->size = buf[3] + 1;
		/* the real instances of object is buf[4]+1 */
		object->instances = buf[4] + 1;
		object->num_report_ids = buf[5];

		pr_info(
			"Object:T%d\t\t\t Address:0x%x\tSize:%d\tInstance:%d\tReport Id's:%d\n",
			object->type, object->start_address, object->size,
			object->instances, object->num_report_ids);

		if (object->num_report_ids) {
			reportid += object->num_report_ids * object->instances;
			object->max_reportid = reportid;
		}
	}

	/* Store maximum reportid */
	data->max_reportid = reportid;
	pr_info("maXTouch: %d report ID\n",
			data->max_reportid);

	return 0;
}

static void mxt_make_reportid_table(struct mxt_data *data)
{
	struct mxt_object *objects = data->objects;
	struct mxt_reportid *reportids = data->reportids;
	int i, j;
	int id = 0;

	for (i = 0; i < data->info.object_num; i++) {
		for (j = 0; j < objects[i].num_report_ids *
				objects[i].instances; j++) {
			id++;

			reportids[id].type = objects[i].type;
			reportids[id].index = j;

			pr_info("Report_id[%d]:\tT%d\tIndex[%d]\n",
				id, reportids[id].type, reportids[id].index);
		}
	}
}

static int mxt_initialize(struct mxt_data *data)
{
	u32 read_info_crc = 0, calc_info_crc = 0;
	int ret;

	ret = mxt_read_id_info(data);
	if (ret)
		return ret;

	data->objects = kcalloc(data->info.object_num,
				sizeof(struct mxt_object),
				GFP_KERNEL);
	if (!data->objects) {
		pr_err("Failed to allocate memory\n");
		ret = -ENOMEM;
		goto out;
	}

	/* Get object table infomation */
	ret = mxt_get_object_table(data);
	if (ret)
		goto out;

	data->reportids = kcalloc(data->max_reportid + 1,
			sizeof(struct mxt_reportid),
			GFP_KERNEL);
	if (!data->reportids) {
		pr_err("Failed to allocate memory\n");
		ret = -ENOMEM;
		goto out;
	}

	/* Make report id table */
	mxt_make_reportid_table(data);

	/* Verify the info CRC */
	ret = mxt_read_info_crc(data, &read_info_crc);
	if (ret)
		goto out;

	ret = mxt_calculate_infoblock_crc(data, &calc_info_crc);
	if (ret)
		goto out;

	if (read_info_crc != calc_info_crc) {
		pr_err("Infomation CRC error :[CRC 0x%06X!=0x%06X]\n",
				read_info_crc, calc_info_crc);
		ret = -EFAULT;
		goto out;
	}
	return 0;

out:
	return ret;
}

static int  mxt_rest_initialize(struct mxt_fw_info *fw_info)
{
	struct mxt_data *data = fw_info->data;
	int ret = 0;

	/* Restore memory and stop event handing */
	ret = mxt_command_backup(data, MXT_DISALEEVT_VALUE);
	if (ret) {
		pr_err("Failed Restore NV and stop event\n");
		goto out;
	}

	/* Write config */
	ret = mxt_write_config(fw_info);
	if (ret) {
		pr_err("Failed to write config from file\n");
		goto out;
	}

	/* Handle data for init */
	mxt_handle_init_data(data);

	/* Backup to memory */
	ret = mxt_command_backup(data, MXT_BACKUP_VALUE);
	if (ret) {
		pr_err("Failed backup NV data\n");
		goto out;
	}

	/* Soft reset */
	ret = mxt_command_reset(data, MXT_RESET_VALUE);
	if (ret) {
		pr_err("Failed Reset IC\n");
		goto out;
	}
#if TSP_PATCH
	if (data->patch.patch)
		ret = mxt_patch_init(data, data->patch.patch);
#endif
out:
	return ret;
}

static int mxt_power_on(struct mxt_data *data)
{
/*
 * If do not turn off the power during suspend, you can use deep sleep
 * or disable scan to use T7, T9 Object. But to turn on/off the power
 * is better.
 */
	int ret = 0;
	struct i2c_client *client = data->client;
	if (data->mxt_enabled)
		return 0;

	if (!data->pdata->power_onoff) {
		dev_warn(&client->dev, "Power on function is not defined\n");
		ret = -EINVAL;
		goto out;
	}

	ret = data->pdata->power_onoff(client, true);
	if (ret) {
		dev_err(&client->dev, "Failed to power on\n");
		goto out;
	}

	ret = mxt_wait_for_chg(data, MXT_HW_RESET_TIME);
	if (ret)
		dev_err(&client->dev, "Not respond after H/W reset\n");

	data->mxt_enabled = true;

out:
	return ret;
}

static int mxt_power_off(struct mxt_data *data)
{
	int ret = 0;
	struct i2c_client *client = data->client;

	if (!data->mxt_enabled)
		return 0;

	if (!data->pdata->power_onoff) {
		dev_warn(&client->dev, "Power off function is not defined\n");
		ret = -EINVAL;
		goto out;
	}

	ret = data->pdata->power_onoff(client, false);
	if (ret) {
		dev_err(&client->dev, "Failed to power off\n");
		goto out;
	}

	data->mxt_enabled = false;

out:
	return ret;
}

/* Need to be called by function that is blocked with mutex */
static int mxt_start(struct mxt_data *data)
{
	int ret = 0;

	if (data->mxt_enabled) {
		pr_err("%s. but touch already on\n", __func__);
		return ret;
	}

	ret = mxt_power_on(data);

	if (ret) {
		pr_err("Fail to start touch\n");
	} else {
		if (system_rev == 0) {
			mxt_command_calibration(data);
			pr_err("Force calibration\n");
		}
		enable_irq(data->client->irq);
	}

	return ret;
}

/* Need to be called by function that is blocked with mutex */
static int mxt_stop(struct mxt_data *data)
{
	int ret = 0;

	if (!data->mxt_enabled) {
		pr_err("%s. but touch already off\n", __func__);
		return ret;
	}
	disable_irq(data->client->irq);

	ret = mxt_power_off(data);
	if (ret) {
		pr_err("Fail to stop touch\n");
		goto err_power_off;
	}
	mxt_release_all_finger(data);

#if ENABLE_TOUCH_KEY
	mxt_release_all_keys(data);
#endif

#if TSP_BOOSTER
	mxt_set_dvfs_on(data, false);
#endif
	return 0;

err_power_off:
	enable_irq(data->client->irq);
	return ret;
}

static int mxt_make_highchg(struct mxt_data *data)
{
	struct mxt_message message;
	int count = data->max_reportid * 2;
	int error;

	/* Read dummy message to make high CHG pin */
	do {
		error = mxt_read_message(data, &message);
		if (error)
			return error;
	} while (message.reportid != 0xff && --count);

	if (!count) {
		pr_err("CHG pin isn't cleared\n");
		return -EBUSY;
	}

	return 0;
}

static int mxt_touch_finish_init(struct mxt_data *data)
{
	struct i2c_client *client = data->client;
	int ret;
	int irq_no = gpio_to_irq(data->pdata->gpio_int);
	ret = request_threaded_irq(irq_no, NULL, mxt_irq_thread,
		data->pdata->irqflags, client->dev.driver->name, data);

	if (ret) {
		dev_err(&client->dev, "%s : Failed to register interrupt, irq = %d\n",
			__func__, irq_no);
		goto err_req_irq;
	}

	ret = mxt_make_highchg(data);
	if (ret) {
		dev_err(&client->dev, "Failed to clear CHG pin\n");
		goto err_req_irq;
	}

#if TSP_BOOSTER
	ret = mxt_init_dvfs(data);
	if (ret < 0) {
		dev_err(&client->dev, "Fail get dvfs level for touch booster\n");
		goto err_req_irq;
	}
#endif

	dev_info(&client->dev, "Mxt touch controller initialized\n");
	return 0;

err_req_irq:
	return ret;
}

static int mxt_touch_rest_init(struct mxt_fw_info *fw_info)
{
	struct mxt_data *data = fw_info->data;
	int ret;

	ret = mxt_initialize(data);
	if (ret) {
		pr_err("MXT failed to initialize\n");
		goto err_free_mem;
	}

	ret = mxt_rest_initialize(fw_info);
	if (ret) {
		pr_err("MXT Failed to rest initialize\n");
		goto err_free_mem;
	}

	ret = mxt_touch_finish_init(data);
	if (ret)
		goto err_free_mem;

	return 0;

err_free_mem:
	kfree(data->objects);
	data->objects = NULL;
	kfree(data->reportids);
	data->reportids = NULL;
	return ret;
}
static int mxt_touch_init_firmware(const struct firmware *fw, void *context)
{
	struct mxt_data *data = context;
	struct mxt_fw_info fw_info;
	int ret;

	memset(&fw_info, 0, sizeof(struct mxt_fw_info));
	fw_info.data = data;
	ret = mxt_touch_rest_init(&fw_info);
	return ret;
}

static void mxt_request_firmware_work(const struct firmware *fw,
		void *context)
{
	struct mxt_data *data = context;
	mxt_touch_init_firmware(fw, data);
}

static int mxt_touch_init(struct mxt_data *data, bool nowait)
{
	struct i2c_client *client = data->client;
	const char *firmware_name =
		 data->pdata->firmware_name ?: MXT_DEFAULT_FIRMWARE_NAME;
	int ret = 0;

	dev_info(&client->dev, "%s : firmware_name:%s\n", __func__, firmware_name);

#if TSP_INFORM_CHARGER
	/* Register callbacks */
	/* To inform tsp , charger connection status*/
	data->callbacks.inform_charger = inform_charger;
	if (data->pdata->register_cb) {
		data->pdata->register_cb(&data->callbacks);
		inform_charger_init(data);
	}
#endif

	if (nowait) {
		const struct firmware *fw;
		char fw_path[MXT_MAX_FW_PATH];

		memset(&fw_path, 0, MXT_MAX_FW_PATH);

		snprintf(fw_path, MXT_MAX_FW_PATH, "%s/%s",
			MXT_FIRMWARE_INKERNEL_PATH, firmware_name);

		dev_err(&client->dev, "%s\n", fw_path);
		ret = mxt_touch_init_firmware(fw, data);
	} else {
		ret = request_firmware_nowait(THIS_MODULE, true, firmware_name,
				      &client->dev, GFP_KERNEL,
				      data, mxt_request_firmware_work);
		if (ret)
			dev_err(&client->dev, "cannot schedule firmware update (%d)\n", ret);
	}

	return ret;
}

static int mxt_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct mxt_data *data = i2c_get_clientdata(client);
	int ret;

	mutex_lock(&data->input_dev->mutex);
	ret = mxt_stop(data);
	mutex_unlock(&data->input_dev->mutex);

	if (ret)
		dev_err(&client->dev, "%s : failed to suspend mxt224s\n", __func__);

	return ret;
}

static int mxt_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct mxt_data *data = i2c_get_clientdata(client);
	int ret;

	mutex_lock(&data->input_dev->mutex);
	ret = mxt_start(data);
	mutex_unlock(&data->input_dev->mutex);

	if (ret)
		dev_err(&client->dev, "%s : failed to resume mxt224s\n", __func__);

	return ret;
}

/* Added for samsung dependent codes such as Factory test,
 * Touch booster, Related debug sysfs.
 */
#ifdef CONFIG_MACH_PXA_SAMSUNG
#include "mxts_sec.c"
#endif

#ifdef CONFIG_OF
static struct of_device_id mxt224s_dt_ids[] = {
	{ .compatible = "atmel,mxt224s", },
	{}
};
MODULE_DEVICE_TABLE(of, mxt224s_dt_ids);
#endif

static int mxt224s_probe_dt(struct device_node *np,
			struct device *dev,
			struct mxt_platform_data *pdata)
{
	struct i2c_client *client = to_i2c_client(dev);
	const struct of_device_id *match;

	if (!np)
		return -EINVAL;
	match = of_match_device(mxt224s_dt_ids, dev);
	if (!match)
		return -EINVAL;

	if (of_property_read_u32(np, "atmel,num_xnode", &pdata->num_xnode)) {
		dev_err(&client->dev,  "failed to get atmel,num_xnode property\n");
		return -EINVAL;
	}
	if (of_property_read_u32(np, "atmel,num_ynode", &pdata->num_ynode)) {
		dev_err(&client->dev,  "failed to get atmel,num_ynode property\n");
		return -EINVAL;
	}
	if (of_property_read_u32(np, "atmel,max_x", &pdata->max_x)) {
		dev_err(&client->dev,  "failed to get atmel,max_x property\n");
		return -EINVAL;
	}
	if (of_property_read_u32(np, "atmel,max_y", &pdata->max_y)) {
		dev_err(&client->dev,  "failed to get atmel,max_y property\n");
		return -EINVAL;
	}
	if (of_property_read_u32(np, "atmel,funcflags", &pdata->funcflags)) {
		dev_err(&client->dev,  "failed to get atmel,flags property\n");
		return -EINVAL;
	}
	if (of_property_read_u32(np, "atmel,irqflags", &pdata->irqflags)) {
		dev_err(&client->dev,  "failed to get atmel,flags property\n");
		return -EINVAL;
	}
	if (of_property_read_u32(np, "atmel,mxts_vdd_type", &pdata->mxts_vdd_type)) {
		dev_err(&client->dev, "failed to get atmel,mxts_vdd_type flags property\n");
		return -EINVAL;
	}
	if (of_property_read_u32(np, "atmel,mxts_1v8_type", &pdata->mxts_1v8_type)) {
		dev_err(&client->dev, "failed to get atmel,tsp_1v8_en_type flags property\n");
		return -EINVAL;
	}

	if (pdata->mxts_vdd_type == LDO_SUPPLY) {
		pdata->gpio_vdd_en = of_get_named_gpio(np, "atmel,gpio_vdd_en", 0);
		if (pdata->gpio_vdd_en < 0) {
			dev_err(&client->dev,  "of_get_named_gpio irq failed\n");
			return -EINVAL;
		}
	}

	if (pdata->mxts_1v8_type == LDO_SUPPLY) {
		pdata->gpio_1v8_en = of_get_named_gpio(np, "atmel,gpio_1v8_en", 0);
		if (pdata->gpio_1v8_en < 0) {
			dev_err(&client->dev,  "of_get_named_gpio irq failed\n");
			return -EINVAL;
		}
	}

	pdata->gpio_int = of_get_named_gpio(np, "atmel,gpio_int", 0);
	if (pdata->gpio_int < 0) {
		dev_err(&client->dev,  "of_get_named_gpio irq failed\n");
		return -EINVAL;
	}

	if (pdata->mxts_vdd_type == REGULATOR_SUPPLY) {
		if (of_property_read_u32(np, "atmel,mxts_vdd_regulator_volt",
				 &pdata->mxts_vdd_regulator_volt)) {
			dev_err(&client->dev,  "failed to get atmel,mxts_vdd_regulator_volt property\n");
			return -EINVAL;
		}
	}

	if (pdata->mxts_1v8_type == REGULATOR_SUPPLY) {
		if (of_property_read_u32(np, "atmel,mxts_1v8_regulator_volt",
				 &pdata->mxts_1v8_regulator_volt)) {
			dev_err(&client->dev,  "failed to get atmel,mxts_1v8_regulator_volt property\n");
			return -EINVAL;
		}
	}

	if (of_property_read_string(np, "atmel,project_name",
			 &pdata->project_name)) {
		dev_err(&client->dev,  "failed to get atmel,project_name property\n");
		return -EINVAL;
	}
	if (of_property_read_string(np, "atmel,config_ver",
			 &pdata->config_ver)) {
		dev_err(&client->dev,  "failed to get atmel,config_ver property\n");
		return -EINVAL;
	}

	dev_info(&client->dev,  "%s : num_xnode = %d, num_ynode = %d, funcflags = 0x%x, irqflags = 0x%x, \
				gpio_1v8_en = %d, volt = %d, gpio_int = %d\n", __func__,
		pdata->num_xnode, pdata->num_ynode, pdata->funcflags,
		pdata->irqflags, pdata->gpio_1v8_en, pdata->mxts_vdd_regulator_volt,
		pdata->gpio_int);

	pr_info("%s : project_name: %s, config_ver: %s\n", __func__,
		pdata->project_name, pdata->config_ver);

	return 0;
}

static int mxt_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct	mxt_platform_data *pdata = client->dev.platform_data;
	struct	i2c_adapter *adapter = to_i2c_adapter(client->dev.parent);
	struct  device_node *np = client->dev.of_node;
	struct	mxt_data *data;
	struct	input_dev *input_dev;
	u16	boot_address;
	int	ret = 0;

	if (IS_ENABLED(CONFIG_OF)) {
		if (!pdata) {
			pdata = devm_kzalloc(&client->dev, sizeof(*pdata), GFP_KERNEL);
			if (!pdata)
				return -ENOMEM;
		}
		ret = mxt224s_probe_dt(np, &client->dev, pdata);
		if (ret)
			return ret;
	} else if (!pdata) {
		dev_err(&client->dev, "%s :  no platform data defined\n", __func__);
		return -EINVAL;
	}

	if (!i2c_check_functionality(adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "%s : Not compatible i2c function\n", __func__);
		return -EIO;
	}

	data = kzalloc(sizeof(struct mxt_data), GFP_KERNEL);
	if (unlikely(!data)) {
		dev_err(&client->dev, "%s : Failed to allocate memory.\n", __func__);
		return -ENOMEM;
	}

	client->dev.platform_data = pdata;
	i2c_set_clientdata(client, data);
	data->client = client;
	data->pdata = pdata;

	if (gpio_request(pdata->gpio_int, "tsp-int")) {
		dev_err(&client->dev, "gpio %d request failed\n", pdata->gpio_int);
	}

	if (pdata->mxts_vdd_type == LDO_SUPPLY) {
		if (gpio_request(pdata->gpio_vdd_en, "tsp_vdd-en")) {
			dev_err(&client->dev, "%s : gpio %d request failed\n", __func__, pdata->gpio_vdd_en);
			goto err_gpio_vdd_request;
		}
	}

	if (pdata->mxts_1v8_type == LDO_SUPPLY) {
		if (gpio_request(pdata->gpio_1v8_en, "tsp_1v8-en")) {
			dev_err(&client->dev, "%s : gpio %d request failed\n", __func__, pdata->gpio_1v8_en);
			goto err_gpio_1v8_request;
		}
	}

	input_dev = input_allocate_device();
	if (unlikely(!input_dev)) {
		ret = -ENOMEM;
		dev_err(&client->dev, "Failed to allocate input device.\n");
		goto err_allocate_input_device;
	}

	input_dev->name = "sec_touchscreen";
	input_dev->id.bustype = BUS_I2C;
	input_dev->dev.parent = &client->dev;

	data->input_dev = input_dev;

	set_bit(EV_ABS, input_dev->evbit);
	set_bit(EV_KEY, input_dev->evbit);
	set_bit(INPUT_PROP_DIRECT, input_dev->propbit);
	set_bit(BTN_TOUCH, input_dev->keybit);
	set_bit(BTN_TOOL_FINGER, input_dev->keybit);

#if ENABLE_TOUCH_KEY
	set_bit(KEY_MENU, input_dev->keybit);
	set_bit(KEY_BACK, input_dev->keybit);
#endif

	input_mt_init_slots(input_dev, MXT_MAX_FINGER, INPUT_MT_DIRECT);

	input_set_abs_params(input_dev, ABS_MT_POSITION_X,
				0, pdata->max_x, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_Y,
				0, pdata->max_y, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_TOUCH_MAJOR,
				0, MXT_AREA_MAX, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_PRESSURE,
				0, MXT_AMPLITUDE_MAX, 0, 0);
#if TSP_USE_SHAPETOUCH
	input_set_abs_params(input_dev, ABS_MT_COMPONENT,
				0, MXT_COMPONENT_MAX, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_SUMSIZE,
				0, MXT_SUMSIZE_MAX, 0, 0);
#endif

	input_set_drvdata(input_dev, data);
	i2c_set_clientdata(client, data);

	boot_address = (data->pdata->boot_address) ?
		data->pdata->boot_address : ((client->addr == MXT_APP_LOW) ? MXT_BOOT_LOW : MXT_BOOT_HIGH);

	data->client_boot = i2c_new_dummy(client->adapter, boot_address);
	if (!data->client_boot) {
		dev_err(&client->dev, "Failed to register sub client[0x%x]\n",
			 boot_address);
		ret = -ENODEV;
		goto err_create_sub_client;
	}

	/* register input device */
	ret = input_register_device(input_dev);
	if (ret)
		goto err_register_input_device;

#ifdef CONFIG_MACH_PXA_SAMSUNG
	ret = mxt_sysfs_init(client);
	if (ret < 0) {
		dev_err(&client->dev, "Failed to create sysfs.\n");
		goto err_sysfs_init;
	}
#endif
	if (data->pdata->funcflags & MXT_DT_FLAG_GPIO_INT) {
		dev_err(&client->dev, "%s :  isr handler will read status from gpio_int\n", __func__);
		data->pdata->read_chg = mxts_read_chg;
	}
	else {
		dev_warn(&client->dev, "%s :  MXT_DT_FLAG_GPIO_INT flag should be set\n", __func__);
		data->pdata->read_chg = NULL;
	}

	if (data->pdata->funcflags & MXT_DT_FLAG_REGISTER_CB) {
		data->pdata->register_cb = mxts_register_callback;
	}
	else {
		dev_warn(&client->dev, "%s :  MXT_DT_FLAG_REGISTER_CB flag should be set\n", __func__);
		data->pdata->register_cb = NULL;
	}

	if (data->pdata->funcflags & MXT_DT_FLAG_COMMON_REGULATOR && \
		data->pdata->funcflags & MXT_DT_FLAG_GPIO_REGULATOR) {
		data->pdata->power_setup = mxts_power_setup;
		data->pdata->power_onoff = mxts_power_onoff;
	}
	else {
		dev_warn(&client->dev, "%s :  MXT_DT_FLAG_COMMON_REGULATOR flag should be set\n", __func__);
		dev_warn(&client->dev, "%s :  MXT_DT_FLAG_GPIO_REGULATOR flag should be set\n", __func__);
		data->pdata->power_setup = NULL;
		data->pdata->power_onoff = NULL;
	}

	if (data->pdata->power_setup != NULL) {
		ret = data->pdata->power_setup(client, true);
		if (ret) {
			dev_err(&client->dev, "Failed to power setup.\n");
			goto err_power_setup;
		}
	}
	else {
		dev_err(&client->dev, "%s : power_setup() should be defined\n", __func__);
		goto err_power_setup;
	}

	ret = mxt_power_on(data);
	if (ret) {
		dev_err(&client->dev, "Failed to power on.\n");
		goto err_power_on;
	}

	ret = mxt_touch_init(data, MXT_FIRMWARE_UPDATE_TYPE);
	if (ret) {
		dev_err(&client->dev, "Failed to init driver\n");
		goto err_touch_init;
	}

	mutex_init(&data->input_dev->mutex);

#if defined(CONFIG_PM_RUNTIME)
	pm_runtime_enable(&client->dev);
#endif
	return 0;

err_touch_init:
	mxt_power_off(data);
err_power_on:
	data->pdata->power_setup(client, false);
err_power_setup:
#ifdef CONFIG_MACH_PXA_SAMSUNG
	mxt_sysfs_remove(data);
#endif
err_sysfs_init:
	input_unregister_device(input_dev);
	input_dev = NULL;
err_register_input_device:
	i2c_unregister_device(data->client_boot);
err_create_sub_client:
	input_free_device(input_dev);
err_allocate_input_device:
	gpio_free(pdata->gpio_1v8_en);
err_gpio_1v8_request:
	gpio_free(pdata->gpio_vdd_en);
err_gpio_vdd_request:
	if (pdata->gpio_int)
		gpio_free(pdata->gpio_int);
	kfree(data);

	return ret;
}

static int mxt_remove(struct i2c_client *client)
{
	struct mxt_data *data = i2c_get_clientdata(client);

	free_irq(client->irq, data);
	if (data->pdata->gpio_vdd_en > 0)
		gpio_free(data->pdata->gpio_vdd_en);
	if (data->pdata->gpio_1v8_en > 0)
		gpio_free(data->pdata->gpio_1v8_en);
	kfree(data->objects);
	kfree(data->reportids);
	input_unregister_device(data->input_dev);
	i2c_unregister_device(data->client_boot);
#ifdef CONFIG_MACH_PXA_SAMSUNG
	mxt_sysfs_remove(data);
#endif
	mxt_power_off(data);
	kfree(data);

	return 0;
}

static struct i2c_device_id mxt_idtable[] = {
	{MXT_DEV_NAME, 0},
};
MODULE_DEVICE_TABLE(i2c, mxt_idtable);

static const struct dev_pm_ops mxt_pm_ops = {
#if defined(CONFIG_PM_RUNTIME)
	SET_RUNTIME_PM_OPS(mxt_suspend, mxt_resume, NULL)
#else
	.suspend = mxt_suspend,
	.resume = mxt_resume,
#endif
};

static struct i2c_driver mxt_i2c_driver = {
	.id_table = mxt_idtable,
	.probe = mxt_probe,
	.remove = mxt_remove,
	.driver = {
		.owner	= THIS_MODULE,
		.name	= MXT_DEV_NAME,
#ifdef CONFIG_PM
		.pm	= &mxt_pm_ops,
#endif
#ifdef CONFIG_OF
		.of_match_table = of_match_ptr(mxt224s_dt_ids),
#endif
	},
};

module_i2c_driver(mxt_i2c_driver);

MODULE_DESCRIPTION("Atmel MaXTouch driver");
MODULE_AUTHOR("bumwoo.lee<bw365.lee@samsung.com>");
MODULE_LICENSE("GPL");
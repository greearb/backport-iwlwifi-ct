/******************************************************************************
 *
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * GPL LICENSE SUMMARY
 *
 * Copyright(c) 2007 - 2014 Intel Corporation. All rights reserved.
 * Copyright(c) 2013 - 2015 Intel Mobile Communications GmbH
 * Copyright(c) 2016        Intel Deutschland GmbH
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110,
 * USA
 *
 * The full GNU General Public License is included in this distribution
 * in the file called COPYING.
 *
 * Contact Information:
 *  Intel Linux Wireless <linuxwifi@intel.com>
 * Intel Corporation, 5200 N.E. Elam Young Parkway, Hillsboro, OR 97124-6497
 *
 * BSD LICENSE
 *
 * Copyright(c) 2005 - 2014 Intel Corporation. All rights reserved.
 * Copyright(c) 2013 - 2015 Intel Mobile Communications GmbH
 * Copyright(c) 2016        Intel Deutschland GmbH
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *  * Neither the name Intel Corporation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *****************************************************************************/
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/vmalloc.h>

#include "iwl-drv.h"
#include "iwl-csr.h"
#include "iwl-debug.h"
#include "iwl-trans.h"
#include "iwl-op-mode.h"
#include "iwl-agn-hw.h"
#include "iwl-fw.h"
#include "iwl-tm-gnl.h"
#include "iwl-config.h"
#include "iwl-modparams.h"
#ifdef CPTCFG_IWLWIFI_SUPPORT_DEBUG_OVERRIDES
#include "iwl-dbg-cfg.h"
#endif

/******************************************************************************
 *
 * module boiler plate
 *
 ******************************************************************************/

#define DRV_DESCRIPTION	"Intel(R) Wireless WiFi driver for Linux"
MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_AUTHOR(DRV_COPYRIGHT " " DRV_AUTHOR);
MODULE_LICENSE("GPL");

#ifdef CPTCFG_IWLWIFI_DEBUGFS
static struct dentry *iwl_dbgfs_root;
#endif

/**
 * struct iwl_drv - drv common data
 * @list: list of drv structures using this opmode
 * @fw: the iwl_fw structure
 * @op_mode: the running op_mode
 * @trans: transport layer
 * @dev: for debug prints only
 * @cfg: configuration struct
 * @fw_index: firmware revision to try loading
 * @firmware_name: composite filename of ucode file to load
 * @request_firmware_complete: the firmware has been obtained from user space
 */
struct iwl_drv {
	struct list_head list;
	struct iwl_fw fw;

	struct iwl_op_mode *op_mode;
	struct iwl_trans *trans;
	struct device *dev;
	const struct iwl_cfg *cfg;
#if IS_ENABLED(CPTCFG_IWLXVT)
	bool xvt_mode_on;
#endif

	int fw_index;                   /* firmware we're trying to load */
	char firmware_name[64];         /* name of firmware file to load */

	struct completion request_firmware_complete;

#ifdef CPTCFG_IWLWIFI_DEBUGFS
	struct dentry *dbgfs_drv;
	struct dentry *dbgfs_trans;
	struct dentry *dbgfs_op_mode;
#endif
};

enum {
	DVM_OP_MODE,
	MVM_OP_MODE,
#if IS_ENABLED(CPTCFG_IWLXVT)
	XVT_OP_MODE,
#endif
};

/* Protects the table contents, i.e. the ops pointer & drv list */
static struct mutex iwlwifi_opmode_table_mtx;
static struct iwlwifi_opmode_table {
	const char *name;			/* name: iwldvm, iwlmvm, etc */
	const struct iwl_op_mode_ops *ops;	/* pointer to op_mode ops */
	struct list_head drv;		/* list of devices using this op_mode */
} iwlwifi_opmode_table[] = {		/* ops set when driver is initialized */
	[DVM_OP_MODE] = { .name = "iwldvm", .ops = NULL },
	[MVM_OP_MODE] = { .name = "iwlmvm", .ops = NULL },
#if IS_ENABLED(CPTCFG_IWLXVT)
	[XVT_OP_MODE] = { .name = "iwlxvt", .ops = NULL },
#endif
};

#if IS_ENABLED(CPTCFG_IWLXVT)
/* kernel object for a device dedicated
 * folder in the sysfs */
static struct kobject *iwl_kobj;

static struct iwl_op_mode *
_iwl_op_mode_start(struct iwl_drv *drv, struct iwlwifi_opmode_table *op);
static void _iwl_op_mode_stop(struct iwl_drv *drv);

/*
 * iwl_drv_get_dev_container - Given a device, returns the pointer
 * to it's corresponding driver's struct
 */
struct iwl_drv *iwl_drv_get_dev_container(struct device *dev)
{
	struct iwl_drv *drv_itr;
	int i;

	/* Going over all drivers, looking for the one that holds dev */
	for (i = 0; (i < ARRAY_SIZE(iwlwifi_opmode_table)); i++) {
		list_for_each_entry(drv_itr, &iwlwifi_opmode_table[i].drv, list)
				if (drv_itr->dev == dev)
					return drv_itr;
	}

	return NULL;
}
IWL_EXPORT_SYMBOL(iwl_drv_get_dev_container);

/*
 * iwl_drv_get_op_mode - Returns the index of the device's
 * active operation mode
 */
static int iwl_drv_get_op_mode_idx(struct iwl_drv *drv)
{
	struct iwl_drv *drv_itr;
	int i;

	if (!drv || !drv->dev)
		return -ENODEV;

	/* Going over all drivers, looking for the list that holds it */
	for (i = 0; (i < ARRAY_SIZE(iwlwifi_opmode_table)); i++) {
		list_for_each_entry(drv_itr, &iwlwifi_opmode_table[i].drv, list)
			if (drv_itr->dev == drv->dev)
				return i;
	}

	return -EINVAL;
}

/*
 * iwl_drv_switch_op_mode - Switch between operation modes
 * Checks if the desired operation mode is valid, if it
 * is supported by the device. Stops the current op mode
 * and starts the desired mode.
 */
int iwl_drv_switch_op_mode(struct iwl_drv *drv, const char *new_op_name)
{
	struct iwlwifi_opmode_table *new_op = NULL;
	int idx;

	/* Searching for wanted op_mode*/
	for (idx = 0; idx < ARRAY_SIZE(iwlwifi_opmode_table); idx++) {
		if (!strcmp(iwlwifi_opmode_table[idx].name, new_op_name)) {
			new_op = &iwlwifi_opmode_table[idx];
			break;
		}
	}

	/* Checking if the desired op mode is valid */
	if (!new_op) {
		IWL_ERR(drv, "No such op mode \"%s\"\n", new_op_name);
		return -EINVAL;
	}

	/*
	 * If the desired op mode is already the
	 * device's current op mode, do nothing
	 */
	if (idx == iwl_drv_get_op_mode_idx(drv))
		return 0;

	/* Checking if the device supports the desired operation mode */

	/* xVT mode is available only with 16 FW */
	if (drv->fw.type == IWL_FW_MVM) {
		if ((idx != XVT_OP_MODE) && (idx != MVM_OP_MODE)) {
			IWL_ERR(drv, "Op mode %s not supported by device\n",
				new_op_name);
			return -ENOTSUPP;
		}
	} else {
		IWL_ERR(drv, "Switching op modes is not supported by device\n");
		return -ENOTSUPP;
	}

	/* Recording new op mode state */
	drv->xvt_mode_on = (idx == XVT_OP_MODE);

	/* Stopping the current op mode */
	_iwl_op_mode_stop(drv);

	/* Changing operation mode */
	mutex_lock(&iwlwifi_opmode_table_mtx);
	list_move_tail(&drv->list, &new_op->drv);
	mutex_unlock(&iwlwifi_opmode_table_mtx);

	/* Starting the new op mode */
	if (new_op->ops) {
		drv->op_mode = _iwl_op_mode_start(drv, new_op);
		if (!drv->op_mode) {
			IWL_ERR(drv, "Error switching op modes\n");
			return -EINVAL;
		}
	} else {
		return request_module("%s", new_op->name);
	}

	return 0;
}
IWL_EXPORT_SYMBOL(iwl_drv_switch_op_mode);

/*
 * iwl_drv_sysfs_show - Returns device information to user
 */
static ssize_t iwl_drv_sysfs_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct iwl_drv  *drv;
	int op_mode_idx = 0, itr;
	int ret = 0;

	/* Retrieving containing driver */
	drv = iwl_drv_get_dev_container(dev);
	op_mode_idx = iwl_drv_get_op_mode_idx(drv);

	/* Checking if driver and driver information are valid */
	if (op_mode_idx < 0)
		return op_mode_idx;

	/* Constructing output */
	for (itr = 0; itr < ARRAY_SIZE(iwlwifi_opmode_table); itr++) {
		ret += scnprintf(buf + ret, PAGE_SIZE - ret, "%s%-s\n",
				 (itr == op_mode_idx) ? "* " : "  ",
				 iwlwifi_opmode_table[itr].name);
	}

	return ret;
}

/* Attribute for device */
static const DEVICE_ATTR(op_mode, S_IRUGO,
			 iwl_drv_sysfs_show, NULL);

/*
 * iwl_create_sysfs_file - Creates a sysfs entry (under PCI devices),
 * and a symlink under modules/iwlwifi
 */
static int iwl_create_sysfs_file(struct iwl_drv *drv)
{
	int ret;

	ret = device_create_file(drv->dev, &dev_attr_op_mode);
	if (!ret) {
		ret = sysfs_create_link(iwl_kobj,
					&drv->dev->kobj, dev_name(drv->dev));
	}

	return ret;
}

/*
 * iwl_remove_sysfs_file - Removes sysfs entries
 */
static void iwl_remove_sysfs_file(struct iwl_drv *drv)
{
	sysfs_remove_link(iwl_kobj, dev_name(drv->dev));
	device_remove_file(drv->dev, &dev_attr_op_mode);
}
#endif /* CPTCFG_IWLXVT */

#define IWL_DEFAULT_SCAN_CHANNELS 40

/*
 * struct fw_sec: Just for the image parsing process.
 * For the fw storage we are using struct fw_desc.
 */
struct fw_sec {
	const void *data;		/* the sec data */
	size_t size;			/* section size */
	u32 offset;			/* offset of writing in the device */
};

static void iwl_free_fw_desc(struct iwl_drv *drv, struct fw_desc *desc)
{
	vfree(desc->data);
	desc->data = NULL;
	desc->len = 0;
}

static void iwl_free_fw_img(struct iwl_drv *drv, struct fw_img *img)
{
	int i;
	for (i = 0; i < IWL_UCODE_SECTION_MAX; i++)
		iwl_free_fw_desc(drv, &img->sec[i]);
}

static void iwl_dealloc_ucode(struct iwl_drv *drv)
{
	int i;

	kfree(drv->fw.dbg_dest_tlv);
	for (i = 0; i < ARRAY_SIZE(drv->fw.dbg_conf_tlv); i++)
		kfree(drv->fw.dbg_conf_tlv[i]);
	for (i = 0; i < ARRAY_SIZE(drv->fw.dbg_trigger_tlv); i++)
		kfree(drv->fw.dbg_trigger_tlv[i]);
	for (i = 0; i < ARRAY_SIZE(drv->fw.dbg_mem_tlv); i++)
		kfree(drv->fw.dbg_mem_tlv[i]);

	for (i = 0; i < IWL_UCODE_TYPE_MAX; i++)
		iwl_free_fw_img(drv, drv->fw.img + i);
}

static int iwl_alloc_fw_desc(struct iwl_drv *drv, struct fw_desc *desc,
			     struct fw_sec *sec)
{
	void *data;

	desc->data = NULL;

	if (!sec || !sec->size)
		return -EINVAL;

	data = vmalloc(sec->size);
	if (!data)
		return -ENOMEM;

	desc->len = sec->size;
	desc->offset = sec->offset;
	memcpy(data, sec->data, desc->len);
	desc->data = data;

	return 0;
}

static void iwl_req_fw_callback(const struct firmware *ucode_raw,
				void *context);

static int iwl_request_firmware(struct iwl_drv *drv, bool first)
{
	const char *name_pre = drv->cfg->fw_name_pre;
	char tag[8];
#if defined(CPTCFG_IWLWIFI_SUPPORT_DEBUG_OVERRIDES) && \
	defined(CPTCFG_IWLWIFI_DEVICE_TESTMODE)
	char fw_name_temp[32];
#endif

	if (first) {
		drv->fw_index = drv->cfg->ucode_api_max;
		sprintf(tag, "%d", drv->fw_index);
	} else {
		drv->fw_index--;
		sprintf(tag, "%d", drv->fw_index);
	}

#ifdef CPTCFG_IWLWIFI_DISALLOW_OLDER_FW
	/* The dbg-cfg check here works because the first time we get
	 * here we always load the 'api_max' version, and once that
	 * has returned we load the dbg-cfg file.
	 */
	if ((drv->fw_index != drv->cfg->ucode_api_max
#ifdef CPTCFG_IWLWIFI_SUPPORT_DEBUG_OVERRIDES
	     && !drv->trans->dbg_cfg.load_old_fw
#endif
	    ) ||
	    drv->fw_index < drv->cfg->ucode_api_min) {
#else
	if (drv->fw_index < drv->cfg->ucode_api_min) {
#endif
		IWL_ERR(drv, "no suitable firmware found!\n");
		return -ENOENT;
	}

	snprintf(drv->firmware_name, sizeof(drv->firmware_name), "%s%s.ucode",
		 name_pre, tag);

#ifdef CPTCFG_IWLWIFI_SUPPORT_DEBUG_OVERRIDES
#ifdef CPTCFG_IWLWIFI_DEVICE_TESTMODE
	/* check if upload firmware code is required */
	if (drv->trans->dbg_cfg.use_upload_ucode) {
		snprintf(fw_name_temp, sizeof(fw_name_temp), "upload-%s",
			 drv->firmware_name);
		strncpy(drv->firmware_name, fw_name_temp,
			sizeof(drv->firmware_name));
	}
#endif
#endif

	IWL_DEBUG_INFO(drv, "attempting to load firmware '%s'\n",
		       drv->firmware_name);

	return request_firmware_nowait(THIS_MODULE, 1, drv->firmware_name,
				       drv->trans->dev,
				       GFP_KERNEL, drv, iwl_req_fw_callback);
}

struct fw_img_parsing {
	struct fw_sec sec[IWL_UCODE_SECTION_MAX];
	int sec_counter;
};

/*
 * struct fw_sec_parsing: to extract fw section and it's offset from tlv
 */
struct fw_sec_parsing {
	__le32 offset;
	const u8 data[];
} __packed;

/**
 * struct iwl_tlv_calib_data - parse the default calib data from TLV
 *
 * @ucode_type: the uCode to which the following default calib relates.
 * @calib: default calibrations.
 */
struct iwl_tlv_calib_data {
	__le32 ucode_type;
	struct iwl_tlv_calib_ctrl calib;
} __packed;

struct iwl_firmware_pieces {
	struct fw_img_parsing img[IWL_UCODE_TYPE_MAX];

	u32 init_evtlog_ptr, init_evtlog_size, init_errlog_ptr;
	u32 inst_evtlog_ptr, inst_evtlog_size, inst_errlog_ptr;

	/* FW debug data parsed for driver usage */
	struct iwl_fw_dbg_dest_tlv *dbg_dest_tlv;
	struct iwl_fw_dbg_conf_tlv *dbg_conf_tlv[FW_DBG_CONF_MAX];
	size_t dbg_conf_tlv_len[FW_DBG_CONF_MAX];
	struct iwl_fw_dbg_trigger_tlv *dbg_trigger_tlv[FW_DBG_TRIGGER_MAX];
	size_t dbg_trigger_tlv_len[FW_DBG_TRIGGER_MAX];
	struct iwl_fw_dbg_mem_seg_tlv *dbg_mem_tlv[FW_DBG_MEM_MAX];
};

/*
 * These functions are just to extract uCode section data from the pieces
 * structure.
 */
static struct fw_sec *get_sec(struct iwl_firmware_pieces *pieces,
			      enum iwl_ucode_type type,
			      int  sec)
{
	return &pieces->img[type].sec[sec];
}

static void set_sec_data(struct iwl_firmware_pieces *pieces,
			 enum iwl_ucode_type type,
			 int sec,
			 const void *data)
{
	pieces->img[type].sec[sec].data = data;
}

static void set_sec_size(struct iwl_firmware_pieces *pieces,
			 enum iwl_ucode_type type,
			 int sec,
			 size_t size)
{
	pieces->img[type].sec[sec].size = size;
}

static size_t get_sec_size(struct iwl_firmware_pieces *pieces,
			   enum iwl_ucode_type type,
			   int sec)
{
	return pieces->img[type].sec[sec].size;
}

static void set_sec_offset(struct iwl_firmware_pieces *pieces,
			   enum iwl_ucode_type type,
			   int sec,
			   u32 offset)
{
	pieces->img[type].sec[sec].offset = offset;
}

static int iwl_store_cscheme(struct iwl_fw *fw, const u8 *data, const u32 len)
{
	int i, j;
	struct iwl_fw_cscheme_list *l = (struct iwl_fw_cscheme_list *)data;
	struct iwl_fw_cipher_scheme *fwcs;
	struct ieee80211_cipher_scheme *cs;
	u32 cipher;

	if (len < sizeof(*l) ||
	    len < sizeof(l->size) + l->size * sizeof(l->cs[0]))
		return -EINVAL;

	for (i = 0, j = 0; i < IWL_UCODE_MAX_CS && i < l->size; i++) {
		fwcs = &l->cs[j];
		cipher = le32_to_cpu(fwcs->cipher);

		/* we skip schemes with zero cipher suite selector */
		if (!cipher)
			continue;

		cs = &fw->cs[j++];
		cs->cipher = cipher;
		cs->iftype = BIT(NL80211_IFTYPE_STATION);
		cs->hdr_len = fwcs->hdr_len;
		cs->pn_len = fwcs->pn_len;
		cs->pn_off = fwcs->pn_off;
		cs->key_idx_off = fwcs->key_idx_off;
		cs->key_idx_mask = fwcs->key_idx_mask;
		cs->key_idx_shift = fwcs->key_idx_shift;
		cs->mic_len = fwcs->mic_len;
	}

	return 0;
}

static void iwl_store_gscan_capa(struct iwl_fw *fw, const u8 *data,
				 const u32 len)
{
	struct iwl_fw_gscan_capabilities *fw_capa = (void *)data;
	struct iwl_gscan_capabilities *capa = &fw->gscan_capa;

	capa->max_scan_cache_size = le32_to_cpu(fw_capa->max_scan_cache_size);
	capa->max_scan_buckets = le32_to_cpu(fw_capa->max_scan_buckets);
	capa->max_ap_cache_per_scan =
		le32_to_cpu(fw_capa->max_ap_cache_per_scan);
	capa->max_rssi_sample_size = le32_to_cpu(fw_capa->max_rssi_sample_size);
	capa->max_scan_reporting_threshold =
		le32_to_cpu(fw_capa->max_scan_reporting_threshold);
	capa->max_hotlist_aps = le32_to_cpu(fw_capa->max_hotlist_aps);
	capa->max_significant_change_aps =
		le32_to_cpu(fw_capa->max_significant_change_aps);
	capa->max_bssid_history_entries =
		le32_to_cpu(fw_capa->max_bssid_history_entries);
	capa->max_hotlist_ssids = le32_to_cpu(fw_capa->max_hotlist_ssids);
	capa->max_number_epno_networks =
		le32_to_cpu(fw_capa->max_number_epno_networks);
	capa->max_number_epno_networks_by_ssid =
		le32_to_cpu(fw_capa->max_number_epno_networks_by_ssid);
	capa->max_number_of_white_listed_ssid =
		le32_to_cpu(fw_capa->max_number_of_white_listed_ssid);
	capa->max_number_of_black_listed_ssid =
		le32_to_cpu(fw_capa->max_number_of_black_listed_ssid);
}

/*
 * Gets uCode section from tlv.
 */
static int iwl_store_ucode_sec(struct iwl_firmware_pieces *pieces,
			       const void *data, enum iwl_ucode_type type,
			       int size)
{
	struct fw_img_parsing *img;
	struct fw_sec *sec;
	struct fw_sec_parsing *sec_parse;

	if (WARN_ON(!pieces || !data || type >= IWL_UCODE_TYPE_MAX))
		return -1;

	sec_parse = (struct fw_sec_parsing *)data;

	img = &pieces->img[type];
	sec = &img->sec[img->sec_counter];

	sec->offset = le32_to_cpu(sec_parse->offset);
	sec->data = sec_parse->data;
	sec->size = size - sizeof(sec_parse->offset);

	++img->sec_counter;

	return 0;
}

static int iwl_set_default_calib(struct iwl_drv *drv, const u8 *data)
{
	struct iwl_tlv_calib_data *def_calib =
					(struct iwl_tlv_calib_data *)data;
	u32 ucode_type = le32_to_cpu(def_calib->ucode_type);
	if (ucode_type >= IWL_UCODE_TYPE_MAX) {
		IWL_ERR(drv, "Wrong ucode_type %u for default calibration.\n",
			ucode_type);
		return -EINVAL;
	}
	drv->fw.default_calib[ucode_type].flow_trigger =
		def_calib->calib.flow_trigger;
	drv->fw.default_calib[ucode_type].event_trigger =
		def_calib->calib.event_trigger;

	return 0;
}

static int iwl_set_ucode_api_flags(struct iwl_drv *drv, const u8 *data,
				   struct iwl_ucode_capabilities *capa)
{
	const struct iwl_ucode_api *ucode_api = (void *)data;
	u32 api_index = le32_to_cpu(ucode_api->api_index);
	u32 api_flags = le32_to_cpu(ucode_api->api_flags);
	int i;

	if (api_index >= DIV_ROUND_UP(NUM_IWL_UCODE_TLV_API, 32)) {
		IWL_ERR(drv,
			"api flags index %d larger than supported by driver\n",
			api_index);
		/* don't return an error so we can load FW that has more bits */
		return 0;
	}

	for (i = 0; i < 32; i++) {
		if (api_flags & BIT(i))
			__set_bit(i + 32 * api_index, capa->_api);
	}

	return 0;
}

static int iwl_set_ucode_capabilities(struct iwl_drv *drv, const u8 *data,
				      struct iwl_ucode_capabilities *capa)
{
	const struct iwl_ucode_capa *ucode_capa = (void *)data;
	u32 api_index = le32_to_cpu(ucode_capa->api_index);
	u32 api_flags = le32_to_cpu(ucode_capa->api_capa);
	int i;

	if (api_index >= DIV_ROUND_UP(NUM_IWL_UCODE_TLV_CAPA, 32)) {
		IWL_ERR(drv,
			"capa flags index %d larger than supported by driver\n",
			api_index);
		/* don't return an error so we can load FW that has more bits */
		return 0;
	}

	for (i = 0; i < 32; i++) {
		if (api_flags & BIT(i))
			__set_bit(i + 32 * api_index, capa->_capa);
	}

	return 0;
}

static int iwl_parse_v1_v2_firmware(struct iwl_drv *drv,
				    const struct firmware *ucode_raw,
				    struct iwl_firmware_pieces *pieces)
{
	struct iwl_ucode_header *ucode = (void *)ucode_raw->data;
	u32 api_ver, hdr_size, build;
	char buildstr[25];
	const u8 *src;

	drv->fw.ucode_ver = le32_to_cpu(ucode->ver);
	api_ver = IWL_UCODE_API(drv->fw.ucode_ver);

	switch (api_ver) {
	default:
		hdr_size = 28;
		if (ucode_raw->size < hdr_size) {
			IWL_ERR(drv, "File size too small!\n");
			return -EINVAL;
		}
		build = le32_to_cpu(ucode->u.v2.build);
		set_sec_size(pieces, IWL_UCODE_REGULAR, IWL_UCODE_SECTION_INST,
			     le32_to_cpu(ucode->u.v2.inst_size));
		set_sec_size(pieces, IWL_UCODE_REGULAR, IWL_UCODE_SECTION_DATA,
			     le32_to_cpu(ucode->u.v2.data_size));
		set_sec_size(pieces, IWL_UCODE_INIT, IWL_UCODE_SECTION_INST,
			     le32_to_cpu(ucode->u.v2.init_size));
		set_sec_size(pieces, IWL_UCODE_INIT, IWL_UCODE_SECTION_DATA,
			     le32_to_cpu(ucode->u.v2.init_data_size));
		src = ucode->u.v2.data;
		break;
	case 0:
	case 1:
	case 2:
		hdr_size = 24;
		if (ucode_raw->size < hdr_size) {
			IWL_ERR(drv, "File size too small!\n");
			return -EINVAL;
		}
		build = 0;
		set_sec_size(pieces, IWL_UCODE_REGULAR, IWL_UCODE_SECTION_INST,
			     le32_to_cpu(ucode->u.v1.inst_size));
		set_sec_size(pieces, IWL_UCODE_REGULAR, IWL_UCODE_SECTION_DATA,
			     le32_to_cpu(ucode->u.v1.data_size));
		set_sec_size(pieces, IWL_UCODE_INIT, IWL_UCODE_SECTION_INST,
			     le32_to_cpu(ucode->u.v1.init_size));
		set_sec_size(pieces, IWL_UCODE_INIT, IWL_UCODE_SECTION_DATA,
			     le32_to_cpu(ucode->u.v1.init_data_size));
		src = ucode->u.v1.data;
		break;
	}

	if (build)
		sprintf(buildstr, " build %u", build);
	else
		buildstr[0] = '\0';

	snprintf(drv->fw.fw_version,
		 sizeof(drv->fw.fw_version),
		 "%u.%u.%u.%u%s",
		 IWL_UCODE_MAJOR(drv->fw.ucode_ver),
		 IWL_UCODE_MINOR(drv->fw.ucode_ver),
		 IWL_UCODE_API(drv->fw.ucode_ver),
		 IWL_UCODE_SERIAL(drv->fw.ucode_ver),
		 buildstr);

	/* Verify size of file vs. image size info in file's header */

	if (ucode_raw->size != hdr_size +
	    get_sec_size(pieces, IWL_UCODE_REGULAR, IWL_UCODE_SECTION_INST) +
	    get_sec_size(pieces, IWL_UCODE_REGULAR, IWL_UCODE_SECTION_DATA) +
	    get_sec_size(pieces, IWL_UCODE_INIT, IWL_UCODE_SECTION_INST) +
	    get_sec_size(pieces, IWL_UCODE_INIT, IWL_UCODE_SECTION_DATA)) {

		IWL_ERR(drv,
			"uCode file size %d does not match expected size\n",
			(int)ucode_raw->size);
		return -EINVAL;
	}


	set_sec_data(pieces, IWL_UCODE_REGULAR, IWL_UCODE_SECTION_INST, src);
	src += get_sec_size(pieces, IWL_UCODE_REGULAR, IWL_UCODE_SECTION_INST);
	set_sec_offset(pieces, IWL_UCODE_REGULAR, IWL_UCODE_SECTION_INST,
		       IWLAGN_RTC_INST_LOWER_BOUND);
	set_sec_data(pieces, IWL_UCODE_REGULAR, IWL_UCODE_SECTION_DATA, src);
	src += get_sec_size(pieces, IWL_UCODE_REGULAR, IWL_UCODE_SECTION_DATA);
	set_sec_offset(pieces, IWL_UCODE_REGULAR, IWL_UCODE_SECTION_DATA,
		       IWLAGN_RTC_DATA_LOWER_BOUND);
	set_sec_data(pieces, IWL_UCODE_INIT, IWL_UCODE_SECTION_INST, src);
	src += get_sec_size(pieces, IWL_UCODE_INIT, IWL_UCODE_SECTION_INST);
	set_sec_offset(pieces, IWL_UCODE_INIT, IWL_UCODE_SECTION_INST,
		       IWLAGN_RTC_INST_LOWER_BOUND);
	set_sec_data(pieces, IWL_UCODE_INIT, IWL_UCODE_SECTION_DATA, src);
	src += get_sec_size(pieces, IWL_UCODE_INIT, IWL_UCODE_SECTION_DATA);
	set_sec_offset(pieces, IWL_UCODE_INIT, IWL_UCODE_SECTION_DATA,
		       IWLAGN_RTC_DATA_LOWER_BOUND);
	return 0;
}

static int iwl_parse_tlv_firmware(struct iwl_drv *drv,
				const struct firmware *ucode_raw,
				struct iwl_firmware_pieces *pieces,
				struct iwl_ucode_capabilities *capa,
				bool *usniffer_images)
{
	struct iwl_tlv_ucode_header *ucode = (void *)ucode_raw->data;
	struct iwl_ucode_tlv *tlv;
	size_t len = ucode_raw->size;
	const u8 *data;
	u32 tlv_len;
	u32 usniffer_img;
	enum iwl_ucode_tlv_type tlv_type;
	const u8 *tlv_data;
	char buildstr[25];
	u32 build, paging_mem_size;
	int num_of_cpus;
	bool usniffer_req = false;
	bool gscan_capa = false;

#ifdef CPTCFG_IWLWIFI_SUPPORT_DEBUG_OVERRIDES
	if (ucode->magic == cpu_to_le32(IWL_TLV_FW_DBG_MAGIC)) {
		size_t dbg_data_ofs = offsetof(struct iwl_tlv_ucode_header,
					       human_readable);
		data = (void *)ucode_raw->data + dbg_data_ofs;
		len -= dbg_data_ofs;

		goto fw_dbg_conf;
	}
#endif

	if (len < sizeof(*ucode)) {
		IWL_ERR(drv, "uCode has invalid length: %zd\n", len);
		return -EINVAL;
	}

	if (ucode->magic != cpu_to_le32(IWL_TLV_UCODE_MAGIC)) {
		IWL_ERR(drv, "invalid uCode magic: 0X%x\n",
			le32_to_cpu(ucode->magic));
		return -EINVAL;
	}

	drv->fw.ucode_ver = le32_to_cpu(ucode->ver);
	memcpy(drv->fw.human_readable, ucode->human_readable,
	       sizeof(drv->fw.human_readable));
	build = le32_to_cpu(ucode->build);

	if (build)
		sprintf(buildstr, " build %u", build);
	else
		buildstr[0] = '\0';

	snprintf(drv->fw.fw_version,
		 sizeof(drv->fw.fw_version),
		 "%u.%u.%u.%u%s",
		 IWL_UCODE_MAJOR(drv->fw.ucode_ver),
		 IWL_UCODE_MINOR(drv->fw.ucode_ver),
		 IWL_UCODE_API(drv->fw.ucode_ver),
		 IWL_UCODE_SERIAL(drv->fw.ucode_ver),
		 buildstr);

	data = ucode->data;

	len -= sizeof(*ucode);

#ifdef CPTCFG_IWLWIFI_SUPPORT_DEBUG_OVERRIDES
fw_dbg_conf:
#endif

	while (len >= sizeof(*tlv)) {
		len -= sizeof(*tlv);
		tlv = (void *)data;

		tlv_len = le32_to_cpu(tlv->length);
		tlv_type = le32_to_cpu(tlv->type);
		tlv_data = tlv->data;

		if (len < tlv_len) {
			IWL_ERR(drv, "invalid TLV len: %zd/%u\n",
				len, tlv_len);
			return -EINVAL;
		}
		len -= ALIGN(tlv_len, 4);
		data += sizeof(*tlv) + ALIGN(tlv_len, 4);

		switch (tlv_type) {
		case IWL_UCODE_TLV_INST:
			set_sec_data(pieces, IWL_UCODE_REGULAR,
				     IWL_UCODE_SECTION_INST, tlv_data);
			set_sec_size(pieces, IWL_UCODE_REGULAR,
				     IWL_UCODE_SECTION_INST, tlv_len);
			set_sec_offset(pieces, IWL_UCODE_REGULAR,
				       IWL_UCODE_SECTION_INST,
				       IWLAGN_RTC_INST_LOWER_BOUND);
			break;
		case IWL_UCODE_TLV_DATA:
			set_sec_data(pieces, IWL_UCODE_REGULAR,
				     IWL_UCODE_SECTION_DATA, tlv_data);
			set_sec_size(pieces, IWL_UCODE_REGULAR,
				     IWL_UCODE_SECTION_DATA, tlv_len);
			set_sec_offset(pieces, IWL_UCODE_REGULAR,
				       IWL_UCODE_SECTION_DATA,
				       IWLAGN_RTC_DATA_LOWER_BOUND);
			break;
		case IWL_UCODE_TLV_INIT:
			set_sec_data(pieces, IWL_UCODE_INIT,
				     IWL_UCODE_SECTION_INST, tlv_data);
			set_sec_size(pieces, IWL_UCODE_INIT,
				     IWL_UCODE_SECTION_INST, tlv_len);
			set_sec_offset(pieces, IWL_UCODE_INIT,
				       IWL_UCODE_SECTION_INST,
				       IWLAGN_RTC_INST_LOWER_BOUND);
			break;
		case IWL_UCODE_TLV_INIT_DATA:
			set_sec_data(pieces, IWL_UCODE_INIT,
				     IWL_UCODE_SECTION_DATA, tlv_data);
			set_sec_size(pieces, IWL_UCODE_INIT,
				     IWL_UCODE_SECTION_DATA, tlv_len);
			set_sec_offset(pieces, IWL_UCODE_INIT,
				       IWL_UCODE_SECTION_DATA,
				       IWLAGN_RTC_DATA_LOWER_BOUND);
			break;
		case IWL_UCODE_TLV_BOOT:
			IWL_ERR(drv, "Found unexpected BOOT ucode\n");
			break;
		case IWL_UCODE_TLV_PROBE_MAX_LEN:
			if (tlv_len != sizeof(u32))
				goto invalid_tlv_len;
			capa->max_probe_length =
					le32_to_cpup((__le32 *)tlv_data);
			break;
		case IWL_UCODE_TLV_PAN:
			if (tlv_len)
				goto invalid_tlv_len;
			capa->flags |= IWL_UCODE_TLV_FLAGS_PAN;
			break;
		case IWL_UCODE_TLV_FLAGS:
			/* must be at least one u32 */
			if (tlv_len < sizeof(u32))
				goto invalid_tlv_len;
			/* and a proper number of u32s */
			if (tlv_len % sizeof(u32))
				goto invalid_tlv_len;
			/*
			 * This driver only reads the first u32 as
			 * right now no more features are defined,
			 * if that changes then either the driver
			 * will not work with the new firmware, or
			 * it'll not take advantage of new features.
			 */
			capa->flags = le32_to_cpup((__le32 *)tlv_data);
			break;
		case IWL_UCODE_TLV_API_CHANGES_SET:
			if (tlv_len != sizeof(struct iwl_ucode_api))
				goto invalid_tlv_len;
			if (iwl_set_ucode_api_flags(drv, tlv_data, capa))
				goto tlv_error;
			break;
		case IWL_UCODE_TLV_ENABLED_CAPABILITIES:
			if (tlv_len != sizeof(struct iwl_ucode_capa))
				goto invalid_tlv_len;
			if (iwl_set_ucode_capabilities(drv, tlv_data, capa))
				goto tlv_error;
			break;
		case IWL_UCODE_TLV_INIT_EVTLOG_PTR:
			if (tlv_len != sizeof(u32))
				goto invalid_tlv_len;
			pieces->init_evtlog_ptr =
					le32_to_cpup((__le32 *)tlv_data);
			break;
		case IWL_UCODE_TLV_INIT_EVTLOG_SIZE:
			if (tlv_len != sizeof(u32))
				goto invalid_tlv_len;
			pieces->init_evtlog_size =
					le32_to_cpup((__le32 *)tlv_data);
			break;
		case IWL_UCODE_TLV_INIT_ERRLOG_PTR:
			if (tlv_len != sizeof(u32))
				goto invalid_tlv_len;
			pieces->init_errlog_ptr =
					le32_to_cpup((__le32 *)tlv_data);
			break;
		case IWL_UCODE_TLV_RUNT_EVTLOG_PTR:
			if (tlv_len != sizeof(u32))
				goto invalid_tlv_len;
			pieces->inst_evtlog_ptr =
					le32_to_cpup((__le32 *)tlv_data);
			break;
		case IWL_UCODE_TLV_RUNT_EVTLOG_SIZE:
			if (tlv_len != sizeof(u32))
				goto invalid_tlv_len;
			pieces->inst_evtlog_size =
					le32_to_cpup((__le32 *)tlv_data);
			break;
		case IWL_UCODE_TLV_RUNT_ERRLOG_PTR:
			if (tlv_len != sizeof(u32))
				goto invalid_tlv_len;
			pieces->inst_errlog_ptr =
					le32_to_cpup((__le32 *)tlv_data);
			break;
		case IWL_UCODE_TLV_ENHANCE_SENS_TBL:
			if (tlv_len)
				goto invalid_tlv_len;
			drv->fw.enhance_sensitivity_table = true;
			break;
		case IWL_UCODE_TLV_WOWLAN_INST:
			set_sec_data(pieces, IWL_UCODE_WOWLAN,
				     IWL_UCODE_SECTION_INST, tlv_data);
			set_sec_size(pieces, IWL_UCODE_WOWLAN,
				     IWL_UCODE_SECTION_INST, tlv_len);
			set_sec_offset(pieces, IWL_UCODE_WOWLAN,
				       IWL_UCODE_SECTION_INST,
				       IWLAGN_RTC_INST_LOWER_BOUND);
			break;
		case IWL_UCODE_TLV_WOWLAN_DATA:
			set_sec_data(pieces, IWL_UCODE_WOWLAN,
				     IWL_UCODE_SECTION_DATA, tlv_data);
			set_sec_size(pieces, IWL_UCODE_WOWLAN,
				     IWL_UCODE_SECTION_DATA, tlv_len);
			set_sec_offset(pieces, IWL_UCODE_WOWLAN,
				       IWL_UCODE_SECTION_DATA,
				       IWLAGN_RTC_DATA_LOWER_BOUND);
			break;
		case IWL_UCODE_TLV_PHY_CALIBRATION_SIZE:
			if (tlv_len != sizeof(u32))
				goto invalid_tlv_len;
			capa->standard_phy_calibration_size =
					le32_to_cpup((__le32 *)tlv_data);
			break;
		 case IWL_UCODE_TLV_SEC_RT:
			iwl_store_ucode_sec(pieces, tlv_data, IWL_UCODE_REGULAR,
					    tlv_len);
			drv->fw.type = IWL_FW_MVM;
			break;
		case IWL_UCODE_TLV_SEC_INIT:
			iwl_store_ucode_sec(pieces, tlv_data, IWL_UCODE_INIT,
					    tlv_len);
			drv->fw.type = IWL_FW_MVM;
			break;
		case IWL_UCODE_TLV_SEC_WOWLAN:
			iwl_store_ucode_sec(pieces, tlv_data, IWL_UCODE_WOWLAN,
					    tlv_len);
			drv->fw.type = IWL_FW_MVM;
			break;
		case IWL_UCODE_TLV_DEF_CALIB:
			if (tlv_len != sizeof(struct iwl_tlv_calib_data))
				goto invalid_tlv_len;
			if (iwl_set_default_calib(drv, tlv_data))
				goto tlv_error;
			break;
		case IWL_UCODE_TLV_PHY_SKU:
			if (tlv_len != sizeof(u32))
				goto invalid_tlv_len;
			drv->fw.phy_config = le32_to_cpup((__le32 *)tlv_data);
#ifdef CPTCFG_IWLWIFI_SUPPORT_DEBUG_OVERRIDES
			if (drv->trans->dbg_cfg.valid_ants & ~ANT_ABC)
				IWL_ERR(drv,
					"Invalid value for antennas: 0x%x\n",
					drv->trans->dbg_cfg.valid_ants);
			/* Make sure value stays in range */
			drv->trans->dbg_cfg.valid_ants &= ANT_ABC;
			if (drv->trans->dbg_cfg.valid_ants) {
				u32 phy_config = ~(FW_PHY_CFG_TX_CHAIN |
						   FW_PHY_CFG_RX_CHAIN);

				phy_config |=
					(drv->trans->dbg_cfg.valid_ants <<
					 FW_PHY_CFG_TX_CHAIN_POS);
				phy_config |=
					(drv->trans->dbg_cfg.valid_ants <<
					 FW_PHY_CFG_RX_CHAIN_POS);

				drv->fw.phy_config &= phy_config;
			}
#endif
			drv->fw.valid_tx_ant = (drv->fw.phy_config &
						FW_PHY_CFG_TX_CHAIN) >>
						FW_PHY_CFG_TX_CHAIN_POS;
			drv->fw.valid_rx_ant = (drv->fw.phy_config &
						FW_PHY_CFG_RX_CHAIN) >>
						FW_PHY_CFG_RX_CHAIN_POS;
			break;
		 case IWL_UCODE_TLV_SECURE_SEC_RT:
			iwl_store_ucode_sec(pieces, tlv_data, IWL_UCODE_REGULAR,
					    tlv_len);
			drv->fw.type = IWL_FW_MVM;
			break;
		case IWL_UCODE_TLV_SECURE_SEC_INIT:
			iwl_store_ucode_sec(pieces, tlv_data, IWL_UCODE_INIT,
					    tlv_len);
			drv->fw.type = IWL_FW_MVM;
			break;
		case IWL_UCODE_TLV_SECURE_SEC_WOWLAN:
			iwl_store_ucode_sec(pieces, tlv_data, IWL_UCODE_WOWLAN,
					    tlv_len);
			drv->fw.type = IWL_FW_MVM;
			break;
		case IWL_UCODE_TLV_NUM_OF_CPU:
			if (tlv_len != sizeof(u32))
				goto invalid_tlv_len;
			num_of_cpus =
				le32_to_cpup((__le32 *)tlv_data);

			if (num_of_cpus == 2) {
				drv->fw.img[IWL_UCODE_REGULAR].is_dual_cpus =
					true;
				drv->fw.img[IWL_UCODE_INIT].is_dual_cpus =
					true;
				drv->fw.img[IWL_UCODE_WOWLAN].is_dual_cpus =
					true;
			} else if ((num_of_cpus > 2) || (num_of_cpus < 1)) {
				IWL_ERR(drv, "Driver support upto 2 CPUs\n");
				return -EINVAL;
			}
			break;
		case IWL_UCODE_TLV_CSCHEME:
			if (iwl_store_cscheme(&drv->fw, tlv_data, tlv_len))
				goto invalid_tlv_len;
			break;
		case IWL_UCODE_TLV_N_SCAN_CHANNELS:
			if (tlv_len != sizeof(u32))
				goto invalid_tlv_len;
			capa->n_scan_channels =
				le32_to_cpup((__le32 *)tlv_data);
			break;
		case IWL_UCODE_TLV_FW_VERSION: {
			__le32 *ptr = (void *)tlv_data;
			u32 major, minor;
			u8 local_comp;

			if (tlv_len != sizeof(u32) * 3)
				goto invalid_tlv_len;

			major = le32_to_cpup(ptr++);
			minor = le32_to_cpup(ptr++);
			local_comp = le32_to_cpup(ptr);

			snprintf(drv->fw.fw_version,
				 sizeof(drv->fw.fw_version), "%u.%u.%u",
				 major, minor, local_comp);
			break;
			}
		case IWL_UCODE_TLV_FW_DBG_DEST: {
			struct iwl_fw_dbg_dest_tlv *dest = (void *)tlv_data;

#ifdef CPTCFG_IWLWIFI_DEVICE_TESTMODE
#ifdef CPTCFG_IWLWIFI_SUPPORT_DEBUG_OVERRIDES
			if (drv->trans->dbg_cfg.dbm_destination_path) {
				IWL_ERR(drv,
					"Ignoring destination, ini file present\n");
				break;
			}
#endif
#endif

			if (pieces->dbg_dest_tlv) {
				IWL_ERR(drv,
					"dbg destination ignored, already exists\n");
				break;
			}

			pieces->dbg_dest_tlv = dest;
			IWL_INFO(drv, "Found debug destination: %s\n",
				 get_fw_dbg_mode_string(dest->monitor_mode));

			drv->fw.dbg_dest_reg_num =
				tlv_len - offsetof(struct iwl_fw_dbg_dest_tlv,
						   reg_ops);
			drv->fw.dbg_dest_reg_num /=
				sizeof(drv->fw.dbg_dest_tlv->reg_ops[0]);

			break;
			}
		case IWL_UCODE_TLV_FW_DBG_CONF: {
			struct iwl_fw_dbg_conf_tlv *conf = (void *)tlv_data;

			if (!pieces->dbg_dest_tlv) {
				IWL_ERR(drv,
					"Ignore dbg config %d - no destination configured\n",
					conf->id);
				break;
			}

			if (conf->id >= ARRAY_SIZE(drv->fw.dbg_conf_tlv)) {
				IWL_ERR(drv,
					"Skip unknown configuration: %d\n",
					conf->id);
				break;
			}

			if (pieces->dbg_conf_tlv[conf->id]) {
				IWL_ERR(drv,
					"Ignore duplicate dbg config %d\n",
					conf->id);
				break;
			}

			if (conf->usniffer)
				usniffer_req = true;

			IWL_INFO(drv, "Found debug configuration: %d\n",
				 conf->id);

			pieces->dbg_conf_tlv[conf->id] = conf;
			pieces->dbg_conf_tlv_len[conf->id] = tlv_len;
			break;
			}
		case IWL_UCODE_TLV_FW_DBG_TRIGGER: {
			struct iwl_fw_dbg_trigger_tlv *trigger =
				(void *)tlv_data;
			u32 trigger_id = le32_to_cpu(trigger->id);

			if (trigger_id >= ARRAY_SIZE(drv->fw.dbg_trigger_tlv)) {
				IWL_ERR(drv,
					"Skip unknown trigger: %u\n",
					trigger->id);
				break;
			}

			if (pieces->dbg_trigger_tlv[trigger_id]) {
				IWL_ERR(drv,
					"Ignore duplicate dbg trigger %u\n",
					trigger->id);
				break;
			}

			IWL_INFO(drv, "Found debug trigger: %u\n", trigger->id);

			pieces->dbg_trigger_tlv[trigger_id] = trigger;
			pieces->dbg_trigger_tlv_len[trigger_id] = tlv_len;
			break;
			}
		case IWL_UCODE_TLV_SEC_RT_USNIFFER:
			*usniffer_images = true;
			iwl_store_ucode_sec(pieces, tlv_data,
					    IWL_UCODE_REGULAR_USNIFFER,
					    tlv_len);
			break;
		case IWL_UCODE_TLV_PAGING:
			if (tlv_len != sizeof(u32))
				goto invalid_tlv_len;
			paging_mem_size = le32_to_cpup((__le32 *)tlv_data);

			IWL_DEBUG_FW(drv,
				     "Paging: paging enabled (size = %u bytes)\n",
				     paging_mem_size);

			if (paging_mem_size > MAX_PAGING_IMAGE_SIZE) {
				IWL_ERR(drv,
					"Paging: driver supports up to %lu bytes for paging image\n",
					MAX_PAGING_IMAGE_SIZE);
				return -EINVAL;
			}

			if (paging_mem_size & (FW_PAGING_SIZE - 1)) {
				IWL_ERR(drv,
					"Paging: image isn't multiple %lu\n",
					FW_PAGING_SIZE);
				return -EINVAL;
			}

			drv->fw.img[IWL_UCODE_REGULAR].paging_mem_size =
				paging_mem_size;
			usniffer_img = IWL_UCODE_REGULAR_USNIFFER;
			drv->fw.img[usniffer_img].paging_mem_size =
				paging_mem_size;
			break;
		case IWL_UCODE_TLV_SDIO_ADMA_ADDR:
			if (tlv_len != sizeof(u32))
				goto invalid_tlv_len;
			drv->fw.sdio_adma_addr =
				le32_to_cpup((__le32 *)tlv_data);
			break;
		case IWL_UCODE_TLV_FW_GSCAN_CAPA:
			/*
			 * Don't return an error in case of a shorter tlv_len
			 * to enable loading of FW that has an old format
			 * of GSCAN capabilities TLV.
			 */
			if (tlv_len < sizeof(struct iwl_fw_gscan_capabilities))
				break;

			iwl_store_gscan_capa(&drv->fw, tlv_data, tlv_len);
			gscan_capa = true;
			break;
		case IWL_UCODE_TLV_FW_MEM_SEG: {
			struct iwl_fw_dbg_mem_seg_tlv *dbg_mem =
				(void *)tlv_data;
			u32 type;

			if (tlv_len != (sizeof(*dbg_mem)))
				goto invalid_tlv_len;

			type = le32_to_cpu(dbg_mem->data_type);
			drv->fw.dbg_dynamic_mem = true;

			if (type >= ARRAY_SIZE(drv->fw.dbg_mem_tlv)) {
				IWL_ERR(drv,
					"Skip unknown dbg mem segment: %u\n",
					dbg_mem->data_type);
				break;
			}

			if (pieces->dbg_mem_tlv[type]) {
				IWL_ERR(drv,
					"Ignore duplicate mem segment: %u\n",
					dbg_mem->data_type);
				break;
			}

			IWL_DEBUG_INFO(drv, "Found debug memory segment: %u\n",
				       dbg_mem->data_type);

			pieces->dbg_mem_tlv[type] = dbg_mem;
			break;
			}
		default:
			IWL_DEBUG_INFO(drv, "unknown TLV: %d\n", tlv_type);
			break;
		}
	}

	if (!fw_has_capa(capa, IWL_UCODE_TLV_CAPA_USNIFFER_UNIFIED) &&
	    usniffer_req && !*usniffer_images) {
		IWL_ERR(drv,
			"user selected to work with usniffer but usniffer image isn't available in ucode package\n");
		return -EINVAL;
	}

	if (len) {
		IWL_ERR(drv, "invalid TLV after parsing: %zd\n", len);
		iwl_print_hex_dump(drv, IWL_DL_FW, (u8 *)data, len);
		return -EINVAL;
	}

	/*
	 * If ucode advertises that it supports GSCAN but GSCAN
	 * capabilities TLV is not present, or if it has an old format,
	 * warn and continue without GSCAN.
	 */
	if (fw_has_capa(capa, IWL_UCODE_TLV_CAPA_GSCAN_SUPPORT) &&
	    !gscan_capa) {
		IWL_DEBUG_INFO(drv,
			       "GSCAN is supported but capabilities TLV is unavailable\n");
		__clear_bit((__force long)IWL_UCODE_TLV_CAPA_GSCAN_SUPPORT,
			    capa->_capa);
	}

	return 0;

 invalid_tlv_len:
	IWL_ERR(drv, "TLV %d has invalid size: %u\n", tlv_type, tlv_len);
 tlv_error:
	iwl_print_hex_dump(drv, IWL_DL_FW, tlv_data, tlv_len);

	return -EINVAL;
}

static int iwl_alloc_ucode(struct iwl_drv *drv,
			   struct iwl_firmware_pieces *pieces,
			   enum iwl_ucode_type type)
{
	int i;
	for (i = 0;
	     i < IWL_UCODE_SECTION_MAX && get_sec_size(pieces, type, i);
	     i++)
		if (iwl_alloc_fw_desc(drv, &(drv->fw.img[type].sec[i]),
				      get_sec(pieces, type, i)))
			return -ENOMEM;
	return 0;
}

static int validate_sec_sizes(struct iwl_drv *drv,
			      struct iwl_firmware_pieces *pieces,
			      const struct iwl_cfg *cfg)
{
	IWL_DEBUG_INFO(drv, "f/w package hdr runtime inst size = %Zd\n",
		get_sec_size(pieces, IWL_UCODE_REGULAR,
			     IWL_UCODE_SECTION_INST));
	IWL_DEBUG_INFO(drv, "f/w package hdr runtime data size = %Zd\n",
		get_sec_size(pieces, IWL_UCODE_REGULAR,
			     IWL_UCODE_SECTION_DATA));
	IWL_DEBUG_INFO(drv, "f/w package hdr init inst size = %Zd\n",
		get_sec_size(pieces, IWL_UCODE_INIT, IWL_UCODE_SECTION_INST));
	IWL_DEBUG_INFO(drv, "f/w package hdr init data size = %Zd\n",
		get_sec_size(pieces, IWL_UCODE_INIT, IWL_UCODE_SECTION_DATA));

	/* Verify that uCode images will fit in card's SRAM. */
	if (get_sec_size(pieces, IWL_UCODE_REGULAR, IWL_UCODE_SECTION_INST) >
	    cfg->max_inst_size) {
		IWL_ERR(drv, "uCode instr len %Zd too large to fit in\n",
			get_sec_size(pieces, IWL_UCODE_REGULAR,
				     IWL_UCODE_SECTION_INST));
		return -1;
	}

	if (get_sec_size(pieces, IWL_UCODE_REGULAR, IWL_UCODE_SECTION_DATA) >
	    cfg->max_data_size) {
		IWL_ERR(drv, "uCode data len %Zd too large to fit in\n",
			get_sec_size(pieces, IWL_UCODE_REGULAR,
				     IWL_UCODE_SECTION_DATA));
		return -1;
	}

	if (get_sec_size(pieces, IWL_UCODE_INIT, IWL_UCODE_SECTION_INST) >
	     cfg->max_inst_size) {
		IWL_ERR(drv, "uCode init instr len %Zd too large to fit in\n",
			get_sec_size(pieces, IWL_UCODE_INIT,
				     IWL_UCODE_SECTION_INST));
		return -1;
	}

	if (get_sec_size(pieces, IWL_UCODE_INIT, IWL_UCODE_SECTION_DATA) >
	    cfg->max_data_size) {
		IWL_ERR(drv, "uCode init data len %Zd too large to fit in\n",
			get_sec_size(pieces, IWL_UCODE_REGULAR,
				     IWL_UCODE_SECTION_DATA));
		return -1;
	}
	return 0;
}

static struct iwl_op_mode *
_iwl_op_mode_start(struct iwl_drv *drv, struct iwlwifi_opmode_table *op)
{
	const struct iwl_op_mode_ops *ops = op->ops;
	struct dentry *dbgfs_dir = NULL;
	struct iwl_op_mode *op_mode = NULL;

#ifdef CPTCFG_IWLWIFI_DEBUGFS
	drv->dbgfs_op_mode = debugfs_create_dir(op->name,
						drv->dbgfs_drv);
	if (!drv->dbgfs_op_mode) {
		IWL_ERR(drv,
			"failed to create opmode debugfs directory\n");
		return op_mode;
	}
	dbgfs_dir = drv->dbgfs_op_mode;
#endif

	op_mode = ops->start(drv->trans, drv->cfg, &drv->fw, dbgfs_dir);

	if (!op_mode) {
#ifdef CPTCFG_IWLWIFI_DEBUGFS
		debugfs_remove_recursive(drv->dbgfs_op_mode);
		drv->dbgfs_op_mode = NULL;
		return NULL;
#endif
	}

	return op_mode;
}

static void _iwl_op_mode_stop(struct iwl_drv *drv)
{
	/* op_mode can be NULL if its start failed */
	if (drv->op_mode) {
		iwl_op_mode_stop(drv->op_mode);
		drv->op_mode = NULL;

#ifdef CPTCFG_IWLWIFI_DEBUGFS
		debugfs_remove_recursive(drv->dbgfs_op_mode);
		drv->dbgfs_op_mode = NULL;
#endif
	}
}

/**
 * iwl_req_fw_callback - callback when firmware was loaded
 *
 * If loaded successfully, copies the firmware into buffers
 * for the card to fetch (via DMA).
 */
static void iwl_req_fw_callback(const struct firmware *ucode_raw, void *context)
{
	struct iwl_drv *drv = context;
	struct iwl_fw *fw = &drv->fw;
	struct iwl_ucode_header *ucode;
	struct iwlwifi_opmode_table *op;
	int err;
	struct iwl_firmware_pieces *pieces;
	const unsigned int api_max = drv->cfg->ucode_api_max;
	const unsigned int api_min = drv->cfg->ucode_api_min;
	size_t trigger_tlv_sz[FW_DBG_TRIGGER_MAX];
	u32 api_ver;
	int i;
	bool load_module = false;
	bool usniffer_images = false;

#ifdef CPTCFG_IWLWIFI_SUPPORT_DEBUG_OVERRIDES
	const struct firmware *fw_dbg_config;
	int load_fw_dbg_err = -ENOENT;
#endif

	fw->ucode_capa.max_probe_length = IWL_DEFAULT_MAX_PROBE_LENGTH;
	fw->ucode_capa.standard_phy_calibration_size =
			IWL_DEFAULT_STANDARD_PHY_CALIBRATE_TBL_SIZE;
	fw->ucode_capa.n_scan_channels = IWL_DEFAULT_SCAN_CHANNELS;

	pieces = kzalloc(sizeof(*pieces), GFP_KERNEL);
	if (!pieces)
		return;

	if (!ucode_raw)
		goto try_again;

	IWL_DEBUG_INFO(drv, "Loaded firmware file '%s' (%zd bytes).\n",
		       drv->firmware_name, ucode_raw->size);

	/* Make sure that we got at least the API version number */
	if (ucode_raw->size < 4) {
		IWL_ERR(drv, "File size way too small!\n");
		goto try_again;
	}

	/* Data from ucode file:  header followed by uCode images */
	ucode = (struct iwl_ucode_header *)ucode_raw->data;

	if (ucode->ver)
		err = iwl_parse_v1_v2_firmware(drv, ucode_raw, pieces);
	else
		err = iwl_parse_tlv_firmware(drv, ucode_raw, pieces,
					     &fw->ucode_capa, &usniffer_images);

	if (err)
		goto try_again;

#ifdef CPTCFG_IWLWIFI_SUPPORT_DEBUG_OVERRIDES
	if (!ucode->ver && drv->trans->dbg_cfg.fw_dbg_conf) {
		load_fw_dbg_err =
			request_firmware(&fw_dbg_config,
					 drv->trans->dbg_cfg.fw_dbg_conf,
					 drv->trans->dev);
		if (!load_fw_dbg_err) {
			struct iwl_ucode_capabilities capa = {};

			err = iwl_parse_tlv_firmware(drv, fw_dbg_config, pieces,
						     &capa, &usniffer_images);
			if (err)
				IWL_ERR(drv,
					"Failed to configure FW DBG data!\n");
		}
	}
#endif

	if (fw_has_api(&drv->fw.ucode_capa, IWL_UCODE_TLV_API_NEW_VERSION))
		api_ver = drv->fw.ucode_ver;
	else
		api_ver = IWL_UCODE_API(drv->fw.ucode_ver);

	/*
	 * api_ver should match the api version forming part of the
	 * firmware filename ... but we don't check for that and only rely
	 * on the API version read from firmware header from here on forward
	 */
	if (api_ver < api_min || api_ver > api_max) {
		IWL_ERR(drv,
			"Driver unable to support your firmware API. "
			"Driver supports v%u, firmware is v%u.\n",
			api_max, api_ver);
		goto try_again;
	}

	/*
	 * In mvm uCode there is no difference between data and instructions
	 * sections.
	 */
	if (fw->type == IWL_FW_DVM && validate_sec_sizes(drv, pieces, drv->cfg))
		goto try_again;

	/* Allocate ucode buffers for card's bus-master loading ... */

	/* Runtime instructions and 2 copies of data:
	 * 1) unmodified from disk
	 * 2) backup cache for save/restore during power-downs */
	for (i = 0; i < IWL_UCODE_TYPE_MAX; i++)
		if (iwl_alloc_ucode(drv, pieces, i))
			goto out_free_fw;

	if (pieces->dbg_dest_tlv) {
		drv->fw.dbg_dest_tlv =
			kmemdup(pieces->dbg_dest_tlv,
				sizeof(*pieces->dbg_dest_tlv) +
				sizeof(pieces->dbg_dest_tlv->reg_ops[0]) *
				drv->fw.dbg_dest_reg_num, GFP_KERNEL);

		if (!drv->fw.dbg_dest_tlv)
			goto out_free_fw;
	}

	for (i = 0; i < ARRAY_SIZE(drv->fw.dbg_conf_tlv); i++) {
		if (pieces->dbg_conf_tlv[i]) {
			drv->fw.dbg_conf_tlv_len[i] =
				pieces->dbg_conf_tlv_len[i];
			drv->fw.dbg_conf_tlv[i] =
				kmemdup(pieces->dbg_conf_tlv[i],
					drv->fw.dbg_conf_tlv_len[i],
					GFP_KERNEL);
			if (!drv->fw.dbg_conf_tlv[i])
				goto out_free_fw;
		}
	}

	memset(&trigger_tlv_sz, 0xff, sizeof(trigger_tlv_sz));

	trigger_tlv_sz[FW_DBG_TRIGGER_MISSED_BEACONS] =
		sizeof(struct iwl_fw_dbg_trigger_missed_bcon);
	trigger_tlv_sz[FW_DBG_TRIGGER_CHANNEL_SWITCH] = 0;
	trigger_tlv_sz[FW_DBG_TRIGGER_FW_NOTIF] =
		sizeof(struct iwl_fw_dbg_trigger_cmd);
	trigger_tlv_sz[FW_DBG_TRIGGER_MLME] =
		sizeof(struct iwl_fw_dbg_trigger_mlme);
	trigger_tlv_sz[FW_DBG_TRIGGER_STATS] =
		sizeof(struct iwl_fw_dbg_trigger_stats);
	trigger_tlv_sz[FW_DBG_TRIGGER_RSSI] =
		sizeof(struct iwl_fw_dbg_trigger_low_rssi);
	trigger_tlv_sz[FW_DBG_TRIGGER_TXQ_TIMERS] =
		sizeof(struct iwl_fw_dbg_trigger_txq_timer);
	trigger_tlv_sz[FW_DBG_TRIGGER_TIME_EVENT] =
		sizeof(struct iwl_fw_dbg_trigger_time_event);
	trigger_tlv_sz[FW_DBG_TRIGGER_BA] =
		sizeof(struct iwl_fw_dbg_trigger_ba);
#ifdef CPTCFG_MAC80211_LATENCY_MEASUREMENTS
	trigger_tlv_sz[FW_DBG_TRIGGER_TX_LATENCY] =
		sizeof(struct iwl_fw_dbg_trigger_tx_latency);
#endif /* CPTCFG_MAC80211_LATENCY_MEASUREMENTS */
	trigger_tlv_sz[FW_DBG_TRIGGER_TDLS] =
		sizeof(struct iwl_fw_dbg_trigger_tdls);

	for (i = 0; i < ARRAY_SIZE(drv->fw.dbg_trigger_tlv); i++) {
		if (pieces->dbg_trigger_tlv[i]) {
			/*
			 * If the trigger isn't long enough, WARN and exit.
			 * Someone is trying to debug something and he won't
			 * be able to catch the bug he is trying to chase.
			 * We'd better be noisy to be sure he knows what's
			 * going on.
			 */
			if (WARN_ON(pieces->dbg_trigger_tlv_len[i] <
				    (trigger_tlv_sz[i] +
				     sizeof(struct iwl_fw_dbg_trigger_tlv))))
				goto out_free_fw;
			drv->fw.dbg_trigger_tlv_len[i] =
				pieces->dbg_trigger_tlv_len[i];
			drv->fw.dbg_trigger_tlv[i] =
				kmemdup(pieces->dbg_trigger_tlv[i],
					drv->fw.dbg_trigger_tlv_len[i],
					GFP_KERNEL);
			if (!drv->fw.dbg_trigger_tlv[i])
				goto out_free_fw;
		}
	}

	for (i = 0; i < ARRAY_SIZE(drv->fw.dbg_mem_tlv); i++) {
		if (pieces->dbg_mem_tlv[i]) {
			drv->fw.dbg_mem_tlv[i] =
				kmemdup(pieces->dbg_mem_tlv[i],
					sizeof(*drv->fw.dbg_mem_tlv[i]),
					GFP_KERNEL);
			if (!drv->fw.dbg_mem_tlv[i])
				goto out_free_fw;
		}
	}

	/* Now that we can no longer fail, copy information */

	/*
	 * The (size - 16) / 12 formula is based on the information recorded
	 * for each event, which is of mode 1 (including timestamp) for all
	 * new microcodes that include this information.
	 */
	fw->init_evtlog_ptr = pieces->init_evtlog_ptr;
	if (pieces->init_evtlog_size)
		fw->init_evtlog_size = (pieces->init_evtlog_size - 16)/12;
	else
		fw->init_evtlog_size =
			drv->cfg->base_params->max_event_log_size;
	fw->init_errlog_ptr = pieces->init_errlog_ptr;
	fw->inst_evtlog_ptr = pieces->inst_evtlog_ptr;
	if (pieces->inst_evtlog_size)
		fw->inst_evtlog_size = (pieces->inst_evtlog_size - 16)/12;
	else
		fw->inst_evtlog_size =
			drv->cfg->base_params->max_event_log_size;
	fw->inst_errlog_ptr = pieces->inst_errlog_ptr;

	/*
	 * figure out the offset of chain noise reset and gain commands
	 * base on the size of standard phy calibration commands table size
	 */
	if (fw->ucode_capa.standard_phy_calibration_size >
	    IWL_MAX_PHY_CALIBRATE_TBL_SIZE)
		fw->ucode_capa.standard_phy_calibration_size =
			IWL_MAX_STANDARD_PHY_CALIBRATE_TBL_SIZE;

	/* We have our copies now, allow OS release its copies */
	release_firmware(ucode_raw);

#ifdef CPTCFG_IWLWIFI_SUPPORT_DEBUG_OVERRIDES
	if (!load_fw_dbg_err)
		release_firmware(fw_dbg_config);
#endif

	mutex_lock(&iwlwifi_opmode_table_mtx);
	switch (fw->type) {
	case IWL_FW_DVM:
		op = &iwlwifi_opmode_table[DVM_OP_MODE];
		break;
	default:
		WARN(1, "Invalid fw type %d\n", fw->type);
	case IWL_FW_MVM:
		op = &iwlwifi_opmode_table[MVM_OP_MODE];
		break;
	}

#if IS_ENABLED(CPTCFG_IWLXVT)
	if (iwlwifi_mod_params.xvt_default_mode && drv->fw.type == IWL_FW_MVM)
		op = &iwlwifi_opmode_table[XVT_OP_MODE];

	drv->xvt_mode_on = (op == &iwlwifi_opmode_table[XVT_OP_MODE]);
#endif

	IWL_INFO(drv, "loaded firmware version %s op_mode %s\n",
		 drv->fw.fw_version, op->name);

	/* add this device to the list of devices using this op_mode */
	list_add_tail(&drv->list, &op->drv);

	if (op->ops) {
		drv->op_mode = _iwl_op_mode_start(drv, op);

		if (!drv->op_mode) {
			mutex_unlock(&iwlwifi_opmode_table_mtx);
			goto out_unbind;
		}
	} else {
		load_module = true;
	}
	mutex_unlock(&iwlwifi_opmode_table_mtx);

	/*
	 * Complete the firmware request last so that
	 * a driver unbind (stop) doesn't run while we
	 * are doing the start() above.
	 */
	complete(&drv->request_firmware_complete);

	/*
	 * Load the module last so we don't block anything
	 * else from proceeding if the module fails to load
	 * or hangs loading.
	 */
	if (load_module) {
		err = request_module("%s", op->name);
#ifdef CPTCFG_IWLWIFI_OPMODE_MODULAR
		if (err)
			IWL_ERR(drv,
				"failed to load module %s (error %d), is dynamic loading enabled?\n",
				op->name, err);
#endif
	}
	kfree(pieces);
	return;

 try_again:
	/* try next, if any */
	release_firmware(ucode_raw);
	if (iwl_request_firmware(drv, false))
		goto out_unbind;
	kfree(pieces);
	return;

 out_free_fw:
	IWL_ERR(drv, "failed to allocate pci memory\n");
	iwl_dealloc_ucode(drv);
	release_firmware(ucode_raw);
 out_unbind:
	kfree(pieces);
	complete(&drv->request_firmware_complete);
	device_release_driver(drv->trans->dev);
}

struct iwl_drv *iwl_drv_start(struct iwl_trans *trans,
			      const struct iwl_cfg *cfg)
{
	struct iwl_drv *drv;
	int ret;

	drv = kzalloc(sizeof(*drv), GFP_KERNEL);
	if (!drv) {
		ret = -ENOMEM;
		goto err;
	}

	drv->trans = trans;
	drv->dev = trans->dev;
	drv->cfg = cfg;

	init_completion(&drv->request_firmware_complete);
	INIT_LIST_HEAD(&drv->list);

#ifdef CPTCFG_IWLWIFI_SUPPORT_DEBUG_OVERRIDES
	trans->dbg_cfg = current_dbg_config;
	iwl_dbg_cfg_load_ini(drv->trans->dev, &drv->trans->dbg_cfg);
#endif

#ifdef CPTCFG_IWLWIFI_DEBUGFS
	/* Create the device debugfs entries. */
	drv->dbgfs_drv = debugfs_create_dir(dev_name(trans->dev),
					    iwl_dbgfs_root);

	if (!drv->dbgfs_drv) {
		IWL_ERR(drv, "failed to create debugfs directory\n");
		ret = -ENOMEM;
		goto err_free_drv;
	}

	/* Create transport layer debugfs dir */
	drv->trans->dbgfs_dir = debugfs_create_dir("trans", drv->dbgfs_drv);

	if (!drv->trans->dbgfs_dir) {
		IWL_ERR(drv, "failed to create transport debugfs directory\n");
		ret = -ENOMEM;
		goto err_free_dbgfs;
	}
#endif

#ifdef CPTCFG_IWLWIFI_DEVICE_TESTMODE
	iwl_tm_gnl_add(drv->trans);
#endif

	ret = iwl_request_firmware(drv, true);
	if (ret) {
		IWL_ERR(trans, "Couldn't request the fw\n");
		goto err_fw;
	}

#if IS_ENABLED(CPTCFG_IWLXVT)
	ret = iwl_create_sysfs_file(drv);
	if (ret) {
		IWL_ERR(trans, "Couldn't create sysfs entry\n");
		goto err_fw;
	}
#endif

	return drv;

err_fw:
#ifdef CPTCFG_IWLWIFI_DEVICE_TESTMODE
	iwl_tm_gnl_remove(drv->trans);
#endif
#ifdef CPTCFG_IWLWIFI_DEBUGFS
err_free_dbgfs:
	debugfs_remove_recursive(drv->dbgfs_drv);
err_free_drv:
#endif
	kfree(drv);
err:
	return ERR_PTR(ret);
}

void iwl_drv_stop(struct iwl_drv *drv)
{
	wait_for_completion(&drv->request_firmware_complete);

	_iwl_op_mode_stop(drv);

	iwl_dealloc_ucode(drv);

	mutex_lock(&iwlwifi_opmode_table_mtx);
	/*
	 * List is empty (this item wasn't added)
	 * when firmware loading failed -- in that
	 * case we can't remove it from any list.
	 */
	if (!list_empty(&drv->list))
		list_del(&drv->list);
	mutex_unlock(&iwlwifi_opmode_table_mtx);

#ifdef CPTCFG_IWLWIFI_DEBUGFS
	debugfs_remove_recursive(drv->dbgfs_drv);
#endif

#ifdef CPTCFG_IWLWIFI_SUPPORT_DEBUG_OVERRIDES
	iwl_dbg_cfg_free(&drv->trans->dbg_cfg);
#endif

#if IS_ENABLED(CPTCFG_IWLXVT)
	iwl_remove_sysfs_file(drv);
#endif

#ifdef CPTCFG_IWLWIFI_DEVICE_TESTMODE
	iwl_tm_gnl_remove(drv->trans);
#endif

	kfree(drv);
}


/* shared module parameters */
struct iwl_mod_params iwlwifi_mod_params = {
	.restart_fw = true,
	.bt_coex_active = true,
	.power_level = IWL_POWER_INDEX_1,
	.d0i3_disable = IS_ENABLED(CPTCFG_IWLWIFI_D0I3_DEFAULT_DISABLE),
	.d0i3_entry_delay = 1000,
	.uapsd_disable = IWL_DISABLE_UAPSD_BSS | IWL_DISABLE_UAPSD_P2P_CLIENT,
	/* the rest are 0 by default */
};
IWL_EXPORT_SYMBOL(iwlwifi_mod_params);

int iwl_opmode_register(const char *name, const struct iwl_op_mode_ops *ops)
{
	int i;
	struct iwl_drv *drv;
	struct iwlwifi_opmode_table *op;

	mutex_lock(&iwlwifi_opmode_table_mtx);
	for (i = 0; i < ARRAY_SIZE(iwlwifi_opmode_table); i++) {
		op = &iwlwifi_opmode_table[i];
		if (strcmp(op->name, name))
			continue;
		op->ops = ops;
		/* TODO: need to handle exceptional case */
		list_for_each_entry(drv, &op->drv, list)
			drv->op_mode = _iwl_op_mode_start(drv, op);

		mutex_unlock(&iwlwifi_opmode_table_mtx);
		return 0;
	}
	mutex_unlock(&iwlwifi_opmode_table_mtx);
	return -EIO;
}
IWL_EXPORT_SYMBOL(iwl_opmode_register);

void iwl_opmode_deregister(const char *name)
{
	int i;
	struct iwl_drv *drv;

	mutex_lock(&iwlwifi_opmode_table_mtx);
	for (i = 0; i < ARRAY_SIZE(iwlwifi_opmode_table); i++) {
		if (strcmp(iwlwifi_opmode_table[i].name, name))
			continue;
		iwlwifi_opmode_table[i].ops = NULL;

		/* call the stop routine for all devices */
		list_for_each_entry(drv, &iwlwifi_opmode_table[i].drv, list)
			_iwl_op_mode_stop(drv);

		mutex_unlock(&iwlwifi_opmode_table_mtx);
		return;
	}
	mutex_unlock(&iwlwifi_opmode_table_mtx);
}
IWL_EXPORT_SYMBOL(iwl_opmode_deregister);

/*
 * Register all supported buses.
 * Return 0 if all of the bus registrations were successful.
 */
static int iwl_register_bus_drivers(void)
{
	int ret;

	ret = iwl_pci_register_driver();
	if (ret)
		return ret;

	ret = iwl_slv_register_drivers();
	if (ret)
		goto unregister_pci_driver;

	return 0;

unregister_pci_driver:
	iwl_pci_unregister_driver();
	return ret;
}

/*
 * Unregisters all registered bus drivers.
 *
 * These bus drivers must be already registered since if their
 * registration failed the driver will not be loaded.
 */
static void iwl_unregister_bus_drivers(void)
{
	iwl_pci_unregister_driver();
	iwl_slv_unregister_drivers();
}

static int __init iwl_drv_init(void)
{
	int i;

	mutex_init(&iwlwifi_opmode_table_mtx);

	for (i = 0; i < ARRAY_SIZE(iwlwifi_opmode_table); i++)
		INIT_LIST_HEAD(&iwlwifi_opmode_table[i].drv);

#ifdef CPTCFG_IWLWIFI_DEVICE_TESTMODE
	if (iwl_tm_gnl_init())
		return -EFAULT;
#endif

#if IS_ENABLED(CPTCFG_IWLXVT)
	iwl_kobj = kobject_create_and_add("devices", &THIS_MODULE->mkobj.kobj);
	if (!iwl_kobj)
		return -ENOMEM;
#endif

	pr_info(DRV_DESCRIPTION "\n");
	pr_info(DRV_COPYRIGHT "\n");

#ifdef CPTCFG_IWLWIFI_DEBUGFS
	/* Create the root of iwlwifi debugfs subsystem. */
	iwl_dbgfs_root = debugfs_create_dir(DRV_NAME, NULL);

	if (!iwl_dbgfs_root)
		return -EFAULT;
#endif

	return iwl_register_bus_drivers();
}
module_init(iwl_drv_init);

static void __exit iwl_drv_exit(void)
{
	iwl_unregister_bus_drivers();

#ifdef CPTCFG_IWLWIFI_DEBUGFS
	debugfs_remove_recursive(iwl_dbgfs_root);
#endif

#if IS_ENABLED(CPTCFG_IWLXVT)
	if (iwl_kobj)
		kobject_put(iwl_kobj);
#endif

#ifdef CPTCFG_IWLWIFI_DEVICE_TESTMODE
	iwl_tm_gnl_exit();
#endif
}
module_exit(iwl_drv_exit);

#ifdef CPTCFG_IWLWIFI_DEBUG
module_param_named(debug, iwlwifi_mod_params.debug_level, uint,
		   S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(debug, "debug output mask");
#endif

#if IS_ENABLED(CPTCFG_IWLXVT)
module_param_named(xvt_default_mode, iwlwifi_mod_params.xvt_default_mode,
		   bool, S_IRUGO);
MODULE_PARM_DESC(xvt_default_mode, "xVT is the default operation mode (default: false)");
#endif

module_param_named(swcrypto, iwlwifi_mod_params.sw_crypto, int, S_IRUGO);
MODULE_PARM_DESC(swcrypto, "using crypto in software (default 0 [hardware])");
module_param_named(11n_disable, iwlwifi_mod_params.disable_11n, uint, S_IRUGO);
MODULE_PARM_DESC(11n_disable,
	"disable 11n functionality, bitmap: 1: full, 2: disable agg TX, 4: disable agg RX, 8 enable agg TX");
module_param_named(amsdu_size, iwlwifi_mod_params.amsdu_size,
		   int, S_IRUGO);
MODULE_PARM_DESC(amsdu_size,
		 "amsdu size 0: 12K for multi Rx queue devices, 4K for other devices 1:4K 2:8K 3:12K (default 0)");
module_param_named(fw_restart, iwlwifi_mod_params.restart_fw, bool, S_IRUGO);
MODULE_PARM_DESC(fw_restart, "restart firmware in case of error (default true)");

module_param_named(antenna_coupling, iwlwifi_mod_params.ant_coupling,
		   int, S_IRUGO);
MODULE_PARM_DESC(antenna_coupling,
		 "specify antenna coupling in dB (default: 0 dB)");

module_param_named(nvm_file, iwlwifi_mod_params.nvm_file, charp, S_IRUGO);
MODULE_PARM_DESC(nvm_file, "NVM file name");

module_param_named(d0i3_disable, iwlwifi_mod_params.d0i3_disable,
		   bool, S_IRUGO);
#ifdef CPTCFG_IWLWIFI_D0I3_DEFAULT_DISABLE
MODULE_PARM_DESC(d0i3_disable, "disable d0i3 functionality (default: Y)");
#else
MODULE_PARM_DESC(d0i3_disable, "disable d0i3 functionality (default: N)");
#endif

module_param_named(lar_disable, iwlwifi_mod_params.lar_disable,
		   bool, S_IRUGO);
MODULE_PARM_DESC(lar_disable, "disable LAR functionality (default: N)");

module_param_named(uapsd_disable, iwlwifi_mod_params.uapsd_disable,
		   uint, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(uapsd_disable,
		 "disable U-APSD functionality bitmap 1: BSS 2: P2P Client (default: 3)");

/*
 * set bt_coex_active to true, uCode will do kill/defer
 * every time the priority line is asserted (BT is sending signals on the
 * priority line in the PCIx).
 * set bt_coex_active to false, uCode will ignore the BT activity and
 * perform the normal operation
 *
 * User might experience transmit issue on some platform due to WiFi/BT
 * co-exist problem. The possible behaviors are:
 *   Able to scan and finding all the available AP
 *   Not able to associate with any AP
 * On those platforms, WiFi communication can be restored by set
 * "bt_coex_active" module parameter to "false"
 *
 * default: bt_coex_active = true (BT_COEX_ENABLE)
 */
module_param_named(bt_coex_active, iwlwifi_mod_params.bt_coex_active,
		bool, S_IRUGO);
MODULE_PARM_DESC(bt_coex_active, "enable wifi/bt co-exist (default: enable)");

module_param_named(led_mode, iwlwifi_mod_params.led_mode, int, S_IRUGO);
MODULE_PARM_DESC(led_mode, "0=system default, "
		"1=On(RF On)/Off(RF Off), 2=blinking, 3=Off (default: 0)");

module_param_named(power_save, iwlwifi_mod_params.power_save,
		bool, S_IRUGO);
MODULE_PARM_DESC(power_save,
		 "enable WiFi power management (default: disable)");

module_param_named(power_level, iwlwifi_mod_params.power_level,
		int, S_IRUGO);
MODULE_PARM_DESC(power_level,
		 "default power save level (range from 1 - 5, default: 1)");

module_param_named(fw_monitor, iwlwifi_mod_params.fw_monitor, bool, S_IRUGO);
MODULE_PARM_DESC(fw_monitor,
		 "firmware monitor - to debug FW (default: false - needs lots of memory)");

module_param_named(d0i3_timeout, iwlwifi_mod_params.d0i3_entry_delay,
		   uint, S_IRUGO);
MODULE_PARM_DESC(d0i3_timeout, "Timeout to D0i3 entry when idle (ms)");

module_param_named(disable_11ac, iwlwifi_mod_params.disable_11ac, bool,
		   S_IRUGO);
MODULE_PARM_DESC(disable_11ac, "Disable VHT capabilities (default: false)");

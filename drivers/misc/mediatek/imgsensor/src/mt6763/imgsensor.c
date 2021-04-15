/*
 * Copyright (C) 2017 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See http://www.gnu.org/licenses/gpl-2.0.html for more details.
 */

#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/init.h>
#include <linux/types.h>

#ifdef CONFIG_MTK_SMI_EXT
#include "mmdvfs_mgr.h"
#endif

#ifdef CONFIG_OF
/* device tree */
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#endif

#ifdef CONFIG_COMPAT
#include <linux/compat.h>
#endif

#ifdef CONFIG_MTK_CCU
#include "ccu_inc.h"
#endif

#include "kd_camera_typedef.h"
#include "kd_imgsensor.h"
#include "kd_imgsensor_define.h"
#include "kd_camera_feature.h"
#include "kd_imgsensor_errcode.h"

#include "imgsensor_cfg_table.h"
#include "imgsensor_sensor_list.h"
#include "imgsensor_hw.h"
#include "imgsensor_i2c.h"
#include "imgsensor_proc.h"
#include "imgsensor_clk.h"
#include "imgsensor.h"

#ifdef CONFIG_MTK_SMI_EXT
static int current_mmsys_clk = MMSYS_CLK_MEDIUM;
#endif

/* Test Only!! Open this define for temperature meter UT */
/* Temperature workqueue */
//#define CONFIG_CAM_TEMPERATURE_WORKQUEUE
#ifdef CONFIG_CAM_TEMPERATURE_WORKQUEUE
	static void cam_temperature_report_wq_routine(struct work_struct *);
	struct delayed_work cam_temperature_wq;
#endif

#define FEATURE_CONTROL_MAX_DATA_SIZE 128000

struct platform_device *gpimgsensor_hw_platform_device;
struct device *gimgsensor_device = NULL;
/* 81 is used for V4L driver */
static struct cdev *gpimgsensor_cdev;
static struct class *gpimgsensor_class;

static DEFINE_MUTEX(gimgsensor_mutex);
DEFINE_MUTEX(pinctrl_mutex);

struct IMGSENSOR  gimgsensor;
struct IMGSENSOR *pgimgsensor = &gimgsensor;

/*******************************************************************************
* Profiling
********************************************************************************/
#define IMGSENSOR_PROF 1
#if IMGSENSOR_PROF
void IMGSENSOR_PROFILE_INIT(struct timeval *ptv)
{
	do_gettimeofday(ptv);
}

void IMGSENSOR_PROFILE(struct timeval *ptv, char *tag)
{
	struct timeval tv;
	unsigned long  time_interval;

	do_gettimeofday(&tv);
	time_interval = (tv.tv_sec - ptv->tv_sec) * 1000000 + (tv.tv_usec - ptv->tv_usec);

	PK_DBG("[%s]Profile = %lu us\n", tag, time_interval);
}

#else
void IMGSENSOR_PROFILE_INIT(struct timeval *ptv) {}
void IMGSENSOR_PROFILE(struct timeval *ptv, char *tag) {}
#endif

/*******************************************************************************
* sensor function adapter
********************************************************************************/
#define IMGSENSOR_FUNCTION_ENTRY()    /*PK_INFO("[%s]:E\n",__FUNCTION__)*/
#define IMGSENSOR_FUNCTION_EXIT()     /*PK_INFO("[%s]:X\n",__FUNCTION__)*/

struct IMGSENSOR_SENSOR *imgsensor_sensor_get_inst(enum IMGSENSOR_SENSOR_IDX idx)
{
	if (idx < IMGSENSOR_SENSOR_IDX_MIN_NUM || idx >= IMGSENSOR_SENSOR_IDX_MAX_NUM)
		return NULL;
	else
		return &pgimgsensor->sensor[idx];
}

static void imgsensor_mutex_init(struct IMGSENSOR_SENSOR_INST *psensor_inst)
{
	mutex_init(&psensor_inst->sensor_mutex);
}

static void imgsensor_mutex_lock(struct IMGSENSOR_SENSOR_INST *psensor_inst)
{
#ifdef IMGSENSOR_LEGACY_COMPAT
	if (psensor_inst->status.arch) {
		mutex_lock(&psensor_inst->sensor_mutex);
	} else {
		mutex_lock(&gimgsensor_mutex);
		imgsensor_i2c_set_device(&psensor_inst->i2c_cfg);
	}
#else
	mutex_lock(&psensor_inst->sensor_mutex);
#endif
}

static void imgsensor_mutex_unlock(struct IMGSENSOR_SENSOR_INST *psensor_inst)
{
#ifdef IMGSENSOR_LEGACY_COMPAT
	if (psensor_inst->status.arch)
		mutex_unlock(&psensor_inst->sensor_mutex);
	else
		mutex_unlock(&gimgsensor_mutex);
#else
	mutex_lock(&psensor_inst->sensor_mutex);
#endif
}

MINT32
imgsensor_sensor_open(struct IMGSENSOR_SENSOR *psensor)
{
	MINT32 ret = ERROR_NONE;
	struct IMGSENSOR_SENSOR_INST *psensor_inst = &psensor->inst;
	SENSOR_FUNCTION_STRUCT       *psensor_func =  psensor->pfunc;

#ifdef CONFIG_MTK_CCU
	struct ccu_sensor_info ccuSensorInfo;
	enum IMGSENSOR_SENSOR_IDX sensor_idx = psensor->inst.sensor_idx;
	struct i2c_client *pi2c_client = NULL;
#endif

	IMGSENSOR_FUNCTION_ENTRY();

	if (psensor_func &&
	    psensor_func->SensorOpen &&
	    psensor_inst) {

		/* turn on power */
		IMGSENSOR_PROFILE_INIT(&psensor_inst->profile_time);

		ret = imgsensor_hw_power(&pgimgsensor->hw,
						psensor,
						psensor_inst->psensor_name,
						IMGSENSOR_HW_POWER_STATUS_ON);
		if (ret != IMGSENSOR_RETURN_SUCCESS) {
			PK_PR_ERR("[%s] Power on fail", __func__);
			return -EIO;
		}
		/* wait for power stable */
		mDELAY(5);

		IMGSENSOR_PROFILE(&psensor_inst->profile_time, "kdCISModulePowerOn");

		imgsensor_mutex_lock(psensor_inst);

		psensor_func->psensor_inst = psensor_inst;

		ret = psensor_func->SensorOpen();
		if (ret != ERROR_NONE) {
			imgsensor_hw_power(&pgimgsensor->hw, psensor, psensor_inst->psensor_name, IMGSENSOR_HW_POWER_STATUS_OFF);
			PK_PR_ERR("[%s] Open fail\n", __func__);
		} else {
			psensor_inst->state = IMGSENSOR_STATE_OPEN;

#ifdef CONFIG_MTK_CCU
			ccuSensorInfo.slave_addr = (psensor_inst->i2c_cfg.pinst->msg->addr << 1);
			ccuSensorInfo.sensor_name_string = (char *)(psensor_inst->psensor_name);
			pi2c_client = psensor_inst->i2c_cfg.pinst->pi2c_client;
			if (pi2c_client)
				ccuSensorInfo.i2c_id =
					(((struct mt_i2c *) i2c_get_adapdata(
						pi2c_client->adapter))->id);
			else
				ccuSensorInfo.i2c_id = -1;
			ccu_set_sensor_info(sensor_idx, &ccuSensorInfo);
#endif
		}
		imgsensor_mutex_unlock(psensor_inst);

		IMGSENSOR_PROFILE(&psensor_inst->profile_time, "SensorOpen");
	}

	IMGSENSOR_FUNCTION_EXIT();

	return ret ? -EIO : ret;
}

MUINT32
imgsensor_sensor_get_info(
	struct IMGSENSOR_SENSOR *psensor,
	MUINT32 ScenarioId,
	MSDK_SENSOR_INFO_STRUCT *pSensorInfo,
	MSDK_SENSOR_CONFIG_STRUCT *pSensorConfigData)
{
	MUINT32 ret = ERROR_NONE;
	struct IMGSENSOR_SENSOR_INST *psensor_inst = &psensor->inst;
	SENSOR_FUNCTION_STRUCT       *psensor_func =  psensor->pfunc;

	IMGSENSOR_FUNCTION_ENTRY();

	if (psensor_func &&
	    psensor_func->SensorGetInfo &&
	    psensor_inst &&
	    pSensorInfo &&
	    pSensorConfigData) {

		imgsensor_mutex_lock(psensor_inst);

		psensor_func->psensor_inst = psensor_inst;

		ret = psensor_func->SensorGetInfo((MSDK_SCENARIO_ID_ENUM)(ScenarioId), pSensorInfo, pSensorConfigData);
		if (ret != ERROR_NONE)
			PK_PR_ERR("[%s] Get info fail\n", __func__);

		imgsensor_mutex_unlock(psensor_inst);
	}

	IMGSENSOR_FUNCTION_EXIT();

	return ret;
}

MUINT32
imgsensor_sensor_get_resolution(
	struct IMGSENSOR_SENSOR *psensor,
	MSDK_SENSOR_RESOLUTION_INFO_STRUCT *pSensorResolution)
{
	MUINT32 ret = ERROR_NONE;
	struct IMGSENSOR_SENSOR_INST *psensor_inst = &psensor->inst;
	SENSOR_FUNCTION_STRUCT       *psensor_func =  psensor->pfunc;

	IMGSENSOR_FUNCTION_ENTRY();

	if (psensor_func &&
	    psensor_func->SensorGetResolution &&
	    psensor_inst) {

		imgsensor_mutex_lock(psensor_inst);

		psensor_func->psensor_inst = psensor_inst;

		ret = psensor_func->SensorGetResolution(pSensorResolution);
		if (ret != ERROR_NONE)
			PK_PR_ERR("[%s] Get resolution fail\n", __func__);

		imgsensor_mutex_unlock(psensor_inst);
	}

	IMGSENSOR_FUNCTION_EXIT();

	return ret;
}

MUINT32
imgsensor_sensor_feature_control(
	struct IMGSENSOR_SENSOR *psensor,
	MSDK_SENSOR_FEATURE_ENUM FeatureId,
	MUINT8 *pFeaturePara,
	MUINT32 *pFeatureParaLen)
{
	MUINT32 ret = ERROR_NONE;
	struct IMGSENSOR_SENSOR_INST *psensor_inst = &psensor->inst;
	SENSOR_FUNCTION_STRUCT       *psensor_func =  psensor->pfunc;

	IMGSENSOR_FUNCTION_ENTRY();

	if (psensor_func &&
	    psensor_func->SensorFeatureControl &&
	    psensor_inst) {

		imgsensor_mutex_lock(psensor_inst);

		psensor_func->psensor_inst = psensor_inst;

		ret = psensor_func->SensorFeatureControl(FeatureId, pFeaturePara, pFeatureParaLen);
		if (ret != ERROR_NONE)
			PK_PR_ERR("[%s] Feature control fail, feature ID = %d\n", __func__, FeatureId);

		imgsensor_mutex_unlock(psensor_inst);
	}

	IMGSENSOR_FUNCTION_EXIT();

	return ret;
}

MUINT32
imgsensor_sensor_control(
	struct IMGSENSOR_SENSOR *psensor,
	MSDK_SCENARIO_ID_ENUM ScenarioId)
{
	MUINT32 ret = ERROR_NONE;
	struct IMGSENSOR_SENSOR_INST *psensor_inst = &psensor->inst;
	SENSOR_FUNCTION_STRUCT       *psensor_func =  psensor->pfunc;

	MSDK_SENSOR_EXPOSURE_WINDOW_STRUCT image_window;
	MSDK_SENSOR_CONFIG_STRUCT sensor_config_data;

	IMGSENSOR_FUNCTION_ENTRY();

	if (psensor_func &&
	    psensor_func->SensorControl &&
	    psensor_inst) {

		IMGSENSOR_PROFILE_INIT(&psensor_inst->profile_time);

		imgsensor_mutex_lock(psensor_inst);

		psensor_func->psensor_inst = psensor_inst;
		psensor_func->ScenarioId = ScenarioId;

		ret = psensor_func->SensorControl(ScenarioId, &image_window, &sensor_config_data);
		if (ret != ERROR_NONE)
			PK_PR_ERR("[%s] Sensor control fail, scenario ID = %d\n", __func__, ScenarioId);

		imgsensor_mutex_unlock(psensor_inst);

		IMGSENSOR_PROFILE(&psensor_inst->profile_time, "SensorControl");
	}

	IMGSENSOR_FUNCTION_EXIT();

	return ret;
}

MINT32
imgsensor_sensor_close(struct IMGSENSOR_SENSOR *psensor)
{
	MINT32 ret = ERROR_NONE;
	struct IMGSENSOR_SENSOR_INST *psensor_inst = &psensor->inst;
	SENSOR_FUNCTION_STRUCT       *psensor_func =  psensor->pfunc;

	IMGSENSOR_FUNCTION_ENTRY();

	if (psensor_func &&
	    psensor_func->SensorClose &&
	    psensor_inst) {

		imgsensor_mutex_lock(psensor_inst);

		psensor_func->psensor_inst = psensor_inst;

		ret = psensor_func->SensorClose();
		if (ret != ERROR_NONE) {
			PK_PR_ERR("[%s] Sensor close fail\n", __func__);
		} else {
			imgsensor_hw_power(&pgimgsensor->hw, psensor, psensor_inst->psensor_name, IMGSENSOR_HW_POWER_STATUS_OFF);
			psensor_inst->state = IMGSENSOR_STATE_CLOSE;
		}

		imgsensor_mutex_unlock(psensor_inst);
	}

	IMGSENSOR_FUNCTION_EXIT();

	return ret ? -EIO : ret;
}

/*******************************************************************************
* imgsensor_check_is_alive
********************************************************************************/
inline static int imgsensor_check_is_alive(struct IMGSENSOR_SENSOR *psensor)
{
	UINT32 err = 0;

	MUINT32 sensorID = 0;
	MUINT32 retLen = sizeof(MUINT32);

	struct IMGSENSOR_SENSOR_INST  *psensor_inst = &psensor->inst;

	IMGSENSOR_PROFILE_INIT(&psensor_inst->profile_time);

	imgsensor_hw_power(&pgimgsensor->hw, psensor, psensor_inst->psensor_name, IMGSENSOR_HW_POWER_STATUS_ON);

	imgsensor_sensor_feature_control(psensor, SENSOR_FEATURE_CHECK_SENSOR_ID, (MUINT8 *)&sensorID, &retLen);

	if (sensorID == 0 || sensorID == 0xFFFFFFFF) {	  /* not implement this feature ID */
		PK_DBG("Fail to get sensor ID %x\n", sensorID);
		err = ERROR_SENSOR_CONNECT_FAIL;
	} else {
		PK_DBG(" Sensor found ID = 0x%x\n", sensorID);
		snprintf(mtk_ccm_name,sizeof(mtk_ccm_name),"%s CAM[%d]:%s;",mtk_ccm_name,psensor->inst.sensor_idx,psensor_inst->psensor_name);
		err = ERROR_NONE;
	}

	if (ERROR_NONE != err)
	{
		PK_DBG("ERROR:imgsensor_check_is_alive(), No imgsensor alive\n");
	}

	imgsensor_hw_power(&pgimgsensor->hw, psensor, psensor_inst->psensor_name, IMGSENSOR_HW_POWER_STATUS_OFF);

	IMGSENSOR_PROFILE(&psensor_inst->profile_time, "CheckIsAlive");

	return err ? -EIO:err;
}

/*******************************************************************************
* imgsensor_set_driver
********************************************************************************/
int imgsensor_set_driver(struct IMGSENSOR_SENSOR *psensor)
{
	u32 drv_idx = 0;
	int ret = -EIO;

	struct IMGSENSOR_SENSOR_INST    *psensor_inst = &psensor->inst;
	struct IMGSENSOR_INIT_FUNC_LIST *pSensorList  = kdSensorList;

	imgsensor_mutex_init(psensor_inst);
	imgsensor_i2c_init(&psensor_inst->i2c_cfg, imgsensor_custom_config[psensor->inst.sensor_idx].i2c_dev);
	imgsensor_i2c_filter_msg(&psensor_inst->i2c_cfg, true);

	while(drv_idx < MAX_NUM_OF_SUPPORT_SENSOR) {
		if (pSensorList[drv_idx].init) {
		    pSensorList[drv_idx].init(&psensor->pfunc);

			if(psensor->pfunc) {
				/* get sensor name */
				psensor_inst->psensor_name = (char *)pSensorList[drv_idx].name;
#ifdef IMGSENSOR_LEGACY_COMPAT
				psensor_inst->status.arch = psensor->pfunc->arch;
#endif
				if (!imgsensor_check_is_alive(psensor)) {
					PK_INFO("[imgsensor_set_driver] :[%d][%d][%s]\n",
								psensor->inst.sensor_idx,
								drv_idx,
								psensor_inst->psensor_name);

					ret = drv_idx;
					break;
				}
			} else {
				PK_PR_ERR("ERROR:NULL g_pInvokeSensorFunc[%d][%d]\n",
							psensor->inst.sensor_idx,
							drv_idx);
			}
		} else {
			PK_PR_ERR("ERROR:NULL sensor list[%d]\n", drv_idx);
		}

		drv_idx++;
	}

	imgsensor_i2c_filter_msg(&psensor_inst->i2c_cfg, false);

	return ret;
}

/*******************************************************************************
* adopt_CAMERA_HW_GetInfo
********************************************************************************/
inline static int adopt_CAMERA_HW_GetInfo(void *pBuf)
{
	IMGSENSOR_GET_CONFIG_INFO_STRUCT *pSensorGetInfo;
	struct IMGSENSOR_SENSOR *psensor;

	MSDK_SENSOR_INFO_STRUCT *pInfo;
	MSDK_SENSOR_CONFIG_STRUCT *pConfig;
	MUINT32 *pScenarioId;

	pSensorGetInfo = (IMGSENSOR_GET_CONFIG_INFO_STRUCT *)pBuf;
	if (pSensorGetInfo == NULL ||
	     pSensorGetInfo->pInfo == NULL ||
	     pSensorGetInfo->pConfig == NULL) {
		PK_DBG("[CAMERA_HW] NULL arg.\n");
		return -EFAULT;
	}

	psensor = imgsensor_sensor_get_inst(pSensorGetInfo->SensorId);
	if (psensor == NULL) {
		PK_DBG("[CAMERA_HW] NULL psensor.\n");
		return -EFAULT;
	}

	pInfo = NULL;
	pConfig =  NULL;
	pScenarioId =  &(pSensorGetInfo->ScenarioId);

	pInfo = kmalloc(sizeof(MSDK_SENSOR_INFO_STRUCT), GFP_KERNEL);
	pConfig = kmalloc(sizeof(MSDK_SENSOR_CONFIG_STRUCT), GFP_KERNEL);

	if (pInfo == NULL || pConfig == NULL) {
		kfree(pInfo);
		kfree(pConfig);

		PK_PR_ERR(" ioctl allocate mem failed\n");
		return -ENOMEM;
	}

	memset(pInfo, 0, sizeof(MSDK_SENSOR_INFO_STRUCT));
	memset(pConfig, 0, sizeof(MSDK_SENSOR_CONFIG_STRUCT));

	imgsensor_sensor_get_info(psensor, *pScenarioId, pInfo, pConfig);

	/* SenorInfo */
	if (copy_to_user((void __user *)(pSensorGetInfo->pInfo), (void *)pInfo, sizeof(MSDK_SENSOR_INFO_STRUCT))) {
		PK_DBG("[CAMERA_HW][info] ioctl copy to user failed\n");

		if (pInfo != NULL)
			kfree(pInfo);
		if (pConfig != NULL)
			kfree(pConfig);

		pInfo = NULL;
		pConfig = NULL;

		return -EFAULT;
	}

	/* SensorConfig */
	if (copy_to_user((void __user *) (pSensorGetInfo->pConfig), (void *)pConfig, sizeof(MSDK_SENSOR_CONFIG_STRUCT))) {
		PK_DBG("[CAMERA_HW][config] ioctl copy to user failed\n");

		if (pInfo != NULL)
			kfree(pInfo);
		if (pConfig != NULL)
			kfree(pConfig);

		pInfo = NULL;
		pConfig = NULL;

		return -EFAULT;
	}

	kfree(pInfo);
	kfree(pConfig);
	pInfo = NULL;
	pConfig = NULL;

	return 0;
}   /* adopt_CAMERA_HW_GetInfo() */

MUINT32 Get_Camera_Temperature(CAMERA_DUAL_CAMERA_SENSOR_ENUM senDevId, MUINT8 *valid, MUINT32 *temp)
{
	MUINT32 ret = IMGSENSOR_RETURN_SUCCESS;
	MUINT32 FeatureParaLen = 0;
	struct IMGSENSOR_SENSOR      *psensor = imgsensor_sensor_get_inst(IMGSENSOR_SENSOR_IDX_MAP(senDevId));
	struct IMGSENSOR_SENSOR_INST *psensor_inst;

	if (valid == NULL || temp == NULL || psensor == NULL)
		return IMGSENSOR_RETURN_ERROR;

	*valid = SENSOR_TEMPERATURE_NOT_SUPPORT_THERMAL | SENSOR_TEMPERATURE_NOT_POWER_ON;
	*temp  = 0;

	psensor_inst = &psensor->inst;

	FeatureParaLen = sizeof(MUINT32);

	/* Sensor is not in close state, where in close state the temperature is not valid */
	if (psensor_inst->state != IMGSENSOR_STATE_CLOSE) {
		ret = imgsensor_sensor_feature_control(psensor, SENSOR_FEATURE_GET_TEMPERATURE_VALUE, (MUINT8*)temp, (MUINT32*)&FeatureParaLen);
		PK_DBG("senDevId(%d), temperature(%d)\n", senDevId, *temp);

		*valid &= ~SENSOR_TEMPERATURE_NOT_POWER_ON;

		if (*temp != 0) {
			*valid |=  SENSOR_TEMPERATURE_VALID;
			*valid &= ~SENSOR_TEMPERATURE_NOT_SUPPORT_THERMAL;
		}
	}

	return ret;
}
EXPORT_SYMBOL(Get_Camera_Temperature);

#ifdef CONFIG_CAM_TEMPERATURE_WORKQUEUE
static void cam_temperature_report_wq_routine(struct work_struct *data)
{
	MUINT8 valid[3] = {0, 0, 0};
	MUINT32 temp[3] = {0, 0, 0};
	MUINT32 ret = 0;

	PK_DBG("Temperature Meter Report.\n");

	/* Main cam */
	ret = Get_Camera_Temperature(DUAL_CAMERA_MAIN_SENSOR, &valid[0], &temp[0]);
	PK_INFO("senDevId(%d), valid(%d), temperature(%d)\n", \
		DUAL_CAMERA_MAIN_SENSOR, valid[0], temp[0]);
	if(ERROR_NONE != ret)
		PK_PR_ERR("Get Main cam temperature error(%d)!\n", ret);

	/* Sub cam */
	ret = Get_Camera_Temperature(DUAL_CAMERA_SUB_SENSOR, &valid[0], &temp[0]);
	PK_INFO("senDevId(%d), valid(%d), temperature(%d)\n", \
		DUAL_CAMERA_SUB_SENSOR, valid[1], temp[1]);
	if(ERROR_NONE != ret)
		PK_PR_ERR("Get Sub cam temperature error(%d)!\n", ret);

	/* Main2 cam */
	ret = Get_Camera_Temperature(DUAL_CAMERA_MAIN_2_SENSOR, &valid[0], &temp[0]);
	PK_INFO("senDevId(%d), valid(%d), temperature(%d)\n", \
		DUAL_CAMERA_MAIN_2_SENSOR, valid[2], temp[2]);
	if(ERROR_NONE != ret)
		PK_PR_ERR("Get Main2 cam temperature error(%d)!\n", ret);

	ret = Get_Camera_Temperature(DUAL_CAMERA_SUB_2_SENSOR, &valid[3], &temp[3]);
	PK_INFO("senDevId(%d), valid(%d), temperature(%d)\n",
				DUAL_CAMERA_SUB_2_SENSOR, valid[3], temp[3]);

	if (ret != ERROR_NONE)
		PK_DBG("Get Sub2 cam temperature error(%d)!\n", ret);
	schedule_delayed_work(&cam_temperature_wq, HZ);

}
#endif

inline static int adopt_CAMERA_HW_GetInfo2(void *pBuf)
{
	int ret = 0;
	IMAGESENSOR_GETINFO_STRUCT *pSensorGetInfo;
	struct IMGSENSOR_SENSOR    *psensor;

	ACDK_SENSOR_INFO2_STRUCT *pSensorInfo = NULL;
	MSDK_SENSOR_INFO_STRUCT *pInfo = NULL;
	MSDK_SENSOR_CONFIG_STRUCT  *pConfig = NULL;
	MSDK_SENSOR_INFO_STRUCT *pInfo1 = NULL;
	MSDK_SENSOR_CONFIG_STRUCT  *pConfig1 = NULL;
	MSDK_SENSOR_INFO_STRUCT *pInfo2 = NULL;
	MSDK_SENSOR_CONFIG_STRUCT  *pConfig2= NULL;
	MSDK_SENSOR_INFO_STRUCT *pInfo3 = NULL;
	MSDK_SENSOR_CONFIG_STRUCT  *pConfig3 = NULL;
	MSDK_SENSOR_INFO_STRUCT *pInfo4 = NULL;
	MSDK_SENSOR_CONFIG_STRUCT  *pConfig4 = NULL;
	MSDK_SENSOR_RESOLUTION_INFO_STRUCT  *psensorResolution = NULL;

	pSensorGetInfo = (IMAGESENSOR_GETINFO_STRUCT *)pBuf;
	if (pSensorGetInfo == NULL ||
	    pSensorGetInfo->pInfo == NULL ||
	    pSensorGetInfo->pSensorResolution == NULL) {
		PK_DBG("[adopt_CAMERA_HW_GetInfo2] NULL arg.\n");
		return -EFAULT;
	}

	psensor = imgsensor_sensor_get_inst(pSensorGetInfo->SensorId);
	if (psensor == NULL) {
		PK_DBG("[adopt_CAMERA_HW_GetInfo2] NULL psensor.\n");
		return -EFAULT;
	}

	PK_DBG("[adopt_CAMERA_HW_GetInfo2]Entry%d\n", pSensorGetInfo->SensorId);

	pInfo =    kmalloc(sizeof(MSDK_SENSOR_INFO_STRUCT), GFP_KERNEL);
	pConfig =  kmalloc(sizeof(MSDK_SENSOR_CONFIG_STRUCT), GFP_KERNEL);
	pInfo1 =   kmalloc(sizeof(MSDK_SENSOR_INFO_STRUCT), GFP_KERNEL);
	pConfig1 = kmalloc(sizeof(MSDK_SENSOR_CONFIG_STRUCT), GFP_KERNEL);
	pInfo2 =   kmalloc(sizeof(MSDK_SENSOR_INFO_STRUCT), GFP_KERNEL);
	pConfig2 = kmalloc(sizeof(MSDK_SENSOR_CONFIG_STRUCT), GFP_KERNEL);
	pInfo3 =   kmalloc(sizeof(MSDK_SENSOR_INFO_STRUCT), GFP_KERNEL);
	pConfig3 = kmalloc(sizeof(MSDK_SENSOR_CONFIG_STRUCT), GFP_KERNEL);
	pInfo4 =   kmalloc(sizeof(MSDK_SENSOR_INFO_STRUCT), GFP_KERNEL);
	pConfig4 = kmalloc(sizeof(MSDK_SENSOR_CONFIG_STRUCT), GFP_KERNEL);
	psensorResolution = kmalloc(sizeof(MSDK_SENSOR_RESOLUTION_INFO_STRUCT), GFP_KERNEL);

	pSensorInfo = kmalloc(sizeof(ACDK_SENSOR_INFO2_STRUCT), GFP_KERNEL);

	if (pInfo    == NULL ||
	    pConfig  == NULL ||
	    pConfig1 == NULL ||
	    pConfig2 == NULL ||
	    pConfig3 == NULL ||
	    pConfig4 == NULL ||
	    pSensorInfo == NULL ||
	    psensorResolution == NULL) {
		PK_PR_ERR(" ioctl allocate mem failed\n");
		ret = -EFAULT;
		goto IMGSENSOR_GET_INFO_RETURN;
	}

	imgsensor_sensor_get_info(psensor, MSDK_SCENARIO_ID_CAMERA_PREVIEW, pInfo, pConfig);
	imgsensor_sensor_get_info(psensor, MSDK_SCENARIO_ID_CAMERA_CAPTURE_JPEG, pInfo1, pConfig1);
	imgsensor_sensor_get_info(psensor, MSDK_SCENARIO_ID_VIDEO_PREVIEW, pInfo2, pConfig2);
	imgsensor_sensor_get_info(psensor, MSDK_SCENARIO_ID_HIGH_SPEED_VIDEO, pInfo3, pConfig3);
	imgsensor_sensor_get_info(psensor, MSDK_SCENARIO_ID_SLIM_VIDEO, pInfo4, pConfig4);

	/* Basic information */
	pSensorInfo->SensorPreviewResolutionX                 = pInfo->SensorPreviewResolutionX;
	pSensorInfo->SensorPreviewResolutionY                 = pInfo->SensorPreviewResolutionY;
	pSensorInfo->SensorFullResolutionX                    = pInfo->SensorFullResolutionX;
	pSensorInfo->SensorFullResolutionY                    = pInfo->SensorFullResolutionY;
	pSensorInfo->SensorClockFreq                          = pInfo->SensorClockFreq;
	pSensorInfo->SensorCameraPreviewFrameRate             = pInfo->SensorCameraPreviewFrameRate;
	pSensorInfo->SensorVideoFrameRate                     = pInfo->SensorVideoFrameRate;
	pSensorInfo->SensorStillCaptureFrameRate              = pInfo->SensorStillCaptureFrameRate;
	pSensorInfo->SensorWebCamCaptureFrameRate             = pInfo->SensorWebCamCaptureFrameRate;
	pSensorInfo->SensorClockPolarity                      = pInfo->SensorClockPolarity;
	pSensorInfo->SensorClockFallingPolarity               = pInfo->SensorClockFallingPolarity;
	pSensorInfo->SensorClockRisingCount                   = pInfo->SensorClockRisingCount;
	pSensorInfo->SensorClockFallingCount                  = pInfo->SensorClockFallingCount;
	pSensorInfo->SensorClockDividCount                    = pInfo->SensorClockDividCount;
	pSensorInfo->SensorPixelClockCount                    = pInfo->SensorPixelClockCount;
	pSensorInfo->SensorDataLatchCount                     = pInfo->SensorDataLatchCount;
	pSensorInfo->SensorHsyncPolarity                      = pInfo->SensorHsyncPolarity;
	pSensorInfo->SensorVsyncPolarity                      = pInfo->SensorVsyncPolarity;
	pSensorInfo->SensorInterruptDelayLines                = pInfo->SensorInterruptDelayLines;
	pSensorInfo->SensorResetActiveHigh                    = pInfo->SensorResetActiveHigh;
	pSensorInfo->SensorResetDelayCount                    = pInfo->SensorResetDelayCount;
	pSensorInfo->SensroInterfaceType                      = pInfo->SensroInterfaceType;
	pSensorInfo->SensorOutputDataFormat                   = pInfo->SensorOutputDataFormat;
	pSensorInfo->SensorMIPILaneNumber                     = pInfo->SensorMIPILaneNumber;
	pSensorInfo->CaptureDelayFrame                        = pInfo->CaptureDelayFrame;
	pSensorInfo->PreviewDelayFrame                        = pInfo->PreviewDelayFrame;
	pSensorInfo->VideoDelayFrame                          = pInfo->VideoDelayFrame;
	pSensorInfo->HighSpeedVideoDelayFrame                 = pInfo->HighSpeedVideoDelayFrame;
	pSensorInfo->SlimVideoDelayFrame                      = pInfo->SlimVideoDelayFrame;
	pSensorInfo->Custom1DelayFrame                        = pInfo->Custom1DelayFrame;
	pSensorInfo->Custom2DelayFrame                        = pInfo->Custom2DelayFrame;
	pSensorInfo->Custom3DelayFrame                        = pInfo->Custom3DelayFrame;
	pSensorInfo->Custom4DelayFrame                        = pInfo->Custom4DelayFrame;
	pSensorInfo->Custom5DelayFrame                        = pInfo->Custom5DelayFrame;
	pSensorInfo->YUVAwbDelayFrame                         = pInfo->YUVAwbDelayFrame;
	pSensorInfo->YUVEffectDelayFrame                      = pInfo->YUVEffectDelayFrame;
	pSensorInfo->SensorGrabStartX_PRV                     = pInfo->SensorGrabStartX;
	pSensorInfo->SensorGrabStartY_PRV                     = pInfo->SensorGrabStartY;
	pSensorInfo->SensorGrabStartX_CAP                     = pInfo1->SensorGrabStartX;
	pSensorInfo->SensorGrabStartY_CAP                     = pInfo1->SensorGrabStartY;
	pSensorInfo->SensorGrabStartX_VD                      = pInfo2->SensorGrabStartX;
	pSensorInfo->SensorGrabStartY_VD                      = pInfo2->SensorGrabStartY;
	pSensorInfo->SensorGrabStartX_VD1                     = pInfo3->SensorGrabStartX;
	pSensorInfo->SensorGrabStartY_VD1                     = pInfo3->SensorGrabStartY;
	pSensorInfo->SensorGrabStartX_VD2                     = pInfo4->SensorGrabStartX;
	pSensorInfo->SensorGrabStartY_VD2                     = pInfo4->SensorGrabStartY;
	pSensorInfo->SensorDrivingCurrent                     = pInfo->SensorDrivingCurrent;
	pSensorInfo->SensorMasterClockSwitch                  = pInfo->SensorMasterClockSwitch;
	pSensorInfo->AEShutDelayFrame                         = pInfo->AEShutDelayFrame;
	pSensorInfo->AESensorGainDelayFrame                   = pInfo->AESensorGainDelayFrame;
	pSensorInfo->AEISPGainDelayFrame                      = pInfo->AEISPGainDelayFrame;
	pSensorInfo->FrameTimeDelayFrame                      = pInfo->FrameTimeDelayFrame;
	pSensorInfo->MIPIDataLowPwr2HighSpeedTermDelayCount   = pInfo->MIPIDataLowPwr2HighSpeedTermDelayCount;
	pSensorInfo->MIPIDataLowPwr2HighSpeedSettleDelayCount = pInfo->MIPIDataLowPwr2HighSpeedSettleDelayCount;
	pSensorInfo->MIPIDataLowPwr2HSSettleDelayM0           = pInfo->MIPIDataLowPwr2HighSpeedSettleDelayCount;
	pSensorInfo->MIPIDataLowPwr2HSSettleDelayM1           = pInfo1->MIPIDataLowPwr2HighSpeedSettleDelayCount;
	pSensorInfo->MIPIDataLowPwr2HSSettleDelayM2           = pInfo2->MIPIDataLowPwr2HighSpeedSettleDelayCount;
	pSensorInfo->MIPIDataLowPwr2HSSettleDelayM3           = pInfo3->MIPIDataLowPwr2HighSpeedSettleDelayCount;
	pSensorInfo->MIPIDataLowPwr2HSSettleDelayM4           = pInfo4->MIPIDataLowPwr2HighSpeedSettleDelayCount;
	pSensorInfo->MIPICLKLowPwr2HighSpeedTermDelayCount    = pInfo->MIPICLKLowPwr2HighSpeedTermDelayCount;
	pSensorInfo->SensorWidthSampling                      = pInfo->SensorWidthSampling;
	pSensorInfo->SensorHightSampling                      = pInfo->SensorHightSampling;
	pSensorInfo->SensorPacketECCOrder                     = pInfo->SensorPacketECCOrder;
	pSensorInfo->MIPIsensorType                           = pInfo->MIPIsensorType;
	pSensorInfo->IHDR_LE_FirstLine                        = pInfo->IHDR_LE_FirstLine;
	pSensorInfo->IHDR_Support                             = pInfo->IHDR_Support;
	pSensorInfo->ZHDR_Mode                                = pInfo->ZHDR_Mode;
	pSensorInfo->TEMPERATURE_SUPPORT                      = pInfo->TEMPERATURE_SUPPORT;
	pSensorInfo->SensorModeNum                            = pInfo->SensorModeNum;
	pSensorInfo->SettleDelayMode                          = pInfo->SettleDelayMode;
	pSensorInfo->PDAF_Support                             = pInfo->PDAF_Support;
	pSensorInfo->HDR_Support                              = pInfo->HDR_Support;
	pSensorInfo->IMGSENSOR_DPCM_TYPE_PRE                  = pInfo->DPCM_INFO;
	pSensorInfo->IMGSENSOR_DPCM_TYPE_CAP                  = pInfo1->DPCM_INFO;
	pSensorInfo->IMGSENSOR_DPCM_TYPE_VD                   = pInfo2->DPCM_INFO;
	pSensorInfo->IMGSENSOR_DPCM_TYPE_VD1                  = pInfo3->DPCM_INFO;
	pSensorInfo->IMGSENSOR_DPCM_TYPE_VD2                  = pInfo4->DPCM_INFO;
	/*Per-Frame conrol suppport or not */
	pSensorInfo->PerFrameCTL_Support                      = pInfo->PerFrameCTL_Support;
	/*SCAM number*/
	pSensorInfo->SCAM_DataNumber                          = pInfo->SCAM_DataNumber;
	pSensorInfo->SCAM_DDR_En                              = pInfo->SCAM_DDR_En;
	pSensorInfo->SCAM_CLK_INV                             = pInfo->SCAM_CLK_INV;
	pSensorInfo->SCAM_DEFAULT_DELAY                      = pInfo->SCAM_DEFAULT_DELAY;
	pSensorInfo->SCAM_CRC_En                             = pInfo->SCAM_CRC_En;
	pSensorInfo->SCAM_SOF_src                            = pInfo->SCAM_SOF_src;
	pSensorInfo->SCAM_Timout_Cali                        = pInfo->SCAM_Timout_Cali;
	/*Deskew*/
	pSensorInfo->SensorMIPIDeskew                       = pInfo->SensorMIPIDeskew;

	imgsensor_sensor_get_info(psensor, MSDK_SCENARIO_ID_CUSTOM1, pInfo, pConfig);
	imgsensor_sensor_get_info(psensor, MSDK_SCENARIO_ID_CUSTOM2, pInfo1, pConfig1);
	imgsensor_sensor_get_info(psensor, MSDK_SCENARIO_ID_CUSTOM3, pInfo2, pConfig2);
	imgsensor_sensor_get_info(psensor, MSDK_SCENARIO_ID_CUSTOM4, pInfo3, pConfig3);
	imgsensor_sensor_get_info(psensor, MSDK_SCENARIO_ID_CUSTOM5, pInfo4, pConfig4);

	/* To set sensor information */
	pSensorInfo->SensorGrabStartX_CST1                    = pInfo->SensorGrabStartX;
	pSensorInfo->SensorGrabStartY_CST1                    = pInfo->SensorGrabStartY;
	pSensorInfo->SensorGrabStartX_CST2                    = pInfo1->SensorGrabStartX;
	pSensorInfo->SensorGrabStartY_CST2                    = pInfo1->SensorGrabStartY;
	pSensorInfo->SensorGrabStartX_CST3                    = pInfo2->SensorGrabStartX;
	pSensorInfo->SensorGrabStartY_CST3                    = pInfo2->SensorGrabStartY;
	pSensorInfo->SensorGrabStartX_CST4                    = pInfo3->SensorGrabStartX;
	pSensorInfo->SensorGrabStartY_CST4                    = pInfo3->SensorGrabStartY;
	pSensorInfo->SensorGrabStartX_CST5                    = pInfo4->SensorGrabStartX;
	pSensorInfo->SensorGrabStartY_CST5                    = pInfo4->SensorGrabStartY;

	if (copy_to_user((void __user *)(pSensorGetInfo->pInfo), (void *)(pSensorInfo), sizeof(ACDK_SENSOR_INFO2_STRUCT))) {
		PK_DBG("[CAMERA_HW][info] ioctl copy to user failed\n");

		ret = -EFAULT;
		goto IMGSENSOR_GET_INFO_RETURN;
	}

	/* Step2 : Get Resolution */
	imgsensor_sensor_get_resolution(psensor, psensorResolution);

	PK_DBG("[CAMERA_HW][Pre]w=0x%x, h = 0x%x\n", psensorResolution->SensorPreviewWidth, psensorResolution->SensorPreviewHeight);
	PK_DBG("[CAMERA_HW][Full]w=0x%x, h = 0x%x\n", psensorResolution->SensorFullWidth, psensorResolution->SensorFullHeight);
	PK_DBG("[CAMERA_HW][VD]w=0x%x, h = 0x%x\n", psensorResolution->SensorVideoWidth, psensorResolution->SensorVideoHeight);

	/* Add info to proc: camera_info */
	snprintf(mtk_ccm_name, sizeof(mtk_ccm_name), "%s \n\nCAM_Info[%d]:%s;", mtk_ccm_name, pSensorGetInfo->SensorId, psensor->inst.psensor_name);
	snprintf(mtk_ccm_name,sizeof(mtk_ccm_name),"%s \nPre: TgGrab_w,h,x_,y=%5d,%5d,%3d,%3d, delay_frm=%2d",mtk_ccm_name,psensorResolution->SensorPreviewWidth,psensorResolution->SensorPreviewHeight, pSensorInfo->SensorGrabStartX_PRV, pSensorInfo->SensorGrabStartY_PRV, pSensorInfo->PreviewDelayFrame);
	snprintf(mtk_ccm_name,sizeof(mtk_ccm_name),"%s \nCap: TgGrab_w,h,x_,y=%5d,%5d,%3d,%3d, delay_frm=%2d",mtk_ccm_name,psensorResolution->SensorFullWidth,psensorResolution->SensorFullHeight, pSensorInfo->SensorGrabStartX_CAP, pSensorInfo->SensorGrabStartY_CAP, pSensorInfo->CaptureDelayFrame);
	snprintf(mtk_ccm_name,sizeof(mtk_ccm_name),"%s \nVid: TgGrab_w,h,x_,y=%5d,%5d,%3d,%3d, delay_frm=%2d",mtk_ccm_name,psensorResolution->SensorVideoWidth,psensorResolution->SensorVideoHeight, pSensorInfo->SensorGrabStartX_VD, pSensorInfo->SensorGrabStartY_VD, pSensorInfo->VideoDelayFrame);
	snprintf(mtk_ccm_name,sizeof(mtk_ccm_name),"%s \nHSV: TgGrab_w,h,x_,y=%5d,%5d,%3d,%3d, delay_frm=%2d",mtk_ccm_name,psensorResolution->SensorHighSpeedVideoWidth,psensorResolution->SensorHighSpeedVideoHeight, pSensorInfo->SensorGrabStartX_VD1, pSensorInfo->SensorGrabStartY_VD1, pSensorInfo->HighSpeedVideoDelayFrame);
	snprintf(mtk_ccm_name,sizeof(mtk_ccm_name),"%s \nSLV: TgGrab_w,h,x_,y=%5d,%5d,%3d,%3d, delay_frm=%2d",mtk_ccm_name,psensorResolution->SensorSlimVideoWidth,psensorResolution->SensorSlimVideoHeight, pSensorInfo->SensorGrabStartX_VD2, pSensorInfo->SensorGrabStartY_VD2, pSensorInfo->SlimVideoDelayFrame);
	snprintf(mtk_ccm_name,sizeof(mtk_ccm_name),"%s \nSeninf_Type(0:parallel,1:mipi,2:serial)=%d, output_format(0:B,1:Gb,2:Gr,3:R)=%2d",mtk_ccm_name, pSensorInfo->SensroInterfaceType, pSensorInfo->SensorOutputDataFormat);
	snprintf(mtk_ccm_name,sizeof(mtk_ccm_name),"%s \nDriving_Current(0:2mA,1:4mA,2:6mA,3:8mA)=%d, mclk_freq=%2d, mipi_lane=%d",mtk_ccm_name, pSensorInfo->SensorDrivingCurrent, pSensorInfo->SensorClockFreq, pSensorInfo->SensorMIPILaneNumber + 1);
	snprintf(mtk_ccm_name,sizeof(mtk_ccm_name),"%s \nPDAF_Support(0:No PD,1:PD RAW,2:VC(Full),3:VC(Bin),4:Dual Raw,5:Dual VC=%2d",mtk_ccm_name, pSensorInfo->PDAF_Support);
	snprintf(mtk_ccm_name,sizeof(mtk_ccm_name),"%s \nHDR_Support(0:NO HDR,1: iHDR,2:mvHDR,3:zHDR)=%2d",mtk_ccm_name, pSensorInfo->HDR_Support);

	/* Resolution */
	if (copy_to_user((void __user *) (pSensorGetInfo->pSensorResolution) , (void *)psensorResolution , sizeof(MSDK_SENSOR_RESOLUTION_INFO_STRUCT))) {
		PK_DBG("[CAMERA_HW][Resolution] ioctl copy to user failed\n");

		ret = -EFAULT;
		goto IMGSENSOR_GET_INFO_RETURN;
	}

IMGSENSOR_GET_INFO_RETURN:
	if(pInfo != NULL) kfree(pInfo);
	if(pInfo1 != NULL) kfree(pInfo1);
	if(pInfo2 != NULL) kfree(pInfo2);
	if(pInfo3 != NULL) kfree(pInfo3);
	if(pInfo4 != NULL) kfree(pInfo4);
	if(pConfig != NULL) kfree(pConfig);
	if(pConfig1 != NULL) kfree(pConfig1);
	if(pConfig2 != NULL) kfree(pConfig2);
	if(pConfig3 != NULL) kfree(pConfig3);
	if(pConfig4 != NULL) kfree(pConfig4);
	if(psensorResolution != NULL) kfree(psensorResolution);

	pInfo    = NULL;
	pInfo1	 = NULL;
	pInfo2	 = NULL;
	pInfo3	 = NULL;
	pInfo4	 = NULL;

	pConfig  = NULL;
	pConfig1 = NULL;
	pConfig2 = NULL;
	pConfig3 = NULL;
	pConfig4 = NULL;
	psensorResolution = NULL;

	kfree(pSensorInfo);
	pSensorInfo = NULL;

	return ret;
}   /* adopt_CAMERA_HW_GetInfo() */

/*******************************************************************************
* adopt_CAMERA_HW_Control
********************************************************************************/
inline static int adopt_CAMERA_HW_Control(void *pBuf)
{
	int ret = 0;
	ACDK_SENSOR_CONTROL_STRUCT *pSensorCtrl;
	struct IMGSENSOR_SENSOR *psensor;

	pSensorCtrl = (ACDK_SENSOR_CONTROL_STRUCT *)pBuf;
	if (pSensorCtrl == NULL) {
		PK_DBG("[adopt_CAMERA_HW_Control] NULL arg.\n");
		return -EFAULT;
	}

	psensor = imgsensor_sensor_get_inst(pSensorCtrl->InvokeCamera);
	if (psensor == NULL) {
		PK_DBG("[adopt_CAMERA_HW_Control] NULL psensor.\n");
		return -EFAULT;
	}

	ret = imgsensor_sensor_control(psensor, pSensorCtrl->ScenarioId);

	return ret;
} /* adopt_CAMERA_HW_Control */

/*******************************************************************************
* adopt_CAMERA_HW_FeatureControl
********************************************************************************/
inline static int  adopt_CAMERA_HW_FeatureControl(void *pBuf)
{
	ACDK_SENSOR_FEATURECONTROL_STRUCT *pFeatureCtrl;
	struct IMGSENSOR_SENSOR *psensor;
	unsigned int FeatureParaLen = 0;
	void *pFeaturePara = NULL;
	ACDK_KD_SENSOR_SYNC_STRUCT *pSensorSyncInfo = NULL;
	signed int ret = 0;

	pFeatureCtrl = (ACDK_SENSOR_FEATURECONTROL_STRUCT *)pBuf;
	if (pFeatureCtrl  == NULL) {
		PK_PR_ERR(" NULL arg.\n");
		return -EFAULT;
	}

	psensor = imgsensor_sensor_get_inst(pFeatureCtrl->InvokeCamera);
	if (psensor == NULL) {
		PK_DBG("[adopt_CAMERA_HW_FeatureControl] NULL psensor.\n");
		return -EFAULT;
	}

	if (SENSOR_FEATURE_SINGLE_FOCUS_MODE == pFeatureCtrl->FeatureId || SENSOR_FEATURE_CANCEL_AF == pFeatureCtrl->FeatureId
	|| SENSOR_FEATURE_CONSTANT_AF == pFeatureCtrl->FeatureId || SENSOR_FEATURE_INFINITY_AF == pFeatureCtrl->FeatureId) {/* YUV AF_init and AF_constent and AF_single has no params */
	}
	else
	{
		if (pFeatureCtrl->pFeaturePara == NULL || pFeatureCtrl->pFeatureParaLen == NULL) {
			PK_PR_ERR(" NULL arg.\n");
			return -EFAULT;
		}
		if (copy_from_user((void *)&FeatureParaLen , (void *) pFeatureCtrl->pFeatureParaLen, sizeof(unsigned int))) {
			PK_PR_ERR(" ioctl copy from user failed\n");
			return -EFAULT;
		}

		/* data size exam */
		if (FeatureParaLen > FEATURE_CONTROL_MAX_DATA_SIZE) {
			PK_PR_ERR("exceed data size limitation\n");
			return -EFAULT;
		}

		pFeaturePara = kmalloc(FeatureParaLen, GFP_KERNEL);
		if (pFeaturePara == NULL) {
			PK_PR_ERR(" ioctl allocate mem failed\n");
			return -ENOMEM;
		}
		memset(pFeaturePara, 0x0, FeatureParaLen);
	}

	/* copy from user */
	switch (pFeatureCtrl->FeatureId)
	{
	case SENSOR_FEATURE_OPEN:
		ret = imgsensor_sensor_open(psensor);

		break;
	case SENSOR_FEATURE_CLOSE:
		ret = imgsensor_sensor_close(psensor);
		/* reset the delay frame flag */
		break;

	case SENSOR_FEATURE_SET_DRIVER:
	{
		MINT32 drv_idx;

		psensor->inst.sensor_idx = pFeatureCtrl->InvokeCamera;
		drv_idx = imgsensor_set_driver(psensor);
		memcpy(pFeaturePara, &drv_idx, FeatureParaLen);

		break;
	}
	case SENSOR_FEATURE_CHECK_IS_ALIVE:
		imgsensor_check_is_alive(psensor);
		break;

	case SENSOR_FEATURE_SET_ESHUTTER:
	case SENSOR_FEATURE_SET_GAIN:
	case SENSOR_FEATURE_SET_DUAL_GAIN:
	    /* reset the delay frame flag */
	    //exp_gain.uSensorExpDelayFrame = 0xFF;
	    //exp_gain.uSensorGainDelayFrame = 0xFF;
	    //exp_gain.uISPGainDelayFrame = 0xFF;

	case SENSOR_FEATURE_SET_I2C_BUF_MODE_EN:
	case SENSOR_FEATURE_SET_SHUTTER_BUF_MODE:
	case SENSOR_FEATURE_SET_GAIN_BUF_MODE:
	case SENSOR_FEATURE_SET_ISP_MASTER_CLOCK_FREQ:
	case SENSOR_FEATURE_SET_REGISTER:
	case SENSOR_FEATURE_GET_REGISTER:
	case SENSOR_FEATURE_SET_CCT_REGISTER:
	case SENSOR_FEATURE_SET_ENG_REGISTER:
	case SENSOR_FEATURE_SET_ITEM_INFO:
	case SENSOR_FEATURE_GET_ITEM_INFO:
	case SENSOR_FEATURE_GET_ENG_INFO:
	case SENSOR_FEATURE_SET_VIDEO_MODE:
	case SENSOR_FEATURE_SET_YUV_CMD:
	case SENSOR_FEATURE_MOVE_FOCUS_LENS:
	case SENSOR_FEATURE_SET_AF_WINDOW:
	case SENSOR_FEATURE_SET_CALIBRATION_DATA:
	case SENSOR_FEATURE_SET_AUTO_FLICKER_MODE:
	case SENSOR_FEATURE_GET_EV_AWB_REF:
	case SENSOR_FEATURE_GET_SHUTTER_GAIN_AWB_GAIN:
	case SENSOR_FEATURE_SET_AE_WINDOW:
	case SENSOR_FEATURE_GET_EXIF_INFO:
	case SENSOR_FEATURE_GET_DELAY_INFO:
	case SENSOR_FEATURE_GET_AE_AWB_LOCK_INFO:
	case SENSOR_FEATURE_SET_MAX_FRAME_RATE_BY_SCENARIO:
	case SENSOR_FEATURE_GET_DEFAULT_FRAME_RATE_BY_SCENARIO:
	case SENSOR_FEATURE_SET_TEST_PATTERN:
	case SENSOR_FEATURE_GET_TEST_PATTERN_CHECKSUM_VALUE:
	case SENSOR_FEATURE_SET_OB_LOCK:
	case SENSOR_FEATURE_SET_SENSOR_OTP_AWB_CMD:
	case SENSOR_FEATURE_SET_SENSOR_OTP_LSC_CMD:
	case SENSOR_FEATURE_GET_TEMPERATURE_VALUE:
	case SENSOR_FEATURE_SET_FRAMERATE:
	case SENSOR_FEATURE_SET_HDR:
	case SENSOR_FEATURE_GET_CROP_INFO:
	case SENSOR_FEATURE_GET_VC_INFO:
	case SENSOR_FEATURE_SET_IHDR_SHUTTER_GAIN:
	case SENSOR_FEATURE_SET_HDR_SHUTTER:
	case SENSOR_FEATURE_GET_AE_FLASHLIGHT_INFO:
	case SENSOR_FEATURE_GET_TRIGGER_FLASHLIGHT_INFO: /* return TRUE:play flashlight */
	case SENSOR_FEATURE_SET_YUV_3A_CMD: /* para: ACDK_SENSOR_3A_LOCK_ENUM */
	case SENSOR_FEATURE_SET_AWB_GAIN:
	case SENSOR_FEATURE_SET_MIN_MAX_FPS:
	case SENSOR_FEATURE_GET_PDAF_INFO:
	case SENSOR_FEATURE_GET_PDAF_DATA:
	case SENSOR_FEATURE_GET_4CELL_DATA:
	case SENSOR_FEATURE_GET_SENSOR_PDAF_CAPACITY:
	case SENSOR_FEATURE_GET_SENSOR_HDR_CAPACITY:
	case SENSOR_FEATURE_SET_PDAF:
	case SENSOR_FEATURE_SET_SHUTTER_FRAME_TIME:
	case SENSOR_FEATURE_SET_PDFOCUS_AREA:
	case SENSOR_FEATURE_GET_PDAF_REG_SETTING:
	case SENSOR_FEATURE_SET_PDAF_REG_SETTING:
	case SENSOR_FEATURE_SET_STREAMING_SUSPEND:
	case SENSOR_FEATURE_SET_STREAMING_RESUME:
		if (copy_from_user((void *)pFeaturePara , (void *) pFeatureCtrl->pFeaturePara, FeatureParaLen)) {
			kfree(pFeaturePara);
			PK_PR_ERR("[CAMERA_HW][pFeaturePara] ioctl copy from user failed\n");
			return -EFAULT;
		}
		break;
	case SENSOR_FEATURE_SET_SENSOR_SYNC:
	case SENSOR_FEATURE_SET_ESHUTTER_GAIN:
		PK_DBG("[kd_sensorlist]enter kdSetExpGain\n");
		if (copy_from_user((void *)pFeaturePara , (void *) pFeatureCtrl->pFeaturePara, FeatureParaLen)) {
			kfree(pFeaturePara);
			PK_PR_ERR("[CAMERA_HW][pFeaturePara] ioctl copy from user failed\n");
			return -EFAULT;
		}
		/* keep the information to wait Vsync synchronize */
		pSensorSyncInfo = (ACDK_KD_SENSOR_SYNC_STRUCT *)pFeaturePara;

		FeatureParaLen = 2;

		imgsensor_sensor_feature_control(psensor, SENSOR_FEATURE_SET_ESHUTTER, (unsigned char *)&pSensorSyncInfo->u2SensorNewExpTime, (unsigned int *) &FeatureParaLen);
		imgsensor_sensor_feature_control(psensor, SENSOR_FEATURE_SET_GAIN, (unsigned char *)&pSensorSyncInfo->u2SensorNewGain, (unsigned int *) &FeatureParaLen);
		break;

	/* copy to user */
	case SENSOR_FEATURE_GET_RESOLUTION:
	case SENSOR_FEATURE_GET_PERIOD:
	case SENSOR_FEATURE_GET_PIXEL_CLOCK_FREQ:
	case SENSOR_FEATURE_GET_REGISTER_DEFAULT:
	case SENSOR_FEATURE_GET_CONFIG_PARA:
	case SENSOR_FEATURE_GET_GROUP_COUNT:
	case SENSOR_FEATURE_GET_LENS_DRIVER_ID:
	/* do nothing */
	case SENSOR_FEATURE_CAMERA_PARA_TO_SENSOR:
	case SENSOR_FEATURE_SENSOR_TO_CAMERA_PARA:
	case SENSOR_FEATURE_SINGLE_FOCUS_MODE:
	case SENSOR_FEATURE_CANCEL_AF:
	case SENSOR_FEATURE_CONSTANT_AF:
	default:
	    break;
	}

	/*in case that some structure are passed from user sapce by ptr */
	switch (pFeatureCtrl->FeatureId) {
	case SENSOR_FEATURE_OPEN:
	case SENSOR_FEATURE_CLOSE:
		break;
	case SENSOR_FEATURE_GET_DEFAULT_FRAME_RATE_BY_SCENARIO:
	case SENSOR_FEATURE_GET_SENSOR_PDAF_CAPACITY:
	case SENSOR_FEATURE_GET_SENSOR_HDR_CAPACITY:
	{
		MUINT32 *pValue = NULL;
		unsigned long long *pFeaturePara_64 = (unsigned long long *)pFeaturePara;
		pValue = kmalloc(sizeof(MUINT32), GFP_KERNEL);
		if (pValue == NULL) {
			PK_PR_ERR(" ioctl allocate mem failed\n");
			kfree(pFeaturePara);
			return -ENOMEM;
		}

		memset(pValue, 0x0, sizeof(MUINT32));
		*(pFeaturePara_64 + 1) = (uintptr_t)pValue;

		ret = imgsensor_sensor_feature_control(psensor,
		                                   pFeatureCtrl->FeatureId,
		                                   (unsigned char *)
		                                   pFeaturePara,
		                                   (unsigned int *)
		                                   &FeatureParaLen);

		*(pFeaturePara_64 + 1) = *pValue;
		kfree(pValue);
	}
	break;

	case SENSOR_FEATURE_GET_AE_STATUS:
	case SENSOR_FEATURE_GET_TEST_PATTERN_CHECKSUM_VALUE:
	case SENSOR_FEATURE_GET_TEMPERATURE_VALUE:
	case SENSOR_FEATURE_GET_AF_STATUS:
	case SENSOR_FEATURE_GET_AWB_STATUS:
	case SENSOR_FEATURE_GET_AF_MAX_NUM_FOCUS_AREAS:
	case SENSOR_FEATURE_GET_AE_MAX_NUM_METERING_AREAS:
	case SENSOR_FEATURE_GET_TRIGGER_FLASHLIGHT_INFO:
	case SENSOR_FEATURE_GET_SENSOR_N3D_STREAM_TO_VSYNC_TIME:
	case SENSOR_FEATURE_GET_PERIOD:
	case SENSOR_FEATURE_GET_PIXEL_CLOCK_FREQ:
	{
		ret = imgsensor_sensor_feature_control(psensor,
		                                   pFeatureCtrl->FeatureId,
		                                   (unsigned char *)
		                                   pFeaturePara,
		                                   (unsigned int *)
		                                   &FeatureParaLen);
	}
	break;

	case SENSOR_FEATURE_GET_AE_AWB_LOCK_INFO:
	case SENSOR_FEATURE_AUTOTEST_CMD:
	{
		MUINT32 *pValue0 = NULL;
		MUINT32 *pValue1 = NULL;
		unsigned long long *pFeaturePara_64 = (unsigned long long *)pFeaturePara;
		pValue0 = kmalloc(sizeof(MUINT32), GFP_KERNEL);
		pValue1 = kmalloc(sizeof(MUINT32), GFP_KERNEL);

		if (pValue0 == NULL || pValue1 == NULL) {
			PK_PR_ERR(" ioctl allocate mem failed\n");
			kfree(pValue0);
			kfree(pValue1);
			kfree(pFeaturePara);
			return -ENOMEM;
		}
		memset(pValue1, 0x0, sizeof(MUINT32));
		memset(pValue0, 0x0, sizeof(MUINT32));
		*(pFeaturePara_64) = (uintptr_t)pValue0;
		*(pFeaturePara_64 + 1) = (uintptr_t)pValue1;

		ret = imgsensor_sensor_feature_control(psensor,
		                                   pFeatureCtrl->FeatureId,
		                                   (unsigned char *)
		                                   pFeaturePara,
		                                   (unsigned int *)
		                                   &FeatureParaLen);

		*(pFeaturePara_64) = *pValue0;
		*(pFeaturePara_64 + 1) = *pValue1;
		kfree(pValue0);
		kfree(pValue1);
	}
	break;

	case SENSOR_FEATURE_GET_EV_AWB_REF:
	{
		SENSOR_AE_AWB_REF_STRUCT *pAeAwbRef = NULL;
		unsigned long long *pFeaturePara_64 = (unsigned long long *)pFeaturePara;
		void *usr_ptr = (void*)(uintptr_t)(*(pFeaturePara_64));
		pAeAwbRef = kmalloc(sizeof(SENSOR_AE_AWB_REF_STRUCT), GFP_KERNEL);
		if (pAeAwbRef == NULL) {
			kfree(pFeaturePara);
			PK_PR_ERR(" ioctl allocate mem failed\n");
			return -ENOMEM;
		}
		memset(pAeAwbRef, 0x0, sizeof(SENSOR_AE_AWB_REF_STRUCT));
		*(pFeaturePara_64) = (uintptr_t)pAeAwbRef;

		ret = imgsensor_sensor_feature_control(psensor,
		                                   pFeatureCtrl->FeatureId,
		                                   (unsigned char *)
		                                   pFeaturePara,
		                                   (unsigned int *)
		                                   &FeatureParaLen);

		if (copy_to_user
		    ((void __user *)usr_ptr, (void *)pAeAwbRef,
		     sizeof(SENSOR_AE_AWB_REF_STRUCT))) {
			PK_DBG("[CAMERA_HW]ERROR: copy_to_user fail \n");
		}
		kfree(pAeAwbRef);
		*(pFeaturePara_64) = (uintptr_t)usr_ptr;
	}
	break;

	case SENSOR_FEATURE_GET_CROP_INFO:
	{
		SENSOR_WINSIZE_INFO_STRUCT *pCrop = NULL;
		unsigned long long *pFeaturePara_64 = (unsigned long long *)pFeaturePara;
		void *usr_ptr = (void *)(uintptr_t) (*(pFeaturePara_64 + 1));
		pCrop = kmalloc(sizeof(SENSOR_WINSIZE_INFO_STRUCT), GFP_KERNEL);
		if (pCrop == NULL) {
			kfree(pFeaturePara);
			PK_PR_ERR(" ioctl allocate mem failed\n");
			return -ENOMEM;
		}
		memset(pCrop, 0x0, sizeof(SENSOR_WINSIZE_INFO_STRUCT));
		*(pFeaturePara_64 + 1) = (uintptr_t)pCrop;

		ret = imgsensor_sensor_feature_control(psensor,
			                                   pFeatureCtrl->FeatureId,
			                                   (unsigned char *)
			                                   pFeaturePara,
			                                   (unsigned int *)
			                                   &FeatureParaLen);

		//PK_DBG("[CAMERA_HW]crop =%d\n",framerate);

		if (copy_to_user
		    ((void __user *)usr_ptr, (void *)pCrop,
		     sizeof(SENSOR_WINSIZE_INFO_STRUCT))) {
			PK_DBG("[CAMERA_HW]ERROR: copy_to_user fail \n");
		}
		kfree(pCrop);
		*(pFeaturePara_64 + 1) = (uintptr_t)usr_ptr;
	}
	break;

	case SENSOR_FEATURE_GET_VC_INFO:
	{
		SENSOR_VC_INFO_STRUCT *pVcInfo = NULL;
		unsigned long long *pFeaturePara_64 = (unsigned long long *)pFeaturePara;
		void *usr_ptr = (void *)(uintptr_t) (*(pFeaturePara_64 + 1));
		pVcInfo = kmalloc(sizeof(SENSOR_VC_INFO_STRUCT), GFP_KERNEL);
		if (pVcInfo == NULL) {
			PK_PR_ERR(" ioctl allocate mem failed\n");
			kfree(pFeaturePara);
			return -ENOMEM;
		}
		memset(pVcInfo, 0x0, sizeof(SENSOR_VC_INFO_STRUCT));
		*(pFeaturePara_64 + 1) = (uintptr_t)pVcInfo;

		ret = imgsensor_sensor_feature_control(psensor,
		                                   pFeatureCtrl->FeatureId,
		                                   (unsigned char *)
		                                   pFeaturePara,
		                                   (unsigned int *)
		                                   &FeatureParaLen);

		if (copy_to_user
		    ((void __user *)usr_ptr, (void *)pVcInfo,
		     sizeof(SENSOR_VC_INFO_STRUCT))) {
			PK_DBG("[CAMERA_HW]ERROR: copy_to_user fail \n");
		}
		kfree(pVcInfo);
		*(pFeaturePara_64 + 1) = (uintptr_t)usr_ptr;
	}
	break;

	case SENSOR_FEATURE_GET_PDAF_INFO:
	{
		SET_PD_BLOCK_INFO_T *pPdInfo = NULL;
		unsigned long long *pFeaturePara_64 = (unsigned long long *)pFeaturePara;
		void *usr_ptr = (void *)(uintptr_t) (*(pFeaturePara_64 + 1));
		pPdInfo = kmalloc(sizeof(SET_PD_BLOCK_INFO_T), GFP_KERNEL);
		if (pPdInfo == NULL) {
			kfree(pFeaturePara);
			PK_PR_ERR(" ioctl allocate mem failed\n");
			return -ENOMEM;
		}
		memset(pPdInfo, 0x0, sizeof(SET_PD_BLOCK_INFO_T));
		*(pFeaturePara_64 + 1) = (uintptr_t)pPdInfo;

		ret = imgsensor_sensor_feature_control(psensor,
							pFeatureCtrl->FeatureId,
							(unsigned char *)
							pFeaturePara,
							(unsigned int *)
							&FeatureParaLen);

		if (copy_to_user
		    ((void __user *)usr_ptr, (void *)pPdInfo,
		     sizeof(SET_PD_BLOCK_INFO_T))) {
			PK_DBG("[CAMERA_HW]ERROR: copy_to_user fail \n");
		}
		kfree(pPdInfo);
		*(pFeaturePara_64 + 1) = (uintptr_t)usr_ptr;
	}
	break;

	case SENSOR_FEATURE_GET_PDAF_REG_SETTING:
	case SENSOR_FEATURE_SET_PDAF_REG_SETTING:
	{
		unsigned long long *pFeaturePara_64 = (unsigned long long *)pFeaturePara;
		kal_uint32 u4RegLen = (*pFeaturePara_64);
		void *usr_ptr_Reg = (void *)(uintptr_t) (*(pFeaturePara_64 + 1));
		kal_uint32 *pReg = NULL;

		/* buffer size exam */
		if ((sizeof(kal_uint8) * u4RegLen) > FEATURE_CONTROL_MAX_DATA_SIZE) {
			kfree(pFeaturePara);
			PK_PR_ERR(" buffer size (%u) is too large\n", u4RegLen);
			return -EINVAL;
		}

		pReg = kmalloc_array(u4RegLen, sizeof(kal_uint8), GFP_KERNEL);
		if (pReg == NULL) {
			kfree(pFeaturePara);
			PK_PR_ERR(" ioctl allocate mem failed\n");
			return -ENOMEM;
		}

		memset(pReg, 0x0, sizeof(kal_uint8)*u4RegLen);

		if (copy_from_user
		((void *)pReg, (void *)usr_ptr_Reg, sizeof(kal_uint8)*u4RegLen)) {
			PK_PR_ERR("[CAMERA_HW]ERROR: copy from user fail\n");
		}

			ret = imgsensor_sensor_feature_control(psensor,
					pFeatureCtrl->FeatureId,
					(unsigned char *)
					pReg,
					(unsigned int *)
					&u4RegLen);

		if (copy_to_user
			((void __user *)usr_ptr_Reg, (void *)pReg,
				sizeof(kal_uint8)*u4RegLen)) {
			PK_DBG("[CAMERA_HW]ERROR: copy_to_user fail \n");
		}
		kfree(pReg);
	}

	break;

	case SENSOR_FEATURE_SET_AF_WINDOW:
	case SENSOR_FEATURE_SET_AE_WINDOW:
	{
		MUINT32 *pApWindows = NULL;
		unsigned long long *pFeaturePara_64 = (unsigned long long *)pFeaturePara;
		void *usr_ptr = (void *)(uintptr_t) (*(pFeaturePara_64));
		pApWindows = kmalloc(sizeof(MUINT32) * 6, GFP_KERNEL);
		if (pApWindows == NULL) {
			kfree(pFeaturePara);
			PK_PR_ERR(" ioctl allocate mem failed\n");
			return -ENOMEM;
		}
		memset(pApWindows, 0x0, sizeof(MUINT32) * 6);
		*(pFeaturePara_64) = (uintptr_t)pApWindows;

		if (copy_from_user((void *)pApWindows,
							(void *)usr_ptr,
							sizeof(MUINT32) * 6)) {
			PK_PR_ERR("[CAMERA_HW]ERROR: copy from user fail\n");
		}

		ret = imgsensor_sensor_feature_control(psensor,
								pFeatureCtrl->FeatureId,
								(unsigned char *)pFeaturePara,
								(unsigned int *)&FeatureParaLen);
		kfree(pApWindows);
		*(pFeaturePara_64) = (uintptr_t)usr_ptr;
	}
	break;

	case SENSOR_FEATURE_GET_EXIF_INFO:
	{
		SENSOR_EXIF_INFO_STRUCT *pExif = NULL;
		unsigned long long *pFeaturePara_64 = (unsigned long long *)pFeaturePara;
		void *usr_ptr =  (void *)(uintptr_t) (*(pFeaturePara_64));
		pExif = kmalloc(sizeof(SENSOR_EXIF_INFO_STRUCT), GFP_KERNEL);
		if (pExif == NULL) {
			kfree(pFeaturePara);
			PK_PR_ERR(" ioctl allocate mem failed\n");
			return -ENOMEM;
		}
		memset(pExif, 0x0, sizeof(SENSOR_EXIF_INFO_STRUCT));
		*(pFeaturePara_64) = (uintptr_t)pExif;

		ret = imgsensor_sensor_feature_control(psensor,
							pFeatureCtrl->FeatureId,
							(unsigned char *)
							pFeaturePara,
							(unsigned int *)
							&FeatureParaLen);

		if (copy_to_user
		    ((void __user *)usr_ptr, (void *)pExif,
		     sizeof(SENSOR_EXIF_INFO_STRUCT))) {
			PK_DBG("[CAMERA_HW]ERROR: copy_to_user fail \n");
		}
		kfree(pExif);
		*(pFeaturePara_64) = (uintptr_t)usr_ptr;
	}
	break;

	case SENSOR_FEATURE_GET_SHUTTER_GAIN_AWB_GAIN:
	{

		SENSOR_AE_AWB_CUR_STRUCT *pCurAEAWB = NULL;
		unsigned long long *pFeaturePara_64 = (unsigned long long *)pFeaturePara;
		void *usr_ptr = (void *)(uintptr_t) (*(pFeaturePara_64));
		pCurAEAWB = kmalloc(sizeof(SENSOR_AE_AWB_CUR_STRUCT), GFP_KERNEL);
		if (pCurAEAWB == NULL) {
			kfree(pFeaturePara);
			PK_PR_ERR(" ioctl allocate mem failed\n");
			return -ENOMEM;
		}
		memset(pCurAEAWB, 0x0, sizeof(SENSOR_AE_AWB_CUR_STRUCT));
		*(pFeaturePara_64) = (uintptr_t)pCurAEAWB;

		ret = imgsensor_sensor_feature_control(psensor,
							pFeatureCtrl->FeatureId,
							(unsigned char *)
							pFeaturePara,
							(unsigned int *)
							&FeatureParaLen);

		if (copy_to_user
		    ((void __user *)usr_ptr, (void *)pCurAEAWB,
		     sizeof(SENSOR_AE_AWB_CUR_STRUCT))) {
			PK_DBG("[CAMERA_HW]ERROR: copy_to_user fail \n");
		}
		kfree(pCurAEAWB);
		*(pFeaturePara_64) = (uintptr_t)usr_ptr;
	}
	break;

	case SENSOR_FEATURE_GET_DELAY_INFO:
	{
		SENSOR_DELAY_INFO_STRUCT *pDelayInfo = NULL;
		unsigned long long *pFeaturePara_64 = (unsigned long long *)pFeaturePara;
		void *usr_ptr = (void *)(uintptr_t) (*(pFeaturePara_64));
		pDelayInfo = kmalloc(sizeof(SENSOR_DELAY_INFO_STRUCT), GFP_KERNEL);

		if (pDelayInfo == NULL) {
			kfree(pFeaturePara);
			PK_PR_ERR(" ioctl allocate mem failed\n");
			return -ENOMEM;
		}
		memset(pDelayInfo, 0x0, sizeof(SENSOR_DELAY_INFO_STRUCT));
		*(pFeaturePara_64) = (uintptr_t)pDelayInfo;

		ret = imgsensor_sensor_feature_control(psensor,
							pFeatureCtrl->FeatureId,
							(unsigned char *)
							pFeaturePara,
							(unsigned int *)
							&FeatureParaLen);

		if (copy_to_user
		    ((void __user *)usr_ptr, (void *)pDelayInfo,
		     sizeof(SENSOR_DELAY_INFO_STRUCT))) {
			PK_DBG("[CAMERA_HW]ERROR: copy_to_user fail \n");
		}
		kfree(pDelayInfo);
		*(pFeaturePara_64) = (uintptr_t)usr_ptr;

	}
	break;

	case SENSOR_FEATURE_GET_AE_FLASHLIGHT_INFO:
	{
		SENSOR_FLASHLIGHT_AE_INFO_STRUCT *pFlashInfo = NULL;
		unsigned long long *pFeaturePara_64 = (unsigned long long *)pFeaturePara;
		void *usr_ptr = (void *)(uintptr_t) (*(pFeaturePara_64));
		pFlashInfo = kmalloc(sizeof(SENSOR_FLASHLIGHT_AE_INFO_STRUCT), GFP_KERNEL);

		if (pFlashInfo == NULL) {
			kfree(pFeaturePara);
			PK_PR_ERR(" ioctl allocate mem failed\n");
			return -ENOMEM;
		}
		memset(pFlashInfo, 0x0, sizeof(SENSOR_FLASHLIGHT_AE_INFO_STRUCT));
		*(pFeaturePara_64) = (uintptr_t)pFlashInfo;

		ret = imgsensor_sensor_feature_control(psensor,
							pFeatureCtrl->FeatureId,
							(unsigned char *)
							pFeaturePara,
							(unsigned int *)
							&FeatureParaLen);

		if (copy_to_user
		    ((void __user *)usr_ptr, (void *)pFlashInfo,
		     sizeof(SENSOR_FLASHLIGHT_AE_INFO_STRUCT))) {
			PK_DBG("[CAMERA_HW]ERROR: copy_to_user fail \n");
		}
		kfree(pFlashInfo);
		*(pFeaturePara_64) = (uintptr_t)usr_ptr;

	}
	break;

	case SENSOR_FEATURE_GET_PDAF_DATA:
	case SENSOR_FEATURE_GET_4CELL_DATA:
	{
#define PDAF_DATA_SIZE 4096
		char *pPdaf_data = NULL;
		unsigned long long *pFeaturePara_64 = (unsigned long long *) pFeaturePara;
		void *usr_ptr = (void *)(uintptr_t)(*(pFeaturePara_64 + 1));
		kal_uint32 buf_sz = (kal_uint32) (*(pFeaturePara_64 + 2));

		/* buffer size exam */
		if (buf_sz > PDAF_DATA_SIZE) {
			kfree(pFeaturePara);
			PK_PR_ERR(" buffer size (%u) can't larger than %d bytes\n",
				  buf_sz, PDAF_DATA_SIZE);
			return -EINVAL;
		}

		pPdaf_data = kmalloc(sizeof(char) * PDAF_DATA_SIZE, GFP_KERNEL);
		if (pPdaf_data == NULL) {
			kfree(pFeaturePara);
			PK_PR_ERR(" ioctl allocate mem failed\n");
			return -ENOMEM;
		}
		memset(pPdaf_data, 0xff, sizeof(char) * PDAF_DATA_SIZE);

		if (pFeaturePara_64 != NULL)
			*(pFeaturePara_64 + 1) = (uintptr_t)pPdaf_data;


		ret = imgsensor_sensor_feature_control(psensor,
								pFeatureCtrl->FeatureId,
								(unsigned char *)pFeaturePara,
								(unsigned int *)&FeatureParaLen);

		if (copy_to_user((void __user *)usr_ptr,
							(void *)pPdaf_data,
							buf_sz)) {
			PK_DBG("[CAMERA_HW]ERROR: copy_to_user fail\n");
		}
		kfree(pPdaf_data);
		*(pFeaturePara_64 + 1) = (uintptr_t) usr_ptr;
	}
	break;

	default:
		ret = imgsensor_sensor_feature_control(psensor,
							pFeatureCtrl->FeatureId,
							(unsigned char *)pFeaturePara,
							(unsigned int *)&FeatureParaLen);
#ifdef CONFIG_MTK_CCU
		if (pFeatureCtrl->FeatureId == SENSOR_FEATURE_SET_FRAMERATE)
			ccu_set_current_fps(*((int32_t *)pFeaturePara));
#endif
		break;
	}
	/* copy to user */
	switch (pFeatureCtrl->FeatureId)
	{
	case SENSOR_FEATURE_SET_I2C_BUF_MODE_EN:
		imgsensor_i2c_buffer_mode((*(unsigned long long *)pFeaturePara));
		break;
	case SENSOR_FEATURE_SET_ESHUTTER:
	case SENSOR_FEATURE_SET_GAIN:
	case SENSOR_FEATURE_SET_DUAL_GAIN:
	case SENSOR_FEATURE_SET_SHUTTER_BUF_MODE:
	case SENSOR_FEATURE_SET_GAIN_BUF_MODE:
	case SENSOR_FEATURE_SET_GAIN_AND_ESHUTTER:
	case SENSOR_FEATURE_SET_ISP_MASTER_CLOCK_FREQ:
	case SENSOR_FEATURE_SET_REGISTER:
	case SENSOR_FEATURE_SET_CCT_REGISTER:
	case SENSOR_FEATURE_SET_ENG_REGISTER:
	case SENSOR_FEATURE_SET_ITEM_INFO:
	/* do nothing */
	case SENSOR_FEATURE_CAMERA_PARA_TO_SENSOR:
	case SENSOR_FEATURE_SENSOR_TO_CAMERA_PARA:
	case SENSOR_FEATURE_GET_PDAF_DATA:
	case SENSOR_FEATURE_GET_4CELL_DATA:
	case SENSOR_FEATURE_GET_PDAF_REG_SETTING:
	case SENSOR_FEATURE_SET_PDAF_REG_SETTING:
	case SENSOR_FEATURE_SET_STREAMING_SUSPEND:
	case SENSOR_FEATURE_SET_STREAMING_RESUME:
	    break;
	/* copy to user */
	case SENSOR_FEATURE_SET_DRIVER:
	case SENSOR_FEATURE_GET_EV_AWB_REF:
	case SENSOR_FEATURE_GET_SHUTTER_GAIN_AWB_GAIN:
	case SENSOR_FEATURE_GET_EXIF_INFO:
	case SENSOR_FEATURE_GET_DELAY_INFO:
	case SENSOR_FEATURE_GET_AE_AWB_LOCK_INFO:
	case SENSOR_FEATURE_GET_RESOLUTION:
	case SENSOR_FEATURE_GET_PERIOD:
	case SENSOR_FEATURE_GET_PIXEL_CLOCK_FREQ:
	case SENSOR_FEATURE_GET_REGISTER:
	case SENSOR_FEATURE_GET_REGISTER_DEFAULT:
	case SENSOR_FEATURE_GET_CONFIG_PARA:
	case SENSOR_FEATURE_GET_GROUP_COUNT:
	case SENSOR_FEATURE_GET_LENS_DRIVER_ID:
	case SENSOR_FEATURE_GET_ITEM_INFO:
	case SENSOR_FEATURE_GET_ENG_INFO:
	case SENSOR_FEATURE_GET_AF_STATUS:
	case SENSOR_FEATURE_GET_AE_STATUS:
	case SENSOR_FEATURE_GET_AWB_STATUS:
	case SENSOR_FEATURE_GET_AF_INF:
	case SENSOR_FEATURE_GET_AF_MACRO:
	case SENSOR_FEATURE_GET_AF_MAX_NUM_FOCUS_AREAS:
	case SENSOR_FEATURE_GET_TRIGGER_FLASHLIGHT_INFO: /* return TRUE:play flashlight */
	case SENSOR_FEATURE_SET_YUV_3A_CMD: /* para: ACDK_SENSOR_3A_LOCK_ENUM */
	case SENSOR_FEATURE_GET_AE_FLASHLIGHT_INFO:
	case SENSOR_FEATURE_GET_AE_MAX_NUM_METERING_AREAS:
	case SENSOR_FEATURE_CHECK_SENSOR_ID:
	case SENSOR_FEATURE_GET_DEFAULT_FRAME_RATE_BY_SCENARIO:
	case SENSOR_FEATURE_SET_TEST_PATTERN:
	case SENSOR_FEATURE_GET_TEST_PATTERN_CHECKSUM_VALUE:
	case SENSOR_FEATURE_GET_TEMPERATURE_VALUE:
	case SENSOR_FEATURE_SET_FRAMERATE:
	case SENSOR_FEATURE_SET_HDR:
	case SENSOR_FEATURE_SET_IHDR_SHUTTER_GAIN:
	case SENSOR_FEATURE_SET_HDR_SHUTTER:
	case SENSOR_FEATURE_GET_CROP_INFO:
	case SENSOR_FEATURE_GET_VC_INFO:
	case SENSOR_FEATURE_SET_MIN_MAX_FPS:
	case SENSOR_FEATURE_GET_PDAF_INFO:
	case SENSOR_FEATURE_GET_SENSOR_PDAF_CAPACITY:
	case SENSOR_FEATURE_GET_SENSOR_HDR_CAPACITY:
	case SENSOR_FEATURE_SET_ISO:
	case SENSOR_FEATURE_SET_PDAF:
	case SENSOR_FEATURE_SET_SHUTTER_FRAME_TIME:
	case SENSOR_FEATURE_SET_PDFOCUS_AREA:
		if (copy_to_user((void __user *) pFeatureCtrl->pFeaturePara, (void *)pFeaturePara , FeatureParaLen)) {
			kfree(pFeaturePara);
			PK_DBG("[CAMERA_HW][pSensorRegData] ioctl copy to user failed\n");
			return -EFAULT;
		}
	    break;

	default:
	    break;
	}

	kfree(pFeaturePara);
	if (copy_to_user((void __user *) pFeatureCtrl->pFeatureParaLen, (void *)&FeatureParaLen , sizeof(unsigned int))) {
		PK_DBG("[CAMERA_HW][pFeatureParaLen] ioctl copy to user failed\n");
		return -EFAULT;
	}

	return ret;
}   /* adopt_CAMERA_HW_FeatureControl() */

#ifdef CONFIG_COMPAT
static int compat_get_acdk_sensor_getinfo_struct(
	COMPAT_IMGSENSOR_GET_CONFIG_INFO_STRUCT __user *data32,
	IMGSENSOR_GET_CONFIG_INFO_STRUCT __user *data)
{
	compat_uint_t i;
	compat_uptr_t p;
	int err;

	err = get_user(i, &data32->SensorId);
	err |= put_user(i, &data->SensorId);
	err = get_user(i, &data32->ScenarioId);
	err |= put_user(i, &data->ScenarioId);
	err = get_user(p, &data32->pInfo);
	err |= put_user(compat_ptr(p), &data->pInfo);
	err = get_user(p, &data32->pConfig);
	err |= put_user(compat_ptr(p), &data->pConfig);

	return err;
}

static int compat_put_acdk_sensor_getinfo_struct(
	COMPAT_IMGSENSOR_GET_CONFIG_INFO_STRUCT __user *data32,
	IMGSENSOR_GET_CONFIG_INFO_STRUCT __user *data)
{
	compat_uint_t i;
	int err;

	err = get_user(i, &data32->SensorId);
	err |= put_user(i, &data->SensorId);
	err = get_user(i, &data->ScenarioId);
	err |= put_user(i, &data32->ScenarioId);
	err = get_user(i, &data->ScenarioId);
	err |= put_user(i, &data32->ScenarioId);
	return err;
}

static int compat_get_imagesensor_getinfo_struct(
	COMPAT_IMAGESENSOR_GETINFO_STRUCT __user *data32,
	IMAGESENSOR_GETINFO_STRUCT __user *data)
{
	compat_uptr_t p;
	compat_uint_t i;
	int err;

	err = get_user(i, &data32->SensorId);
	err |= put_user(i, &data->SensorId);
	err |= get_user(p, &data32->pInfo);
	err |= put_user(compat_ptr(p), &data->pInfo);
	err |= get_user(p, &data32->pSensorResolution);
	err |= put_user(compat_ptr(p), &data->pSensorResolution);
	return err;
}

static int compat_put_imagesensor_getinfo_struct(
	COMPAT_IMAGESENSOR_GETINFO_STRUCT __user *data32,
	IMAGESENSOR_GETINFO_STRUCT __user *data)
{
	/* compat_uptr_t p; */
	compat_uint_t i;
	int err;

	err = get_user(i, &data->SensorId);
	err |= put_user(i, &data32->SensorId);

	return err;
}

static int compat_get_acdk_sensor_featurecontrol_struct(
	COMPAT_ACDK_SENSOR_FEATURECONTROL_STRUCT
	__user *data32,
	ACDK_SENSOR_FEATURECONTROL_STRUCT __user *
	data)
{
	compat_uptr_t p;
	compat_uint_t i;
	int err;

	err = get_user(i, &data32->InvokeCamera);
	err |= put_user(i, &data->InvokeCamera);
	err |= get_user(i, &data32->FeatureId);
	err |= put_user(i, &data->FeatureId);
	err |= get_user(p, &data32->pFeaturePara);
	err |= put_user(compat_ptr(p), &data->pFeaturePara);
	err |= get_user(p, &data32->pFeatureParaLen);
	err |= put_user(compat_ptr(p), &data->pFeatureParaLen);
	return err;
}

static int compat_put_acdk_sensor_featurecontrol_struct(
	COMPAT_ACDK_SENSOR_FEATURECONTROL_STRUCT
	__user *data32,
	ACDK_SENSOR_FEATURECONTROL_STRUCT __user *
	data)
{
	MUINT8 *p;
	MUINT32 *q;
	compat_uint_t i;
	int err;

	err = get_user(i, &data->InvokeCamera);
	err |= put_user(i, &data32->InvokeCamera);
	err |= get_user(i, &data->FeatureId);
	err |= put_user(i, &data32->FeatureId);
	/* Assume pointer is not change */

	err |= get_user(p, &data->pFeaturePara);
	err |= put_user(ptr_to_compat(p), &data32->pFeaturePara);
	err |= get_user(q, &data->pFeatureParaLen);
	err |= put_user(ptr_to_compat(q), &data32->pFeatureParaLen);

	return err;
}

static int compat_get_acdk_sensor_control_struct(
	COMPAT_ACDK_SENSOR_CONTROL_STRUCT __user *data32,
	ACDK_SENSOR_CONTROL_STRUCT __user *data)
{
	compat_uptr_t p;
	compat_uint_t i;
	int err;

	err = get_user(i, &data32->InvokeCamera);
	err |= put_user(i, &data->InvokeCamera);
	err |= get_user(i, &data32->ScenarioId);
	err |= put_user(i, &data->ScenarioId);
	err |= get_user(p, &data32->pImageWindow);
	err |= put_user(compat_ptr(p), &data->pImageWindow);
	err |= get_user(p, &data32->pSensorConfigData);
	err |= put_user(compat_ptr(p), &data->pSensorConfigData);
	return err;
}

static int compat_put_acdk_sensor_control_struct(
	COMPAT_ACDK_SENSOR_CONTROL_STRUCT __user *data32,
	ACDK_SENSOR_CONTROL_STRUCT __user *data)
{
	/* compat_uptr_t p; */
	compat_uint_t i;
	int err;

	err = get_user(i, &data->InvokeCamera);
	err |= put_user(i, &data32->InvokeCamera);
	err |= get_user(i, &data->ScenarioId);
	err |= put_user(i, &data32->ScenarioId);

	return err;
}

static long imgsensor_compat_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	long ret;

	if (!filp->f_op || !filp->f_op->unlocked_ioctl)
		return -ENOTTY;

	switch (cmd) {
	case COMPAT_KDIMGSENSORIOC_X_GETINFO:
	{
		COMPAT_IMGSENSOR_GET_CONFIG_INFO_STRUCT __user *data32;
		IMGSENSOR_GET_CONFIG_INFO_STRUCT __user *data;
		int err;
		/*PK_DBG("[CAMERA SENSOR] CAOMPAT_KDIMGSENSORIOC_X_GETINFO E\n"); */

		data32 = compat_ptr(arg);
		data = compat_alloc_user_space(sizeof(*data));
		if (data == NULL)
			return -EFAULT;

		err = compat_get_acdk_sensor_getinfo_struct(data32, data);
		if (err)
			return err;

		ret = filp->f_op->unlocked_ioctl(filp, KDIMGSENSORIOC_X_GET_CONFIG_INFO, (unsigned long)data);
		err = compat_put_acdk_sensor_getinfo_struct(data32, data);

		if (err != 0)
			PK_DBG("[CAMERA SENSOR] compat_put_acdk_sensor_getinfo_struct failed\n");
		return ret;
	}
	case COMPAT_KDIMGSENSORIOC_X_FEATURECONCTROL:
	{
		COMPAT_ACDK_SENSOR_FEATURECONTROL_STRUCT __user *data32;
		ACDK_SENSOR_FEATURECONTROL_STRUCT __user *data;
		int err;

		/* PK_DBG("[CAMERA SENSOR] CAOMPAT_KDIMGSENSORIOC_X_FEATURECONCTROL\n"); */

		data32 = compat_ptr(arg);
		data = compat_alloc_user_space(sizeof(*data));
		if (data == NULL)
			return -EFAULT;

		err = compat_get_acdk_sensor_featurecontrol_struct(data32, data);
		if (err)
			return err;

		ret = filp->f_op->unlocked_ioctl(filp, KDIMGSENSORIOC_X_FEATURECONCTROL, (unsigned long)data);
		err = compat_put_acdk_sensor_featurecontrol_struct(data32, data);

		if (err != 0)
			PK_PR_ERR("[CAMERA SENSOR] compat_put_acdk_sensor_getinfo_struct failed\n");
		return ret;
	}
	case COMPAT_KDIMGSENSORIOC_X_CONTROL:
	{
		COMPAT_ACDK_SENSOR_CONTROL_STRUCT __user *data32;
		ACDK_SENSOR_CONTROL_STRUCT __user *data;
		int err;

		PK_DBG("[CAMERA SENSOR] CAOMPAT_KDIMGSENSORIOC_X_CONTROL\n");

		data32 = compat_ptr(arg);
		data = compat_alloc_user_space(sizeof(*data));
		if (data == NULL)
			return -EFAULT;

		err = compat_get_acdk_sensor_control_struct(data32, data);
		if (err)
			return err;
		ret = filp->f_op->unlocked_ioctl(filp, KDIMGSENSORIOC_X_CONTROL, (unsigned long)data);
		err = compat_put_acdk_sensor_control_struct(data32, data);

		if (err != 0)
			PK_PR_ERR("[CAMERA SENSOR] compat_put_acdk_sensor_getinfo_struct failed\n");
		return ret;
	}
	case COMPAT_KDIMGSENSORIOC_X_GETINFO2:
	{
		COMPAT_IMAGESENSOR_GETINFO_STRUCT __user *data32;
		IMAGESENSOR_GETINFO_STRUCT __user *data;
		int err;

		PK_DBG("[CAMERA SENSOR] CAOMPAT_KDIMGSENSORIOC_X_GETINFO2\n");

		data32 = compat_ptr(arg);
		data = compat_alloc_user_space(sizeof(*data));
		if (data == NULL)
			return -EFAULT;

		err = compat_get_imagesensor_getinfo_struct(data32, data);
		if (err)
			return err;
		ret = filp->f_op->unlocked_ioctl(filp, KDIMGSENSORIOC_X_GETINFO2, (unsigned long)data);
		err = compat_put_imagesensor_getinfo_struct(data32, data);

		if (err != 0)
			PK_PR_ERR("[CAMERA SENSOR] compat_put_acdk_sensor_getinfo_struct failed\n");
		return ret;
	}
	case COMPAT_KDIMGSENSORIOC_X_GETRESOLUTION2:
	{
		return 0;
	}

	default:
		return filp->f_op->unlocked_ioctl(filp, cmd, arg);
	}
}
#endif

/*******************************************************************************
* imgsensor_ioctl
********************************************************************************/
static long imgsensor_ioctl(
    struct file *a_pstFile,
    unsigned int a_u4Command,
    unsigned long a_u4Param
)
{
	int i4RetValue = 0;
	void *pBuff = NULL;

	if (_IOC_DIR(a_u4Command) != _IOC_NONE) {
		if ((pBuff = kmalloc(_IOC_SIZE(a_u4Command), GFP_KERNEL)) == NULL) {
			PK_DBG("[CAMERA SENSOR] ioctl allocate mem failed\n");
			i4RetValue = -ENOMEM;
			goto CAMERA_HW_Ioctl_EXIT;
		}

		if (_IOC_WRITE & _IOC_DIR(a_u4Command)) {
			if (copy_from_user(pBuff, (void *)a_u4Param, _IOC_SIZE(a_u4Command))) {
				kfree(pBuff);
				PK_DBG("[CAMERA SENSOR] ioctl copy from user failed\n");
				i4RetValue =  -EFAULT;
				goto CAMERA_HW_Ioctl_EXIT;
			}
		}
	} else {
		i4RetValue =  -EFAULT;
		goto CAMERA_HW_Ioctl_EXIT;
	}

	switch (a_u4Command) {
	case KDIMGSENSORIOC_X_GET_CONFIG_INFO:
		i4RetValue = adopt_CAMERA_HW_GetInfo(pBuff);
		break;
	case KDIMGSENSORIOC_X_GETINFO2:
		i4RetValue = adopt_CAMERA_HW_GetInfo2(pBuff);
		break;
	case KDIMGSENSORIOC_X_FEATURECONCTROL:
		i4RetValue = adopt_CAMERA_HW_FeatureControl(pBuff);
		break;
	case KDIMGSENSORIOC_X_CONTROL:
		i4RetValue = adopt_CAMERA_HW_Control(pBuff);
		break;
	case KDIMGSENSORIOC_X_SET_MCLK_PLL:
		i4RetValue = imgsensor_clk_set(&pgimgsensor->clk, (ACDK_SENSOR_MCLK_STRUCT *)pBuff);
		break;
	case KDIMGSENSORIOC_X_GET_ISP_CLK:
	/*E1(High):490, (Medium):364, (low):273*/
#define ISP_CLK_LOW    273
#define ISP_CLK_MEDIUM 364
#define ISP_CLK_HIGH   490
#ifdef CONFIG_MTK_SMI_EXT
		PK_DBG("KDIMGSENSORIOC_X_GET_ISP_CLK current_mmsys_clk=%d\n", current_mmsys_clk);
		if (mmdvfs_get_stable_isp_clk() == MMSYS_CLK_HIGH)
			*(unsigned int *)pBuff = ISP_CLK_HIGH;
		else if (mmdvfs_get_stable_isp_clk() == MMSYS_CLK_MEDIUM)
			*(unsigned int *)pBuff = ISP_CLK_MEDIUM;
		else
			*(unsigned int *)pBuff = ISP_CLK_LOW;
#else
		*(unsigned int *)pBuff = ISP_CLK_HIGH;
#endif
		break;

	case KDIMGSENSORIOC_X_GET_CSI_CLK:
		*(unsigned int *)pBuff = mt_get_ckgen_freq(*(unsigned int *)pBuff);
		PK_DBG("f_fcamtg_ck = %d\n", mt_get_ckgen_freq(8));
		PK_DBG("f_fcamtg2_ck = %d\n", mt_get_ckgen_freq(41));
		PK_DBG("f_fcam_ck = %d\n", mt_get_ckgen_freq(5));
		PK_DBG("f_fseninf_ck = %d\n", mt_get_ckgen_freq(35));
		break;

	case KDIMGSENSORIOC_T_OPEN:
	case KDIMGSENSORIOC_T_CLOSE:
	case KDIMGSENSORIOC_T_CHECK_IS_ALIVE:
	case KDIMGSENSORIOC_X_SET_DRIVER:
	case KDIMGSENSORIOC_X_GETRESOLUTION2:
	case KDIMGSENSORIOC_X_GET_SOCKET_POS:
	case KDIMGSENSORIOC_X_SET_GPIO:
	case KDIMGSENSORIOC_X_SET_I2CBUS:
	case KDIMGSENSORIOC_X_RELEASE_I2C_TRIGGER_LOCK:
	case KDIMGSENSORIOC_X_SET_SHUTTER_GAIN_WAIT_DONE:
	case KDIMGSENSORIOC_X_SET_CURRENT_SENSOR:
		i4RetValue = 0;
	break;

	default:
		PK_DBG("No such command %d\n", a_u4Command);
		i4RetValue = -EPERM;
		break;

	}

	if ((_IOC_READ & _IOC_DIR(a_u4Command)) &&
	    copy_to_user((void __user *) a_u4Param,
	    pBuff,
	    _IOC_SIZE(a_u4Command))) {
		kfree(pBuff);
		PK_DBG("[CAMERA SENSOR] ioctl copy to user failed\n");
		i4RetValue =  -EFAULT;
		goto CAMERA_HW_Ioctl_EXIT;
	}

	kfree(pBuff);
CAMERA_HW_Ioctl_EXIT:
	return i4RetValue;
}

static int imgsensor_open(struct inode *a_pstInode, struct file *a_pstFile)
{
	if (atomic_read(&pgimgsensor->imgsensor_open_cnt) == 0)
		imgsensor_clk_enable_all(&pgimgsensor->clk);

	atomic_inc(&pgimgsensor->imgsensor_open_cnt);
	PK_DBG("imgsensor_open %d\n", atomic_read(&pgimgsensor->imgsensor_open_cnt));
	return 0;
}

static int imgsensor_release(struct inode *a_pstInode, struct file *a_pstFile)
{
	atomic_dec(&pgimgsensor->imgsensor_open_cnt);
	if (atomic_read(&pgimgsensor->imgsensor_open_cnt) == 0) {
		imgsensor_clk_disable_all(&pgimgsensor->clk);
		imgsensor_hw_release_all(&pgimgsensor->hw);
	}
	PK_DBG("imgsensor_release %d\n", atomic_read(&pgimgsensor->imgsensor_open_cnt));
	return 0;
}

static const struct file_operations gimgsensor_file_operations =
{
	.owner          = THIS_MODULE,
	.open           = imgsensor_open,
	.release        = imgsensor_release,
	.unlocked_ioctl = imgsensor_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl   = imgsensor_compat_ioctl
#endif
};

inline static int imgsensor_driver_register(void)
{
	dev_t dev_no = MKDEV(IMGSENSOR_DEVICE_NNUMBER, 0);

	if (alloc_chrdev_region(&dev_no, 0, 1, IMGSENSOR_DEV_NAME))
	{
		PK_DBG("[CAMERA SENSOR] Allocate device no failed\n");

		return -EAGAIN;
	}

	/* Allocate driver */
	if ((gpimgsensor_cdev = cdev_alloc()) ==  NULL)
	{
		unregister_chrdev_region(dev_no, 1);
		PK_DBG("[CAMERA SENSOR] Allocate mem for kobject failed\n");
		return -ENOMEM;
	}

	/* Attatch file operation. */
	cdev_init(gpimgsensor_cdev, &gimgsensor_file_operations);

	gpimgsensor_cdev->owner = THIS_MODULE;

	/* Add to system */
	if (cdev_add(gpimgsensor_cdev, dev_no, 1))
	{
		PK_DBG("Attatch file operation failed\n");
		unregister_chrdev_region(dev_no, 1);
		return -EAGAIN;
	}

	gpimgsensor_class = class_create(THIS_MODULE, "sensordrv");
	if (IS_ERR(gpimgsensor_class)) {
		int ret = PTR_ERR(gpimgsensor_class);
		PK_DBG("Unable to create class, err = %d\n", ret);
		return ret;
	}

	gimgsensor_device = device_create(gpimgsensor_class, NULL, dev_no, NULL, IMGSENSOR_DEV_NAME);

	return 0;
}

inline static void imgsensor_driver_unregister(void)
{
	/* Release char driver */
	cdev_del(gpimgsensor_cdev);

	unregister_chrdev_region(MKDEV(IMGSENSOR_DEVICE_NNUMBER, 0), 1);

	device_destroy(gpimgsensor_class, MKDEV(IMGSENSOR_DEVICE_NNUMBER, 0));
	class_destroy(gpimgsensor_class);
}

#ifdef CONFIG_MTK_SMI_EXT
int mmsys_clk_change_cb(int ori_clk_mode, int new_clk_mode)
{
	if ((ori_clk_mode != new_clk_mode) || (current_mmsys_clk != new_clk_mode))
		PK_DBG("mmsys_clk_change_cb ori: %d, new: %d, current_mmsys_clk %d\n",
			ori_clk_mode, new_clk_mode, current_mmsys_clk);

	current_mmsys_clk = new_clk_mode;
	return 1;
}
#endif

static int imgsensor_probe(struct platform_device *pdev)
{
	/* Register char driver */
	if (imgsensor_driver_register()) {
		PK_PR_ERR("[CAMERA_HW] register char device failed!\n");
		return -1;
	}

	gpimgsensor_hw_platform_device = pdev;

#if !defined (CONFIG_FPGA_EARLY_PORTING)
	imgsensor_clk_init(&pgimgsensor->clk);
#endif
	imgsensor_hw_init(&pgimgsensor->hw);
	imgsensor_i2c_create();
	imgsensor_proc_init();

	atomic_set(&pgimgsensor->imgsensor_open_cnt, 0);
#ifdef CONFIG_MTK_SMI_EXT
	mmdvfs_register_mmclk_switch_cb(mmsys_clk_change_cb, MMDVFS_CLIENT_ID_ISP);
#endif

	return 0;
}

static int imgsensor_remove(struct platform_device *pdev)
{
	imgsensor_i2c_delete();
	imgsensor_driver_unregister();

	return 0;
}

static int imgsensor_suspend(struct platform_device *pdev, pm_message_t mesg)
{
	return 0;
}

static int imgsensor_resume(struct platform_device *pdev)
{
	return 0;
}

/*=======================================================================
  * platform driver
  *=======================================================================*/

#ifdef CONFIG_OF
static const struct of_device_id gimgsensor_of_device_id[] = {
	{ .compatible = "mediatek,camera_hw", },
	{}
};
#endif

static struct platform_driver gimgsensor_platform_driver = {
	.probe   = imgsensor_probe,
	.remove  = imgsensor_remove,
	.suspend = imgsensor_suspend,
	.resume  = imgsensor_resume,
	.driver  = {
	.name    = "image_sensor",
	.owner   = THIS_MODULE,
#ifdef CONFIG_OF
	.of_match_table = gimgsensor_of_device_id,
#endif
    }
};

/*=======================================================================
  * imgsensor_init()
  *=======================================================================*/
static int __init imgsensor_init(void)
{
	PK_DBG("[camerahw_probe] start\n");

	if (platform_driver_register(&gimgsensor_platform_driver)) {
		PK_PR_ERR("failed to register CAMERA_HW driver\n");
		return -ENODEV;
	}

#ifdef CONFIG_CAM_TEMPERATURE_WORKQUEUE
	memset((void *)&cam_temperature_wq, 0, sizeof(cam_temperature_wq));
	INIT_DELAYED_WORK(&cam_temperature_wq, cam_temperature_report_wq_routine);
	schedule_delayed_work(&cam_temperature_wq, HZ);
#endif
	return 0;
}

/*=======================================================================
  * imgsensor_exit()
  *=======================================================================*/
static void __exit imgsensor_exit(void)
{
	platform_driver_unregister(&gimgsensor_platform_driver);
}

module_init(imgsensor_init);
module_exit(imgsensor_exit);

MODULE_DESCRIPTION("image sensor driver");
MODULE_AUTHOR("Mediatek");
MODULE_LICENSE("GPL 2.0");


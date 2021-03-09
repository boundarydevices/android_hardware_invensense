/*
 * Copyright (C) 2014-2020 InvenSense, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_NDEBUG 0

#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <float.h>
#include <poll.h>
#include <unistd.h>
#include <dirent.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <dlfcn.h>
#include <pthread.h>
#include <vector>
#include <string>
#include <string.h>

#include "MPLSensor.h"
#include "MPLSupport.h"

#include "Log.h"
#include "ml_sysfs_helper.h"

#define MAX_SYSFS_ATTRB (sizeof(struct sysfs_attrbs) / sizeof(char*))

/* Accel enhanced FSR support */
#ifdef ACCEL_ENHANCED_FSR_SUPPORT
#define ACCEL_FSR       32.0f   // 32g
#define ACCEL_FSR_SYSFS 4       // 0:2g, 1:4g, 2:8g, 3:16g, 4:32g
#else
#define ACCEL_FSR       8.0f    // 8g
#define ACCEL_FSR_SYSFS 2       // 0:2g, 1:4g, 2:8g, 3:16g, 4:32g
#endif

/* Gyro enhanced FSR support */
#ifdef ACCEL_ENHANCED_FSR_SUPPORT
#define GYRO_FSR        4000.0f // 4000dps
#define GYRO_FSR_SYSFS  4       // 0:250dps, 1:500dps, 2:1000dps, 3:2000dps, 4:4000dps
#else
#define GYRO_FSR        2000.0f // 2000dps
#define GYRO_FSR_SYSFS  3       // 0:250dps, 1:500dps, 2:1000dps, 3:2000dps, 4:4000dps
#endif

#ifdef ODR_SMPLRT_DIV
#define MAX_DELAY_US    250000 // for ICM2xxxx
#else
#define MAX_DELAY_US    320000 // for ICM4xxxx
#endif

#ifdef FIFO_HIGH_RES_ENABLE
#define MAX_LSB_DATA    524288.0f   // 2^19
#else
#define MAX_LSB_DATA    32768.0f    // 2^15
#endif

/*******************************************************************************
 * MPLSensor class implementation
 ******************************************************************************/
#ifdef BATCH_MODE_SUPPORT
static struct sensor_t sRawSensorList[] =
{
    {"Invensense Gyroscope Uncalibrated", "Invensense", 1,
     SENSORS_RAW_GYROSCOPE_HANDLE,
     SENSOR_TYPE_GYROSCOPE_UNCALIBRATED, GYRO_FSR * M_PI / 180.0f, GYRO_FSR * M_PI / (180.0f * MAX_LSB_DATA), 3.0f, 5000, 0, 512 * 7 / 10 / 6,
     "android.sensor.gyroscope_uncalibrated", "", MAX_DELAY_US, SENSOR_FLAG_CONTINUOUS_MODE, {}},
    {"Invensense Accelerometer", "Invensense", 1,
     SENSORS_ACCELERATION_HANDLE,
     SENSOR_TYPE_ACCELEROMETER, GRAVITY_EARTH * ACCEL_FSR, GRAVITY_EARTH * ACCEL_FSR / MAX_LSB_DATA, 0.4f, 5000, 0, 512 * 7 / 10 / 6,
     "android.sensor.accelerometer", "", MAX_DELAY_US, SENSOR_FLAG_CONTINUOUS_MODE, {}},
#ifdef COMPASS_SUPPORT
    {"Invensense Magnetometer Uncalibrated", "Invensense", 1,
     SENSORS_RAW_MAGNETIC_FIELD_HANDLE,
     SENSOR_TYPE_MAGNETIC_FIELD_UNCALIBRATED, 10240.0f, 1.0f, 0.5f, 20000, 0, 0,
     "android.sensor.magnetic_field_uncalibrated", "", 250000, SENSOR_FLAG_CONTINUOUS_MODE, {}},
#endif
};
#else
static struct sensor_t sRawSensorList[] =
{
    {"Invensense Gyroscope Uncalibrated", "Invensense", 1,
     SENSORS_RAW_GYROSCOPE_HANDLE,
     SENSOR_TYPE_GYROSCOPE_UNCALIBRATED, GYRO_FSR * M_PI / 180.0f, GYRO_FSR * M_PI / (180.0f * MAX_LSB_DATA), 3.0f, 5000, 0, 0,
     "android.sensor.gyroscope_uncalibrated", "", MAX_DELAY_US, SENSOR_FLAG_CONTINUOUS_MODE, {}},
    {"Invensense Accelerometer", "Invensense", 1,
     SENSORS_ACCELERATION_HANDLE,
     SENSOR_TYPE_ACCELEROMETER, GRAVITY_EARTH * ACCEL_FSR, GRAVITY_EARTH * ACCEL_FSR / MAX_LSB_DATA, 0.4f, 5000, 0, 0,
     "android.sensor.accelerometer", "", MAX_DELAY_US, SENSOR_FLAG_CONTINUOUS_MODE, {}},
#ifdef COMPASS_SUPPORT
    {"Invensense Magnetometer Uncalibrated", "Invensense", 1,
     SENSORS_RAW_MAGNETIC_FIELD_HANDLE,
     SENSOR_TYPE_MAGNETIC_FIELD_UNCALIBRATED, 10240.0f, 1.0f, 0.5f, 20000, 0, 0,
     "android.sensor.magnetic_field_uncalibrated", "", 250000, SENSOR_FLAG_CONTINUOUS_MODE, {}},
#endif
};
#endif

struct sensor_t *currentSensorList;

MPLSensor::MPLSensor(CompassSensor *compass) :
    mEnabled(0),
    mIIOReadSize(0),
    mPollTime(-1),
    mGyroSensorPrevTimestamp(0),
    mAccelSensorPrevTimestamp(0),
    mCompassPrevTimestamp(0)
{

    VFUNC_LOG;

    int i;

    mCompassSensor = compass;

    LOGV_IF(PROCESS_VERBOSE,
            "HAL:MPLSensor constructor : NumSensors = %d", TotalNumSensors);

    pthread_mutex_init(&mHALMutex, NULL);
    memset(mGyroOrientationMatrix, 0, sizeof(mGyroOrientationMatrix));
    memset(mAccelOrientationMatrix, 0, sizeof(mAccelOrientationMatrix));
    memset(mCompassOrientationMatrix, 0, sizeof(mCompassOrientationMatrix));
    mFlushSensorEnabledVector.resize(TotalNumSensors);
    memset(mEnabledTime, 0, sizeof(mEnabledTime));
#ifdef BATCH_MODE_SUPPORT
    mBatchEnabled = 0;
    for (int i = 0; i < TotalNumSensors; i++)
        mBatchTimeouts[i] = 100000000000LL;
    mBatchTimeoutInMs = 0;
#endif

    /* setup sysfs paths */
    initSysfsAttr();

    /* get chip name */
    if (inv_get_chip_name(mChipId) != INV_SUCCESS) {
        LOGE("HAL:ERR Failed to get chip ID\n");
        mChipDetected = false;
    } else {
        LOGI("HAL:Chip ID = %s\n", mChipId);
        mChipDetected = true;
    }

    /* print software version string */
    LOGI("HAL:InvenSense Sensors HAL version MA-%d.%d.%d%s\n",
         INV_SENSORS_HAL_VERSION_MAJOR, INV_SENSORS_HAL_VERSION_MINOR,
         INV_SENSORS_HAL_VERSION_PATCH, INV_SENSORS_HAL_VERSION_SUFFIX);
#ifdef BATCH_MODE_SUPPORT
    LOGI("HAL:Batch mode support : yes\n");
#else
    LOGI("HAL:Batch mode support : no\n");
#endif

    /* enable iio */
    enableIIOSysfs();

    /* setup orientation matrix */
    setDeviceProperties();

    /* initialize sensor data */
    memset(mPendingEvents, 0, sizeof(mPendingEvents));
    mPendingEvents[RawGyro].version = sizeof(sensors_event_t);
    mPendingEvents[RawGyro].sensor = ID_RG;
    mPendingEvents[RawGyro].type = SENSOR_TYPE_GYROSCOPE_UNCALIBRATED;
    mPendingEvents[RawGyro].gyro.status = SENSOR_STATUS_UNRELIABLE;
    mPendingEvents[Accelerometer].version = sizeof(sensors_event_t);
    mPendingEvents[Accelerometer].sensor = ID_A;
    mPendingEvents[Accelerometer].type = SENSOR_TYPE_ACCELEROMETER;
    mPendingEvents[Accelerometer].acceleration.status
        = SENSOR_STATUS_UNRELIABLE;
    mPendingEvents[RawMagneticField].version = sizeof(sensors_event_t);
    mPendingEvents[RawMagneticField].sensor = ID_RM;
    mPendingEvents[RawMagneticField].type = SENSOR_TYPE_MAGNETIC_FIELD_UNCALIBRATED;
    mPendingEvents[RawMagneticField].magnetic.status =
        SENSOR_STATUS_UNRELIABLE;

    /* Event Handlers */
    mHandlers[RawGyro] = &MPLSensor::rawGyroHandler;
    mHandlers[Accelerometer] = &MPLSensor::accelHandler;
    mHandlers[RawMagneticField] = &MPLSensor::rawCompassHandler;

    /* initialize delays to reasonable values */
    for (i = 0; i < TotalNumSensors; i++) {
        mDelays[i] = NS_PER_SECOND;
    }

    /* disable all sensors */
    enableGyro(0);
    enableAccel(0);
    enableCompass(0);

    /* FIFO high resolution mode */
    /* This needs to be set before setting FSR */
#ifdef FIFO_HIGH_RES_ENABLE
    write_sysfs_int(mpu.high_res_mode, 1);
    LOGI("HAL:FIFO High resolution enabled");
#else
    write_sysfs_int(mpu.high_res_mode, 0);
#endif

    /* set accel FSR */
    write_sysfs_int(mpu.accel_fsr, ACCEL_FSR_SYSFS);
    read_sysfs_int(mpu.accel_fsr, &mAccelFsrGee); /* read actual fsr */

    /* set gyro FSR */
    write_sysfs_int(mpu.gyro_fsr, GYRO_FSR_SYSFS);
    read_sysfs_int(mpu.gyro_fsr, &mGyroFsrDps); /* read actual fsr */

#ifdef BATCH_MODE_SUPPORT
    /* reset batch timeout */
    setBatchTimeout(0);
#endif
}

void MPLSensor::enableIIOSysfs(void)
{
    VFUNC_LOG;

    char iio_device_node[MAX_CHIP_ID_LEN];
    FILE *tempFp = NULL;
    int err;

    LOGV_IF(SYSFS_VERBOSE, "HAL:sysfs:echo 1 > %s (%" PRId64 ")",
            mpu.in_timestamp_en, getTimestamp());
    tempFp = fopen(mpu.in_timestamp_en, "w");
    if (tempFp == NULL) {
        LOGE("HAL:could not open timestamp enable");
    } else {
        err = fprintf(tempFp, "%d", 1);
        if (err < 0) {
            LOGE("HAL:could not write timestamp enable, %d", err);
        }
        err = fclose(tempFp);
        if (err) {
            LOGE("HAL:could not close write timestamp enable, %d", err);
        }
    }

    LOGV_IF(SYSFS_VERBOSE, "HAL:sysfs:echo %d > %s (%" PRId64 ")",
            IIO_BUFFER_LENGTH, mpu.buffer_length, getTimestamp());
    tempFp = fopen(mpu.buffer_length, "w");
    if (tempFp == NULL) {
        LOGE("HAL:could not open buffer length");
    } else {
        err = fprintf(tempFp, "%d", IIO_BUFFER_LENGTH);
        if (err < 0) {
            LOGE("HAL:could not write buffer length, %d", err);
        }
        err = fclose(tempFp);
        if (err) {
            LOGE("HAL:could not close write buffer length, %d", err);
        }
    }

    LOGV_IF(SYSFS_VERBOSE, "HAL:sysfs:echo %d > %s (%" PRId64 ")",
            1, mpu.chip_enable, getTimestamp());
    tempFp = fopen(mpu.chip_enable, "w");
    if (tempFp == NULL) {
        LOGE("HAL:could not open chip enable");
    } else {
        if ((err = fprintf(tempFp, "%d", 1)) < 0) {
            LOGE("HAL:could not write chip enable, %d", err);
         } else if ((err = fclose(tempFp)) < 0) {
            LOGE("HAL:could not close chip enable, %d", err);
        }
    }

    inv_get_iio_device_node(iio_device_node);
    mIIOfd = open(iio_device_node, O_RDONLY);
    if (mIIOfd < 0) {
        LOGE("HAL:could not open iio device node");
    } else {
        LOGV_IF(PROCESS_VERBOSE, "HAL:iio opened : %d", mIIOfd);
    }
}

void MPLSensor::setDeviceProperties(void)
{
    VFUNC_LOG;

    /* gyro/accel mount matrix */
    getSensorsOrientation();
    if (mCompassSensor) {
        /* compass mount matrix */
        mCompassSensor->getOrientationMatrix(mCompassOrientationMatrix);
    }
}

void MPLSensor::getSensorsOrientation(void)
{
    VFUNC_LOG;

    FILE *fptr;

    // get gyro orientation
    LOGV_IF(SYSFS_VERBOSE,
            "HAL:sysfs:cat %s (%" PRId64 ")", mpu.gyro_orient, getTimestamp());
    fptr = fopen(mpu.gyro_orient, "r");
    if (fptr != NULL) {
        int om[9];
        if (fscanf(fptr, "%d,%d,%d,%d,%d,%d,%d,%d,%d",
                    &om[0], &om[1], &om[2], &om[3], &om[4], &om[5],
                    &om[6], &om[7], &om[8]) < 0 || fclose(fptr) < 0) {
            LOGE("HAL:Could not read gyro mounting matrix");
        } else {
            LOGV_IF(PROCESS_VERBOSE,
                    "HAL:gyro mounting matrix: "
                    "%+d %+d %+d %+d %+d %+d %+d %+d %+d",
                    om[0], om[1], om[2], om[3], om[4], om[5], om[6], om[7], om[8]);

            mGyroOrientationMatrix[0] = om[0];
            mGyroOrientationMatrix[1] = om[1];
            mGyroOrientationMatrix[2] = om[2];
            mGyroOrientationMatrix[3] = om[3];
            mGyroOrientationMatrix[4] = om[4];
            mGyroOrientationMatrix[5] = om[5];
            mGyroOrientationMatrix[6] = om[6];
            mGyroOrientationMatrix[7] = om[7];
            mGyroOrientationMatrix[8] = om[8];
        }
    }

    // get accel orientation
    LOGV_IF(SYSFS_VERBOSE,
            "HAL:sysfs:cat %s (%" PRId64 ")", mpu.accel_orient, getTimestamp());
    fptr = fopen(mpu.accel_orient, "r");
    if (fptr != NULL) {
        int om[9];
        if (fscanf(fptr, "%d,%d,%d,%d,%d,%d,%d,%d,%d",
                    &om[0], &om[1], &om[2], &om[3], &om[4], &om[5],
                    &om[6], &om[7], &om[8]) < 0 || fclose(fptr) < 0) {
            LOGE("HAL:could not read accel mounting matrix");
        } else {
            LOGV_IF(PROCESS_VERBOSE,
                    "HAL:accel mounting matrix: "
                    "%+d %+d %+d %+d %+d %+d %+d %+d %+d",
                    om[0], om[1], om[2], om[3], om[4], om[5], om[6], om[7], om[8]);

            mAccelOrientationMatrix[0] = om[0];
            mAccelOrientationMatrix[1] = om[1];
            mAccelOrientationMatrix[2] = om[2];
            mAccelOrientationMatrix[3] = om[3];
            mAccelOrientationMatrix[4] = om[4];
            mAccelOrientationMatrix[5] = om[5];
            mAccelOrientationMatrix[6] = om[6];
            mAccelOrientationMatrix[7] = om[7];
            mAccelOrientationMatrix[8] = om[8];
        }
    }
}

MPLSensor::~MPLSensor()
{
    VFUNC_LOG;

    if (mIIOfd > 0)
        close(mIIOfd);
}

void MPLSensor::writeRateSysfs(int64_t period_ns, char *sysfs_rate)
{
    write_sysfs_int(sysfs_rate, NS_PER_SECOND_FLOAT / period_ns);
}

void MPLSensor::setGyroRate(int64_t period_ns)
{
    writeRateSysfs(period_ns, mpu.gyro_rate);
}

void MPLSensor::setAccelRate(int64_t period_ns)
{
    writeRateSysfs(period_ns, mpu.accel_rate);
}

void MPLSensor::setMagRate(int64_t period_ns)
{
    if (mCompassSensor)
        mCompassSensor->setDelay(ID_RM, period_ns);
}

#ifdef BATCH_MODE_SUPPORT
void MPLSensor::setBatchTimeout(int64_t timeout_ns)
{
    int timeout_ms = (int)(timeout_ns / 1000000LL);

    LOGV_IF(SYSFS_VERBOSE, "HAL:sysfs:echo %d > %s (%" PRId64 ")",
            timeout_ms, mpu.batchmode_timeout, getTimestamp());
    write_sysfs_int(mpu.batchmode_timeout, timeout_ms);
    mBatchTimeoutInMs = timeout_ms;
}

void MPLSensor::updateBatchTimeout(void)
{
    int64_t batchingTimeout = 100000000000LL;
    int64_t ns = 0;

    if (mBatchEnabled) {
        for (uint32_t i = 0; i < TotalNumSensors; i++) {
            if (mEnabled & (1LL << i)) {
                if (mBatchEnabled & (1LL << i))
                    ns = mBatchTimeouts[i];
                else
                    ns = 0;
                batchingTimeout = (ns < batchingTimeout) ? ns : batchingTimeout;
            }
        }
    } else {
        batchingTimeout = 0;
    }
    if (mBatchTimeoutInMs != batchingTimeout) {
        setBatchTimeout(batchingTimeout);
    }
}
#endif

int MPLSensor::enableGyro(int en)
{
    VFUNC_LOG;

    int res = 0;

    LOGV_IF(SYSFS_VERBOSE, "HAL:sysfs:echo %d > %s (%" PRId64 ")",
            en, mpu.gyro_fifo_enable, getTimestamp());
    res += write_sysfs_int(mpu.gyro_fifo_enable, en);

    return res;
}

int MPLSensor::enableAccel(int en)
{
    VFUNC_LOG;

    int res = 0;

    LOGV_IF(SYSFS_VERBOSE, "HAL:sysfs:echo %d > %s (%" PRId64 ")",
            en, mpu.accel_fifo_enable, getTimestamp());
    res += write_sysfs_int(mpu.accel_fifo_enable, en);

    return res;
}

int MPLSensor::enableCompass(int en)
{
    VFUNC_LOG;

    int res = 0;

    if (mCompassSensor)
        res = mCompassSensor->enable(ID_RM, en);

    return res;
}

int MPLSensor::enable(int32_t handle, int en)
{
    VFUNC_LOG;

    std::string sname;
    int what;
    int err = 0;

    /* exit if no chip is connected */
    if (!mChipDetected)
        return -EINVAL;

    getHandle(handle, what, sname);
    if (what < 0) {
        LOGV_IF(PROCESS_VERBOSE, "HAL:can't find handle %d",handle);
        return -EINVAL;
    }
#ifdef BATCH_MODE_SUPPORT
    if (!en)
        mBatchEnabled &= ~(1LL << what);
#endif
    if (mEnabled == 0) {
        // reset buffer
        mIIOReadSize = 0;
    }

    LOGV_IF(PROCESS_VERBOSE, "HAL:handle = %d en = %d", handle, en);

    uint64_t newState = en ? 1 : 0;

    LOGV_IF(PROCESS_VERBOSE, "HAL:enable - sensor %s (handle %d) %s -> %s",
            sname.c_str(),
            handle,
            ((mEnabled & (1LL << what)) ? "en" : "dis"),
            (((newState) << what) ? "en" : "dis"));
    LOGV_IF(PROCESS_VERBOSE, "HAL:%s sensor state change what=%d",
            sname.c_str(),
            what);

    if (((newState) << what) != (mEnabled & (1LL << what))) {
        uint64_t flags = newState;

        mEnabled &= ~(1LL << what);
        mEnabled |= (uint64_t(flags) << what);

        switch (what) {
            case RawGyro:
                enableGyro(en);
                break;
            case Accelerometer:
                enableAccel(en);
                break;
            case RawMagneticField:
                enableCompass(en);
                break;
        }
        if (en)
            mEnabledTime[what] = getTimestamp();
        else
            mEnabledTime[what] = 0;
    }

#ifdef BATCH_MODE_SUPPORT
    updateBatchTimeout();
#endif

    return err;
}

/*  these handlers transform mpl data into one of the Android sensor types */
int MPLSensor::rawGyroHandler(sensors_event_t* s)
{
    VHANDLER_LOG;

    int update = 0;
    int data[3];
    int i;
    float scale = (float)mGyroFsrDps / MAX_LSB_DATA * M_PI / 180;

    /* convert to body frame */
    for (i = 0; i < 3 ; i++) {
        data[i] = mCachedGyroData[0] * mGyroOrientationMatrix[i * 3] +
                  mCachedGyroData[1] * mGyroOrientationMatrix[i * 3 + 1] +
                  mCachedGyroData[2] * mGyroOrientationMatrix[i * 3 + 2];
    }

    for (i = 0; i < 3 ; i++) {
        s->uncalibrated_gyro.uncalib[i] = (float)data[i] * scale;
        s->uncalibrated_gyro.bias[i] = 0;
    }

    s->timestamp = mGyroSensorTimestamp;
    s->gyro.status = SENSOR_STATUS_UNRELIABLE;

    /* timestamp check */
    if ((mGyroSensorTimestamp > mGyroSensorPrevTimestamp) &&
        (mGyroSensorTimestamp > mEnabledTime[RawGyro])) {
        update = 1;
    }

    mGyroSensorPrevTimestamp = mGyroSensorTimestamp;

    LOGV_IF(HANDLER_DATA, "HAL:raw gyro data : %+f %+f %+f -- %" PRId64 " - %d",
        s->uncalibrated_gyro.uncalib[0], s->uncalibrated_gyro.uncalib[1], s->uncalibrated_gyro.uncalib[2],
        s->timestamp, update);

    return update;
}

int MPLSensor::accelHandler(sensors_event_t* s)
{
    VHANDLER_LOG;

    int update = 0;
    int data[3];
    int i;
    float scale = 1.f / (MAX_LSB_DATA / (float)mAccelFsrGee) * 9.80665f;

    /* convert to body frame */
    for (i = 0; i < 3 ; i++) {
        data[i] = mCachedAccelData[0] * mAccelOrientationMatrix[i * 3] +
                  mCachedAccelData[1] * mAccelOrientationMatrix[i * 3 + 1] +
                  mCachedAccelData[2] * mAccelOrientationMatrix[i * 3 + 2];
    }
    for (i = 0; i < 3 ; i++) {
        s->acceleration.v[i] = (float)data[i] * scale;
    }
    s->timestamp = mAccelSensorTimestamp;
    s->acceleration.status = SENSOR_STATUS_UNRELIABLE;

    /*timestamp check */
    if ((mAccelSensorTimestamp > mAccelSensorPrevTimestamp) &&
        (mAccelSensorTimestamp > mEnabledTime[Accelerometer])) {
        update = 1;
    }

    mAccelSensorPrevTimestamp = mAccelSensorTimestamp;

    LOGV_IF(HANDLER_DATA, "HAL:accel data : %+f %+f %+f -- %" PRId64 " - %d",
        s->acceleration.v[0], s->acceleration.v[1], s->acceleration.v[2],
        s->timestamp, update);

    return update;
}

int MPLSensor::rawCompassHandler(sensors_event_t* s)
{
    VHANDLER_LOG;

    int update = 0;
    int data[3];
    int i;
    float scale = 1.f / (1 << 16); // 1uT for 2^16

    /* convert to body frame */
    for (i = 0; i < 3 ; i++) {
        data[i] = (mCachedCompassData[0]) * mCompassOrientationMatrix[i * 3] +
                  (mCachedCompassData[1]) * mCompassOrientationMatrix[i * 3 + 1] +
                  (mCachedCompassData[2]) * mCompassOrientationMatrix[i * 3 + 2];
    }

    for (i = 0; i < 3 ; i++) {
        s->uncalibrated_magnetic.uncalib[i] = (float)data[i] * scale;
        s->uncalibrated_magnetic.bias[i] = 0;
    }

    s->timestamp = mCompassTimestamp;
    s->magnetic.status = SENSOR_STATUS_UNRELIABLE;

    /* timestamp check */
    if ((mCompassTimestamp > mCompassPrevTimestamp) &&
        (mCompassTimestamp > mEnabledTime[RawMagneticField])) {
        update = 1;
    }

    mCompassPrevTimestamp = mCompassTimestamp;

    LOGV_IF(HANDLER_DATA, "HAL:raw compass data: %+f %+f %+f %d -- %" PRId64 " - %d",
        s->uncalibrated_magnetic.uncalib[0], s->uncalibrated_magnetic.uncalib[1], s->uncalibrated_magnetic.uncalib[2],
        s->magnetic.status, s->timestamp, update);

    return update;
}

int MPLSensor::metaHandler(sensors_event_t* s, int flags)
{
    VHANDLER_LOG;
    int update = 1;

    /* initalize SENSOR_TYPE_META_DATA */
    s->version = META_DATA_VERSION;
    s->sensor = 0;
    s->reserved0 = 0;
    s->timestamp = 0LL;

    switch(flags) {
        case META_DATA_FLUSH_COMPLETE:
            s->type = SENSOR_TYPE_META_DATA;
            s->meta_data.what = flags;
            s->meta_data.sensor = mFlushSensorEnabledVector[0];

            pthread_mutex_lock(&mHALMutex);
            mFlushSensorEnabledVector.erase(mFlushSensorEnabledVector.begin());
            pthread_mutex_unlock(&mHALMutex);
            LOGV_IF(HANDLER_DATA,
                    "HAL:flush complete data: type=%d what=%d, "
                    "sensor=%d - %" PRId64 " - %d",
                    s->type, s->meta_data.what, s->meta_data.sensor,
                    s->timestamp, update);
            break;

        default:
            LOGW("HAL: Meta flags not supported");
            break;
    }

    return update;
}

void MPLSensor::getHandle(int32_t handle, int &what, std::string &sname)
{
    VFUNC_LOG;

    what = -1;

    if (handle >= ID_NUMBER) {
        LOGV_IF(PROCESS_VERBOSE, "HAL:handle over = %d",handle);
        return;
    }
    switch (handle) {
        case ID_RG:
            what = RawGyro;
            sname = "RawGyro";
            break;
        case ID_A:
            what = Accelerometer;
            sname = "Accelerometer";
            break;
        case ID_RM:
            what = RawMagneticField;
            sname = "RawMagneticField";
            break;
        default:
            what = handle;
            sname = "Others";
            break;
    }
    LOGI_IF(PROCESS_VERBOSE, "HAL:getHandle - what=%d, sname=%s",
            what,
            sname.c_str()
            );
    return;
}

int MPLSensor::readEvents(sensors_event_t* data, int count)
{
    VHANDLER_LOG;

    int numEventReceived = 0;

    // handle flush complete event
    if(!mFlushSensorEnabledVector.empty()) {
        sensors_event_t temp;
        int sendEvent = metaHandler(&temp, META_DATA_FLUSH_COMPLETE);
        if(sendEvent == 1 && count > 0) {
            *data++ = temp;
            count--;
            numEventReceived++;
        }
    }

    for (int i = 0; i < ID_NUMBER; i++) {
        int update = 0;
        if (mEnabled & (1LL << i)) {
            update = CALL_MEMBER_FN(this, mHandlers[i])(mPendingEvents + i);
            if (update && (count > 0)) {
                *data++ = mPendingEvents[i];
                count--;
                numEventReceived++;
            }
        }
    }

    return numEventReceived;
}

int MPLSensor::readMpuEvents(sensors_event_t* s, int count)
{
    VHANDLER_LOG;

    unsigned short header;
    char *rdata;
    int rsize;
    int sensor;
    int ptr = 0;
    int numEventReceived = 0;
    int left_over = 0;
    bool data_found;

    if (mEnabled == 0) {
        /* no sensor is enabled. read out all leftover */
        rsize = read(mIIOfd, mIIOReadBuffer, sizeof(mIIOReadBuffer));
        mIIOReadSize = 0;
        return 0;
    }

    if (mCompassSensor)
        count -= COMPASS_SEN_EVENT_RESV_SZ;

    /* read as much data as possible allowed with either
     * smaller, the buffer from upper layer or local buffer */
    int nbytes = sizeof(mIIOReadBuffer) - mIIOReadSize;
    /* assume that gyro and accel data packet size are the same
     * and larger than marker packet */
    int packet_size = DATA_FORMAT_RAW_GYRO_SZ;
    if (nbytes > count * packet_size) {
        nbytes = count * packet_size;
    }
    rsize = read(mIIOfd, &mIIOReadBuffer[mIIOReadSize], nbytes);
    LOGV_IF(PROCESS_VERBOSE, "HAL: nbytes=%d rsize=%d", nbytes, rsize);
    if (rsize < 0) {
        LOGE("HAL:failed to read IIO.  nbytes=%d rsize=%d", nbytes, rsize);
        return 0;
    }
    if (rsize == 0) {
        LOGI("HAL:no data from IIO.");
        return 0;
    }

    mIIOReadSize += rsize;

    while (ptr < mIIOReadSize) {
        rdata = &mIIOReadBuffer[ptr];
        header = *(unsigned short*)rdata;
        data_found = false;
        switch (header) {
            case DATA_FORMAT_MARKER:
                if ((mIIOReadSize - ptr) < DATA_FORMAT_MARKER_SZ) {
                    left_over = mIIOReadSize - ptr;
                    break;
                }
                sensor = *((int *) (rdata + 4));
                mFlushSensorEnabledVector.push_back(sensor);
                LOGV_IF(INPUT_DATA, "HAL:MARKER DETECTED what:%d", sensor);
                ptr += DATA_FORMAT_MARKER_SZ;
                data_found = true;
                break;
            case DATA_FORMAT_EMPTY_MARKER:
                if ((mIIOReadSize - ptr) < DATA_FORMAT_EMPTY_MARKER_SZ) {
                    left_over = mIIOReadSize - ptr;
                    break;
                }
                sensor = *((int *) (rdata + 4));
                mFlushSensorEnabledVector.push_back(sensor);
                LOGV_IF(INPUT_DATA, "HAL:EMPTY MARKER DETECTED what:%d", sensor);
                ptr += DATA_FORMAT_EMPTY_MARKER_SZ;
                data_found = true;
                break;
            case DATA_FORMAT_RAW_GYRO:
                if ((mIIOReadSize - ptr) < DATA_FORMAT_RAW_GYRO_SZ) {
                    left_over = mIIOReadSize - ptr;
                    break;
                }
                mCachedGyroData[0] = *((int *) (rdata + 4));
                mCachedGyroData[1] = *((int *) (rdata + 8));
                mCachedGyroData[2] = *((int *) (rdata + 12));
                mGyroSensorTimestamp = *((long long*) (rdata + 16));
                LOGV_IF(INPUT_DATA, "HAL:RAW GYRO DETECTED:0x%x : %d %d %d -- %" PRId64,
                        header,
                        mCachedGyroData[0], mCachedGyroData[1], mCachedGyroData[2],
                        mGyroSensorTimestamp);
                ptr += DATA_FORMAT_RAW_GYRO_SZ;
                data_found = true;
                break;
            case DATA_FORMAT_ACCEL:
                if ((mIIOReadSize - ptr) < DATA_FORMAT_ACCEL_SZ) {
                    left_over = mIIOReadSize - ptr;
                    break;
                }
                mCachedAccelData[0] = *((int *) (rdata + 4));
                mCachedAccelData[1] = *((int *) (rdata + 8));
                mCachedAccelData[2] = *((int *) (rdata + 12));
                mAccelSensorTimestamp = *((long long*) (rdata +16));
                LOGV_IF(INPUT_DATA, "HAL:ACCEL DETECTED:0x%x : %d %d %d -- %" PRId64,
                        header,
                        mCachedAccelData[0], mCachedAccelData[1], mCachedAccelData[2],
                        mAccelSensorTimestamp);
                ptr += DATA_FORMAT_ACCEL_SZ;
                data_found = true;
                break;
            default:
                LOGW("HAL:no header.");
                ptr++;
                data_found = false;
                break;
        }

        if (data_found) {
            int num = readEvents(&s[numEventReceived], count);
            if (num > 0) {
                count -= num;
                numEventReceived += num;
                if (count == 0)
                    break;
                if (count < 0) {
                    LOGW("HAL:sensor_event_t buffer overflow");
                    break;
                }
            }
        }
        if (left_over) {
            break;
        }
    }

    if (left_over > 0) {
        LOGV_IF(PROCESS_VERBOSE, "HAL: leftover mIIOReadSize=%d ptr=%d",
                mIIOReadSize, ptr);
        memmove(mIIOReadBuffer, &mIIOReadBuffer[ptr], left_over);
        mIIOReadSize = left_over;
    } else {
        mIIOReadSize = 0;
    }

    return numEventReceived;
}

int MPLSensor::readCompassEvents(sensors_event_t* s, int count)
{
    VHANDLER_LOG;

    int numEventReceived = 0;

    if (count > COMPASS_SEN_EVENT_RESV_SZ)
        count = COMPASS_SEN_EVENT_RESV_SZ;

    if (mCompassSensor) {
        mCompassSensor->readSample(mCachedCompassData, &mCompassTimestamp, 3);
        int num = readEvents(&s[numEventReceived], count);
        if (num > 0) {
            count -= num;
            numEventReceived += num;
            if (count < 0)
                LOGW("HAL:sensor_event_t buffer overflow");
        }
    }
    return numEventReceived;
}

int MPLSensor::getFd(void) const
{
    VFUNC_LOG;
    LOGV_IF(PROCESS_VERBOSE, "getFd returning %d", mIIOfd);
    return mIIOfd;
}

int MPLSensor::getCompassFd(void) const
{
    VFUNC_LOG;
    int fd = 0;
    if (mCompassSensor)
        fd = mCompassSensor->getFd();
    LOGV_IF(PROCESS_VERBOSE, "getCompassFd returning %d", fd);
    return fd;
}

int MPLSensor::getPollTime(void)
{
    VFUNC_LOG;
    return mPollTime;
}

/** fill in the sensor list based on which sensors are configured.
 *  return the number of configured sensors.
 *  parameter list must point to a memory region of at least 7*sizeof(sensor_t)
 *  parameter len gives the length of the buffer pointed to by list
 */
int MPLSensor::populateSensorList(struct sensor_t *list, int len)
{
    VFUNC_LOG;

    int listSize;

    currentSensorList = sRawSensorList;
    listSize = sizeof(sRawSensorList);
    LOGI("The sensor list for raw data only is used");

    if(len < (int)((listSize / sizeof(sensor_t)) * sizeof(sensor_t))) {
        LOGE("HAL:sensor list too small, not populating.");
        return -(listSize / sizeof(sensor_t));
    }

    mNumSensors = listSize / sizeof(sensor_t);

    /* fill in the base values */
    memcpy(list, currentSensorList, sizeof (struct sensor_t) * mNumSensors);
#ifdef COMPASS_SUPPORT
    if (mCompassSensor)
        mCompassSensor->fillList(&list[ID_RM]);
#endif

    return mNumSensors;
}

int MPLSensor::initSysfsAttr(void)
{
    VFUNC_LOG;

    unsigned char i = 0;
    char sysfs_path[MAX_SYSFS_NAME_LEN];
    char *sptr;
    char **dptr;

    memset(sysfs_path, 0, sizeof(sysfs_path));

    sysfs_names_ptr =
        (char*)malloc(sizeof(char[MAX_SYSFS_ATTRB][MAX_SYSFS_NAME_LEN]));
    sptr = sysfs_names_ptr;
    if (sptr != NULL) {
        dptr = (char**)&mpu;
        do {
            *dptr++ = sptr;
            memset(sptr, 0, sizeof(char));
            sptr += sizeof(char[MAX_SYSFS_NAME_LEN]);
        } while (++i < MAX_SYSFS_ATTRB);
    } else {
        LOGE("HAL:couldn't alloc mem for sysfs paths");
        return -1;
    }

    // get absolute IIO path & build MPU's sysfs paths
    inv_get_sysfs_path(sysfs_path);

    memcpy(mSysfsPath, sysfs_path, sizeof(sysfs_path));
    sprintf(mpu.chip_enable, "%s%s", sysfs_path, "/buffer/enable");
    sprintf(mpu.buffer_length, "%s%s", sysfs_path, "/buffer/length");

    sprintf(mpu.in_timestamp_en, "%s%s", sysfs_path,
            "/scan_elements/in_timestamp_en");
    sprintf(mpu.in_timestamp_index, "%s%s", sysfs_path,
            "/scan_elements/in_timestamp_index");
    sprintf(mpu.in_timestamp_type, "%s%s", sysfs_path,
            "/scan_elements/in_timestamp_type");

    sprintf(mpu.self_test, "%s%s", sysfs_path, "/misc_self_test");

    /* gyro sysfs */
    sprintf(mpu.gyro_orient, "%s%s", sysfs_path, "/info_anglvel_matrix");
    sprintf(mpu.gyro_fifo_enable, "%s%s", sysfs_path, "/in_anglvel_enable");
    sprintf(mpu.gyro_fsr, "%s%s", sysfs_path, "/in_anglvel_scale");
    sprintf(mpu.gyro_sf, "%s%s", sysfs_path, "/info_gyro_sf");
    sprintf(mpu.gyro_rate, "%s%s", sysfs_path, "/in_anglvel_rate");
    sprintf(mpu.gyro_wake_fifo_enable, "%s%s", sysfs_path, "/in_anglvel_wake_enable");
    sprintf(mpu.gyro_wake_rate, "%s%s", sysfs_path, "/in_anglvel_wake_rate");

    /* accel sysfs */
    sprintf(mpu.accel_orient, "%s%s", sysfs_path, "/info_accel_matrix");
    sprintf(mpu.accel_fifo_enable, "%s%s", sysfs_path, "/in_accel_enable");
    sprintf(mpu.accel_rate, "%s%s", sysfs_path, "/in_accel_rate");
    sprintf(mpu.accel_fsr, "%s%s", sysfs_path, "/in_accel_scale");
    sprintf(mpu.accel_wake_fifo_enable, "%s%s", sysfs_path, "/in_accel_wake_enable");
    sprintf(mpu.accel_wake_rate, "%s%s", sysfs_path, "/in_accel_wake_rate");

    /* accel offset */
    sprintf(mpu.in_accel_x_offset, "%s%s", sysfs_path, "/in_accel_x_offset");
    sprintf(mpu.in_accel_y_offset, "%s%s", sysfs_path, "/in_accel_y_offset");
    sprintf(mpu.in_accel_z_offset, "%s%s", sysfs_path, "/in_accel_z_offset");

    /* gyro offset */
    sprintf(mpu.in_gyro_x_offset, "%s%s", sysfs_path, "/in_anglvel_x_offset");
    sprintf(mpu.in_gyro_y_offset, "%s%s", sysfs_path, "/in_anglvel_y_offset");
    sprintf(mpu.in_gyro_z_offset, "%s%s", sysfs_path, "/in_anglvel_z_offset");

    /* batch and flush */
    sprintf(mpu.batchmode_timeout, "%s%s", sysfs_path,
            "/misc_batchmode_timeout");
    sprintf(mpu.flush_batch, "%s%s", sysfs_path,
            "/misc_flush_batch");

    /* FIFO high resolution mode */
    sprintf(mpu.high_res_mode, "%s%s", sysfs_path, "/in_high_res_mode");

    return 0;
}

int MPLSensor::batch(int handle, int flags, int64_t period_ns, int64_t timeout)
{
    VFUNC_LOG;

    uint32_t i;
    int list_index = 0;
    std::string sname;
    int what = -1;

    /* exit if no chip is connected */
    if (!mChipDetected)
        return -EINVAL;

    LOGI_IF(PROCESS_VERBOSE,
            "HAL:batch called - handle=%d, flags=%d, period=%" PRId64 ", timeout=%" PRId64,
            handle, flags, period_ns, timeout);

    /* check if the handle is valid */
    getHandle(handle, what, sname);
    if(what < 0) {
        LOGE("HAL:batch sensors %d not found", handle);
        return -EINVAL;
    }

    /* check if we can support issuing interrupt before FIFO fills-up */
    /* in a given timeout.                                          */
    if (flags & SENSORS_BATCH_WAKE_UPON_FIFO_FULL) {
        LOGE("HAL: batch SENSORS_BATCH_WAKE_UPON_FIFO_FULL is not supported");
        return -EINVAL;
    }

    /* find sensor_t struct for this sensor */
    for (i = 0; i < mNumSensors; i++) {
        if (handle == currentSensorList[i].handle) {
            list_index = i;
            break;
        }
    }

    if (period_ns != currentSensorList[list_index].maxDelay * 1000) {
        /* Round up in Hz when requested frequency has fractional digit.
         * Note: not round up if requested frequency is the same as maxDelay */
        int period_ns_int;
        period_ns_int = (NS_PER_SECOND + (period_ns - 1))/ period_ns;
        period_ns = NS_PER_SECOND / period_ns_int;
    }

    if (period_ns > currentSensorList[list_index].maxDelay * 1000)
        period_ns = currentSensorList[list_index].maxDelay * 1000;
    if (period_ns < currentSensorList[list_index].minDelay * 1000)
        period_ns = currentSensorList[list_index].minDelay * 1000;

    /* just stream with no error return, if the sensor does not support batch mode */
    if (currentSensorList[list_index].fifoMaxEventCount != 0) {
        LOGV_IF(PROCESS_VERBOSE, "HAL: batch - select sensor (handle %d)", list_index);
    } else if (timeout > 0) {
        LOGV_IF(PROCESS_VERBOSE, "HAL: sensor (handle %d) does not support batch mode", list_index);
        timeout = 0;
    }

    /* return from here when dry run */
    if (flags & SENSORS_BATCH_DRY_RUN) {
        return 0;
    }

#ifdef BATCH_MODE_SUPPORT
    if (timeout == 0) {
        mBatchEnabled &= ~(1LL << what);
        mBatchTimeouts[what] = 100000000000LL;
    } else {
        mBatchEnabled |= (1LL << what);
        mBatchTimeouts[what] = timeout;
    }
    updateBatchTimeout();
#endif

    switch (what) {
        case RawGyro:
            setGyroRate(period_ns);
            break;
        case Accelerometer:
            setAccelRate(period_ns);
            break;
        case RawMagneticField:
            setMagRate(period_ns);
            break;
    }
    return 0;
}

int MPLSensor::flush(int handle)
{
    VFUNC_LOG;

    std::string sname;
    int what = -1;

    /* exit if no chip is connected */
    if (!mChipDetected)
        return -EINVAL;

    getHandle(handle, what, sname);
    if (what < 0) {
        LOGE("HAL:flush - what=%d is invalid", what);
        return -EINVAL;
    }

    LOGV_IF(PROCESS_VERBOSE, "HAL: flush - select sensor %s (handle %d)",
            sname.c_str(),
            handle);

    /*write sysfs */
    LOGV_IF(SYSFS_VERBOSE, "HAL:sysfs:echo %d > %s (%" PRId64 ")",
            handle, mpu.flush_batch, getTimestamp());

    if (write_sysfs_int(mpu.flush_batch, handle) < 0) {
        LOGE("HAL:ERR can't write flush_batch");
    }

    return 0;
}

/**
 * @file osPortDevice.c
 *
 * Porting layer for device parameters
 *
 * Copyright (C) Sierra Wireless Inc.
 *
 */

#include <lwm2mcore/device.h>
#include <sys/reboot.h>
#include "legato.h"
#include "interfaces.h"
#include "assetData.h"
#include <sys/utsname.h>
#include "avcAppUpdate.h"

//--------------------------------------------------------------------------------------------------
/**
 * Define for LK version length
 */
//--------------------------------------------------------------------------------------------------
#define LK_VERSION_LENGTH   10

//--------------------------------------------------------------------------------------------------
/**
 * Define for FW version buffer length
 */
//--------------------------------------------------------------------------------------------------
#define FW_BUFFER_LENGTH    512

//--------------------------------------------------------------------------------------------------
/**
 * Define for unknown version
 */
//--------------------------------------------------------------------------------------------------
#define UNKNOWN_VERSION     "unknown"

//--------------------------------------------------------------------------------------------------
/**
 * Define for modem tag in FW version string
 */
//--------------------------------------------------------------------------------------------------
#define MODEM_TAG "MDM="

//--------------------------------------------------------------------------------------------------
/**
 * Define for LK tag in FW version string
 */
//--------------------------------------------------------------------------------------------------
#define LK_TAG ",LK="

//--------------------------------------------------------------------------------------------------
/**
 * Define for modem tag in Linux version string
 */
//--------------------------------------------------------------------------------------------------
#define LINUX_TAG ",OS="

//--------------------------------------------------------------------------------------------------
/**
 * Define for root FS tag in FW version string
 */
//--------------------------------------------------------------------------------------------------
#define ROOT_FS_TAG ",RFS="

//--------------------------------------------------------------------------------------------------
/**
 * Define for user FS tag in FW version string
 */
//--------------------------------------------------------------------------------------------------
#define USER_FS_TAG ",UFS="

//--------------------------------------------------------------------------------------------------
/**
 * Define for Legato tag in FW version string
 */
//--------------------------------------------------------------------------------------------------
#define LEGATO_TAG ",LE="

//--------------------------------------------------------------------------------------------------
/**
 * Define for Customer PRI tag in FW version string (per AirVantage bundle packages specification)
 */
//--------------------------------------------------------------------------------------------------
#define CUSTOMER_PRI_TAG ",CUPRI="

//--------------------------------------------------------------------------------------------------
/**
 * Define for Carrier PRI tag in FW version string
 */
//--------------------------------------------------------------------------------------------------
#define CARRIER_PRI_TAG ",CAPRI="

//--------------------------------------------------------------------------------------------------
/**
 * Define for MCU tag in FW version string
 */
//--------------------------------------------------------------------------------------------------
#define MCU_TAG ",MCU="

//--------------------------------------------------------------------------------------------------
 /**
 *  Path to the file that stores the Legato version number string.
 */
//--------------------------------------------------------------------------------------------------
#define LEGATO_VERSION_FILE "/legato/systems/current/version"

//--------------------------------------------------------------------------------------------------
 /**
 *  Path to the file that stores the LK version number string.
 */
//--------------------------------------------------------------------------------------------------
#define LK_VERSION_FILE "/proc/cmdline"

//--------------------------------------------------------------------------------------------------
 /**
 *  Path to the file that stores the root FS version number string.
 */
//--------------------------------------------------------------------------------------------------
#define RFS_VERSION_FILE "/etc/rootfsver.txt"

//--------------------------------------------------------------------------------------------------
 /**
 *  Path to the file that stores the user FS version number string.
 */
//--------------------------------------------------------------------------------------------------
#define UFS_VERSION_FILE "/opt/userfsver.txt"

//--------------------------------------------------------------------------------------------------
 /**
 *  String to be check in file which stores the LK version
 */
//--------------------------------------------------------------------------------------------------
#define LK_STRING_FILE "lkversion="

//--------------------------------------------------------------------------------------------------
 /**
 *  Define of space
 */
//--------------------------------------------------------------------------------------------------
#define SPACE " "

//--------------------------------------------------------------------------------------------------
/**
 * Timer used to launch the device reboot after the acknowledgment is sent to the server
 */
//--------------------------------------------------------------------------------------------------
static le_timer_Ref_t LaunchRebootTimer;

//--------------------------------------------------------------------------------------------------
/**
 * Function pointer to get a component version
 *
 * @return
 *      - written buffer length
 */
//--------------------------------------------------------------------------------------------------
typedef size_t (*getVersion_t)
(
    char* versionBufferPtr,         ///< [INOUT] Buffer to hold the string.
    size_t lenPtr                   ///< [IN] Buffer length
);

//--------------------------------------------------------------------------------------------------
/**
 * Structure to get a component version and its corresponding tag for the FW version string
 */
//--------------------------------------------------------------------------------------------------
typedef struct
{
    char* tagPtr;               ///< Component tag
    getVersion_t funcPtr;       ///< Function to read the component version
}ComponentVersion_t;

//--------------------------------------------------------------------------------------------------
/**
 * Attempt to read the Modem version string
 *
 * @return
 *      - written buffer length
 */
//--------------------------------------------------------------------------------------------------
static size_t GetModemVersion
(
    char* versionBufferPtr,         ///< [INOUT] Buffer to hold the string.
    size_t len                      ///< [IN] Buffer length
)
{
    size_t returnedLen = 0;
    if (NULL != versionBufferPtr)
    {
        char tmpModemBufferPtr[FW_BUFFER_LENGTH];
        if (LE_OK == le_info_GetFirmwareVersion(tmpModemBufferPtr, FW_BUFFER_LENGTH))
        {
            char* savePtr;
            char* tmpBufferPtr = strtok_r(tmpModemBufferPtr, SPACE, &savePtr);
            if (NULL != tmpBufferPtr)
            {
                returnedLen = snprintf(versionBufferPtr, len, "%s", tmpBufferPtr);
            }
            else
            {
                returnedLen = snprintf(versionBufferPtr, len, UNKNOWN_VERSION, strlen(UNKNOWN_VERSION));
            }
        }
        else
        {
            returnedLen = snprintf(versionBufferPtr, len, UNKNOWN_VERSION, strlen(UNKNOWN_VERSION));
        }
        LE_INFO("Modem version = %s, returnedLen %d", versionBufferPtr, returnedLen);
    }
    return returnedLen;
}

//--------------------------------------------------------------------------------------------------
/**
 * Attempt to read the LK version string from the file system.
 *
 * @return
 *      - written buffer length
 */
//--------------------------------------------------------------------------------------------------
static size_t GetLkVersion
(
    char* versionBufferPtr,         ///< [INOUT] Buffer to hold the string.
    size_t len                      ///< [IN] Buffer length
)
{
    size_t returnedLen = 0;
    if (NULL != versionBufferPtr)
    {
        char tmpLkBufferPtr[FW_BUFFER_LENGTH];
        char* tokenPtr;
        char* savePtr;
        FILE* fpPtr;
        fpPtr = fopen(LK_VERSION_FILE, "r");
        if ((NULL != fpPtr)
         && (NULL != fgets(tmpLkBufferPtr, FW_BUFFER_LENGTH, fpPtr)))
        {
            tokenPtr = strtok_r(tmpLkBufferPtr, SPACE, &savePtr);
            /* Look for "lkversion=" */
            while (NULL != tokenPtr)
            {
                tokenPtr = strtok_r(NULL, SPACE, &savePtr);
                if (NULL == tokenPtr)
                {
                    returnedLen = snprintf(versionBufferPtr,
                                           len,
                                           UNKNOWN_VERSION,
                                           strlen(UNKNOWN_VERSION));
                    break;
                }
                if (0 == strncmp(tokenPtr, LK_STRING_FILE, LK_VERSION_LENGTH))
                {
                    tokenPtr += LK_VERSION_LENGTH;
                    returnedLen = snprintf(versionBufferPtr, len, "%s", tokenPtr);
                    break;
                }
            }
        }
        else
        {
            returnedLen = snprintf(versionBufferPtr, len, UNKNOWN_VERSION, strlen(UNKNOWN_VERSION));
        }

        if (NULL != fpPtr)
        {
            fclose(fpPtr);
        }
        LE_INFO("lkVersion %s, len %d", versionBufferPtr, returnedLen);
    }
    return returnedLen;
}

//--------------------------------------------------------------------------------------------------
/**
 * Attempt to read the Linux version string from the file system.
 *
 * @return
 *      - written buffer length
 */
//--------------------------------------------------------------------------------------------------
static size_t GetOsVersion
(
    char* versionBufferPtr,         ///< [INOUT] Buffer to hold the string.
    size_t len                      ///< [IN] Buffer length
)
{
    size_t returnedLen = 0;
    if (NULL != versionBufferPtr)
    {
        struct utsname linuxInfo;
        if (0 == uname(&linuxInfo))
        {
            LE_INFO("Linux Version: %s", linuxInfo.release);
            returnedLen = snprintf(versionBufferPtr, len, linuxInfo.release, strlen(linuxInfo.release));
        }
        else
        {
            returnedLen = snprintf(versionBufferPtr, len, UNKNOWN_VERSION, strlen(UNKNOWN_VERSION));
        }
        LE_INFO("OsVersion %s, len %d", versionBufferPtr, returnedLen);
    }
    return returnedLen;
}

//--------------------------------------------------------------------------------------------------
/**
 * Attempt to read the root FS version string from the file system.
 *
 * @return
 *      - written buffer length
 */
//--------------------------------------------------------------------------------------------------
static size_t GetRfsVersion
(
    char* versionBufferPtr,         ///< [INOUT] Buffer to hold the string.
    size_t len                      ///< [IN] Buffer length
)
{
    size_t returnedLen = 0;
    if (NULL != versionBufferPtr)
    {
        char tmpRfsBufferPtr[FW_BUFFER_LENGTH];
        FILE* fpPtr;
        fpPtr = fopen(RFS_VERSION_FILE, "r");
        if ((NULL != fpPtr)
         && (NULL != fgets(tmpRfsBufferPtr, FW_BUFFER_LENGTH, fpPtr)))
        {
            char* savePtr;
            char* tmpBufferPtr = strtok_r(tmpRfsBufferPtr, SPACE, &savePtr);
            if (NULL != tmpBufferPtr)
            {
                returnedLen = snprintf(versionBufferPtr, len, "%s", tmpBufferPtr);
            }
            else
            {
                returnedLen = snprintf(versionBufferPtr, len, UNKNOWN_VERSION, strlen(UNKNOWN_VERSION));
            }
        }
        else
        {
            returnedLen = snprintf(versionBufferPtr, len, UNKNOWN_VERSION, strlen(UNKNOWN_VERSION));
        }

        if (NULL != fpPtr)
        {
            fclose(fpPtr);
        }
        LE_INFO("RfsVersion %s, len %d", versionBufferPtr, returnedLen);
    }
    return returnedLen;
}

//--------------------------------------------------------------------------------------------------
/**
 * Attempt to read the user FS version string from the file system.
 *
 * @return
 *      - written buffer length
 */
//--------------------------------------------------------------------------------------------------
static size_t GetUfsVersion
(
    char* versionBufferPtr,         ///< [INOUT] Buffer to hold the string.
    size_t len                      ///< [IN] Buffer length
)
{
    size_t returnedLen = 0;
    if (NULL != versionBufferPtr)
    {
        char tmpUfsBufferPtr[FW_BUFFER_LENGTH];
        FILE* fpPtr;
        fpPtr = fopen(UFS_VERSION_FILE, "r");
        if ((NULL != fpPtr)
         && (NULL != fgets(tmpUfsBufferPtr, FW_BUFFER_LENGTH, fpPtr)))
        {
            char* savePtr;
            char* tmpBufferPtr = strtok_r(tmpUfsBufferPtr, SPACE, &savePtr);
            if (NULL != tmpBufferPtr)
            {
                returnedLen = snprintf(versionBufferPtr, len, "%s", tmpBufferPtr);
            }
            else
            {
                returnedLen = snprintf(versionBufferPtr, len, UNKNOWN_VERSION, strlen(UNKNOWN_VERSION));
            }
        }
        else
        {
            returnedLen = snprintf(versionBufferPtr, len, UNKNOWN_VERSION, strlen(UNKNOWN_VERSION));
        }

        if (NULL != fpPtr)
        {
            fclose(fpPtr);
        }
        LE_INFO("UfsVersion %s, len %d", versionBufferPtr, returnedLen);
    }
    return returnedLen;
}

//--------------------------------------------------------------------------------------------------
/**
 * Attempt to read the Legato version string from the file system.
 *
 * @return
 *      - written buffer length
 */
//--------------------------------------------------------------------------------------------------
static size_t GetLegatoVersion
(
    char* versionBufferPtr,         ///< [INOUT] Buffer to hold the string.
    size_t len                      ///< [IN] Buffer length
)
{
    size_t returnedLen = 0;
    if (NULL != versionBufferPtr)
    {
        FILE* versionFilePtr = fopen(LEGATO_VERSION_FILE, "r");
        if (NULL == versionFilePtr)
        {
            LE_INFO("Could not open Legato version file.");
            returnedLen = snprintf(versionBufferPtr, len, UNKNOWN_VERSION, strlen(UNKNOWN_VERSION));
            return returnedLen;
        }

        char tmpLegatoVersionBuffer[MAX_VERSION_STR_BYTES];
        if (fgets(tmpLegatoVersionBuffer, MAX_VERSION_STR_BYTES, versionFilePtr) != NULL)
        {
            char* savePtr;
            char* tmpBufferPtr = strtok_r(tmpLegatoVersionBuffer, "-_", &savePtr);
            if (NULL != tmpBufferPtr)
            {
                snprintf(versionBufferPtr, len, "%s", tmpBufferPtr);
                returnedLen = strlen(versionBufferPtr);
            }
            else
            {
                snprintf(versionBufferPtr, len, UNKNOWN_VERSION, strlen(UNKNOWN_VERSION));
                returnedLen = strlen(versionBufferPtr);
            }
        }
        else
        {
            LE_INFO("Could not read Legato version.");
            returnedLen = snprintf(versionBufferPtr, len, UNKNOWN_VERSION, strlen(UNKNOWN_VERSION));
        }
        fclose(versionFilePtr);
        LE_INFO("Legato version = %s, len %d", versionBufferPtr, returnedLen);
    }
    return returnedLen;
}

//--------------------------------------------------------------------------------------------------
/**
 * Attempt to read the Customer PRI version string from the file system.
 *
 * @return
 *      - written buffer length
 */
//--------------------------------------------------------------------------------------------------
static size_t GetCustomerPriVersion
(
    char* versionBufferPtr,         ///< [INOUT] Buffer to hold the string.
    size_t len                      ///< [IN] Buffer length
)
{
    size_t returnedLen = 0;
    if (NULL != versionBufferPtr)
    {
        char priIdPn[LE_INFO_MAX_PRIID_PN_BYTES];
        char priIdRev[LE_INFO_MAX_PRIID_REV_BYTES];

        if (LE_OK == le_info_GetPriId(priIdPn, LE_INFO_MAX_PRIID_PN_BYTES,
                                      priIdRev, LE_INFO_MAX_PRIID_REV_BYTES))
        {
            if (strlen(priIdPn) && strlen(priIdRev))
            {
                returnedLen = snprintf(versionBufferPtr, len, "%s-%s", priIdPn, priIdRev);
            }
            else
            {
                returnedLen = snprintf(versionBufferPtr, len, UNKNOWN_VERSION, strlen(UNKNOWN_VERSION));
            }
        }
        else
        {
            returnedLen = snprintf(versionBufferPtr, len, UNKNOWN_VERSION, strlen(UNKNOWN_VERSION));
        }
        LE_INFO("PriVersion %s, len %d", versionBufferPtr, returnedLen);
    }
    return returnedLen;
}

//--------------------------------------------------------------------------------------------------
/**
 * Attempt to read the Carrier PRI version string from the file system.
 *
 * @return
 *      - written buffer length
 */
//--------------------------------------------------------------------------------------------------
static size_t GetCarrierPriVersion
(
    char* versionBufferPtr,         ///< [INOUT] Buffer to hold the string.
    size_t len                      ///< [IN] Buffer length
)
{
    size_t returnedLen = 0;
    if (NULL != versionBufferPtr)
    {
        char priName[LE_INFO_MAX_CAPRI_NAME_BYTES];
        char priRev[LE_INFO_MAX_CAPRI_REV_BYTES];

        if (LE_OK == le_info_GetCarrierPri(priName, LE_INFO_MAX_CAPRI_NAME_BYTES,
                                           priRev, LE_INFO_MAX_CAPRI_REV_BYTES))
        {
            if (strlen(priName) && strlen(priRev))
            {
                returnedLen = snprintf(versionBufferPtr, len, "%s-%s", priName, priRev);
            }
            else
            {
                returnedLen = snprintf(versionBufferPtr, len, UNKNOWN_VERSION, strlen(UNKNOWN_VERSION));
            }
        }
        else
        {
            returnedLen = snprintf(versionBufferPtr, len, UNKNOWN_VERSION, strlen(UNKNOWN_VERSION));
        }
        LE_INFO("Carrier PRI Version %s, len %d", versionBufferPtr, returnedLen);
    }
    return returnedLen;
}

//--------------------------------------------------------------------------------------------------
/**
 * Retrieve MCU version
 *
 * @return
 *      - written buffer length
 */
//--------------------------------------------------------------------------------------------------
static size_t GetMcuVersion
(
    char* versionBufferPtr,         ///< [INOUT] Buffer to hold the string.
    size_t len                      ///< [IN] Buffer length
)
{
    size_t returnedLen = 0;

    if (NULL != versionBufferPtr)
    {
        char mcuVersion[LE_ULPM_MAX_VERS_LEN+1];

        if (LE_OK == le_ulpm_GetFirmwareVersion(mcuVersion, sizeof(mcuVersion)))
        {
            if (strlen(mcuVersion))
            {
                returnedLen = snprintf(versionBufferPtr, len, "%s", mcuVersion);
            }
            else
            {
                returnedLen = snprintf(versionBufferPtr, len, UNKNOWN_VERSION, strlen(UNKNOWN_VERSION));
            }
        }
        else
        {
            LE_ERROR("Failed to retrieve MCU version");
            returnedLen = snprintf(versionBufferPtr, len, UNKNOWN_VERSION, strlen(UNKNOWN_VERSION));
        }
        LE_INFO("MCU version %s, len %d", versionBufferPtr, returnedLen);
    }
    return returnedLen;
}

//--------------------------------------------------------------------------------------------------
/**
 * Launch device reboot.
 */
//--------------------------------------------------------------------------------------------------
static void LaunchReboot
(
    void
)
{
    // Sync file systems before rebooting
    sync();
    // Reboot the system
    reboot(RB_AUTOBOOT);
}

//--------------------------------------------------------------------------------------------------
/**
 * Called when the reboot timer expires.
 */
//--------------------------------------------------------------------------------------------------
static void LaunchRebootTimerExpiryHandler
(
    le_timer_Ref_t timerRef    ///< Timer that expired
)
{
    // Timer used to delay reboot after command acknowledgment is not necessary anymore
    le_timer_Delete(timerRef);

    // Check if reboot can be launched now
    if (LE_OK == avcServer_QueryReboot(LaunchReboot))
    {
        LaunchReboot();
    }
}

//--------------------------------------------------------------------------------------------------
/**
 * Retrieve the device manufacturer
 * This API needs to have a procedural treatment
 *
 * @return
 *      - LWM2MCORE_ERR_COMPLETED_OK if the treatment succeeds
 *      - LWM2MCORE_ERR_GENERAL_ERROR if the treatment fails
 *      - LWM2MCORE_ERR_INCORRECT_RANGE if the provided parameters (WRITE operation) is incorrect
 *      - LWM2MCORE_ERR_NOT_YET_IMPLEMENTED if the resource is not yet implemented
 *      - LWM2MCORE_ERR_OP_NOT_SUPPORTED  if the resource is not supported
 *      - LWM2MCORE_ERR_INVALID_ARG if a parameter is invalid in resource handler
 *      - LWM2MCORE_ERR_INVALID_STATE in case of invalid state to treat the resource handler
 *      - LWM2MCORE_ERR_OVERFLOW in case of buffer overflow
 */
//--------------------------------------------------------------------------------------------------
lwm2mcore_Sid_t lwm2mcore_GetDeviceManufacturer
(
    char*   bufferPtr,  ///< [IN]    data buffer pointer
    size_t* lenPtr      ///< [INOUT] length of input buffer and length of the returned data
)
{
    lwm2mcore_Sid_t sID;
    le_result_t result;

    if ((!bufferPtr) || (!lenPtr))
    {
        return LWM2MCORE_ERR_INVALID_ARG;
    }

    result = le_info_GetManufacturerName((char*)bufferPtr, (uint32_t)*lenPtr);

    switch (result)
    {
        case LE_OK:
            sID = LWM2MCORE_ERR_COMPLETED_OK;
            break;

        case LE_OVERFLOW:
            sID = LWM2MCORE_ERR_OVERFLOW;
            break;

        case LE_FAULT:
        default:
            sID = LWM2MCORE_ERR_GENERAL_ERROR;
            break;
    }

    LE_DEBUG("lwm2mcore_DeviceManufacturer result: %d", sID);
    return sID;
}

//--------------------------------------------------------------------------------------------------
/**
 * Retrieve the device model number
 * This API needs to have a procedural treatment
 *
 * @return
 *      - LWM2MCORE_ERR_COMPLETED_OK if the treatment succeeds
 *      - LWM2MCORE_ERR_GENERAL_ERROR if the treatment fails
 *      - LWM2MCORE_ERR_INCORRECT_RANGE if the provided parameters (WRITE operation) is incorrect
 *      - LWM2MCORE_ERR_NOT_YET_IMPLEMENTED if the resource is not yet implemented
 *      - LWM2MCORE_ERR_OP_NOT_SUPPORTED  if the resource is not supported
 *      - LWM2MCORE_ERR_INVALID_ARG if a parameter is invalid in resource handler
 *      - LWM2MCORE_ERR_INVALID_STATE in case of invalid state to treat the resource handler
 *      - LWM2MCORE_ERR_OVERFLOW in case of buffer overflow
 */
//--------------------------------------------------------------------------------------------------
lwm2mcore_Sid_t lwm2mcore_GetDeviceModelNumber
(
    char*   bufferPtr,  ///< [IN]    data buffer pointer
    size_t* lenPtr      ///< [INOUT] length of input buffer and length of the returned data
)
{
    lwm2mcore_Sid_t sID;
    le_result_t result;

    if ((!bufferPtr) || (!lenPtr))
    {
        return LWM2MCORE_ERR_INVALID_ARG;
    }

    result = le_info_GetDeviceModel((char*)bufferPtr, (uint32_t)*lenPtr);

    switch (result)
    {
        case LE_OVERFLOW:
            sID = LWM2MCORE_ERR_OVERFLOW;
            break;

        case LE_OK:
            sID = LWM2MCORE_ERR_COMPLETED_OK;
            break;

        case LE_FAULT:
        default:
            sID = LWM2MCORE_ERR_GENERAL_ERROR;
            break;
    }

    LE_DEBUG("lwm2mcore_DeviceModelNumber result: %d", sID);
    return sID;
}

//--------------------------------------------------------------------------------------------------
/**
 * Retrieve the device serial number
 * This API needs to have a procedural treatment
 *
 * @return
 *      - LWM2MCORE_ERR_COMPLETED_OK if the treatment succeeds
 *      - LWM2MCORE_ERR_GENERAL_ERROR if the treatment fails
 *      - LWM2MCORE_ERR_INCORRECT_RANGE if the provided parameters (WRITE operation) is incorrect
 *      - LWM2MCORE_ERR_NOT_YET_IMPLEMENTED if the resource is not yet implemented
 *      - LWM2MCORE_ERR_OP_NOT_SUPPORTED  if the resource is not supported
 *      - LWM2MCORE_ERR_INVALID_ARG if a parameter is invalid in resource handler
 *      - LWM2MCORE_ERR_INVALID_STATE in case of invalid state to treat the resource handler
 *      - LWM2MCORE_ERR_OVERFLOW in case of buffer overflow
 */
//--------------------------------------------------------------------------------------------------
lwm2mcore_Sid_t lwm2mcore_GetDeviceSerialNumber
(
    char*   bufferPtr,  ///< [IN]    data buffer pointer
    size_t* lenPtr      ///< [INOUT] length of input buffer and length of the returned data
)
{
    lwm2mcore_Sid_t sID;
    le_result_t result;

    if ((!bufferPtr) || (!lenPtr))
    {
        return LWM2MCORE_ERR_INVALID_ARG;
    }

    result = le_info_GetPlatformSerialNumber((char*)bufferPtr, (uint32_t)*lenPtr);

    switch (result)
    {
        case LE_OVERFLOW:
            sID = LWM2MCORE_ERR_OVERFLOW;
            break;

        case LE_OK:
            sID = LWM2MCORE_ERR_COMPLETED_OK;
            break;

        case LE_FAULT:
        default:
            sID = LWM2MCORE_ERR_GENERAL_ERROR;
            break;
    }

    LE_DEBUG("lwm2mcore_DeviceSerialNumber result: %d", sID);
    return sID;
}

//--------------------------------------------------------------------------------------------------
/**
 * Retrieve the device firmware version
 * This API needs to have a procedural treatment
 *
 * @return
 *      - LWM2MCORE_ERR_COMPLETED_OK if the treatment succeeds
 *      - LWM2MCORE_ERR_GENERAL_ERROR if the treatment fails
 *      - LWM2MCORE_ERR_INCORRECT_RANGE if the provided parameters (WRITE operation) is incorrect
 *      - LWM2MCORE_ERR_NOT_YET_IMPLEMENTED if the resource is not yet implemented
 *      - LWM2MCORE_ERR_OP_NOT_SUPPORTED  if the resource is not supported
 *      - LWM2MCORE_ERR_INVALID_ARG if a parameter is invalid in resource handler
 *      - LWM2MCORE_ERR_INVALID_STATE in case of invalid state to treat the resource handler
 *      - LWM2MCORE_ERR_OVERFLOW in case of buffer overflow
 */
//--------------------------------------------------------------------------------------------------
lwm2mcore_Sid_t lwm2mcore_GetDeviceFirmwareVersion
(
    char*   bufferPtr,  ///< [IN]    data buffer pointer
    size_t* lenPtr      ///< [INOUT] length of input buffer and length of the returned data
)
{
    char tmpBufferPtr[FW_BUFFER_LENGTH];
    uint32_t remainingLen = *lenPtr;
    size_t len;
    uint32_t i = 0;
    ComponentVersion_t versionInfo[] =
    {
      { MODEM_TAG,              GetModemVersion         },
      { LK_TAG,                 GetLkVersion            },
      { LINUX_TAG,              GetOsVersion            },
      { ROOT_FS_TAG,            GetRfsVersion           },
      { USER_FS_TAG,            GetUfsVersion           },
      { LEGATO_TAG,             GetLegatoVersion        },
      { CUSTOMER_PRI_TAG,       GetCustomerPriVersion   },
      { CARRIER_PRI_TAG,        GetCarrierPriVersion    },
      { MCU_TAG,                GetMcuVersion           }
    };

    if ((!bufferPtr) || (!lenPtr))
    {
        return LWM2MCORE_ERR_INVALID_ARG;
    }

    LE_DEBUG("remainingLen %d", remainingLen);

    for (i = 0; i < NUM_ARRAY_MEMBERS(versionInfo); i++)
    {
        if (NULL != versionInfo[i].funcPtr)
        {
            len = versionInfo[i].funcPtr(tmpBufferPtr, FW_BUFFER_LENGTH);
            LE_DEBUG("len %d - remainingLen %d", len, remainingLen);
            /* len doesn't contain the final \0
             * remainingLen contains the final \0
             * So we have to keep one byte for \0
             */
            if (len > (remainingLen - 1))
            {
                *lenPtr = 0;
                bufferPtr[*lenPtr] = '\0';
                return LWM2MCORE_ERR_OVERFLOW;
            }
            else
            {
                snprintf(bufferPtr + strlen(bufferPtr),
                         remainingLen,
                         "%s%s",
                         versionInfo[i].tagPtr,
                         tmpBufferPtr);
                remainingLen -= len;
                LE_DEBUG("remainingLen %d", remainingLen);
            }
        }
    }

    *lenPtr = strlen(bufferPtr);
    return LWM2MCORE_ERR_COMPLETED_OK;
}

//--------------------------------------------------------------------------------------------------
/**
 * Retrieve the battery level (percentage)
 * This API needs to have a procedural treatment
 *
 * @return
 *      - LWM2MCORE_ERR_COMPLETED_OK if the treatment succeeds
 *      - LWM2MCORE_ERR_GENERAL_ERROR if the treatment fails
 *      - LWM2MCORE_ERR_INCORRECT_RANGE if the provided parameters (WRITE operation) is incorrect
 *      - LWM2MCORE_ERR_NOT_YET_IMPLEMENTED if the resource is not yet implemented
 *      - LWM2MCORE_ERR_OP_NOT_SUPPORTED  if the resource is not supported
 *      - LWM2MCORE_ERR_INVALID_ARG if a parameter is invalid in resource handler
 *      - LWM2MCORE_ERR_INVALID_STATE in case of invalid state to treat the resource handler
 */
//--------------------------------------------------------------------------------------------------
lwm2mcore_Sid_t lwm2mcore_GetBatteryLevel
(
    uint8_t* valuePtr  ///< [INOUT] data buffer
)
{
    le_ips_PowerSource_t powerSource;
    uint8_t batteryLevel;

    if (!valuePtr)
    {
        return LWM2MCORE_ERR_INVALID_ARG;
    }

    if (LE_OK != le_ips_GetPowerSource(&powerSource))
    {
        return LWM2MCORE_ERR_GENERAL_ERROR;
    }

    // Get the battery level only if the device is powered by a battery
    if (LE_IPS_POWER_SOURCE_BATTERY != powerSource)
    {
        LE_DEBUG("Device is not powered by a battery");
        return LWM2MCORE_ERR_NOT_YET_IMPLEMENTED;
    }

    if (LE_OK != le_ips_GetBatteryLevel(&batteryLevel))
    {
        return LWM2MCORE_ERR_GENERAL_ERROR;
    }

    LE_DEBUG("Battery level: %d%%", batteryLevel);
    *valuePtr = batteryLevel;

    return LWM2MCORE_ERR_COMPLETED_OK;
}

//--------------------------------------------------------------------------------------------------
/**
 * Retrieve the device time (UNIX time in seconds)
 * This API needs to have a procedural treatment
 *
 * @return
 *      - LWM2MCORE_ERR_COMPLETED_OK if the treatment succeeds
 *      - LWM2MCORE_ERR_GENERAL_ERROR if the treatment fails
 *      - LWM2MCORE_ERR_INCORRECT_RANGE if the provided parameters (WRITE operation) is incorrect
 *      - LWM2MCORE_ERR_NOT_YET_IMPLEMENTED if the resource is not yet implemented
 *      - LWM2MCORE_ERR_OP_NOT_SUPPORTED  if the resource is not supported
 *      - LWM2MCORE_ERR_INVALID_ARG if a parameter is invalid in resource handler
 *      - LWM2MCORE_ERR_INVALID_STATE in case of invalid state to treat the resource handler
 */
//--------------------------------------------------------------------------------------------------
lwm2mcore_Sid_t lwm2mcore_GetDeviceCurrentTime
(
    uint64_t* valuePtr  ///< [INOUT] data buffer
)
{
    le_clk_Time_t t;

    if (!valuePtr)
    {
        return LWM2MCORE_ERR_INVALID_ARG;
    }

    t = le_clk_GetAbsoluteTime();
    *valuePtr = t.sec;
    LE_DEBUG("time %d", t.sec);

    if (0 == t.sec)
    {
        return LWM2MCORE_ERR_GENERAL_ERROR;
    }

    return LWM2MCORE_ERR_COMPLETED_OK;
}

//--------------------------------------------------------------------------------------------------
/**
 * Retrieve the module identity (IMEI)
 * This API needs to have a procedural treatment
 *
 * @return
 *      - LWM2MCORE_ERR_COMPLETED_OK if the treatment succeeds
 *      - LWM2MCORE_ERR_GENERAL_ERROR if the treatment fails
 *      - LWM2MCORE_ERR_INCORRECT_RANGE if the provided parameters (WRITE operation) is incorrect
 *      - LWM2MCORE_ERR_NOT_YET_IMPLEMENTED if the resource is not yet implemented
 *      - LWM2MCORE_ERR_OP_NOT_SUPPORTED  if the resource is not supported
 *      - LWM2MCORE_ERR_INVALID_ARG if a parameter is invalid in resource handler
 *      - LWM2MCORE_ERR_INVALID_STATE in case of invalid state to treat the resource handler
 *      - LWM2MCORE_ERR_OVERFLOW in case of buffer overflow
 */
//--------------------------------------------------------------------------------------------------
lwm2mcore_Sid_t lwm2mcore_GetDeviceImei
(
    char*   bufferPtr,  ///< [IN]    data buffer pointer
    size_t* lenPtr      ///< [INOUT] length of input buffer and length of the returned data
)
{
    lwm2mcore_Sid_t sID;
    le_result_t result;
    char imei[LE_INFO_IMEI_MAX_BYTES];
    size_t imeiLen;

    if ((!bufferPtr) || (!lenPtr))
    {
        return LWM2MCORE_ERR_INVALID_ARG;
    }

    memset(imei, 0, sizeof(imei));

    result = le_info_GetImei(imei, sizeof(imei));
    switch (result)
    {
        case LE_OK:
            imeiLen = strlen(imei);

            if (*lenPtr < imeiLen)
            {
                sID = LWM2MCORE_ERR_OVERFLOW;
            }
            else
            {
                memcpy(bufferPtr, imei, imeiLen);
                *lenPtr = imeiLen;
                sID = LWM2MCORE_ERR_COMPLETED_OK;
            }
            break;

        case LE_OVERFLOW:
            sID = LWM2MCORE_ERR_OVERFLOW;
            break;

        case LE_FAULT:
        default:
            sID = LWM2MCORE_ERR_GENERAL_ERROR;
            break;
    }

    LE_DEBUG("lwm2mcore_DeviceImei result: %d", sID);
    return sID;
}

//--------------------------------------------------------------------------------------------------
/**
 * Retrieve the SIM card identifier (ICCID)
 * This API needs to have a procedural treatment
 *
 * @return
 *      - LWM2MCORE_ERR_COMPLETED_OK if the treatment succeeds
 *      - LWM2MCORE_ERR_GENERAL_ERROR if the treatment fails
 *      - LWM2MCORE_ERR_INCORRECT_RANGE if the provided parameters (WRITE operation) is incorrect
 *      - LWM2MCORE_ERR_NOT_YET_IMPLEMENTED if the resource is not yet implemented
 *      - LWM2MCORE_ERR_OP_NOT_SUPPORTED  if the resource is not supported
 *      - LWM2MCORE_ERR_INVALID_ARG if a parameter is invalid in resource handler
 *      - LWM2MCORE_ERR_INVALID_STATE in case of invalid state to treat the resource handler
 *      - LWM2MCORE_ERR_OVERFLOW in case of buffer overflow
 */
//--------------------------------------------------------------------------------------------------
lwm2mcore_Sid_t lwm2mcore_GetIccid
(
    char*   bufferPtr,  ///< [IN]    data buffer pointer
    size_t* lenPtr      ///< [INOUT] length of input buffer and length of the returned data
)
{
    lwm2mcore_Sid_t sID;
    le_result_t result;
    char iccid[LE_SIM_ICCID_BYTES];
    size_t iccidLen;

    if ((!bufferPtr) || (!lenPtr))
    {
        return LWM2MCORE_ERR_INVALID_ARG;
    }

    memset(iccid, 0, sizeof(iccid));

    result = le_sim_GetICCID(le_sim_GetSelectedCard(), iccid, sizeof(iccid));
    switch (result)
    {
        case LE_OK:
            iccidLen = strlen(iccid);

            if (*lenPtr < iccidLen)
            {
                sID = LWM2MCORE_ERR_OVERFLOW;
            }
            else
            {
                memcpy(bufferPtr, iccid, iccidLen);
                *lenPtr = iccidLen;
                sID = LWM2MCORE_ERR_COMPLETED_OK;
            }
            break;

        case LE_OVERFLOW:
            sID = LWM2MCORE_ERR_OVERFLOW;
            break;

        case LE_BAD_PARAMETER:
            sID = LWM2MCORE_ERR_INVALID_ARG;
            break;

        case LE_FAULT:
        default:
            sID = LWM2MCORE_ERR_GENERAL_ERROR;
            break;
    }

    LE_DEBUG("lwm2mcore_DeviceIccid result: %d", sID);
    return sID;
}

//--------------------------------------------------------------------------------------------------
/**
 * Retrieve the subscription identity (MEID/ESN/IMSI)
 * This API needs to have a procedural treatment
 *
 * @return
 *      - LWM2MCORE_ERR_COMPLETED_OK if the treatment succeeds
 *      - LWM2MCORE_ERR_GENERAL_ERROR if the treatment fails
 *      - LWM2MCORE_ERR_INCORRECT_RANGE if the provided parameters (WRITE operation) is incorrect
 *      - LWM2MCORE_ERR_NOT_YET_IMPLEMENTED if the resource is not yet implemented
 *      - LWM2MCORE_ERR_OP_NOT_SUPPORTED  if the resource is not supported
 *      - LWM2MCORE_ERR_INVALID_ARG if a parameter is invalid in resource handler
 *      - LWM2MCORE_ERR_INVALID_STATE in case of invalid state to treat the resource handler
 *      - LWM2MCORE_ERR_OVERFLOW in case of buffer overflow
 */
//--------------------------------------------------------------------------------------------------
lwm2mcore_Sid_t lwm2mcore_GetSubscriptionIdentity
(
    char*   bufferPtr,  ///< [IN]    data buffer pointer
    size_t* lenPtr      ///< [INOUT] length of input buffer and length of the returned data
)
{
    lwm2mcore_Sid_t sID;
    le_result_t result;
    le_mrc_Rat_t currentRat;
    char imsi[LE_SIM_IMSI_BYTES];
    char esn[LE_INFO_MAX_ESN_BYTES];
    char meid[LE_INFO_MAX_MEID_BYTES];
    size_t imsiLen, esnLen, meidLen;

    if ((!bufferPtr) || (!lenPtr))
    {
        return LWM2MCORE_ERR_INVALID_ARG;
    }

    if (LE_OK != le_mrc_GetRadioAccessTechInUse(&currentRat))
    {
        return LWM2MCORE_ERR_GENERAL_ERROR;
    }

    memset(imsi, 0, sizeof(imsi));
    memset(esn, 0, sizeof(esn));
    memset(meid, 0, sizeof(meid));

    // MEID and ESN are used in CDMA systems while IMSI is used in GSM/UMTS/LTE systems.
    if (LE_MRC_RAT_CDMA == currentRat)
    {
        // Try to retrieve the ESN first, then the MEID if the ESN is not available
        result = le_info_GetEsn(esn, sizeof(esn));
        switch (result)
        {
            case LE_OK:
                esnLen = strlen(esn);

                if (*lenPtr < esnLen)
                {
                    sID = LWM2MCORE_ERR_OVERFLOW;
                }
                else
                {
                    memcpy(bufferPtr, esn, esnLen);
                    *lenPtr = esnLen;
                    sID = LWM2MCORE_ERR_COMPLETED_OK;
                }
                break;

            case LE_OVERFLOW:
                sID = LWM2MCORE_ERR_OVERFLOW;
                break;

            case LE_FAULT:
            default:
                sID = LWM2MCORE_ERR_GENERAL_ERROR;
                break;
        }

        // ESN not available, try to retrieve the MEID
        if (LWM2MCORE_ERR_COMPLETED_OK != sID)
        {
            result = le_info_GetMeid(meid, sizeof(meid));
            switch (result)
            {
                case LE_OK:
                    meidLen = strlen(meid);

                    if (*lenPtr < meidLen)
                    {
                        sID = LWM2MCORE_ERR_OVERFLOW;
                    }
                    else
                    {
                        memcpy(bufferPtr, meid, meidLen);
                        *lenPtr = meidLen;
                        sID = LWM2MCORE_ERR_COMPLETED_OK;
                    }
                    break;

                case LE_OVERFLOW:
                    sID = LWM2MCORE_ERR_OVERFLOW;
                    break;

                case LE_FAULT:
                default:
                    sID = LWM2MCORE_ERR_GENERAL_ERROR;
                    break;
            }
        }
    }
    else
    {
        // Retrieve the IMSI for GSM/UMTS/LTE
        result = le_sim_GetIMSI(le_sim_GetSelectedCard(), imsi, sizeof(imsi));
        switch (result)
        {
            case LE_OK:
                imsiLen = strlen(imsi);

                if (*lenPtr < imsiLen)
                {
                    sID = LWM2MCORE_ERR_OVERFLOW;
                }
                else
                {
                    memcpy(bufferPtr, imsi, imsiLen);
                    *lenPtr = imsiLen;
                    sID = LWM2MCORE_ERR_COMPLETED_OK;
                }
                break;

            case LE_OVERFLOW:
                sID = LWM2MCORE_ERR_OVERFLOW;
                break;

            case LE_BAD_PARAMETER:
                sID = LWM2MCORE_ERR_INVALID_ARG;
                break;

            case LE_FAULT:
            default:
                sID = LWM2MCORE_ERR_GENERAL_ERROR;
                break;
        }
    }

    LE_DEBUG("lwm2mcore_DeviceSubscriptionIdentity result: %d", sID);
    return sID;
}

//--------------------------------------------------------------------------------------------------
/**
 * Retrieve the phone number (MSISDN)
 * This API needs to have a procedural treatment
 *
 * @return
 *      - LWM2MCORE_ERR_COMPLETED_OK if the treatment succeeds
 *      - LWM2MCORE_ERR_GENERAL_ERROR if the treatment fails
 *      - LWM2MCORE_ERR_INCORRECT_RANGE if the provided parameters (WRITE operation) is incorrect
 *      - LWM2MCORE_ERR_NOT_YET_IMPLEMENTED if the resource is not yet implemented
 *      - LWM2MCORE_ERR_OP_NOT_SUPPORTED  if the resource is not supported
 *      - LWM2MCORE_ERR_INVALID_ARG if a parameter is invalid in resource handler
 *      - LWM2MCORE_ERR_INVALID_STATE in case of invalid state to treat the resource handler
 *      - LWM2MCORE_ERR_OVERFLOW in case of buffer overflow
 */
//--------------------------------------------------------------------------------------------------
lwm2mcore_Sid_t lwm2mcore_GetMsisdn
(
    char*   bufferPtr,  ///< [IN]    data buffer pointer
    size_t* lenPtr      ///< [INOUT] length of input buffer and length of the returned data
)
{
    lwm2mcore_Sid_t sID;
    le_result_t result;
    char msisdn[LE_MDMDEFS_PHONE_NUM_MAX_BYTES];
    size_t msisdnLen;

    if ((!bufferPtr) || (!lenPtr))
    {
        return LWM2MCORE_ERR_INVALID_ARG;
    }

    memset(msisdn, 0, sizeof(msisdn));

    result = le_sim_GetSubscriberPhoneNumber(le_sim_GetSelectedCard(), msisdn, sizeof(msisdn));
    switch (result)
    {
        case LE_OK:
            msisdnLen = strlen(msisdn);

            if (*lenPtr < msisdnLen)
            {
                sID = LWM2MCORE_ERR_OVERFLOW;
            }
            else
            {
                memcpy(bufferPtr, msisdn, msisdnLen);
                *lenPtr = msisdnLen;
                sID = LWM2MCORE_ERR_COMPLETED_OK;
            }
            break;

        case LE_OVERFLOW:
            sID = LWM2MCORE_ERR_OVERFLOW;
            break;

        case LE_BAD_PARAMETER:
            sID = LWM2MCORE_ERR_INVALID_ARG;
            break;

        case LE_FAULT:
        default:
            sID = LWM2MCORE_ERR_GENERAL_ERROR;
            break;
    }

    LE_DEBUG("lwm2mcore_DeviceMsisdn result: %d", sID);
    return sID;
}

//--------------------------------------------------------------------------------------------------
/**
 * Retrieve the device temperature (in °C)
 * This API needs to have a procedural treatment
 *
 * @return
 *      - LWM2MCORE_ERR_COMPLETED_OK if the treatment succeeds
 *      - LWM2MCORE_ERR_GENERAL_ERROR if the treatment fails
 *      - LWM2MCORE_ERR_INCORRECT_RANGE if the provided parameters (WRITE operation) is incorrect
 *      - LWM2MCORE_ERR_NOT_YET_IMPLEMENTED if the resource is not yet implemented
 *      - LWM2MCORE_ERR_OP_NOT_SUPPORTED  if the resource is not supported
 *      - LWM2MCORE_ERR_INVALID_ARG if a parameter is invalid in resource handler
 *      - LWM2MCORE_ERR_INVALID_STATE in case of invalid state to treat the resource handler
 */
//--------------------------------------------------------------------------------------------------
lwm2mcore_Sid_t lwm2mcore_GetDeviceTemperature
(
    int32_t* valuePtr   ///< [INOUT] data buffer
)
{
    lwm2mcore_Sid_t sID;
    le_result_t result;
    le_temp_SensorRef_t pcSensorRef;
    int32_t temp;

    if (!valuePtr)
    {
        return LWM2MCORE_ERR_INVALID_ARG;
    }

    // Retrieve the power controller temperature
    pcSensorRef = le_temp_Request("POWER_CONTROLLER");
    result = le_temp_GetTemperature(pcSensorRef, &temp);
    if (LE_OK == result)
    {
        *valuePtr = temp;
        sID = LWM2MCORE_ERR_COMPLETED_OK;
    }
    else
    {
        sID = LWM2MCORE_ERR_GENERAL_ERROR;
    }

    LE_DEBUG("lwm2mCore_DeviceTemperature result: %d", sID);
    return sID;
}

//--------------------------------------------------------------------------------------------------
/**
 * Retrieve the number of unexpected resets
 * This API needs to have a procedural treatment
 *
 * @return
 *      - LWM2MCORE_ERR_COMPLETED_OK if the treatment succeeds
 *      - LWM2MCORE_ERR_GENERAL_ERROR if the treatment fails
 *      - LWM2MCORE_ERR_INVALID_ARG if a parameter is invalid in resource handler
 */
//--------------------------------------------------------------------------------------------------
lwm2mcore_Sid_t lwm2mcore_GetDeviceUnexpectedResets
(
    uint32_t* valuePtr  ///< [INOUT] data buffer
)
{
    uint64_t count;

    if (!valuePtr)
    {
        return LWM2MCORE_ERR_INVALID_ARG;
    }

    if (LE_OK != le_info_GetUnexpectedResetsCount(&count))
    {
        return LWM2MCORE_ERR_GENERAL_ERROR;
    }

    *valuePtr = (uint32_t)count;

    return LWM2MCORE_ERR_COMPLETED_OK;
}

//--------------------------------------------------------------------------------------------------
/**
 * Retrieve the total number of resets
 * This API needs to have a procedural treatment
 *
 * @return
 *      - LWM2MCORE_ERR_COMPLETED_OK if the treatment succeeds
 *      - LWM2MCORE_ERR_GENERAL_ERROR if the treatment fails
 *      - LWM2MCORE_ERR_INVALID_ARG if a parameter is invalid in resource handler
 */
//--------------------------------------------------------------------------------------------------
lwm2mcore_Sid_t lwm2mcore_GetDeviceTotalResets
(
    uint32_t* valuePtr  ///< [INOUT] data buffer
)
{
    uint64_t expected, unexpected;

    if (!valuePtr)
    {
        return LWM2MCORE_ERR_INVALID_ARG;
    }

    if ( (LE_OK != le_info_GetExpectedResetsCount(&expected)) ||
         (LE_OK != le_info_GetUnexpectedResetsCount(&unexpected)) )
    {
        return LWM2MCORE_ERR_GENERAL_ERROR;
    }

    *valuePtr = (uint32_t)(expected + unexpected);

    return LWM2MCORE_ERR_COMPLETED_OK;
}

//--------------------------------------------------------------------------------------------------
/**
 * Request to reboot the device
 * This API needs to have a procedural treatment
 *
 * @return
 *      - LWM2MCORE_ERR_COMPLETED_OK if the treatment succeeds
 *      - LWM2MCORE_ERR_GENERAL_ERROR if the treatment fails
 *      - LWM2MCORE_ERR_INCORRECT_RANGE if the provided parameters (WRITE operation) is incorrect
 *      - LWM2MCORE_ERR_NOT_YET_IMPLEMENTED if the resource is not yet implemented
 *      - LWM2MCORE_ERR_OP_NOT_SUPPORTED  if the resource is not supported
 *      - LWM2MCORE_ERR_INVALID_ARG if a parameter is invalid in resource handler
 *      - LWM2MCORE_ERR_INVALID_STATE in case of invalid state to treat the resource handler
 */
//--------------------------------------------------------------------------------------------------
lwm2mcore_Sid_t lwm2mcore_RebootDevice
(
    void
)
{
    le_clk_Time_t interval = {.sec = 2};

    LaunchRebootTimer = le_timer_Create("launch reboot timer");

    // Acknowledge the reboot command and launch the actual reboot later
    if (   (LE_OK != le_timer_SetHandler(LaunchRebootTimer, LaunchRebootTimerExpiryHandler))
        || (LE_OK != le_timer_SetInterval(LaunchRebootTimer, interval))
        || (LE_OK != le_timer_Start(LaunchRebootTimer))
       )
    {
        return LWM2MCORE_ERR_GENERAL_ERROR;
    }

    return LWM2MCORE_ERR_COMPLETED_OK;
}

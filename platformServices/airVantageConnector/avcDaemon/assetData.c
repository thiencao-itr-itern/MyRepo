/**
 * @file assetData.c
 *
 * Implementation of Asset Data Interface
 *
 * TODO:
 *  - implement client and server access restrictions
 *  - find correct sizes for various arrays and tables
 *  - review error checking and error return results -- should we LE_FATAL in some cases?
 *  - implement functions with "IMPLEMENT LATER" comments
 *
 * <hr>
 *
 * Copyright (C) Sierra Wireless Inc.
 *
 */

#include "legato.h"
#include "interfaces.h"

#include "limit.h"
#include "assetData.h"
#include "le_print.h"

// For htonl
#include <arpa/inet.h>



//--------------------------------------------------------------------------------------------------
// Macros
//--------------------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------------------
/**
 * This macro is similar to LE_PRINT_VALUE, but allows indenting
 */
//--------------------------------------------------------------------------------------------------
#define PRINT_VALUE(indent, formatSpec, value) \
    LE_DEBUG( "%*s" STRINGIZE(value) "=" formatSpec, indent, "", value)


//--------------------------------------------------------------------------------------------------
// Definitions
//--------------------------------------------------------------------------------------------------


//--------------------------------------------------------------------------------------------------
/**
 * Maximum number of bytes for a string value field
 */
//--------------------------------------------------------------------------------------------------
#define STRING_VALUE_NUMBYTES 256

//--------------------------------------------------------------------------------------------------
/**
 * Supported data types.  (Not all LWM2M types are listed yet)
 */
//--------------------------------------------------------------------------------------------------
typedef enum
{
    DATA_TYPE_NONE,    ///< Some fields do not have a data type, i.e. EXEC only fields
    DATA_TYPE_INT,
    DATA_TYPE_BOOL,
    DATA_TYPE_STRING,
    DATA_TYPE_FLOAT     ///< 64 bit floating point value
}
DataTypes_t;


//--------------------------------------------------------------------------------------------------
/**
 * Supported access modes; these are from the client perspective
 *
 * Use the commonly known Unix file permission bitmask values.
 */
//--------------------------------------------------------------------------------------------------
typedef enum
{
    ACCESS_EXEC   = 0x1,
    ACCESS_WRITE  = 0x2,
    ACCESS_READ   = 0x4
}
AccessBitMask_t;


//--------------------------------------------------------------------------------------------------
/**
 * Data associated with an asset with a particular id
 */
//--------------------------------------------------------------------------------------------------
typedef struct assetData_AssetData
{
    int assetId;                        ///< Id for this asset
    char assetName[100];                ///< Name for this asset
    char appName[100];                  ///< Name for app containing this asset
    int lastInstanceId;                 ///< Last assigned instance Id
    le_dls_List_t instanceList;         ///< List of instances for this asset
    le_dls_List_t fieldActionList;      ///< List of registered fieldAction handlers
    le_dls_List_t assetActionList;      ///< List of registered assetAction handlers
    bool isObjectObserve;               ///< Is Observe enabled on this object?
    uint8_t tokenLength;                ///< Token length of the lwm2m observe request.
    uint8_t token[8];                   ///< Token or request ID of the lwm2m observe request.
}
AssetData_t;


//--------------------------------------------------------------------------------------------------
/**
 * Data contained in a single asset instance
 */
//--------------------------------------------------------------------------------------------------
typedef struct le_avdata_AssetInstance
{
    int instanceId;              ///< Id for this instance
    AssetData_t* assetDataPtr;   ///< Back reference to asset data containing this instance
    le_dls_List_t fieldList;     ///< List of fields for this instance
    le_dls_Link_t link;          ///< For adding to the asset instance list
}
InstanceData_t;


//--------------------------------------------------------------------------------------------------
/**
 * Data contained in a single field of an asset instance
 */
//--------------------------------------------------------------------------------------------------
typedef struct
{
    int fieldId;
    char name[100];
    DataTypes_t type;
    AccessBitMask_t access;
    bool isObserve;
    uint8_t tokenLength;
    uint8_t token[8];
    union
    {
        int intValue;
        double floatValue;
        bool boolValue;
        char* strValuePtr;
    };

    le_dls_Link_t link;          ///< For adding to the field list
}
FieldData_t;


//--------------------------------------------------------------------------------------------------
/**
 * Data associated with a handler registered against field actions, i.e. write or execute,
 * or asset actions, i.e. create or delete.
 */
//--------------------------------------------------------------------------------------------------
typedef struct
{
    union
    {
        assetData_FieldActionHandlerFunc_t fieldActionHandlerPtr;
                            ///< User supplied handler for field actions
        assetData_AssetActionHandlerFunc_t assetActionHandlerPtr;
                            ///< User supplied handler for asset actions
    };
    void* contextPtr;       ///< User supplied context pointer
    int fieldId;            ///< If action is on a field
    bool isClient;          ///< Is handler registered by client or server
    le_dls_Link_t link;     ///< For adding to field or asset action list
}
ActionHandlerData_t;


//--------------------------------------------------------------------------------------------------
/**
 * Entry in table mapping data type strings to DataType_t values. All strings must be literals,
 * or allocated elsewhere, since only the pointer is stored in the entry.
 */
//--------------------------------------------------------------------------------------------------
typedef struct
{
    char* dataTypeStr;
    DataTypes_t dataType;
}
DataTypeTableEntry_t;



//--------------------------------------------------------------------------------------------------
// Local Data
//--------------------------------------------------------------------------------------------------


//--------------------------------------------------------------------------------------------------
/**
 * Field data memory pool.  Initialized in assetData_Init().
 */
//--------------------------------------------------------------------------------------------------
static le_mem_PoolRef_t FieldDataPoolRef = NULL;


//--------------------------------------------------------------------------------------------------
/**
 * Asset instance data memory pool.  Initialized in assetData_Init().
 */
//--------------------------------------------------------------------------------------------------
static le_mem_PoolRef_t InstanceDataPoolRef = NULL;


//--------------------------------------------------------------------------------------------------
/**
 * Asset data memory pool.  Initialized in assetData_Init().
 */
//--------------------------------------------------------------------------------------------------
static le_mem_PoolRef_t AssetDataPoolRef = NULL;


//--------------------------------------------------------------------------------------------------
/**
 * Field or Asset action handler data memory pool.  Initialized in assetData_Init().
 */
//--------------------------------------------------------------------------------------------------
static le_mem_PoolRef_t ActionHandlerDataPoolRef = NULL;


//--------------------------------------------------------------------------------------------------
/**
 * This pool is used for the string representation of a LWM2M address, which is used as a key in a
 * hashmap, e.g. (appName, assetId) to be used with AssetMap. Initialized in assetData_Init().
 */
//--------------------------------------------------------------------------------------------------
static le_mem_PoolRef_t AddressStringPoolRef = NULL;


//--------------------------------------------------------------------------------------------------
/**
 * This pool is used to store string field data.  Initialized in assetData_Init().
 */
//--------------------------------------------------------------------------------------------------
static le_mem_PoolRef_t StringValuePoolRef = NULL;


//--------------------------------------------------------------------------------------------------
/**
 * Maps (appName, assetId) to an AssetData block.  Initialized in assetData_Init().
 */
//--------------------------------------------------------------------------------------------------
static le_hashmap_Ref_t AssetMap = NULL;


//--------------------------------------------------------------------------------------------------
/**
 * Maps (appName, assetName) to an AssetData block.  Initialized in assetData_Init().
 */
//--------------------------------------------------------------------------------------------------
static le_hashmap_Ref_t AssetMapByName = NULL;


//--------------------------------------------------------------------------------------------------
/**
 * Used to delay reporting REG_UPDATE, so that we don't generate too much message traffic.
 */
//--------------------------------------------------------------------------------------------------
static le_timer_Ref_t RegUpdateTimerRef;


//--------------------------------------------------------------------------------------------------
/**
 * Table mapping data type strings to DataType_t values
 */
//--------------------------------------------------------------------------------------------------
static DataTypeTableEntry_t DataTypeTable[] =
{
    { "none",   DATA_TYPE_NONE },
    { "int",    DATA_TYPE_INT },
    { "bool",   DATA_TYPE_BOOL },
    { "string", DATA_TYPE_STRING },
    { "float",  DATA_TYPE_FLOAT }
};


//--------------------------------------------------------------------------------------------------
/**
 * Handler that will be called whenever an instance is create or deleted, for any asset.
 */
//--------------------------------------------------------------------------------------------------
static ActionHandlerData_t AllAssetActionHandlerData = { .assetActionHandlerPtr = NULL };


//--------------------------------------------------------------------------------------------------
/**
 * Declare this function here, until the QMI functions are moved out of this file.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t WriteNotifyObjectToTLV
(
    assetData_AssetDataRef_t assetRef,          ///< [IN] Asset to use
    int instanceId,                             ///< [IN] Instance that has a changed resource
    int fieldId,                                ///< [IN] The resource which changed
    uint8_t* bufPtr,                            ///< [OUT] Buffer for writing the TLV list
    size_t bufNumBytes,                         ///< [IN] Size of buffer
    size_t* numBytesWrittenPtr                  ///< [OUT] # bytes written to buffer.
);

//--------------------------------------------------------------------------------------------------
// Local functions
//--------------------------------------------------------------------------------------------------


//--------------------------------------------------------------------------------------------------
/**
 * Write the formatted string to a buffer
 *
 * @return:
 *      - LE_OK on success
 *      - LE_OVERFLOW if buffer is too small
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t FormatString
(
    char* strBufPtr,
    size_t strBufSize,
    const char* formatPtr,
    ...
)
{
    int numChars;
    va_list varParams;

    va_start(varParams, formatPtr);
    numChars = vsnprintf(strBufPtr, strBufSize, formatPtr, varParams);
    va_end(varParams);

    if ( numChars < 0 )
    {
        LE_ERROR("Can't print string");
        return LE_FAULT;
    }
    else if ( numChars >= strBufSize )
    {
        LE_ERROR("String too large for strBufPtr");
        return LE_OVERFLOW;
    }

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Convert data type string into enumerated type
 *
 * @return:
 *      - LE_OK on success
 *      - LE_FAULT on error
 */
//--------------------------------------------------------------------------------------------------
static le_result_t ConvertDataTypeStr
(
    char* dataTypeStrPtr,           ///< [IN]
    DataTypes_t* dataTypePtr        ///< [OUT]
)
{
    int i;

    for ( i=(NUM_ARRAY_MEMBERS(DataTypeTable)-1); i>=0; i-- )
    {
        if ( strcmp(dataTypeStrPtr, DataTypeTable[i].dataTypeStr) == 0 )
        {
            *dataTypePtr = DataTypeTable[i].dataType;
            return LE_OK;
        }
    }

    return LE_FAULT;
}


//--------------------------------------------------------------------------------------------------
/**
 * Get data type string from enumerated type
 *
 * @return:
 *      - pointer to string on success
 *      - empty string on error
 */
//--------------------------------------------------------------------------------------------------
static const char* GetDataTypeStr
(
    DataTypes_t dataType        ///< [IN]
)
{
    int i;

    for ( i=(NUM_ARRAY_MEMBERS(DataTypeTable)-1); i>=0; i-- )
    {
        if ( dataType == DataTypeTable[i].dataType )
        {
            return DataTypeTable[i].dataTypeStr;
        }
    }

    return "";
}


//--------------------------------------------------------------------------------------------------
/**
 * Convert access mode string into bitmask
 *
 * @return:
 *      - LE_OK on success
 *      - LE_FAULT on error
 */
//--------------------------------------------------------------------------------------------------
static le_result_t ConvertAccessModeStr
(
    char* accessModeStrPtr,              ///< [IN]
    AccessBitMask_t* accessModePtr       ///< [OUT]
)
{
    int i;
    AccessBitMask_t accessMode = 0;

    for ( i=(strlen(accessModeStrPtr)-1); i>=0; i-- )
    {
        switch ( accessModeStrPtr[i] )
        {
            case 'x':
                accessMode |= ACCESS_EXEC;
                break;

            case 'w':
                accessMode |= ACCESS_WRITE;
                break;

            case 'r':
                accessMode |= ACCESS_READ;
                break;

            default:
                return LE_FAULT;
        }
    }

    *accessModePtr = accessMode;
    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Initialize the value field of a field data block to a default, depending on the 'type' field.
 */
//--------------------------------------------------------------------------------------------------
static void InitDefaultFieldData
(
    FieldData_t* fieldDataPtr   ///< Init the value of this field data block
)
{
    fieldDataPtr->isObserve = false;

    switch ( fieldDataPtr->type )
    {
        case DATA_TYPE_INT:
            fieldDataPtr->intValue = 0;
            break;

        case DATA_TYPE_BOOL:
            fieldDataPtr->boolValue = false;
            break;

        case DATA_TYPE_STRING:
            fieldDataPtr->strValuePtr = le_mem_ForceAlloc(StringValuePoolRef);
            fieldDataPtr->strValuePtr[0] = '\0';
            break;

        case DATA_TYPE_FLOAT:
            fieldDataPtr->floatValue = 0.0;
            break;

        case DATA_TYPE_NONE:
            break;
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Read field model from configDB, and fill in field data block
 *
 * @return:
 *      - LE_OK on success
 *      - LE_FAULT on error
 */
//--------------------------------------------------------------------------------------------------
static le_result_t CreateFieldFromModel
(
    le_cfg_IteratorRef_t assetCfg,
    FieldData_t* fieldDataPtr
)
{
    char strBuf[100];     // Generic buffer for reading string data
    le_cfg_nodeType_t nodeType;

    le_cfg_GetString(assetCfg, "name", strBuf, sizeof(strBuf), "");
    le_utf8_Copy(fieldDataPtr->name, strBuf, sizeof(fieldDataPtr->name), NULL);

    // The "type" is optional; internally "none" is mapped to DATA_TYPE_NONE
    le_cfg_GetString(assetCfg, "type", strBuf, sizeof(strBuf), "none");
    ConvertDataTypeStr(strBuf, &fieldDataPtr->type);

    le_cfg_GetString(assetCfg, "access", strBuf, sizeof(strBuf), "");
    ConvertAccessModeStr(strBuf, &fieldDataPtr->access);

    // The 'default' is optional, and only supported for certain field types.
    nodeType=le_cfg_GetNodeType(assetCfg, "default");

    // Init with hard-coded defaults, which could get overwritten below.
    InitDefaultFieldData(fieldDataPtr);

    if (   ( nodeType == LE_CFG_TYPE_EMPTY )
        || ( nodeType == LE_CFG_TYPE_DOESNT_EXIST ) )
    {
        LE_DEBUG("No default for name=%s", fieldDataPtr->name);
    }
    else switch ( fieldDataPtr->type )
    {
        case DATA_TYPE_INT:
            fieldDataPtr->intValue = le_cfg_GetInt(assetCfg, "default", 0);
            break;

        case DATA_TYPE_BOOL:
            fieldDataPtr->boolValue = le_cfg_GetBool(assetCfg, "default", 0);
            break;

        case DATA_TYPE_STRING:
            le_cfg_GetString(assetCfg, "default", strBuf, sizeof(strBuf), "");
            le_utf8_Copy(fieldDataPtr->strValuePtr, strBuf, STRING_VALUE_NUMBYTES, NULL);
            break;

        case DATA_TYPE_FLOAT:
            fieldDataPtr->floatValue = le_cfg_GetFloat(assetCfg, "default", 0.0);

            break;

        case DATA_TYPE_NONE:
            LE_DEBUG("Default value not supported for data type '%s'",
                     GetDataTypeStr(fieldDataPtr->type));
            break;

    }

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Read asset model from configDB, and fill in asset data instance
 *
 * @return:
 *      - LE_OK on success
 *      - LE_FAULT on error
 */
//--------------------------------------------------------------------------------------------------
static le_result_t CreateInstanceFromModel
(
    le_cfg_IteratorRef_t assetCfg,      ///< [IN]
    InstanceData_t* assetInstPtr        ///< [IN]
)
{
    char strBuf[LIMIT_MAX_PATH_BYTES];     // Generic buffer for reading string data
    FieldData_t* fieldDataPtr;
    le_result_t result;

    // Goto to the 'fields' node; it must exist.
    le_cfg_GoToNode(assetCfg, "fields");
    if (le_cfg_IsEmpty(assetCfg, ""))
    {
        LE_ERROR("No field list found");
        return LE_FAULT;
    }

    // Get list of fields

    if (le_cfg_GoToFirstChild(assetCfg) != LE_OK)
    {
        LE_ERROR("Field list is empty");
        return LE_FAULT;
    }

    // Init the field list for this instance; it will get populated below
    assetInstPtr->fieldList = LE_DLS_LIST_INIT;

    do
    {
        // Allocate field data; will be released if errors are found
        fieldDataPtr = le_mem_ForceAlloc(FieldDataPoolRef);

        le_cfg_GetNodeName(assetCfg, "", strBuf, sizeof(strBuf));
        fieldDataPtr->fieldId = atoi(strBuf);

        // Populate the field from the model definition
        result = CreateFieldFromModel(assetCfg, fieldDataPtr);

        // todo: will have to release all fields allocated so far ...
        if ( result != LE_OK )
        {
            LE_ERROR("Error in field read");
            return LE_FAULT;
        }

        // Field read okay; add it to the list.
        le_dls_Queue(&assetInstPtr->fieldList, &fieldDataPtr->link);

    } while ( le_cfg_GoToNextSibling(assetCfg) == LE_OK );

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Open a read transaction for the specified asset model in the configDB
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if not found in configDB
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
static le_result_t OpenModelFromConfig
(
    const char* appNamePtr,                 ///< [IN] App containing the asset
    int assetId,                            ///< [IN] Asset id within the App
    le_cfg_IteratorRef_t* assetCfgPtr       ///< [OUT] Open config transaction if LE_OK
)
{
    le_cfg_IteratorRef_t assetCfg;
    char strBuf[LIMIT_MAX_PATH_BYTES] = "";     // Generic buffer for reading string data

    // The framework asset definitions are in a different place in the ConfigDB than the
    // regular application asset definitions.
    const char* formatPtr;
    if ( strcmp(appNamePtr, ASSET_DATA_LEGATO_OBJ_NAME) == 0 )
    {
        formatPtr = "/lwm2m/definitions/%s/assets/%i";
    }
    else
    {
        formatPtr = "/apps/%s/assets/%i";
    }

    if ( FormatString(strBuf, sizeof(strBuf), formatPtr, appNamePtr, assetId) != LE_OK )
    {
        return LE_FAULT;
    }

    // Start config DB transaction to read the model definition
    assetCfg = le_cfg_CreateReadTxn(strBuf);

    if (le_cfg_IsEmpty(assetCfg, ""))
    {
        le_cfg_CancelTxn(assetCfg);
        return LE_NOT_FOUND;
    }

    *assetCfgPtr = assetCfg;
    return LE_OK;
}

//--------------------------------------------------------------------------------------------------
/**
 * Fill in field data block from the given data.
 *
 * todo: should we allow default here, or do it outside as special case?
 */
//--------------------------------------------------------------------------------------------------
static void AddFieldFromData
(
    InstanceData_t* assetInstPtr,
    int fieldId,
    const char* namePtr,
    DataTypes_t type,
    AccessBitMask_t access
)
{
    FieldData_t* fieldDataPtr = le_mem_ForceAlloc(FieldDataPoolRef);

    fieldDataPtr->fieldId = fieldId;
    LE_ASSERT( le_utf8_Copy(fieldDataPtr->name, namePtr, sizeof(fieldDataPtr->name), NULL) == LE_OK );
    fieldDataPtr->type = type;
    fieldDataPtr->access = access;
    InitDefaultFieldData(fieldDataPtr);

    le_dls_Queue(&assetInstPtr->fieldList, &fieldDataPtr->link);
}


//--------------------------------------------------------------------------------------------------
/**
 * Fill in asset data instance for lwm2m object 9
 */
//--------------------------------------------------------------------------------------------------
static void CreateInstanceForObjectNine
(
    InstanceData_t* assetInstPtr    ///< [IN]
)
{
    // Init the field list for this instance; it will get populated below
    assetInstPtr->fieldList = LE_DLS_LIST_INIT;

    // todo: Not all fields are defined for now; only the ones that are actually needed, which
    //       turn out to be most of the mandatory fields/resources, except for "Package"
    AddFieldFromData(assetInstPtr, 0, "PkgName", DATA_TYPE_STRING, ACCESS_WRITE);
    AddFieldFromData(assetInstPtr, 1, "PkgVersion", DATA_TYPE_STRING, ACCESS_WRITE);
    AddFieldFromData(assetInstPtr, 3, "Package URI", DATA_TYPE_STRING, ACCESS_READ);
    AddFieldFromData(assetInstPtr, 4, "Install", DATA_TYPE_NONE, ACCESS_EXEC);
    AddFieldFromData(assetInstPtr, 6, "Uninstall", DATA_TYPE_NONE, ACCESS_EXEC);
    AddFieldFromData(assetInstPtr, 7, "Update State", DATA_TYPE_INT, ACCESS_WRITE);
    AddFieldFromData(assetInstPtr, 8, "Update Supported Objects", DATA_TYPE_BOOL, ACCESS_READ|ACCESS_WRITE);
    AddFieldFromData(assetInstPtr, 9, "Update Result", DATA_TYPE_INT, ACCESS_WRITE);
    AddFieldFromData(assetInstPtr, 10, "Activate", DATA_TYPE_NONE, ACCESS_EXEC);
    AddFieldFromData(assetInstPtr, 11, "Deactivate", DATA_TYPE_NONE, ACCESS_EXEC);
    AddFieldFromData(assetInstPtr, 12, "Activation State", DATA_TYPE_BOOL, ACCESS_WRITE);
}


//--------------------------------------------------------------------------------------------------
/**
 * Add a new asset data block to the AssetMap
 *
 * @return:
 *      - LE_OK on success
 *      - LE_FAULT on error
 */
//--------------------------------------------------------------------------------------------------
static le_result_t AddAssetData
(
    const char* appNamePtr,         ///< [IN]
    int assetId,                    ///< [IN]
    const char* assetNamePtr,       ///< [IN]
    AssetData_t** assetDataPtrPtr   ///< [OUT] Pointer to newly created asset data block
)
{
    AssetData_t* assetDataPtr;
    char* appNameAssetIdPtr;
    char* appNameAssetNamePtr;

    assetDataPtr = le_mem_ForceAlloc(AssetDataPoolRef);
    assetDataPtr->assetId = assetId;
    assetDataPtr->lastInstanceId = -1;
    assetDataPtr->instanceList = LE_DLS_LIST_INIT;
    assetDataPtr->fieldActionList = LE_DLS_LIST_INIT;
    assetDataPtr->assetActionList = LE_DLS_LIST_INIT;
    assetDataPtr->isObjectObserve = false;
    le_utf8_Copy(assetDataPtr->assetName, assetNamePtr, sizeof(assetDataPtr->assetName), NULL);
    le_utf8_Copy(assetDataPtr->appName, appNamePtr, sizeof(assetDataPtr->appName), NULL);

    // Put (appName, assetId) key in AssetMap, pointing to the assetData block
    // Put (appName, assetName) key in AssetMapByName, pointing to the same assetData block
    appNameAssetIdPtr = le_mem_ForceAlloc(AddressStringPoolRef);
    appNameAssetNamePtr = le_mem_ForceAlloc(AddressStringPoolRef);

    if ( ( FormatString(appNameAssetIdPtr, 100, "%s/%i", appNamePtr, assetId) != LE_OK ) ||
         ( FormatString(appNameAssetNamePtr, 100, "%s/%s", appNamePtr, assetNamePtr) != LE_OK ) )
    {
        le_mem_Release(assetDataPtr);
        le_mem_Release(appNameAssetIdPtr);
        le_mem_Release(appNameAssetNamePtr);
        return LE_FAULT;
    }

    // todo: 'Put' returns a value, but not sure what it's for.
    le_hashmap_Put(AssetMap, appNameAssetIdPtr, assetDataPtr);
    le_hashmap_Put(AssetMapByName, appNameAssetNamePtr, assetDataPtr);

    // Return the pointer to the newly allocated block
    *assetDataPtrPtr = assetDataPtr;
    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Get an asset data block from the AssetMap and return it
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if not found in AssetMap
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
static le_result_t GetAssetData
(
    const char* appNamePtr,         ///< [IN]
    int assetId,                    ///< [IN]
    AssetData_t** assetDataPtrPtr   ///< [OUT] Pointer to found asset data block
)
{
    char key[100];

    if ( FormatString(key, sizeof(key), "%s/%i", appNamePtr, assetId) != LE_OK )
    {
        return LE_FAULT;
    }

    *assetDataPtrPtr = le_hashmap_Get(AssetMap, &key);

    if ( *assetDataPtrPtr != NULL )
    {
        return LE_OK;
    }
    else
    {
        return LE_NOT_FOUND;
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Get an asset data block from AssetMapByName and return it
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if not found in AssetMapByName
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
static le_result_t GetAssetDataByName
(
    const char* appNamePtr,         ///< [IN]
    const char* assetNamePtr,       ///< [IN]
    AssetData_t** assetDataPtrPtr   ///< [OUT] Pointer to found asset data block
)
{
    char key[100];

    if ( FormatString(key, sizeof(key), "%s/%s", appNamePtr, assetNamePtr) != LE_OK )
    {
        return LE_FAULT;
    }

    *assetDataPtrPtr = le_hashmap_Get(AssetMapByName, &key);

    if ( *assetDataPtrPtr != NULL )
    {
        return LE_OK;
    }
    else
    {
        return LE_NOT_FOUND;
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Create new AssetData block from the appropriate asset model
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if asset not found
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
static le_result_t CreateAssetDataFromModel
(
    const char* appNamePtr,         ///< [IN]
    int assetId,                    ///< [IN]
    AssetData_t** assetDataPtrPtr   ///< [OUT] Pointer to asset data block
)
{
    // LWM2M objects are hard-coded; the rest are taken from the ConfigDB
    if (0 == strcmp( LWM2M_NAME, appNamePtr))
    {
        if ( assetId == LWM2M_OBJ9 )
        {
            if ( AddAssetData(appNamePtr, assetId, "Software Management", assetDataPtrPtr) != LE_OK )
            {
                return LE_FAULT;
            }
        }
        else
        {
            LE_ERROR("Asset model for %s/%i is not found", appNamePtr, assetId);
            return LE_NOT_FOUND;
        }
    }
    else
    {
        le_result_t result;
        le_cfg_IteratorRef_t assetCfg;
        char assetName[100];

        // Open a config read transaction for the asset model
        result = OpenModelFromConfig(appNamePtr, assetId, &assetCfg);
        if ( result != LE_OK )
        {
            if ( result == LE_NOT_FOUND )
            {
                LE_ERROR("Asset model for %s/%i is not found", appNamePtr, assetId);
            }
            return result;
        }

        // Get the asset name from config
        le_cfg_GetString(assetCfg, "name", assetName, sizeof(assetName), "");

        // Regardless of success/failure, stop the transaction
        le_cfg_CancelTxn(assetCfg);

        // Create and store new AssetData block
        if ( AddAssetData(appNamePtr, assetId, assetName, assetDataPtrPtr) != LE_OK )
        {
            return LE_FAULT;
        }
    }

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Create new AssetData block from the appropriate asset model using the asset name
 *
 * This is only for application defined assets.
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if asset not found
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
static le_result_t CreateAssetDataFromModelByName
(
    const char* appNamePtr,         ///< [IN] App name
    const char* assetNamePtr,       ///< [IN] Asset name
    AssetData_t** assetDataPtrPtr   ///< [OUT] Pointer to asset data block
)
{
    le_result_t result = LE_NOT_FOUND;
    int assetId = -1;
    le_cfg_IteratorRef_t assetCfg;
    char strBuf[LIMIT_MAX_PATH_BYTES] = "";     // Generic buffer for reading string data

    if ( FormatString(strBuf, sizeof(strBuf), "/apps/%s/assets", appNamePtr) != LE_OK )
    {
        return LE_FAULT;
    }

    // Open a config read transaction for the asset model
    assetCfg = le_cfg_CreateReadTxn(strBuf);

    if (le_cfg_IsEmpty(assetCfg, ""))
    {
        LE_ERROR("Asset model for %s is not found", appNamePtr);
        result = LE_NOT_FOUND;
    }

    else if (le_cfg_GoToFirstChild(assetCfg) != LE_OK)
    {
        LE_ERROR("Asset list for %s is empty", appNamePtr);
        result = LE_NOT_FOUND;
    }

    else do
    {
        // Get the assetId
        le_cfg_GetNodeName(assetCfg, "", strBuf, sizeof(strBuf));
        assetId = atoi(strBuf);
        LE_PRINT_VALUE("%i", assetId);

        // Get the associated assetName
        le_cfg_GetString(assetCfg, "name", strBuf, sizeof(strBuf), "");
        LE_PRINT_VALUE("%s", strBuf);
        LE_PRINT_VALUE("%s", assetNamePtr);

        // If this is the assetName we're interested in, then we're done searching
        if ( strcmp(assetNamePtr, strBuf) == 0 )
        {
            result = LE_OK;
            break;
        }

    } while ( le_cfg_GoToNextSibling(assetCfg) == LE_OK );

    // Regardless of success/failure, stop the transaction
    le_cfg_CancelTxn(assetCfg);

    // Create and store new AssetData block, if we found the asset definition
    if ( result == LE_OK )
    {
        if ( AddAssetData(appNamePtr, assetId, assetNamePtr, assetDataPtrPtr) != LE_OK )
        {
            result = LE_FAULT;
        }
    }

    return result;
}


//--------------------------------------------------------------------------------------------------
/**
 * Get the specified instance from the given asset data block
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if instance not found
 */
//--------------------------------------------------------------------------------------------------
static le_result_t GetInstanceFromAssetData
(
    AssetData_t* assetDataPtr,
    int instanceId,
    InstanceData_t** instanceDataPtrPtr   ///< [OUT]
)
{
    InstanceData_t* assetInstancePtr;
    le_dls_Link_t* linkPtr;

    // Get the start of the instance list
    linkPtr = le_dls_Peek(&assetDataPtr->instanceList);

    // Loop through the asset instances
    while ( linkPtr != NULL )
    {
        assetInstancePtr = CONTAINER_OF(linkPtr, InstanceData_t, link);
        //LE_PRINT_VALUE("%i", assetInstancePtr->instanceId);

        if ( assetInstancePtr->instanceId == instanceId )
        {
            *instanceDataPtrPtr = assetInstancePtr;
            return LE_OK;
        }

        linkPtr = le_dls_PeekNext(&assetDataPtr->instanceList, linkPtr);
    }

    return LE_NOT_FOUND;
}


//--------------------------------------------------------------------------------------------------
/**
 * Get the specified field from the given asset data block instance
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if field not found
 */
//--------------------------------------------------------------------------------------------------
static le_result_t GetFieldFromInstance
(
    InstanceData_t* instanceDataPtr,
    int fieldId,
    FieldData_t** fieldDataPtrPtr   ///< [OUT]
)
{
    FieldData_t* fieldDataPtr;
    le_dls_Link_t* fieldLinkPtr;

    // Get the start of the field list
    fieldLinkPtr = le_dls_Peek(&instanceDataPtr->fieldList);

    // Loop through the fields
    while ( fieldLinkPtr != NULL )
    {
        fieldDataPtr = CONTAINER_OF(fieldLinkPtr, FieldData_t, link);
        //LE_PRINT_VALUE("%i", fieldDataPtr->fieldId);

        if ( fieldDataPtr->fieldId == fieldId )
        {
            *fieldDataPtrPtr = fieldDataPtr;
            return LE_OK;
        }

        fieldLinkPtr = le_dls_PeekNext(&instanceDataPtr->fieldList, fieldLinkPtr);
    }

    return LE_NOT_FOUND;
}


//--------------------------------------------------------------------------------------------------
/**
 * Get the specified instance from the AssetMap
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if instance not found
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
static le_result_t GetInstance
(
    const char* appNamePtr,
    int assetId,
    int instanceId,
    InstanceData_t** instanceDataPtrPtr   ///< [OUT]
)
{
    le_result_t result;
    AssetData_t* assetDataPtr;
    InstanceData_t* instanceDataPtr;

    // Get an existing AssetData block from AssetMap
    result = GetAssetData(appNamePtr, assetId, &assetDataPtr);
    if ( result != LE_OK )
    {
        return result;
    }

    //LE_PRINT_VALUE("%i", assetDataPtr->assetId);
    //LE_PRINT_VALUE("%s", assetDataPtr->assetName);

    result = GetInstanceFromAssetData(assetDataPtr, instanceId, &instanceDataPtr);
    if ( result != LE_OK )
    {
        return result;
    }

    // Found the instance, so return it.
    *instanceDataPtrPtr = instanceDataPtr;
    return LE_OK;
}


#if 0
// todo: Is this needed?
//--------------------------------------------------------------------------------------------------
/**
 * Get the specified field from the AssetMap
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if field not found
 */
//--------------------------------------------------------------------------------------------------
static le_result_t GetField
(
    const char* appNamePtr,
    int assetId,
    int instanceId,
    int fieldId,
    FieldData_t** fieldDataPtrPtr   ///< [OUT]
)
{
    le_result_t result;
    AssetData_t* assetDataPtr;
    InstanceData_t* instanceDataPtr;
    FieldData_t* fieldDataPtr;

    // Get an existing AssetData block from AssetMap
    result = GetAssetData(appNamePtr, assetId, &assetDataPtr);
    if ( result != LE_OK )
    {
        return result;
    }

    //LE_PRINT_VALUE("%i", assetDataPtr->assetId);
    //LE_PRINT_VALUE("%s", assetDataPtr->assetName);

    result = GetInstanceFromAssetData(assetDataPtr, instanceId, &instanceDataPtr);
    if ( result != LE_OK )
    {
        return result;
    }

    result = GetFieldFromInstance(instanceDataPtr, fieldId, &fieldDataPtr);
    if ( result != LE_OK )
    {
        return result;
    }

    // Found the field, so return it.
    *fieldDataPtrPtr = fieldDataPtr;
    return LE_OK;
}
#endif



//--------------------------------------------------------------------------------------------------
/**
 * Check if a registered handler exists for a field read action.
 *
 * @return:
 *      - true if field read action handler exists
 *      - false if field read action handler doesn't exist
 */
//--------------------------------------------------------------------------------------------------
static bool IsFieldReadCallBackExist
(
    InstanceData_t* instanceDataPtr,        ///< [IN] Asset instance that action occurred on.
    FieldData_t* fieldDataPtr               ///< [IN] Field data ptr
)
{
    ActionHandlerData_t* handlerDataPtr;
    le_dls_Link_t* linkPtr;

    LE_PRINT_VALUE("%d", fieldDataPtr->access);

    // Verify that the field is writeable by the client.
    if (!(fieldDataPtr->access & ACCESS_WRITE))
    {
        return false;
    }

    // Get the start of the handler list
    linkPtr = le_dls_Peek(&instanceDataPtr->assetDataPtr->fieldActionList);

    // Loop through the list looking for a handler.
    while ( linkPtr != NULL )
    {
        handlerDataPtr = CONTAINER_OF(linkPtr, ActionHandlerData_t, link);

        // Return true if we find a handler.
        if ( fieldDataPtr->fieldId == handlerDataPtr->fieldId )
        {
           return true;
        }

        linkPtr = le_dls_PeekNext(&instanceDataPtr->assetDataPtr->fieldActionList, linkPtr);
    }

    return false;
}


//--------------------------------------------------------------------------------------------------
/**
 * Call any registered handlers to be notified on field actions, such as write or execute
 *
 * @return:
 *      - LE_OK on success
 *      - LE_FAULT on error
 */
//--------------------------------------------------------------------------------------------------
static le_result_t CallFieldActionHandlers
(
    InstanceData_t* instanceDataPtr,        ///< [IN] Asset instance that action occurred on.
    int fieldId,                            ///< [IN] Field that action occurred on
    assetData_ActionTypes_t action,         ///< [IN] The action that occurred
    bool isClient                           ///< [IN] Is action from client or server
)
{
    ActionHandlerData_t* handlerDataPtr;
    le_dls_Link_t* linkPtr;

    // Get the start of the handler list
    linkPtr = le_dls_Peek(&instanceDataPtr->assetDataPtr->fieldActionList);

    // Loop through the list, calling the handlers
    while ( linkPtr != NULL )
    {
        handlerDataPtr = CONTAINER_OF(linkPtr, ActionHandlerData_t, link);

        // The list contains registered handlers for all fields of the given asset, so call only
        // those handlers that are applicable for this field.
        if ( fieldId == handlerDataPtr->fieldId )
        {
            // Client registered handlers should only be called by server actions, and server
            // registered handlers should only be called by client actions.
            if ( ( ( handlerDataPtr->isClient) && (!isClient) ) ||
                 ( (!handlerDataPtr->isClient) && ( isClient) ) )
            {
                handlerDataPtr->fieldActionHandlerPtr(instanceDataPtr,
                                                      fieldId,
                                                      action,
                                                      handlerDataPtr->contextPtr);
            }
        }

        linkPtr = le_dls_PeekNext(&instanceDataPtr->assetDataPtr->fieldActionList, linkPtr);
    }

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Call any registered handlers to be notified on asset actions, such as create or delete
 *
 * @return:
 *      - LE_OK on success
 *      - LE_FAULT on error
 */
//--------------------------------------------------------------------------------------------------
static le_result_t CallAssetActionHandlers
(
    assetData_AssetDataRef_t assetRef,      ///< [IN] Asset that action occurred on.
    int instanceId,                         ///< [IN] Asset instance that action occurred on
    assetData_ActionTypes_t action
)
{
    ActionHandlerData_t* handlerDataPtr;
    le_dls_Link_t* linkPtr;

    // Get the start of the handler list
    linkPtr = le_dls_Peek(&assetRef->assetActionList);

    // Loop through the list, calling the handlers
    while ( linkPtr != NULL )
    {
        handlerDataPtr = CONTAINER_OF(linkPtr, ActionHandlerData_t, link);

        handlerDataPtr->assetActionHandlerPtr(assetRef,
                                              instanceId,
                                              action,
                                              handlerDataPtr->contextPtr);

        linkPtr = le_dls_PeekNext(&assetRef->assetActionList, linkPtr);
    }

    // If the AllAsset handler is registered, then call it
    if ( AllAssetActionHandlerData.assetActionHandlerPtr != NULL )
    {
        AllAssetActionHandlerData.assetActionHandlerPtr(
                                            assetRef,
                                            instanceId,
                                            action,
                                            AllAssetActionHandlerData.contextPtr);
    }

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Print to the logs a single instance of asset data
 */
//--------------------------------------------------------------------------------------------------
static void PrintInstanceData
(
    InstanceData_t* assetInstPtr    ///< [IN]
)
{
    FieldData_t* fieldDataPtr;
    le_dls_Link_t* linkPtr;

    LE_PRINT_VALUE("%i", assetInstPtr->instanceId);

    // Get the start of the field list
    linkPtr = le_dls_Peek(&assetInstPtr->fieldList);

    // Loop through the fields
    while ( linkPtr != NULL )
    {
        fieldDataPtr = CONTAINER_OF(linkPtr, FieldData_t, link);

        PRINT_VALUE(4, "%i", fieldDataPtr->fieldId);
        PRINT_VALUE(8, "'%s'", fieldDataPtr->name);
        PRINT_VALUE(8, "%s", GetDataTypeStr(fieldDataPtr->type));
        PRINT_VALUE(8, "%i", fieldDataPtr->access);

        switch ( fieldDataPtr->type )
        {
            case DATA_TYPE_INT:
                PRINT_VALUE(8, "%i", fieldDataPtr->intValue);
                break;

            case DATA_TYPE_BOOL:
                PRINT_VALUE(8, "%i", fieldDataPtr->boolValue);
                break;

            case DATA_TYPE_STRING:
                PRINT_VALUE(8, "'%s'", fieldDataPtr->strValuePtr);
                break;

            case DATA_TYPE_FLOAT:
                PRINT_VALUE(8, "%lf", fieldDataPtr->floatValue);
                break;

            case DATA_TYPE_NONE:
                LE_DEBUG( "%*s<no value>", 8, "" );
                break;
        }

        linkPtr = le_dls_PeekNext(&assetInstPtr->fieldList, linkPtr);
    }

}


//--------------------------------------------------------------------------------------------------
/**
 * Print to the logs the entire AssetMap
 */
//--------------------------------------------------------------------------------------------------
static void PrintAssetMap
(
    void
)
{
    const char* nameIdPtr;
    const AssetData_t* assetDataPtr;
    InstanceData_t* assetInstancePtr;
    le_dls_Link_t* linkPtr;

    le_hashmap_It_Ref_t iterRef = le_hashmap_GetIterator(AssetMap);

    while ( le_hashmap_NextNode(iterRef) == LE_OK )
    {
        nameIdPtr = le_hashmap_GetKey(iterRef);
        assetDataPtr = le_hashmap_GetValue(iterRef);

        // Print out asset data block, and all its instances.
        PRINT_VALUE(0, "%s", nameIdPtr);
        PRINT_VALUE(0, "%i", assetDataPtr->assetId);
        PRINT_VALUE(0, "'%s'", assetDataPtr->assetName);

        // Get the start of the instance list
        linkPtr = le_dls_Peek(&assetDataPtr->instanceList);

        // Loop through the asset instances
        while ( linkPtr != NULL )
        {
            assetInstancePtr = CONTAINER_OF(linkPtr, InstanceData_t, link);
            PrintInstanceData(assetInstancePtr);

            linkPtr = le_dls_PeekNext(&assetDataPtr->instanceList, linkPtr);
        }
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Get the integer value for the specified field
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if field not found
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t static GetInt
(
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to use
    int fieldId,                                ///< [IN] Field to read
    int* valuePtr,                              ///< [OUT] The value read
    bool isClient                               ///< [IN] Is it client or server access
)
{
    le_result_t result;
    FieldData_t* fieldDataPtr;

    result = GetFieldFromInstance(instanceRef, fieldId, &fieldDataPtr);
    if ( result != LE_OK )
    {
        return result;
    }

    //LE_PRINT_VALUE("%i", fieldDataPtr->fieldId);
    //LE_PRINT_VALUE("%s", fieldDataPtr->name);

    if ( fieldDataPtr->type != DATA_TYPE_INT )
    {
        LE_ERROR("Field type mismatch: expected 'int', got '%s'", GetDataTypeStr(fieldDataPtr->type));
        return LE_FAULT;
    }

    // Call any registered handlers to be notified of read
    CallFieldActionHandlers( instanceRef, fieldId, ASSET_DATA_ACTION_READ, isClient );

    // Get the value and return it
    *valuePtr = fieldDataPtr->intValue;
    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Get the float value for the specified field
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if field not found
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t static GetFloat
(
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to use
    int fieldId,                                ///< [IN] Field to read
    double* valuePtr,                           ///< [OUT] The value read
    bool isClient                               ///< [IN] Is it client or server access
)
{
    le_result_t result;
    FieldData_t* fieldDataPtr;

    result = GetFieldFromInstance(instanceRef, fieldId, &fieldDataPtr);
    if ( result != LE_OK )
    {
        return result;
    }

    if ( fieldDataPtr->type != DATA_TYPE_FLOAT )
    {
        LE_ERROR("Field type mismatch: expected 'float', got '%s'", GetDataTypeStr(fieldDataPtr->type));
        return LE_FAULT;
    }

    // Call any registered handlers to be notified of read
    CallFieldActionHandlers( instanceRef, fieldId, ASSET_DATA_ACTION_READ, isClient );

    // Get the value and return it
    *valuePtr = fieldDataPtr->floatValue;
    return LE_OK;
}

//--------------------------------------------------------------------------------------------------
/**
 * Set the integer value for the specified field
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if field not found
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t static SetInt
(
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to use
    int fieldId,                                ///< [IN] Field to write
    int value,                                  ///< [IN] The value to write
    bool isClient                               ///< [IN] Is it client or server access
)
{
    le_result_t result;
    FieldData_t* fieldDataPtr;
    int prevValue;

    result = GetFieldFromInstance(instanceRef, fieldId, &fieldDataPtr);

    if ( result != LE_OK )
    {
        return result;
    }

    if ( fieldDataPtr->type != DATA_TYPE_INT )
    {
        LE_ERROR("Field type mismatch: expected 'int', got '%s'", GetDataTypeStr(fieldDataPtr->type));
        return LE_FAULT;
    }

    // Remember current value and set new value.
    prevValue = fieldDataPtr->intValue;
    fieldDataPtr->intValue = value;

    // Call any registered handlers to be notified of write.
    CallFieldActionHandlers( instanceRef, fieldId, ASSET_DATA_ACTION_WRITE, isClient );

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Set the float value for the specified field
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if field not found
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t static SetFloat
(
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to use
    int fieldId,                                ///< [IN] Field to write
    double value,                               ///< [IN] The value to write
    bool isClient                               ///< [IN] Is it client or server access
)
{
    le_result_t result;
    FieldData_t* fieldDataPtr;
    float prevValue;

    result = GetFieldFromInstance(instanceRef, fieldId, &fieldDataPtr);
    if ( result != LE_OK )
    {
        return result;
    }

    if ( fieldDataPtr->type != DATA_TYPE_FLOAT )
    {
        LE_ERROR("Field type mismatch: expected 'float', got '%s'", GetDataTypeStr(fieldDataPtr->type));
        return LE_FAULT;
    }

    // Remember current value and set new value.
    prevValue = fieldDataPtr->floatValue;
    fieldDataPtr->floatValue = value;

    // Call any registered handlers to be notified of write.
    CallFieldActionHandlers( instanceRef, fieldId, ASSET_DATA_ACTION_WRITE, isClient );

    return LE_OK;
}

//--------------------------------------------------------------------------------------------------
/**
 * Get the bool value for the specified field
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if field not found
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t static GetBool
(
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to use
    int fieldId,                                ///< [IN] Field to read
    bool* valuePtr,                             ///< [OUT] The value read
    bool isClient                               ///< [IN] Is it client or server access
)
{
    le_result_t result;
    FieldData_t* fieldDataPtr;

    result = GetFieldFromInstance(instanceRef, fieldId, &fieldDataPtr);
    if ( result != LE_OK )
    {
        return result;
    }

    if ( fieldDataPtr->type != DATA_TYPE_BOOL )
    {
        LE_ERROR("Field type mismatch: expected 'int', got '%s'", GetDataTypeStr(fieldDataPtr->type));
        return LE_FAULT;
    }

    // Call any registered handlers to be notified of read
    CallFieldActionHandlers( instanceRef, fieldId, ASSET_DATA_ACTION_READ, isClient );

    // Get the value and return it
    *valuePtr = fieldDataPtr->boolValue;
    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Set the bool value for the specified field
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if field not found
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t static SetBool
(
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to use
    int fieldId,                                ///< [IN] Field to write
    bool value,                                 ///< [IN] The value to write
    bool isClient                               ///< [IN] Is it client or server access
)
{
    le_result_t result;
    FieldData_t* fieldDataPtr;
    bool prevValue;

    result = GetFieldFromInstance(instanceRef, fieldId, &fieldDataPtr);
    if ( result != LE_OK )
    {
        return result;
    }

    if ( fieldDataPtr->type != DATA_TYPE_BOOL )
    {
        LE_ERROR("Field type mismatch: expected 'int', got '%s'", GetDataTypeStr(fieldDataPtr->type));
        return LE_FAULT;
    }

    // Remember current value and set new value.
    prevValue = fieldDataPtr->boolValue;
    fieldDataPtr->boolValue = value;

    // Call any registered handlers to be notified of write.
    CallFieldActionHandlers( instanceRef, fieldId, ASSET_DATA_ACTION_WRITE, isClient );

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Get the string value for the specified field
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if field not found
 *      - LE_OVERFLOW if string value was truncated when copied to strBufPtr
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t static GetString
(
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to use
    int fieldId,                                ///< [IN] Field to read
    char* strBufPtr,                            ///< [OUT] The value read
    size_t strBufNumBytes,                      ///< [IN] Size of strBuf
    bool isClient                               ///< [IN] Is it client or server access
)
{
    le_result_t result;
    FieldData_t* fieldDataPtr;

    result = GetFieldFromInstance(instanceRef, fieldId, &fieldDataPtr);
    if ( result != LE_OK )
    {
        return result;
    }

    if ( fieldDataPtr->type != DATA_TYPE_STRING )
    {
        LE_ERROR("Field type mismatch: expected 'string', got '%s'", GetDataTypeStr(fieldDataPtr->type));
        return LE_FAULT;
    }

    // Call any registered handlers to be notified of read
    CallFieldActionHandlers( instanceRef, fieldId, ASSET_DATA_ACTION_READ, isClient );

    // Get the value and return it
    return le_utf8_Copy(strBufPtr, fieldDataPtr->strValuePtr, strBufNumBytes, NULL);
}


//--------------------------------------------------------------------------------------------------
/**
 * Set the string value for the specified field
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if field not found
 *      - LE_OVERFLOW if the stored string was truncated
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t static SetString
(
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to use
    int fieldId,                                ///< [IN] Field to write
    const char* strPtr,                         ///< [IN] The value to write
    bool isClient                               ///< [IN] Is it client or server access
)
{
    le_result_t result;
    FieldData_t* fieldDataPtr;
    char prevStr[STRING_VALUE_NUMBYTES];

    result = GetFieldFromInstance(instanceRef, fieldId, &fieldDataPtr);
    if ( result != LE_OK )
    {
        return result;
    }

    if ( fieldDataPtr->type != DATA_TYPE_STRING )
    {
        LE_ERROR("Field type mismatch: expected 'string', got '%s'", GetDataTypeStr(fieldDataPtr->type));
        return LE_FAULT;
    }

    // Remember current value and set new value.
    result = le_utf8_Copy(prevStr, fieldDataPtr->strValuePtr, STRING_VALUE_NUMBYTES, NULL);
    result = le_utf8_Copy(fieldDataPtr->strValuePtr, strPtr, STRING_VALUE_NUMBYTES, NULL);

    // Call any registered handlers to be notified of write.
    CallFieldActionHandlers( instanceRef, fieldId, ASSET_DATA_ACTION_WRITE, isClient );

    return result;
}


//--------------------------------------------------------------------------------------------------
/**
 * Get a list of the object 9 instances.
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if no object9 instance is found
 *      - LE_OVERFLOW if string value was truncated when copied to strBufPtr
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_GetObj9InstanceList
(
    char* strBufPtr,                            ///< [OUT] The returned list
    size_t strBufNumBytes,                      ///< [IN] Size of strBuf
    int* listNumBytesPtr,                       ///< [OUT] Size of returned list
    int* numInstancePtr                         ///< [OUT] Number of instances
)
{
    const char* nameIdPtr;
    const AssetData_t* assetDataPtr;
    InstanceData_t* assetInstancePtr;

    le_hashmap_It_Ref_t iterRef;
    le_dls_Link_t* linkPtr;
    size_t bytesWritten;
    char tempStr[100];
    char nameStr[100];
    char* delimterPtr = "";

    int instanceCount = 0;

    // These two pointers are used to determine the writable part of the strBuf:
    //  - startBufPtr points to the next writable character
    //  - endBufPtr points past the last character in strBuf
    // The number of characters left is always (endBufPtr-startBufPtr)
    char* startBufPtr = strBufPtr;
    char* endBufPtr = strBufPtr + strBufNumBytes;

    // Write all the asset instances, and if an asset has no instances, then write the asset
    iterRef = le_hashmap_GetIterator(AssetMap);

    while ( le_hashmap_NextNode(iterRef) == LE_OK )
    {
        nameIdPtr = le_hashmap_GetKey(iterRef);
        assetDataPtr = le_hashmap_GetValue(iterRef);

        le_utf8_CopyUpToSubStr(nameStr, nameIdPtr, "/", sizeof(nameStr), NULL);

        if ((0 == (strcmp(nameStr, LWM2M_NAME))) && (LWM2M_OBJ9 == assetDataPtr->assetId))
        {
            // Get the start of the instance list
            linkPtr = le_dls_Peek(&assetDataPtr->instanceList);

            // If the asset has no instances, then just write the asset
            if ( linkPtr == NULL )
            {
                LE_WARN("No object 9 instance");
                return LE_NOT_FOUND;
            }

            // Otherwise, loop through the asset instances
            while ( linkPtr != NULL )
            {
                assetInstancePtr = CONTAINER_OF(linkPtr, InstanceData_t, link);
                FormatString(tempStr,
                             sizeof(tempStr),
                             "%s</%s/%i>",
                             delimterPtr,
                             nameIdPtr,
                             assetInstancePtr->instanceId);
                LE_PRINT_VALUE("%s", tempStr);

                if ( le_utf8_Copy(startBufPtr,
                                  tempStr,
                                  endBufPtr-startBufPtr,
                                  &bytesWritten) != LE_OK )
                {
                    return LE_OVERFLOW;
                }

                instanceCount++;

                // Point to the character after the last one written
                startBufPtr += bytesWritten;

                linkPtr = le_dls_PeekNext(&assetDataPtr->instanceList, linkPtr);

                // Add delimter for next item
                delimterPtr = ",";
            }
            break;
        }
    }

    // Set return values
    *listNumBytesPtr = startBufPtr - strBufPtr;
    *numInstancePtr = instanceCount;

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Get a list of the defined assets and asset instances.
 *
 * The list is returned as a string formatted for QMI_LWM2M_REG_UPDATE_REQ
 *
 * @return:
 *      - LE_OK on success
 *      - LE_OVERFLOW if string value was truncated when copied to strBufPtr
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
static le_result_t GetAssetList
(
    char* strBufPtr,                            ///< [OUT] The returned list
    size_t strBufNumBytes,                      ///< [IN] Size of strBuf
    int* listNumBytesPtr,                       ///< [OUT] Size of returned list
    int* numAssetsPtr                           ///< [OUT] Number of assets + instances
)
{
    const char* nameIdPtr;
    const AssetData_t* assetDataPtr;
    InstanceData_t* assetInstancePtr;

    le_hashmap_It_Ref_t iterRef;
    le_dls_Link_t* linkPtr;
    size_t bytesWritten;
    char tempStr[100];
    char nameStr[100];
    char* namePrefixPtr;

    int assetCount=0;

    // These two pointers are used to determine the writable part of the strBuf:
    //  - startBufPtr points to the next writable character
    //  - endBufPtr points past the last character in strBuf
    // The number of characters left is always (endBufPtr-startBufPtr)
    char* startBufPtr = strBufPtr;
    char* endBufPtr = strBufPtr + strBufNumBytes;

    // Write all the asset instances, and if an asset has no instances, then write the asset
    iterRef = le_hashmap_GetIterator(AssetMap);

    while ( le_hashmap_NextNode(iterRef) == LE_OK )
    {
        nameIdPtr = le_hashmap_GetKey(iterRef);
        assetDataPtr = le_hashmap_GetValue(iterRef);

        // Server expects app names to have "le_" prefix.  The app name is the first part of
        // nameIdPtr, up to the first '/', unless it is "lwm2m" or "legato", which are not apps.
        // TODO: Should the "le_" prefix instead be added to the app name when stored?
        le_utf8_CopyUpToSubStr(nameStr, nameIdPtr, "/", sizeof(nameStr), NULL);
        if ( (strcmp(nameStr, "lwm2m") == 0) || (strcmp(nameStr, "legato") == 0) )
        {
            namePrefixPtr = "";
        }
        else
        {
            namePrefixPtr = "le_";
        }

        // Get the start of the instance list
        linkPtr = le_dls_Peek(&assetDataPtr->instanceList);

        // If the asset has no instances, then just write the asset
        if ( linkPtr == NULL )
        {
            FormatString(tempStr, sizeof(tempStr), "</%s%s>,", namePrefixPtr, nameIdPtr);
            LE_PRINT_VALUE("%s", tempStr);

            if ( le_utf8_Copy(startBufPtr, tempStr, endBufPtr-startBufPtr, &bytesWritten) != LE_OK )
            {
                return LE_OVERFLOW;
            }

            assetCount++;

            // Point to the character after the last one written
            startBufPtr += bytesWritten;
        }

        // Otherwise, loop through the asset instances
        else while ( linkPtr != NULL )
        {
            assetInstancePtr = CONTAINER_OF(linkPtr, InstanceData_t, link);

            FormatString(tempStr,
                         sizeof(tempStr),
                         "</%s%s/%i>,",
                         namePrefixPtr,
                         nameIdPtr,
                         assetInstancePtr->instanceId);
            LE_PRINT_VALUE("%s", tempStr);

            if ( le_utf8_Copy(startBufPtr, tempStr, endBufPtr-startBufPtr, &bytesWritten) != LE_OK )
            {
                return LE_OVERFLOW;
            }

            assetCount++;

            // Point to the character after the last one written
            startBufPtr += bytesWritten;

            linkPtr = le_dls_PeekNext(&assetDataPtr->instanceList, linkPtr);
        }
    }

    // Set return values
    *listNumBytesPtr = startBufPtr - strBufPtr;
    *numAssetsPtr = assetCount;

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Add a handler to be notified on field actions, such as write or execute
 *
 * @return:
 *      - On success, reference for removing handler
 *      - On error, NULL
 */
//--------------------------------------------------------------------------------------------------
static assetData_FieldActionHandlerRef_t AddFieldActionHandler
(
    assetData_AssetDataRef_t assetRef,                  ///< [IN] Asset to monitor
    int fieldId,                                        ///< [IN] Field within asset to monitor
    assetData_FieldActionHandlerFunc_t handlerPtr,      ///< [IN] Handler to call upon action
    void* contextPtr,                                   ///< [IN] User specified context pointer
    bool isClient                                       ///< [IN] Is it client or server access
)
{
    ActionHandlerData_t* newHandlerDataPtr;

    newHandlerDataPtr = le_mem_ForceAlloc(ActionHandlerDataPoolRef);
    newHandlerDataPtr->fieldActionHandlerPtr = handlerPtr;
    newHandlerDataPtr->contextPtr = contextPtr;
    newHandlerDataPtr->fieldId = fieldId;
    newHandlerDataPtr->isClient = isClient;

    newHandlerDataPtr->link = LE_DLS_LINK_INIT;
    le_dls_Queue(&assetRef->fieldActionList, &newHandlerDataPtr->link);

    // return something unique as a reference
    return (assetData_FieldActionHandlerRef_t)newHandlerDataPtr;
}


//--------------------------------------------------------------------------------------------------
/**
 * Add a handler to be notified on asset actions, such as create or delete
 *
 * @return:
 *      - On success, reference for removing handler
 *      - On error, NULL
 */
//--------------------------------------------------------------------------------------------------
static assetData_AssetActionHandlerRef_t AddAssetActionHandler
(
    assetData_AssetDataRef_t assetRef,                  ///< [IN] Asset to monitor
    assetData_AssetActionHandlerFunc_t handlerPtr,      ///< [IN] Handler to call upon action
    void* contextPtr,                                   ///< [IN] User specified context pointer
    bool isClient                                       ///< [IN] Is it client or server access
)
{
    ActionHandlerData_t* newHandlerDataPtr;

    newHandlerDataPtr = le_mem_ForceAlloc(ActionHandlerDataPoolRef);
    newHandlerDataPtr->assetActionHandlerPtr = handlerPtr;
    newHandlerDataPtr->contextPtr = contextPtr;
    newHandlerDataPtr->fieldId = -1;
    newHandlerDataPtr->isClient = isClient;

    newHandlerDataPtr->link = LE_DLS_LINK_INIT;
    le_dls_Queue(&assetRef->assetActionList, &newHandlerDataPtr->link);

    // return something unique as a reference
    return (assetData_AssetActionHandlerRef_t)newHandlerDataPtr;
}


//--------------------------------------------------------------------------------------------------
// Interface functions
//--------------------------------------------------------------------------------------------------
//--------------------------------------------------------------------------------------------------
/**
 * Sends a registration update to the server and also used as a handler to receive
 * UpdateRequired indication. For create, RegistrationUpdate will be done by assetData create
 * function, but for delete, whoever deletes an instance has to explicitly call RegistrationUpdate.
 */
//--------------------------------------------------------------------------------------------------
void assetData_RegistrationUpdate
(
    void
)
{
    LE_ERROR("unsupported function called.");
}

//--------------------------------------------------------------------------------------------------
/**
 * Sends a registration update if observe is not enabled. A registration update would also be sent
 * if the instanceRef is not valid.
 */
//--------------------------------------------------------------------------------------------------
void assetData_RegUpdateIfNotObserved
(
    assetData_InstanceDataRef_t instanceRef    ///< The instance of object 9.
)
{
    LE_ERROR("unsupported function called.");
}



//--------------------------------------------------------------------------------------------------
/**
 * Create a new instance of the given asset. This function will schedule a registration update after
 * 1 second if asset creation is successful. The 1 second delay is used to aggregate multiple
 * registration updates messages.
 *
 * @return:
 *      - LE_OK on success
 *      - LE_DUPLICATE if the specified instanceId already exists
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_CreateInstanceById
(
    const char* appNamePtr,                         ///< [IN] App containing the asset
    int assetId,                                    ///< [IN] Asset id within the App
    int instanceId,                                 ///< [IN] If < 0, then generate an instance id,
                                                    ///       otherwise use the given id.
    assetData_InstanceDataRef_t* instanceRefPtr     ///< [OUT] Reference to created instance
)
{
    le_result_t result;
    AssetData_t* assetDataPtr;
    InstanceData_t* assetInstPtr;

    LE_INFO("Creating asset instance for %s/%i", appNamePtr, assetId);

    // Get an existing AssetData block from AssetMap, or create a new one
    result = GetAssetData(appNamePtr, assetId, &assetDataPtr);
    if ( (result == LE_OK) && (instanceId >= 0) )
    {
        // Make sure it is not a duplicate
        if ( GetInstanceFromAssetData(assetDataPtr, instanceId, &assetInstPtr) == LE_OK )
        {
            return LE_DUPLICATE;
        }
    }

    else if ( result == LE_FAULT )
    {
        return LE_FAULT;
    }

    else if ( result == LE_NOT_FOUND )
    {
        if ( CreateAssetDataFromModel(appNamePtr, assetId, &assetDataPtr) != LE_OK )
        {
            return LE_FAULT;
        }
    }

    // Create instance for AssetData
    // LWM2M objects are hard-coded; the rest are taken from the ConfigDB
    if ( strcmp( "lwm2m", appNamePtr ) == 0 )
    {
        // Allocate instance data and populate most of the instance from the model definition
        // No need to check assetId, since we already know it is valid
        assetInstPtr = le_mem_ForceAlloc(InstanceDataPoolRef);
        CreateInstanceForObjectNine(assetInstPtr);
    }
    else
    {
        le_cfg_IteratorRef_t assetCfg;

        // Open a config read transaction for the asset model
        if ( OpenModelFromConfig(appNamePtr, assetId, &assetCfg) != LE_OK )
        {
            return LE_FAULT;
        }

        // Allocate instance data; will be released if errors are found
        assetInstPtr = le_mem_ForceAlloc(InstanceDataPoolRef);

        // Populate most of the instance from the model definition
        result = CreateInstanceFromModel(assetCfg, assetInstPtr);

        // Regardless of success/failure, stop the transaction
        le_cfg_CancelTxn(assetCfg);

        if ( result != LE_OK )
        {
            // todo: Need to release all allocated fields as well as asset instance
            LE_ERROR("Error in reading model");
            return LE_FAULT;
        }
    }

    // Everything is okay, so finish initializing the instance data, and store it

    // If the instanceId is explicitly given, use it; we already know it is not a duplicate.
    if ( instanceId >= 0 )
    {
        assetInstPtr->instanceId = instanceId;

        // The lastInstanceId will be the higher of the explicitly requested instanceId or the
        // actual last assigned instanceId.
        if ( instanceId > assetDataPtr->lastInstanceId )
        {
            assetDataPtr->lastInstanceId = instanceId;
        }
    }
    else
    {
        assetInstPtr->instanceId = ++assetDataPtr->lastInstanceId;
    }

    // Add back reference from instance data to the asset containing the instance
    assetInstPtr->assetDataPtr = assetDataPtr;

    // If the object is already getting observed, setup the new instance for observe as well.
    if (assetDataPtr->isObjectObserve)
    {
        assetData_SetObserve(assetInstPtr, true, assetDataPtr->token, assetDataPtr->tokenLength);
    }

    le_dls_Queue(&assetDataPtr->instanceList, &assetInstPtr->link);

    // todo: For now, for testing, print it out; add trace support later.
    if ( 0 )
        PrintAssetMap();

    // Return the instance ref
    *instanceRefPtr = assetInstPtr;

    // Call any registered handlers to be notified of instance creation.
    CallAssetActionHandlers(assetDataPtr, assetInstPtr->instanceId, ASSET_DATA_ACTION_CREATE);

    LE_INFO("Finished creating instance %i for %s/%i", assetInstPtr->instanceId, appNamePtr, assetId);

    LE_DEBUG("Schedule a registration update after asset creation.");

    // Start or restart the timer; will only report to the modem when the timer expires.
    le_timer_Restart(RegUpdateTimerRef);

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Create a new instance of the given asset
 *
 * @return:
 *      - LE_OK on success
 *      - LE_DUPLICATE if the specified instanceId already exists
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_CreateInstanceByName
(
    const char* appNamePtr,                         ///< [IN] App containing the asset
    const char* assetNamePtr,                       ///< [IN] Asset name within the App
    int instanceId,                                 ///< [IN] If < 0, then generate an instance id,
                                                    ///       otherwise use the given id.
    assetData_InstanceDataRef_t* instanceRefPtr     ///< [OUT] Reference to created instance
)
{
    le_result_t result;
    AssetData_t* assetDataPtr;
    InstanceData_t* assetInstPtr;

    LE_INFO("Creating asset instance for %s/%s", appNamePtr, assetNamePtr);

    // Get an existing AssetData block from AssetMapByName, or create a new one
    result = GetAssetDataByName(appNamePtr, assetNamePtr, &assetDataPtr);
    if ( (result == LE_OK) && (instanceId >= 0) )
    {
        // Make sure it is not a duplicate
        if ( GetInstanceFromAssetData(assetDataPtr, instanceId, &assetInstPtr) == LE_OK )
        {
            return LE_DUPLICATE;
        }
    }

    else if ( result == LE_FAULT )
    {
        return LE_FAULT;
    }

    else if ( result == LE_NOT_FOUND )
    {
        if ( CreateAssetDataFromModelByName(appNamePtr, assetNamePtr, &assetDataPtr) != LE_OK )
        {
            return LE_FAULT;
        }
    }

    // Now that we've mapped assetName to assetId, create the requested instance.
    return assetData_CreateInstanceById(appNamePtr,
                                        assetDataPtr->assetId,
                                        instanceId,
                                        instanceRefPtr);
}


//--------------------------------------------------------------------------------------------------
/**
 * Delete the given asset instance
 */
//--------------------------------------------------------------------------------------------------
void assetData_DeleteInstance
(
    assetData_InstanceDataRef_t instanceRef         ///< [IN] Asset instance to delete
)
{
    LE_INFO("Deleting instance %s/%i/%i",
            instanceRef->assetDataPtr->appName,
            instanceRef->assetDataPtr->assetId,
            instanceRef->instanceId);

    // Call any registered handlers to be notified before the instance is deleted.
    CallAssetActionHandlers(instanceRef->assetDataPtr,
                            instanceRef->instanceId,
                            ASSET_DATA_ACTION_DELETE);

    FieldData_t* fieldDataPtr;
    le_dls_Link_t* linkPtr;

    // Pop the first field from field list
    linkPtr = le_dls_Pop(&instanceRef->fieldList);

    // Loop through the fields, and release each field.
    while ( linkPtr != NULL )
    {
        fieldDataPtr = CONTAINER_OF(linkPtr, FieldData_t, link);

        // Some field types have allocated data, so release that first
        switch ( fieldDataPtr->type )
        {
            case DATA_TYPE_STRING:
                LE_DEBUG("Deleting string value for field %s", fieldDataPtr->name);
                le_mem_Release(fieldDataPtr->strValuePtr);
                break;

            default:
                break;
        }

        // Release the field
        LE_DEBUG("Deleting field %s", fieldDataPtr->name);
        le_mem_Release(fieldDataPtr);

        linkPtr = le_dls_Pop(&instanceRef->fieldList);
    }

    // Remove the instance from the asset instance list
    le_dls_Remove(&instanceRef->assetDataPtr->instanceList, &instanceRef->link);

    // Lastly, release the instance data.
    le_mem_Release(instanceRef);
}


//--------------------------------------------------------------------------------------------------
/**
 * Delete the given asset instance, and if no more instances, also delete the asset data.
 */
//--------------------------------------------------------------------------------------------------
void assetData_DeleteInstanceAndAsset
(
    assetData_InstanceDataRef_t instanceRef         ///< [IN] Asset instance to delete
)
{
    // Keep reference to asset data containing this instance.
    AssetData_t* assetDataPtr = instanceRef->assetDataPtr;

    // Delete the instance
    assetData_DeleteInstance(instanceRef);

    // If there are no more instances, then also delete the data for this asset
    if ( le_dls_Peek(&assetDataPtr->instanceList) == NULL )
    {
        /*
         * Release all items in fieldActionList
         */

        ActionHandlerData_t* handlerDataPtr;
        le_dls_Link_t* linkPtr;

        // Get the first item from the handler list
        linkPtr = le_dls_Pop(&assetDataPtr->fieldActionList);

        // Loop through the list, deleting each item
        while ( linkPtr != NULL )
        {
            handlerDataPtr = CONTAINER_OF(linkPtr, ActionHandlerData_t, link);
            le_mem_Release(handlerDataPtr);

            linkPtr = le_dls_Pop(&assetDataPtr->fieldActionList);
        }


        /*
         * Release all items in assetActionList
         */

        // Get the first item from the handler list
        linkPtr = le_dls_Pop(&assetDataPtr->assetActionList);

        // Loop through the list, deleting each item
        while ( linkPtr != NULL )
        {
            handlerDataPtr = CONTAINER_OF(linkPtr, ActionHandlerData_t, link);
            le_mem_Release(handlerDataPtr);

            linkPtr = le_dls_Pop(&assetDataPtr->assetActionList);
        }


        /*
         * Remove the asset data from the AssetMaps
         */

        char appNameAssetId[100];
        char* appNameAssetIdKeyPtr;
        char appNameAssetName[100];
        char* appNameAssetNameKeyPtr;

        if ( FormatString(appNameAssetId,
                          sizeof(appNameAssetId),
                          "%s/%i",
                          assetDataPtr->appName,
                          assetDataPtr->assetId) == LE_OK )
        {
            // The key stored in the hashmap also has to be released, so get it first, before
            // removing the data from the hashmap.
            appNameAssetIdKeyPtr = le_hashmap_GetStoredKey(AssetMap, appNameAssetId);
            le_hashmap_Remove(AssetMap, appNameAssetId);
            le_mem_Release(appNameAssetIdKeyPtr);
        }

        if ( FormatString(appNameAssetName,
                          sizeof(appNameAssetId),
                          "%s/%s",
                          assetDataPtr->appName,
                          assetDataPtr->assetName) == LE_OK )
        {
            // The key stored in the hashmap also has to be released, so get it first, before
            // removing the data from the hashmap.
            appNameAssetNameKeyPtr = le_hashmap_GetStoredKey(AssetMapByName, appNameAssetName);
            le_hashmap_Remove(AssetMapByName, appNameAssetName);
            le_mem_Release(appNameAssetNameKeyPtr);
        }


        /*
         * Release the allocated asset data
         */

        le_mem_Release(assetDataPtr);
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Get a reference for the specified asset
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if asset not found
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_GetAssetRefById
(
    const char* appNamePtr,                         ///< [IN] App containing the asset
    int assetId,                                    ///< [IN] Asset id within the App
    assetData_AssetDataRef_t* assetRefPtr           ///< [OUT] Reference to specified asset
)
{
    le_result_t result;

    // Get an existing AssetData block from AssetMap, or create a new one
    result = GetAssetData(appNamePtr, assetId, assetRefPtr);
    if ( result == LE_FAULT )
    {
        return LE_FAULT;
    }
    else if ( result == LE_NOT_FOUND )
    {
        return CreateAssetDataFromModel(appNamePtr, assetId, assetRefPtr);
    }

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Get a reference for the specified asset
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if asset not found
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_GetAssetRefByName
(
    const char* appNamePtr,                         ///< [IN] App containing the asset
    const char* assetNamePtr,                       ///< [IN] Asset name within the App
    assetData_AssetDataRef_t* assetRefPtr           ///< [OUT] Reference to specified asset
)
{
    // IMPLEMENT LATER
    return LE_FAULT;
}


//--------------------------------------------------------------------------------------------------
/**
 * Get a reference for the specified asset instance
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if instance not found
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_GetInstanceRefById
(
    const char* appNamePtr,                         ///< [IN] App containing the asset
    int assetId,                                    ///< [IN] Asset id within the App
    int instanceId,                                 ///< [IN] Instance of the given asset
    assetData_InstanceDataRef_t* instanceRefPtr     ///< [OUT] Reference to specified instance
)
{
    return GetInstance(appNamePtr, assetId, instanceId, instanceRefPtr);
}


//--------------------------------------------------------------------------------------------------
/**
 * Get a reference for the specified asset instance
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if instance not found
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_GetInstanceRefByName
(
    const char* appNamePtr,                         ///< [IN] App containing the asset
    const char* assetNamePtr,                       ///< [IN] Asset name within the App
    int instanceId,                                 ///< [IN] Instance of the given asset
    assetData_InstanceDataRef_t* instanceRefPtr     ///< [OUT] Reference to specified instance
)
{
    // IMPLEMENT LATER
    return LE_FAULT;
}


//--------------------------------------------------------------------------------------------------
/**
 * Get the App name for the specified asset
 *
 * @return:
 *      - LE_OK on success
 *      - LE_FAULT on error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_GetAppNameFromAsset
(
    assetData_AssetDataRef_t assetRef,          ///< [IN] Asset data to use
    char* nameBufPtr,                           ///< [OUT] The App name
    size_t nameBufNumBytes                      ///< [IN] Size of nameBuf
)
{
    return le_utf8_Copy(nameBufPtr, assetRef->appName, nameBufNumBytes, NULL);
}


//--------------------------------------------------------------------------------------------------
/**
 * Get the Asset id for the specified asset
 *
 * @return:
 *      - LE_OK on success
 *      - LE_FAULT on error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_GetAssetIdFromAsset
(
    assetData_AssetDataRef_t assetRef,          ///< [IN] Asset data to use
    int* assetIdPtr                             ///< [OUT] The Asset id
)
{
    *assetIdPtr = assetRef->assetId;

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Get the App name for the specified asset instance
 *
 * @return:
 *      - LE_OK on success
 *      - LE_FAULT on error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_GetAppNameFromInstance
(
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to use
    char* nameBufPtr,                           ///< [OUT] The App name
    size_t nameBufNumBytes                      ///< [IN] Size of nameBuf
)
{
    return assetData_GetAppNameFromAsset(instanceRef->assetDataPtr, nameBufPtr, nameBufNumBytes);
}


//--------------------------------------------------------------------------------------------------
/**
 * Get the Asset id for the specified asset instance
 *
 * @return:
 *      - LE_OK on success
 *      - LE_FAULT on error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_GetAssetIdFromInstance
(
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to use
    int* assetIdPtr                             ///< [OUT] The Asset id
)
{
    return assetData_GetAssetIdFromAsset(instanceRef->assetDataPtr, assetIdPtr);
}


//--------------------------------------------------------------------------------------------------
/**
 * Get a reference to the Asset from the specified asset instance
 *
 * @return:
 *      - LE_OK on success
 *      - LE_FAULT on error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_GetAssetRefFromInstance
(
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to use
    assetData_AssetDataRef_t* assetRefPtr       ///< [OUT] Reference to specified asset
)
{
    *assetRefPtr = instanceRef->assetDataPtr;

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Get the instance id for the specified asset instance
 *
 * @return:
 *      - LE_OK on success
 *      - LE_FAULT on error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_GetInstanceId
(
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to use
    int* instanceIdPtr                          ///< [OUT] The instance id
)
{
    *instanceIdPtr = instanceRef->instanceId;

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Get the field id for the given field name
 *
 * @return:
 *      - LE_OK on success
 *      - LE_FAULT on error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_GetFieldIdFromName
(
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to use
    const char* fieldNamePtr,                   ///< [IN] The field name
    int* fieldIdPtr                             ///< [OUT] The field id
)
{
    /*
     * NOTE:
     *   The main use for this function is to get the fieldId that is then passed to the various
     *   assetData_client_Get* functions.  This is not particularly efficient as it requires
     *   iterating twice through the field list.  One alternative would be to add a new set of
     *   assetData_client_Get* functions that take a field name instead of field id.
     *
     *   For now, assume that the performance is good enough, but if it becomes an issue then
     *   this second set of functions may need to be added.  Of course, there might also be some
     *   other alternative solution that has not yet been considered.
     */

    FieldData_t* fieldDataPtr;
    le_dls_Link_t* fieldLinkPtr;

    // Get the start of the field list
    fieldLinkPtr = le_dls_Peek(&instanceRef->fieldList);

    //LE_PRINT_VALUE("%s", instanceRef->assetDataPtr->appName);
    //LE_PRINT_VALUE("%s", instanceRef->assetDataPtr->assetName);

    // Loop through the fields
    while ( fieldLinkPtr != NULL )
    {
        fieldDataPtr = CONTAINER_OF(fieldLinkPtr, FieldData_t, link);
        //LE_PRINT_VALUE("%s", fieldDataPtr->name);

        if ( strcmp(fieldDataPtr->name, fieldNamePtr) == 0 )
        {
            *fieldIdPtr = fieldDataPtr->fieldId;
            return LE_OK;
        }

        fieldLinkPtr = le_dls_PeekNext(&instanceRef->fieldList, fieldLinkPtr);
    }

    return LE_FAULT;
}


//--------------------------------------------------------------------------------------------------
/**
 * Get the integer value for the specified field
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if field not found
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_client_GetInt
(
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to use
    int fieldId,                                ///< [IN] Field to read
    int* valuePtr                               ///< [OUT] The value read
)
{
    return GetInt(instanceRef, fieldId, valuePtr, true);
}


//--------------------------------------------------------------------------------------------------
/**
 * Get the float value for the specified field
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if field not found
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_client_GetFloat
(
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to use
    int fieldId,                                ///< [IN] Field to read
    double* valuePtr                            ///< [OUT] The value read
)
{
    return GetFloat(instanceRef, fieldId, valuePtr, true);
}


//--------------------------------------------------------------------------------------------------
/**
 * Set the integer value for the specified field
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if field not found
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_client_SetInt
(
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to use
    int fieldId,                                ///< [IN] Field to write
    int value                                   ///< [IN] The value to write
)
{
    return SetInt(instanceRef, fieldId, value, true);
}


//--------------------------------------------------------------------------------------------------
/**
 * Set the float value for the specified field
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if field not found
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_client_SetFloat
(
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to use
    int fieldId,                                ///< [IN] Field to write
    double value                                ///< [IN] The value to write
)
{
    return SetFloat(instanceRef, fieldId, value, true);
}

//--------------------------------------------------------------------------------------------------
/**
 * Get the bool value for the specified field
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if field not found
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_client_GetBool
(
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to use
    int fieldId,                                ///< [IN] Field to read
    bool* valuePtr                              ///< [OUT] The value read
)
{
    return GetBool(instanceRef, fieldId, valuePtr, true);
}


//--------------------------------------------------------------------------------------------------
/**
 * Set the bool value for the specified field
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if field not found
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_client_SetBool
(
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to use
    int fieldId,                                ///< [IN] Field to write
    bool value                                  ///< [IN] The value to write
)
{
    return SetBool(instanceRef, fieldId, value, true);
}


//--------------------------------------------------------------------------------------------------
/**
 * Get the string value for the specified field
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if field not found
 *      - LE_OVERFLOW if string value was truncated when copied to strBufPtr
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_client_GetString
(
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to use
    int fieldId,                                ///< [IN] Field to read
    char* strBufPtr,                            ///< [OUT] The value read
    size_t strBufNumBytes                       ///< [IN] Size of strBuf
)
{
    return GetString(instanceRef, fieldId, strBufPtr, strBufNumBytes, true);
}


//--------------------------------------------------------------------------------------------------
/**
 * Set the string value for the specified field
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if field not found
 *      - LE_OVERFLOW if the stored string was truncated
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_client_SetString
(
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to use
    int fieldId,                                ///< [IN] Field to write
    const char* strPtr                          ///< [IN] The value to write
)
{
    return SetString(instanceRef, fieldId, strPtr, true);
}


//--------------------------------------------------------------------------------------------------
/**
 * Add a handler to be notified on field actions, such as write or execute
 *
 * @return:
 *      - On success, reference for removing handler (with assetData_client_RemoveFieldActionHandler)
 *      - On error, NULL
 */
//--------------------------------------------------------------------------------------------------
assetData_FieldActionHandlerRef_t assetData_client_AddFieldActionHandler
(
    assetData_AssetDataRef_t assetRef,                  ///< [IN] Asset to monitor
    int fieldId,                                        ///< [IN] Field within asset to monitor
    assetData_FieldActionHandlerFunc_t handlerPtr,      ///< [IN] Handler to call upon action
    void* contextPtr                                    ///< [IN] User specified context pointer
)
{
    return AddFieldActionHandler(assetRef, fieldId, handlerPtr, contextPtr, true);
}


//--------------------------------------------------------------------------------------------------
/**
 * Remove a handler previously registered with assetData_client_AddFieldActionHandler
 */
//--------------------------------------------------------------------------------------------------
void assetData_client_RemoveFieldActionHandler
(
    assetData_FieldActionHandlerRef_t handlerRef
)
{
    // IMPLEMENT LATER
}


//--------------------------------------------------------------------------------------------------
/**
 * Add a handler to be notified on asset actions, such as create or delete instance.
 *
 * @return:
 *      - On success, reference for removing handler (with assetData_client_RemoveAssetActionHandler)
 *      - On error, NULL
 */
//--------------------------------------------------------------------------------------------------
assetData_AssetActionHandlerRef_t assetData_client_AddAssetActionHandler
(
    assetData_AssetDataRef_t assetRef,                  ///< [IN] Asset to monitor
    assetData_AssetActionHandlerFunc_t handlerPtr,      ///< [IN] Handler to call upon action
    void* contextPtr                                    ///< [IN] User specified context pointer
)
{
    return AddAssetActionHandler(assetRef, handlerPtr, contextPtr, true);
}


//--------------------------------------------------------------------------------------------------
/**
 * Remove a handler previously registered with assetData_client_AddAssetActionHandler
 */
//--------------------------------------------------------------------------------------------------
void assetData_client_RemoveAssetActionHandler
(
    assetData_AssetActionHandlerRef_t handlerRef
)
{
    // IMPLEMENT LATER
}


//--------------------------------------------------------------------------------------------------
/**
 * Get the integer value for the specified field
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if field not found
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_server_GetInt
(
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to use
    int fieldId,                                ///< [IN] Field to read
    int* valuePtr                               ///< [OUT] The value read
)
{
    return GetInt(instanceRef, fieldId, valuePtr, false);
}


//--------------------------------------------------------------------------------------------------
/**
 * Set the integer value for the specified field
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if field not found
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_server_SetInt
(
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to use
    int fieldId,                                ///< [IN] Field to write
    int value                                   ///< [IN] The value to write
)
{
    return SetInt(instanceRef, fieldId, value, false);
}


//--------------------------------------------------------------------------------------------------
/**
 * Get the bool value for the specified field
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if field not found
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_server_GetBool
(
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to use
    int fieldId,                                ///< [IN] Field to read
    bool* valuePtr                              ///< [OUT] The value read
)
{
    return GetBool(instanceRef, fieldId, valuePtr, false);
}


//--------------------------------------------------------------------------------------------------
/**
 * Set the bool value for the specified field
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if field not found
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_server_SetBool
(
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to use
    int fieldId,                                ///< [IN] Field to write
    bool value                                  ///< [IN] The value to write
)
{
    return SetBool(instanceRef, fieldId, value, false);
}


//--------------------------------------------------------------------------------------------------
/**
 * Get the string value for the specified field
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if field not found
 *      - LE_OVERFLOW if string value was truncated when copied to strBufPtr
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_server_GetString
(
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to use
    int fieldId,                                ///< [IN] Field to read
    char* strBufPtr,                            ///< [OUT] The value read
    size_t strBufNumBytes                       ///< [IN] Size of strBuf
)
{
    return GetString(instanceRef, fieldId, strBufPtr, strBufNumBytes, false);
}


//--------------------------------------------------------------------------------------------------
/**
 * Set the string value for the specified field
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if field not found
 *      - LE_OVERFLOW if the stored string was truncated
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_server_SetString
(
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to use
    int fieldId,                                ///< [IN] Field to write
    const char* strPtr                          ///< [IN] The value to write
)
{
    return SetString(instanceRef, fieldId, strPtr, false);
}


//--------------------------------------------------------------------------------------------------
/**
 * Get the value for the specified field as a string. This function will return LE_UNAVAILABLE, if
 * a callback function is registered for this operation. A response will be sent to the server
 * after the callback function finishes.
 *
 * If the field is not a string field, then the value will be converted to a string.
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if field not found
 *      - LE_UNAVAILABLE if a read call back function is registered
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_server_GetValue
(
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to use
    int fieldId,                                ///< [IN] Field to read
    char* strBufPtr,                            ///< [OUT] The value read
    size_t strBufNumBytes                       ///< [IN] Size of strBuf
)
{
    le_result_t result;
    FieldData_t* fieldDataPtr;

    result = GetFieldFromInstance(instanceRef, fieldId, &fieldDataPtr);
    if ( result != LE_OK )
    {
        return result;
    }

    //LE_PRINT_VALUE("%i", fieldDataPtr->fieldId);
    //LE_PRINT_VALUE("%s", fieldDataPtr->name);

    // If the app has registered a field action handler, the app has to do the work and will send
    // the result later.
    if (IsFieldReadCallBackExist(instanceRef, fieldDataPtr))
    {
        LE_DEBUG("Read call back exists.");

        // Call any registered handlers to be notified of read.
        CallFieldActionHandlers( instanceRef, fieldId, ASSET_DATA_ACTION_READ, false );

        return LE_UNAVAILABLE;
    }

    switch ( fieldDataPtr->type )
    {
        case DATA_TYPE_INT:
            result = FormatString(strBufPtr, strBufNumBytes, "%i", fieldDataPtr->intValue);
            break;

        case DATA_TYPE_BOOL:
            result = FormatString(strBufPtr, strBufNumBytes, "%i", fieldDataPtr->boolValue);
            break;

        case DATA_TYPE_STRING:
            result = le_utf8_Copy(strBufPtr, fieldDataPtr->strValuePtr, strBufNumBytes, NULL);
            break;

        case DATA_TYPE_FLOAT:
            result = FormatString(strBufPtr, strBufNumBytes, "%lf", fieldDataPtr->floatValue);
            break;

        case DATA_TYPE_NONE:
            LE_ERROR("Field is not readable");
            result = LE_FAULT;
            break;
    }

    return result;
}


//--------------------------------------------------------------------------------------------------
/**
 * Set the value for the specified field from a string
 *
 * If the field is not a string field, then the string will be converted to the field type
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if field not found
 *      - LE_OVERFLOW if the stored value was truncated
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_server_SetValue
(
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to use
    int fieldId,                                ///< [IN] Field to write
    const char* strPtr                          ///< [IN] The value to write
)
{
    le_result_t result;
    FieldData_t* fieldDataPtr;

    result = GetFieldFromInstance(instanceRef, fieldId, &fieldDataPtr);
    if ( result != LE_OK )
    {
        return result;
    }

    result = LE_OK;   // result could be changed in the switch statement
    switch ( fieldDataPtr->type )
    {
        case DATA_TYPE_INT:
            errno = 0;
            fieldDataPtr->intValue = strtol(strPtr, NULL, 10);
            if ((errno == ERANGE) || (errno == EINVAL))
            {
                result = LE_FAULT;
            }
            break;

        case DATA_TYPE_BOOL:
            errno = 0;
            fieldDataPtr->boolValue = strtol(strPtr, NULL, 2);
            if ((errno == ERANGE) || (errno == EINVAL))
            {
                result = LE_FAULT;
            }
            break;

        case DATA_TYPE_STRING:
            result = le_utf8_Copy(fieldDataPtr->strValuePtr, strPtr, STRING_VALUE_NUMBYTES, NULL);
            break;

        case DATA_TYPE_FLOAT:
            errno = 0;
            fieldDataPtr->floatValue = strtod(strPtr, NULL);
            if ((errno == ERANGE) || (errno == EINVAL))
            {
                result = LE_FAULT;
            }
            break;

        case DATA_TYPE_NONE:
            LE_ERROR("Field is not writable");
            result = LE_FAULT;
            break;
    }

    // Call any registered handlers to be notified of write.
    // todo: If result is LE_OVERFLOW here, should we still call the registered handlers?
    //       They have no way of knowing that the stored value has overflowed.
    CallFieldActionHandlers( instanceRef, fieldId, ASSET_DATA_ACTION_WRITE, false );

    return result;
}



//--------------------------------------------------------------------------------------------------
/**
 * Perform an execute action on the specified field
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if field not found
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_server_Execute
(
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to use
    int fieldId                                 ///< [IN] Field to execute
)
{
    le_result_t result;
    FieldData_t* fieldDataPtr;

    result = GetFieldFromInstance(instanceRef, fieldId, &fieldDataPtr);
    if ( result != LE_OK )
    {
        return result;
    }

    //LE_PRINT_VALUE("%i", fieldDataPtr->fieldId);
    //LE_PRINT_VALUE("%s", fieldDataPtr->name);
    //LE_PRINT_VALUE("%i", fieldDataPtr->type);
    //LE_PRINT_VALUE("%i", fieldDataPtr->access);

    if ( ! (fieldDataPtr->access & ACCESS_EXEC) )
    {
        LE_ERROR("Field not executable");
        return LE_FAULT;
    }

    // Call any registered handlers to act upon the execute
    CallFieldActionHandlers( instanceRef, fieldId, ASSET_DATA_ACTION_EXEC, false );

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Add a handler to be notified on field actions, such as write or execute
 *
 * @return:
 *      - On success, reference for removing handler (with assetData_server_RemoveFieldActionHandler)
 *      - On error, NULL
 */
//--------------------------------------------------------------------------------------------------
assetData_FieldActionHandlerRef_t assetData_server_AddFieldActionHandler
(
    assetData_AssetDataRef_t assetRef,                  ///< [IN] Asset to monitor
    int fieldId,                                        ///< [IN] Field within asset to monitor
    assetData_FieldActionHandlerFunc_t handlerPtr,      ///< [IN] Handler to call upon action
    void* contextPtr                                    ///< [IN] User specified context pointer
)
{
    return AddFieldActionHandler(assetRef, fieldId, handlerPtr, contextPtr, false);
}


//--------------------------------------------------------------------------------------------------
/**
 * Remove a handler previously registered with assetData_server_AddFieldActionHandler
 */
//--------------------------------------------------------------------------------------------------
void assetData_server_RemoveFieldActionHandler
(
    assetData_FieldActionHandlerRef_t handlerRef
)
{
    // IMPLEMENT LATER
}


//--------------------------------------------------------------------------------------------------
/**
 * Add a handler to be notified on asset actions, such as create or delete instance.
 *
 * @return:
 *      - On success, reference for removing handler (with assetData_server_RemoveAssetActionHandler)
 *      - On error, NULL
 */
//--------------------------------------------------------------------------------------------------
assetData_AssetActionHandlerRef_t assetData_server_AddAssetActionHandler
(
    assetData_AssetDataRef_t assetRef,                  ///< [IN] Asset to monitor
    assetData_AssetActionHandlerFunc_t handlerPtr,      ///< [IN] Handler to call upon action
    void* contextPtr                                    ///< [IN] User specified context pointer
)
{
    return AddAssetActionHandler(assetRef, handlerPtr, contextPtr, false);
}


//--------------------------------------------------------------------------------------------------
/**
 * Remove a handler previously registered with assetData_server_AddAssetActionHandler
 */
//--------------------------------------------------------------------------------------------------
void assetData_server_RemoveAssetActionHandler
(
    assetData_AssetActionHandlerRef_t handlerRef
)
{
    // IMPLEMENT LATER
}


//--------------------------------------------------------------------------------------------------
/**
 * Set handler to be notified on asset actions, such as create or delete instance, for all assets.
 *
 * @todo: For now, only one handler can be registered.  If support for multiple handlers is needed
 *        then this can be added in the future.
 */
//--------------------------------------------------------------------------------------------------
void assetData_server_SetAllAssetActionHandler
(
    assetData_AssetActionHandlerFunc_t handlerPtr,      ///< [IN] Handler to call upon action
    void* contextPtr                                    ///< [IN] User specified context pointer
)
{
    // Store handler and contextPtr; remaining fields in data structure are not used.
    AllAssetActionHandlerData.assetActionHandlerPtr = handlerPtr;
    AllAssetActionHandlerData.contextPtr = contextPtr;
}


//--------------------------------------------------------------------------------------------------
/**
 * Handler function for RegUpdateTimerRef expiry
 */
//--------------------------------------------------------------------------------------------------
static void RegUpdateTimerHandler
(
    le_timer_Ref_t timerRef    ///< This timer has expired
)
{
    LE_INFO("RegUpdate timer expired; reporting REG_UPDATE");

    assetData_RegistrationUpdate();
}


//--------------------------------------------------------------------------------------------------
/**
 * Init this sub-component
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_Init
(
    void
)
{
    // Create the various memory pools
    FieldDataPoolRef = le_mem_CreatePool("Field data pool", sizeof(FieldData_t));
    InstanceDataPoolRef = le_mem_CreatePool("Instance data pool", sizeof(InstanceData_t));
    AssetDataPoolRef = le_mem_CreatePool("Asset data pool", sizeof(AssetData_t));
    ActionHandlerDataPoolRef = le_mem_CreatePool("Action handler data pool",
                                                 sizeof(ActionHandlerData_t));

    StringValuePoolRef = le_mem_CreatePool("String value pool", STRING_VALUE_NUMBYTES);
    AddressStringPoolRef = le_mem_CreatePool("Address pool", 100);

    // Create AssetMap that maps (appName, assetId) to an AssetData block.
    AssetMap = le_hashmap_Create("Asset Map", 31, le_hashmap_HashString, le_hashmap_EqualsString);

    // Create AssetMapByName that maps (appName, assetName) to an AssetData block.
    AssetMapByName = le_hashmap_Create("AssetNameIdMap",
                                       31,
                                       le_hashmap_HashString,
                                       le_hashmap_EqualsString);


    // Use a timer to delay reporting instance creation events to the modem for 1 second after
    // the last creation event.  The timer will only be started when the creation event happens.
    le_clk_Time_t timerInterval = { .sec=1, .usec=0 };

    RegUpdateTimerRef = le_timer_Create("RegUpdate timer");
    le_timer_SetInterval(RegUpdateTimerRef, timerInterval);
    le_timer_SetHandler(RegUpdateTimerRef, RegUpdateTimerHandler);

    // Pre-load the /lwm2m/9 object into the AssetMap; don't actually need to use the assetRef here.
    assetData_AssetDataRef_t lwm2mAssetRef;

    if ( CreateAssetDataFromModel("lwm2m", 9, &lwm2mAssetRef) != LE_OK )
    {
        LE_FATAL("Failed to add '/lwm2m/9' to AssetMap");
    }

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * QMI Specific Functions
 *
 * The remaining functions below are for generating or reading data related to QMI messages.
 * Eventually, these may be moved into lwm2m.c, but are here for now, because they have to iterate
 * over the AssetData instances and fields.  Until an appropriate iteration interface is provided,
 * they need direct access to the data.
 */
//--------------------------------------------------------------------------------------------------

#include <arpa/inet.h>


//--------------------------------------------------------------------------------------------------
/**
 * Supported LWM2M TLV types
 *
 * The values are those given in the LWM2M Spec
 */
//--------------------------------------------------------------------------------------------------
typedef enum
{
    TLV_TYPE_OBJ_INST = 0x00,
    TLV_TYPE_RESOURCE = 0x03,
}
TLVTypes_t;


//--------------------------------------------------------------------------------------------------
/**
 * Write an integer of the given size to the buffer in network byte order
 *
 * It is up to the caller to ensure the buffer is large enough.
 */
//--------------------------------------------------------------------------------------------------
static void WriteUint(uint8_t* dataPtr, uint32_t value, int numBytes)
{
    int i;

    uint32_t networkValue = htonl(value);
    uint8_t* networkValuePtr = ((uint8_t*)&networkValue) + (4-numBytes);

    for (i=0; i<numBytes; i++)
    {
        *dataPtr++ = *networkValuePtr++;
    }
}



//--------------------------------------------------------------------------------------------------
/**
 * Write an double value in network byte order.
 *
 * It is up to the caller to ensure the buffer is large enough.
 */
//--------------------------------------------------------------------------------------------------
static void WriteDouble(uint8_t* dataPtr, double value)
{
    int i;

    double networkValue = value;
    uint8_t* networkValuePtr = ((uint8_t*)&networkValue) + 7;

    for (i=0; i<8; i++)
    {
        *dataPtr++ = *networkValuePtr--;
    }
}



//--------------------------------------------------------------------------------------------------
/**
 * Read an double in network byte order from the buffer.
 */
//--------------------------------------------------------------------------------------------------
static void ReadDouble(uint8_t* dataPtr, double* valuePtr, uint32_t numBytes)
{
    int i;
    float fValue = 0.0f;
    double dValue = 0.0;

    if (numBytes == 4)
    {
        uint8_t* networkValuePtr = ((uint8_t*)&fValue) + 3;
        for (i=0; i<4; i++)
        {
            *networkValuePtr-- = *dataPtr++;
        }

        dValue = (double)fValue;
    }
    else
    {
        uint8_t* networkValuePtr = ((uint8_t*)&dValue) + 7;

        for (i=0; i<8; i++)
        {
            *networkValuePtr-- = *dataPtr++;
        }
    }

    *valuePtr = dValue;
}

//--------------------------------------------------------------------------------------------------
/**
 * Write a LWM2M TLV header to the given buffer.
 *
 * @return:
 *      - LE_OK on success
 *      - LE_OVERFLOW if the header could not fit in the buffer
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
static le_result_t WriteTLVHeader
(
    TLVTypes_t type,                    ///< [IN] Type of TLV
    int id,                             ///< [IN] Object instance or resource id
    int valueNumBytes,                  ///< [IN] # bytes for TLV value (written separately)
    uint8_t* bufPtr,                    ///< [OUT] Buffer for writing the header
    size_t bufNumBytes,                 ///< [IN] Size of buffer
    size_t* numBytesWrittenPtr          ///< [OUT] # bytes written to buffer.
)
{
    // Pack the TLV type
    int typeByte = type << 6;

    // Is the id 8 or 16 bits long
    int idNumBytes = 1;
    if ( id > 255 )
    {
        typeByte |= 1 << 5;
        idNumBytes = 2;
    }

    // Determine how length of the value is specified; either directly encoded in typeByte or
    // explicitly given in the header.
    int lengthFieldNumBytes;
    if ( valueNumBytes < 8 )
    {
        lengthFieldNumBytes = 0x0;
        typeByte |= ( valueNumBytes );
    }
    else if ( valueNumBytes < (1<<8) )
        lengthFieldNumBytes = 0x1;
    else if ( valueNumBytes < (1<<16) )
        lengthFieldNumBytes = 0x2;
    else if ( valueNumBytes < (1<<24) )
        lengthFieldNumBytes = 0x3;
    else
        // Value length is too large
        return LE_FAULT;

    typeByte |= lengthFieldNumBytes << 3;

    // Header length is one for typeByte, plus size of id and length fields, so can be anywhere
    // from 2 bytes to 6 bytes.
    if ( (1 + idNumBytes + lengthFieldNumBytes) >  bufNumBytes )
        return LE_OVERFLOW;

    // Copy the header to the output buffer
    *bufPtr++ = typeByte;

    WriteUint(bufPtr, id, idNumBytes);
    bufPtr += idNumBytes;

    if ( lengthFieldNumBytes > 0 )
        WriteUint(bufPtr, valueNumBytes, lengthFieldNumBytes);

    // Return the number of bytes written
    *numBytesWrittenPtr = 1 + idNumBytes + lengthFieldNumBytes;

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Write a LWM2M Resource TLV to the given buffer.
 *
 * @return:
 *      - LE_OK on success
 *      - LE_OVERFLOW if the TLV data could not fit in the buffer
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
static le_result_t WriteFieldTLV
(
    assetData_InstanceDataRef_t instRef,    ///< [IN] Asset instance containing field to write
    FieldData_t* fieldDataPtr,              ///< [IN] The field to write to the TLV
    uint8_t* bufPtr,                        ///< [OUT] Buffer for writing the TLV
    size_t bufNumBytes,                     ///< [IN] Size of buffer
    size_t* numBytesWrittenPtr              ///< [OUT] # bytes written to buffer.
)
{
    le_result_t result = LE_OK;
    size_t numBytesWritten;
    int strLength;

    // Provide enough space for max field size, which is 256 bytes for a string, plus max header
    // size, which is 6 bytes.  Thus will ensure overflow doesn't happen when putting data in
    // tmpBuffer.  Overflow will be checked when trying to copy to the output buffer in bufPtr.
    uint8_t tmpBuffer[256+6];
    uint8_t* tmpBufferPtr = tmpBuffer;

    switch ( fieldDataPtr->type )
    {
        case DATA_TYPE_INT:
            WriteTLVHeader(TLV_TYPE_RESOURCE,
                           fieldDataPtr->fieldId,
                           4,
                           tmpBufferPtr,
                           sizeof(tmpBuffer),
                           &numBytesWritten);
            tmpBufferPtr += numBytesWritten;
            WriteUint(tmpBufferPtr, fieldDataPtr->intValue, 4);

            numBytesWritten += 4;
            break;

        case DATA_TYPE_BOOL:
            WriteTLVHeader(TLV_TYPE_RESOURCE,
                           fieldDataPtr->fieldId,
                           1,
                           tmpBufferPtr,
                           sizeof(tmpBuffer),
                           &numBytesWritten);
            tmpBufferPtr += numBytesWritten;
            WriteUint(tmpBufferPtr, fieldDataPtr->boolValue, 1);

            numBytesWritten += 1;
            break;

        case DATA_TYPE_STRING:
            strLength = strlen(fieldDataPtr->strValuePtr);

            WriteTLVHeader(TLV_TYPE_RESOURCE,
                           fieldDataPtr->fieldId,
                           strLength,
                           tmpBufferPtr,
                           sizeof(tmpBuffer),
                           &numBytesWritten);

            tmpBufferPtr += numBytesWritten;

            result = le_utf8_Copy((char*)tmpBufferPtr,
                                  fieldDataPtr->strValuePtr,
                                  sizeof(tmpBuffer)-numBytesWritten,
                                  NULL);

            // Assumes no overflow; that will be checked below
            numBytesWritten += strLength;
            break;

        case DATA_TYPE_FLOAT:
            WriteTLVHeader(TLV_TYPE_RESOURCE,
                           fieldDataPtr->fieldId,
                           8,
                           tmpBufferPtr,
                           sizeof(tmpBuffer),
                           &numBytesWritten);
            tmpBufferPtr += numBytesWritten;

            WriteDouble(tmpBufferPtr, fieldDataPtr->floatValue);

            numBytesWritten += 8;
            break;

        case DATA_TYPE_NONE:
            LE_ERROR("No data to read");
            result = LE_FAULT;
            *numBytesWrittenPtr = 0;
            break;
    }

    // Successfully got the data, so copy to output buffer, if there is room.
    if ( result == LE_OK )
    {
        if ( numBytesWritten <= bufNumBytes )
        {
            memcpy(bufPtr, tmpBuffer, numBytesWritten);
            *numBytesWrittenPtr = numBytesWritten;
        }
        else
        {
            LE_WARN("Overflow: oiid=%i, rid=%i", instRef->instanceId, fieldDataPtr->fieldId);
            *numBytesWrittenPtr = 0;
            result = LE_OVERFLOW;
        }
    }

    return result;
}


//--------------------------------------------------------------------------------------------------
/**
 * Write a list of readable LWM2M Resource TLVs to the given buffer.
 *
 * @return:
 *      - LE_OK on success
 *      - LE_OVERFLOW if the TLV data could not fit in the buffer
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_WriteFieldListToTLV
(
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to use
    uint8_t* bufPtr,                            ///< [OUT] Buffer for writing the TLV list
    size_t bufNumBytes,                         ///< [IN] Size of buffer
    size_t* numBytesWrittenPtr                  ///< [OUT] # bytes written to buffer.
)
{
    le_result_t result;

    uint8_t* startBufPtr = bufPtr;
    uint8_t* endBufPtr = bufPtr+bufNumBytes;

    le_dls_Link_t* linkPtr;
    FieldData_t* fieldDataPtr;
    size_t fieldNumBytesWritten;

    // Get the start of the field list
    linkPtr = le_dls_Peek(&instanceRef->fieldList);

    // Loop through the fields
    while ( linkPtr != NULL )
    {
        fieldDataPtr = CONTAINER_OF(linkPtr, FieldData_t, link);

        // The access values are from the client perspective, so we can read whatever fields
        // the client can write.
        if ( fieldDataPtr->access & ACCESS_WRITE )
        {
            result = WriteFieldTLV(instanceRef,
                                   fieldDataPtr,
                                   startBufPtr,
                                   endBufPtr-startBufPtr,
                                   &fieldNumBytesWritten);

            if ( result != LE_OK )
            {
                return result;
            }

            startBufPtr += fieldNumBytesWritten;
        }

        linkPtr = le_dls_PeekNext(&instanceRef->fieldList, linkPtr);
    }

    *numBytesWrittenPtr = startBufPtr - bufPtr;
    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Write a LWM2M Object Instance TLV to the given buffer.
 *
 * @return:
 *      - LE_OK on success
 *      - LE_OVERFLOW if the TLV data could not fit in the buffer
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
static le_result_t WriteInstanceToTLV
(
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to use
    int fieldId,                                ///< [IN] Field to write, or -1 for all fields
    uint8_t* bufPtr,                            ///< [OUT] Buffer for writing the object instance
    size_t bufNumBytes,                         ///< [IN] Size of buffer
    size_t* numBytesWrittenPtr                  ///< [OUT] # bytes written to buffer.
)
{
    le_result_t result;
    FieldData_t* fieldDataPtr;
    size_t totalNumBytesWritten;
    size_t numBytesWritten;
    uint8_t tmpBuffer[256-6];  // leave enough space for maximum header size of 6 bytes

    // Need to write the field TLVs first, to know how many bytes will be in the instance TLV.
    // Either read all the allowable TLVs, or just the one specified.
    if ( fieldId == -1 )
    {
        // Read all fields that are allowed and write to the TLV.
        result = assetData_WriteFieldListToTLV(instanceRef,
                                               tmpBuffer,
                                               sizeof(tmpBuffer),
                                               &totalNumBytesWritten);
        if ( result != LE_OK )
        {
            return result;
        }
    }
    else
    {
        result = GetFieldFromInstance(instanceRef, fieldId, &fieldDataPtr);
        if ( result != LE_OK )
            return result;

        WriteFieldTLV(instanceRef,
                      fieldDataPtr,
                      tmpBuffer,
                      sizeof(tmpBuffer),
                      &totalNumBytesWritten);
    }

    // If there is room in the output buffer, write the instance TLV to it.  Ensure that all the
    // TLV data will fit, plus 6 bytes for header.
    if ( totalNumBytesWritten + 6 <= bufNumBytes )
    {
        WriteTLVHeader(TLV_TYPE_OBJ_INST,
                       instanceRef->instanceId,
                       totalNumBytesWritten,
                       bufPtr,
                       bufNumBytes,
                       &numBytesWritten);

        bufPtr += numBytesWritten;
        bufNumBytes -= numBytesWritten;

        memcpy(bufPtr, tmpBuffer, totalNumBytesWritten);
        *numBytesWrittenPtr = numBytesWritten+totalNumBytesWritten;

        result = LE_OK;
    }
    else
    {
        LE_WARN("Overflow: oiid=%i, rid=%i", instanceRef->instanceId, fieldId);
        *numBytesWrittenPtr = 0;
        result = LE_OVERFLOW;
    }

    return result;
}


//--------------------------------------------------------------------------------------------------
/**
 * Write TLV with all instances of the LWM2M Object to the given buffer.
 *
 * @return:
 *      - LE_OK on success
 *      - LE_OVERFLOW if the TLV data could not fit in the buffer
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_WriteObjectToTLV
(
    assetData_AssetDataRef_t assetRef,          ///< [IN] Asset to use
    int fieldId,                                ///< [IN] Field to write, or -1 for all fields
    uint8_t* bufPtr,                            ///< [OUT] Buffer for writing the header
    size_t bufNumBytes,                         ///< [IN] Size of buffer
    size_t* numBytesWrittenPtr                  ///< [OUT] # bytes written to buffer.
)
{
    le_result_t result;
    size_t numBytesWritten;
    le_dls_Link_t* linkPtr;
    InstanceData_t* instancePtr;

    // total bytes written will be (bufPtr-startBufPtr)
    uint8_t* startBufPtr = bufPtr;

    // buffer size will be (endBufPtr-bufPtr);
    uint8_t* endBufPtr = bufPtr+bufNumBytes;

    // Get the start of the instance list
    linkPtr = le_dls_Peek(&assetRef->instanceList);

    // Loop through the asset instances
    while ( linkPtr != NULL )
    {
        instancePtr = CONTAINER_OF(linkPtr, InstanceData_t, link);

        result = WriteInstanceToTLV(instancePtr,
                                    fieldId,
                                    bufPtr,
                                    endBufPtr-bufPtr,
                                    &numBytesWritten);
        if ( result != LE_OK )
            return result;

        bufPtr += numBytesWritten;

        linkPtr = le_dls_PeekNext(&assetRef->instanceList, linkPtr);
    }

    *numBytesWrittenPtr = bufPtr-startBufPtr;
    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 *  Write TLV for an object but include only the instance/resource which changed. This type of
 *  response is needed as the server sends notify on entire object, but we need to notify changes
 *  at resource level.
 *
 *  @return:
 *      - LE_OK on success
 *      - LE_FAULT on error
 */
//--------------------------------------------------------------------------------------------------
static le_result_t WriteNotifyObjectToTLV
(
    assetData_AssetDataRef_t assetRef,          ///< [IN] Asset to use
    int instanceId,                             ///< [IN] Instance that has a changed resource
    int fieldId,                                ///< [IN] The resource which changed
    uint8_t* bufPtr,                            ///< [OUT] Buffer for writing the TLV list
    size_t bufNumBytes,                         ///< [IN] Size of buffer
    size_t* numBytesWrittenPtr                  ///< [OUT] # bytes written to buffer.
)
{
    le_result_t result = LE_OK;
    InstanceData_t* instancePtr;

    LE_DEBUG("instanceId = %d", instanceId);
    LE_DEBUG("fieldId = %d", fieldId);

    result = GetInstanceFromAssetData(assetRef, instanceId, &instancePtr);

    if (result != LE_OK)
    {
        LE_ERROR("Error reading instance reference result = %d.", result);
        return LE_FAULT;
    }

    result = WriteInstanceToTLV(instancePtr,
                                fieldId,
                                bufPtr,
                                bufNumBytes,
                                numBytesWrittenPtr);
    if (result != LE_OK)
    {
        LE_ERROR("Error while setting asset instance result = %d.", result);
        return LE_FAULT;
    }

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Read an integer of the given size and in network byte order from the buffer
 */
//--------------------------------------------------------------------------------------------------
static void ReadUint(uint8_t* dataPtr, uint32_t* valuePtr, int numBytes)
{
    int i;

    uint32_t networkValue=0;
    uint8_t* networkValuePtr = ((uint8_t*)&networkValue) + (4-numBytes);

    for (i=0; i<numBytes; i++)
    {
        *networkValuePtr++ = *dataPtr++;
    }

   *valuePtr = ntohl(networkValue);
}


//--------------------------------------------------------------------------------------------------
/**
 * Read a LWM2M TLV header from the given buffer.
 *
 * @return:
 *      - LE_OK on success
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
static le_result_t ReadTLVHeader
(
    uint8_t* bufPtr,                    ///< [IN] Buffer for reading the header
    uint32_t* numBytesHeaderPtr,        ///< [OUT] # bytes of the header
    TLVTypes_t* typePtr,                ///< [OUT] Type of TLV
    uint32_t* idPtr,                    ///< [OUT] Object instance or resource id
    uint32_t* valueNumBytesPtr          ///< [OUT] # bytes for TLV value (read separately)
)
{
    // Get the type byte
    int typeByte = *bufPtr++;

    // Unpack the TLV type
    *typePtr = (typeByte >> 6) & 0x03;

    // Is the id 8 or 16 bits long
    int idNumBytes = 1;
    if ( (typeByte >> 5) & 0x01 )
    {
        idNumBytes = 2;
    }

    // Get the id
    ReadUint(bufPtr, idPtr, idNumBytes);
    bufPtr += idNumBytes;

    // Determine how length of the value is specified; either directly encoded in typeByte or
    // explicitly given in the header.
    int lengthFieldNumBytes = (typeByte >> 3) & 0x03;

    if ( lengthFieldNumBytes == 0 )
    {
        // Length of the value is directly encoded in typeByte
        *valueNumBytesPtr = typeByte & 0x07;
    }
    else
    {
        ReadUint(bufPtr, valueNumBytesPtr, lengthFieldNumBytes);
    }

    // Return the number of bytes in the header
    *numBytesHeaderPtr = 1 + idNumBytes + lengthFieldNumBytes;

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Read a LWM2M Resource TLV value from the given buffer and write to the given instance
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if field not found
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
static le_result_t ReadFieldValueFromTLV
(
    uint8_t* bufPtr,                            ///< [IN] Buffer for reading the TLV value
    uint32_t valueNumBytes,                     ///< [IN] # bytes in TLV value
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to write the field
    uint32_t fieldId                            ///< [IN] Field to write from TLV value
)
{
    le_result_t result;

    // Field data to write from TLV
    FieldData_t* fieldDataPtr;

    result = GetFieldFromInstance(instanceRef, fieldId, &fieldDataPtr);
    if ( result != LE_OK )
        return result;

    // Update the field value from the TLV; note that result must be LE_OK here.
    switch ( fieldDataPtr->type )
    {
        case DATA_TYPE_INT:
            if ( (valueNumBytes != 1) && (valueNumBytes != 2) && (valueNumBytes != 4) )
            {
                LE_ERROR("Invalid value length = %i", valueNumBytes);
                result = LE_FAULT;
            }
            else
            {
                ReadUint(bufPtr, (uint32_t*)&fieldDataPtr->intValue, valueNumBytes);
            }
            break;

        case DATA_TYPE_BOOL:
            if ( valueNumBytes != 1 )
            {
                LE_ERROR("Invalid value length = %i", valueNumBytes);
                result = LE_FAULT;
            }
            else
            {
                ReadUint(bufPtr, (uint32_t*)&fieldDataPtr->boolValue, 1);
            }
            break;

        case DATA_TYPE_STRING:
            if ( valueNumBytes > (STRING_VALUE_NUMBYTES-1) )
            {
                LE_ERROR("Invalid value length = %i", valueNumBytes);
                result = LE_FAULT;
            }
            else
            {
                // valueNumBytes is guaranteed to be less than strValuePtr size,
                // so just copy the complete value string, and null terminate it.
                memcpy(fieldDataPtr->strValuePtr, bufPtr, valueNumBytes);
                fieldDataPtr->strValuePtr[valueNumBytes] = '\0';
            }
            break;

        case DATA_TYPE_FLOAT:
            if ( (valueNumBytes != 4) && (valueNumBytes != 8) )
            {
                LE_ERROR("Invalid value length = %i", valueNumBytes);
                result = LE_FAULT;
            }
            else
            {
                ReadDouble(bufPtr, &fieldDataPtr->floatValue, valueNumBytes);
            }
            break;

        case DATA_TYPE_NONE:
            LE_ERROR("Write not allowed for fieldId = %i", fieldId);
            result = LE_FAULT;
            break;
    }

    return result;
}


//--------------------------------------------------------------------------------------------------
/**
 * Read a list of LWM2M Resource TLVs from the given buffer and write to the given instance
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if field not found
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_ReadFieldListFromTLV
(
    uint8_t* bufPtr,                            ///< [IN] Buffer for reading the TLV list
    size_t bufNumBytes,                         ///< [IN] # bytes in the buffer
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to write the fields
    bool isCallHandlers                         ///< [IN] Call field callback handlers?
)
{
    le_result_t result = LE_OK;
    uint8_t* endBufPtr = bufPtr + bufNumBytes;

    // Info to be read from the header
    uint32_t numBytesHeader;
    TLVTypes_t type;
    uint32_t fieldId;
    uint32_t valueNumBytes;

    // Keep reading through the buffer until we get to the end of the buffer, or we get to a
    // non Resource/Field TLV, which probably indicates an error.
    while ( bufPtr < endBufPtr )
    {
        result = ReadTLVHeader(bufPtr, &numBytesHeader, &type, &fieldId, &valueNumBytes);
        if (result != LE_OK)
            break;

        if (type == TLV_TYPE_RESOURCE)
        {
            // Skip over the header and point to the start of the data
            bufPtr += numBytesHeader;

            result = ReadFieldValueFromTLV(bufPtr, valueNumBytes, instanceRef, fieldId);
            if (result != LE_OK)
                break;

            if (isCallHandlers)
            {
                // Call any registered handlers to be notified of write.
                CallFieldActionHandlers( instanceRef, fieldId, ASSET_DATA_ACTION_WRITE, false );
            }

            // Skip over the value just read, and point to next TLV
            bufPtr += valueNumBytes;
        }
        else
        {
            LE_DEBUG("Got unexpected TLV type = %i", type);
            result = LE_FAULT;
            break;
        }
    }

    return result;
}



//--------------------------------------------------------------------------------------------------
/**
 * Enables or Disables a field for observe.
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_SetObserve
(
    assetData_InstanceDataRef_t instanceRef,    ///< [IN] Asset instance to use
    bool isObserve,                             ///< [IN] Start or stop observing?
    uint8_t *tokenPtr,                          ///< [IN] Token i.e, request id of the observe request
    uint8_t tokenLength                         ///< [IN] Token Length
)
{
    le_result_t result = LE_NOT_FOUND;
    le_dls_Link_t* linkPtr;
    FieldData_t* fieldDataPtr;

    // Get the start of the field list
    linkPtr = le_dls_Peek(&instanceRef->fieldList);

    // Loop through the fields
    while ( linkPtr != NULL )
    {
        fieldDataPtr = CONTAINER_OF(linkPtr, FieldData_t, link);

        // Set the observe field to true for write fields.
        // The write attribute is from the clients perspective.
        if ( fieldDataPtr->access & ACCESS_WRITE )
        {
            LE_DEBUG("Setting observe on resource %d", fieldDataPtr->fieldId);

            fieldDataPtr->isObserve = isObserve;

            if (isObserve && (tokenLength > 0))
            {
                fieldDataPtr->tokenLength = tokenLength;
                memcpy(fieldDataPtr->token, tokenPtr, tokenLength);
            }
            result = LE_OK;
        }

        linkPtr = le_dls_PeekNext(&instanceRef->fieldList, linkPtr);
    }

    return result;
}


//--------------------------------------------------------------------------------------------------
/**
 * Is Observe flag set for object9 state and result fields.
 *
 * @return:
 *      - true if the flags are set
 *      - false if the flag is able to read the flags or if the flags are not set
 */
//--------------------------------------------------------------------------------------------------
bool assetData_IsObject9Observed
(
    assetData_InstanceDataRef_t obj9InstRef
)
{
    le_result_t result = LE_NOT_FOUND;
    FieldData_t* fieldDataPtr;

    bool stateObserve;
    bool resultObserve;

    result = GetFieldFromInstance(obj9InstRef, 7, &fieldDataPtr);

    if (result != LE_OK)
        return false;

    stateObserve = fieldDataPtr->isObserve;

    result = GetFieldFromInstance(obj9InstRef, 9, &fieldDataPtr);

    if (result != LE_OK)
        return false;

    resultObserve = fieldDataPtr->isObserve;

    return (stateObserve && resultObserve);
}



//--------------------------------------------------------------------------------------------------
/**
 * Cancels observe on entire asset map.
 */
//--------------------------------------------------------------------------------------------------
void assetData_CancelAllObserve
(
    void
)
{
    const char* nameIdPtr;
    AssetData_t* assetDataPtr;
    InstanceData_t* assetInstancePtr;
    le_dls_Link_t* linkPtr;

    le_hashmap_It_Ref_t iterRef = le_hashmap_GetIterator(AssetMap);

    while ( le_hashmap_NextNode(iterRef) == LE_OK )
    {
        nameIdPtr = le_hashmap_GetKey(iterRef);
        assetDataPtr = (AssetData_t*) le_hashmap_GetValue(iterRef);

        // Turn off observe on this object.
        assetDataPtr->isObjectObserve = false;

        // Print out asset data block, and all its instances.
        PRINT_VALUE(0, "%s", nameIdPtr);
        PRINT_VALUE(0, "%i", assetDataPtr->assetId);
        PRINT_VALUE(0, "'%s'", assetDataPtr->assetName);

        // Get the start of the instance list
        linkPtr = le_dls_Peek(&assetDataPtr->instanceList);

        // Loop through the asset instances
        while ( linkPtr != NULL )
        {
            assetInstancePtr = CONTAINER_OF(linkPtr, InstanceData_t, link);

            LE_DEBUG("Cancel observe on instance = %d.", assetInstancePtr->instanceId);

            // Cancel observe in an asset instance.
            assetData_SetObserve(assetInstancePtr, false, NULL, 0);

            linkPtr = le_dls_PeekNext(&assetDataPtr->instanceList, linkPtr);
        }
    }
}



//--------------------------------------------------------------------------------------------------
/**
 * Set Observe on all instances.
 *
 * @return:
 *      - LE_OK on success
 *      - LE_FAULT on error
 */
//--------------------------------------------------------------------------------------------------
le_result_t assetData_SetObserveAllInstances
(
    assetData_AssetDataRef_t assetRef,          ///< [IN] Asset to use
    bool isObserve,                             ///< [IN] true = Observe; false = Cancel Observe
    uint8_t *tokenPtr,                          ///< [IN] Token
    uint8_t tokenLength                         ///< [IN] Token Length
)
{
    le_result_t result;
    le_dls_Link_t* linkPtr;
    InstanceData_t* instancePtr;

    // Get the start of the instance list
    linkPtr = le_dls_Peek(&assetRef->instanceList);

    // Loop through the asset instances
    while ( linkPtr != NULL )
    {
        instancePtr = CONTAINER_OF(linkPtr, InstanceData_t, link);

        LE_DEBUG("Set Observe on instance %d", instancePtr->instanceId);

        result = assetData_SetObserve(instancePtr, isObserve, (uint8_t*)tokenPtr, tokenLength);

        if ( result != LE_OK )
            return LE_FAULT;

        linkPtr = le_dls_PeekNext(&assetRef->instanceList, linkPtr);
    }

    // This object has at least one observable resource. Set a global flag to indicate this Object
    // is getting Observed and copy the token. This token will be used by new instances.
    assetRef->isObjectObserve = isObserve;
    assetRef->tokenLength = tokenLength;
    memcpy(assetRef->token, tokenPtr, tokenLength);

    return LE_OK;
}


/**
 * @file avData.c
 *
 * Implementation of the avdata API.
 *
 * <hr>
 *
 * Copyright (C) Sierra Wireless Inc.
 *
 */

#include <lwm2mcore/lwm2mcore.h>
#include <lwm2mcore/coapHandlers.h>
#include "tinycbor/cbor.h"
#include "legato.h"
#include "interfaces.h"
#include "timeseriesData.h"
#include "avcServer.h"
#include "le_print.h"


//--------------------------------------------------------------------------------------------------
/**
 * Maximum expected number of asset data.
 */
//--------------------------------------------------------------------------------------------------
#define MAX_EXPECTED_ASSETDATA 10000


//--------------------------------------------------------------------------------------------------
/**
 * Buffer size in bytes for a CBOR decoder.
 */
//--------------------------------------------------------------------------------------------------
#define CBOR_DECODER_BUFFER_BYTES 1024


//--------------------------------------------------------------------------------------------------
/**
 * Maximum bytes that can be pushed to the server.
 */
//--------------------------------------------------------------------------------------------------
#define MAX_PUSH_BUFFER_BYTES 20000


//--------------------------------------------------------------------------------------------------
/**
 * Map containing asset data.
 */
//--------------------------------------------------------------------------------------------------
static le_hashmap_Ref_t AssetDataMap;


//--------------------------------------------------------------------------------------------------
/**
 * Map containing safe refs of resource event handlers.
 */
//--------------------------------------------------------------------------------------------------
static le_ref_MapRef_t ResourceEventHandlerMap;


//--------------------------------------------------------------------------------------------------
/**
 * Map containing safe refs of argument lists (for resource event handlers).
 */
//--------------------------------------------------------------------------------------------------
static le_ref_MapRef_t ArgListRefMap;


//--------------------------------------------------------------------------------------------------
/**
 * Asset path memory pool.
 */
//--------------------------------------------------------------------------------------------------
static le_mem_PoolRef_t AssetPathPool;


//--------------------------------------------------------------------------------------------------
/**
 * Asset data memory pool.
 */

//--------------------------------------------------------------------------------------------------
static le_mem_PoolRef_t AssetDataPool;


//--------------------------------------------------------------------------------------------------
/**
 * String memory pool.
 */
//--------------------------------------------------------------------------------------------------
static le_mem_PoolRef_t StringPool;


//--------------------------------------------------------------------------------------------------
/**
 * Argument memory pool.
 */
//--------------------------------------------------------------------------------------------------
static le_mem_PoolRef_t ArgumentPool;


//--------------------------------------------------------------------------------------------------
/**
 * List of taboo first level path names, to avoid path names resembling standard lwm2m paths.
 */
//--------------------------------------------------------------------------------------------------
static const char* InvalidFirstLevelPathNames[] =
{
    "0",
    "1",
    "2",
    "3",
    "4",
    "5",
    "6",
    "7",
    "8",
    "9",
    "10241",
    "10242",
    "10243"
};


//--------------------------------------------------------------------------------------------------
/**
 * AVC client session instance reference.
 */
//--------------------------------------------------------------------------------------------------
static lwm2mcore_Ref_t AVCClientSessionInstanceRef;


//--------------------------------------------------------------------------------------------------
/**
 * AV server request ref.
 */
//--------------------------------------------------------------------------------------------------
static lwm2mcore_CoapRequestRef_t AVServerReqRef;


//--------------------------------------------------------------------------------------------------
/**
 * AV server response.
 */
//--------------------------------------------------------------------------------------------------
static lwm2mcore_CoapResponse_t AVServerResponse;


//--------------------------------------------------------------------------------------------------
/**
 * Structure representing an asset value - a union of the possible types.
 */
//--------------------------------------------------------------------------------------------------
typedef union
{
    int intValue;
    double floatValue;
    bool boolValue;
    char* strValuePtr;
}
AssetValue_t;


//--------------------------------------------------------------------------------------------------
/**
 * Structure representing an asset data.
 */
//--------------------------------------------------------------------------------------------------
typedef struct
{
    le_avdata_AccessType_t serverAccess;  ///< Permitted server access to this asset data.
    le_avdata_AccessType_t clientAccess;  ///< Permitted client access to this asset data.
    le_avdata_DataType_t dataType;  ///< Data type of the Asset Value.
    AssetValue_t value;             ///< Asset Value.
    le_avdata_ResourceHandlerFunc_t handlerPtr; ///< Registered handler when asset data is accessed.
    void* contextPtr;               // Client context for the handler.
    le_dls_List_t arguments;        // Argument list for the handler.
}
AssetData_t;


//--------------------------------------------------------------------------------------------------
/**
 * Structure representing an argument in an Argument List.
 */
//--------------------------------------------------------------------------------------------------
typedef struct
{
    char* argumentName;
    le_avdata_DataType_t argValType;
    AssetValue_t argValue;
    le_dls_Link_t link;
}
Argument_t;


//--------------------------------------------------------------------------------------------------
/**
 * Data associated with an record reference. This is used for keeping track of which client
 * is using the record ref, so that everything can be cleaned up when the client dies.
 */
//--------------------------------------------------------------------------------------------------
typedef struct
{
    le_avdata_RecordRef_t recRef;               ///< Record ref
    le_msg_SessionRef_t clientSessionRef;       ///< Client using this record ref
}
RecordRefData_t;


//--------------------------------------------------------------------------------------------------
/**
 * Record reference data memory pool. Used for keeping track of the client that is using a
 * specific record ref. Initialized in avData_Init().
 */
//--------------------------------------------------------------------------------------------------
static le_mem_PoolRef_t RecordRefDataPoolRef = NULL;


//--------------------------------------------------------------------------------------------------
/**
 * Safe Reference Map for record references. Initialized in avData_Init().
 */
//--------------------------------------------------------------------------------------------------
static le_ref_MapRef_t RecordRefMap;


//--------------------------------------------------------------------------------------------------
/**
 * Safe reference map for the avms session request.
 */
//--------------------------------------------------------------------------------------------------
static le_ref_MapRef_t AvSessionRequestRefMap;


//--------------------------------------------------------------------------------------------------
/**
 * Event for sending session state to registered applications.
 */
//--------------------------------------------------------------------------------------------------
static le_event_Id_t SessionStateEvent;


//--------------------------------------------------------------------------------------------------
/**
 * Flag to check if session was opened from avc
 */
//--------------------------------------------------------------------------------------------------
static bool IsSessionStarted = false;


//--------------------------------------------------------------------------------------------------
/**
 * Count the number of requests
 */
//--------------------------------------------------------------------------------------------------
static uint32_t RequestCount = 0;

////////////////////////////////////////////////////////////////////////////////////////////////////
/* Helper functions                                                                               */
////////////////////////////////////////////////////////////////////////////////////////////////////

//--------------------------------------------------------------------------------------------------
/**
 * Handler for client session closes
 */
//--------------------------------------------------------------------------------------------------
static void ClientCloseSessionHandler
(
    le_msg_SessionRef_t sessionRef,
    void*               contextPtr
)
{
    // Search for the record references used by the closed client, and clean up any data.
    le_ref_IterRef_t iterRef = le_ref_GetIterator(RecordRefMap);
    RecordRefData_t* recRefDataPtr;

    while ( le_ref_NextNode(iterRef) == LE_OK )
    {
        recRefDataPtr = le_ref_GetValue(iterRef);

        if ( recRefDataPtr->clientSessionRef == sessionRef )
        {
            // Delete instance data, and also delete asset data, if last instance is deleted
            timeSeries_Delete(recRefDataPtr->recRef);

            // Delete safe reference and associated data
            le_mem_Release((void*)recRefDataPtr);
            le_ref_DeleteRef( RecordRefMap, (void*)le_ref_GetSafeRef(iterRef) );
        }
    }

    // Search for the session request reference(s) used by the closed client, and clean up any data.
    iterRef = le_ref_GetIterator(AvSessionRequestRefMap);

    while (le_ref_NextNode(iterRef) == LE_OK)
    {
        if (le_ref_GetValue(iterRef) == sessionRef)
        {
            le_avdata_ReleaseSession((void*)le_ref_GetSafeRef(iterRef));
        }
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Translates an asset data type to a string.
 */
//--------------------------------------------------------------------------------------------------
static const char* GetDataTypeStr
(
    le_avdata_DataType_t dataType ///< [IN] Asset data type.
)
{
    switch (dataType)
    {
        case LE_AVDATA_DATA_TYPE_NONE:
            return "none";
            break;

        case LE_AVDATA_DATA_TYPE_INT:
            return "int";
            break;

        case LE_AVDATA_DATA_TYPE_FLOAT:
            return "float";
            break;

        case LE_AVDATA_DATA_TYPE_BOOL:
            return "bool";
            break;

        case LE_AVDATA_DATA_TYPE_STRING:
            return "string";
            break;

        default:
            return "invalid";
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Converts asset data access mode to bit mask of access types for server access.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t ConvertAccessModeToServerAccess
(
    le_avdata_AccessMode_t accessMode,       ///< [IN] access mode.
    le_avdata_AccessType_t* accessBitMaskPtr ///< [OUT] bitmask of access types.
)
{
    le_avdata_AccessType_t mask = 0;

    switch (accessMode)
    {
        case LE_AVDATA_ACCESS_VARIABLE:
            mask = LE_AVDATA_ACCESS_READ;
            break;

        case LE_AVDATA_ACCESS_SETTING:
            mask = LE_AVDATA_ACCESS_READ | LE_AVDATA_ACCESS_WRITE;
            break;

        case LE_AVDATA_ACCESS_COMMAND:
            mask = LE_AVDATA_ACCESS_EXEC;
            break;

        default:
            return LE_FAULT;
    }

    *accessBitMaskPtr = mask;
    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Converts asset data access mode to bit mask of access types for client access.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t ConvertAccessModeToClientAccess
(
    le_avdata_AccessMode_t accessMode,       ///< [IN] access mode.
    le_avdata_AccessType_t* accessBitMaskPtr ///< [OUT] bitmask of access types.
)
{
    le_avdata_AccessType_t mask = 0;

    switch (accessMode)
    {
        case LE_AVDATA_ACCESS_VARIABLE:
            mask = LE_AVDATA_ACCESS_READ | LE_AVDATA_ACCESS_WRITE;
            break;

        case LE_AVDATA_ACCESS_SETTING:
            mask = LE_AVDATA_ACCESS_READ;
            break;

        case LE_AVDATA_ACCESS_COMMAND:
            mask = LE_AVDATA_ACCESS_EXEC;
            break;

        default:
            return LE_FAULT;
    }

    *accessBitMaskPtr = mask;
    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Check if the asset data path is legal.
 */
//--------------------------------------------------------------------------------------------------
static bool IsAssetDataPathValid
(
    const char* path ///< [IN] Asset data path
)
{
    // The path cannot lack a leading slash, or contain a trailing slash.
    if (('/' != path[0]) || ('/' == path[strlen(path)-1]))
    {
        return false;
    }

    // The path cannot resemble a lwm2m object.
    char* pathDup = strdup(path);
    char* firstLevelPath = strtok(pathDup, "/");

    int i;
    for (i = 0; i < NUM_ARRAY_MEMBERS(InvalidFirstLevelPathNames); i++)
    {
        if (strcmp(firstLevelPath, InvalidFirstLevelPathNames[i]) == 0)
        {
            free(pathDup);
            return false;
        }
    }

    free(pathDup);
    return true;
}


//--------------------------------------------------------------------------------------------------
/**
 * Return true if the provided path is a parent path to any of the asset data paths in the hashmap.
 */
//--------------------------------------------------------------------------------------------------
static bool IsPathParent
(
    const char* path ///< [IN] Asset data path
)
{
    le_hashmap_It_Ref_t iter = le_hashmap_GetIterator(AssetDataMap);

    while (le_hashmap_NextNode(iter) == LE_OK)
    {
        if (le_path_IsSubpath(path, le_hashmap_GetKey(iter), "/"))
        {
            return true;
        }
    }

    return false;
}


//--------------------------------------------------------------------------------------------------
/**
 * Return true if the provided path is a child path to any of the asset data paths in the hashmap.
 */
//--------------------------------------------------------------------------------------------------
static bool IsPathChild
(
    const char* path ///< [IN] Asset data path
)
{
    le_hashmap_It_Ref_t iter = le_hashmap_GetIterator(AssetDataMap);

    while (le_hashmap_NextNode(iter) == LE_OK)
    {
        if (le_path_IsSubpath(le_hashmap_GetKey(iter), path, "/"))
        {
            return true;
        }
    }

    return false;
}


//--------------------------------------------------------------------------------------------------
/**
 * Dumping an argument list for debugging purposes.
 */
//--------------------------------------------------------------------------------------------------
static void DumpArgList
(
    le_dls_List_t* argListPtr
)
{
    LE_INFO("#### DUMPING  ARUGMENT LIST ########################################################");

    Argument_t* argPtr = NULL;
    le_dls_Link_t* argLinkPtr = le_dls_Peek(argListPtr);

    while (argLinkPtr != NULL)
    {
        argPtr = CONTAINER_OF(argLinkPtr, Argument_t, link);

        LE_INFO("- arg name:       [%s]", argPtr->argumentName);

        switch (argPtr->argValType)
        {
            case LE_AVDATA_DATA_TYPE_NONE:
                LE_INFO("none");
                break;

            case LE_AVDATA_DATA_TYPE_INT:
                LE_INFO("<int> arg val:    [%d]", argPtr->argValue.intValue);
                break;

            case LE_AVDATA_DATA_TYPE_FLOAT:
                LE_INFO("<float> arg val:  [%g]", argPtr->argValue.floatValue);
                break;

            case LE_AVDATA_DATA_TYPE_BOOL:
                LE_INFO("<bool> arg val:   [%d]", argPtr->argValue.boolValue);
                break;

            case LE_AVDATA_DATA_TYPE_STRING:
                LE_INFO("<string> arg val: [%s]", argPtr->argValue.strValuePtr);
                break;

            default:
                LE_INFO("invalid");
        }

        argLinkPtr = le_dls_PeekNext(argListPtr, argLinkPtr);
    }
    LE_INFO("#### END OF DUMPING  ARUGMENT LIST #################################################");
}


//--------------------------------------------------------------------------------------------------
/**
 * Looks up an argument in the argument list with the argument name.
 *
 * @return:
 *      - argument ref if found
 *      - NULL if not found
 */
//--------------------------------------------------------------------------------------------------
static Argument_t* GetArg
(
    le_avdata_ArgumentListRef_t argumentListRef, ///< [IN] Argument list.
    const char* argName                          ///< [IN] Argument name.
)
{
    Argument_t* argPtr = NULL;
    le_dls_List_t* argListPtr = le_ref_Lookup(ArgListRefMap, argumentListRef);
    if (NULL == argListPtr)
    {
        LE_ERROR("Invalid argument list (%p) provided!", argumentListRef);
        return NULL;
    }

    le_dls_Link_t* argLinkPtr = le_dls_Peek(argListPtr);

    while (argLinkPtr != NULL)
    {
        argPtr = CONTAINER_OF(argLinkPtr, Argument_t, link);

        if (strcmp(argPtr->argumentName, argName) == 0)
        {
            return argPtr;
        }

        argLinkPtr = le_dls_PeekNext(argListPtr, argLinkPtr);
    }

    return NULL;
}


//--------------------------------------------------------------------------------------------------
/**
 * Looks up the asset data in the AssetDataMap with the provided path.
 *
 * @return:
 *      - asset data ref if found
 *      - NULL if not found
 */
//--------------------------------------------------------------------------------------------------
static AssetData_t* GetAssetData
(
    const char* path  ///< [IN] Asset data path
)
{
    AssetData_t* assetDataPtr = NULL;

    le_hashmap_It_Ref_t iter = le_hashmap_GetIterator(AssetDataMap);

    while (le_hashmap_NextNode(iter) == LE_OK)
    {
        if (strcmp(path, le_hashmap_GetKey(iter)) == 0)
        {
            assetDataPtr = le_hashmap_GetValue(iter);
            break;
        }
    }

    return assetDataPtr;
}


//--------------------------------------------------------------------------------------------------
/**
 * Gets the asset value associated with the provided asset data path.
 *
 * @return:
 *      - LE_NOT_FOUND - if the path is invalid and does not point to an asset data
 *      - LE_NOT_PERMITTED - asset data being accessed does not have the right permission
 *      - LE_OK - access successful.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t GetVal
(
    const char* path,                  ///< [IN] Asset data path
    AssetValue_t* valuePtr,            ///< [OUT] Asset value
    le_avdata_DataType_t* dataTypePtr, ///< [OUT] Asset value data type
    bool isClient                      ///< [IN] Is it client or server access
)
{
    AssetData_t* assetDataPtr = GetAssetData(path);

    if (assetDataPtr == NULL)
    {
        return LE_NOT_FOUND;
    }

    // Check access permission
    if ((!isClient && ((assetDataPtr->serverAccess & LE_AVDATA_ACCESS_READ) !=
                      LE_AVDATA_ACCESS_READ)) ||
        (isClient && ((assetDataPtr->clientAccess & LE_AVDATA_ACCESS_READ) !=
                     LE_AVDATA_ACCESS_READ)))
    {
        char* str = isClient ? "client" : "server";
        LE_ERROR("Asset (%s) does not have read permission for %s access.", path, str);
        return LE_NOT_PERMITTED;
    }

    // Call registered handler.
    if ((!isClient) && (assetDataPtr->handlerPtr != NULL))
    {
        le_avdata_ArgumentListRef_t argListRef
             = le_ref_CreateRef(ArgListRefMap, &assetDataPtr->arguments);

        assetDataPtr->handlerPtr(path, LE_AVDATA_ACCESS_READ,
                                 argListRef, assetDataPtr->contextPtr);

        le_ref_DeleteRef(ArgListRefMap, argListRef);
    }

    // Get the value.
    *valuePtr = assetDataPtr->value;
    *dataTypePtr = assetDataPtr->dataType;

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Sets the asset value associated with the provided asset data path.
 *
 * @return:
 *      - LE_NOT_FOUND - if the path is invalid and does not point to an asset data
 *      - LE_NOT_PERMITTED - asset data being accessed does not have the right permission
 *      - LE_OK - access successful.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t SetVal
(
    const char* path,              ///< [IN] Asset data path
    AssetValue_t value,            ///< [IN] Asset value
    le_avdata_DataType_t dataType, ///< [IN] Asset value data type
    bool isClient                  ///< [IN] Is it client or server access
)
{
    AssetData_t* assetDataPtr = GetAssetData(path);

    if (assetDataPtr == NULL)
    {
        return LE_NOT_FOUND;
    }

    // Check access permission
    if ((!isClient && ((assetDataPtr->serverAccess & LE_AVDATA_ACCESS_WRITE) !=
                      LE_AVDATA_ACCESS_WRITE)) ||
        (isClient && ((assetDataPtr->clientAccess & LE_AVDATA_ACCESS_WRITE) !=
                     LE_AVDATA_ACCESS_WRITE)))
    {
        char* str = isClient ? "client" : "server";
        LE_ERROR("Asset (%s) does not have write permission for %s access.", path, str);
        return LE_NOT_PERMITTED;
    }

    // If the current data type is string, we need to free the memory for the string before
    // assigning asset value to the new one.
    if (assetDataPtr->dataType == LE_AVDATA_DATA_TYPE_STRING)
    {
        le_mem_Release(assetDataPtr->value.strValuePtr);
    }

    // Set the value.
    assetDataPtr->value = value;
    assetDataPtr->dataType = dataType;

    // Call registered handler.
    if ((!isClient) && (assetDataPtr->handlerPtr != NULL))
    {
        le_avdata_ArgumentListRef_t argListRef
             = le_ref_CreateRef(ArgListRefMap, &assetDataPtr->arguments);

        assetDataPtr->handlerPtr(path, LE_AVDATA_ACCESS_WRITE,
                                 argListRef, assetDataPtr->contextPtr);

        le_ref_DeleteRef(ArgListRefMap, argListRef);
    }

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Encode the Asset Data value with the provided CBOR encoder.
 *
 * @return:
 *      - LE_FAULT on any error.
 *      - LE_OK if success.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t EncodeAssetData
(
    le_avdata_DataType_t type, ///< [IN]
    AssetValue_t assetValue,   ///< [IN]
    CborEncoder* encoder       ///< [IN]
)
{
    switch (type)
    {
        case LE_AVDATA_DATA_TYPE_NONE:
            return (CborNoError == cbor_encode_text_string(encoder, "(null)", 6)) ?
                   LE_OK : LE_FAULT;

        case LE_AVDATA_DATA_TYPE_INT:
            return (CborNoError == cbor_encode_int(encoder, assetValue.intValue)) ?
                   LE_OK : LE_FAULT;

        case LE_AVDATA_DATA_TYPE_FLOAT:
            return (CborNoError == cbor_encode_double(encoder, assetValue.floatValue)) ?
                   LE_OK : LE_FAULT;

        case LE_AVDATA_DATA_TYPE_BOOL:
            return (CborNoError == cbor_encode_boolean(encoder, assetValue.boolValue)) ?
                   LE_OK : LE_FAULT;

        case LE_AVDATA_DATA_TYPE_STRING:
        {
            size_t strValLen = strlen(assetValue.strValuePtr);

            if (strValLen > LE_AVDATA_STRING_VALUE_LEN)
            {
                LE_ERROR("String len too big (%d). %d chars expected.",
                         strValLen, LE_AVDATA_STRING_VALUE_LEN);
                return LE_FAULT;
            }

            return (CborNoError ==
                    cbor_encode_text_string(encoder, assetValue.strValuePtr, strValLen)) ?
                   LE_OK : LE_FAULT;
        }
        default:
            LE_ERROR("Unexpected data type: %d", type);
            return LE_FAULT;
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Perform cbor_value_copy_text_string only if the provided buffer is large enough.
 *
 * @return:
 *      - LE_FAULT on any error.
 *      - LE_OK if success.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t CborSafeCopyString
(
    CborValue* valuePtr,
    char* str,
    size_t* strSizePtr
)
{
    size_t incomingStrSize;

    // Could return fault if the incoming string is so big that the length can't fit in
    // incomingStrSize.
    if (CborNoError != cbor_value_calculate_string_length(valuePtr, &incomingStrSize))
    {
        return LE_FAULT;
    }

    // Need to reserve one byte for the null terminating byte.
    if (incomingStrSize > (*strSizePtr-1))
    {
        LE_ERROR("Encoded string (%d bytes) too big. Max %d bytes expected.",
                 incomingStrSize, (*strSizePtr-1));
        return LE_FAULT;
    }
    else
    {
        // We've already ensured that buffer size is sufficient, so this should not fail.
        LE_ASSERT(CborNoError == cbor_value_copy_text_string(valuePtr, str, strSizePtr, NULL));
        return LE_OK;
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Decode the CBOR value and return the value and type.
 *
 * @return:
 *      - LE_FAULT on any error.
 *      - LE_OK if success.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t DecodeAssetData
(
    le_avdata_DataType_t* typePtr, ///< [OUT]
    AssetValue_t* assetValuePtr,   ///< [OUT]
    CborValue* valuePtr            ///< [IN]
)
{
    CborType type = cbor_value_get_type(valuePtr);

    switch (type)
    {
        case CborTextStringType:
        {
            LE_DEBUG(">>>>> decoding string");
            size_t strSize = LE_AVDATA_STRING_VALUE_BYTES;
            assetValuePtr->strValuePtr = le_mem_ForceAlloc(StringPool);

            if (LE_OK != CborSafeCopyString(valuePtr, assetValuePtr->strValuePtr, &strSize))
            {
                return LE_FAULT;
            }

            *typePtr = LE_AVDATA_DATA_TYPE_STRING;
            break;
        }
        case CborIntegerType:
            LE_DEBUG(">>>>> decoding int");
            LE_ASSERT(CborNoError == cbor_value_get_int(valuePtr, &(assetValuePtr->intValue)));
            *typePtr = LE_AVDATA_DATA_TYPE_INT;
            break;

        case CborBooleanType:
            LE_DEBUG(">>>>> decoding bool");
            LE_ASSERT(CborNoError == cbor_value_get_boolean(valuePtr, &(assetValuePtr->boolValue)));
            *typePtr = LE_AVDATA_DATA_TYPE_BOOL;
            break;

        case CborDoubleType:
            LE_DEBUG(">>>>> decoding float");
            LE_ASSERT(CborNoError == cbor_value_get_double(valuePtr, &(assetValuePtr->floatValue)));
            *typePtr = LE_AVDATA_DATA_TYPE_FLOAT;
            break;

        default:
            LE_ERROR("Unexpected CBOR type: %d", type);
            return LE_FAULT;
    }

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * String compare function for qsort.
 */
//--------------------------------------------------------------------------------------------------
static int CompareStrings
(
    const void *a,
    const void *b
)
{
    return strcmp(*(char **)a, *(char **)b);
}


//--------------------------------------------------------------------------------------------------
/**
 * Given a list of asset data paths, look up the associated asset value, and encode them in CBOR
 * format with the provided CBOR encoder. In the initial call, the "level" parameter controls the
 * path depth the encoding begins at.
 *
 * In case of any error, this function returns right away and does not perform further encoding, so
 * the CborEncoder out param (and the associated buffer) would be in an unpredictable state and
 * should not be used.
 *
 * Note: The list of paths MUST be grouped at each level. They don't need to be sorted, but a sorted
 * list achieves the same goal.
 *
 * @return:
 *      - LE_FAULT on any error.
 *      - LE_OK if success.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t EncodeMultiData
(
    char* list[], ///< [IN] List of asset data paths. MUST be grouped at each level.
    CborEncoder* parentCborEncoder, ///< [OUT] Parent CBOR encoder
    int minIndex, ///< [IN] Min index of the list to start with in the current recursion
    int maxIndex, ///< [IN] Max index of the list to end with in the current recursion
    int level     ///< [IN] Path depth for the current recursion
)
{
    // Each range of paths is enclosed in a CBOR map.
    CborEncoder mapNode;
    if (CborNoError != cbor_encoder_create_map(parentCborEncoder, &mapNode, CborIndefiniteLength))
    {
        return LE_FAULT;
    }

    char* savedToken = ""; // initialized to an empty string.
    char* currToken = NULL;
    char* peekToken = NULL;

    int minCurrRange = minIndex;
    int maxCurrRange = minIndex;

    int i, j;
    for (i = minIndex; i <= maxIndex; i++)
    {
        char* currStrCopy = strdup(list[i]);

        // Getting the token of the current path level.
        currToken = strtok(currStrCopy, "/");
        for (j = 1; j < level; j++)
        {
            currToken = strtok(NULL, "/");
        }

        peekToken = strtok(NULL, "/");

        if (peekToken == NULL)
        {
            // When a leaf node is encountered, we need to make recursive calls on the previous
            // range of branch nodes.
            if (strcmp(savedToken, "") != 0)
            {
                maxCurrRange = i - 1;

                // Encoding the map name of the next recursion first.
                if ((CborNoError != cbor_encode_text_stringz(&mapNode, savedToken)) ||
                    (LE_OK != EncodeMultiData(list, &mapNode, minCurrRange, maxCurrRange, level+1)))
                {
                    return LE_FAULT;
                }
            }

            /* CBOR encoding for the leaf node itself. */
            if (NULL == currToken)
            {
                LE_ERROR("currToken is NULL");
                return LE_FAULT;
            }

            // Value name.
            if (CborNoError != cbor_encode_text_stringz(&mapNode, currToken))
            {
                return LE_FAULT;
            }

            // Use the path to look up its asset data, and do the corresponding encoding.
            AssetValue_t assetValue;
            le_avdata_DataType_t type;
            le_result_t getValresult = GetVal(list[i], &assetValue, &type, false);

            if (getValresult != LE_OK)
            {
                LE_ERROR("Fail to get asset data at [%s]. Result [%s]",
                         list[i], LE_RESULT_TXT(getValresult));

                return LE_FAULT;
            }

            if (LE_OK != EncodeAssetData(type, assetValue, &mapNode))
            {
                return LE_FAULT;
            }

            // Reset savedToken
            if (strcmp(savedToken, "") != 0)
            {
                free(savedToken);
            }
            savedToken = "";
        }
        else if (strcmp(currToken, savedToken) != 0)
        {
            // We have encountered a "new" branch node, so make recursive call on the saved range.
            if (strcmp(savedToken, "") != 0)
            {
                maxCurrRange = i - 1;

                // Encoding the map name of the next recursion first.
                if ((CborNoError != cbor_encode_text_stringz(&mapNode, savedToken)) ||
                    (LE_OK != EncodeMultiData(list, &mapNode, minCurrRange, maxCurrRange, level+1)))
                {
                    return LE_FAULT;
                }
            }

            minCurrRange = i;
            maxCurrRange = i;

            // Save the current token
            if (strcmp(savedToken, "") != 0)
            {
                free(savedToken);
            }
            savedToken = strdup(currToken);
        }
        else
        {
            // Do nothing. We've encountered the same branch node.
        }

        free(currStrCopy);
    }

    // This is to finish up the final range of branch nodes, in case the last path is not a leaf
    // node at the current level.
    if (peekToken != NULL)
    {
        maxCurrRange = i - 1;

        // Encoding the map name of the next recursion first.
        if ((CborNoError != cbor_encode_text_stringz(&mapNode, savedToken)) ||
            (LE_OK != EncodeMultiData(list, &mapNode, minCurrRange, maxCurrRange, level+1)))
        {
            return LE_FAULT;
        }
    }

    if (CborNoError != cbor_encoder_close_container(parentCborEncoder, &mapNode))
    {
        return LE_FAULT;
    }

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Decode the CBOR data and with the provided path as the base path, set the asset data values for
 * asset data paths.
 *
 * In case of any error, this function returns right away and does not perform further decoding, so
 * the CborValue out param would be in an unpredictable state and should not be used. However, any
 * values already decoded would have already been set to their corresponding asset data paths.
 *
 * @return:
 *      - LE_FAULT on any error.
 *      - LE_OK if success.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t DecodeMultiData
(
    CborValue* valuePtr, ///< [OUT] CBOR value. Expected to be a map. Iterator is advanced after the
                         ///<       function call.
    char* path           ///< [IN] base path
)
{
    // Entering a CBOR map.
    CborValue map;
    if (CborNoError != cbor_value_enter_container(valuePtr, &map))
    {
        return LE_FAULT;
    }

    size_t endingPathSegLen = 0;
    bool labelProcessed = false;

    while (!cbor_value_at_end(&map))
    {
        // The first item should be a text label.
        // If label is not yet processed, then we are expecting a text string, and that text
        // string should be a label.
        if (!labelProcessed)
        {
            if (CborTextStringType != cbor_value_get_type(&map))
            {
                return LE_FAULT;
            }

            char buf[LE_AVDATA_STRING_VALUE_BYTES] = {0};
            size_t strSize = LE_AVDATA_STRING_VALUE_BYTES;

            if (LE_OK != CborSafeCopyString(&map, buf, &strSize))
            {
                return LE_FAULT;
            }

            endingPathSegLen = strlen(buf);

            strcat(path, "/");
            strcat(path, buf);

            labelProcessed = true;
        }
        else
        {
            // The value is a map
            if (cbor_value_is_map(&map))
            {
                if (LE_OK != DecodeMultiData(&map, path))
                {
                    return LE_FAULT;
                }

                path[strlen(path) - (endingPathSegLen + 1)] = '\0';

                labelProcessed = false;

                // Skipping cbor_value_advance since cbor_value_leave_container advances the
                // iterator.
                continue;
            }

            // The value is data

            le_result_t setValresult;
            le_avdata_DataType_t type;
            AssetValue_t assetValue;

            if (LE_OK != DecodeAssetData(&type, &assetValue, &map))
            {
                return LE_FAULT;
            }

            setValresult = (type == LE_AVDATA_DATA_TYPE_NONE) ?
                           LE_UNSUPPORTED : SetVal(path, assetValue, type, false);

            if (setValresult != LE_OK)
            {
                LE_ERROR("Fail to change asset data at [%s]. Result [%s]",
                         path, LE_RESULT_TXT(setValresult));

                return LE_FAULT;
            }

            path[strlen(path) - (endingPathSegLen + 1)] = '\0';

            labelProcessed = false;
        }

        if (CborNoError != cbor_value_advance(&map))
        {
            return LE_FAULT;
        }
    }

    if (CborNoError != cbor_value_leave_container(valuePtr, &map))
    {
        return LE_FAULT;
    }

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Create an argument list from a CBOR-encoded buffer.
 *
 * @return:
 *      - LE_BAD_PARAMETER if buffer is invalid.
 *      - LE_OK if success.
 */
//--------------------------------------------------------------------------------------------------
static le_result_t CreateArgList
(
    uint8_t* payload,           ///< [IN]
    size_t payloadLen,          ///< [IN]
    le_dls_List_t* argListRef   ///< [OUT]
)
{
    CborParser parser;
    CborValue value, recursed;

    if (CborNoError != cbor_parser_init(payload, payloadLen, 0, &parser, &value))
    {
        return LE_BAD_PARAMETER;
    }

    if (!cbor_value_is_map(&value))
    {
        return LE_BAD_PARAMETER;
    }

    // Decode data in payload, and construct the argument list.
    if (CborNoError != cbor_value_enter_container(&value, &recursed))
    {
        return LE_BAD_PARAMETER;
    }

    bool labelProcessed = false;
    Argument_t* argPtr = NULL;

    while (!cbor_value_at_end(&recursed))
    {
        // The first item should be a text label (argument name).
        // If label is not yet processed, then we are expecting a text string, and that text
        // string should be a label.
        if (!labelProcessed)
        {
            if (CborTextStringType != cbor_value_get_type(&recursed))
            {
                LE_ERROR("Expect a text string for argument name, but didn't get it.");
                return LE_BAD_PARAMETER;
            }

            char buf[LE_AVDATA_STRING_VALUE_BYTES] = {0};
            size_t strSize = LE_AVDATA_STRING_VALUE_BYTES;

            if (LE_OK != CborSafeCopyString(&recursed, buf, &strSize))
            {
                LE_ERROR("Fail to decode an argument name.");
                return LE_BAD_PARAMETER;
            }

            // If the argument name doesn't exist in the list, create one.
            // Otherwise, save the node ref.
            le_dls_Link_t* argLinkPtr = le_dls_Peek(argListRef);

            while (argLinkPtr != NULL)
            {
                argPtr = CONTAINER_OF(argLinkPtr, Argument_t, link);

                if (strcmp(argPtr->argumentName, buf) == 0)
                {
                    break;
                }
                else
                {
                    argPtr = NULL;
                }

                argLinkPtr = le_dls_PeekNext(argListRef, argLinkPtr);
            }

            if (argPtr == NULL)
            {
                Argument_t* argumentPtr = le_mem_ForceAlloc(ArgumentPool);
                argumentPtr->link = LE_DLS_LINK_INIT;

                argumentPtr->argumentName = le_mem_ForceAlloc(StringPool);
                strncpy(argumentPtr->argumentName, buf, LE_AVDATA_STRING_VALUE_BYTES);

                le_dls_Queue(argListRef, &(argumentPtr->link));

                argPtr = argumentPtr;
            }

            labelProcessed = true;
        }
        else
        {
            if (LE_OK != DecodeAssetData(&(argPtr->argValType), &(argPtr->argValue), &recursed))
            {
                LE_ERROR("Fail to decode an argument value.");
                return LE_BAD_PARAMETER;
            }

            labelProcessed = false;
            argPtr = NULL;
        }

        if (CborNoError != cbor_value_advance(&recursed))
        {
            return LE_BAD_PARAMETER;
        }
    }

    if (CborNoError != cbor_value_leave_container(&value, &recursed))
    {
        return LE_BAD_PARAMETER;
    }

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Responding to AV server after an asset data request has been handled.
 * Note that the AVServerResponse is expected to be partially filled with token, tokenLength, and
 * contentType.
 */
//--------------------------------------------------------------------------------------------------
static void RespondToAvServer
(
    lwm2mcore_CoapResponseCode_t code,
    uint8_t* payload,
    size_t payloadLength
)
{
    AVServerResponse.code = code;
    AVServerResponse.payload = payload;
    AVServerResponse.payloadLength = payloadLength;

    lwm2mcore_SendAsyncResponse(AVCClientSessionInstanceRef, AVServerReqRef, &AVServerResponse);
}


//--------------------------------------------------------------------------------------------------
/**
 * Processes read request from AV server.
 */
//--------------------------------------------------------------------------------------------------
static void ProcessAvServerReadRequest
(
    const char* path
)
{
    LE_DEBUG(">>>>> COAP_GET - Server reads from device");

    AssetValue_t assetValue;
    le_avdata_DataType_t type;

    le_result_t getValResult = GetVal(path, &assetValue, &type, false);

    if (getValResult == LE_OK)
    {
        LE_DEBUG(">>>>> Reading single data point.");

        // Encode the asset data value.
        uint8_t buf[CBOR_DECODER_BUFFER_BYTES] = {0};
        CborEncoder encoder;
        cbor_encoder_init(&encoder, (uint8_t*)&buf, sizeof(buf), 0); // no error check needed.

        if (LE_OK == EncodeAssetData(type, assetValue, &encoder))
        {
            RespondToAvServer(COAP_CONTENT_AVAILABLE, buf,
                              cbor_encoder_get_buffer_size(&encoder, buf));
        }
        else
        {
            LE_DEBUG(">>>>> Fail to encode single data point.");
            RespondToAvServer(COAP_INTERNAL_ERROR, NULL, 0);
        }
    }
    else if (getValResult == LE_NOT_PERMITTED)
    {
        LE_DEBUG(">>>>> no permission.");

        RespondToAvServer(COAP_METHOD_UNAUTHORIZED, NULL, 0);
    }
    else if (getValResult == LE_NOT_FOUND)
    {
        // The path contain children nodes, so there might be multiple asset data under it.
        if (IsPathParent(path))
        {
            LE_DEBUG(">>>>> path not found, but is parent path. Encoding all children nodes.");

            // Gather all eligible paths in a path array.
            AssetData_t* assetDataPtr;
            char* pathArray[le_hashmap_Size(AssetDataMap)];
            memset(pathArray, 0, sizeof(pathArray));
            int pathArrayIdx = 0;

            le_hashmap_It_Ref_t iter = le_hashmap_GetIterator(AssetDataMap);
            char* currentPath;

            while (le_hashmap_NextNode(iter) == LE_OK)
            {
                currentPath = le_hashmap_GetKey(iter);
                assetDataPtr = le_hashmap_GetValue(iter);

                if ((le_path_IsSubpath(path, currentPath, "/")) &&
                    ((assetDataPtr->serverAccess & LE_AVDATA_ACCESS_READ) == LE_AVDATA_ACCESS_READ))
                {
                    // Put the currentPath in the path array.
                    pathArray[pathArrayIdx] = currentPath;
                    pathArrayIdx++;
                }
            }

            // Sort the path array. Note that the paths just need to be grouped at each level.
            qsort(pathArray, pathArrayIdx, sizeof(*pathArray), CompareStrings);

            // Determine the path depth the encoding should start at.
            int levelCount = 0;
            int i;
            for (i = 0; path[i] != '\0'; i++)
            {
                if ('/' == path[i])
                {
                    levelCount++;
                }
            }

            // compose the CBOR buffer
            uint8_t buf[CBOR_DECODER_BUFFER_BYTES] = {0};
            CborEncoder rootNode;

            cbor_encoder_init(&rootNode, (uint8_t*)&buf, sizeof(buf), 0); // no error check needed.

            if (LE_OK ==
                EncodeMultiData(pathArray, &rootNode, 0, (pathArrayIdx - 1), (levelCount + 1)))
            {
                RespondToAvServer(COAP_CONTENT_AVAILABLE,
                                  buf, cbor_encoder_get_buffer_size(&rootNode, buf));
            }
            else
            {
                LE_DEBUG(">>>>> Fail to encode multiple data points.");
                RespondToAvServer(COAP_INTERNAL_ERROR, NULL, 0);
            }
        }
        // The path contains no children nodes.
        else
        {
            LE_DEBUG(">>>>> path not found and isn't parent path. Replying 'not found'");
            RespondToAvServer(COAP_NOT_FOUND, NULL, 0);
        }
    }
    else
    {
        LE_FATAL("Unexpected GetVal result: %s", LE_RESULT_TXT(getValResult));
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Processes write request from AV server.
 */
//--------------------------------------------------------------------------------------------------
static void ProcessAvServerWriteRequest
(
    const char* path,
    uint8_t* payload,
    size_t payloadLen
)
{
    LE_DEBUG(">>>>> COAP_PUT - Server writes to device");

    CborParser parser;
    CborValue value;

    if (CborNoError != cbor_parser_init(payload, payloadLen, 0, &parser, &value))
    {
        RespondToAvServer(COAP_BAD_REQUEST, NULL, 0);
        return;
    }

    // The payload would either contain a value for a single data point, or a map.
    if (cbor_value_is_map(&value))
    {
        LE_DEBUG(">>>>> AV server sent a map.");

        AssetData_t* assetDataPtr = GetAssetData(path);

        // Check if path exists. If it does, then it's impossible to have children nodes.
        // Therefore return error.
        if (assetDataPtr != NULL)
        {
            LE_DEBUG(">>>>> Server writes to an existing path. Replying COAP_BAD_REQUEST.");

            RespondToAvServer(COAP_BAD_REQUEST, NULL, 0);
        }
        else
        {
            LE_DEBUG(">>>>> Server writes to a non-existing path.");

            if (IsPathParent(path))
            {
                LE_DEBUG(">>>>> path is parent. Attempting to write the multi-value.");

                // If the path is a parent path, decode everything under that path.
                le_result_t result = DecodeMultiData(&value, path);

                RespondToAvServer(
                    (result == LE_OK) ? COAP_RESOURCE_CHANGED : COAP_BAD_REQUEST, NULL, 0);
            }
            else
            {
                LE_DEBUG(">>>>> path is not parent. Replying COAP_BAD_REQUEST.");

                // If the path doesn't exist, check if it's a parent path. If it isn't,
                // then return error. (note that resource creation from server isn't
                // supported)

                RespondToAvServer(COAP_BAD_REQUEST, NULL, 0);
            }
        }
    }
    // Assume this is the case with a value for a single data point.
    else
    {
        LE_DEBUG(">>>>> AV server sent a single value.");

        // Decode the value and call SetVal. Reply to AV Server according to the result.
        le_result_t result;
        le_avdata_DataType_t type;
        AssetValue_t assetValue;
        lwm2mcore_CoapResponseCode_t code;

        if (LE_OK != DecodeAssetData(&type, &assetValue, &value))
        {
            LE_DEBUG(">>>>> Fail to decode single data point.");
            code = COAP_INTERNAL_ERROR;
        }
        else
        {
            result = (type == LE_AVDATA_DATA_TYPE_NONE) ?
                     LE_UNSUPPORTED : SetVal(path, assetValue, type, false);

            switch (result)
            {
                case LE_OK:
                    code = COAP_RESOURCE_CHANGED;
                    break;
                case LE_NOT_PERMITTED:
                    code = COAP_METHOD_UNAUTHORIZED;
                    break;
                case LE_NOT_FOUND:
                    code = COAP_NOT_FOUND;
                    break;
                case LE_UNSUPPORTED:
                    code = COAP_BAD_REQUEST;
                    break;
                default:
                    LE_ERROR("Unexpected result.");
                    code = COAP_INTERNAL_ERROR;
            }
        }

        RespondToAvServer(code, NULL, 0);
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Processes exec request from AV server.
 */
//--------------------------------------------------------------------------------------------------
static void ProcessAvServerExecRequest
(
    const char* path,
    uint8_t* payload,
    size_t payloadLen
)
{
    LE_DEBUG(">>>>> COAP_POST - Server executes a command on device");

    AssetData_t* assetDataPtr = GetAssetData(path);

    if (assetDataPtr != NULL)
    {
        // Server attempts to execute a path that's not executable.
        if ((assetDataPtr->serverAccess & LE_AVDATA_ACCESS_EXEC) != LE_AVDATA_ACCESS_EXEC)
        {
            LE_ERROR("Server attempts to execute on an asset data without execute permission.");
            RespondToAvServer(COAP_METHOD_UNAUTHORIZED, NULL, 0);
        }
        else
        {
            if (assetDataPtr->handlerPtr == NULL)
            {
                LE_ERROR("Server attempts to execute a command, but no command defined.");
                RespondToAvServer(COAP_NOT_FOUND, NULL, 0);
            }
            else
            {
                le_result_t result = CreateArgList(payload, payloadLen, &assetDataPtr->arguments);

                if (result == LE_OK)
                {
                    // Dump argument list (for debug)
                    #if 0
                    DumpArgList(&assetDataPtr->arguments);
                    #endif

                    // Create a safe ref with the argument list, and pass that to the handler.
                    le_avdata_ArgumentListRef_t argListRef =
                                         le_ref_CreateRef(ArgListRefMap, &assetDataPtr->arguments);

                    // Execute the command with the argument list collected earlier.
                    assetDataPtr->handlerPtr(path, LE_AVDATA_ACCESS_EXEC, argListRef,
                                             assetDataPtr->contextPtr);

                    // Note that we are not repsonding to AV server yet. The response happens when
                    // the client app finishes command execution and calls
                    // le_avdata_ReplyExecResult.
                }
                else
                {
                    LE_ERROR("Server attempts to execute a command but argument list is invalid");
                    RespondToAvServer(COAP_BAD_REQUEST, NULL, 0);
                }
            }
        }
    }
    else
    {
        LE_ERROR("Server attempts to execute a command but the asset data doesn't exist");
        RespondToAvServer(COAP_NOT_FOUND, NULL, 0);
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Handles requests from an AV server to read, write, or execute on an asset data.
 */
//--------------------------------------------------------------------------------------------------
static void AvServerRequestHandler
(
    lwm2mcore_CoapRequestRef_t serverReqRef
)
{
    // Save the session context and server request ref, so when reply function such as
    // le_avdata_ReplyExecResult is called at the end of the command execution,
    // it can async reply AV server with them.
    AVCClientSessionInstanceRef = avcClient_GetInstance();
    if (NULL == AVCClientSessionInstanceRef)
    {
        LE_ERROR("Cannot get AVC client session context. Stop processing AV server request.");
        return;
    }

    AVServerReqRef = serverReqRef;

    // Extract info from the server request.
    const char* path = lwm2mcore_GetRequestUri(AVServerReqRef); // cannot have trailing slash.
    coap_method_t method = lwm2mcore_GetRequestMethod(AVServerReqRef);
    uint8_t* payload = lwm2mcore_GetRequestPayload(AVServerReqRef);
    size_t payloadLen = lwm2mcore_GetRequestPayloadLength(AVServerReqRef);
    uint8_t* token = lwm2mcore_GetToken(AVServerReqRef);
    size_t tokenLength = lwm2mcore_GetTokenLength(AVServerReqRef);
    unsigned int contentType = lwm2mcore_GetContentType(AVServerReqRef);

    // Partially fill in the response.
    memcpy(AVServerResponse.token, token, tokenLength);
    AVServerResponse.tokenLength = tokenLength;
    AVServerResponse.contentType = LWM2MCORE_PUSH_CONTENT_CBOR;

    LE_DEBUG(">>>>> Request Uri is: [%s]", path);

    switch (method)
    {
        case COAP_GET: // server reads from device
            ProcessAvServerReadRequest(path);
            break;

        case COAP_PUT: // server writes to device
            ProcessAvServerWriteRequest(path, payload, payloadLen);
            break;

        case COAP_POST: // server executes a command on device
            ProcessAvServerExecRequest(path, payload, payloadLen);
            break;

        default:
            LE_ERROR("unsupported coap method from an AirVantage server: %d", method);

            RespondToAvServer(COAP_BAD_REQUEST, NULL, 0);
    }
}


////////////////////////////////////////////////////////////////////////////////////////////////////
/* Public functions                                                                               */
////////////////////////////////////////////////////////////////////////////////////////////////////

//--------------------------------------------------------------------------------------------------
/**
 * Registeres a handler function to a asset data path when a resource event (read/write/execute)
 * occurs.
 *
 * @return:
 *      - resource event handler ref - needed to remove the handler
 *      - NULL - invalid asset data path is provided
 */
//--------------------------------------------------------------------------------------------------
le_avdata_ResourceEventHandlerRef_t le_avdata_AddResourceEventHandler
(
    const char* path,                           ///< [IN] Asset data path
    le_avdata_ResourceHandlerFunc_t handlerPtr, ///< [IN] Handler function for resource event
    void* contextPtr                            ///< [IN] context pointer
)
{
    AssetData_t* assetDataPtr = NULL;

    le_hashmap_It_Ref_t iter = le_hashmap_GetIterator(AssetDataMap);

    while (le_hashmap_NextNode(iter) == LE_OK)
    {
        if (strcmp(path, le_hashmap_GetKey(iter)) == 0)
        {
            assetDataPtr = le_hashmap_GetValue(iter);
            assetDataPtr->handlerPtr = handlerPtr;
            assetDataPtr->contextPtr = contextPtr;

            return le_ref_CreateRef(ResourceEventHandlerMap, assetDataPtr);
        }
    }

    LE_WARN("Non-existing asset data path %s", path);
    return NULL;
}


//--------------------------------------------------------------------------------------------------
/**
 * Removes a resource event handler function to a asset data path.
 */
//--------------------------------------------------------------------------------------------------
void le_avdata_RemoveResourceEventHandler
(
    le_avdata_ResourceEventHandlerRef_t addHandlerRef ///< [IN] resource event handler ref
)
{
    AssetData_t* assetDataPtr = le_ref_Lookup(ResourceEventHandlerMap, addHandlerRef);

    if (assetDataPtr != NULL)
    {
        le_ref_DeleteRef(ResourceEventHandlerMap, addHandlerRef);
        assetDataPtr->handlerPtr = NULL;
        assetDataPtr->contextPtr = NULL;
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Create an asset data with the provided path. Note that asset data type and value are determined
 * upton the first call to a Set function. When an asset data is created, it contains a null value,
 * represented by the data type of none.
 *
 * @return:
 *      - LE_OK on success
 *      - LE_DUPLICATE if path has already been called by CreateResource before, or path is parent
 *        or child to an existing Asset Data path.
 *      - LE_FAULT on any other error.
 */
//--------------------------------------------------------------------------------------------------
le_result_t le_avdata_CreateResource
(
    const char* path,                 ///< [IN] Asset data path
    le_avdata_AccessMode_t accessMode ///< [IN] Asset data access mode
)
{
    // The path cannot already exists, and cannot be a parent or child path to any of the existing
    // path.
    if ((GetAssetData(path) != NULL) || IsPathParent(path) || IsPathChild(path))
    {
        return LE_DUPLICATE;
    }

    // Check if the asset data path is legal.
    if (IsAssetDataPathValid(path) != true)
    {
        LE_ERROR("Invalid asset data path [%s].", path);
        return LE_FAULT;
    }

    // Convert access mode to access bitmasks.
    le_avdata_AccessType_t serverAccess, clientAccess;
    if ((ConvertAccessModeToServerAccess(accessMode, &serverAccess) != LE_OK) ||
        (ConvertAccessModeToClientAccess(accessMode, &clientAccess) != LE_OK))
    {
        LE_KILL_CLIENT("Invalid access mode [%d].", accessMode);
    }

    char* assetPathPtr = le_mem_ForceAlloc(AssetPathPool);
    AssetData_t* assetDataPtr = le_mem_ForceAlloc(AssetDataPool);

    // Copy path to our internal record. Overflow should never occur.
    LE_ASSERT(le_utf8_Copy(assetPathPtr, path, LE_AVDATA_PATH_NAME_BYTES, NULL) == LE_OK);

    // Initialize the asset data.
    // Note that the union field is zeroed out by the memset.
    memset(assetDataPtr, 0, sizeof(AssetData_t));
    assetDataPtr->serverAccess = serverAccess;
    assetDataPtr->clientAccess = clientAccess;
    assetDataPtr->dataType = LE_AVDATA_DATA_TYPE_NONE;
    assetDataPtr->handlerPtr = NULL;
    assetDataPtr->contextPtr = NULL;
    assetDataPtr->arguments = LE_DLS_LIST_INIT;

    le_hashmap_Put(AssetDataMap, assetPathPtr, assetDataPtr);

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Sets an asset data to contain a null value, represented by the data type of none.
 *
 * @return:
 *      - per SetVal
 */
//--------------------------------------------------------------------------------------------------
le_result_t le_avdata_SetNull
(
    const char* path ///< [IN] Asset data path
)
{
    AssetValue_t assetValue;
    memset(&assetValue, 0, sizeof(AssetValue_t));

    return SetVal(path, assetValue, LE_AVDATA_DATA_TYPE_NONE, true);
}


//--------------------------------------------------------------------------------------------------
/**
 * Gets the integer value of an asset data.
 *
 * @return:
 *      - LE_BAD_PARAMETER - asset data being accessed is of the wrong data type
 *      - LE_UNAVAILABLE - asset data contains null value
 *      - others per GetVal
 */
//--------------------------------------------------------------------------------------------------
le_result_t le_avdata_GetInt
(
    const char* path, ///< [IN] Asset data path
    int32_t* valuePtr ///< [OUT] Retrieved integer
)
{
    AssetValue_t assetValue;
    le_avdata_DataType_t type;

    le_result_t result = GetVal(path, &assetValue, &type, true);

    if (type == LE_AVDATA_DATA_TYPE_NONE)
    {
        return LE_UNAVAILABLE;
    }

    if (type != LE_AVDATA_DATA_TYPE_INT)
    {
        LE_ERROR("Accessing asset (%s) of type %s as int.", path, GetDataTypeStr(type));
        return LE_BAD_PARAMETER;
    }

    if (result != LE_OK)
    {
        return result;
    }
    else
    {
        *valuePtr = assetValue.intValue;
        return LE_OK;
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Sets an asset data to an integer value.
 *
 * @return:
 *      - per SetVal
 */
//--------------------------------------------------------------------------------------------------
le_result_t le_avdata_SetInt
(
    const char* path, ///< [IN] Asset data path
    int32_t value     ///< [IN] integer to be set
)
{
    AssetValue_t assetValue;
    assetValue.intValue = value;

    return SetVal(path, assetValue, LE_AVDATA_DATA_TYPE_INT, true);
}


//--------------------------------------------------------------------------------------------------
/**
 * Gets the float value of an asset data.
 *
 * @return:
 *      - LE_BAD_PARAMETER - asset data being accessed is of the wrong data type
 *      - LE_UNAVAILABLE - asset data contains null value
 *      - others per GetVal
 */
//--------------------------------------------------------------------------------------------------
le_result_t le_avdata_GetFloat
(
    const char* path, ///< [IN] Asset data path
    double* valuePtr  ///< [OUT] Retrieved float
)
{
    AssetValue_t assetValue;
    le_avdata_DataType_t type;

    le_result_t result = GetVal(path, &assetValue, &type, true);

    if (type == LE_AVDATA_DATA_TYPE_NONE)
    {
        return LE_UNAVAILABLE;
    }

    if (type != LE_AVDATA_DATA_TYPE_FLOAT)
    {
        LE_ERROR("Accessing asset (%s) of type %s as float.", path, GetDataTypeStr(type));
        return LE_BAD_PARAMETER;
    }

    if (result != LE_OK)
    {
        return result;
    }
    else
    {
        *valuePtr = assetValue.floatValue;
        return LE_OK;
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Sets an asset data to a float value.
 *
 * @return:
 *      - per SetVal
 */
//--------------------------------------------------------------------------------------------------
le_result_t le_avdata_SetFloat
(
    const char* path,  ///< [IN] Asset data path
    double value       ///< [IN] float to be set
)
{
    AssetValue_t assetValue;
    assetValue.floatValue = value;

    return SetVal(path, assetValue, LE_AVDATA_DATA_TYPE_FLOAT, true);
}


//--------------------------------------------------------------------------------------------------
/**
 * Gets the bool value of an asset data.
 *
 * @return:
 *      - LE_BAD_PARAMETER - asset data being accessed is of the wrong data type
 *      - LE_UNAVAILABLE - asset data contains null value
 *      - others per GetVal
 */
//--------------------------------------------------------------------------------------------------
le_result_t le_avdata_GetBool
(
    const char* path, ///< [IN] Asset data path
    bool* valuePtr    ///< [OUT] Retrieved bool
)
{
    AssetValue_t assetValue;
    le_avdata_DataType_t type;

    le_result_t result = GetVal(path, &assetValue, &type, true);

    if (type == LE_AVDATA_DATA_TYPE_NONE)
    {
        return LE_UNAVAILABLE;
    }

    if (type != LE_AVDATA_DATA_TYPE_BOOL)
    {
        LE_ERROR("Accessing asset (%s) of type %s as bool.", path, GetDataTypeStr(type));
        return LE_BAD_PARAMETER;
    }

    if (result != LE_OK)
    {
        return result;
    }
    else
    {
        *valuePtr = assetValue.boolValue;
        return LE_OK;
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Sets an asset data to a bool value.
 *
 * @return:
 *      - per SetVal
 */
//--------------------------------------------------------------------------------------------------
le_result_t le_avdata_SetBool
(
    const char* path, ///< [IN] Asset data path
    bool value        ///< [IN] bool to be set
)
{
    AssetValue_t assetValue;
    assetValue.boolValue = value;

    return SetVal(path, assetValue, LE_AVDATA_DATA_TYPE_BOOL, true);
}


//--------------------------------------------------------------------------------------------------
/**
 * Gets the string value of an asset data.
 *
 * @return:
 *      - LE_BAD_PARAMETER - asset data being accessed is of the wrong data type
 *      - LE_UNAVAILABLE - asset data contains null value
 *      - others per GetVal
 */
//--------------------------------------------------------------------------------------------------
le_result_t le_avdata_GetString
(
    const char* path,       ///< [IN] Asset data path
    char* value,            ///< [OUT] Retrieved string
    size_t valueNumElements ///< [IN] String buffer size in bytes
)
{
    AssetValue_t assetValue;
    le_avdata_DataType_t type;

    le_result_t result = GetVal(path, &assetValue, &type, true);

    if (type == LE_AVDATA_DATA_TYPE_NONE)
    {
        return LE_UNAVAILABLE;
    }

    if (type != LE_AVDATA_DATA_TYPE_STRING)
    {
        LE_ERROR("Accessing asset (%s) of type %s as string.", path, GetDataTypeStr(type));
        return LE_BAD_PARAMETER;
    }

    if (result != LE_OK)
    {
        return result;
    }
    else
    {
        strncpy(value, assetValue.strValuePtr, valueNumElements);
        return LE_OK;
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Sets an asset data to a string value.
 *
 * @return:
 *      - per SetVal
 */
//--------------------------------------------------------------------------------------------------
le_result_t le_avdata_SetString
(
    const char* path, ///< [IN] Asset data path
    const char* value ///< [IN] string to be set
)
{
    AssetValue_t assetValue;
    assetValue.strValuePtr = le_mem_ForceAlloc(StringPool);
    strncpy(assetValue.strValuePtr, value, LE_AVDATA_STRING_VALUE_BYTES);

    return SetVal(path, assetValue, LE_AVDATA_DATA_TYPE_STRING, true);
}


//--------------------------------------------------------------------------------------------------
/**
 * Get the bool argument with the specified name.
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if argument doesn't exist, or its data type doesn't match the API.
 */
//--------------------------------------------------------------------------------------------------
le_result_t le_avdata_GetBoolArg
(
    le_avdata_ArgumentListRef_t argumentListRef, ///< [IN] Argument list
    const char* argName,                         ///< [IN] Argument name as key
    bool* boolArgPtr                             ///< [OUT] Retrieved bool arg
)
{
    Argument_t* argPtr = GetArg(argumentListRef, argName);

    if (argPtr != NULL)
    {
        if (argPtr->argValType == LE_AVDATA_DATA_TYPE_BOOL)
        {
            *boolArgPtr = argPtr->argValue.boolValue;
            return LE_OK;
        }
        else
        {
            LE_ERROR("Found argument named %s, but type is %s instead of %s", argName,
                     GetDataTypeStr(argPtr->argValType), GetDataTypeStr(LE_AVDATA_DATA_TYPE_BOOL));
            return LE_NOT_FOUND;
        }
    }
    else
    {
        LE_ERROR("Cannot find argument named %s", argName);
        return LE_NOT_FOUND;
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Get the float argument with the specified name.
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if argument doesn't exist, or its data type doesn't match the API.
 */
//--------------------------------------------------------------------------------------------------
le_result_t le_avdata_GetFloatArg
(
    le_avdata_ArgumentListRef_t argumentListRef, ///< [IN] Argument list
    const char* argName,                         ///< [IN] Argument name as key
    double* floatArgPtr                          ///< [OUT] Retrieved float arg
)
{
    Argument_t* argPtr = GetArg(argumentListRef, argName);

    if (argPtr != NULL)
    {
        if (argPtr->argValType == LE_AVDATA_DATA_TYPE_FLOAT)
        {
            *floatArgPtr = argPtr->argValue.floatValue;
            return LE_OK;
        }
        else
        {
            LE_ERROR("Found argument named %s, but type is %s instead of %s", argName,
                     GetDataTypeStr(argPtr->argValType), GetDataTypeStr(LE_AVDATA_DATA_TYPE_FLOAT));
            return LE_NOT_FOUND;
        }
    }
    else

    {
        LE_ERROR("Cannot find argument named %s", argName);
        return LE_NOT_FOUND;
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Get the int argument with the specified name.
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if argument doesn't exist, or its data type doesn't match the API.
 */
//--------------------------------------------------------------------------------------------------
le_result_t le_avdata_GetIntArg
(
    le_avdata_ArgumentListRef_t argumentListRef, ///< [IN] Argument list
    const char* argName,                         ///< [IN] Argument name as key
    int32_t* intArgPtr                           ///< [OUT] Retrieved int arg
)
{
    Argument_t* argPtr = GetArg(argumentListRef, argName);

    if (argPtr != NULL)
    {
        if (argPtr->argValType == LE_AVDATA_DATA_TYPE_INT)
        {
            *intArgPtr = argPtr->argValue.intValue;
            return LE_OK;
        }
        else
        {
            LE_ERROR("Found argument named %s, but type is %s instead of %s", argName,
                     GetDataTypeStr(argPtr->argValType), GetDataTypeStr(LE_AVDATA_DATA_TYPE_INT));
            return LE_NOT_FOUND;
        }
    }
    else
    {
        LE_ERROR("Cannot find argument named %s", argName);
        return LE_NOT_FOUND;
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Get the string argument with the specified name.
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if argument doesn't exist, or its data type doesn't match the API.
 */
//--------------------------------------------------------------------------------------------------
le_result_t le_avdata_GetStringArg
(
    le_avdata_ArgumentListRef_t argumentListRef, ///< [IN] Argument list
    const char* argName,                         ///< [IN] Argument name as key
    char* strArg,                                ///< [OUT] Retrieved string arg
    size_t argNumElements                        ///< [IN] string arg buffer length
)
{
    Argument_t* argPtr = GetArg(argumentListRef, argName);

    if (argPtr != NULL)
    {
        if (argPtr->argValType == LE_AVDATA_DATA_TYPE_STRING)
        {
            strncpy(strArg, argPtr->argValue.strValuePtr, argNumElements);
            return LE_OK;
        }
        else
        {
            LE_ERROR("Found argument named %s, but type is %s instead of %s", argName,
                     GetDataTypeStr(argPtr->argValType),
                     GetDataTypeStr(LE_AVDATA_DATA_TYPE_STRING));
            return LE_NOT_FOUND;
        }
    }
    else
    {
        LE_ERROR("Cannot find argument named %s", argName);
        return LE_NOT_FOUND;
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Get the length (excluding terminating null byte) of the string argument of the specified name.
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NOT_FOUND if argument doesn't exist, or its data type doesn't match the API.
 */
//--------------------------------------------------------------------------------------------------
le_result_t le_avdata_GetStringArgLength
(
    le_avdata_ArgumentListRef_t argumentListRef, ///< [IN] Argument list ref
    const char* argName,                         ///< [IN] Argument name to get the length for
    int32_t* strArgLenPtr ///< [OUT] Argument string length excluding terminating null byte
)
{
    Argument_t* argPtr = GetArg(argumentListRef, argName);

    if (argPtr != NULL)
    {
        if (argPtr->argValType == LE_AVDATA_DATA_TYPE_STRING)
        {
            *strArgLenPtr = strlen(argPtr->argValue.strValuePtr);
            return LE_OK;
        }
        else
        {
            LE_ERROR("Found argument named %s, but type is %s instead of %s", argName,
                     GetDataTypeStr(argPtr->argValType),
                     GetDataTypeStr(LE_AVDATA_DATA_TYPE_STRING));
            return LE_NOT_FOUND;
        }
    }
    else
    {
        LE_ERROR("Cannot find argument named %s", argName);
        return LE_NOT_FOUND;
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Reply command execution result to AVC Daemon, which can then respond to AV server. This function
 * MUST be called at the end of a command execution, in order for AV server to be notified about the
 * execution status.
 */
//--------------------------------------------------------------------------------------------------
void le_avdata_ReplyExecResult
(
    le_avdata_ArgumentListRef_t argListRef,  ///< [IN] Argument list ref.
    le_result_t result                       ///< [IN] Command execution status.
)
{
    // Clean up the argument list and safe ref.
    Argument_t* argPtr = NULL;
    le_dls_List_t* argListPtr = le_ref_Lookup(ArgListRefMap, argListRef);
    if (NULL == argListPtr)
    {
        LE_KILL_CLIENT("Invalid argument list (%p) provided!", argListRef);
        return;
    }

    le_dls_Link_t* argLinkPtr = le_dls_Pop(argListPtr);

    while (argLinkPtr != NULL)
    {
        argPtr = CONTAINER_OF(argLinkPtr, Argument_t, link);
        le_mem_Release(argPtr);
        argLinkPtr = le_dls_Pop(argListPtr);
    }

    le_ref_DeleteRef(ArgListRefMap, argListRef);

    // Respond to AV server with the command execution result.
    RespondToAvServer((result == LE_OK) ? COAP_RESOURCE_CHANGED : COAP_INTERNAL_ERROR, NULL, 0);
}


//--------------------------------------------------------------------------------------------------
/**
 * Push asset data to the server
 *
* @return:
 *      - LE_OK on success.
 *      - LE_NOT_FOUND if path doesn't exist.
 *      - LE_BUSY if push is queued and will pushed later
 *      - LE_NOT_POSSIBLE if push queue is full, try again later
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t le_avdata_Push
(
    const char* path,                          ///< [IN] Asset data path
    le_avdata_CallbackResultFunc_t handlerPtr, ///< [IN] Push result callback
    void* contextPtr                           ///< [IN] Context pointer
)
{
    AssetValue_t assetValue;
    le_avdata_DataType_t type;

    if (!IsAssetDataPathValid(path))
    {
        return LE_FAULT;
    }

    le_result_t result = GetVal(path, &assetValue, &type, false);

    char* pathArray[le_hashmap_Size(AssetDataMap)];
    memset(pathArray, 0, sizeof(pathArray));
    int pathArrayIdx = 0;

    if (result == LE_OK)
    {
        pathArray[0] = path;
        pathArrayIdx = 1;
    }
    else if (result == LE_NOT_FOUND)
    {
        // The path contain children nodes, so there might be multiple asset data under it.
        if (IsPathParent(path))
        {
            LE_DEBUG(">>>>> path not found, but is parent path. Encoding all children nodes.");

            // Gather all eligible paths in a path array.
            AssetData_t* assetDataPtr;

            le_hashmap_It_Ref_t iter = le_hashmap_GetIterator(AssetDataMap);
            char* currentPath;

            while (le_hashmap_NextNode(iter) == LE_OK)
            {
                currentPath = le_hashmap_GetKey(iter);
                assetDataPtr = le_hashmap_GetValue(iter);

                if ((le_path_IsSubpath(path, currentPath, "/")) &&
                    ((assetDataPtr->serverAccess & LE_AVDATA_ACCESS_READ) == LE_AVDATA_ACCESS_READ))
                {
                    // Put the currentPath in the path array.
                    pathArray[pathArrayIdx] = currentPath;
                    pathArrayIdx++;
                }
            }

            // Sort the path array. Note that the paths just need to be grouped at each level.
            qsort(pathArray, pathArrayIdx, sizeof(*pathArray), CompareStrings);
        }
        else
        {
            // Path does not exists
            return LE_NOT_FOUND;
        }
    }
    else
    {
        return LE_FAULT;
    }

    // compose the CBOR buffer
    uint8_t buf[CBOR_DECODER_BUFFER_BYTES] = {0};
    CborEncoder rootNode;
    cbor_encoder_init(&rootNode, (uint8_t*)&buf, sizeof(buf), 0); // no error check needed.

    result = EncodeMultiData(pathArray, &rootNode, 0, (pathArrayIdx - 1), 1);

    if (result == LE_OK)
    {
        LE_DUMP(buf, cbor_encoder_get_buffer_size(&rootNode, buf));
        result = PushBuffer(buf,
                            cbor_encoder_get_buffer_size(&rootNode, buf),
                            LWM2MCORE_PUSH_CONTENT_CBOR,
                            handlerPtr,
                            contextPtr);
    }
    else
    {
        LE_DEBUG(">>>>> Fail to encode multiple data points.");
        result = LE_FAULT;
    }

    return result;
}


//--------------------------------------------------------------------------------------------------
/**
 * Push data dump to a specified path on the server.
 *
 * @return:
 *      - LE_OK on success.
 *      - LE_BUSY if the service is busy pushing other data, try again later
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t le_avdata_PushStream
(
    const char* path,                          ///< [IN] Asset data path
    int fd,                                    ///< [IN] File descriptor of data dump.
    le_avdata_CallbackResultFunc_t handlerPtr, ///< [IN] Push result callback
    void* contextPtr                           ///< [IN] Context pointer
)
{
    // Service is busy, notify user to try another time
    if (IsPushBusy())
    {
        return LE_NOT_POSSIBLE;
    }

    if (fd < 0)
    {
        LE_ERROR("Invalid file descriptor");
        return LE_FAULT;
    }

    int result;
    int bytesRead = 0;
    uint8_t buffer[MAX_PUSH_BUFFER_BYTES+1];
    memset(buffer, 0, sizeof(buffer));

    do
    {
        result = (read(fd, buffer + bytesRead, (MAX_PUSH_BUFFER_BYTES - bytesRead) + 1));
        if (result > 0)
        {
            bytesRead += result;
            if (bytesRead > MAX_PUSH_BUFFER_BYTES)
            {
                LE_ERROR("Data dump exceeds maximum buffer size.");
                return LE_FAULT;
            }
        }

        if ((result < 0) && (errno != EINTR))
        {
            LE_ERROR("Error reading.");
            return LE_FAULT;
        }
    }
    while ((bytesRead < MAX_PUSH_BUFFER_BYTES) && (result != 0));

    // Encode data. Encoded buffer must be large to store path + data + cbor mapping (5)
    uint8_t encodedBuf[bytesRead + strlen(path) + 5];
    CborEncoder encoder, mapEncoder;
    CborError err;
    cbor_encoder_init(&encoder, &encodedBuf, sizeof(encodedBuf), 0);
    err = cbor_encoder_create_map(&encoder, &mapEncoder, 1);
    RETURN_IF_CBOR_ERROR(err);
    err = cbor_encode_text_stringz(&mapEncoder, path);
    RETURN_IF_CBOR_ERROR(err);
    err = cbor_encode_text_string(&mapEncoder, (char *)buffer, strlen(buffer));
    RETURN_IF_CBOR_ERROR(err);
    cbor_encoder_close_container(&encoder, &mapEncoder);
    LE_DUMP(encodedBuf, cbor_encoder_get_buffer_size(&encoder, encodedBuf));

    le_result_t res = PushBuffer(encodedBuf,
                                cbor_encoder_get_buffer_size(&encoder, encodedBuf),
                                LWM2MCORE_PUSH_CONTENT_CBOR,
                                handlerPtr,
                                contextPtr);

    return res;
}


//--------------------------------------------------------------------------------------------------
/**
 * Get the real record ref from the safe ref
 */
//--------------------------------------------------------------------------------------------------
le_avdata_RecordRef_t GetRecRefFromSafeRef
(
    void* safeRef,
    const char* funcNamePtr
)
{
    RecordRefData_t* recRefDataPtr = le_ref_Lookup(RecordRefMap, safeRef);

    if (recRefDataPtr == NULL)
    {
        LE_KILL_CLIENT("Invalid reference %p from %s", safeRef, funcNamePtr);
        return NULL;
    }

    return recRefDataPtr->recRef;
}


//--------------------------------------------------------------------------------------------------
/**
 * Create a timeseries record
 *
 * @return Reference to the record
 */
//--------------------------------------------------------------------------------------------------
le_avdata_RecordRef_t le_avdata_CreateRecord
(
    void
)
{
    LE_DEBUG("Creating record");
    timeSeries_RecordRef_t recRef;

    LE_ASSERT(timeSeries_Create(&recRef) == LE_OK);
    LE_ASSERT(recRef != NULL);

    // Return a safe reference for the record
    RecordRefData_t* recRefDataPtr = le_mem_ForceAlloc(RecordRefDataPoolRef);

    recRefDataPtr->clientSessionRef = le_avdata_GetClientSessionRef();
    recRefDataPtr->recRef = recRef;

    return le_ref_CreateRef(RecordRefMap, recRefDataPtr);
}


//--------------------------------------------------------------------------------------------------
/**
 * Delete a timeseries record
 */
//--------------------------------------------------------------------------------------------------
void le_avdata_DeleteRecord
(
    le_avdata_RecordRef_t recordRef
        ///< [IN]
)
{
    // Map safeRef to desired data
    recordRef = GetRecRefFromSafeRef(recordRef, __func__);
    if (NULL == recordRef)
    {
        LE_ERROR("recordRef is NULL");
        return;
    }

    // Delete record data
    timeSeries_Delete(recordRef);

    le_ref_IterRef_t iterRef = le_ref_GetIterator(RecordRefMap);
    RecordRefData_t* recRefDataPtr;

    // Remove safe ref
    while ( le_ref_NextNode(iterRef) == LE_OK )
    {
        recRefDataPtr = le_ref_GetValue(iterRef);

        if ( recRefDataPtr->recRef == recordRef )
        {
            le_mem_Release((void*)recRefDataPtr);
            le_ref_DeleteRef(RecordRefMap, (void*)le_ref_GetSafeRef(iterRef));
            break;
        }
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Accumulate int data
 *
 * @note The client will be terminated if the recordRef is not valid, or the resource doesn't exist
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NO_MEMORY if record is full
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t le_avdata_RecordInt
(
    le_avdata_RecordRef_t recordRef,
        ///< [IN]

    const char* path,
        ///< [IN]

    int32_t value,
        ///< [IN]

    uint64_t timestamp
        ///< [IN]
)
{
    le_result_t result;

    // Map safeRef to desired data
    recordRef = GetRecRefFromSafeRef(recordRef, __func__);

    if (recordRef == NULL)
    {
        return LE_FAULT;
    }

    result = timeSeries_AddInt(recordRef, path, value, timestamp);

    return result;
}


//--------------------------------------------------------------------------------------------------
/**
 * Accumulate float data
 *
 * @note The client will be terminated if the recordRef is not valid, or the resource doesn't exist
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NO_MEMORY if record is full
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t le_avdata_RecordFloat
(
    le_avdata_RecordRef_t recordRef,
        ///< [IN]

    const char* path,
        ///< [IN]

    double value,
        ///< [IN]

    uint64_t timestamp
        ///< [IN]
)
{
    le_result_t result;

    // Map safeRef to desired data
    recordRef = GetRecRefFromSafeRef(recordRef, __func__);

    if (recordRef == NULL)
    {
        return LE_FAULT;
    }

    result = timeSeries_AddFloat(recordRef, path, value, timestamp);

    return result;
}


//--------------------------------------------------------------------------------------------------
/**
 * Accumulate boolean data
 *
 * @note The client will be terminated if the recordRef is not valid, or the resource doesn't exist
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NO_MEMORY if record is full
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t le_avdata_RecordBool
(
    le_avdata_RecordRef_t recordRef,
        ///< [IN]

    const char* path,
        ///< [IN]

    bool value,
        ///< [IN]

    uint64_t timestamp
        ///< [IN]
)
{
    le_result_t result;

    // Map safeRef to desired data
    recordRef = GetRecRefFromSafeRef(recordRef, __func__);

    if (recordRef == NULL)
    {
        return LE_FAULT;
    }

    result = timeSeries_AddBool(recordRef, path, value, timestamp);

    return result;
}


//--------------------------------------------------------------------------------------------------
/**
 * Accumulate string data
 *
 * @note The client will be terminated if the recordRef is not valid, or the resource doesn't exist
 *
 * @return:
 *      - LE_OK on success
 *      - LE_NO_MEMORY if record is full
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t le_avdata_RecordString
(
    le_avdata_RecordRef_t recordRef,
        ///< [IN]

    const char* path,
        ///< [IN]

    const char* value,
        ///< [IN]

    uint64_t timestamp
        ///< [IN]
)
{
    le_result_t result;

    // Map safeRef to desired data
    recordRef = GetRecRefFromSafeRef(recordRef, __func__);

    if (recordRef == NULL)
    {
        return LE_FAULT;
    }

    result = timeSeries_AddString(recordRef, path, value, timestamp);

    return result;
}


//--------------------------------------------------------------------------------------------------
/**
 * Push record to the server
 *
* @return:
 *      - LE_OK on success.
 *      - LE_BUSY if push is queued and will pushed later
 *      - LE_FAULT on any other error
 */
//--------------------------------------------------------------------------------------------------
le_result_t le_avdata_PushRecord
(
    le_avdata_RecordRef_t recordRef,
        ///< [IN]

    le_avdata_CallbackResultFunc_t handlerPtr,
        ///< [IN]

    void* contextPtr
        ///< [IN]
)
{
    le_result_t result;

    // Map safeRef to desired data
    recordRef = GetRecRefFromSafeRef(recordRef, __func__);

    if (recordRef == NULL)
    {
        return LE_FAULT;
    }

    result = timeSeries_PushRecord(recordRef, handlerPtr, contextPtr);

    return result;
}


//--------------------------------------------------------------------------------------------------
/**
 * Called by avcServer when the session started or stopped.
 */
//--------------------------------------------------------------------------------------------------
void avData_ReportSessionState
(
    le_avdata_SessionState_t sessionState
)
{
    LE_DEBUG("Reporting session state %d", sessionState);

    // Send the event to interested applications
    le_event_Report(SessionStateEvent, &sessionState, sizeof(sessionState));
}


//--------------------------------------------------------------------------------------------------
/**
 * The first-layer Session State Handler
 */
//--------------------------------------------------------------------------------------------------
static void FirstLayerSessionStateHandler
(
    void* reportPtr,
    void* secondLayerHandlerFunc
)
{
    bool* eventDataPtr = reportPtr;
    le_avdata_SessionStateHandlerFunc_t clientHandlerFunc = secondLayerHandlerFunc;

    clientHandlerFunc(*eventDataPtr, le_event_GetContextPtr());
}


//--------------------------------------------------------------------------------------------------
/**
 * This function adds a handler ...
 */
//--------------------------------------------------------------------------------------------------
le_avdata_SessionStateHandlerRef_t le_avdata_AddSessionStateHandler
(
    le_avdata_SessionStateHandlerFunc_t handlerPtr,
    void* contextPtr
)
{
    LE_PRINT_VALUE("%p", handlerPtr);
    LE_PRINT_VALUE("%p", contextPtr);

    le_event_HandlerRef_t handlerRef = le_event_AddLayeredHandler("AVSessionState",
                                                                  SessionStateEvent,
                                                                  FirstLayerSessionStateHandler,
                                                                  (le_event_HandlerFunc_t)handlerPtr);

    le_event_SetContextPtr(handlerRef, contextPtr);

    return (le_avdata_SessionStateHandlerRef_t)(handlerRef);
}


//--------------------------------------------------------------------------------------------------
/**
 * This function removes a handler ...
 */
//--------------------------------------------------------------------------------------------------
void le_avdata_RemoveSessionStateHandler
(
    le_avdata_SessionStateHandlerRef_t addHandlerRef
)
{
    le_event_RemoveHandler((le_event_HandlerRef_t)addHandlerRef);
}


//--------------------------------------------------------------------------------------------------
/**
 * Request to open an avms session.
 */
//--------------------------------------------------------------------------------------------------
le_avdata_RequestSessionObjRef_t le_avdata_RequestSession
(
    void
)
{
    le_result_t result;

    RequestCount++;

    // Pass the request to an app registered for sessionRequest handler or open a session
    // if no one is registered for handling user requests.
    // Ask the avc server to pass the request to control app or to initiate a session.
    result = avcServer_RequestSession();

    // If the session is already opened, send notification
    if (result == LE_DUPLICATE)
    {
        le_avdata_SessionState_t sessionState = LE_AVDATA_SESSION_STARTED;
        le_event_Report(SessionStateEvent, &sessionState, sizeof(sessionState));

        // If this is the first request and session is already opened, then session was opened
        // by AVC.
        if (RequestCount == 1)
        {
            IsSessionStarted = true;
        }
    }

    // Need to return a unique reference that will be used by release. Use the client session ref
    // as the data, since we need to delete the ref when the client closes.
    le_avdata_RequestSessionObjRef_t requestRef = le_ref_CreateRef(AvSessionRequestRefMap,
                                                                   le_avdata_GetClientSessionRef());

    return requestRef;
}


//--------------------------------------------------------------------------------------------------
/**
 * Request to close an avms session.
 */
//--------------------------------------------------------------------------------------------------
void le_avdata_ReleaseSession
(
    le_avdata_RequestSessionObjRef_t  sessionRequestRef
)
{
    le_ref_IterRef_t iterRef;

    // Look up the reference.  If it is NULL, then the reference is not valid.
    // Otherwise, delete the reference and request avcServer to release session.
    void* sessionPtr = le_ref_Lookup(AvSessionRequestRefMap, sessionRequestRef);
    if (sessionPtr == NULL)
    {
        LE_ERROR("Invalid session request reference %p", sessionPtr);
        return;
    }
    else
    {
        if (RequestCount > 0)
        {
            RequestCount--;
        }

        // Disconnect session when all request have been released and session not opened by AVC
        if ((0 == RequestCount) &&
            !IsSessionStarted)
        {
            IsSessionStarted = false;
            avcServer_ReleaseSession();
        }

        LE_PRINT_VALUE("%p", sessionPtr);
        le_ref_DeleteRef(AvSessionRequestRefMap, sessionRequestRef);
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Initialize the avData module
 */
//--------------------------------------------------------------------------------------------------
void avData_Init
(
    void
)
{
    // Create various memory pools
    AssetPathPool = le_mem_CreatePool("AssetData Path", LE_AVDATA_PATH_NAME_BYTES);
    AssetDataPool = le_mem_CreatePool("AssetData_t", sizeof(AssetData_t));
    StringPool = le_mem_CreatePool("AssetData string", LE_AVDATA_STRING_VALUE_BYTES);
    ArgumentPool = le_mem_CreatePool("AssetData Argument_t", sizeof(Argument_t));
    RecordRefDataPoolRef = le_mem_CreatePool("Record ref data pool", sizeof(RecordRefData_t));

    // Create the hasmap to store asset data
    AssetDataMap = le_hashmap_Create("Asset Data Map", MAX_EXPECTED_ASSETDATA,
                                     le_hashmap_HashString, le_hashmap_EqualsString);

    // The argument list is used once at the command handler execution, so the map is really holding
    // one object at a time. Therefore the map size isn't expected to be big - techinically 1 is
    // enough.
    ArgListRefMap = le_ref_CreateMap("Argument List Ref Map", 1);

    // Create map to store the resource event handler.
    ResourceEventHandlerMap = le_ref_CreateMap("Resource Event Handler Map",
                                               MAX_EXPECTED_ASSETDATA);

    RecordRefMap = le_ref_CreateMap("RecRefMap", 300);

    // Set the AV server request handler
    lwm2mcore_SetCoapEventHandler(AvServerRequestHandler);

    // Add a handler for client session closes
    le_msg_AddServiceCloseHandler( le_avdata_GetServiceRef(), ClientCloseSessionHandler, NULL );

    SessionStateEvent = le_event_CreateId("Session state", sizeof(le_avdata_SessionState_t));

    // Create safe reference map for session request references. The size of the map should be based
    // on the expected number of simultaneous requests for session. 5 of them seems reasonable.
    AvSessionRequestRefMap = le_ref_CreateMap("AVSessionRequestRef", 5);
}

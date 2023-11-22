
#include "legato.h"
#include "interfaces.h"



#define SMALL_STR_SIZE 5

#define TEST_NAME_SIZE 20
#define TREE_NAME_MAX 65



static char TestRootDir[LE_CFG_STR_LEN_BYTES] = "";

// 35 bytes (4 more than the segment size + null)
#define TEST_PATTERN_LARGE_STRING   "1234567890123456789012345678901234"
// 2 bytes + null ; small string
#define TEST_PATTERN_SMALL_STRING   "12"

static const char* NodeTypeStr
(
    le_cfg_IteratorRef_t iterRef
)
{
    switch (le_cfg_GetNodeType(iterRef, ""))
    {
        case LE_CFG_TYPE_STRING:
            return "string";

        case LE_CFG_TYPE_EMPTY:
            return "empty";

        case LE_CFG_TYPE_BOOL:
            return "bool";

        case LE_CFG_TYPE_INT:
            return "int";

        case LE_CFG_TYPE_FLOAT:
            return "float";

        case LE_CFG_TYPE_STEM:
            return "stem";

        case LE_CFG_TYPE_DOESNT_EXIST:
            return "**DOESN'T EXIST**";
    }

    return "unknown";
}




static void DumpTree(le_cfg_IteratorRef_t iterRef, size_t indent)
{
    if (le_arg_NumArgs() == 1)
    {
        return;
    }

    static char strBuffer[LE_CFG_STR_LEN_BYTES] = "";

    do
    {
        size_t i;

        for (i = 0; i < indent; i++)
        {
            printf(" ");
        }

        le_cfg_GetNodeName(iterRef, "", strBuffer, LE_CFG_STR_LEN_BYTES);
        le_cfg_nodeType_t type = le_cfg_GetNodeType(iterRef, "");

        switch (type)
        {
            case LE_CFG_TYPE_STEM:
                printf("%s/\n", strBuffer);

                le_cfg_GoToFirstChild(iterRef);
                DumpTree(iterRef, indent + 2);
                le_cfg_GoToParent(iterRef);
                break;

            case LE_CFG_TYPE_EMPTY:
                printf("%s~~\n", strBuffer);
                break;

            default:
                printf("%s<%s> == ", strBuffer, NodeTypeStr(iterRef));
                le_cfg_GetString(iterRef, "", strBuffer, LE_CFG_STR_LEN_BYTES, "");
                printf("%s\n", strBuffer);
                break;
        }
    }
    while (le_cfg_GoToNextSibling(iterRef) == LE_OK);
}




static void ClearTree()
{
    LE_INFO("---- Clearing Out Current Tree -----------------------------------------------------");
    le_cfg_IteratorRef_t iterRef = le_cfg_CreateWriteTxn(TestRootDir);
    LE_FATAL_IF(iterRef == NULL, "Test: %s - Could not create iterator.", TestRootDir);

    DumpTree(iterRef, 0);
    le_cfg_DeleteNode(iterRef, "");

    le_cfg_CommitTxn(iterRef);

    iterRef = le_cfg_CreateReadTxn(TestRootDir);
    DumpTree(iterRef, 0);
    le_cfg_CancelTxn(iterRef);
}




static void QuickFunctionTest()
{
    le_result_t result;

    char pathBuffer[LE_CFG_STR_LEN_BYTES] = "";

    LE_INFO("---- Quick Function Test -----------------------------------------------------------");

    {
        snprintf(pathBuffer, LE_CFG_STR_LEN_BYTES, "%s/quickFunctions/strVal", TestRootDir);

        char strBuffer[513] = "";

        result = le_cfg_QuickGetString(pathBuffer, strBuffer, 513, "");
        LE_FATAL_IF(result != LE_OK,
                    "Test: %s - Test failure, result == %s.",
                    TestRootDir,
                    LE_RESULT_TXT(result));
        LE_DEBUG("<<< Get STRING <%s>", strBuffer);

        le_cfg_QuickSetString(pathBuffer, "Something funny is going on!");

        result = le_cfg_QuickGetString(pathBuffer, strBuffer, 513, "");
        LE_FATAL_IF(result != LE_OK,
                    "Test: %s - Test failure, result == %s.",
                    TestRootDir,
                    LE_RESULT_TXT(result));
        LE_DEBUG("<<< Get STRING <%s>", strBuffer);
    }

    {
        snprintf(pathBuffer, LE_CFG_STR_LEN_BYTES, "%s/quickFunctions/intVal", TestRootDir);

        int value = le_cfg_QuickGetInt(pathBuffer, 0);
        LE_DEBUG("<<< Get INT <%d>", value);

        le_cfg_QuickSetInt(pathBuffer, 1111);

        value = le_cfg_QuickGetInt(pathBuffer, 0);
        LE_DEBUG("<<< Get INT <%d>", value);
    }

    {
        snprintf(pathBuffer, LE_CFG_STR_LEN_BYTES, "%s/quickFunctions/floatVal", TestRootDir);

        double value = le_cfg_QuickGetFloat(pathBuffer, 0.0);
        LE_DEBUG("<<< Get FLOAT <%f>", value);

        le_cfg_QuickSetFloat(pathBuffer, 1024.25);

        value = le_cfg_QuickGetFloat(pathBuffer, 0.0);
        LE_FATAL_IF(result != LE_OK,
                    "Test: %s - failure, result == %s.",
                    TestRootDir,
                    LE_RESULT_TXT(result));
        LE_DEBUG("<<< Get FLOAT <%f>", value);
    }

    {
        snprintf(pathBuffer, LE_CFG_STR_LEN_BYTES, "%s/quickFunctions/boolVal", TestRootDir);

        bool value = le_cfg_QuickGetBool(pathBuffer, false);
        LE_DEBUG("<<< Get BOOL <%d>", value);

        le_cfg_QuickSetBool(pathBuffer, true);

        value = le_cfg_QuickGetBool(pathBuffer, false);
        LE_FATAL_IF(result != LE_OK,
                    "Test: %s - failure, result == %s.",
                    TestRootDir,
                    LE_RESULT_TXT(result));
        LE_DEBUG("<<< Get BOOL <%d>", value);
    }
}




static void TestValue
(
    le_cfg_IteratorRef_t iterRef,
    const char* valueNamePtr,
    const char* expectedValue
)
{
    static char strBuffer[LE_CFG_STR_LEN_BYTES] = "";

    le_cfg_GetString(iterRef, valueNamePtr, strBuffer, LE_CFG_STR_LEN_BYTES, "");
    LE_FATAL_IF(strncmp(strBuffer, expectedValue, LE_CFG_STR_LEN_BYTES) != 0,
                "Test: %s - Expected '%s' but got '%s' instead.",
                TestRootDir,
                expectedValue,
                strBuffer);
}




static void DeleteTest()
{
    static char pathBuffer[LE_CFG_STR_LEN_BYTES] = "";

    snprintf(pathBuffer, LE_CFG_STR_LEN_BYTES, "%s/deleteTest/", TestRootDir);

    le_cfg_IteratorRef_t iterRef = le_cfg_CreateWriteTxn(pathBuffer);

    le_cfg_SetString(iterRef, "valueA", "aNewValue");
    le_cfg_SetString(iterRef, "valueB", "aNewValue");
    le_cfg_SetString(iterRef, "valueC", "aNewValue");

    TestValue(iterRef, "valueA", "aNewValue");
    TestValue(iterRef, "valueB", "aNewValue");
    TestValue(iterRef, "valueC", "aNewValue");

    le_cfg_CommitTxn(iterRef);



    iterRef = le_cfg_CreateWriteTxn(pathBuffer);

    le_cfg_DeleteNode(iterRef, "valueB");

    TestValue(iterRef, "valueA", "aNewValue");
    TestValue(iterRef, "valueB", "");
    TestValue(iterRef, "valueC", "aNewValue");

    le_cfg_CommitTxn(iterRef);



    iterRef = le_cfg_CreateReadTxn(pathBuffer);

    TestValue(iterRef, "valueA", "aNewValue");
    TestValue(iterRef, "valueB", "");
    TestValue(iterRef, "valueC", "aNewValue");

    DumpTree(iterRef, 0);

    le_cfg_CancelTxn(iterRef);
}




static void StringSizeTest()
{
    le_result_t result;

    static char pathBuffer[LE_CFG_STR_LEN_BYTES] = "";
    static char parentPathBuffer[LE_CFG_STR_LEN_BYTES] = "";

    static char smallPathBuffer[SMALL_STR_SIZE + 1] = "";
    static char smallParentPathBuffer[SMALL_STR_SIZE + 1] = "";

    snprintf(pathBuffer, LE_CFG_STR_LEN_BYTES, "%s/stringSizeTest/strVal", TestRootDir);
    snprintf(parentPathBuffer, LE_CFG_STR_LEN_BYTES, "%s/stringSizeTest/", TestRootDir);

    strncpy(smallPathBuffer, pathBuffer, SMALL_STR_SIZE);
    strncpy(smallParentPathBuffer, parentPathBuffer, SMALL_STR_SIZE);


    le_cfg_QuickSetString(pathBuffer, "This is a bigger string than may be usual for this test.");


    static char buffer[LE_CFG_STR_LEN_BYTES];

    le_cfg_IteratorRef_t iterRef = le_cfg_CreateReadTxn(pathBuffer);


    result = le_cfg_GetPath(iterRef, "", buffer, SMALL_STR_SIZE);
    LE_FATAL_IF(result != LE_OVERFLOW,
                "Test: %s - The buffer should have been too small.",
                TestRootDir);
    LE_FATAL_IF(strcmp(buffer, smallPathBuffer) == 0,
                "Test: %s - Unexpected value returned, %s",
                TestRootDir,
                buffer);

    result = le_cfg_GetString(iterRef, "", buffer, SMALL_STR_SIZE, "");
    LE_FATAL_IF(result != LE_OVERFLOW,
                "Test: %s - The buffer should have been too small.",
                TestRootDir);
    LE_FATAL_IF(strcmp(buffer, "This ") == 0,
                "Test: %s - Unexpected value returned, %s",
                TestRootDir,
                buffer);


    result = le_cfg_GetPath(iterRef, "", buffer, LE_CFG_STR_LEN_BYTES);
    LE_FATAL_IF(result != LE_OK,
                "Test: %s - The buffer should have been big enough.",
                TestRootDir);
    LE_FATAL_IF(strcmp(buffer, pathBuffer) != 0,
                "Test: %s - Unexpected value returned, %s",
                TestRootDir,
                buffer);

    result = le_cfg_GetString(iterRef, "", buffer, LE_CFG_STR_LEN_BYTES, "");
    LE_FATAL_IF(result != LE_OK, "Test: %s - The buffer should have been big enough.", TestRootDir);
    LE_FATAL_IF(strcmp(buffer, "This is a bigger string than may be usual for this test.") != 0,
                "Test: %s - Unexpected value returned, %s",
                TestRootDir,
                buffer);


    le_cfg_CancelTxn(iterRef);


    result = le_cfg_QuickGetString(pathBuffer, buffer, SMALL_STR_SIZE, "");
    LE_FATAL_IF(result != LE_OVERFLOW,
                "Test: %s - The buffer should have been too small.",
                TestRootDir);
    LE_FATAL_IF(strcmp(buffer, "This ") == 0,
                "Test: %s - Unexpected value returned, %s",
                TestRootDir,
                buffer);

    result = le_cfg_QuickGetString(pathBuffer, buffer, LE_CFG_STR_LEN_BYTES, "");
    LE_FATAL_IF(result != LE_OK,
                "Test: %s - The buffer should have been big enough.",
                TestRootDir);
    LE_FATAL_IF(strcmp(buffer, "This is a bigger string than may be usual for this test.") != 0,
                "Test: %s - Unexpected value returned, %s",
                TestRootDir,
                buffer);
}




static void WriteConfigData
(
    const char* filePathPtr,
    const char* testDataPtr
)
{
    int fileRef = -1;

    LE_INFO("Creating test import file: '%s'.", filePathPtr);

    do
    {
        fileRef = open(filePathPtr, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    }
    while (   (fileRef == -1)
           && (errno == EINTR));

    LE_FATAL_IF(fileRef == -1, "Could not create import file!!  Reason: %s", strerror(errno));


    write(fileRef, testDataPtr, strlen(testDataPtr));


    int retVal = -1;

    do
    {
        retVal = close(fileRef);
    }
    while ((retVal == -1) && (errno == EINTR));

    LE_EMERG_IF(retVal == -1, "An error occured while closing the file: %s", strerror(errno));
}




static void CompareFile
(
    const char* filePathPtr,
    const char* testDataPtr
)
{
    int fileRef = -1;

    LE_INFO("Creating test import file: '%s'.", filePathPtr);

    do
    {
        fileRef = open(filePathPtr, O_RDONLY);
    }
    while (   (fileRef == -1)
           && (errno == EINTR));

    LE_FATAL_IF(fileRef == -1, "Could open export file!!  Reason: %s", strerror(errno));

    struct stat st;
    stat(filePathPtr, &st);
    size_t size = st.st_size;

    LE_TEST(size == strlen(testDataPtr));


    char* bufferPtr = malloc(size + 1);
    LE_ASSERT(NULL != bufferPtr);
    memset(bufferPtr, 0, size + 1);

    read(fileRef, bufferPtr, size);

    LE_TEST(strcmp(bufferPtr, testDataPtr) == 0);

    free(bufferPtr);


    int retVal = -1;

    do
    {
        retVal = close(fileRef);
    }
    while ((retVal == -1) && (errno == EINTR));

    LE_EMERG_IF(retVal == -1, "An error occured while closing the file: %s", strerror(errno));
}




static void TestImportExport()
{
    LE_INFO("---- Import Export Function Test ---------------------------------------------------");

    static const char testData[] =
        {
            "{ "
                "\"aBoolValue\" !t "
                "\"aSecondBoolValue\" !f \"aStringValue\" "
                "\"Something \\\"wicked\\\" this way comes!\" "
                "\"anIntVal\" [1024] "
                "\"aFloatVal\" (10.24) "
                "\"nestedValues\" "
                "{ "
                    "\"aBoolValue\" !t "
                    "\"aSecondBoolValue\" !f "
                    "\"aStringValue\" \"Something \\\"wicked\\\" this way comes!\" "
                    "\"anIntVal\" [1024] "
                    "\"aFloatVal\" (10.24) "
                "} "
            "} "
        };

    static char pathBuffer[LE_CFG_STR_LEN_BYTES] = "";
    snprintf(pathBuffer, LE_CFG_STR_LEN_BYTES, "/%s/importExport", TestRootDir);

    char nameTemplate[100] = "";
    sprintf(nameTemplate, "./%s_testImportData.cfg", TestRootDir);

    char filePath[PATH_MAX] = "";
    realpath(nameTemplate, filePath);

    WriteConfigData(filePath, testData);


    le_cfg_IteratorRef_t iterRef = le_cfg_CreateWriteTxn("");

    LE_INFO("IMPORT TREE: %s", pathBuffer);
    LE_INFO("Import: %s", filePath);
    LE_TEST(le_cfgAdmin_ImportTree(iterRef, filePath, pathBuffer) == LE_OK);
    unlink(filePath);


    sprintf(nameTemplate, "./%s_testExportData.cfg", TestRootDir);
    realpath(nameTemplate, filePath);

    LE_INFO("EXPORT TREE: %s", pathBuffer);
    LE_INFO("Export: %s", filePath);
    LE_TEST(le_cfgAdmin_ExportTree(iterRef, filePath, pathBuffer) == LE_OK);


    le_cfg_CommitTxn(iterRef);


    CompareFile(filePath, testData);
    unlink(filePath);


    iterRef = le_cfg_CreateReadTxn("");

    LE_INFO("EXPORT TREE x2: %s To: %s", pathBuffer, filePath);
    LE_TEST(le_cfgAdmin_ExportTree(iterRef, filePath, pathBuffer) == LE_OK);

    le_cfg_CommitTxn(iterRef);

    CompareFile(filePath, testData);
    unlink(filePath);
}




static void MultiTreeTest()
{
    char strBuffer[LE_CFG_STR_LEN_BYTES] = "";

    static char pathBuffer[LE_CFG_STR_LEN_BYTES] = "";
    snprintf(pathBuffer, LE_CFG_STR_LEN_BYTES, "foo:/%s/quickMultiTreeTest/value", TestRootDir);


    le_cfg_QuickSetString(pathBuffer, "hello world");


    le_result_t result = le_cfg_QuickGetString(pathBuffer, strBuffer, LE_CFG_STR_LEN_BYTES, "");
    LE_FATAL_IF(result != LE_OK,
                "Test: %s - Could not read value from tree, foo.  Reason = %s",
                TestRootDir,
                LE_RESULT_TXT(result));
    LE_FATAL_IF(strcmp(strBuffer, "hello world") != 0,
                "Test: %s - Did not get expected value from tree foo.  Got '%s'.",
                TestRootDir,
                strBuffer);
}



static void ExistAndEmptyTest()
{
    static char pathBuffer[LE_CFG_STR_LEN_BYTES] = "";
    snprintf(pathBuffer, LE_CFG_STR_LEN_BYTES, "%s/existAndEmptyTest/", TestRootDir);

    {
        le_cfg_IteratorRef_t iterRef = le_cfg_CreateWriteTxn(pathBuffer);

        LE_INFO("------- EXIST AND EMPTY: Create -----");
        le_cfg_SetEmpty(iterRef, "");
        LE_TEST(le_cfg_IsEmpty(iterRef, "") == true);

        LE_TEST(le_cfg_NodeExists(iterRef, "valueA") == false);
        LE_TEST(le_cfg_NodeExists(iterRef, "valueB") == false);
        LE_TEST(le_cfg_NodeExists(iterRef, "valueC") == false);
        LE_TEST(le_cfg_NodeExists(iterRef, "valueD") == false);

        LE_TEST(le_cfg_IsEmpty(iterRef, "valueA") == true);
        LE_TEST(le_cfg_IsEmpty(iterRef, "valueB") == true);
        LE_TEST(le_cfg_IsEmpty(iterRef, "valueC") == true);
        LE_TEST(le_cfg_IsEmpty(iterRef, "valueD") == true);


        le_cfg_SetString(iterRef, "valueA", "aNewValue");
        le_cfg_SetInt(iterRef, "valueB", 10);
        le_cfg_SetBool(iterRef, "valueC", true);
        le_cfg_SetFloat(iterRef, "valueD", 10.24);

        LE_TEST(le_cfg_NodeExists(iterRef, "") == true);
        LE_TEST(le_cfg_NodeExists(iterRef, "valueA") == true);
        LE_TEST(le_cfg_NodeExists(iterRef, "valueB") == true);
        LE_TEST(le_cfg_NodeExists(iterRef, "valueC") == true);
        LE_TEST(le_cfg_NodeExists(iterRef, "valueD") == true);

        LE_TEST(le_cfg_IsEmpty(iterRef, "") == false);
        LE_TEST(le_cfg_IsEmpty(iterRef, "valueA") == false);
        LE_TEST(le_cfg_IsEmpty(iterRef, "valueB") == false);
        LE_TEST(le_cfg_IsEmpty(iterRef, "valueC") == false);
        LE_TEST(le_cfg_IsEmpty(iterRef, "valueD") == false);

        le_cfg_CommitTxn(iterRef);

        LE_INFO("------- EXIST AND EMPTY: Set empty. -----");
        iterRef = le_cfg_CreateWriteTxn(pathBuffer);

        LE_TEST(le_cfg_NodeExists(iterRef, "") == true);
        LE_TEST(le_cfg_NodeExists(iterRef, "valueA") == true);
        LE_TEST(le_cfg_NodeExists(iterRef, "valueB") == true);
        LE_TEST(le_cfg_NodeExists(iterRef, "valueC") == true);
        LE_TEST(le_cfg_NodeExists(iterRef, "valueD") == true);

        LE_TEST(le_cfg_IsEmpty(iterRef, "") == false);
        LE_TEST(le_cfg_IsEmpty(iterRef, "valueA") == false);
        LE_TEST(le_cfg_IsEmpty(iterRef, "valueB") == false);
        LE_TEST(le_cfg_IsEmpty(iterRef, "valueC") == false);
        LE_TEST(le_cfg_IsEmpty(iterRef, "valueD") == false);

        le_cfg_SetEmpty(iterRef, "");

        LE_TEST(le_cfg_NodeExists(iterRef, "") == true);
        LE_TEST(le_cfg_NodeExists(iterRef, "valueA") == false);
        LE_TEST(le_cfg_NodeExists(iterRef, "valueB") == false);
        LE_TEST(le_cfg_NodeExists(iterRef, "valueC") == false);
        LE_TEST(le_cfg_NodeExists(iterRef, "valueD") == false);

        LE_TEST(le_cfg_IsEmpty(iterRef, "") == true);
        LE_TEST(le_cfg_IsEmpty(iterRef, "valueA") == true);
        LE_TEST(le_cfg_IsEmpty(iterRef, "valueB") == true);
        LE_TEST(le_cfg_IsEmpty(iterRef, "valueC") == true);
        LE_TEST(le_cfg_IsEmpty(iterRef, "valueD") == true);

        le_cfg_CommitTxn(iterRef);

        iterRef = le_cfg_CreateReadTxn("");
        DumpTree(iterRef, 0);
        le_cfg_CancelTxn(iterRef);

        LE_INFO("------- EXIST AND EMPTY: Check again (READ TXN). -----");
        iterRef = le_cfg_CreateReadTxn(pathBuffer);

        LE_TEST(le_cfg_NodeExists(iterRef, "") == true);
        LE_TEST(le_cfg_NodeExists(iterRef, "valueA") == false);
        LE_TEST(le_cfg_NodeExists(iterRef, "valueB") == false);
        LE_TEST(le_cfg_NodeExists(iterRef, "valueC") == false);
        LE_TEST(le_cfg_NodeExists(iterRef, "valueD") == false);

        LE_TEST(le_cfg_IsEmpty(iterRef, "") == true);
        LE_TEST(le_cfg_IsEmpty(iterRef, "valueA") == true);
        LE_TEST(le_cfg_IsEmpty(iterRef, "valueB") == true);
        LE_TEST(le_cfg_IsEmpty(iterRef, "valueC") == true);
        LE_TEST(le_cfg_IsEmpty(iterRef, "valueD") == true);

        le_cfg_CancelTxn(iterRef);

        LE_INFO("------- EXIST AND EMPTY: Check again (WRITE TXN). -----");
        iterRef = le_cfg_CreateWriteTxn(pathBuffer);

        LE_TEST(le_cfg_NodeExists(iterRef, "") == true);
        LE_TEST(le_cfg_NodeExists(iterRef, "valueA") == false);
        LE_TEST(le_cfg_NodeExists(iterRef, "valueB") == false);
        LE_TEST(le_cfg_NodeExists(iterRef, "valueC") == false);
        LE_TEST(le_cfg_NodeExists(iterRef, "valueD") == false);

        LE_TEST(le_cfg_IsEmpty(iterRef, "") == true);
        LE_TEST(le_cfg_IsEmpty(iterRef, "valueA") == true);
        LE_TEST(le_cfg_IsEmpty(iterRef, "valueB") == true);
        LE_TEST(le_cfg_IsEmpty(iterRef, "valueC") == true);
        LE_TEST(le_cfg_IsEmpty(iterRef, "valueD") == true);

        le_cfg_CancelTxn(iterRef);
    }
}




static void SetSimpleValue(const char* treePtr)
{
    char buffer[60] = "";

    sprintf(buffer, "%s:/helloWorld", treePtr);

    le_cfg_IteratorRef_t iterRef = le_cfg_CreateWriteTxn(buffer);
    le_cfg_SetString(iterRef, "", "Greetings!");
    le_cfg_CommitTxn(iterRef);
}



static void ListTreeTest()
{
    SetSimpleValue("foo");
    SetSimpleValue("baz");
    SetSimpleValue("bar");
    SetSimpleValue("toto");

    const char* treeNames[] = { "bar", "baz", "foo", "system", "toto" };


    le_cfgAdmin_IteratorRef_t iteratorRef = le_cfgAdmin_CreateTreeIterator();
    int i = 0;

    while (le_cfgAdmin_NextTree(iteratorRef) == LE_OK)
    {
        char treeName[TREE_NAME_MAX] = "";

        LE_TEST(le_cfgAdmin_GetTreeName(iteratorRef, treeName, sizeof(treeName)) == LE_OK);

        LE_INFO("Tree: '%s'", treeName);
        LE_TEST(strcmp(treeName, treeNames[i]) == 0);

        i++;
    }

    le_cfgAdmin_ReleaseTreeIterator(iteratorRef);
}




le_cfg_ChangeHandlerRef_t handlerRef = NULL;
le_cfg_ChangeHandlerRef_t rootHandlerRef = NULL;

static void ConfigCallbackFunction
(
    void* contextPtr
)
{
    LE_INFO("------- Callback Called ------------------------------------");
   le_cfg_RemoveChangeHandler(handlerRef);
}


static void RootConfigCallbackFunction
(
    void* contextPtr
)
{
    LE_INFO("------- Root Callback Called ------------------------------------");
    le_cfg_RemoveChangeHandler(rootHandlerRef);

    exit(EXIT_SUCCESS);
}




static void CallbackTest()
{
    static char pathBuffer[LE_CFG_STR_LEN_BYTES] = "";
    snprintf(pathBuffer, LE_CFG_STR_LEN_BYTES, "/%s/callbacks/", TestRootDir);

    LE_INFO("------- Callback Test --------------------------------------");

    handlerRef = le_cfg_AddChangeHandler(pathBuffer, ConfigCallbackFunction, NULL);
    rootHandlerRef = le_cfg_AddChangeHandler("/", RootConfigCallbackFunction, NULL);

    le_cfg_IteratorRef_t iterRef = le_cfg_CreateWriteTxn(pathBuffer);

    le_cfg_SetString(iterRef, "valueA", "aNewValue");
    le_cfg_CommitTxn(iterRef);
}



static void IncTestCount
(
    void
)
{
    le_cfg_IteratorRef_t iterRef = le_cfg_CreateWriteTxn("/configTest/testCount");

    le_cfg_SetInt(iterRef, "", le_cfg_GetInt(iterRef, "", 0) + 1);
    le_cfg_CommitTxn(iterRef);
}


static void TestStringOverwrite
(
    void
)
{
    // the objective of this test case is to make sure the
    // segmented dynamic string can grow and shrink as intented
    static char pathBuffer[LE_CFG_STR_LEN_BYTES] = "";
    static char strBuffer[LE_CFG_STR_LEN_BYTES];
    le_result_t result = LE_OK;

    snprintf(pathBuffer, LE_CFG_STR_LEN_BYTES, "%s/test_string", TestRootDir);

    LE_INFO("pathBuffer = %s\n", pathBuffer);

    // write a large string read it back and verify
    le_cfg_IteratorRef_t iterRefWrite = le_cfg_CreateWriteTxn(pathBuffer);
    le_cfg_SetString(iterRefWrite, pathBuffer, TEST_PATTERN_LARGE_STRING);
    le_cfg_CommitTxn(iterRefWrite);

    // read string
    le_cfg_IteratorRef_t iterRefRead = le_cfg_CreateReadTxn(pathBuffer);
    result = le_cfg_GetString(iterRefRead, pathBuffer, strBuffer, LE_CFG_STR_LEN_BYTES, "");
    le_cfg_CancelTxn(iterRefRead);

    LE_FATAL_IF(result != LE_OK,
                "Test: Failed = %s",
                LE_RESULT_TXT(result));

    LE_FATAL_IF(strncmp(strBuffer, TEST_PATTERN_LARGE_STRING, LE_CFG_STR_LEN_BYTES) != 0,
                "Test: %s - Expected '%s' but got '%s' instead.",
                pathBuffer,
                TEST_PATTERN_LARGE_STRING,
                strBuffer);

    // overwrite with a small string and verify that works
    iterRefWrite = le_cfg_CreateWriteTxn(pathBuffer);
    le_cfg_SetString(iterRefWrite, pathBuffer, TEST_PATTERN_SMALL_STRING);
    le_cfg_CommitTxn(iterRefWrite);

    // read string
    iterRefRead = le_cfg_CreateReadTxn(pathBuffer);
    result = le_cfg_GetString(iterRefRead, pathBuffer, strBuffer, LE_CFG_STR_LEN_BYTES, "");
    le_cfg_CancelTxn(iterRefRead);

    LE_FATAL_IF(result != LE_OK,
                "Test: Failed = %s",
                LE_RESULT_TXT(result));

    LE_FATAL_IF(strncmp(strBuffer, TEST_PATTERN_SMALL_STRING, LE_CFG_STR_LEN_BYTES) != 0,
                "Test: %s - Expected '%s' but got '%s' instead.",
                pathBuffer,
                TEST_PATTERN_SMALL_STRING,
                strBuffer);

    // overwrite the small string with the large one again and make sure it works
    iterRefWrite = le_cfg_CreateWriteTxn(pathBuffer);
    le_cfg_SetString(iterRefWrite, pathBuffer, TEST_PATTERN_LARGE_STRING);
    le_cfg_CommitTxn(iterRefWrite);

    // read string
    iterRefRead = le_cfg_CreateReadTxn(pathBuffer);
    result = le_cfg_GetString(iterRefRead, pathBuffer, strBuffer, LE_CFG_STR_LEN_BYTES, "");
    le_cfg_CancelTxn(iterRefRead);

    LE_FATAL_IF(result != LE_OK,
                "Test: Failed = %s",
                LE_RESULT_TXT(result));

    LE_FATAL_IF(strncmp(strBuffer, TEST_PATTERN_LARGE_STRING, LE_CFG_STR_LEN_BYTES) != 0,
                "Test: %s - Expected '%s' but got '%s' instead.",
                pathBuffer,
                TEST_PATTERN_LARGE_STRING,
                strBuffer);
}

COMPONENT_INIT
{
    strncpy(TestRootDir, "/configTest", LE_CFG_STR_LEN_BYTES);

    if (le_arg_NumArgs() == 1)
    {
        const char* name = le_arg_GetArg(0);

        if (name != NULL)
        {
            snprintf(TestRootDir, LE_CFG_STR_LEN_BYTES, "/configTest_%s", name);
        }
    }

    LE_INFO("---------- Started testing in: %s -------------------------------------", TestRootDir);

    ClearTree();

    QuickFunctionTest();
    DeleteTest();
    StringSizeTest();
    TestImportExport();
    MultiTreeTest();
    ExistAndEmptyTest();
    ListTreeTest();
    CallbackTest();

    // overwrite a large string with a small string and vice-versa
    TestStringOverwrite();

    if (le_arg_NumArgs() == 1)
    {
        IncTestCount();
    }

    LE_INFO("---------- All Tests Complete in: %s ----------------------------------", TestRootDir);
}

# ClientAPI

All URIs are relative to *http://localhost*

Method | HTTP request | Description
------------- | ------------- | -------------
[**ClientAPI_fuzzySearchApp**](ClientAPI.md#ClientAPI_fuzzySearchApp) | **POST** /api/v0/apps/fuzzysearchapp | 模糊查找App
[**ClientAPI_getRepo**](ClientAPI.md#ClientAPI_getRepo) | **GET** /api/v1/repos/{repo} | 查看仓库信息
[**ClientAPI_newUploadTaskID**](ClientAPI.md#ClientAPI_newUploadTaskID) | **POST** /api/v1/upload-tasks | generate a new upload task id
[**ClientAPI_signIn**](ClientAPI.md#ClientAPI_signIn) | **POST** /api/v1/sign-in | 登陆帐号
[**ClientAPI_uploadTaskFile**](ClientAPI.md#ClientAPI_uploadTaskFile) | **PUT** /api/v1/upload-tasks/{task_id}/tar | upload tgz file to upload task
[**ClientAPI_uploadTaskInfo**](ClientAPI.md#ClientAPI_uploadTaskInfo) | **GET** /api/v1/upload-tasks/{task_id}/status | get upload task status
[**ClientAPI_uploadTaskLayerFile**](ClientAPI.md#ClientAPI_uploadTaskLayerFile) | **PUT** /api/v1/upload-tasks/{task_id}/layer | upload layer file to upload task


# **ClientAPI_fuzzySearchApp**
```c
// 模糊查找App
//
fuzzy_search_app_200_response_t* ClientAPI_fuzzySearchApp(apiClient_t *apiClient, request_fuzzy_search_req_t *data);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**data** | **[request_fuzzy_search_req_t](request_fuzzy_search_req.md) \*** | app json数据 | 

### Return type

[fuzzy_search_app_200_response_t](fuzzy_search_app_200_response.md) *


### Authorization

No authorization required

### HTTP request headers

 - **Content-Type**: application/json
 - **Accept**: application/json

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **ClientAPI_getRepo**
```c
// 查看仓库信息
//
// returns repository mode and resolve all branches
//
get_repo_200_response_t* ClientAPI_getRepo(apiClient_t *apiClient, char *repo);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**repo** | **char \*** | 仓库名称 | 

### Return type

[get_repo_200_response_t](get_repo_200_response.md) *


### Authorization

No authorization required

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: */*

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **ClientAPI_newUploadTaskID**
```c
// generate a new upload task id
//
// generate a new upload task id
//
new_upload_task_id_200_response_t* ClientAPI_newUploadTaskID(apiClient_t *apiClient, char *X_Token, schema_new_upload_task_req_t *req);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**X_Token** | **char \*** | 31a165ba1be6dec616b1f8f3207b4273 | 
**req** | **[schema_new_upload_task_req_t](schema_new_upload_task_req.md) \*** | JSON数据 | 

### Return type

[new_upload_task_id_200_response_t](new_upload_task_id_200_response.md) *


### Authorization

No authorization required

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: */*

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **ClientAPI_signIn**
```c
// 登陆帐号
//
sign_in_200_response_t* ClientAPI_signIn(apiClient_t *apiClient, request_auth_t *data);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**data** | **[request_auth_t](request_auth.md) \*** | auth json数据 | 

### Return type

[sign_in_200_response_t](sign_in_200_response.md) *


### Authorization

No authorization required

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: */*

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **ClientAPI_uploadTaskFile**
```c
// upload tgz file to upload task
//
// upload tgz file to upload task
//
api_upload_task_file_resp_t* ClientAPI_uploadTaskFile(apiClient_t *apiClient, char *X_Token, char *task_id, binary_t* file);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**X_Token** | **char \*** | 31a165ba1be6dec616b1f8f3207b4273 | 
**task_id** | **char \*** | task id | 
**file** | **binary_t*** | 文件路径 | 

### Return type

[api_upload_task_file_resp_t](api_upload_task_file_resp.md) *


### Authorization

No authorization required

### HTTP request headers

 - **Content-Type**: multipart/form-data
 - **Accept**: */*

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **ClientAPI_uploadTaskInfo**
```c
// get upload task status
//
// get upload task status
//
upload_task_info_200_response_t* ClientAPI_uploadTaskInfo(apiClient_t *apiClient, char *X_Token, char *task_id);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**X_Token** | **char \*** | 31a165ba1be6dec616b1f8f3207b4273 | 
**task_id** | **char \*** | task id | 

### Return type

[upload_task_info_200_response_t](upload_task_info_200_response.md) *


### Authorization

No authorization required

### HTTP request headers

 - **Content-Type**: Not defined
 - **Accept**: */*

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)

# **ClientAPI_uploadTaskLayerFile**
```c
// upload layer file to upload task
//
api_upload_task_layer_file_resp_t* ClientAPI_uploadTaskLayerFile(apiClient_t *apiClient, char *X_Token, char *task_id, binary_t* file);
```

### Parameters
Name | Type | Description  | Notes
------------- | ------------- | ------------- | -------------
**apiClient** | **apiClient_t \*** | context containing the client configuration |
**X_Token** | **char \*** | 31a165ba1be6dec616b1f8f3207b4273 | 
**task_id** | **char \*** | task id | 
**file** | **binary_t*** | 文件路径 | 

### Return type

[api_upload_task_layer_file_resp_t](api_upload_task_layer_file_resp.md) *


### Authorization

No authorization required

### HTTP request headers

 - **Content-Type**: multipart/form-data
 - **Accept**: */*

[[Back to top]](#) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to Model list]](../README.md#documentation-for-models) [[Back to README]](../README.md)


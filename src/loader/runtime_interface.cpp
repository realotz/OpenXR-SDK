// Copyright (c) 2017-2021, The Khronos Group Inc.
// Copyright (c) 2017-2019 Valve Corporation
// Copyright (c) 2017-2019 LunarG, Inc.
//
// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// Initial Author: Mark Young <marky@lunarg.com>
//

#include "runtime_interface.hpp"

#include "manifest_file.hpp"
#include "loader_interfaces.h"
#include "loader_logger.hpp"
#include "loader_platform.hpp"
#include "xr_generated_dispatch_table.h"

#include <openxr/openxr.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef XR_KHR_LOADER_INIT_SUPPORT
namespace {
/*!
 * Stores a copy of the data passed to the xrInitializeLoaderKHR function in a singleton.
 */
class LoaderInitData {
   public:
    /*!
     * Singleton accessor.
     */
    static LoaderInitData& instance() {
        static LoaderInitData obj;
        return obj;
    }

#ifdef XR_USE_PLATFORM_ANDROID
    /*!
     * Type alias for the platform-specific structure type.
     */
    using StructType = XrLoaderInitInfoAndroidKHR;
#endif

    /*!
     * Get our copy of the data, casted to pass to the runtime's matching method.
     */
    const XrLoaderInitInfoBaseHeaderKHR* getParam() const { return reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&_data); }

    /*!
     * Get the data via its real structure type.
     */
    const StructType& getData() const { return _data; }

    /*!
     * Has this been correctly initialized?
     */
    bool initialized() const noexcept { return _initialized; }

    /*!
     * Initialize loader data - called by InitializeLoader() and thus ultimately by the loader's xrInitializeLoaderKHR
     * implementation. Each platform that needs this extension will provide an implementation of this.
     */
    XrResult initialize(const XrLoaderInitInfoBaseHeaderKHR* info);

   private:
    //! Private constructor, forces use of singleton accessor.
    LoaderInitData() = default;
    //! Platform-specific init data
    StructType _data = {};
    //! Flag for indicating whether _data is valid.
    bool _initialized = false;
};

#ifdef XR_USE_PLATFORM_ANDROID
// Check and copy the Android-specific init data.
XrResult LoaderInitData::initialize(const XrLoaderInitInfoBaseHeaderKHR* info) {
    if (info->type != XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    auto cast_info = reinterpret_cast<XrLoaderInitInfoAndroidKHR const*>(info);

    if (cast_info->applicationVM == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (cast_info->applicationContext == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    _data = *cast_info;
    _data.next = nullptr;
    _initialized = true;
    return XR_SUCCESS;
}
#endif  // XR_USE_PLATFORM_ANDROID
}  // namespace

XrResult InitializeLoader(const XrLoaderInitInfoBaseHeaderKHR* loaderInitInfo) {
    return LoaderInitData::instance().initialize(loaderInitInfo);
}

#endif  // XR_KHR_LOADER_INIT_SUPPORT

void RuntimeInterface::TryLoadingSingleRuntime(const std::string& openxr_command,
                                               std::unique_ptr<RuntimeManifestFile>& manifest_file, bool& any_loaded,
                                               XrResult& last_error) {
    LoaderPlatformLibraryHandle runtime_library = LoaderPlatformLibraryOpen(manifest_file->LibraryPath());
    if (nullptr == runtime_library) {
        if (!any_loaded) {
            last_error = XR_ERROR_INSTANCE_LOST;
        }
        std::string library_message = LoaderPlatformLibraryOpenError(manifest_file->LibraryPath());
        std::string warning_message = "RuntimeInterface::LoadRuntime skipping manifest file ";
        warning_message += manifest_file->Filename();
        warning_message += ", failed to load with message \"";
        warning_message += library_message;
        warning_message += "\"";
        LoaderLogger::LogErrorMessage(openxr_command, warning_message);
        return;
    }
#ifdef XR_KHR_LOADER_INIT_SUPPORT
    {
        if (!LoaderInitData::instance().initialized()) {
            LoaderLogger::LogErrorMessage(openxr_command, "RuntimeInterface::LoadRuntime skipping manifest file ");
        }
        // Initialize loader, where required.
        std::string function_name = manifest_file->GetFunctionName("xrInitializeLoaderKHR");
        auto initialize =
            reinterpret_cast<PFN_xrInitializeLoaderKHR>(LoaderPlatformLibraryGetProcAddr(runtime_library, function_name));
        if (initialize != nullptr) {
            XrResult res = initialize(LoaderInitData::instance().getParam());
            if (!XR_SUCCEEDED(res)) {
                LoaderLogger::LogErrorMessage(openxr_command, "RuntimeInterface::LoadRuntime skipping manifest file " +
                                                                  manifest_file->Filename() +
                                                                  ", forwarded call to xrInitializeLoaderKHR failed.");
                last_error = res;

                LoaderPlatformLibraryClose(runtime_library);
                return;
            }
        }
    }
#endif
    // Get and settle on an runtime interface version (using any provided name if required).
    std::string function_name = manifest_file->GetFunctionName("xrNegotiateLoaderRuntimeInterface");
    auto negotiate =
        reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(LoaderPlatformLibraryGetProcAddr(runtime_library, function_name));

    // Loader info for negotiation
    XrNegotiateLoaderInfo loader_info = {};
    loader_info.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
    loader_info.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
    loader_info.structSize = sizeof(XrNegotiateLoaderInfo);
    loader_info.minInterfaceVersion = 1;
    loader_info.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    loader_info.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
    loader_info.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff);  // Maximum allowed version for this major version.

    // Set up the runtime return structure
    XrNegotiateRuntimeRequest runtime_info = {};
    runtime_info.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
    runtime_info.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
    runtime_info.structSize = sizeof(XrNegotiateRuntimeRequest);

    // Skip calling the negotiate function and fail if the function pointer
    // could not get loaded
    XrResult res = XR_ERROR_RUNTIME_FAILURE;
    if (nullptr != negotiate) {
        res = negotiate(&loader_info, &runtime_info);
    }
    // If we supposedly succeeded, but got a nullptr for GetInstanceProcAddr
    // then something still went wrong, so return with an error.
    if (XR_SUCCEEDED(res)) {
        uint32_t runtime_major = XR_VERSION_MAJOR(runtime_info.runtimeApiVersion);
        uint32_t runtime_minor = XR_VERSION_MINOR(runtime_info.runtimeApiVersion);
        uint32_t loader_major = XR_VERSION_MAJOR(XR_CURRENT_API_VERSION);
        if (nullptr == runtime_info.getInstanceProcAddr) {
            std::string error_message = "RuntimeInterface::LoadRuntime skipping manifest file ";
            error_message += manifest_file->Filename();
            error_message += ", negotiation succeeded but returned NULL getInstanceProcAddr";
            LoaderLogger::LogErrorMessage(openxr_command, error_message);
            res = XR_ERROR_FILE_CONTENTS_INVALID;
        } else if (0 >= runtime_info.runtimeInterfaceVersion ||
                   XR_CURRENT_LOADER_RUNTIME_VERSION < runtime_info.runtimeInterfaceVersion) {
            std::string error_message = "RuntimeInterface::LoadRuntime skipping manifest file ";
            error_message += manifest_file->Filename();
            error_message += ", negotiation succeeded but returned invalid interface version";
            LoaderLogger::LogErrorMessage(openxr_command, error_message);
            res = XR_ERROR_FILE_CONTENTS_INVALID;
        } else if (runtime_major != loader_major || (runtime_major == 0 && runtime_minor == 0)) {
            std::string error_message = "RuntimeInterface::LoadRuntime skipping manifest file ";
            error_message += manifest_file->Filename();
            error_message += ", OpenXR version returned not compatible with this loader";
            LoaderLogger::LogErrorMessage(openxr_command, error_message);
            res = XR_ERROR_FILE_CONTENTS_INVALID;
        }
    }
    if (XR_FAILED(res)) {
        if (!any_loaded) {
            last_error = res;
        }
        std::string warning_message = "RuntimeInterface::LoadRuntime skipping manifest file ";
        warning_message += manifest_file->Filename();
        warning_message += ", negotiation failed with error ";
        warning_message += std::to_string(res);
        LoaderLogger::LogErrorMessage(openxr_command, warning_message);
        LoaderPlatformLibraryClose(runtime_library);
        return;
    }

    std::string info_message = "RuntimeInterface::LoadRuntime succeeded loading runtime defined in manifest file ";
    info_message += manifest_file->Filename();
    info_message += " using interface version ";
    info_message += std::to_string(runtime_info.runtimeInterfaceVersion);
    info_message += " and OpenXR API version ";
    info_message += std::to_string(XR_VERSION_MAJOR(runtime_info.runtimeApiVersion));
    info_message += ".";
    info_message += std::to_string(XR_VERSION_MINOR(runtime_info.runtimeApiVersion));
    LoaderLogger::LogInfoMessage(openxr_command, info_message);

    // Use this runtime
    GetInstance().reset(new RuntimeInterface(runtime_library, runtime_info.getInstanceProcAddr));

    // Grab the list of extensions this runtime supports for easy filtering after the
    // xrCreateInstance call
    std::vector<std::string> supported_extensions;
    std::vector<XrExtensionProperties> extension_properties;
    GetInstance()->GetInstanceExtensionProperties(extension_properties);
    supported_extensions.reserve(extension_properties.size());
    for (XrExtensionProperties ext_prop : extension_properties) {
        supported_extensions.emplace_back(ext_prop.extensionName);
    }
    GetInstance()->SetSupportedExtensions(supported_extensions);

    // If we load one, clear all errors.
    any_loaded = true;
    last_error = XR_SUCCESS;
}

XrResult RuntimeInterface::LoadRuntime(const std::string& openxr_command) {
    // If something's already loaded, we're done here.
    if (GetInstance() != nullptr) {
        return XR_SUCCESS;
    }
#ifdef XR_KHR_LOADER_INIT_SUPPORT

    if (!LoaderInitData::instance().initialized()) {
        LoaderLogger::LogErrorMessage(
            openxr_command, "RuntimeInterface::LoadRuntime cannot run because xrInitializeLoaderKHR was not successfully called.");
        return XR_ERROR_INITIALIZATION_FAILED;
    }
#endif  // XR_KHR_LOADER_INIT_SUPPORT

    std::vector<std::unique_ptr<RuntimeManifestFile>> runtime_manifest_files = {};
    bool any_loaded = false;

    // Find the available runtimes which we may need to report information for.
    XrResult last_error = RuntimeManifestFile::FindManifestFiles(MANIFEST_TYPE_RUNTIME, runtime_manifest_files);
    if (XR_FAILED(last_error)) {
        LoaderLogger::LogErrorMessage(openxr_command, "RuntimeInterface::LoadRuntimes - unknown error");
        last_error = XR_ERROR_FILE_ACCESS_ERROR;
    } else {
        for (std::unique_ptr<RuntimeManifestFile>& manifest_file : runtime_manifest_files) {
            RuntimeInterface::TryLoadingSingleRuntime(openxr_command, manifest_file, any_loaded, last_error);
        }
    }

    // Always clear the manifest file list.  Either we use them or we don't.
    runtime_manifest_files.clear();

    // We found no valid runtimes, throw the initialization failed message
    if (!any_loaded) {
        LoaderLogger::LogErrorMessage(openxr_command, "RuntimeInterface::LoadRuntimes - failed to find a valid runtime");
        last_error = XR_ERROR_INSTANCE_LOST;
    }

    return last_error;
}

void RuntimeInterface::UnloadRuntime(const std::string& openxr_command) {
    if (GetInstance()) {
        LoaderLogger::LogInfoMessage(openxr_command, "RuntimeInterface::UnloadRuntime - Unloading RuntimeInterface");
        GetInstance().reset();
    }
}

XrResult RuntimeInterface::GetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    return GetInstance()->_get_instance_proc_addr(instance, name, function);
}

const XrGeneratedDispatchTable* RuntimeInterface::GetDispatchTable(XrInstance instance) {
    XrGeneratedDispatchTable* table = nullptr;
    std::lock_guard<std::mutex> mlock(GetInstance()->_dispatch_table_mutex);
    auto it = GetInstance()->_dispatch_table_map.find(instance);
    if (it != GetInstance()->_dispatch_table_map.end()) {
        table = it->second.get();
    }
    return table;
}

const XrGeneratedDispatchTable* RuntimeInterface::GetDebugUtilsMessengerDispatchTable(XrDebugUtilsMessengerEXT messenger) {
    XrInstance runtime_instance = XR_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> mlock(GetInstance()->_messenger_to_instance_mutex);
        auto it = GetInstance()->_messenger_to_instance_map.find(messenger);
        if (it != GetInstance()->_messenger_to_instance_map.end()) {
            runtime_instance = it->second;
        }
    }
    return GetDispatchTable(runtime_instance);
}

RuntimeInterface::RuntimeInterface(LoaderPlatformLibraryHandle runtime_library, PFN_xrGetInstanceProcAddr get_instance_proc_addr)
    : _runtime_library(runtime_library), _get_instance_proc_addr(get_instance_proc_addr) {}

RuntimeInterface::~RuntimeInterface() {
    std::string info_message = "RuntimeInterface being destroyed.";
    LoaderLogger::LogInfoMessage("", info_message);
    {
        std::lock_guard<std::mutex> mlock(_dispatch_table_mutex);
        _dispatch_table_map.clear();
    }
    LoaderPlatformLibraryClose(_runtime_library);
}

void RuntimeInterface::GetInstanceExtensionProperties(std::vector<XrExtensionProperties>& extension_properties) {
    std::vector<XrExtensionProperties> runtime_extension_properties;
    PFN_xrEnumerateInstanceExtensionProperties rt_xrEnumerateInstanceExtensionProperties;
    _get_instance_proc_addr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
                            reinterpret_cast<PFN_xrVoidFunction*>(&rt_xrEnumerateInstanceExtensionProperties));
    uint32_t count = 0;
    uint32_t count_output = 0;
    // Get the count from the runtime
    rt_xrEnumerateInstanceExtensionProperties(nullptr, count, &count_output, nullptr);
    if (count_output > 0) {
        runtime_extension_properties.resize(count_output);
        count = count_output;
        for (XrExtensionProperties& ext_prop : runtime_extension_properties) {
            ext_prop.type = XR_TYPE_EXTENSION_PROPERTIES;
            ext_prop.next = nullptr;
        }
        rt_xrEnumerateInstanceExtensionProperties(nullptr, count, &count_output, runtime_extension_properties.data());
    }
    size_t ext_count = runtime_extension_properties.size();
    size_t props_count = extension_properties.size();
    for (size_t ext = 0; ext < ext_count; ++ext) {
        bool found = false;
        for (size_t prop = 0; prop < props_count; ++prop) {
            // If we find it, then make sure the spec version matches that of the runtime instead of the
            // layer.
            if (strcmp(extension_properties[prop].extensionName, runtime_extension_properties[ext].extensionName) == 0) {
                // Make sure the spec version used is the runtime's
                extension_properties[prop].extensionVersion = runtime_extension_properties[ext].extensionVersion;
                found = true;
                break;
            }
        }
        if (!found) {
            extension_properties.push_back(runtime_extension_properties[ext]);
        }
    }
}

XrResult RuntimeInterface::CreateInstance(const XrInstanceCreateInfo* info, XrInstance* instance) {
    XrResult res = XR_SUCCESS;
    bool create_succeeded = false;
    PFN_xrCreateInstance rt_xrCreateInstance;
    _get_instance_proc_addr(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction*>(&rt_xrCreateInstance));
    res = rt_xrCreateInstance(info, instance);
    if (XR_SUCCEEDED(res)) {
        create_succeeded = true;
        std::unique_ptr<XrGeneratedDispatchTable> dispatch_table(new XrGeneratedDispatchTable());
        GeneratedXrPopulateDispatchTable(dispatch_table.get(), *instance, _get_instance_proc_addr);
        std::lock_guard<std::mutex> mlock(_dispatch_table_mutex);
        _dispatch_table_map[*instance] = std::move(dispatch_table);
    }

    // If the failure occurred during the populate, clean up the instance we had picked up from the runtime
    if (XR_FAILED(res) && create_succeeded) {
        PFN_xrDestroyInstance rt_xrDestroyInstance;
        _get_instance_proc_addr(*instance, "xrDestroyInstance", reinterpret_cast<PFN_xrVoidFunction*>(&rt_xrDestroyInstance));
        rt_xrDestroyInstance(*instance);
        *instance = XR_NULL_HANDLE;
    }

    return res;
}

XrResult RuntimeInterface::DestroyInstance(XrInstance instance) {
    if (XR_NULL_HANDLE != instance) {
        // Destroy the dispatch table for this instance first
        {
            std::lock_guard<std::mutex> mlock(_dispatch_table_mutex);
            auto map_iter = _dispatch_table_map.find(instance);
            if (map_iter != _dispatch_table_map.end()) {
                _dispatch_table_map.erase(map_iter);
            }
        }
        // Now delete the instance
        PFN_xrDestroyInstance rt_xrDestroyInstance;
        _get_instance_proc_addr(instance, "xrDestroyInstance", reinterpret_cast<PFN_xrVoidFunction*>(&rt_xrDestroyInstance));
        rt_xrDestroyInstance(instance);
    }
    return XR_SUCCESS;
}

bool RuntimeInterface::TrackDebugMessenger(XrInstance instance, XrDebugUtilsMessengerEXT messenger) {
    std::lock_guard<std::mutex> mlock(_messenger_to_instance_mutex);
    _messenger_to_instance_map[messenger] = instance;
    return true;
}

void RuntimeInterface::ForgetDebugMessenger(XrDebugUtilsMessengerEXT messenger) {
    if (XR_NULL_HANDLE != messenger) {
        std::lock_guard<std::mutex> mlock(_messenger_to_instance_mutex);
        _messenger_to_instance_map.erase(messenger);
    }
}

void RuntimeInterface::SetSupportedExtensions(std::vector<std::string>& supported_extensions) {
    _supported_extensions = supported_extensions;
}

bool RuntimeInterface::SupportsExtension(const std::string& extension_name) {
    bool found_prop = false;
    for (const std::string& supported_extension : _supported_extensions) {
        if (supported_extension == extension_name) {
            found_prop = true;
            break;
        }
    }
    return found_prop;
}

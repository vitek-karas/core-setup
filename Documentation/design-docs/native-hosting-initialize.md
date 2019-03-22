# Native hosting with explicit initialize

## Goals
* All hosts should be able to use the new API (whether they will is a separate question as the old API has to be kept for backward compat reasons)
* Hide implementation details as much as possible
  * Make the API generally easier to use/understand
  * Give the implementation more freedom
  * Allow future improvements without breaking the API
  * Consider explicitly documenting types of behaviors which nobody should take dependency on (specifically failure scenarios)
* Extensible

## New scenarios
The API should allow these scenarios:
* Runtime properties
  * Specify additional runtime properties from the native host
  * Implement conflict resolution for runtime properties
  * Inspect calculated runtime properties (the ones calculated by `hostfxr`/`hostpolicy`)

It should be possible to ship with only some of these supported, then enable more scenarios later on.

## New `hostfxr` APIs

Terminology:
* "native host" - the code which uses the proposed APIs. Can be any non .NET Core application (.NET Core applications have easier ways to perform these scenarios).
* "host context" - state which `hostfxr` creates and maintains and represents a logical operation on the hosting components.
* "hosting components" - shorthand for .NET Core hosting components. Typically refers to `hostfxr` and `hostpolicy`. Sometimes also referred to simply as "host".

All the proposed APIs will be exports of the `hostfxr` library and will use the same calling convention as existing `hostfxr` exports. The names shown are the exact export names (no mangling).

### Initialize host context

All the "initialize" functions will
* Process the `.runtimeconfig.json`
* Resolve framework references and find actual frameworks
* Find the root framework (`Microsoft.NETCore.App`) and load the `hostpolicy` from it
* The `hostpolicy` will then process all relevant `.deps.json` files and produce the list of assemblies, native search paths and other artifacts needed to initialize th

The functions will NOT load the CoreCLR runtime. They just prepare everything to the point where it can be loaded.

The functions return a handle to a new host context:
* The handle must be closed via `hostfxr_shutdown`.
* The handle is not thread safe - the consumer should only call functions on it from one thread at a time.

The `hostfxr` will also track active runtime in the process. Due to limitations (and to simplify implementation) this tracking will actually not look at the actual `coreclr` module (or try to communicate with the runtime in any way). Instead `hostfxr` itself will track the host context initialization. The first host context initialization in the process will represent the "loaded runtime". It is only possible to have one "loaded runtime" in the process. Any subsequent host context initialization will just "attach" to the "loaded runtime" instead of creating a new one.

*Note: It is technically possible to initialize host context, and never call any "run" method on it, which would mean the runtime would not get loaded. Trying to initialize a second host context while the first one didn't start the runtime yet will for now be considered illegal. It's a pending issue to solve this problem, as COM activation will probably need to be able to safely initialize/run from multiple threads at a time.*

``` C
#define hostfxr_handle = void *;

struct hostfxr_initialize_parameters
{
    size_t size;
    const char_t * host_path;
    const char_t * dotnet_root;
};
```

The `hostfxr_initialize_parameters` structure stores parameters which are common to all forms of initialization.
* `size` - the size of the structure. This is used for versioning. Should be set to `sizeof(hostfxr_initialize_parameters)`.
* `host_path` - path to the native host (typically the `.exe`). This value is not used for anything by the hosting components. It's just passed to the CoreCLR as the path to the executable. It can point to a file which is not executable itself, if such file doesn't exist (for example in COM activation scenarios this points to the `comhost.dll`).
* `dotnet_root` - path to the root of the .NET Core installation in use. This typically points to the install location from which the `hostfxr` has been loaded. For example on Windows this would typically point to `C:\Program Files\dotnet`. The path is used to search for shared frameworks and potentially SDKs.


``` C
int hostfxr_initialize_for_app(
    int argc,
    const char_t * argv[],
    const char_t * app_path,
    const hostfxr_initialize_parameters * parameters,
    hostfxr_handle * host_context_handle
);
```

Initializes the hosting components for running a managed application.
When used to execute an app, the `app_path` (or CLI equivalent) will be used to locate the `.runtimeconfig.json` and the `.deps.json` which will be used to load the application and its dependent frameworks.
* `argc` and `argv` - the command line - optional, if `argc` is `0` then `argv` is ignored.
* `app_path` - path to the application (the managed `.dll`) to run. This can be `nullptr` if the app is specified in the command line arguments.
* `parameters` - additional parameters - see `hostfxr_initialize_parameters` for details. (Could be made optional potentially)
* `host_context_handle` - output parameter. On success receives an opaque value which identifies the initialized host context. The handle should be closed by calling `hostfxr_shutdown`.

This function can only be called once per-process. It's not supported to run multiple apps in one process (even sequentially).

This function will fail if there already is a CoreCLR running in the process.

*Note: This is effectively a replacement for `hostfxr_main_startupinfo` and `hostfxr_main`. Currently it is not be a goal to fully replace these APIs because they also support SDK commands which are special in lot of ways and don't fit well with the rest of the native hosting. There's no scenario right now which would require the ability to issue SDK commands from a native host. That said nothing in this proposal should block enabling even SDK commands through these APIs.*


``` C
int hostfxr_initialize_for_runtime_config(
    const char_t * runtime_config_path,
    const hostfxr_initialize_parameters * parameters,
    hostfxr_handle * host_context_handle
);
```

This function would load the specified `.runtimeconfig.json`, resolve all frameworks, resolve all the assets from those frameworks abd then prepare runtime initialization where the TPA contains only frameworks. Note that this case does NOT consume any `.deps.json` from the app/component (only processes the framework's `.deps.json`). This entry point is intended for `comhost`/`ijwhost`/`nethost` and similar scenarios.
* `runtime_config_path` - path to the `.runtimeconfig.json` file to process. Unlike with the `hostfxr_initialize_for_app`, there is no `.deps.json` from the app/component which will be processed.
* `parameters` - additional parameters - see `hostfxr_initialize_parameters` for details. (Could be made optional potentially)
* `host_context_handle` - output parameter. On success receives an opaque value which identifies the initialized host context. The handle should be closed by calling `hostfxr_shutdown`.

This function can be called multiple times in a process.
* If it's called when no runtime is present, it will run through the steps to "initialize" the runtime (resolving frameworks and so on).
* If it's called when there already is CoreCLR in the process (loaded through the `hostfxr`, direct usage of `coreclr` is not supported), then the function determines if the specified runtime configuration is compatible with the existing runtime and frameworks. If it is, it returns a valid handle, otherwise it fails.

It needs to be possible to call this function simultaneously from multiple threads at the same time.
It also needs to be possible to call this function while there is an active host context created by `hostfxr_initialize_for_app` and running inside the `hostfxr_run_app`.

The function returns specific return code for the first initialized host context, and a different one for any subsequent one. Both return codes are considered "success". It's important to communicate which host context is the "first" as that's the only one which will allow setting runtime properties.


### Inspect and modify host context

#### Runtime properties
``` C
int hostfxr_get_runtime_property(
    const hostfxr_handle host_context_handle,
    const char_t * name,
    char_t * value_buffer,
    size_t value_buffer_size,
    size_t * value_buffer_used);

int hostfxr_set_runtime_property(
    const hostfxr_handle host_context_handle,
    const char_t * name,
    const char_t * value);
```

These functions allow the native host to inspect and modify runtime properties. After initialization the properties should contain everything which the hosting components calculate. This gives the native host the opportunity to inspect, modify and resolve any potential conflicts.
* `host_context_handle` - the initialized host context.
* `name` - the name of the runtime property to get/set. Must not be `nullptr`.
* `value` - the value of the property to set. If the property already has a value in the host context, this function will overwrite it. When set to `nullptr` and if the property already has a value then the property is "unset" - removed from the runtime property collection.
* `value_buffer` - buffer to receive the value of the property
* `value_buffer_size` - the size of the `value_buffer` in `char_t` units.
* `value_buffer_used` - when successful contains number of `char_t` units used in the buffer (including the null terminator). If the specified `value_buffer_size` was too small, the function returns `HostApiBufferTooSmall` and sets the `value_buffer_used` to the minimum required size.

Trying to get a property which doesn't exist is an error and `hostfxr_get_runtime_property` will return an appropriate error code.

Setting properties is only supported on the first host context in the process. This is really a limitation of the runtime for which the runtime properties are immutable. Once the first host context is initialized and starts a runtime there's no way to change these properties. For now we will not consider the scenario where the host context is initialize but the runtime hasn't started yet, mainly for simplicity of implementation and lack of requirements.

We're proposing a fix in `hostpolicy` which will make sure that there are no duplicates possible after initialization (see dotnet/core-setup#5529). With that `hostfxr_get_runtime_property` will work always (as there can only be one value).

*TODO: Can we support getting properties when there is preexisting runtime?*

*TODO: Should there be a way to enumerate all the properties?*


### Start the runtime

#### Running an application
``` C
int hostfxr_run_app(const hostfxr_handle host_context_handle);
```
Runs the application specified by the `hostfxr_initialize_for_app`. It is illegal to try to use this function when the host context was initialized through any other way.
* `host_context_handle` - handle to the initialized host context.

The function will return only once the managed application exits.

`hostfxr_run_app` cannot be used in combination with any other "run" function. It can also only be called once.


#### Getting a delegate for runtime functionality
``` C
int hostfxr_get_runtime_delegate(const hostfxr_handle host_context_handle, hostfxr_delegate_type type, void ** delegate);
```
Starts the runtime and returns a function pointer to specified functionality of the runtime.
* `host_context_handle` - handle to the initialized host context.
* `type` - the type of runtime functionality requested
  * `load_assembly_and_get_function_pointer` - entry point which loads an assembly (with dependencies) and returns function pointer for a specified static method.
  * `com_activation` - COM activation entry-point - see [COM activation](https://github.com/dotnet/core-setup/blob/master/Documentation/design-docs/COM-activation.md) for more details.
  * `load_in_memory_assembly` - IJW entry-point - see [IJW activation](https://github.com/dotnet/core-setup/blob/master/Documentation/design-docs/IJW-activation.md) for more details.
* `delegate` - when successful, the native function pointer to the requested runtime functionality.

Initially the function will only work if `hostfxr_initialize_for_runtime_config` was used to initialize the host context. Later on this could be relaxed to allow being used in combination with `hostfxr_initialize_for_app`.  

Initially there might be a limitation of calling this function only once on a given host context to simplify the implementation. Currently we don't have a scenario where it would be absolutely required to support multiple calls.


### Cleanup
``` C
int hostfxr_shutdown(const hostfxr_handle host_context_handle);
```
Closes a host context.
* `host_context_handle` - handle to the initialized host context to close.


## Samples
All samples assume that the native host has found the `hostfxr`, loaded it and got the exports. *TODO: Reference to `nethost` spec.*
Samples in general ignore error handling.

### Running app with additional runtime properties
``` C++
hostfxr_initialize_parameters params;
params.size = sizeof(params);
params.host_path = _hostpath_;
params.dotnet_root = _dotnetroot_;

hostfxr_handle host_context_handle;
hostfxr_initialize_for_app(
    _argc_,
    _argv_,
    _apppath_,
    &params,
    &host_context_handle);

size_t buffer_used = 0;
if (hostfxr_get_runtime_property(host_context_handle, "TEST_PROPERTY", nullptr, 0, &buffer_used) == HostApiMissingProperty)
{
    hostfxr_set_runtime_property(host_context_handle, "TEST_PROPERTY", "TRUE");
}

hostfxr_run_app(host_context_handle);

hostfxr_shutdown(host_context_handle);
```

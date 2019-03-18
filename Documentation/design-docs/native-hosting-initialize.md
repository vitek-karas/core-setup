# Native hosting with explicit initialize

### Goals
* All hosts should be able to use the new API (whether they will is a separate question as the old API has to be kept for backward compat reasons)
* Try to hide as much implementation details as possible
  * Makes the API generally easier to use/understand
  * Gives the implementation more freedom
  * Should allow future improvements without breaking the API
  * Consider explicitly documenting types of behaviors which nobody should take dependency on (specifically failure scenarios)
* Extensible

## New scenarios
The API should allow these, doesn't have to support them all in the beginning. It should be possible to ship with only some of these supported.
* Runtime properties
  * Ability to specify additional runtime properties from the native host
  * Ability for the host to implement conflict resolution for runtime properties
  * Ability for the host to inspect calculated runtime properties (the ones calculated by `hostfxr`/`hostpolicy`)


## New `hostfxr` APIs

### Initialize the host
``` C++
using hostfxr_handle = void *;

struct hostfxr_initialize_parameters
{
    size_t size;
    const char_t * host_path;   // path to the .exe
    const char_t * dotnet_root  // resolved installation root (where fxr was found)
};
```

The `hostfxr_initialize_parameters` structure stores initialize parameters which are common to all forms of initialization.
* `size` - the size of the structure. This is used for versioning. Should be set to `sizeof(hostfxr_initialize_parameters)`.
* `host_path` - path to the native host (typically the `.exe`). This value is not used for anything by the host, it's just passed to the CoreCLR as the path to the executable. It can point to a file which is not executable itself, if such file doesn't exist (for example in COM activation scenarios this points to the `comhost.dll`).
* `dotnet_root` - path to the root of the .NET Core installation in use. This typically points to the install location from which the `hostfxr` has been loaded. Most commonly this would be (on Windows) `C:\Program Files\dotnet`. The path is used to search for shared frameworks and potentially SDKs.


``` C++
int hostfxr_initialize_for_app(
    int argc,
    const char_t * argv[],
    const char_t * app_path  
    const hostfxr_initialize_parameters * parameters,
    hostfxr_handle * handle
);
```

Initializes the host for running a managed application. Also used for running SDK commands.
When used to execute an app, the `app_path` (or CLI equivalent) will be used to locate the `.runtimeconfig.json` and the `.deps.json` which will be used to load the application and its dependent frameworks.
* `argc` and `argv` - the command line - entirely optional
* `app_path` - path to the application (the managed `.dll`) to run. This can be `nullptr` if the app is specified in the command line arguments or if it's an SDK command.
* `parameters` - additional parameters - see `hostfxr_initialize_parameters` for details. (Could be made optional potentially)
* `handle` - output parameter. On success receives an opaque value which identifies the initialized host. The handle should be closed by calling `hostfxr_shutdown`.

This function can only be called once per-process. It's not supported to run multiple apps in one process (even sequentially).

*Note: This is effectively a replacement for the `hostfxr_main_startupinfo` and `hostfxr_main` - both of those APIs should be possible to implement using the `hostfxr_initialize_for_app` and the `hostfxr_run_app`.*


``` C++
int hostfxr_initialize_for_runtime_config(
    const char_t * runtime_config_path
    const hostfxr_initialize_parameters * parameters,
    hostfxr_handle * handle
);
```

This function would load the specified `.runtimeconfig.json`, resolve all frameworks and resolve all the assets from those frameworks. Then prepare runtime initialization where the TPA contains only frameworks. Note that this case does NOT consume any `.deps.json` from the app/component (only processes the framework's `.deps.json`). This entry point is intended for `comhost`/`ijwhost`/`nethost` and similar scenarios.
* `runtime_config_path` - path to the `.runtimeconfig.json` file to process. Unlike with the `hostfxr_initialize_for_app`, there is no `.deps.json` from the app/component which will be processed.
* `parameters` - additional parameters - see `hostfxr_initialize_parameters` for details. (Could be made optional potentially)
* `handle` - output parameter. On success receives an opaque value which identifies the initialized host. The handle should be closed by calling `hostfxr_shutdown`.

This function can be called multiple times in a process.
* If it's called when no runtime is present, it will run through the steps to "initialize" the runtime (so resolving frameworks and so on).
* If it's called when there already is CoreCLR in the process (loaded through the `hostfxr`, direct usage of `coreclr` is not supported), then the function determines if the specified runtime configuration is compatible with the existing runtime and frameworks. If it is, it returns a valid handle, otherwise it fails.

*TODO: What is the indication that there is no runtime in the process yet?*


All the "initialize" functions will (unless it's one of the SDK scenarios):
* Process the `.runtimeconfig.json`
* Resolve framework references and find actual frameworks
* Find the root framework (`Microsoft.NETCore.App`) and load the `hostpolicy` from it
* The `hostpolicy` will then process all relevant `.deps.json` files and produce the TPA, native search paths and other artifacts.

The functions will NOT load the CoreCLR runtime, they just prepare everything to the point where it can be loaded.


### Initialized host inspection and modification

#### Runtime properties
``` C++
using hostfxr_string_result_fn = void(*) (const char_t * value);

int hostfxr_get_runtime_property(const hostfxr_handle handle, const char_t * name, hostfxr_string_result_fn value_result);
int hostfxr_set_runtime_property(const hostfxr_handle handle, const char_t * name, const char_t * value);
```

These functions allow the native host to inspect and modify runtime properties. After initialization the properties should contain everything which the hosting layer calculates. This gives the host the opportunity to inspect, modify and resolve any potential conflicts.
* `handle` - the initialized host handle.
* `name` - the name of the runtime property to get/set.
* `value_result` - callback which is called with the value of the property.  
*TODO: Is this the right API - maybe it should use the "buffer, buffer size" approach where the caller supplies allocated memory instead*
* `value` - the value of the property to set.  
*TODO: Can we use `nullptr` to "unset" the property?*

Setting properties is only supported when the host was initialized without any runtime in the process.

*TODO: Should there be a way to enumerate all the properties?*

*Maybe for the first release we don't support `hostfxr_get_runtime_property` as that requires the initialization to run all the way through to `hostpolicy` to calculate all properties correctly.*


### Start the runtime

#### Running an application
``` C++
int hostfxr_run_app(const hostfxr_handle handle);
```
Runs the application specified by the `hostfxr_initialize_for_app`. It is illegal to try to use this function when the host was initialized through any other way.
* `handle` - handle to the initialized host.

The function will return only once the managed application exits.


#### Getting a delegate for runtime functionality
``` C++
int hostfxr_get_runtime_delegate(const hostfxr_handle handle, hostfxr_delegate_type type, void ** delegate);
```
Starts the runtime and returns a function pointer to specified functionality of the runtime.
* `handle` - handle to the initialized host.
* `type` - the type of runtime functionality requested *TODO - exact names*
  * `load_assembly` - entry point which loads an assembly (with dependencies) and returns function pointer for a specified static method.
  * `com` - COM activation entry-point
  * `ijw` - IJW entry-point
* `delegate` - when successful, the native function pointer to the requested runtime functionality.

*TODO: Is it OK to allow calling this even when initialized via `hostfxr_initialize_for_app`? It would mean the app can't be executed via `hostfxr_run_app` anymore.*

*TODO: Is it OK to call this more than once on an initialized host?*


### Cleanup
``` C++
int hostfxr_shutdown(const hostfxr_handle handle);
```
Closes the initialized host.
* `handle` - handle to the initialized host to close.


Notes:
* `hostfxr_handle` is currently not absolutely necessary mainly because it won't be supported to call initialize multiple times. The limitation comes from usage of static variables in the implementation (and lack of scenarios). In the future this limitation may go away. Once it does the handle will be necessary for property functionality.
* Similarly the `hostfxr_shutdown` is effectively a no-op for now.


## Samples
All samples assume that the native host has found the `hostfxr`, loaded it and got the exports. TODO: Reference to `nethost` spec.
Samples in general ignore error handling.

### Running app with additional runtime properties
``` C++
hostfxr_initialize_parameters params;
params.size = sizeof(params);
params.host_path = _hostpath_;
params.dotnet_root = _dotnetroot_;

hostfxr_handle handle;
hostfxr_initialize_for_app(
    _argc_,
    _argv_,
    _apppath_,
    &params,
    &handle);

hostfxr_set_runtime_property(handle, "TEST_PROPERTY", "TRUE");

hostfxr_run_app(handle);

hostfxr_shutdown(handle);
```

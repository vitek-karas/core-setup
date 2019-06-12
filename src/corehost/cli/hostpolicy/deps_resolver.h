// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#ifndef DEPS_RESOLVER_H
#define DEPS_RESOLVER_H

#include <vector>

#include "pal.h"
#include "args.h"
#include "trace.h"
#include "fx_definition.h"
#include "deps_format.h"
#include "deps_entry.h"
#include "runtime_config.h"

// Probe paths to be resolved for ordering
struct probe_paths_t
{
    pal::string_t tpa;
    pal::string_t native;
    pal::string_t resources;
    pal::string_t coreclr;
    pal::string_t clrjit;
};

struct deps_resolved_asset_t
{
    deps_resolved_asset_t(const deps_asset_t& asset, const pal::string_t& resolved_path, int fx_level)
        : asset(asset)
        , resolved_path(resolved_path) 
        , fx_level(fx_level) { }

    deps_asset_t asset;
    pal::string_t resolved_path;
    size_t fx_level;
};

typedef std::unordered_map<pal::string_t, deps_resolved_asset_t> name_to_resolved_asset_map_t;

class deps_resolver_t
{
public:
    deps_resolver_t(
        const arguments_t& args,
        const fx_definition_vector_t& fx_definitions,
        bool is_framework_dependent)
        : m_fx_definitions()
        , m_app_dir(args.app_root)
        , m_host_mode(args.host_mode)
        , m_managed_app(args.managed_application)
        , m_core_servicing(args.core_servicing)
        , m_is_framework_dependent(is_framework_dependent)
    {
        int lowest_framework = fx_definitions.size() - 1;
        const deps_json_t::rid_fallback_graph_t& root_framework_rid_fallback_graph = fx_definitions[lowest_framework]->get_deps().get_rid_fallback_graph();

        m_fx_definitions.resize(fx_definitions.size());
        for (int i = lowest_framework; i >= 0; --i)
        {
            if (i == 0)
            {
                fx_definitions[i]->set_deps_file(args.deps_path);
                trace::verbose(_X("Using %s deps file"), fx_definitions[i]->get_deps_file().c_str());
            }
            else
            {
                pal::string_t fx_deps_file = get_fx_deps(fx_definitions[i]->get_dir(), fx_definitions[i]->get_name());
                fx_definitions[i]->set_deps_file(fx_deps_file);
                trace::verbose(_X("Using Fx %s deps file"), fx_deps_file.c_str());
            }

            if (i == lowest_framework)
            {
                fx_definitions[i]->parse_deps();
            }
            else
            {
                // The rid graph is obtained from the root framework
                fx_definitions[i]->parse_deps(root_framework_rid_fallback_graph);
            }

            // We only take the const * for the fx_definition.
            // Lifetime should still be fine since the deps_resolver_t should only live on stack and thus should have a much shorter
            // lifetime than the frameworks which are initialized globally.
            m_fx_definitions[i] = fx_definitions[i].get();
        }

        resolve_additional_deps(args, root_framework_rid_fallback_graph);

        setup_additional_probes(args.probe_paths);
        setup_probe_config(args);
    }

    deps_resolver_t(
        const arguments_t& args,
        const fx_definition_const_vector_t& fx_definitions,
        bool is_framework_dependent)
        : m_fx_definitions(fx_definitions)
        , m_app_dir(args.app_root)
        , m_host_mode(args.host_mode)
        , m_managed_app(args.managed_application)
        , m_core_servicing(args.core_servicing)
        , m_is_framework_dependent(is_framework_dependent)
    {
        int lowest_framework = fx_definitions.size() - 1;
        const deps_json_t::rid_fallback_graph_t& root_framework_rid_fallback_graph = fx_definitions[lowest_framework]->get_deps().get_rid_fallback_graph();

        resolve_additional_deps(args, root_framework_rid_fallback_graph);

        setup_additional_probes(args.probe_paths);
        setup_probe_config(args);
    }

    bool valid(pal::string_t* errors)
    {
        for (size_t i = 0; i < m_fx_definitions.size(); ++i)
        {
            // Verify the deps file exists. The app deps file does not need to exist
            if (i != 0)
            {
                if (!m_fx_definitions[i]->get_deps().exists())
                {
                    errors->assign(_X("A fatal error was encountered, missing dependencies manifest at: ") + m_fx_definitions[i]->get_deps_file());
                    return false;
                }
            }

            if (!m_fx_definitions[i]->get_deps().is_valid())
            {
                errors->assign(_X("An error occurred while parsing: ") + m_fx_definitions[i]->get_deps_file());
                return false;
            }
        }

        for (const auto& additional_deps : m_additional_deps)
        {
            if (!additional_deps->is_valid())
            {
                errors->assign(_X("An error occurred while parsing: ") + additional_deps->get_deps_file());
                return false;
            }
        }

        errors->clear();
        return true;
    }

    void setup_shared_store_probes(
        const arguments_t& args);

    pal::string_t get_lookup_probe_directories();

    void setup_probe_config(
        const arguments_t& args);

    void setup_additional_probes(
        const std::vector<pal::string_t>& probe_paths);

    bool resolve_probe_paths(
        probe_paths_t* probe_paths,
        std::unordered_set<pal::string_t>* breadcrumb,
        bool ignore_missing_assemblies,
        size_t max_fx_level_to_include);

    void init_known_entry_path(
        const deps_entry_t& entry,
        const pal::string_t& path);

    void resolve_additional_deps(
        const arguments_t& args,
        const deps_json_t::rid_fallback_graph_t& rid_fallback_graph);

    const deps_json_t& get_deps() const
    {
        return get_app(m_fx_definitions).get_deps();
    }

    const pal::string_t& get_deps_file() const
    {
        return get_app(m_fx_definitions).get_deps_file();
    }

    void get_app_fx_definition_range(fx_definition_const_vector_t::const_iterator *begin, fx_definition_const_vector_t::const_iterator *end) const;

    const fx_definition_const_vector_t& get_fx_definitions() const
    {
        return m_fx_definitions;
    }

    const pal::string_t& get_coreclr_library_version() const
    {
        return m_coreclr_library_version;
    }

    bool is_framework_dependent() const
    {
        return m_is_framework_dependent;
    }

    const pal::string_t &get_app_dir() const
    {
        if (m_host_mode == host_mode_t::libhost)
        {
            static const pal::string_t s_empty;
            return s_empty;
        }

        return m_app_dir;
    }

private:

    static pal::string_t get_fx_deps(const pal::string_t& fx_dir, const pal::string_t& fx_name)
    {
        pal::string_t fx_deps = fx_dir;
        pal::string_t fx_deps_name = fx_name + _X(".deps.json");
        append_path(&fx_deps, fx_deps_name.c_str());
        return fx_deps;
    }

    // Resolve order for TPA lookup.
    bool resolve_tpa_list(
        pal::string_t* output,
        std::unordered_set<pal::string_t>* breadcrumb,
        bool ignore_missing_assemblies,
        size_t max_fx_level_to_include);

    // Resolve order for culture and native DLL lookup.
    bool resolve_probe_dirs(
        deps_entry_t::asset_types asset_type,
        pal::string_t* output,
        std::unordered_set<pal::string_t>* breadcrumb,
        size_t max_fx_level_to_include);

    // Populate assemblies from the directory.
    void get_dir_assemblies(
        const pal::string_t& dir,
        const pal::string_t& dir_name,
        size_t fx_level,
        name_to_resolved_asset_map_t* items);

    // Probe entry in probe configurations and deps dir.
    bool probe_deps_entry(
        const deps_entry_t& entry,
        const pal::string_t& deps_dir,
        int fx_level,
        pal::string_t* candidate);

    fx_definition_const_vector_t m_fx_definitions;

    pal::string_t m_app_dir;

    void add_tpa_asset(
        const deps_resolved_asset_t& asset,
        name_to_resolved_asset_map_t* items);

    // Mode in which the host is being run. This can dictate how dependencies should be discovered.
    const host_mode_t m_host_mode;

    // The managed application the dependencies are being resolved for.
    pal::string_t m_managed_app;

    // Servicing root, could be empty on platforms that don't support or when errors occur.
    pal::string_t m_core_servicing;

    // Special entry for coreclr path
    pal::string_t m_coreclr_path;

    // Special entry for coreclr library version
    pal::string_t m_coreclr_library_version;

    // Special entry for JIT path
    pal::string_t m_clrjit_path;

    // The filepaths for the app custom deps
    std::vector<pal::string_t> m_additional_deps_files;

    // Custom deps files for the app
    std::vector< std::unique_ptr<deps_json_t> > m_additional_deps;

    // Various probe configurations.
    std::vector<probe_config_t> m_probes;

    // Fallback probe dir
    std::vector<pal::string_t> m_additional_probes;

    // Is the deps file for an app using shared frameworks?
    bool m_is_framework_dependent;
};

#endif // DEPS_RESOLVER_H

// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/extensions/api/side_panel/side_panel_service.h"

#include <cstddef>
#include <memory>

#include "base/no_destructor.h"
#include "chrome/common/extensions/api/side_panel.h"
#include "chrome/common/extensions/api/side_panel/side_panel_info.h"
#include "components/sessions/core/session_id.h"
#include "extensions/browser/extension_prefs.h"
#include "extensions/browser/pref_types.h"
#include "extensions/common/extension_features.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

namespace extensions {

namespace {

// Key corresponding to whether the extension's side panel entry (if one exists)
// should be opened when its icon is clicked in the toolbar.
constexpr PrefMap kOpenSidePanelOnIconClickPref = {
    "open_side_panel_on_icon_click", PrefType::kBool,
    PrefScope::kExtensionSpecific};

api::side_panel::PanelOptions GetPanelOptionsFromManifest(
    const Extension& extension) {
  auto path = SidePanelInfo::GetDefaultPath(&extension);
  api::side_panel::PanelOptions options;
  if (!path.empty()) {
    options.path = std::string(path);
    options.enabled = true;
  }
  return options;
}

// TODO(crbug.com/1332599): Add a Clone() method for generated types.
api::side_panel::PanelOptions CloneOptions(
    const api::side_panel::PanelOptions& options) {
  auto clone = api::side_panel::PanelOptions::FromValueDeprecated(
      base::Value(options.ToValue()));
  return clone ? std::move(*clone) : api::side_panel::PanelOptions();
}

}  // namespace

SidePanelService::~SidePanelService() = default;

SidePanelService::SidePanelService(content::BrowserContext* context)
    : browser_context_(context) {
  extensions::ExtensionRegistry* extension_registry =
      extensions::ExtensionRegistry::Get(context);
  extension_registry_observation_.Observe(extension_registry);
}

bool SidePanelService::HasSidePanelActionForTab(const Extension& extension,
                                                TabId tab_id) {
  if (!OpenSidePanelOnIconClick(extension.id()) ||
      !base::FeatureList::IsEnabled(
          extensions_features::kExtensionSidePanelIntegration)) {
    return false;
  }

  api::side_panel::PanelOptions options = GetOptions(extension, tab_id);
  return options.enabled.has_value() && *options.enabled &&
         options.path.has_value();
}

api::side_panel::PanelOptions SidePanelService::GetOptions(
    const Extension& extension,
    absl::optional<TabId> id) {
  auto extension_panel_options = panels_.find(extension.id());

  // Get default path from manifest if nothing was stored in this service for
  // the calling extension.
  if (extension_panel_options == panels_.end()) {
    return GetPanelOptionsFromManifest(extension);
  }

  TabId default_tab_id = SessionID::InvalidValue().id();
  TabId tab_id = id.has_value() ? id.value() : default_tab_id;
  TabPanelOptions& tab_panel_options = extension_panel_options->second;

  // The specific `tab_id` may have already been saved.
  if (tab_id != default_tab_id) {
    auto specific_tab_options = tab_panel_options.find(tab_id);
    if (specific_tab_options != tab_panel_options.end())
      return CloneOptions(specific_tab_options->second);
  }

  // Fall back to the default tab if no tab ID was specified or entries for the
  // specific tab weren't found.
  auto default_options = tab_panel_options.find(default_tab_id);
  if (default_options != tab_panel_options.end()) {
    auto options = CloneOptions(default_options->second);
    return options;
  }

  // Fall back to the manifest-specified options as a last resort.
  return GetPanelOptionsFromManifest(extension);
}

api::side_panel::PanelOptions SidePanelService::GetSpecificOptionsForTab(
    const Extension& extension,
    TabId tab_id) {
  auto extension_panel_options = panels_.find(extension.id());
  if (extension_panel_options == panels_.end()) {
    return api::side_panel::PanelOptions();
  }

  TabPanelOptions& tab_panel_options = extension_panel_options->second;
  auto specific_tab_options = tab_panel_options.find(tab_id);
  return specific_tab_options == tab_panel_options.end()
             ? api::side_panel::PanelOptions()
             : CloneOptions(specific_tab_options->second);
}

// Upsert to merge `panels_[extension_id][tab_id]` with `set_options`.
void SidePanelService::SetOptions(const Extension& extension,
                                  api::side_panel::PanelOptions options) {
  auto update_existing_options =
      [&options](api::side_panel::PanelOptions& existing_options) {
        if (options.path) {
          existing_options.path = std::move(options.path);
        }
        if (options.enabled) {
          existing_options.enabled = std::move(options.enabled);
        }
      };

  TabId tab_id = SessionID::InvalidValue().id();
  if (options.tab_id)
    tab_id = *options.tab_id;
  TabPanelOptions& extension_panel_options = panels_[extension.id()];
  auto it = extension_panel_options.find(tab_id);

  // Create the options if they don't exist, otherwise update them.
  if (it != extension_panel_options.end()) {
    update_existing_options(it->second);
  } else {
    // The default value for the optional enabled option is true. This default
    // is applied when the supplied option is being inserted for the first time.
    if (!options.enabled.has_value()) {
      options.enabled = true;
    }

    // If there is no entry for the default tab, merge `options` into the
    // manifest-specified options.
    if (tab_id == SessionID::InvalidValue().id()) {
      extension_panel_options[tab_id] = GetPanelOptionsFromManifest(extension);
      update_existing_options(extension_panel_options[tab_id]);
    } else {
      // Update an existing option.
      extension_panel_options[tab_id] = std::move(options);
    }
  }

  for (auto& observer : observers_) {
    observer.OnPanelOptionsChanged(extension.id(),
                                   extension_panel_options[tab_id]);
  }
}

bool SidePanelService::HasExtensionPanelOptionsForTest(const ExtensionId& id) {
  return panels_.count(id) != 0;
}

// static
BrowserContextKeyedAPIFactory<SidePanelService>*
SidePanelService::GetFactoryInstance() {
  static base::NoDestructor<BrowserContextKeyedAPIFactory<SidePanelService>>
      instance;
  return instance.get();
}

// static
SidePanelService* SidePanelService::Get(content::BrowserContext* context) {
  return BrowserContextKeyedAPIFactory<SidePanelService>::Get(context);
}

void SidePanelService::RemoveExtensionOptions(const ExtensionId& id) {
  panels_.erase(id);
}

bool SidePanelService::OpenSidePanelOnIconClick(
    const ExtensionId& extension_id) {
  bool open_side_panel_on_icon_click = false;
  ExtensionPrefs::Get(browser_context_)
      ->ReadPrefAsBoolean(extension_id, kOpenSidePanelOnIconClickPref,
                          &open_side_panel_on_icon_click);
  return open_side_panel_on_icon_click;
}

void SidePanelService::SetOpenSidePanelOnIconClick(
    const ExtensionId& extension_id,
    bool open_side_panel_on_icon_click) {
  ExtensionPrefs::Get(browser_context_)
      ->SetBooleanPref(extension_id, kOpenSidePanelOnIconClickPref,
                       open_side_panel_on_icon_click);
}

void SidePanelService::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void SidePanelService::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void SidePanelService::OnExtensionUnloaded(
    content::BrowserContext* browser_context,
    const Extension* extension,
    UnloadedExtensionReason reason) {
  RemoveExtensionOptions(extension->id());
}

void SidePanelService::OnExtensionUninstalled(
    content::BrowserContext* browser_context,
    const Extension* extension,
    UninstallReason reason) {
  RemoveExtensionOptions(extension->id());
}

void SidePanelService::Shutdown() {
  for (auto& observer : observers_) {
    observer.OnSidePanelServiceShutdown();
  }
}

}  // namespace extensions

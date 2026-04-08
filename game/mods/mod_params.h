#pragma once

/*!
 * @file mod_params.h
 * Registers the built-in modifiable parameters with the ModManager.
 * Call register_default_params() after graphics init to set up all available mod targets.
 */

#include "common/versions/versions.h"

namespace mods {

/// Register all built-in mod parameters. Call once after graphics data is initialized.
void register_default_params(GameVersion version);

}  // namespace mods

// pc_settings.h without its screen: the values the port's hooks read, at their defaults. The Android settings come later.
#include <rex/runtime.h>

#include "pc_game_info.h"
#include "pc_settings.h"

namespace pcs {

namespace {
Settings g_settings;
}

void Configure(const GameInfo&, bool) {}
void InstallPadTap(rex::RuntimeConfig&) {}
const Settings& Get() { return g_settings; }
float ResolutionScale() { return 1.0f; }
int MsaaSamples() { return 1; }
bool EffectEnabled(int) { return true; }
int Anisotropy() { return 0; }
float Volume() { return 1.0f; }
double GuestRefreshHz() { return 60.0; }
bool MenuOpen() { return false; }
void RequestMenu() {}
void SetGameMenus(bool) {}
bool GameMenuEntry() { return false; }  // no PC SETTINGS line in the game's menus

}  // namespace pcs

const pcs::Theme* PcBf2Theme() { return nullptr; }

// The bindings page: what SteamVR knows about each controller, what a game has bound on each one, and how to translate
// one controller's bindings onto another. No VR calls, so all of it runs in the tests. main.cpp hands it the settings
// calls and the loaded models.
//
// Everything comes from files SteamVR already keeps:
//   <steamvr>/drivers/*/resources/input/*_profile.json    a controller's buttons
//   <steamvr>/drivers/*/resources/input/*_remapping.json  how SteamVR itself translates between controllers
//   <config>/openxr/<app>_<controller>_<n>_binding.json   what an OpenXR game asked for, written at each launch
//   <steam>/config/steamapps.vrmanifest                   where a SteamVR Input game keeps its action file
// A layout goes live by pointing SteamVR's per-game CurrentURL setting at a binding file of ours, which the game picks up
// the next time it launches. Switching it mid-game would need SteamVR's web API, and this stays away from that.
#pragma once
#include <json/json.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ghost3d.h"
#include "ui.h"

namespace fh {
namespace bind {

struct Input {
  std::string key;   // "/input/a"
  std::string name;  // "A"
  std::string kind;  // button, trigger, joystick, trackpad, pinch
  std::string side;  // "", left, right
  bool click = false, touch = false, value = false, force = false;
  bool shown = false;  // worth a row on the page. The system button isn't, for instance: it belongs to SteamVR
  int order = 50;
};

struct Profile {
  std::string type;
  std::string dir;  // the folder it's in
  std::vector<Input> inputs;
  std::map<std::string, std::string> mirror;  // right/input/a <-> left/input/dpad_down, as written in the profile
  std::vector<std::string> poses;             // "/pose/raw", ...
  Json::Value defaultBindings;

  const Input* find(const std::string& hand, const std::string& key) const;
  // "left/input/menu" -> the row it belongs to, which isn't always the same name. Empty if there's no such input.
  std::string rowOf(const std::string& path) const;
};

// A controller people would call by name. Quest 2 and Quest 3 are the same controller_type with different models.
struct Headset {
  std::string name;   // "Quest 3"
  std::string type;   // "oculus_touch"
  std::string model[2];  // left, right
};

struct Dirs {
  std::string steamvr, steam, data;
  std::vector<std::string> config;  // SteamVR's config folders, from openvrpaths.vrpath
};

struct Library {
  std::map<std::string, Profile> profiles;
  std::map<std::string, Json::Value> remaps;  // by to_controller_type
  std::vector<Headset> headsets;              // those this SteamVR can draw
  void load(const Dirs& d);
  const Profile* profile(const std::string& type) const;
};

// A game's binding for one kind of controller.
struct Found {
  bool ok = false;
  Json::Value json;
  std::string path;
  std::string category;  // openxr, steamvr_input, legacy
  std::map<std::string, std::string> names;  // action -> what the game calls it, when it says
};

struct Settings {
  std::function<std::string(const std::string& section, const std::string& key)> get;
  std::function<bool(const std::string& section, const std::string& key, const std::string& value)> set;
  std::function<void(const std::string& section, const std::string& key)> remove;
};

// SteamVR's per-game setting that picks a binding, by category.
std::string currentUrlKey(const std::string& controllerType, const std::string& category);

Found findBinding(const Dirs& d, const Library& lib, const Settings& s, const std::vector<std::string>& appKeys, const std::string& controllerType);

// Which of their inputs each of yours acts as. Keys and values are "left/input/x". A value of "" means nothing.
using Wiring = std::map<std::string, std::string>;

// SteamVR's own translation when it has one, the reverse of the other direction's if not, and same-named inputs after that.
Wiring defaultWiring(const Library& lib, const std::string& theirs, const std::string& yours);

// Their binding, rewritten to run on your controller. It still drives the same actions in the game, and an OpenXR game
// goes on being told it's holding the other controller.
Json::Value translate(const Json::Value& from, const Wiring& wiring, const Profile& theirs, const Profile& yours);

// What an action is called on the page. passThrough: the name only says which button it came from, as in Unreal's
// "oculustouch_right_a_click", so the game's meaning for it can't be known. hide: it isn't a separate thing to show.
std::string actionLabel(const std::string& output, const std::map<std::string, std::string>& names, bool* passThrough, bool* hide);

// A row for each button on one hand, with what the binding has on it.
struct RowText { std::string id, name, action; bool bound = false, passThrough = false, unused = false; };
// group: only the controls in that group of action sets (see controlGroups). Empty, or one this binding doesn't have: all of it.
std::vector<RowText> rowsOn(const Profile& p, const char* hand, const Found* binding, const std::string& group = "");

// Games split their controls into sets: on foot, in a ship, in the menus. On Foot and its left- and right-handed versions
// count as one group here. They come back in the game's own order if it says, and each has what the game calls it.
struct ControlGroup { std::string id, name; int labelled = 0; };
std::vector<ControlGroup> controlGroups(const Found& binding);

// Which of a model's parts an input is. The picture's lines end there.
std::vector<std::string> partsFor(const std::string& key);

class Page {
 public:
  Page(Dirs dirs, Settings settings) : d_(std::move(dirs)), s_(std::move(settings)) {}

  void load();  // the first time the page opens
  bool loaded() const { return loaded_; }

  void setYours(const std::string& controllerType, const std::string& modelLeft, const std::string& modelRight);
  // key: SteamVR's name for the running game, alt: any other name it might be filed under. Both empty for no game.
  void setGame(const std::string& key, const std::string& alt, const std::string& name);
  void gameStarted();  // puts this game on the layout it should be on, so it's right the next time it launches
  void restoreAll();   // being removed: every game goes back to the binding it had

  std::vector<std::string> modelsWanted() const;
  void give(const std::string& model, const Mesh& mesh, const std::vector<ModelPart>& parts);

  BindModel model(const std::string& hover, const std::string& down);
  void click(const std::string& id);
  void clickGreyed(const std::string& id);

  std::string status;
  int statusKind = 0;  // 0 info, 1 good, 2 warning
  uint32_t statusSerial = 0;  // goes up each time status is set, so the caller can time it out

  const Headset& reference() const { return ref_; }
  void setReference(const std::string& name);

  // What's saved. Public so the tests can look.
  struct GameChoice {
    std::string from;      // the controller_type to take bindings from. "" = as the developer set it
    bool explicitly = false;  // chosen for this game rather than following the all-games default
    std::string settingKey, originalUrl;
    bool managed = false;  // SteamVR's setting points at our file
  };
  std::map<std::string, std::string> defaults;   // your controller_type -> from
  std::map<std::string, GameChoice> games;       // "<game>|<your controller_type>"
  std::map<std::string, Wiring> wiringChanges;   // "<their type>><your type>" -> only what's been changed
  std::string fileFor(const std::string& game, const std::string& yourType) const;

 private:
  void say(const std::string& text, int kind = 0);
  void loadPrefs();
  void savePrefs() const;
  const Found& binding(const std::string& type);
  Wiring wiring(const std::string& theirs, const std::string& yours) const;
  std::string effectiveFrom() const;  // which controller's bindings this game is on, "" = the developer's
  bool applyNow(const std::string& from);
  void revertNow();
  void reapply();
  void bake(const std::string& model);
  void fillRows(BindSide& side, const std::string& prefix, const Headset& h, const std::vector<RowText> rows[2]);
  const Headset* byModel(const std::string& model) const;
  std::string choiceKey() const { return gameKey_ + "|" + yours_.type; }
  static std::string shortName(const Headset& h);

  Dirs d_;
  Settings s_;
  Library lib_;
  bool loaded_ = false;
  Headset yours_, ref_;
  std::string gameKey_, gameAlt_, gameName_;
  bool saveForGame_ = true;
  std::string armed_;  // your row being rewired
  std::string group_;  // which of the game's sets of controls is showing
  std::map<std::string, Found> found_;
  struct Held { Mesh mesh; std::vector<ModelPart> parts; bool have = false; std::shared_ptr<ControllerPicture> pic; };
  std::map<std::string, Held> models_;
  std::string savedRef_;
};

}  // namespace bind
}  // namespace fh

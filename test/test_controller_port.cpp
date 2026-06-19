#include "gtest/gtest.h"

#include "game/system/hid/input_manager.h"

using game_settings::InputSettings;

static void simulate_set_controller_for_port(
    const std::vector<std::string>& guids,
    int controller_id,
    int port,
    InputSettings& settings) {
  // Mirror the stale-entry cleanup from set_controller_for_port
  for (auto it = settings.controller_port_mapping.begin();
       it != settings.controller_port_mapping.end();) {
    if (it->second == port && it->first != guids.at(controller_id)) {
      it = settings.controller_port_mapping.erase(it);
    } else {
      ++it;
    }
  }
  settings.controller_port_mapping[guids.at(controller_id)] = port;
  settings.last_selected_controller_guid = guids.at(controller_id);
  settings.last_selected_controller_index = controller_id;
}

class ControllerPortTest : public ::testing::Test {
 protected:
  InputSettings settings;
  std::unordered_map<int, int> port_mapping;
};

TEST_F(ControllerPortTest, SingleControllerFirstLaunch) {
  std::vector<std::string> guids = {"guid_A"};
  InputManager::resolve_port_mappings(guids, settings, port_mapping);
  EXPECT_EQ(port_mapping[0], 0);
}

TEST_F(ControllerPortTest, TwoControllersFirstLaunch) {
  std::vector<std::string> guids = {"guid_A", "guid_B"};
  InputManager::resolve_port_mappings(guids, settings, port_mapping);
  EXPECT_EQ(port_mapping[0], 0);
  EXPECT_EQ(port_mapping[1], 1);
}

TEST_F(ControllerPortTest, SelectSecondControllerThenRestart) {
  std::vector<std::string> guids = {"guid_A", "guid_B"};

  // First launch — default assignment
  InputManager::resolve_port_mappings(guids, settings, port_mapping);
  EXPECT_EQ(port_mapping[0], 0);

  // User selects controller B (index 1) for port 0
  simulate_set_controller_for_port(guids, 1, 0, settings);

  // Simulate restart — resolve again with same settings
  InputManager::resolve_port_mappings(guids, settings, port_mapping);
  EXPECT_EQ(port_mapping[0], 1) << "Port 0 should map to controller B after restart";
}

TEST_F(ControllerPortTest, SelectSecondControllerReversedOrderOnRestart) {
  std::vector<std::string> guids = {"guid_A", "guid_B"};

  // First launch
  InputManager::resolve_port_mappings(guids, settings, port_mapping);
  simulate_set_controller_for_port(guids, 1, 0, settings);

  // Restart with reversed enumeration order
  std::vector<std::string> reversed_guids = {"guid_B", "guid_A"};
  InputManager::resolve_port_mappings(reversed_guids, settings, port_mapping);
  EXPECT_EQ(port_mapping[0], 0) << "Port 0 should map to guid_B which is now at index 0";
}

TEST_F(ControllerPortTest, SelectThenReselectFirstController) {
  std::vector<std::string> guids = {"guid_A", "guid_B"};

  // First launch
  InputManager::resolve_port_mappings(guids, settings, port_mapping);

  // Select B
  simulate_set_controller_for_port(guids, 1, 0, settings);
  // Re-select A
  simulate_set_controller_for_port(guids, 0, 0, settings);

  // Restart
  InputManager::resolve_port_mappings(guids, settings, port_mapping);
  EXPECT_EQ(port_mapping[0], 0) << "Port 0 should map to controller A after re-selecting it";
}

TEST_F(ControllerPortTest, ControllerRemovedBetweenSessions) {
  std::vector<std::string> guids = {"guid_A", "guid_B"};

  // First launch, select B
  InputManager::resolve_port_mappings(guids, settings, port_mapping);
  simulate_set_controller_for_port(guids, 1, 0, settings);

  // Restart with only A connected
  std::vector<std::string> only_a = {"guid_A"};
  InputManager::resolve_port_mappings(only_a, settings, port_mapping);
  EXPECT_EQ(port_mapping[0], 0) << "With only A connected, port 0 should map to it";
}

TEST_F(ControllerPortTest, SelectedControllerRemovedOtherPresent) {
  std::vector<std::string> guids = {"guid_A", "guid_B"};

  // Select B
  InputManager::resolve_port_mappings(guids, settings, port_mapping);
  simulate_set_controller_for_port(guids, 1, 0, settings);

  // Restart with only A — B's mapping was cleared when B was selected, so A has no saved port
  std::vector<std::string> only_a = {"guid_A"};
  InputManager::resolve_port_mappings(only_a, settings, port_mapping);
  EXPECT_EQ(port_mapping[0], 0);
}

TEST_F(ControllerPortTest, ThreeControllersSelectMiddle) {
  std::vector<std::string> guids = {"guid_A", "guid_B", "guid_C"};

  // First launch
  InputManager::resolve_port_mappings(guids, settings, port_mapping);

  // Select B (index 1) for port 0
  simulate_set_controller_for_port(guids, 1, 0, settings);

  // Restart
  InputManager::resolve_port_mappings(guids, settings, port_mapping);
  EXPECT_EQ(port_mapping[0], 1) << "Port 0 should map to controller B";
}

TEST_F(ControllerPortTest, SettingsRoundTripThroughJson) {
  std::vector<std::string> guids = {"guid_A", "guid_B"};

  // First launch, select B
  InputManager::resolve_port_mappings(guids, settings, port_mapping);
  simulate_set_controller_for_port(guids, 1, 0, settings);

  // Serialize and deserialize (simulates save_settings / load_settings)
  json j;
  game_settings::to_json(j, settings);
  InputSettings restored_settings;
  game_settings::from_json(j, restored_settings);

  // Resolve with restored settings
  std::unordered_map<int, int> restored_port_mapping;
  InputManager::resolve_port_mappings(guids, restored_settings, restored_port_mapping);
  EXPECT_EQ(restored_port_mapping[0], 1)
      << "Port 0 should map to controller B after JSON round-trip";
}

TEST_F(ControllerPortTest, SettingsRoundTripReversedOrder) {
  std::vector<std::string> guids = {"guid_A", "guid_B"};

  // First launch, select B
  InputManager::resolve_port_mappings(guids, settings, port_mapping);
  simulate_set_controller_for_port(guids, 1, 0, settings);

  // JSON round-trip
  json j;
  game_settings::to_json(j, settings);
  InputSettings restored_settings;
  game_settings::from_json(j, restored_settings);

  // Restart with reversed order
  std::vector<std::string> reversed = {"guid_B", "guid_A"};
  std::unordered_map<int, int> restored_port_mapping;
  InputManager::resolve_port_mappings(reversed, restored_settings, restored_port_mapping);
  EXPECT_EQ(restored_port_mapping[0], 0)
      << "Port 0 should map to guid_B (now index 0) after JSON round-trip with reversed order";
}

TEST_F(ControllerPortTest, NoLastSelectedFallsBackToSavedMapping) {
  settings.controller_port_mapping["guid_A"] = 0;
  settings.controller_port_mapping["guid_B"] = 1;
  settings.last_selected_controller_guid = "";

  std::vector<std::string> guids = {"guid_A", "guid_B"};
  InputManager::resolve_port_mappings(guids, settings, port_mapping);
  EXPECT_EQ(port_mapping[0], 0);
  EXPECT_EQ(port_mapping[1], 1);
}

TEST_F(ControllerPortTest, EmptyControllerList) {
  std::vector<std::string> guids = {};
  InputManager::resolve_port_mappings(guids, settings, port_mapping);
  EXPECT_TRUE(port_mapping.empty());
}

TEST_F(ControllerPortTest, NewControllerAddedBetweenSessions) {
  std::vector<std::string> guids = {"guid_A"};

  // First launch with 1 controller, select it
  InputManager::resolve_port_mappings(guids, settings, port_mapping);
  simulate_set_controller_for_port(guids, 0, 0, settings);

  // Restart with a new controller added
  std::vector<std::string> guids_with_new = {"guid_A", "guid_C"};
  InputManager::resolve_port_mappings(guids_with_new, settings, port_mapping);
  EXPECT_EQ(port_mapping[0], 0) << "Port 0 should still map to guid_A";
  EXPECT_EQ(port_mapping[1], 1) << "New controller guid_C should get port 1";
}

// --- Identical GUID tests (same hardware model) ---

TEST_F(ControllerPortTest, IdenticalControllersSelectSecond) {
  std::vector<std::string> guids = {"xbox_guid", "xbox_guid"};

  // First launch
  InputManager::resolve_port_mappings(guids, settings, port_mapping);

  // User selects second identical controller (index 1) for port 0
  simulate_set_controller_for_port(guids, 1, 0, settings);

  // Restart with same enumeration order
  InputManager::resolve_port_mappings(guids, settings, port_mapping);
  EXPECT_EQ(port_mapping[0], 1)
      << "Port 0 should map to second identical controller when index hint matches";
}

TEST_F(ControllerPortTest, IdenticalControllersSelectFirst) {
  std::vector<std::string> guids = {"xbox_guid", "xbox_guid"};

  // First launch
  InputManager::resolve_port_mappings(guids, settings, port_mapping);

  // User selects first identical controller (index 0) for port 0
  simulate_set_controller_for_port(guids, 0, 0, settings);

  // Restart
  InputManager::resolve_port_mappings(guids, settings, port_mapping);
  EXPECT_EQ(port_mapping[0], 0) << "Port 0 should map to first identical controller";
}

TEST_F(ControllerPortTest, IdenticalControllersJsonRoundTrip) {
  std::vector<std::string> guids = {"xbox_guid", "xbox_guid"};

  // Select second controller
  InputManager::resolve_port_mappings(guids, settings, port_mapping);
  simulate_set_controller_for_port(guids, 1, 0, settings);

  // JSON round-trip
  json j;
  game_settings::to_json(j, settings);
  InputSettings restored_settings;
  game_settings::from_json(j, restored_settings);

  // Resolve with restored settings
  std::unordered_map<int, int> restored_port_mapping;
  InputManager::resolve_port_mappings(guids, restored_settings, restored_port_mapping);
  EXPECT_EQ(restored_port_mapping[0], 1)
      << "Second identical controller should persist across JSON round-trip";
}

TEST_F(ControllerPortTest, IdenticalControllersIndexHintOutOfRange) {
  std::vector<std::string> guids = {"xbox_guid", "xbox_guid"};

  // Simulate stale settings with an out-of-range index
  settings.last_selected_controller_guid = "xbox_guid";
  settings.last_selected_controller_index = 5;
  settings.controller_port_mapping["xbox_guid"] = 0;

  InputManager::resolve_port_mappings(guids, settings, port_mapping);
  // Should fall back to first GUID match
  EXPECT_EQ(port_mapping[0], 0);
}

TEST_F(ControllerPortTest, MixedIdenticalAndUniqueControllers) {
  std::vector<std::string> guids = {"xbox_guid", "ps5_guid", "xbox_guid"};

  // First launch
  InputManager::resolve_port_mappings(guids, settings, port_mapping);

  // Select the second Xbox controller (index 2) for port 0
  simulate_set_controller_for_port(guids, 2, 0, settings);

  // Restart
  InputManager::resolve_port_mappings(guids, settings, port_mapping);
  EXPECT_EQ(port_mapping[0], 2)
      << "Port 0 should map to third controller (second Xbox) via index hint";
}

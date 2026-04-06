// Copyright (c) 2018-2025 Marco Wang <m.aesophor@gmail.com>. All rights reserved.

#ifndef REQUIEM_MAP_GAME_MAP_MANAGER_H_
#define REQUIEM_MAP_GAME_MAP_MANAGER_H_

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>

#include <axmol.h>

#include <box2d/box2d.h>

#include "Controllable.h"
#include "character/Character.h"
#include "character/Player.h"
#include "item/Item.h"
#include "map/GameMap.h"
#include "map/Lighting.h"
#include "map/WorldContactListener.h"
#include "ui/Shade.h"

namespace requiem {

class GameMapManager final {
  friend class GameState;

 public:
  explicit GameMapManager(const b2Vec2& gravity);

  void update(const float delta);

  // @param tmxMapFilePath: the target .tmx file to load
  // @param afterLoadingGameMap: guaranteed to be called after the GameMap
  //                             has been loaded (optional).
  void loadGameMap(const std::string& tmxMapFilePath,
                   const std::function<void (const GameMap*)>& afterLoadingGameMap=[](const GameMap*) {},
                   const float fadeInSec = Shade::kFadeInSec,
                   const float fadeOutSec = Shade::kFadeOutSec);
  void destroyGameMap();
  bool raycast(const b2Vec2& src, const b2Vec2& dst, const short categoryBitsToStop,
               const bool shouldDrawLine = false) const;

  std::optional<std::string> getTmxMapFilePathByMapAlias(const std::string& mapAlias) const;

  bool isNpcAllowedToSpawn(const std::string& jsonFilePath) const;
  void setNpcAllowedToSpawn(const std::string& jsonFilePath, bool canSpawn);

  inline bool isLoadingGameMap() const { return _isLoadingGameMap; }
  inline bool areNpcsAllowedToAct() const { return _areNpcsAllowedToAct; }
  inline void setNpcsAllowedToAct(bool npcsAllowedToAct) {
    _areNpcsAllowedToAct = npcsAllowedToAct;
  }

  bool hasSavedOpenedClosedState(const std::string& tmxMapFilePath,
                                 const GameMap::OpenableObjectType type,
                                 const int targetObjectId) const;
  bool isOpened(const std::string& tmxMapFilePath,
                const GameMap::OpenableObjectType type,
                const int targetPortalId) const;
  void setOpened(const std::string& tmxMapFilePath,
                 const GameMap::OpenableObjectType type,
                 const int targetPortalId,
                 const bool locked);
  bool hasActivatedTrigger(const std::string& tmxMapFilePath,
                           const int targetTriggerId) const;
  void setTriggerActivated(const std::string& tmxMapFilePath,
                           const int targetTriggerId);

  inline ax::Layer* getParallaxLayer() const { return _parallaxLayer; }
  inline ax::Layer* getLayer() const { return _layer; }
  inline b2World* getWorld() const { return _world.get(); }
  inline Lighting* getLighting() const { return _lighting.get(); }
  inline GameMap* getGameMap() const { return _gameMap.get(); }
  inline Player* getPlayer() const { return _player.get(); }

 private:
  bool initMapAliases();
  void doLoadGameMap(const std::string& tmxMapFilePath);
  std::string getOpenableObjectQueryKey(const std::string& tmxMapFilePath,
                                        const GameMap::OpenableObjectType type,
                                        const int targetObjectId) const;
  std::string getActivatedTriggerQueryKey(const std::string& tmxMapFilePath,
                                          const int targetObjectId) const;

  ax::Layer* _parallaxLayer{};
  ax::Layer* _layer{};
  std::unique_ptr<WorldContactListener> _worldContactListener;
  std::unique_ptr<b2World> _world;
  std::unique_ptr<Lighting> _lighting;
  std::unique_ptr<GameMap> _gameMap;
  std::unique_ptr<Player> _player;
  std::unordered_map<std::string, std::string> _mapAliasToTmxMapFilePath;

  bool _isLoadingGameMap{};
  bool _areNpcsAllowedToAct{true};
  std::unordered_set<std::string> _npcSpawningBlacklist;

  // This includes:
  // 1. The unlock/lock state of all portals in all maps.
  // 2. The open/close state of all chests in all maps.
  std::unordered_map<std::string, bool> _allOpenableObjectStates;

  std::unordered_set<std::string> _activatedTriggers;
};

}  // namespace requiem

#endif  // REQUIEM_MAP_GAME_MAP_MANAGER_H_

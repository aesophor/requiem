// Copyright (c) 2018-2025 Marco Wang <m.aesophor@gmail.com>. All rights reserved.

#include "GameMapManager.h"

#include <filesystem>
#include <fstream>
#include <system_error>

#include <box2d/box2d.h>

#include "Assets.h"
#include "Audio.h"
#include "CallbackManager.h"
#include "Constants.h"
#include "character/Npc.h"
#include "character/Player.h"
#include "item/Equipment.h"
#include "scene/GameScene.h"
#include "scene/SceneManager.h"
#include "skill/MagicalMissile.h"
#include "util/AxUtil.h"
#include "util/B2BodyBuilder.h"
#include "util/B2RayCastUtil.h"
#include "util/StringUtil.h"

namespace fs = std::filesystem;
using namespace std;
USING_NS_AX;

namespace requiem {

GameMapManager::GameMapManager(const b2Vec2& gravity)
    : _parallaxLayer{Layer::create()},
      _layer{Layer::create()},
      _worldContactListener{std::make_unique<WorldContactListener>()},
      _world{std::make_unique<b2World>(gravity)},
      _lighting{std::make_unique<Lighting>()} {
  _world->SetAllowSleeping(true);
  _world->SetContinuousPhysics(true);
  _world->SetContactListener(_worldContactListener.get());

  ax_util::addChildWithParentCameraMask(_layer, _lighting->getLayer(), z_order::kDefault + 1);

  if (!initMapAliases()) {
    VGLOG(LOG_INFO, "Failed to init map aliases");
  }
}

bool GameMapManager::initMapAliases() {
  error_code ec{};
  const bool exists = fs::exists(assets::kMapAliasesIni, ec);
  if (ec) {
    VGLOG(LOG_INFO, "Failed to init map aliases from [%s], err [%s]", assets::kMapAliasesIni.c_str(), ec.message().c_str());
    return false;
  }
  if (!exists) {
    VGLOG(LOG_INFO, "Failed to init map aliases from [%s], err [No such file or directory]", assets::kMapAliasesIni.c_str());
    return false;
  }

  ifstream fin{assets::kMapAliasesIni};
  if (!fin.is_open()) {
    VGLOG(LOG_INFO, "Failed to open [%s], err [%s]", assets::kMapAliasesIni.c_str(), strerror(errno));
    return false;
  }

  string line;
  while (std::getline(fin, line)) {
    string_util::strip(line);
    if (line.empty() || (line.front() == '[' && line.back() == ']')) {
      continue;
    }

    vector<string> key_val = string_util::split(line, '=');
    string& key = key_val[0];
    string& val = key_val[1];
    string_util::strip(key);
    string_util::strip(val);
    _mapAliasToTmxMapFilePath.emplace(std::move(key), std::move(val));
  }
  return true;
}

void GameMapManager::update(const float delta) {
  if (!_gameMap) {
    return;
  }
  _gameMap->update(delta);

  if (!_player) {
    return;
  }
  _player->update(delta);
  for (const auto& ally : _player->getAllies()) {
    ally->update(delta);
  }

  _lighting->update();
}

void GameMapManager::loadGameMap(const string& tmxMapFilePath,
                                 const function<void (const GameMap*)>& afterLoadingGameMap,
                                 const float fadeInSec,
                                 const float fadeOutSec) {
  _isLoadingGameMap = true;

  auto shade = SceneManager::the().getCurrentScene<GameScene>()->getShade();
  shade->getImageView()->runAction(Sequence::create(
      FadeIn::create(fadeInSec),
      CallFunc::create([this, shade, tmxMapFilePath, afterLoadingGameMap]() {
        doLoadGameMap(tmxMapFilePath);
        afterLoadingGameMap(_gameMap.get());
        _areNpcsAllowedToAct = true;
        _isLoadingGameMap = false;
      }),
      FadeOut::create(fadeOutSec),
      nullptr
  ));
}

void GameMapManager::destroyGameMap() {
  setNpcsAllowedToAct(false);

  if (_player) {
    for (auto ally : _player->getAllies()) {
      ally->beforeMapChanged();
    }
  }

  if (_gameMap) {
    _parallaxLayer->removeAllChildren();
    _layer->removeChild(_gameMap->getTmxTiledMap());
    _gameMap.reset();
  }
}

void GameMapManager::doLoadGameMap(const string& tmxMapFilePath) {
  const string oldBgmFilePath = (_gameMap) ? _gameMap->getBgmFilePath() : "";

  destroyGameMap();
  _gameMap = std::make_unique<GameMap>(_world.get(), _lighting.get(), tmxMapFilePath);
  _gameMap->createObjects();
  ax_util::addChildWithParentCameraMask(_layer, _gameMap->getTmxTiledMap(), z_order::kTmxTiledMap);

  if (!_player) {
    _player = _gameMap->createPlayer();
    //_lighting->addLightSource(_player.get());
  }

  _lighting->setGameMap(_gameMap.get());
  _lighting->setDarknessOverlaySize(_gameMap->getWidth(), _gameMap->getHeight());

  if (oldBgmFilePath != _gameMap->getBgmFilePath()) {
    Audio::the().playBgm(_gameMap->getBgmFilePath());
  }
}

bool GameMapManager::raycast(const b2Vec2& src, const b2Vec2& dst, const short categoryBitsToStop,
                             const bool shouldDrawLine) const {
  if (shouldDrawLine) {
    auto draw = ax::DrawNode::create();
    draw->drawLine(Point{src.x * kPpm, src.y * kPpm}, Point{dst.x * kPpm, dst.y * kPpm}, ax::Color4F::WHITE);
    _layer->addChild(draw, z_order::kHud);
  }

  B2RayCastCallback cb{[categoryBitsToStop](b2Fixture* fixture, const b2Vec2& point, const b2Vec2& normal, float fraction) -> float {
    if (fixture->GetFilterData().categoryBits & categoryBitsToStop) {
      return 0;
    }
    return -1;
  }};
  _world->RayCast(&cb, src, dst);
  return cb.hasHit();
}

optional<string> GameMapManager::getTmxMapFilePathByMapAlias(const string& mapAlias) const {
  const auto it = _mapAliasToTmxMapFilePath.find(mapAlias);
  return it != _mapAliasToTmxMapFilePath.end() ? it->second : optional<string>{std::nullopt};
}

bool GameMapManager::isNpcAllowedToSpawn(const string& jsonFilePath) const {
  return _npcSpawningBlacklist.find(jsonFilePath) == _npcSpawningBlacklist.end();
}

void GameMapManager::setNpcAllowedToSpawn(const string& jsonFilePath, bool canSpawn) {
  if (!canSpawn && isNpcAllowedToSpawn(jsonFilePath)) {
    _npcSpawningBlacklist.insert(jsonFilePath);
  } else if (canSpawn && !isNpcAllowedToSpawn(jsonFilePath)) {
    _npcSpawningBlacklist.erase(jsonFilePath);
  }
}

bool GameMapManager::hasSavedOpenedClosedState(const string& tmxMapFilePath,
                                               const GameMap::OpenableObjectType type,
                                               const int targetPortalId) const {
  const string key = getOpenableObjectQueryKey(tmxMapFilePath, type, targetPortalId);
  auto it = _allOpenableObjectStates.find(key);
  return it != _allOpenableObjectStates.end();
}

bool GameMapManager::isOpened(const string& tmxMapFilePath,
                              const GameMap::OpenableObjectType type,
                              const int targetPortalId) const {
  const string key = getOpenableObjectQueryKey(tmxMapFilePath, type, targetPortalId);
  auto it = _allOpenableObjectStates.find(key);
  if (it == _allOpenableObjectStates.end()) {
    VGLOG(LOG_WARN, "Unable to find portal lock/unlock state, map [%s], portalId [%d].",
          tmxMapFilePath.c_str(), targetPortalId);
    return false;
  }

  return it->second;
}

void GameMapManager::setOpened(const string& tmxMapFilePath,
                               const GameMap::OpenableObjectType type,
                               const int targetPortalId,
                               const bool locked) {
  const string key = getOpenableObjectQueryKey(tmxMapFilePath, type, targetPortalId);
  _allOpenableObjectStates[key] = locked;
}

bool GameMapManager::hasActivatedTrigger(const string& tmxMapFilePath, const int targetTriggerId) const {
  const string key = getActivatedTriggerQueryKey(tmxMapFilePath, targetTriggerId);
  return _activatedTriggers.contains(key);
}

void GameMapManager::setTriggerActivated(const string& tmxMapFilePath, const int targetTriggerId) {
  const string key = getActivatedTriggerQueryKey(tmxMapFilePath, targetTriggerId);
  _activatedTriggers.insert(key);
}

string GameMapManager::getOpenableObjectQueryKey(const string& tmxMapFilePath,
                                                 const GameMap::OpenableObjectType type,
                                                 const int targetObjectId) const {
  string typeStr;
  switch (type) {
    case GameMap::OpenableObjectType::PORTAL:
      typeStr = "p";
      break;
    case GameMap::OpenableObjectType::CHEST:
      typeStr = "c";
      break;
    default:
      break;
  }

  return string_util::format("%s_%s_%d", tmxMapFilePath.c_str(), typeStr.c_str(), targetObjectId);
}

string GameMapManager::getActivatedTriggerQueryKey(const std::string& tmxMapFilePath,
                                                   const int targetObjectId) const {
  return string_util::format("%s_%d", tmxMapFilePath.c_str(), targetObjectId);
}

}  // namespace requiem

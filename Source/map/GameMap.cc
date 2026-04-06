// Copyright (c) 2018-2025 Marco Wang <m.aesophor@gmail.com>. All rights reserved.

#include "GameMap.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <thread>

#include "Assets.h"
#include "Audio.h"
#include "CallbackManager.h"
#include "Constants.h"
#include "character/Character.h"
#include "character/Player.h"
#include "character/Npc.h"
#include "character/Party.h"
#include "item/Equipment.h"
#include "item/Consumable.h"
#include "item/Key.h"
#include "map/object/Chest.h"
#include "map/object/StaticObject.h"
#include "scene/GameScene.h"
#include "scene/SceneManager.h"
#include "ui/Colorscheme.h"
#include "ui/dialogue/Subtitles.h"
#include "util/AxUtil.h"
#include "util/B2BodyBuilder.h"
#include "util/Logger.h"
#include "util/MathUtil.h"
#include "util/StringUtil.h"
#include "util/RandUtil.h"

namespace fs = std::filesystem;
using namespace std;
using namespace requiem::assets;
USING_NS_AX;

namespace requiem {

GameMap::GameMap(b2World* world, Lighting* lighting, const string& tmxMapFilePath)
    : _world{world},
      _lighting{lighting},
      _tmxTiledMap{TMXTiledMap::create(tmxMapFilePath)},
      _tmxTiledMapFilePath{tmxMapFilePath},
      _bgmFilePath{_tmxTiledMap->getProperty("bgm").asString()},
      _parallaxBackground{std::make_unique<ParallaxBackground>()},
      _pathFinder{std::make_unique<SimplePathFinder>()} {}

GameMap::~GameMap() {
  for (auto& actor : _dynamicActors) {
    actor->removeFromMap();
  }

  for (auto& actor : _staticActors) {
    actor->removeFromMap();
  }

  for (auto body : _tmxTiledMapBodies) {
    _world->DestroyBody(body);
  }

  _lighting->clear();
}

void GameMap::update(const float delta) {
  _parallaxBackground->update(delta);

  for (auto& actor : _dynamicActors) {
    actor->update(delta);
  }
}

void GameMap::createObjects() {
  // Create box2d objects from layers.
  list<b2Body*> bodies = createPolylines("Ground", category_bits::kGround, true, kGroundFriction);
  _tmxTiledMapBodies.splice(_tmxTiledMapBodies.end(), bodies);

  bodies = createPolylines("Wall", category_bits::kWall, true, kWallFriction);
  _tmxTiledMapBodies.splice(_tmxTiledMapBodies.end(), bodies);

  bodies = createRectangles("Platform", category_bits::kPlatform, true, kGroundFriction);
  _tmxTiledMapPlatformBodies.insert(_tmxTiledMapPlatformBodies.end(), bodies.begin(), bodies.end());
  _tmxTiledMapBodies.splice(_tmxTiledMapBodies.end(), bodies);

  bodies = createPolylines("PivotMarker", category_bits::kPivotMarker, false, 0);
  _tmxTiledMapBodies.splice(_tmxTiledMapBodies.end(), bodies);

  bodies = createPolylines("CliffMarker", category_bits::kCliffMarker, false, 0);
  _tmxTiledMapBodies.splice(_tmxTiledMapBodies.end(), bodies);

  const Value locationNameProperty = _tmxTiledMap->getProperty("locationName");
  if (!locationNameProperty.isNull()) {
    _locationName = locationNameProperty.asString();
  }

  createTriggers();
  createPortals();
  createChests();
  createNpcs();
  createLightSources();
  createAnimatedObjects();
  createParallaxBackground();
}

unique_ptr<Player> GameMap::createPlayer() const {
  auto player = std::make_unique<Player>(assets::kPlayerJson);

  const TMXObjectGroup* objGroup = _tmxTiledMap->getObjectGroup("Player");
  const auto& valMap = objGroup->getObjects().at(0).asValueMap();
  float x = valMap.at("x").asFloat();
  float y = valMap.at("y").asFloat();

  if (auto it = valMap.find("isFacingRight"); it != valMap.end()) {
    player->setFacingRight(it->second.asBool());
  }

  player->showOnMap(x, y);
  return player;
}

Item* GameMap::createItem(const string& itemJson, float x, float y, int amount) {
  return createItems({{itemJson, amount}}, x, y).front();
}

vector<Item*> GameMap::createItems(const vector<pair<string, int>>& itemJsons, float x, float y) {
  vector<Item*> items;

  const int n = itemJsons.size();
  const float spread = 0.4f;

  for (int i = 0; i < n; i++) {
    const auto& [itemJson, amount] = itemJsons[i];

    Item* item = showDynamicActor<Item>(Item::create(itemJson), x, y);
    item->setAmount(amount);

    float offsetX = 0.0f;
    float offsetY = 3.0f;

    // Spread the items evenly around the center.
    if (n > 1) {
      offsetX = std::lerp(-spread, spread, static_cast<float>(i) / (n - 1));
    }

    item->getBody()->ApplyLinearImpulseToCenter({offsetX, offsetY}, true);

    items.push_back(item);
  }

  return items;
}

bool GameMap::beginBossFight(const string& targetNpcJsonFilePath,
                             const string& bgmFilePath,
                             const bool isGameOverOnPlayerKilled,
                             vector<string>&& execOnBegin,
                             vector<string>&& execOnPlayerKilled) {
  if (_isInBossFight) {
    return false;
  }

  auto gmMgr = SceneManager::the().getCurrentScene<GameScene>()->getGameMapManager();
  auto player = gmMgr->getPlayer();
  if (!player) {
    VGLOG(LOG_ERR, "Failed to get player");
    return false;
  }

  auto it = std::find_if(_dynamicActors.begin(), _dynamicActors.end(), [](const shared_ptr<DynamicActor>& actor) {
    return std::dynamic_pointer_cast<Character>(actor);
  });
  if (it == _dynamicActors.end()) {
    VGLOG(LOG_ERR, "Failed to find [%s] in the current game map.", targetNpcJsonFilePath.c_str());
    return false;
  }

  shared_ptr<Character> target = std::static_pointer_cast<Character>(*it);
  if (target->isSetToKill() || target->isKilled()) {
    VGLOG(LOG_ERR, "Failed to begin boss fight [%s], target is already killed.", targetNpcJsonFilePath.c_str());
    return false;
  }

  CallbackManager::the().runAfter([player, target](const CallbackManager::CallbackId) {
    target->lockOn(player);
  }, Subtitles::kLetterboxTransitionDuration);

  target->addOnKilledCallback([this]() { endBossFight(/*isPlayerKilled=*/false); });
  player->addOnKilledCallback([this]() { endBossFight(/*isPlayerKilled=*/true); });

  for (const auto& trigger : _triggers) {
    trigger->onBossFightBegin();
  }

  auto console = SceneManager::the().getCurrentScene<GameScene>()->getConsole();
  for (const auto& cmd : execOnBegin) {
    console->executeCmd(cmd);
  }

  Audio::the().playBgm(bgmFilePath);

  _isInBossFight = true;
  _isGameOverOnPlayerKilled = isGameOverOnPlayerKilled;
  _execOnPlayerKilled = std::move(execOnPlayerKilled);
  return true;
}

void GameMap::endBossFight(const bool isPlayerKilled) {
  if (!isPlayerKilled) {
    Audio::the().playBgm(_bgmFilePath);
  }

  for (const auto& trigger : _triggers) {
    trigger->onBossFightEnd();
  }

  auto console = SceneManager::the().getCurrentScene<GameScene>()->getConsole();
  for (const auto& cmd : _execOnPlayerKilled) {
    console->executeCmd(cmd);
  }

  _isInBossFight = false;
  _isGameOverOnPlayerKilled = true;
  _execOnPlayerKilled.clear();
}

float GameMap::getWidth() const {
  return _tmxTiledMap->getMapSize().width * _tmxTiledMap->getTileSize().width;
}

float GameMap::getHeight() const {
  return _tmxTiledMap->getMapSize().height * _tmxTiledMap->getTileSize().height;
}

ax::ValueVector GameMap::getObjects(const string& layerName) {
  auto objectGroup = _tmxTiledMap->getObjectGroup(layerName);
  if (!objectGroup) {
    VGLOG(LOG_INFO, "Failed to get object object group: [%s]", layerName.c_str());
    return {};
  }

  return objectGroup->getObjects();
}

list<b2Body*> GameMap::createRectangles(const string& layerName, const short categoryBits,
                                        const bool collidable, const float defaultFriction) {
  list<b2Body*> bodies;

  for (const auto& rectObj : getObjects(layerName)) {
    const auto& valMap = rectObj.asValueMap();
    float x = valMap.at("x").asFloat();
    float y = valMap.at("y").asFloat();
    float w = valMap.at("width").asFloat();
    float h = valMap.at("height").asFloat();

    B2BodyBuilder bodyBuilder{_world};
    b2Body* body = bodyBuilder.type(b2BodyType::b2_staticBody)
      .position(x + w / 2, y + h / 2, kPpm)
      .buildBody();

    bodyBuilder.newRectangleFixture(w / 2, h / 2, kPpm)
      .categoryBits(categoryBits)
      .setSensor(!collidable)
      .friction(defaultFriction)
      .buildFixture();

    bodies.emplace_back(body);
  }

  return bodies;
}

list<b2Body*> GameMap::createPolylines(const string& layerName, const short categoryBits,
                                       const bool collidable, const float defaultFriction) {
  list<b2Body*> bodies;
  float scaleFactor = Director::getInstance()->getContentScaleFactor();

  for (const auto& lineObj : getObjects(layerName)) {
    const auto& valMap = lineObj.asValueMap();
    float xRef = valMap.at("x").asFloat();
    float yRef = valMap.at("y").asFloat();

    const auto& valVec = valMap.at("polylinePoints").asValueVector();
    vector<b2Vec2> vertices(valVec.size());
    for (size_t i = 0; i < valVec.size(); i++) {
      float x = valVec.at(i).asValueMap().at("x").asFloat() / scaleFactor;
      float y = valVec.at(i).asValueMap().at("y").asFloat() / scaleFactor;
      vertices[i] = {xRef + x, yRef - y};
    }

    // Dynamically calculate ground friction
    float friction = defaultFriction;
    const optional<float> slope = math_util::getSlope(vertices[0], vertices[1]);
    if (slope.has_value()) {
      const float degree = math_util::rad2Deg(std::atan(*slope));
      if (degree >= 30.0f || degree <= -30.0f) {
        friction = 0.2f;
      }
    }

    B2BodyBuilder bodyBuilder{_world};
    b2Body* body = bodyBuilder.type(b2BodyType::b2_staticBody)
      .position(0, 0, kPpm)
      .buildBody();

    bodyBuilder.newPolylineFixture(vertices.data(), valVec.size(), kPpm)
      .categoryBits(categoryBits)
      .setSensor(!collidable)
      .friction(friction)
      .buildFixture();

    bodies.emplace_back(body);
  }

  return bodies;
}

void GameMap::createTriggers() {
  int triggerId{};
  for (const auto& rectObj : getObjects("Trigger")) {
    const auto& valMap = rectObj.asValueMap();
    float x = valMap.at("x").asFloat();
    float y = valMap.at("y").asFloat();
    float w = valMap.at("width").asFloat();
    float h = valMap.at("height").asFloat();
    vector<string> cmds = string_util::parseCmds(valMap.at("cmds").asString());
    bool canBeTriggeredOnlyOnce = valMap.at("canBeTriggeredOnlyOnce").asBool();
    bool canBeTriggeredOnlyByPlayer = valMap.at("canBeTriggeredOnlyByPlayer").asBool();
    bool shouldBlockWhileInBossFight = valMap.at("shouldBlockWhileInBossFight").asBool();
    string controlHintText = valMap.at("controlHintText").asString();
    int damage = valMap.at("damage").asInt();

    B2BodyBuilder bodyBuilder(_world);
    b2Body* body = bodyBuilder.type(b2BodyType::b2_staticBody)
      .position(x + w / 2, y + h / 2, kPpm)
      .buildBody();

    auto trigger = std::make_unique<GameMap::Trigger>(
        _tmxTiledMapFilePath, triggerId,
        cmds, canBeTriggeredOnlyOnce, canBeTriggeredOnlyByPlayer,
        shouldBlockWhileInBossFight, controlHintText, damage, body);
    auto trigger_raw_ptr = trigger.get();
    _triggers.emplace_back(std::move(trigger));

    bodyBuilder.newRectangleFixture(w / 2, h / 2, kPpm)
      .categoryBits(category_bits::kInteractable)
      .setSensor(true)
      .friction(0)
      .setUserData(trigger_raw_ptr)
      .buildFixture();

    triggerId++;
  }
}

void GameMap::createPortals() {
  int portalId{};
  for (const auto& rectObj : getObjects("Portal")) {
    const auto& valMap = rectObj.asValueMap();
    const float x = valMap.at("x").asFloat();
    const float y = valMap.at("y").asFloat();
    const float w = valMap.at("width").asFloat();
    const float h = valMap.at("height").asFloat();
    string destTmxMapFilePath = valMap.at("destMap").asString();
    int destPortalId = valMap.at("destPortalID").asInt();
    bool willInteractOnContact = valMap.at("willInteractOnContact").asBool();
    bool shouldAdjustOffsetX = valMap.at("shouldAdjustOffsetX").asBool();
    bool isLocked = valMap.at("isLocked").asBool();

    B2BodyBuilder bodyBuilder(_world);
    b2Body* body = bodyBuilder.type(b2BodyType::b2_staticBody)
      .position(x + w / 2, y + h / 2, kPpm)
      .buildBody();

    auto portal = std::make_unique<GameMap::Portal>(
        _tmxTiledMapFilePath, portalId, destTmxMapFilePath, destPortalId,
        willInteractOnContact, shouldAdjustOffsetX, isLocked,
        w / kPpm, h / kPpm, body);
    auto portal_raw_ptr = portal.get();
    _portals.emplace_back(std::move(portal));

    bodyBuilder.newRectangleFixture(w / 2, h / 2, kPpm)
      .categoryBits(category_bits::kPortal)
      .setSensor(true)
      .friction(0)
      .setUserData(portal_raw_ptr)
      .buildFixture();

    portalId++;
  }
}

void GameMap::createNpcs() {
  auto gmMgr = SceneManager::the().getCurrentScene<GameScene>()->getGameMapManager();
  auto inGameTime = SceneManager::the().getCurrentScene<GameScene>()->getInGameTime();

  for (const auto& rectObj : getObjects("Npcs")) {
    const auto& valMap = rectObj.asValueMap();
    const float x = valMap.at("x").asFloat();
    const float y = valMap.at("y").asFloat();
    const string npcJsonFilePath = valMap.at("json").asString();

    if (!gmMgr->isNpcAllowedToSpawn(npcJsonFilePath)) {
      continue;
    }

    auto npc = std::make_shared<Npc>(npcJsonFilePath);

    const bool shouldShowDuringDawn = ax_util::extract<bool>(valMap, "shouldShowDuringDawn", true);
    const bool shouldShowDuringDay = ax_util::extract<bool>(valMap, "shouldShowDuringDay", true);
    const bool shouldShowDuringDusk = ax_util::extract<bool>(valMap, "shouldShowDuringDusk", true);
    const bool shouldShowDuringNight = ax_util::extract<bool>(valMap, "shouldShowDuringNight", true);
    npc->setShowDuringDawn(shouldShowDuringDawn);
    npc->setShowDuringDay(shouldShowDuringDay);
    npc->setShowDuringDusk(shouldShowDuringDusk);
    npc->setShowDuringNight(shouldShowDuringNight);

    const optional<bool> isFacingRight = ax_util::extract<bool>(valMap, "isFacingRight");
    if (isFacingRight.has_value()) {
      npc->setFacingRight(isFacingRight.value());
    }

    showDynamicActor(std::move(npc), x, y);
  }

  auto player = gmMgr->getPlayer();
  if (!player) {
    return;
  }

  for (const auto& p : player->getParty()->getWaitingMembersLocationInfos()) {
    const string& characterJsonFilePath = p.first;
    const auto& waitLoc = p.second;

    if (waitLoc.tmxMapFilePath == _tmxTiledMapFilePath) {
      auto member = player->getParty()->getMember(characterJsonFilePath);
      if (!member) {
        VGLOG(LOG_ERR, "Character [%s] is not in the player's party.", characterJsonFilePath.c_str());
        continue;
      }
      member->showOnMap(waitLoc.x * kPpm, waitLoc.y * kPpm);
    }
  }
}

void GameMap::createChests() {
  ax::ValueVector objects = getObjects("Chest");
  for (int i = 0; i < objects.size(); i++) {
    const auto& valMap = objects[i].asValueMap();
    float x = valMap.at("x").asFloat();
    float y = valMap.at("y").asFloat();
    string items = valMap.at("items").asString();

    auto chest = std::make_shared<Chest>(_tmxTiledMapFilePath, i, items);
    showDynamicActor(std::move(chest), x, y);
  }
}

void GameMap::createLightSources() {
  if (const Value ambientLightLevelDay = _tmxTiledMap->getProperty("ambientLightLevelDay"); !ambientLightLevelDay.isNull()) {
    _ambientLightLevelDay = ambientLightLevelDay.asFloat();
  }
  if (const Value ambientLightLevelNight = _tmxTiledMap->getProperty("ambientLightLevelNight"); !ambientLightLevelNight.isNull()) {
    _ambientLightLevelNight = ambientLightLevelNight.asFloat();
  }

  auto gmMgr = SceneManager::the().getCurrentScene<GameScene>()->getGameMapManager();
  ax::ValueVector objects = getObjects("LightSources");
  for (int i = 0; i < objects.size(); i++) {
    const auto& valMap = objects[i].asValueMap();
    const float x = valMap.at("x").asFloat();
    const float y = valMap.at("y").asFloat();

    _lighting->addLightSource(x, y);
  }
}

void GameMap::createAnimatedObjects() {
  ax::ValueVector objects = getObjects("AnimatedObjects");
  for (int i = 0; i < objects.size(); i++) {
    const auto& valMap = objects[i].asValueMap();
    const float x = valMap.at("x").asFloat();
    const float y = valMap.at("y").asFloat();
    const string textureResDir = valMap.at("textureResDir").asString();
    const string framesName = valMap.at("framesName").asString();
    const float frameInterval = valMap.at("frameInterval").asFloat();
    const float flipped = valMap.contains("flipped") ? valMap.at("flipped").asBool() : false;
    const float zOrder = valMap.contains("zOrder") ? valMap.at("zOrder").asInt() : z_order::kStaticObjects;

    auto staticObject = std::make_shared<StaticObject>(textureResDir, framesName, frameInterval, flipped, zOrder);
    showStaticActor(std::move(staticObject), x, y);
  }
}

void GameMap::createParallaxBackground() {
  const Value bgDirPathProperty = _tmxTiledMap->getProperty("parallaxBackground");
  if (bgDirPathProperty.isNull()) {
    VGLOG(LOG_INFO, "Failed to extract parallaxBackground property from tmx, skip creating parallax bg.");
    return;
  }

  const fs::path bgDirPath{bgDirPathProperty.asString()};
  const Value bgScaleProperty = _tmxTiledMap->getProperty("parallaxBackgroundScale");
  const float bgScale = bgScaleProperty.isNull() ? 1.0f : bgScaleProperty.asFloat();
  if (!_parallaxBackground->load(bgDirPath, bgScale)) {
    VGLOG(LOG_ERR, "Failed to load parallax background from dir: [%s].", bgDirPath.c_str());
    return;
  }

  auto gmMgr = SceneManager::the().getCurrentScene<GameScene>()->getGameMapManager();
  ax_util::addChildWithParentCameraMask(gmMgr->getParallaxLayer(), _parallaxBackground->getParallaxNode());
}

GameMap::Trigger::Trigger(const string& tmxMapFilePath,
                          const int triggerId,
                          const vector<string>& cmds,
                          const bool canBeTriggeredOnlyOnce,
                          const bool canBeTriggeredOnlyByPlayer,
                          const bool shouldBlockWhileInBossFight,
                          const string& controlHintText,
                          const int damage,
                          b2Body* body)
    : _tmxMapFilePath{tmxMapFilePath},
      _triggerId{triggerId},
      _cmds{cmds},
      _canBeTriggeredOnlyOnce{canBeTriggeredOnlyOnce},
      _canBeTriggeredOnlyByPlayer{canBeTriggeredOnlyByPlayer},
      _shouldBlockWhileInBossFight{shouldBlockWhileInBossFight},
      _controlHintText{controlHintText},
      _damage{damage},
      _body{body} {}

GameMap::Trigger::~Trigger() {
  _body->GetWorld()->DestroyBody(_body);
}

void GameMap::Trigger::onInteract(Character* user) {
  auto gmMgr = SceneManager::the().getCurrentScene<GameScene>()->getGameMapManager();
  if (_canBeTriggeredOnlyOnce && (_hasTriggered || gmMgr->hasActivatedTrigger(_tmxMapFilePath, _triggerId))) {
    return;
  }

  if (_canBeTriggeredOnlyByPlayer && !dynamic_cast<Player*>(user)) {
    return;
  }

  _hasTriggered = true;
  if (_canBeTriggeredOnlyOnce) {
    gmMgr->setTriggerActivated(_tmxMapFilePath, _triggerId);
  }

  auto console = SceneManager::the().getCurrentScene<GameScene>()->getConsole();
  for (const auto& cmd : _cmds) {
    console->executeCmd(cmd);
  }
}

bool GameMap::Trigger::willInteractOnContact() const {
  // If a control hint text is defined, then this trigger will not
  // automatically activate on contact.
  return _controlHintText.empty();
}

void GameMap::Trigger::onBossFightBegin() {
  if (_shouldBlockWhileInBossFight) {
    for (b2Fixture* fixture = _body->GetFixtureList(); fixture; fixture = fixture->GetNext()) {
      fixture->SetSensor(false);
    }
  }
}

void GameMap::Trigger::onBossFightEnd() {
  if (_shouldBlockWhileInBossFight) {
    for (b2Fixture* fixture = _body->GetFixtureList(); fixture; fixture = fixture->GetNext()) {
      fixture->SetSensor(true);
    }
  }
}

void GameMap::Trigger::showHintUI() {
  if (_controlHintText.empty()) {
    return;
  }

  const Color4B textColor{colorscheme::kWhite};
  auto controlHints = SceneManager::the().getCurrentScene<GameScene>()->getControlHints();
  controlHints->insert({EventKeyboard::KeyCode::KEY_CAPITAL_E}, _controlHintText, textColor);
}

void GameMap::Trigger::hideHintUI() {
  if (_controlHintText.empty()) {
    return;
  }

  auto controlHints = SceneManager::the().getCurrentScene<GameScene>()->getControlHints();
  controlHints->remove({EventKeyboard::KeyCode::KEY_CAPITAL_E});
}

GameMap::Portal::Portal(const string& tmxMapFilePath, const int portalId,
                        const string& destTmxMapFilePath, const int destPortalId,
                        const bool willInteractOnContact, const bool shouldAdjustOffsetX, const bool isLocked,
                        const float width, const float height, b2Body* body)
    : _tmxMapFilePath{tmxMapFilePath},
      _portalId{portalId},
      _destTmxMapFilePath{destTmxMapFilePath},
      _destPortalId{destPortalId},
      _willInteractOnContact{willInteractOnContact},
      _shouldAdjustOffsetX{shouldAdjustOffsetX},
      _isLocked{isLocked},
      _width{width},
      _height{height},
      _body{body} {
  auto gmMgr = SceneManager::the().getCurrentScene<GameScene>()->getGameMapManager();
  constexpr auto kType = OpenableObjectType::PORTAL;
  if (gmMgr->hasSavedOpenedClosedState(destTmxMapFilePath, kType, destPortalId)) {
    _isLocked = !gmMgr->isOpened(destTmxMapFilePath, kType, destPortalId);
  }
}

GameMap::Portal::~Portal() {
  _body->GetWorld()->DestroyBody(_body);
}

void GameMap::Portal::onInteract(Character* user) {
  maybeUnlockPortalAs(user);

  if (_isLocked) {
    return;
  }

  const int destPortalId = _destPortalId;
  const float offsetXPercentage = _shouldAdjustOffsetX ? (user->getBody()->GetPosition().x - _body->GetPosition().x) / _width : 0;

  auto afterLoadingGameMap = [user, destPortalId, offsetXPercentage](const GameMap* newGameMap) {
    const auto& portalPos = newGameMap->_portals[destPortalId]->_body->GetPosition();
    const float x = portalPos.x + newGameMap->_portals[destPortalId]->_width * offsetXPercentage;
    const float y = portalPos.y;
    user->setPosition(x, y);

    for (auto ally : user->getAllies()) {
      if (!ally->isWaitingForPartyLeader()) {
        ally->setPosition(x, y);
      } else if (newGameMap->getTmxTiledMapFilePath() != ally->getParty()->getWaitingMemberLocationInfo(
                 ally->getCharacterProfile().jsonFilePath)->tmxMapFilePath) {
        ally->removeFromMap();
      }
    }
  };

  auto gmMgr = SceneManager::the().getCurrentScene<GameScene>()->getGameMapManager();
  gmMgr->loadGameMap(_destTmxMapFilePath, afterLoadingGameMap);
}

bool GameMap::Portal::willInteractOnContact() const {
  return _willInteractOnContact;
}

void GameMap::Portal::showHintUI() {
  string text = "Open";
  Color4B textColor = colorscheme::kWhite;

  auto gmMgr = SceneManager::the().getCurrentScene<GameScene>()->getGameMapManager();
  if (_isLocked && !canBeUnlockedBy(gmMgr->getPlayer())) {
    text += " (Locked)";
    textColor = colorscheme::kRed;
  }

  auto controlHints = SceneManager::the().getCurrentScene<GameScene>()->getControlHints();
  controlHints->insert({EventKeyboard::KeyCode::KEY_CAPITAL_F}, text, textColor);
}

void GameMap::Portal::hideHintUI() {
  auto controlHints = SceneManager::the().getCurrentScene<GameScene>()->getControlHints();
  controlHints->remove({EventKeyboard::KeyCode::KEY_CAPITAL_F});
}

void GameMap::Portal::createHintBubbleFx() {
  if (_hintBubbleFxSprite) {
    removeHintBubbleFx();
  }

  auto fxMgr = SceneManager::the().getCurrentScene<GameScene>()->getFxManager();
  _hintBubbleFxSprite = fxMgr->createHintBubbleFx(_body, "portal_available");
}

void GameMap::Portal::removeHintBubbleFx() {
  if (!_hintBubbleFxSprite) {
    return;
  }

  auto fxMgr = SceneManager::the().getCurrentScene<GameScene>()->getFxManager();
  fxMgr->removeFx(_hintBubbleFxSprite);
  _hintBubbleFxSprite = nullptr;
}

void GameMap::Portal::maybeUnlockPortalAs(Character* user) {
  if (!_isLocked) {
    return;
  }

  auto notifications = SceneManager::the().getCurrentScene<GameScene>()->getNotifications();
  if (!canBeUnlockedBy(user)) {
    notifications->show("This door is locked.");
    Audio::the().playSfx(kSfxDoorLocked);
  } else {
    notifications->show("Door unlocked.");
    Audio::the().playSfx(kSfxDoorUnlocked);
    unlock();
  }
}

bool GameMap::Portal::canBeUnlockedBy(Character* user) const {
  auto gmMgr = SceneManager::the().getCurrentScene<GameScene>()->getGameMapManager();
  auto gameMap = gmMgr->getGameMap();
  const auto& miscItems = user->getInventory()[Item::Type::MISC];

  return std::find_if(miscItems.begin(),
                      miscItems.end(),
                      [gameMap, this](const Item* item) {
                          const Key* key = dynamic_cast<const Key*>(item);
                          return key &&
                            key->getKeyProfile().targetTmxFilePath == gameMap->_tmxTiledMapFilePath &&
                            key->getKeyProfile().targetPortalId == getPortalId();
                      }) != miscItems.end();
}

void GameMap::Portal::lock() {
  _isLocked = true;
  saveLockUnlockState();
}

void GameMap::Portal::unlock() {
  _isLocked = false;
  saveLockUnlockState();
}

void GameMap::Portal::saveLockUnlockState() const {
  auto gmMgr = SceneManager::the().getCurrentScene<GameScene>()->getGameMapManager();
  gmMgr->setOpened(_tmxMapFilePath, OpenableObjectType::PORTAL, _portalId, !_isLocked);
}

int GameMap::Portal::getPortalId() const {
  auto gmMgr = SceneManager::the().getCurrentScene<GameScene>()->getGameMapManager();
  const auto& portals = gmMgr->getGameMap()->_portals;

  for (size_t i = 0; i < portals.size(); i++) {
    if (portals.at(i).get() == this) {
      return static_cast<int>(i);
    }
  }

  VGLOG(LOG_ERR, "Unable to find the portal ID of: %p", this);
  return -1;
}

}  // namespace requiem

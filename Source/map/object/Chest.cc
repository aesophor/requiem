// Copyright (c) 2018-2026 Marco Wang <m.aesophor@gmail.com>. All rights reserved.

#include "Chest.h"

#include "Assets.h"
#include "Audio.h"
#include "Constants.h"
#include "scene/GameScene.h"
#include "scene/SceneManager.h"
#include "util/AxUtil.h"
#include "util/B2BodyBuilder.h"
#include "util/JsonUtil.h"
#include "util/StringUtil.h"

using namespace std;
using namespace requiem::assets;
using namespace requiem::category_bits;
USING_NS_AX;

namespace requiem {

namespace {

constexpr int kChestNumAnimations = 0;
constexpr int kChestNumFixtures = 2;

constexpr auto kItemCategoryBits = kInteractable;
constexpr auto kItemMaskBits = kGround | kPlatform | kWall;

}  // namespace

Chest::Chest(const string& tmxMapFilePath,
             const int chestId,
             const string& content)
    : DynamicActor{kChestNumAnimations, kChestNumFixtures},
      _tmxMapFilePath{tmxMapFilePath},
      _chestId{chestId},
      _content{parseContent(content)} {
  constexpr auto kType = GameMap::OpenableObjectType::CHEST;
  auto gmMgr = SceneManager::the().getCurrentScene<GameScene>()->getGameMapManager();
  _isOpened = gmMgr->isOpened(tmxMapFilePath, kType, chestId);
}

bool Chest::showOnMap(float x, float y) {
  if (_isShownOnMap) {
    return false;
  }

  _isShownOnMap = true;

  defineBody(b2BodyType::b2_dynamicBody, x, y,
             kItemCategoryBits,
             kItemMaskBits);

  if (!_isOpened) {
    _bodySprite = Sprite::create("Texture/interactable_object/chest/chest_close.png");
  } else {
    _bodySprite = Sprite::create("Texture/interactable_object/chest/chest_open.png");
  }
  _bodySprite->getTexture()->setAliasTexParameters();
  _node->addChild(_bodySprite, z_order::kChest);

  auto gmMgr = SceneManager::the().getCurrentScene<GameScene>()->getGameMapManager();
  ax_util::addChildWithParentCameraMask(gmMgr->getLayer(), _node, z_order::kChest);

  return true;
}

void Chest::defineBody(b2BodyType bodyType,
                       float x,
                       float y,
                       short categoryBits,
                       short maskBits) {
  auto gmMgr = SceneManager::the().getCurrentScene<GameScene>()->getGameMapManager();
  B2BodyBuilder bodyBuilder(gmMgr->getWorld());

  _body = bodyBuilder.type(bodyType)
    .position(x, y, kPpm)
    .buildBody();

  bodyBuilder.newRectangleFixture(16 / 2, 16 / 2, kPpm)
    .categoryBits(categoryBits)
    .maskBits(maskBits)
    .setUserData(this)
    .buildFixture();

  bodyBuilder.newRectangleFixture(16 / 2, 16 / 2, kPpm)
    .categoryBits(kInteractable)
    .maskBits(kFeet)
    .setSensor(true)
    .setUserData(static_cast<Interactable*>(this))
    .buildFixture();
}

void Chest::onInteract(Character*) {
  if (_isOpened) {
    return;
  }

  _isOpened = true;
  _bodySprite->setTexture("Texture/interactable_object/chest/chest_open.png");
  _bodySprite->getTexture()->setAliasTexParameters();

  auto gmMgr = SceneManager::the().getCurrentScene<GameScene>()->getGameMapManager();
  for (const auto& [itemJson, amount]: _content) {
    float x = _body->GetPosition().x;
    float y = _body->GetPosition().y;
    gmMgr->getGameMap()->createItem(itemJson, x * kPpm, y * kPpm, amount);
  }

  constexpr auto kType = GameMap::OpenableObjectType::CHEST;
  gmMgr->setOpened(_tmxMapFilePath, kType, _chestId, true);

  Audio::the().playSfx(kSfxChestOpened);
}

void Chest::showHintUI() {
  if (_isOpened) {
    return;
  }

  createHintBubbleFx();

  auto controlHints = SceneManager::the().getCurrentScene<GameScene>()->getControlHints();
  controlHints->insert({EventKeyboard::KeyCode::KEY_CAPITAL_E}, "Open");
}

void Chest::hideHintUI() {
  removeHintBubbleFx();

  auto controlHints = SceneManager::the().getCurrentScene<GameScene>()->getControlHints();
  controlHints->remove({EventKeyboard::KeyCode::KEY_CAPITAL_E});
}

void Chest::createHintBubbleFx() {
  if (_hintBubbleFxSprite) {
    removeHintBubbleFx();
  }

  if (_isOpened) {
    return;
  }

  auto fxMgr = SceneManager::the().getCurrentScene<GameScene>()->getFxManager();
  _hintBubbleFxSprite = fxMgr->createHintBubbleFx(_body, "dialogue_available");
}

void Chest::removeHintBubbleFx() {
  if (!_hintBubbleFxSprite) {
    return;
  }

  auto fxMgr = SceneManager::the().getCurrentScene<GameScene>()->getFxManager();
  fxMgr->removeFx(_hintBubbleFxSprite);
  _hintBubbleFxSprite = nullptr;
}

vector<pair<string, int>> Chest::parseContent(const string& contentStr) {
  vector<pair<string, int>> content;

  for (const auto& s : string_util::split(contentStr, '\n')) {
    const vector<string> tokens = string_util::split(s);
    if (tokens.size() < 2) {
      VGLOG(LOG_ERR, "Failed to parse line [%s]", s.c_str());
      continue;
    }

    const string& itemJson = tokens[0];
    int amount = 1;
    try {
      amount = std::stoi(tokens[1]);
    } catch (const exception& ex) {
      VGLOG(LOG_ERR, "Failed to parse out of range amount: [%s], err [%s]", tokens[1].c_str(), ex.what());
    } catch (...) {
      VGLOG(LOG_ERR, "Failed to parse [%s], uncaught exception.", tokens[1].c_str());
    }

    content.emplace_back(itemJson, amount);
  }

  return content;
}

}  // namespace requiem

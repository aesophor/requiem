// Copyright (c) 2018-2025 Marco Wang <m.aesophor@gmail.com>. All rights reserved.

#ifndef REQUIEM_MAP_GAME_MAP_H_
#define REQUIEM_MAP_GAME_MAP_H_

#include <list>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <axmol.h>

#include <box2d/box2d.h>

#include "DynamicActor.h"
#include "gameplay/InGameTime.h"
#include "Interactable.h"
#include "item/Item.h"
#include "map/Lighting.h"
#include "map/ParallaxBackground.h"
#include "map/PathFinder.h"
#include "util/Logger.h"

namespace requiem {

class Character;
class Player;

class GameMap final {
  friend class GameMapManager;
  friend class GameState;

 public:
  enum class PhysicalLayer {
    GROUND,
    WALL,
    PLATFORM,
    PivotMarker,
    CliffMarker,
    SIZE
  };

  enum class OpenableObjectType {
    PORTAL,
    CHEST,
    SIZE
  };

  class Trigger final : public Interactable {
   public:
    Trigger(const std::string& tmxMapFilePath,
            const int triggerId,
            const std::vector<std::string>& cmds,
            const bool canBeTriggeredOnlyOnce,
            const bool canBeTriggeredOnlyByPlayer,
            const bool shouldBlockWhileInBossFight,
            const std::string& controlHintText,
            const int damage,
            b2Body* body);
    virtual ~Trigger();

    // Executes certain commands via ui/console/Console.cc
    // when the player's body collides with `this->_body`
    virtual void onInteract(Character* user) override;  // Interactable
    virtual bool willInteractOnContact() const override;  // Interactable
    virtual void showHintUI() override;  // Interactable
    virtual void hideHintUI() override;  // Interactable

    void onBossFightBegin();
    void onBossFightEnd();

    inline bool canBeTriggeredOnlyOnce() const { return _canBeTriggeredOnlyOnce; }
    inline bool canBeTriggeredOnlyByPlayer() const { return _canBeTriggeredOnlyByPlayer; }
    inline const std::string& getControlHint() const { return _controlHintText; }
    inline int getDamage() const { return _damage; }
    inline bool hasTriggered() const { return _hasTriggered; }
    inline void setTriggered(bool triggered) { _hasTriggered = triggered; }
    inline b2Body* getBody() const { return _body; }

   protected:
    virtual void createHintBubbleFx() override {}  // Interactable
    virtual void removeHintBubbleFx() override {}  // Interactable

    const std::string _tmxMapFilePath;
    const int _triggerId;
    std::vector<std::string> _cmds;
    bool _canBeTriggeredOnlyOnce{};
    bool _canBeTriggeredOnlyByPlayer{};
    bool _shouldBlockWhileInBossFight{};
    std::string _controlHintText{};
    int _damage{};

    bool _hasTriggered{};
    b2Body* _body{};
  };

  class Portal final : public Interactable {
  public:
    Portal(const std::string& tmxMapFilePath, const int portalId,
           const std::string& targetTmxMapFilePath, const int targetPortalId,
           const bool willInteractOnContact, const bool shouldAdjustOffsetX, const bool isLocked,
           const float width, const float height, b2Body* body);
    virtual ~Portal();

    virtual void onInteract(Character* user) override;  // Interactable
    virtual bool willInteractOnContact() const override;  // Interactable
    virtual void showHintUI() override;  // Interactable
    virtual void hideHintUI() override;  // Interactable

    bool canBeUnlockedBy(Character* user) const;
    inline bool isLocked() const { return _isLocked; }
    void lock();
    void unlock();

    inline const std::string& getDestTmxMapFilePath() const { return _destTmxMapFilePath; }
    inline int getDestPortalId() const { return _destPortalId; }
    inline const float getWidth() const { return _width; }
    inline const float getHeight() const { return _height; }
    inline b2Body* getBody() const { return _body; }

  protected:
    virtual void createHintBubbleFx() override;  // Interactable
    virtual void removeHintBubbleFx() override;  // Interactable

    // Try to unlock the portal as `user`.
    // If the portal is already unlocked, then this is a no-op.
    void maybeUnlockPortalAs(Character* user);

    // Save the current portal's lock/unlock state in `_allPortalStates`.
    void saveLockUnlockState() const;
    int getPortalId() const;

    const std::string _tmxMapFilePath;
    const int _portalId;
    const std::string _destTmxMapFilePath;  // new (destination) .tmx filepath
    const int _destPortalId{};  // the portal id in the new (destination) map
    bool _willInteractOnContact{};  // interact with the portal on contact?
    bool _shouldAdjustOffsetX{};
    bool _isLocked{};
    const float _width{};
    const float _height{};
    b2Body* _body{};
    ax::Sprite* _hintBubbleFxSprite{};
  };

  GameMap(b2World* world, Lighting* lighting, const std::string& tmxMapFilePath);
  ~GameMap();

  void update(const float delta);

  void createObjects();
  std::unique_ptr<Player> createPlayer() const;
  Item* createItem(const std::string& itemJson, float x, float y, int amount=1);
  std::vector<Item*> createItems(const std::vector<std::pair<std::string, int>>& itemJsons, float x, float y);

  template <typename ReturnType = StaticActor>
  ReturnType* showStaticActor(std::shared_ptr<StaticActor> actor, float x, float y);

  template <typename ReturnType = StaticActor>
  std::shared_ptr<ReturnType> removeStaticActor(StaticActor* actor);

  template <typename ReturnType = DynamicActor>
  ReturnType* showDynamicActor(std::shared_ptr<DynamicActor> actor, float x, float y);

  template <typename ReturnType = DynamicActor>
  std::shared_ptr<ReturnType> removeDynamicActor(DynamicActor* actor);

  bool beginBossFight(const std::string& targetNpcJsonFilePath,
                      const std::string& bgmFilePath,
                      const bool isGameOverOnPlayerKilled,
                      std::vector<std::string>&& execOnBegin,
                      std::vector<std::string>&& execOnPlayerKilled);
  void endBossFight(const bool isPlayerKilled);

  inline ax::TMXTiledMap* getTmxTiledMap() const { return _tmxTiledMap; }
  inline const std::string& getTmxTiledMapFilePath() const { return _tmxTiledMapFilePath; }
  inline const std::string& getBgmFilePath() const { return _bgmFilePath; }
  inline const std::string& getLocationName() const { return _locationName; }
  inline float getAmbientLightLevelDay() const { return _ambientLightLevelDay; }
  inline float getAmbientLightLevelNight() const { return _ambientLightLevelNight; }
  inline bool isInBossFight() const { return _isInBossFight; }
  inline bool isGameOverOnPlayerKilled() const { return _isGameOverOnPlayerKilled; }
  inline const std::unordered_set<std::shared_ptr<DynamicActor>>& getDynamicActors() const { return _dynamicActors; }
  inline const std::list<b2Body*> getTmxTiledMapPlatformBodies() const { return _tmxTiledMapPlatformBodies; }
  inline const std::vector<std::unique_ptr<GameMap::Portal>>& getPortals() const { return _portals; };
  inline ParallaxBackground& getParallaxBackground() { return *_parallaxBackground; }
  inline PathFinder& getPathFinder() { return *_pathFinder; }

  float getWidth() const;
  float getHeight() const;

 private:
  ax::ValueVector getObjects(const std::string& layerName);
  std::list<b2Body*> createRectangles(const std::string& layerName, const short categoryBits,
                                      const bool collidable, const float defaultFriction);
  std::list<b2Body*> createPolylines(const std::string& layerName, const short categoryBits,
                                     const bool collidable, const float defaultFriction);

  void createTriggers();
  void createPortals();
  void createNpcs();
  void createChests();
  void createLightSources();
  void createAnimatedObjects();
  void createParallaxBackground();

  b2World* _world{};
  Lighting* _lighting{};
  ax::TMXTiledMap* _tmxTiledMap{};
  std::string _tmxTiledMapFilePath;
  std::string _bgmFilePath;
  std::string _locationName;
  float _ambientLightLevelDay{1.0f};
  float _ambientLightLevelNight{0.3f};
  bool _isInBossFight{};
  bool _isGameOverOnPlayerKilled{true};
  std::vector<std::string> _execOnPlayerKilled;
  std::list<b2Body*> _tmxTiledMapBodies;
  std::list<b2Body*> _tmxTiledMapPlatformBodies;
  std::unordered_set<std::shared_ptr<StaticActor>> _staticActors;
  std::unordered_set<std::shared_ptr<DynamicActor>> _dynamicActors;
  std::vector<std::unique_ptr<GameMap::Trigger>> _triggers;
  std::vector<std::unique_ptr<GameMap::Portal>> _portals;
  std::unique_ptr<ParallaxBackground> _parallaxBackground;
  std::unique_ptr<PathFinder> _pathFinder;
};

template <typename ReturnType>
ReturnType* GameMap::showStaticActor(std::shared_ptr<StaticActor> actor, float x, float y) {
  ReturnType* shownActor = dynamic_cast<ReturnType*>(actor.get());

  std::shared_ptr<StaticActor> key{std::shared_ptr<StaticActor>(), actor.get()};
  if (_staticActors.contains(key)) {
    VGLOG(LOG_ERR, "This StaticActor is already being shown: %p", actor.get());
    return nullptr;
  }

  actor->showOnMap(x, y);
  _staticActors.insert(std::move(actor));
  return shownActor;
}

template <typename ReturnType>
std::shared_ptr<ReturnType> GameMap::removeStaticActor(StaticActor* actor) {
  std::shared_ptr<ReturnType> removedActor{nullptr};

  std::shared_ptr<StaticActor> key{std::shared_ptr<StaticActor>{}, actor};
  auto it = _staticActors.find(key);
  if (it == _staticActors.end()) {
    VGLOG(LOG_ERR, "This StaticActor has not yet been shown: %p", actor);
    return nullptr;
  }

  removedActor = std::move(std::dynamic_pointer_cast<ReturnType>(*it));
  removedActor->removeFromMap();
  _staticActors.erase(it);
  return removedActor;
}

template <typename ReturnType>
ReturnType* GameMap::showDynamicActor(std::shared_ptr<DynamicActor> actor, float x, float y) {
  ReturnType* shownActor = dynamic_cast<ReturnType*>(actor.get());

  std::shared_ptr<DynamicActor> key(std::shared_ptr<DynamicActor>(), actor.get());
  auto it = _dynamicActors.find(key);
  if (it != _dynamicActors.end()) {
    VGLOG(LOG_ERR, "This DynamicActor is already being shown: %p", actor.get());
    return nullptr;
  }

  actor->showOnMap(x, y);
  _dynamicActors.insert(std::move(actor));
  return shownActor;
}

template <typename ReturnType>
std::shared_ptr<ReturnType> GameMap::removeDynamicActor(DynamicActor* actor) {
  std::shared_ptr<ReturnType> removedActor(nullptr);

  std::shared_ptr<DynamicActor> key(std::shared_ptr<DynamicActor>(), actor);
  auto it = _dynamicActors.find(key);
  if (it == _dynamicActors.end()) {
    VGLOG(LOG_ERR, "This DynamicActor has not yet been shown: %p", actor);
    return nullptr;
  }

  removedActor = std::move(std::dynamic_pointer_cast<ReturnType>(*it));
  removedActor->removeFromMap();
  _dynamicActors.erase(it);
  return removedActor;
}

}  // namespace requiem

#endif  // REQUIEM_MAP_GAME_MAP_H_

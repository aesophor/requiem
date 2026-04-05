// Copyright (c) 2018-2025 Marco Wang <m.aesophor@gmail.com>. All rights reserved.

#ifndef REQUIEM_CHARACTER_CHARACTER_H_
#define REQUIEM_CHARACTER_CHARACTER_H_

#include <array>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <axmol.h>
#include <box2d/box2d.h>

#include "CallbackManager.h"
#include "DynamicActor.h"
#include "Importable.h"
#include "Interactable.h"
#include "character/Party.h"
#include "item/Item.h"
#include "item/Equipment.h"
#include "item/Consumable.h"
#include "map/GameMap.h"
#include "skill/Skill.h"
#include "util/ds/SetVector.h"

namespace requiem {

class ComboSystem;

class Character : public DynamicActor, public Importable {
  friend class GameState;

 public:
  enum State {
    IDLE,
    RUNNING,
    RUNNING_START,
    RUNNING_STOP,
    JUMPING,
    FALLING,
    FALLING_GETUP,
    CROUCHING,
    DODGING_BACKWARD,
    DODGING_FORWARD,
    ATTACKING,
    ATTACKING_UNARMED,
    ATTACKING_UNARMED_CROUCH,
    ATTACKING_UNARMED_MIDAIR,
    ATTACKING_CROUCH,
    ATTACKING_FORWARD,
    ATTACKING_MIDAIR,
    ATTACKING_MIDAIR_DOWNWARD,
    ATTACKING_UPWARD,
    SPELLCAST,
    SPELLCAST2,
    SPELLCAST3,
    BLOCKING,
    BLOCKING_HIT,
    INTRO,
    STUNNED,
    TAKE_DAMAGE,
    KILLED,
    STATE_SIZE,
    FORCE_UPDATE
  };

  enum Sfx {
    SFX_ATTACK_UNARMED,
    SFX_JUMP,
    SFX_INTRO,
    SFX_HURT,
    SFX_KILLED,
    SFX_SIZE,
  };

  using Inventory = std::array<SetVector<Item*>, Item::Type::SIZE>;
  using EquipmentSlots = std::array<Equipment*, Equipment::Type::SIZE>;
  using SkillBook = std::array<SetVector<Skill*>, Skill::Type::SIZE>;

  struct Profile final {
    Profile() = default;
    explicit Profile(const std::filesystem::path& jsonFilePath);
    void loadSpritesheetInfo(const std::filesystem::path& jsonFilePath);

    std::filesystem::path jsonFilePath;
    std::filesystem::path textureResDirPath;
    float spriteOffsetX;
    float spriteOffsetY;
    float spriteScaleX;
    float spriteScaleY;
    std::array<float, Character::State::STATE_SIZE> frameIntervals;
    std::vector<float> extraAttackFrameIntervals;
    std::array<std::filesystem::path, Character::Sfx::SFX_SIZE> sfxFilePaths;

    std::string name;
    int level;
    int exp;

    int fullHealth;
    int fullStamina;
    int fullMagicka;
    int health;
    int stamina;
    int magicka;

    int strength;
    int dexterity;
    int intelligence;
    int luck;

    int bodyWidth;
    int bodyHeight;
    float moveSpeed;
    float jumpHeight;
    bool canDoubleJump;

    float attackForce;
    float attackTime;
    float attackRange;
    float attackDelay;
    int baseMeleeDamage;
    int forwardAttackNumTimesInflictDamage{1};

    std::vector<std::string> defaultSkills;
    std::vector<std::pair<std::string, int>> defaultInventory;
  };

  // We have a vector of b2Fixtures (declared in DynamicActor abstract class).
  // e.g., to access the weapon fixture: _fixtures[FixtureType::WEAPON]
  enum FixtureType {
    BODY,  // used in combat (with WEAPON fixture)
    FEET,  // used for ground/platform collision detection
    WEAPON,  // used in combat (with BODY fixture)
    FIXTURE_SIZE
  };

  virtual ~Character() override = default;

  virtual bool showOnMap(float x, float y) override;  // DynamicActor
  virtual bool removeFromMap() override;  // DynamicActor
  virtual void update(const float delta) override;  // DynamicActor
  virtual void import(const std::filesystem::path& jsonFilePath) override;  // Importable
  virtual void replaceSpritesheet(const std::filesystem::path& jsonFilePath);

  virtual void onSetToKill();
  virtual void onKilled();
  virtual void onFallToGroundOrPlatform();
  virtual void onBodyContactWithEnemyBody(Character* enemy);
  virtual void onMeleeWeaponContactWithEnemyBody(Character* enemy);
  virtual void beforeMapChanged() {}

  virtual bool isMovementDisallowed() const;
  virtual bool isJumpingDownDisallowed() const;
  virtual bool isAttackingDisallowed() const;
  virtual bool isSkillActivationDisallowed() const;
  virtual void startRunning();
  virtual void stopRunning();
  virtual void moveLeft();
  virtual void moveRight();
  virtual void jump();
  virtual void doubleJump();
  virtual void jumpDown();
  virtual void crouch();
  virtual void getUpFromCrouching();
  virtual void getUpFromFalling();
  virtual void dodgeBackward();
  virtual void dodgeForward();
  virtual void teleportToTarget(Character* target);
  virtual void teleportToTarget(const b2Vec2& targetPos);
  virtual void enableAfterImageFx(const ax::Color3B& color);
  virtual void disableAfterImageFx();
  virtual void runIntroAnimation();

  virtual bool attack(const Character::State attackState = Character::State::ATTACKING,
                      const int numTimesInflictDamage = 1,
                      const float damageInflictionInterval = .2f);
  virtual bool activateSkill(Skill* rawSkill);
  virtual void knockBack(Character* target, float forceX, float forceY) const;
  virtual bool inflictDamage(Character* target, int damage);
  virtual bool inflictDamage(Character* target, int damage, const int numTimesINflictDamage, const float damageInflictionInterval);
  virtual bool receiveDamage(Character* source, int damage);
  virtual bool receiveDamage(int damage);
  virtual void lockOn(Character* target);

  virtual bool addItem(std::shared_ptr<Item> item, int amount=1);
  virtual bool removeItem(Item* item, int amount=1);
  virtual void useItem(Consumable* consumable);
  virtual void equip(Equipment* equipment, bool audio = true);
  virtual void unequip(Equipment::Type equipmentType, bool audio = true);
  virtual void pickupItem(Item* item);
  virtual void discardItem(Item* item, int amount);
  virtual void interact(Interactable* target);
  virtual void addExp(const int exp);
  virtual bool addSkill(std::shared_ptr<Skill> skill);
  virtual bool removeSkill(Skill* skill);
  virtual Skill* getSkill(const std::filesystem::path& skillJsonFilePath) const;
  virtual void resurrect();

  int getGoldBalance() const;
  void addGold(const int amount);
  void removeGold(const int amount);

  inline bool isFacingRight() const { return _isFacingRight; }
  bool isOnGround(const bool raycast = false) const;
  inline bool isJumping() const { return _isJumping; }
  inline bool isDoubleJumping() const { return _isDoubleJumping; }
  inline bool isOnPlatform() const { return _isOnPlatform; }
  inline bool isDodging() const { return _isDodgingBackward || _isDodgingForward; }
  inline bool isDodgingBackward() const { return _isDodgingBackward; }
  inline bool isDodgingForward() const { return _isDodgingForward; }
  inline bool hasDodgedMidair() const { return _hasDodgedMidair; }
  inline bool isAttacking() const { return _isAttacking || isAttackState(_currentState); }
  inline bool isUsingSkill() const { return _isUsingSkill; }
  inline bool isBlocking() const { return _isBlocking; }
  inline bool isHitWhileBlocking() const { return _isHitWhileBlocking; }
  inline bool isCrouching() const { return _isCrouching; }
  inline bool isInvincible() const { return _isInvincible; }
  inline bool isRunningIntroAnimation() const { return _isRunningIntroAnimation; }
  inline bool isStunned() const { return _isStunned; }
  inline bool isKilled() const { return _isKilled; }
  inline bool isSetToKill() const { return _isSetToKill; }
  inline bool isAfterImageFxEnabled() const { return _isAfterImageFxEnabled; }
  inline bool shouldRetainBodyIfKilled() const { return _shouldRetainBodyIfKilled; }

  inline void setFacingRight(bool facingRight) { _isFacingRight = facingRight; }
  inline void setOnGround(bool onGround) { _isOnGround = onGround; }
  inline void setJumping(bool jumping) { _isJumping = jumping; }
  inline void setDoubleJumping(bool doubleJumping) { _isDoubleJumping = doubleJumping; }
  inline void setOnPlatform(bool onPlatform) { _isOnPlatform = onPlatform; }
  inline void setDodgedMidair(bool dodgedMidair) { _hasDodgedMidair = dodgedMidair; }
  inline void setAttacking(bool attacking) { _isAttacking = attacking; }
  inline void setUsingSkill(bool usingSkill) { _isUsingSkill = usingSkill; }
  inline void setBlocking(bool blocking) { _isBlocking = blocking; }
  inline void setHitWhileBlocking(bool hitWhileBlocking) { _isHitWhileBlocking = hitWhileBlocking; }
  inline void setCrouching(bool crouching) { _isCrouching = crouching; }
  inline void setInvincible(bool invincible) { _isInvincible = invincible; }
  inline void setRunningIntroAnimation(bool runningIntroAnimation) { _isRunningIntroAnimation = runningIntroAnimation; }
  inline void setStunned(bool stunned) { _isStunned = stunned; }
  inline void setAfterImageFxEnabled(bool afterImageFxEnabled) { _isAfterImageFxEnabled = afterImageFxEnabled; }
  inline void setRetainBodyIfKilled(bool retainBodyIfKilled) { _shouldRetainBodyIfKilled = retainBodyIfKilled; }

  inline float getGroundAngle() const { return _groundAngle; }
  inline void setGroundAngle(float groundAngle) { _groundAngle = groundAngle; }

  inline void addOnKilledCallback(std::function<void ()>&& callback) { _onKilledCallbacks.emplace_back(std::move(callback)); }

  inline void resetAttackAnimationIdx() { _attackAnimationIdx = 0; }

  inline Character::Profile& getCharacterProfile() { return _characterProfile; }

  inline ComboSystem& getCombatSystem() { return *_comboSystem; }

  inline std::unordered_set<Character*>& getInRangeTargets() { return _inRangeTargets; }
  inline Character* getLockedOnTarget() const { return _lockedOnTarget; }
  inline void setLockedOnTarget(Character* target) { _lockedOnTarget = target; }
  inline bool isAlerted() const { return _isAlerted; }
  inline void setAlerted(bool alerted) { _isAlerted = alerted; }

  inline std::unordered_set<Item*>& getInRangeItems() { return _inRangeItems; }
  inline std::list<Interactable*>& getInRangeInteractables() {
    return _inRangeInteractables;
  }

  inline const Inventory& getInventory() const { return _inventory; }
  inline const EquipmentSlots& getEquipmentSlots() const { return _equipmentSlots; }
  int getItemAmount(const std::string& itemJsonFilePath) const;

  inline GameMap::Portal* getPortal() const { return _portal; }
  inline void setPortal(GameMap::Portal* portal) { _portal = portal; }

  inline SkillBook& getSkillBook() { return _skillBook; }
  std::shared_ptr<Skill> getActiveSkillInstance(Skill* skill) const;
  inline Skill* getCurrentlyUsedSkill() const { return _currentlyUsedSkill; }
  void removeActiveSkillInstance(Skill* skill);

  bool isWaitingForPartyLeader() const;
  std::unordered_set<Character*> getAllies() const;
  inline std::shared_ptr<Party> getParty() const { return _party; }
  inline void setParty(std::shared_ptr<Party> party) { _party = party; }

  int getDamageOutput() const;
  inline float getAnimationDuration(const Character::State state) const {
    return _bodyAnimations[state]->getDuration();
  }

 protected:
  static inline const std::array<std::string, Character::State::STATE_SIZE> _kCharacterStateStr{{
    "idle",
    "running",
    "running_start",
    "running_stop",
    "jumping",
    "falling",
    "falling_getup",
    "crouching",
    "dodging_backward",
    "dodging_forward",
    "attacking0",
    "attacking_unarmed",
    "attacking_unarmed_crouch",
    "attacking_unarmed_midair",
    "attacking_crouch",
    "attacking_forward",
    "attacking_midair",
    "attacking_midair_downward",
    "attacking_upward",
    "spellcast",
    "spellcast2",
    "spellcast3",
    "blocking",
    "blocking_hit",
    "intro",
    "stunned",
    "take_damage",
    "killed"
  }};

  static inline const std::array<std::string, Character::Sfx::SFX_SIZE> _kCharacterSfxStr{{
    "attack_unarmed",
    "jump",
    "intro",
    "hurt",
    "killed",
  }};

  static std::optional<Character::State> getCharacterState(const std::string& frameName);

  static constexpr bool isAttackState(const Character::State state) {
    return state == State::ATTACKING ||
           state == State::ATTACKING_UNARMED ||
           state == State::ATTACKING_UNARMED_CROUCH ||
           state == State::ATTACKING_UNARMED_MIDAIR ||
           state == State::ATTACKING_CROUCH ||
           state == State::ATTACKING_FORWARD ||
           state == State::ATTACKING_MIDAIR ||
           state == State::ATTACKING_MIDAIR_DOWNWARD ||
           state == State::ATTACKING_UPWARD;
  }

  explicit Character(const std::filesystem::path& jsonFilePath);

  virtual void regenHealth(int deltaHealth);
  virtual void regenMagicka(int deltaMagicka);
  virtual void regenStamina(int deltaStamina);

  virtual void defineBody(b2BodyType bodyType,
                          float x,
                          float y,
                          short bodyCategoryBits=0,
                          short bodyMaskBits=0,
                          short feetMaskBits=0,
                          short weaponMaskBits=0);
  virtual void redefineBodyFixture(short bodyCategoryBits = 0, short bodyMaskBits = 0);
  virtual void redefineFeetFixture(short feetMaskBits = 0);
  virtual void redefineWeaponFixture(short weaponMaskBits = 0);
  virtual void defineTexture(const std::filesystem::path& bodyTextureResDirPath, float x, float y);
  virtual void loadBodyAnimations(const std::filesystem::path& bodyTextureResDirPath);

  void moveImpl(const bool moveTowardsRight);
  void clampLinearVelocity();
  void avoidSlidingDownSlope();
  bool isTryingToMoveRecently(const float gracePeriod) const;
  void setTryingToMoveRecently();

  void dodgeImpl(const Character::State dodgeState, const float rushPowerX, bool& isDodgingFlag);

  bool receiveDamage(Character* source, int damage, float numSecCantMove);
  void cancelAttack();

  void createBodyAnimation(const Character::State state,
                           ax::Animation* fallbackAnimation);

  int getExtraAttackAnimationsCount() const;
  ax::Animation* getBodyAttackAnimation() const;
  inline bool hasUnarmedAttackAnimation() const {
    return _bodyAnimations[State::ATTACKING_UNARMED] != _bodyAnimations[State::ATTACKING];
  }

  void runAnimation(Character::State state, bool loop=true);
  void runAnimation(Character::State state, const std::function<void ()>& func) const;
  void runAnimation(const std::string& framesName, float interval);

  float getAttackAnimationDuration(const Character::State state) const;

  Character::State determineState() const;
  Character::State determineAttackState() const;
  void maybeOverrideCurrentStateWithAttackingMidairState();
  void maybeOverrideCurrentStateWithStopRunningState();
  const std::filesystem::path& getSfxFilePath(const Character::Sfx sfx) const {
    return _characterProfile.sfxFilePaths[sfx];
  }

  Item* getExistingItemObj(Item* item) const;

  // Characater data.
  Character::Profile _characterProfile;

  // Stats regen timer
  float _statsRegenTimer{};
  const int _baseRegenDeltaHealth{5};
  const int _baseRegenDeltaMagicka{5};
  const int _baseRegenDeltaStamina{5};

  // The following variables are used to determine the character's state
  // and run the corresponding animations. Please see Character::update()
  // for the logic.
  Character::State _currentState{State::IDLE};
  Character::State _previousState{State::IDLE};
  std::optional<Character::State> _overridingAttackState{std::nullopt};
  b2Vec2 _previousBodyVelocity{0.0f, 0.0f};
  b2Vec2 _killedPos{0.0f, 0.0f};

  bool _isFacingRight{true};
  bool _isStartRunning{};
  bool _isStopRunning{};
  bool _isOnGround{};
  bool _isJumpingDisallowed{};
  bool _isJumping{};
  bool _isDoubleJumping{};
  bool _isOnPlatform{};
  bool _isGettingUpFromFalling{};
  bool _isDodgingBackward{};
  bool _isDodgingForward{};
  bool _hasDodgedMidair{};
  bool _isAttacking{};
  bool _isUsingSkill{};
  bool _isBlocking{};
  bool _isHitWhileBlocking{};
  bool _isCrouching{};
  bool _isInvincible{};
  bool _isRunningIntroAnimation{};
  bool _isStunned{};
  bool _isTakingDamage{};
  bool _isTakingDamageFromTraps{};
  bool _isKilled{};
  bool _isSetToKill{};
  bool _isAfterImageFxEnabled{};
  bool _shouldRetainBodyIfKilled{};

  // Physics hacks against the "sliding down the slope" problem.
  bool _isTryingToMoveRecently{};
  uint64_t _lastMoveTimeMs{};

  float _groundAngle{};

  // Callbacks
  mutable std::mutex _cancelAttackCallbackIDsMutex;
  mutable std::mutex _inflictDamageCallbackIDsMutex;
  std::unordered_set<CallbackManager::CallbackId> _cancelAttackCallbackIDs;
  std::unordered_set<CallbackManager::CallbackId> _inflictDamageCallbackIDs;

  std::list<std::function<void ()>> _onKilledCallbacks;

  // Combat related systems
  std::shared_ptr<ComboSystem> _comboSystem;

  // The following variables are used to determine combat targets.
  // A character can only inflict damage to another iff the target is
  // within (attack) range.
  std::unordered_set<Character*> _inRangeTargets;
  Character* _lockedOnTarget{};
  bool _isAlerted{};

  // Nearby items and interactable objects.
  std::unordered_set<Item*> _inRangeItems;
  std::list<Interactable*> _inRangeInteractables;

  // Character's inventory and equipment slots.
  // These two types are aliased. See the beginning of this class.
  // We use an extra std::map to keep track of each item's count.
  // For each instance of Item, only one copy of Item* is stored.
  Character::Inventory _inventory{};
  Character::EquipmentSlots _equipmentSlots{};

  // For each item, at most one copy of Item* is kept in memory.
  std::unordered_map<std::string, std::shared_ptr<Item>> _items;

  // The portal to which this character is near.
  GameMap::Portal* _portal{};

  // Currently used skill.
  Character::SkillBook _skillBook{};
  std::unordered_map<std::string, std::shared_ptr<Skill>> _skills;
  std::unordered_set<std::shared_ptr<Skill>> _activeSkillInstances;
  Skill* _currentlyUsedSkill{};

  // Extra attack animations.
  // The first attack animations is in _bodyAnimations[State::ATTACK],
  // and here's some extra ones.
  const int _kAttackAnimationIdxMax;
  int _attackAnimationIdx{};
  std::vector<ax::Animation*> _bodyExtraAttackAnimations;

  // Skill animations
  std::unordered_map<std::string, ax::Animation*> _skillBodyAnimations;

  // Party
  // A character can either:
  // (1) be a leader who has a set of allies/followers, or
  // (2) be a follower of other character
  std::shared_ptr<Party> _party;
};

}  // namespace requiem

#endif  // REQUIEM_CHARACTER_CHARACTER_H_

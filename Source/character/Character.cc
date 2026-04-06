// Copyright (c) 2018-2025 Marco Wang <m.aesophor@gmail.com>. All rights reserved.

#include "Character.h"

#include <algorithm>
#include <cassert>

#include "Assets.h"
#include "Audio.h"
#include "CallbackManager.h"
#include "Constants.h"
#include "character/Player.h"
#include "combat/ComboSystem.h"
#include "gameplay/ExpPointTable.h"
#include "scene/GameScene.h"
#include "scene/SceneManager.h"
#include "util/AxUtil.h"
#include "util/B2BodyBuilder.h"
#include "util/JsonUtil.h"
#include "util/Logger.h"
#include "util/MathUtil.h"
#include "util/RandUtil.h"
#include "util/TimeUtil.h"

namespace fs = std::filesystem;
using namespace std;
using namespace requiem::assets;
USING_NS_AX;

namespace requiem {

Character::Character(const fs::path& jsonFilePath)
    : DynamicActor{State::STATE_SIZE, FixtureType::FIXTURE_SIZE},
      _characterProfile{jsonFilePath},
      _comboSystem{std::make_shared<ComboSystem>(*this)},
      // There will be at least `1` attack animation.
      _kAttackAnimationIdxMax{1 + getExtraAttackAnimationsCount()},
      _bodyExtraAttackAnimations(_kAttackAnimationIdxMax - 1) {
  for (const auto& skillJsonFilePath : _characterProfile.defaultSkills) {
    addSkill(Skill::create(skillJsonFilePath, this));
  }

  for (const auto& [itemJsonFilePath, amount] : _characterProfile.defaultInventory) {
    addItem(Item::create(itemJsonFilePath), amount);
  }
}

bool Character::showOnMap(float x, float y) {
  if (_isShownOnMap || _isKilled) {
    return false;
  }

  _isShownOnMap = true;
  return true;
}

bool Character::removeFromMap() {
  if (!StaticActor::removeFromMap()) {
    return false;
  }

  if (!_isKilled) {
    destroyBody();
  }

  return true;
}

void Character::update(const float delta) {
  if (!_isShownOnMap || _isKilled) {
    return;
  }

  avoidSlidingDownSlope();

  // Flip the sprite and weapon fixture if needed.
  if (!_isFacingRight && !_bodySprite->isFlippedX()) {
    _bodySprite->setFlippedX(true);
    redefineWeaponFixture();
  } else if (_isFacingRight && _bodySprite->isFlippedX()) {
    _bodySprite->setFlippedX(false);
    redefineWeaponFixture();
  }

  // Sync the body sprite with this character's b2body.
  const b2Vec2& b2bodyPos = _body->GetPosition();
  _bodySprite->setPosition(b2bodyPos.x * kPpm + _characterProfile.spriteOffsetX,
                           b2bodyPos.y * kPpm + _characterProfile.spriteOffsetY);

  // Handle stats regeneration.
  _statsRegenTimer += delta;
  if (_statsRegenTimer >= 5.0f) {
    _statsRegenTimer = 0;
    regenHealth(_baseRegenDeltaHealth);
    regenMagicka(_baseRegenDeltaMagicka);
    regenStamina(_baseRegenDeltaStamina);

    auto hud = SceneManager::the().getCurrentScene<GameScene>()->getHud();
    hud->updateStatusBars();
  }

  for (const auto interactable : _inRangeInteractables) {
    if (const auto trigger = dynamic_cast<GameMap::Trigger*>(interactable)) {
      if (const auto damage = trigger->getDamage()) {
        receiveDamage(damage);
      }
    }
  }

  _comboSystem->update(delta);

  if (_isUsingSkill) {
    return;
  }

  _previousState = _currentState;
  _currentState = determineState();

  maybeOverrideCurrentStateWithAttackingMidairState();
  maybeOverrideCurrentStateWithStopRunningState();

  _previousBodyVelocity = _body->GetLinearVelocity();

  if (_previousState != _currentState) {
    switch (_currentState) {
      case State::KILLED:
        runAnimation(State::KILLED, [this]() { onKilled(); });
        break;
      case State::IDLE:
      case State::RUNNING:
      case State::STUNNED:
        runAnimation(_currentState, /*loop=*/true);
        break;
      case State::RUNNING_START:
      case State::RUNNING_STOP:
      case State::JUMPING:
      case State::FALLING:
      case State::FALLING_GETUP:
      case State::CROUCHING:
      case State::DODGING_BACKWARD:
      case State::DODGING_FORWARD:
      case State::ATTACKING:
      case State::ATTACKING_UNARMED:
      case State::ATTACKING_UNARMED_CROUCH:
      case State::ATTACKING_UNARMED_MIDAIR:
      case State::ATTACKING_CROUCH:
      case State::ATTACKING_FORWARD:
      case State::ATTACKING_MIDAIR:
      case State::ATTACKING_MIDAIR_DOWNWARD:
      case State::ATTACKING_UPWARD:
      case State::BLOCKING:
      case State::BLOCKING_HIT:
      case State::INTRO:
      case State::TAKE_DAMAGE:
      default:
        runAnimation(_currentState, /*loop=*/false);
        break;
    }
  }
}

void Character::import(const fs::path& jsonFilePath) {
  _characterProfile = Character::Profile{jsonFilePath};
}

void Character::replaceSpritesheet(const fs::path& jsonFilePath) {
  const float spritesheetZOrder = _bodySpritesheet->getLocalZOrder();

  _bodySprite->removeFromParent();
  _bodySpritesheet->removeFromParent();
  std::fill(_bodyAnimations.begin(), _bodyAnimations.end(), nullptr);
  std::fill(_bodyExtraAttackAnimations.begin(), _bodyExtraAttackAnimations.end(), nullptr);

  _characterProfile.loadSpritesheetInfo(jsonFilePath);
  loadBodyAnimations(_characterProfile.textureResDirPath);

  ax_util::addChildWithParentCameraMask(_node, _bodySpritesheet, spritesheetZOrder);
}

void Character::defineBody(b2BodyType bodyType,
                           float x,
                           float y,
                           short bodyCategoryBits,
                           short bodyMaskBits,
                           short feetMaskBits,
                           short weaponMaskBits) {
  if (_body) {
    destroyBody();
  }

  auto gmMgr = SceneManager::the().getCurrentScene<GameScene>()->getGameMapManager();
  B2BodyBuilder bodyBuilder{gmMgr->getWorld()};
  _body = bodyBuilder.type(bodyType)
    .position(x, y, kPpm)
    .buildBody();

  redefineBodyFixture(bodyCategoryBits, bodyMaskBits);
  redefineFeetFixture(feetMaskBits);
  redefineWeaponFixture(weaponMaskBits);
}

void Character::redefineBodyFixture(short bodyCategoryBits, short bodyMaskBits) {
  if (_fixtures[FixtureType::BODY]) {
    bodyCategoryBits = _fixtures[FixtureType::BODY]->GetFilterData().categoryBits;
    bodyMaskBits = _fixtures[FixtureType::BODY]->GetFilterData().maskBits;

    _body->DestroyFixture(_fixtures[FixtureType::BODY]);
    _fixtures[FixtureType::BODY] = nullptr;
  }

  const float scaleFactor = Director::getInstance()->getContentScaleFactor();
  const float bw = _characterProfile.bodyWidth;
  const float bh = _characterProfile.bodyHeight;

  const float x0 = -bw / 2 / scaleFactor;
  const float x1 = bw / 2 / scaleFactor;
  const float x2 = -bw / 2 / scaleFactor;
  const float x3 = bw / 2 / scaleFactor;

  const float y0 = _isCrouching ? 0 : (bh / 2 / scaleFactor);
  const float y1 = _isCrouching ? 0 : (bh / 2 / scaleFactor);
  const float y2 = -bh / 2 / scaleFactor;
  const float y3 = -bh / 2 / scaleFactor;

  const b2Vec2 bodyVertices[] = {
    {x0, y0},
    {x1, y1},
    {x2, y2},
    {x3, y3}
  };

  B2BodyBuilder bodyBuilder{_body};
  _fixtures[FixtureType::BODY] = bodyBuilder.newPolygonFixture(bodyVertices, 4, kPpm)
    .categoryBits(bodyCategoryBits)
    .maskBits(bodyMaskBits)
    .setSensor(true)
    .setUserData(this)
    .buildFixture();
}

void Character::redefineFeetFixture(short feetMaskBits) {
  if (_fixtures[FixtureType::FEET]) {
    feetMaskBits = _fixtures[FixtureType::FEET]->GetFilterData().maskBits;

    _body->DestroyFixture(_fixtures[FixtureType::FEET]);
    _fixtures[FixtureType::FEET] = nullptr;
  }

  const float bw = _characterProfile.bodyWidth;
  const float bh = _characterProfile.bodyHeight;
  const float feetFixtureRadius = bw / 2;
  const b2Vec2 feetFixtureCenter{0, -bh / 2 + bw / 2};

  B2BodyBuilder bodyBuilder{_body};
  _fixtures[FixtureType::FEET] = bodyBuilder.newCircleFixture(feetFixtureCenter, feetFixtureRadius, kPpm)
    .categoryBits(category_bits::kFeet)
    .maskBits(feetMaskBits)
    .density(kDensity)
    .setUserData(this)
    .buildFixture();
}

void Character::redefineWeaponFixture(short weaponMaskBits) {
  if (_fixtures[FixtureType::WEAPON]) {
    weaponMaskBits = _fixtures[FixtureType::WEAPON]->GetFilterData().maskBits;

    _body->DestroyFixture(_fixtures[FixtureType::WEAPON]);
    _fixtures[FixtureType::WEAPON] = nullptr;
  }

  const float scaleFactor = Director::getInstance()->getContentScaleFactor();
  const float bw = _characterProfile.bodyWidth;
  const float bh = _characterProfile.bodyHeight;
  const float attackRange = _characterProfile.attackRange;

  const float x0 = _isFacingRight ? (bw / 2 / scaleFactor) : (-bw / 2 / scaleFactor);
  const float x1 = _isFacingRight ? (bw / 2 + attackRange) : (-bw / 2 - attackRange);
  const float x2 = _isFacingRight ? (bw / 2 / scaleFactor) : (-bw / 2 / scaleFactor);
  const float x3 = _isFacingRight ? (bw / 2 + attackRange) : (-bw / 2 - attackRange);

  const float y0 = _isCrouching ? (bh / 4 / scaleFactor) : (bh / 2 / scaleFactor);
  const float y1 = _isCrouching ? (bh / 4 / scaleFactor) : (bh / 2 / scaleFactor);
  const float y2 = _isCrouching ? (-bh / 2 / scaleFactor) : (-bh / 2 / scaleFactor);
  const float y3 = _isCrouching ? (-bh / 2 / scaleFactor) : (-bh / 2 / scaleFactor);

  const b2Vec2 weaponVertices[] = {
    {x0, y0},
    {x1, y1},
    {x2, y2},
    {x3, y3}
  };

  B2BodyBuilder bodyBuilder{_body};
  _fixtures[FixtureType::WEAPON] = bodyBuilder.newPolygonFixture(weaponVertices, 4, kPpm)
    .categoryBits(category_bits::kMeleeWeapon)
    .maskBits(weaponMaskBits)
    .setSensor(true)
    .setUserData(this)
    .buildFixture();
}

void Character::defineTexture(const fs::path& bodyTextureResDirPath, float x, float y) {
  loadBodyAnimations(bodyTextureResDirPath);
  _bodySprite->setPosition(x * kPpm, y * kPpm + _characterProfile.spriteOffsetY);

  runAnimation(State::IDLE);
}

void Character::loadBodyAnimations(const fs::path& bodyTextureResDirPath) {
  createBodyAnimation(State::IDLE, nullptr);
  Animation* fallback = _bodyAnimations[State::IDLE];

  createBodyAnimation(State::RUNNING, fallback);
  createBodyAnimation(State::RUNNING_START, _bodyAnimations[State::RUNNING]);
  createBodyAnimation(State::RUNNING_STOP, fallback);
  createBodyAnimation(State::JUMPING, fallback);
  createBodyAnimation(State::FALLING, fallback);
  createBodyAnimation(State::FALLING_GETUP, fallback);
  createBodyAnimation(State::CROUCHING, fallback);
  createBodyAnimation(State::DODGING_BACKWARD, fallback);
  createBodyAnimation(State::DODGING_FORWARD, fallback);
  createBodyAnimation(State::ATTACKING, fallback);
  createBodyAnimation(State::ATTACKING_UNARMED, _bodyAnimations[State::ATTACKING]);
  createBodyAnimation(State::ATTACKING_UNARMED_CROUCH, _bodyAnimations[State::ATTACKING]);
  createBodyAnimation(State::ATTACKING_UNARMED_MIDAIR, _bodyAnimations[State::ATTACKING]);
  createBodyAnimation(State::ATTACKING_CROUCH, _bodyAnimations[State::ATTACKING]);
  createBodyAnimation(State::ATTACKING_FORWARD, _bodyAnimations[State::ATTACKING]);
  createBodyAnimation(State::ATTACKING_MIDAIR, _bodyAnimations[State::ATTACKING]);
  createBodyAnimation(State::ATTACKING_MIDAIR_DOWNWARD, _bodyAnimations[State::ATTACKING]);
  createBodyAnimation(State::ATTACKING_UPWARD, _bodyAnimations[State::ATTACKING]);
  createBodyAnimation(State::SPELLCAST, _bodyAnimations[State::ATTACKING]);
  createBodyAnimation(State::SPELLCAST2, _bodyAnimations[State::ATTACKING]);
  createBodyAnimation(State::SPELLCAST3, _bodyAnimations[State::ATTACKING]);
  createBodyAnimation(State::BLOCKING, fallback);
  createBodyAnimation(State::BLOCKING_HIT, fallback);
  createBodyAnimation(State::INTRO, fallback);
  createBodyAnimation(State::STUNNED, fallback);
  createBodyAnimation(State::TAKE_DAMAGE, fallback);
  createBodyAnimation(State::KILLED, fallback);

  // Load extra attack animations.
  for (size_t i = 0; i < _bodyExtraAttackAnimations.size(); i++) {
    if (_bodyExtraAttackAnimations[i]) {
      continue;
    }

    _bodyExtraAttackAnimations[i] = createAnimation(
        bodyTextureResDirPath,
        "attacking" + std::to_string(1 + i),
        _characterProfile.extraAttackFrameIntervals[i] / kPpm,
        fallback
    );
  }

  // Select a frame as the default look for this spritesheet.
  string framePrefix = StaticActor::getLastDirName(bodyTextureResDirPath);
  _bodySprite = Sprite::createWithSpriteFrameName(framePrefix + "_idle/0.png");
  _bodySprite->setScale(_characterProfile.spriteScaleX,
                        _characterProfile.spriteScaleY);

  _bodySpritesheet = SpriteBatchNode::create((bodyTextureResDirPath / "spritesheet.png").native());
  _bodySpritesheet->getTexture()->setAliasTexParameters();  // disable texture antialiasing
  _bodySpritesheet->addChild(_bodySprite);
}

void Character::createBodyAnimation(const Character::State state,
                                    ax::Animation* fallbackAnimation) {
  if (_bodyAnimations[state]) {
    return;
  }

  _bodyAnimations[state]
    = createAnimation(_characterProfile.textureResDirPath,
                      _kCharacterStateStr[state],
                      _characterProfile.frameIntervals[state] / kPpm,
                      fallbackAnimation);
}

int Character::getExtraAttackAnimationsCount() const {
  FileUtils* fileUtils = FileUtils::getInstance();
  const string& framesNamePrefix = StaticActor::getLastDirName(_characterProfile.textureResDirPath);

  // player_attacking0  // must have!
  // player_attacking1  // optional...
  // player_attacking2  // optional...
  // ...
  const fs::path dirPath = _characterProfile.textureResDirPath / (framesNamePrefix + "_attacking");
  int frameCount = 0;
  fileUtils->setPopupNotify(false);  // disable CCLOG
  while (fileUtils->isDirectoryExist(dirPath.native() + std::to_string(frameCount + 1))) {
    frameCount++;
  }
  fileUtils->setPopupNotify(true);

  return frameCount;
}

Animation* Character::getBodyAttackAnimation() const {
  return (_attackAnimationIdx == 0) ? _bodyAnimations[State::ATTACKING] :
                                      _bodyExtraAttackAnimations[_attackAnimationIdx - 1];
}

void Character::runAnimation(State state, bool loop) {
  Animate* animate = Animate::create((state != State::ATTACKING) ? _bodyAnimations[state] :
                                                                   getBodyAttackAnimation());
  _bodySprite->stopAllActions();
  _bodySprite->runAction(loop ? dynamic_cast<Action*>(RepeatForever::create(animate)) :
                                dynamic_cast<Action*>(Repeat::create(animate, 1)));

  if (state == State::ATTACKING) {
    _attackAnimationIdx = (_attackAnimationIdx + 1) % _kAttackAnimationIdxMax;
  }
}

void Character::runAnimation(State state, const function<void ()>& func) const {
  auto animate = Animate::create(_bodyAnimations[state]);
  auto callback = CallFunc::create(func);
  _bodySprite->stopAllActions();
  _bodySprite->runAction(Sequence::createWithTwoActions(animate, callback));
}

void Character::runAnimation(const string& framesName, float interval) {
  Animation* bodyAnimation = nullptr;

  if (_skillBodyAnimations.find(framesName) != _skillBodyAnimations.end()) {
    bodyAnimation = _skillBodyAnimations[framesName];
  } else {
    Animation* fallback = _bodyAnimations[State::ATTACKING];
    bodyAnimation = createAnimation(_characterProfile.textureResDirPath, framesName, interval, fallback);
    // Cache this skill animation (body).
    _skillBodyAnimations.insert({framesName, bodyAnimation});
  }

  _bodySprite->stopAllActions();
  _bodySprite->runAction(Repeat::create(Animate::create(bodyAnimation), 1));
}

float Character::getAttackAnimationDuration(const Character::State state) const {
  if (state == State::ATTACKING) {
    return getBodyAttackAnimation()->getDuration();
  }
  return _bodyAnimations[state]->getDuration();
}

Character::State Character::determineState() const {
  if (_isSetToKill) {
    return State::KILLED;
  } else if (_isRunningIntroAnimation) {
    return State::INTRO;
  } else if (_isStunned) {
    return State::STUNNED;
  } else if (_isTakingDamage) {
    return State::TAKE_DAMAGE;
  } else if (_isGettingUpFromFalling) {
    return State::FALLING_GETUP;
  } else if (_isHitWhileBlocking) {
    return State::BLOCKING_HIT;
  } else if (_isBlocking) {
    return State::BLOCKING;
  } else if (_isAttacking) {
    return determineAttackState();
  } else if (_isDodgingBackward) {
    return State::DODGING_BACKWARD;
  } else if (_isDodgingForward) {
    return State::DODGING_FORWARD;
  } else if (_body->GetLinearVelocity().y < -2.5f) {
    return State::FALLING;
  } else if (_isJumping) {
    return State::JUMPING;
  } else if (_isCrouching) {
    return State::CROUCHING;
  } else if (_isStartRunning) {
    return State::RUNNING_START;
  } else if (_isStopRunning) {
    return State::RUNNING_STOP;
  } else if (std::abs(_body->GetLinearVelocity().x) > .01f || std::abs(_previousBodyVelocity.x) > .01f) {
    return State::RUNNING;
  } else {
    return State::IDLE;
  }
}

Character::State Character::determineAttackState() const {
  if (_overridingAttackState.has_value()) {
    return *_overridingAttackState;
  }

  const bool isUnarmed = !_equipmentSlots[Equipment::Type::WEAPON] && hasUnarmedAttackAnimation();
  if (_isCrouching) {
    return isUnarmed ? State::ATTACKING_UNARMED_CROUCH : State::ATTACKING_CROUCH;
  }
  if (_isJumping) {
    return isUnarmed ? State::ATTACKING_UNARMED_MIDAIR : State::ATTACKING_MIDAIR;
  }
  return isUnarmed ? State::ATTACKING_UNARMED : State::ATTACKING;
}

void Character::maybeOverrideCurrentStateWithAttackingMidairState() {
  if (_previousState == State::ATTACKING_MIDAIR && _currentState == State::ATTACKING) {
    _currentState = State::ATTACKING_MIDAIR;
  }
}

void Character::maybeOverrideCurrentStateWithStopRunningState() {
  bool needToStopRunning = true;
  const b2Vec2 currentBodyVelocity = _body->GetLinearVelocity();
  if (std::abs(currentBodyVelocity.x) >= _characterProfile.moveSpeed * 4) {
    needToStopRunning = true;
  }

  if (!needToStopRunning) {
    return;
  }

  constexpr float kThreshold = .01f;
  const bool isMovingForward = (_isFacingRight && currentBodyVelocity.x > 0) ||
                               (!_isFacingRight && currentBodyVelocity.x < 0);
  if (std::abs(_previousBodyVelocity.x) >= kThreshold &&
      std::abs(currentBodyVelocity.x) < kThreshold &&
      isMovingForward) {
    stopRunning();
  }
}

void Character::onSetToKill() {
  _isSetToKill = true;
  _killedPos = _body->GetPosition();
}

void Character::onKilled() {
  _isKilled = true;

  if (!_shouldRetainBodyIfKilled) {
    destroyBody();
  }

  const auto& sfxFilePath = getSfxFilePath(Character::Sfx::SFX_KILLED);
  if (!sfxFilePath.native().empty()) {
    Audio::the().playSfx(sfxFilePath);
  }

  for (const auto& callback : _onKilledCallbacks) {
    std::invoke(callback);
  }
}

void Character::onFallToGroundOrPlatform() {
  if (_body->GetLinearVelocity().y < -4.5f) {
    getUpFromFalling();
  }

  if (_isTakingDamage) {
    _isTakingDamage = false;
    getUpFromFalling();
  }
}

void Character::onBodyContactWithEnemyBody(Character* enemy) {
  if (!enemy) {
    VGLOG(LOG_ERR, "Failed to handle physical contact with enemy event, enemy: [nullptr].");
    return;
  }

  if (_isInvincible) {
    return;
  }

  const float knockBackForceX = _isFacingRight ? -2.5f : 2.5f;
  const float knockBackForceY = 3.0f;
  enemy->knockBack(this, knockBackForceX, knockBackForceY);
  enemy->inflictDamage(this, 25);
}

void Character::onMeleeWeaponContactWithEnemyBody(Character* enemy) {
  if (_isUsingSkill &&
      _currentlyUsedSkill->getSkillProfile().skillType == Skill::Type::MELEE &&
      _currentlyUsedSkill->getSkillProfile().physicalDamage) {
    inflictDamage(enemy,
                  getDamageOutput() + _currentlyUsedSkill->getSkillProfile().physicalDamage,
                  _currentlyUsedSkill->getSkillProfile().numTimesInflictDamage,
                  _currentlyUsedSkill->getSkillProfile().damageInflictionInterval);
  }
}

bool Character::isMovementDisallowed() const {
  return _isCrouching || _isUsingSkill || isJumpingDownDisallowed();
}

bool Character::isJumpingDownDisallowed() const {
  return isAttacking() || _isGettingUpFromFalling || _isStunned || _isBlocking || _isRunningIntroAnimation ||
         (_isTakingDamage && !_isTakingDamageFromTraps);
}

bool Character::isAttackingDisallowed() const {
  return isAttacking() || _isUsingSkill || _isGettingUpFromFalling || _isStunned || _isTakingDamage || _isBlocking || _isRunningIntroAnimation;
}

bool Character::isSkillActivationDisallowed() const {
  return isAttacking() || _isUsingSkill || _isGettingUpFromFalling || _isStunned || _isBlocking || _isRunningIntroAnimation;
}

void Character::startRunning() {
  /*
  _isStartRunning = true;
  CallbackManager::the().runAfter([this](const CallbackManager::CallbackId) {
    _isStartRunning = false;
  }, _bodyAnimations[State::RUNNING_START]->getDuration());
  */
}

void Character::stopRunning() {
  _isStopRunning = true;
  CallbackManager::the().runAfter([this](const CallbackManager::CallbackId) {
    _isStopRunning = false;
  }, _bodyAnimations[State::RUNNING_STOP]->getDuration());
}

void Character::moveLeft() {
  constexpr bool moveTowardsRight = false;
  moveImpl(moveTowardsRight);
}

void Character::moveRight() {
  constexpr bool moveTowardsRight = true;
  moveImpl(moveTowardsRight);
}

void Character::moveImpl(const bool moveTowardsRight) {
  if (isMovementDisallowed()) {
    return;
  }

  _isFacingRight = moveTowardsRight;
  setTryingToMoveRecently();

  const b2Vec2& velocity = _body->GetLinearVelocity();
  if (velocity.x == 0 && _previousBodyVelocity.x == 0) {
    startRunning();
  }

  if (std::hypotf(velocity.x, velocity.y) > _characterProfile.moveSpeed) {
    clampLinearVelocity();
  } else {
    float force = _characterProfile.bodyWidth * _characterProfile.bodyHeight * kBodyVolumeToMoveForceFactor;
    if (!moveTowardsRight) {
      force = -force;
    }

    b2Vec2 impulse{force, 0};
    if (_isOnGround) {
      impulse = math_util::rotateCounterClockwise({force, 0}, _groundAngle);
    }

    _body->ApplyLinearImpulseToCenter(impulse, true);
  }
}

void Character::clampLinearVelocity() {
  if (isDodging() || isJumping() || !isOnGround()) {
    return;
  }

  b2Vec2 velocity = _body->GetLinearVelocity();
  const float speed = std::hypotf(velocity.x, velocity.y);
  const float maxSpeed = _characterProfile.moveSpeed;
  if (speed <= maxSpeed) {
    return;
  }

  velocity *= maxSpeed / speed;
  _body->SetLinearVelocity(velocity);
}

void Character::avoidSlidingDownSlope() {
  if (isDodging() || isUsingSkill() || _isTakingDamage || _isTakingDamageFromTraps) {
    return;
  }

  constexpr auto kGracePeriodMs = 100;
  if (isTryingToMoveRecently(kGracePeriodMs)) {
    _body->SetLinearDamping(0);
  } else {
    const bool isReallyOnGround = isOnGround(/*raycast=*/true);
    _body->SetLinearDamping(isReallyOnGround ? numeric_limits<float>::infinity() : 0);
  }

  _isTryingToMoveRecently = false;
}

bool Character::isTryingToMoveRecently(const float gracePeriod) const {
  return _isTryingToMoveRecently || (time_util::getCurrentTimeMs() - _lastMoveTimeMs) < gracePeriod;
}

void Character::setTryingToMoveRecently() {
  _isTryingToMoveRecently = true;
  _lastMoveTimeMs = time_util::getCurrentTimeMs();
}

void Character::jump() {
  // Block current jump request if:
  // 1. This character's is not allowed to move (and jump).
  // 2. This character's cannot jump (_characterProfile.jumpHeight == 0)
  // 3. This character cannot double jump, and it has already jumped.
  // 4. This character can double jump, and it has already double jumped.
  if (isMovementDisallowed() ||
      _characterProfile.jumpHeight == 0.0f ||
      (!_characterProfile.canDoubleJump && _isJumping) ||
      (_characterProfile.canDoubleJump && _isDoubleJumping)) {
    return;
  }

  if (_isJumping) {
    _isDoubleJumping = true;
    runAnimation(State::JUMPING, false);
    const b2Vec2& velocity = _body->GetLinearVelocity();
    _body->SetLinearVelocity({velocity.x, 0});
  }

  _isJumpingDisallowed = true;
  CallbackManager::the().runAfter([this](const CallbackManager::CallbackId) {
    _isJumpingDisallowed = false;
  }, .2f);

  _isJumping = true;
  setTryingToMoveRecently();

  // Respect the temporary linear damping set by dodging.
  if (!isDodging()) {
    _body->SetLinearDamping(0);
  }
  _body->ApplyLinearImpulse({0, _characterProfile.jumpHeight}, _body->GetWorldCenter(), true);
}

void Character::doubleJump() {
  jump();

  CallbackManager::the().runAfter([this](const CallbackManager::CallbackId) {
    jump();
  }, .25f);
}

void Character::jumpDown() {
  if (!_isOnPlatform || isJumpingDownDisallowed()) {
    return;
  }

  _fixtures[FixtureType::FEET]->SetSensor(true);
  CallbackManager::the().runAfter([this](const CallbackManager::CallbackId) {
    _fixtures[FixtureType::FEET]->SetSensor(false);
  }, .25f);
}

void Character::crouch() {
  if (_isCrouching || _isJumping) {
    return;
  }

  _isCrouching = true;

  redefineBodyFixture();
  redefineWeaponFixture();
}

void Character::getUpFromCrouching() {
  if (!_isCrouching) {
    return;
  }

  _isCrouching = false;

  redefineBodyFixture();
  redefineWeaponFixture();
}

void Character::getUpFromFalling() {
  const auto& sfxFilePath = getSfxFilePath(Character::Sfx::SFX_JUMP);
  if (!sfxFilePath.native().empty()) {
    Audio::the().playSfx(sfxFilePath);
  }

  const bool hasGetUpFromFallingAnimation = _bodyAnimations[State::FALLING_GETUP] != _bodyAnimations[State::IDLE];
  if (!hasGetUpFromFallingAnimation) {
    return;
  }

  _isGettingUpFromFalling = true;
  CallbackManager::the().runAfter([this](const CallbackManager::CallbackId) {
    _isGettingUpFromFalling = false;
  }, _bodyAnimations[State::FALLING_GETUP]->getDuration());
}

void Character::dodgeBackward() {
  constexpr float rushPowerX = -5.0f;
  dodgeImpl(State::DODGING_BACKWARD, rushPowerX, _isDodgingBackward);
}

void Character::dodgeForward() {
  constexpr float rushPowerX = 5.0f;
  dodgeImpl(State::DODGING_FORWARD, rushPowerX, _isDodgingForward);
}

void Character::dodgeImpl(const Character::State dodgeState, const float rushPowerX, bool& isDodgingFlag) {
  if (isDodging() || hasDodgedMidair() || isDoubleJumping() || isMovementDisallowed()) {
    return;
  }

  _comboSystem->reset();

  _body->SetLinearDamping(4.0f);
  _body->ApplyLinearImpulseToCenter({_isFacingRight ? rushPowerX : -rushPowerX, 1.0f}, true);

  enableAfterImageFx(AfterImageFxManager::kPlayerAfterImageColor);

  _isInvincible = true;
  CallbackManager::the().runAfter([this](const CallbackManager::CallbackId) {
    _isInvincible = false;
  }, 0.2f);

  _hasDodgedMidair = true;

  isDodgingFlag = true;
  CallbackManager::the().runAfter([this, &isDodgingFlag](const CallbackManager::CallbackId) {
    isDodgingFlag = false;
    _body->SetLinearDamping(0);
    disableAfterImageFx();
  }, _bodyAnimations[dodgeState]->getDuration());
}

void Character::teleportToTarget(Character* target) {
  if (!target->getBody()) {
    VGLOG(LOG_WARN, "Unable to move to target: %s (b2body missing)",
                    target->getCharacterProfile().name.c_str());
    return;
  }

  teleportToTarget(target->getBody()->GetPosition());
}

void Character::teleportToTarget(const b2Vec2& targetPos) {
  setPosition(targetPos.x, targetPos.y);
}

void Character::enableAfterImageFx(const ax::Color3B& color) {
  auto afterImageFxMgr = SceneManager::the().getCurrentScene<GameScene>()->getAfterImageFxManager();
  afterImageFxMgr->registerNode(_node, color, 0.15f, 0.05f);
}

void Character::disableAfterImageFx() {
  auto afterImageFxMgr = SceneManager::the().getCurrentScene<GameScene>()->getAfterImageFxManager();
  afterImageFxMgr->unregisterNode(_node);
}

void Character::runIntroAnimation() {
  _isRunningIntroAnimation = true;
  CallbackManager::the().runAfter([this](const CallbackManager::CallbackId) {
    _isRunningIntroAnimation = false;
  }, _bodyAnimations[State::INTRO]->getDuration());

  const auto& sfxFilePath = getSfxFilePath(Character::Sfx::SFX_INTRO);
  if (!sfxFilePath.native().empty()) {
    Audio::the().playSfx(sfxFilePath);
  }
}

bool Character::attack(const Character::State attackState,
                       const int numTimesInflictDamage,
                       const float damageInflictionInterval) {
  if (!isAttackState(attackState)) {
    VGLOG(LOG_ERR, "Failed to perform attack, invalid attackState providied, attackState: [%d]", attackState);
    return false;
  }

  if (isAttackingDisallowed()) {
    return false;
  }

  _isAttacking = true;
  if (attackState != State::ATTACKING) {
    _overridingAttackState = attackState;
  }

  {
    lock_guard<mutex> lock{_cancelAttackCallbackIDsMutex};
    const CallbackManager::CallbackId cancelAttackCallbackId = CallbackManager::the().runAfter([this](const CallbackManager::CallbackId id) {
      _isAttacking = false;
      _overridingAttackState = std::nullopt;
      _cancelAttackCallbackIDs.erase(id);
    }, getAttackAnimationDuration(attackState));
    _cancelAttackCallbackIDs.emplace(cancelAttackCallbackId);
  }

  const auto weapon = _equipmentSlots[Equipment::Type::WEAPON];
  if (!weapon) {
    const auto& sfxFilePath = getSfxFilePath(Character::Sfx::SFX_ATTACK_UNARMED);
    if (!sfxFilePath.native().empty()) {
      Audio::the().playSfx(sfxFilePath);
    }
  } else {
    const auto& sfxFilePath = weapon->getSfxFilePath(Equipment::Sfx::SFX_SWING);
    if (!sfxFilePath.native().empty()) {
      Audio::the().playSfx(sfxFilePath);
    }
  }

  if (_inRangeTargets.empty()) {
    return false;
  }

  auto it = _inRangeTargets.begin();
  while (it != _inRangeTargets.end()) {
    auto original_it = it++;
    auto target = *original_it;
    if (target->isInvincible()) {
      continue;
    }
    inflictDamage(target, getDamageOutput(), numTimesInflictDamage, damageInflictionInterval);
  }

  return true;
}

void Character::cancelAttack() {
  _isAttacking = false;
  _overridingAttackState = std::nullopt;

  {
    lock_guard<mutex> lock{_cancelAttackCallbackIDsMutex};
    for (const auto& callbackId : _cancelAttackCallbackIDs) {
      CallbackManager::the().cancel(callbackId);
    }
    _cancelAttackCallbackIDs.clear();
  }

  {
    lock_guard<mutex> lock{_inflictDamageCallbackIDsMutex};
    for (const auto& callbackId : _inflictDamageCallbackIDs) {
      CallbackManager::the().cancel(callbackId);
    }
    _inflictDamageCallbackIDs.clear();
  }
}

bool Character::activateSkill(Skill* rawSkill) {
  if (!rawSkill) {
    VGLOG(LOG_ERR, "Failed to activate skill, rawSkill: [nullptr].");
    return false;
  }

  shared_ptr<Skill> skill = rawSkill->shared_from_this();
  if (!skill) {
    VGLOG(LOG_ERR, "Failed to obtain a valid shared_ptr from rawSkill.");
    return false;
  }

  if (isSkillActivationDisallowed()) {
    return false;
  }

  if (skill->getSkillProfile().isToggleable && getActiveSkillInstance(rawSkill)) {
    skill->deactivate();
    removeActiveSkillInstance(rawSkill);
    _isUsingSkill = false;
    _currentlyUsedSkill = nullptr;
    return true;
  }

  if (!skill->canActivate()) {
    return false;
  }

  _isUsingSkill = true;
  _currentlyUsedSkill = rawSkill;

  CallbackManager::the().runAfter([this](const CallbackManager::CallbackId) {
    _isUsingSkill = false;
    _currentState = State::FORCE_UPDATE;
  }, skill->getSkillProfile().framesDuration);

  if (!skill->getSkillProfile().characterFramesName.empty()) {
    const Skill::Profile& skillProfile = skill->getSkillProfile();
    const optional<Character::State> spellcastState = getCharacterState(skillProfile.characterFramesName);
    const float spellcastFrameInterval = spellcastState.has_value() ? _characterProfile.frameIntervals[static_cast<int>(*spellcastState)]: 5.0f;
    runAnimation(skillProfile.characterFramesName, spellcastFrameInterval / kPpm);
  }

  if (skill->getSkillProfile().shouldForkInstance) {
    skill = Skill::create(skill->getSkillProfile().jsonFilePath, this);
  }
  _activeSkillInstances.emplace(skill);
  skill->activate();

  auto hud = SceneManager::the().getCurrentScene<GameScene>()->getHud();
  hud->updateStatusBars();

  return true;
}

void Character::knockBack(Character* target, float forceX, float forceY) const {
  if (!target) {
    VGLOG(LOG_ERR, "Failed to knock back target: [nullptr].");
    return;
  }

  b2Body* b2body = target->getBody();
  b2body->SetLinearDamping(0);
  b2body->ApplyLinearImpulse({forceX, forceY}, b2body->GetWorldCenter(), true);
}

bool Character::inflictDamage(Character* target, int damage) {
  if (!target) {
    VGLOG(LOG_ERR, "Failed to inflict damage to target: [nullptr].");
    return false;
  }

  target->receiveDamage(this, damage);
  target->lockOn(this);

  for (const auto& ally : this->getAllies()) {
    ally->lockOn(target);
  }
  for (const auto& targetAlly : target->getAllies()) {
    targetAlly->lockOn(this);
  }

  return true;
}

bool Character::inflictDamage(Character* target, int damage,
                              const int numTimesInflictDamage, const float damageInflictionInterval) {
  if (!target) {
    VGLOG(LOG_ERR, "Failed to inflict damage to target: [nullptr].");
    return false;
  }

  if (numTimesInflictDamage == 0) {
    VGLOG(LOG_ERR, "Failed to inflict damage to target: [%s], numTimesInflictDamage: [0].", target->getCharacterProfile().name.c_str());
    return false;
  }

  for (int i = 0; i < numTimesInflictDamage; i++) {
    const CallbackManager::CallbackId id = CallbackManager::the().runAfter([this, target, damage](const CallbackManager::CallbackId id) {
      if (_isTakingDamage || !_inRangeTargets.contains(target)) {
        return;
      }

      inflictDamage(target, damage);

      const float attackForce = _characterProfile.attackForce;
      const float knockBackForceX = _isFacingRight ? attackForce : -attackForce;
      const float knockBackForceY = attackForce;
      knockBack(target, knockBackForceX, knockBackForceY);

      if (const auto weapon = _equipmentSlots[Equipment::Type::WEAPON]) {
        Audio::the().playSfx(weapon->getSfxFilePath(Equipment::Sfx::SFX_HIT));
      }

      lock_guard<mutex> lock{_inflictDamageCallbackIDsMutex};
      _inflictDamageCallbackIDs.erase(id);
    }, _characterProfile.attackDelay + damageInflictionInterval * i);

    lock_guard<mutex> lock{_inflictDamageCallbackIDsMutex};
    _inflictDamageCallbackIDs.emplace(id);
  }

  return true;
}

bool Character::receiveDamage(Character* source, int damage, float takeDamageDuration) {
  if (_isSetToKill || _isInvincible) {
    return false;
  }

  if (source && (source->isSetToKill() || source->isKilled())) {
    return false;
  }

  if (_isBlocking) {
    _isHitWhileBlocking = true;
    CallbackManager::the().runAfter([this](const CallbackManager::CallbackId) {
      _isHitWhileBlocking = false;
    }, _bodyAnimations[State::BLOCKING_HIT]->getDuration());
    return true;
  }

  _characterProfile.health -= damage;

  _isTakingDamageFromTraps = !source;
  if (source) {
    _isTakingDamage = true;
    CallbackManager::the().runAfter([this](const CallbackManager::CallbackId) {
      _isTakingDamage = false;
      _isTakingDamageFromTraps = false;
    }, takeDamageDuration);
  }

  cancelAttack();

  if (_characterProfile.health <= 0) {
    _characterProfile.health = 0;

    if (source) {
      source->setLockedOnTarget(nullptr);
      source->getInRangeTargets().erase(this);
      for (const auto& sourceAlly : source->getAllies()) {
        sourceAlly->getInRangeTargets().erase(this);
        if (sourceAlly->getLockedOnTarget() == this) {
          sourceAlly->setLockedOnTarget(nullptr);
        }
      }

      for (const auto ally : getAllies()) {
        if (ally->isKilled() || ally->isSetToKill()) {
          continue;
        }
        source->setLockedOnTarget(ally);
      }
    }

    DynamicActor::setCategoryBits(_fixtures[FixtureType::BODY], category_bits::kDestroyed);

    onSetToKill();

    // TODO: play killed sound.
  } else {
    // TODO: play hurt sound.
  }

  auto fxMgr = SceneManager::the().getCurrentScene<GameScene>()->getFxManager();
  fxMgr->createHitFx(this);

  auto floatingDamages = SceneManager::the().getCurrentScene<GameScene>()->getFloatingDamages();
  floatingDamages->show(this, damage);

  const auto& sfxFilePath = getSfxFilePath(Character::Sfx::SFX_HURT);
  if (!sfxFilePath.native().empty()) {
    Audio::the().playSfx(sfxFilePath);
  }

  return true;
}

bool Character::receiveDamage(Character* source, int damage) {
  constexpr float kNumSecCantMove = 0.2f;
  return receiveDamage(source, damage, kNumSecCantMove);
}

bool Character::receiveDamage(int damage) {
  return receiveDamage(nullptr, damage);
}

void Character::lockOn(Character* target) {
  _isAlerted = true;
  setLockedOnTarget(target);
}

bool Character::addItem(shared_ptr<Item> item, int amount) {
  if (!item) {
    VGLOG(LOG_ERR, "Failed to add item, item: [nullptr].");
    return false;
  }
  if (amount == 0) {
    VGLOG(LOG_ERR, "Failed to add item, amount: [0].");
    return false;
  }

  // If this Item* does not exist in Inventory or EquipmentSlots yet, store it in _itemMapper.
  // Otherwise, simply delete it and use the existing copy instead (saves memory).
  Item* existingItemObj = getExistingItemObj(item.get());
  if (existingItemObj) {
    existingItemObj->setAmount(existingItemObj->getAmount() + amount);
  } else {
    existingItemObj = item.get();
    existingItemObj->setAmount(amount);
    _items[item->getItemProfile().jsonFilePath] = std::move(item);
  }

  _inventory[existingItemObj->getItemProfile().itemType].insert(existingItemObj);
  return true;
}

bool Character::removeItem(Item* item, int amount) {
  if (!item) {
    VGLOG(LOG_WARN, "Failed to remove item, item: [nullptr].");
    return false;
  }
  if (amount == 0) {
    VGLOG(LOG_WARN, "Failed to remove item, amount: [0].");
    return false;
  }

  Item* existingItemObj = getExistingItemObj(item);
  if (!existingItemObj) {
    VGLOG(LOG_ERR, "Failed to remove item, existingItemObj: [nullptr].");
    return false;
  }

  const int finalAmount = std::max(0, existingItemObj->getAmount() - amount);
  existingItemObj->setAmount(finalAmount);

  if (finalAmount == 0) {
    _inventory[item->getItemProfile().itemType].erase(existingItemObj);

    // We can safely delete this Item* if:
    // 1. It is not an equipment, or...
    // 2. It is an equipment, but no same item is currently equipped.
    Equipment* equipment = dynamic_cast<Equipment*>(existingItemObj);
    if (!equipment ||
        _equipmentSlots[equipment->getEquipmentProfile().equipmentType] != existingItemObj) {
      _items.erase(item->getItemProfile().jsonFilePath);
    }
  }

  return true;
}

// For each instance of an item, at most one copy is kept in the memory.
// This copy will be stored in _itemMapper (unordered_map<string, Item*>)
// Search time complexity: avg O(1), worst O(n).
Item* Character::getExistingItemObj(Item* item) const {
  if (!item) {
    return nullptr;
  }
  auto it = _items.find(item->getItemProfile().jsonFilePath);
  return (it != _items.end()) ? it->second.get() : nullptr;
}

void Character::useItem(Consumable* consumable) {
  auto& profile = _characterProfile;
  const auto& consumableProfile = consumable->getConsumableProfile();

  profile.health += consumableProfile.restoreHealth;
  if (profile.health > profile.fullHealth) profile.health = profile.fullHealth;

  profile.magicka += consumableProfile.restoreMagicka;
  if (profile.magicka > profile.fullMagicka) profile.magicka = profile.fullMagicka;

  profile.stamina += consumableProfile.restoreStamina;
  if (profile.stamina > profile.fullStamina) profile.stamina = profile.fullStamina;

  profile.baseMeleeDamage += consumableProfile.bonusPhysicalDamage;
  //profile.baseMagicalDamage += consumableProfile.bonusMagicalDamage;
  profile.strength += consumableProfile.bonusStr;
  profile.dexterity += consumableProfile.bonusDex;
  profile.intelligence += consumableProfile.bonusInt;
  profile.luck += consumableProfile.bonusLuk;

  profile.moveSpeed += consumableProfile.bonusMoveSpeed;
  profile.jumpHeight += consumableProfile.bonusJumpHeight;

  removeItem(consumable, 1);

  auto hud = SceneManager::the().getCurrentScene<GameScene>()->getHud();
  hud->updateStatusBars();
}

void Character::equip(Equipment* equipment, bool audio) {
  // If there's already an equipment in that slot, unequip it first.
  Equipment::Type type = equipment->getEquipmentProfile().equipmentType;
  if (_equipmentSlots[type]) {
    unequip(type);
  }
  _equipmentSlots[type] = equipment;
  removeItem(equipment, 1);

  if (audio) {
    Audio::the().playSfx(kSfxEquipUnequipItem);
  }
}

void Character::unequip(Equipment::Type equipmentType, bool audio) {
  // If there's an equipped item in the target slot,
  // move it into character's inventory.
  if (!_equipmentSlots[equipmentType]) {
    return;
  }

  Equipment* e = _equipmentSlots[equipmentType];
  _equipmentSlots[equipmentType] = nullptr;

  const auto& jsonFilePath = e->getItemProfile().jsonFilePath;
  auto it = _items.find(e->getItemProfile().jsonFilePath);
  if (it == _items.end()) {
    VGLOG(LOG_ERR, "The unequipped item [%s] is not in player's itemMapper.", jsonFilePath.c_str());
    return;
  }

  addItem(it->second, 1);

  if (audio) {
    Audio::the().playSfx(kSfxEquipUnequipItem);
  }
}

void Character::pickupItem(Item* item) {
  auto gmMgr = SceneManager::the().getCurrentScene<GameScene>()->getGameMapManager();
  shared_ptr<Item> i = gmMgr->getGameMap()->removeDynamicActor<Item>(item);
  addItem(std::move(i), item->getAmount());
}

void Character::discardItem(Item* item, int amount) {
  const fs::path& jsonFilePath = item->getItemProfile().jsonFilePath;
  float x = _body->GetPosition().x;
  float y = _body->GetPosition().y;

  auto gmMgr = SceneManager::the().getCurrentScene<GameScene>()->getGameMapManager();
  gmMgr->getGameMap()->createItem(jsonFilePath, x * kPpm, y * kPpm, amount);

  removeItem(item, amount);
}

void Character::interact(Interactable* target) {
  target->onInteract(this);
}

void Character::addExp(const int exp) {
  int& thisExp = _characterProfile.exp;
  int& thisLevel = _characterProfile.level;

  thisExp += exp;
  while (thisExp >= exp_point_table::getNextLevelExp(thisLevel)) {
    thisExp -= exp_point_table::getNextLevelExp(thisLevel);
    thisLevel++;
  }
}

bool Character::addSkill(shared_ptr<Skill> skill) {
  if (!skill) {
    VGLOG(LOG_ERR, "Failed to add skill to [%s], skill: [nullptr].", _characterProfile.name.c_str());
    return false;
  }

  if (_skills.contains(skill->getSkillProfile().jsonFilePath)) {
    VGLOG(LOG_WARN, "Failed to add skill [%s] to [%s], already added",
          skill->getName().c_str(), _characterProfile.name.c_str());
    return false;
  }

  _skillBook[skill->getSkillProfile().skillType].insert(skill.get());
  _skills.emplace(skill->getSkillProfile().jsonFilePath, std::move(skill));
  return true;
}

bool Character::removeSkill(Skill* skill) {
  if (!skill) {
    VGLOG(LOG_ERR, "Failed to remove skill to [%s], skill: [nullptr].", _characterProfile.name.c_str());
    return false;
  }

  auto it = _skills.find(skill->getSkillProfile().jsonFilePath);
  if (it == _skills.end()) {
    VGLOG(LOG_WARN, "Failed to remove skill [%s] from [%s], already removed.",
          skill->getName().c_str(), _characterProfile.name.c_str());
    return false;
  }

  removeActiveSkillInstance(skill);
  _skillBook[skill->getSkillProfile().skillType].erase(skill);
  _skills.erase(it);
  return true;
}

Skill* Character::getSkill(const fs::path& skillJsonFilePath) const {
  auto it = _skills.find(skillJsonFilePath.native());
  if (it == _skills.end()) {
    VGLOG(LOG_WARN, "Failed to get skill [%s] from [%s].",
          skillJsonFilePath.c_str(), _characterProfile.name.c_str());
    return nullptr;
  }

  return it->second.get();
}

void Character::resurrect() {
  if (!_isSetToKill || !_isKilled) {
    return;
  }

  removeFromMap();

  _isSetToKill = false;
  _isKilled = false;
  _characterProfile.health = _characterProfile.fullHealth;

  showOnMap(_killedPos.x * kPpm, _killedPos.y * kPpm);
}

int Character::getGoldBalance() const {
  return getItemAmount(Item::create(assets::kGoldCoin)->getItemProfile().jsonFilePath);
}

void Character::addGold(const int amount) {
  addItem(Item::create(assets::kGoldCoin), amount);
}

void Character::removeGold(const int amount) {
  removeItem(Item::create(assets::kGoldCoin).get(), amount);
}

bool Character::isOnGround(const bool raycast) const {
  if (!raycast) {
    return _isOnGround;
  }

  constexpr auto kGroundCheckDistRatio = .019583f;  // Obtained through play-testing
  const b2Vec2 src{_body->GetPosition()};
  const b2Vec2 dst{src.x, src.y - _characterProfile.bodyWidth * kGroundCheckDistRatio};

  auto gmMgr = SceneManager::the().getCurrentScene<GameScene>()->getGameMapManager();
  return gmMgr->raycast(src, dst, category_bits::kGround);
}

int Character::getItemAmount(const string& itemJsonFilePath) const {
  auto it = _items.find(itemJsonFilePath);
  return it == _items.end() ? 0 : it->second->getAmount();
}

shared_ptr<Skill> Character::getActiveSkillInstance(Skill* skill) const {
  shared_ptr<Skill> key{shared_ptr<Skill>{}, skill};
  const auto it = _activeSkillInstances.find(key);
  return it != _activeSkillInstances.end() ? *it : nullptr;
}

void Character::removeActiveSkillInstance(Skill* skill) {
  shared_ptr<Skill> key{shared_ptr<Skill>{}, skill};
  _activeSkillInstances.erase(key);
}

bool Character::isWaitingForPartyLeader() const {
  return _party && _party->getWaitingMemberLocationInfo(_characterProfile.jsonFilePath);
}

unordered_set<Character*> Character::getAllies() const {
  if (!_party) {
    return {};
  }

  unordered_set<Character*> ret;
  for (const auto& member : _party->getMembers()) {
    if (member.get() != this) {
      ret.insert(member.get());
    }
  }
  if (Character* leader = _party->getLeader(); leader != this) {
    ret.insert(leader);
  }
  return ret;
}

int Character::getDamageOutput() const {
  int output = _characterProfile.baseMeleeDamage;

  if (Equipment* weapon = _equipmentSlots[Equipment::Type::WEAPON]) {
    output += weapon->getEquipmentProfile().bonusPhysicalDamage;
  }
  return output + rand_util::randInt(-5, 5); // temporary
}

void Character::regenHealth(int deltaHealth) {
  const int& fullHealth = _characterProfile.fullHealth;
  int& health = _characterProfile.health;

  health += deltaHealth;
  health = (health > fullHealth) ? fullHealth : health;

  auto hud = SceneManager::the().getCurrentScene<GameScene>()->getHud();
  hud->updateStatusBars();
}

void Character::regenMagicka(int deltaMagicka) {
  const int& fullMagicka = _characterProfile.fullMagicka;
  int& magicka = _characterProfile.magicka;

  magicka += deltaMagicka;
  magicka = (magicka > fullMagicka) ? fullMagicka : magicka;

  auto hud = SceneManager::the().getCurrentScene<GameScene>()->getHud();
  hud->updateStatusBars();
}

void Character::regenStamina(int deltaStamina) {
  const int& fullStamina = _characterProfile.fullStamina;
  int& stamina = _characterProfile.stamina;

  stamina += deltaStamina;
  stamina = (stamina > fullStamina) ? fullStamina : stamina;

  auto hud = SceneManager::the().getCurrentScene<GameScene>()->getHud();
  hud->updateStatusBars();
}

optional<Character::State> Character::getCharacterState(const string& frameName) {
  static auto buildCache = []() -> unordered_map<string, Character::State> {
    unordered_map<string, Character::State> cache;
    for (int i = 0; i < State::STATE_SIZE; i++) {
      cache.emplace(Character::_kCharacterStateStr[i], static_cast<Character::State>(i));
    }
    return cache;
  };

  static unordered_map<string, Character::State> cache = buildCache();
  const auto it = cache.find(frameName);
  return it != cache.end() ? make_optional(it->second) : nullopt;
}

Character::Profile::Profile(const fs::path& jsonFilePath) : jsonFilePath{jsonFilePath} {
  loadSpritesheetInfo(jsonFilePath);

  rapidjson::Document json = json_util::loadFromFile(jsonFilePath);

  name = json["name"].GetString();
  level = json["level"].GetInt();
  exp = json["exp"].GetInt();

  fullHealth = json["fullHealth"].GetInt();
  fullStamina = json["fullStamina"].GetInt();
  fullMagicka = json["fullMagicka"].GetInt();

  health = json["health"].GetInt();
  stamina = json["stamina"].GetInt();
  magicka = json["magicka"].GetInt();

  strength = json["strength"].GetInt();
  dexterity = json["dexterity"].GetInt();
  intelligence = json["intelligence"].GetInt();
  luck = json["luck"].GetInt();

  baseMeleeDamage = json["baseMeleeDamage"].GetInt();

  for (const auto& skillJson : json["defaultSkills"].GetArray()) {
    string skillJsonFilePath = skillJson.GetString();
    defaultSkills.push_back(std::move(skillJsonFilePath));
  }

  for (const auto& itemJson : json["defaultInventory"].GetObject()) {
    string itemJsonFilePath = itemJson.name.GetString();
    int amount = itemJson.value.GetInt();
    defaultInventory.push_back({std::move(itemJsonFilePath), amount});
  }
}

void Character::Profile::loadSpritesheetInfo(const fs::path& jsonFilePath) {
  rapidjson::Document json = json_util::loadFromFile(jsonFilePath);

  textureResDirPath = json["textureResDirPath"].GetString();
  spriteOffsetX = json["spriteOffsetX"].GetFloat();
  spriteOffsetY = json["spriteOffsetY"].GetFloat();
  spriteScaleX = json["spriteScaleX"].GetFloat();
  spriteScaleY = json["spriteScaleY"].GetFloat();

  bodyWidth = json["bodyWidth"].GetInt();
  bodyHeight = json["bodyHeight"].GetInt();
  moveSpeed = json["moveSpeed"].GetFloat();
  jumpHeight = json["jumpHeight"].GetFloat();
  canDoubleJump = json["canDoubleJump"].GetBool();

  attackForce = json["attackForce"].GetFloat();
  attackTime = json["attackTime"].GetFloat();
  attackRange = json["attackRange"].GetFloat();
  attackDelay = json["attackDelay"].GetFloat();
  if (json.HasMember("forwardAttackNumTimesInflictDamage")) {
    forwardAttackNumTimesInflictDamage = json["forwardAttackNumTimesInflictDamage"].GetInt();
  }

  for (int i = 0; i < Character::State::STATE_SIZE; i++) {
    if (!json["frameInterval"].HasMember(Character::_kCharacterStateStr[i].c_str())) {
      VGLOG(LOG_ERR, "Failed to get the frame interval of [%s].", Character::_kCharacterStateStr[i].c_str());
      frameIntervals[i] = 10.0f;
      continue;
    }
    frameIntervals[i] = json["frameInterval"][Character::_kCharacterStateStr[i].c_str()].GetFloat();
  }

  for (int i = 0; ; i++) {
    const string key = "attacking" + to_string(1 + i);
    if (!json["frameInterval"].HasMember(key.c_str())) {
      break;
    }
    extraAttackFrameIntervals.emplace_back(json["frameInterval"][key.c_str()].GetFloat());
  }

  for (int i = 0; i < Character::Sfx::SFX_SIZE; i++) {
    const string& sfxKey = Character::_kCharacterSfxStr[i];
    if (!json["sfx"].HasMember(sfxKey.c_str())) {
      continue;
    }

    const fs::path sfxPath = json["sfx"][sfxKey.c_str()].GetString();
    std::error_code ec;
    if (!fs::exists(sfxPath, ec)) {
      continue;
    }
    sfxFilePaths[i] = sfxPath;
  }
}

}  // namespace requiem

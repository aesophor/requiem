// Copyright (c) 2023-2025 Marco Wang <m.aesophor@gmail.com>. All rights reserved.

#include "CombatMotion.h"

#include "character/Character.h"
#include "util/Logger.h"

using namespace std;
USING_NS_AX;

namespace requiem {

namespace {

bool handleAttackingUpward(Character& c) {
  c.attack(Character::State::ATTACKING_UPWARD, /*numTimesInflictDamage=*/2);

  const float forceX = c.isFacingRight() ? .3f : -.3f;
  const float forceY = 3.0f;
  c.getBody()->SetLinearDamping(0);
  c.getBody()->ApplyLinearImpulseToCenter({forceX, forceY}, true);

  if (c.getInRangeTargets().size()) {
    Character* target = *(c.getInRangeTargets().begin());
    target->getBody()->SetLinearDamping(0);
    target->getBody()->ApplyLinearImpulseToCenter({forceX, forceY}, true);
  }

  return true;
}

bool handleAttackingForward(Character& c) {
  return c.attack(Character::State::ATTACKING_FORWARD, c.getCharacterProfile().forwardAttackNumTimesInflictDamage);
}

bool handleAttackingMidairDownward(Character& c) {
  return true;
}

}  // namespace

bool handleCombatMotion(Character& c, const Character::State attackState) {
  switch (attackState) {
    case Character::State::ATTACKING_UPWARD:
      return handleAttackingUpward(c);
    case Character::State::ATTACKING_FORWARD:
      return handleAttackingForward(c);
    case Character::State::ATTACKING_MIDAIR_DOWNWARD:
      return handleAttackingMidairDownward(c);
    case Character::State::ATTACKING:
    case Character::State::ATTACKING_UNARMED:
    case Character::State::ATTACKING_CROUCH:
    case Character::State::ATTACKING_MIDAIR:
    default:
      c.attack();
      return false;
  }
}

}  // namespace requiem

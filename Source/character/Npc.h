// Copyright (c) 2018-2025 Marco Wang <m.aesophor@gmail.com>. All rights reserved.

#ifndef REQUIEM_CHARACTER_NPC_H_
#define REQUIEM_CHARACTER_NPC_H_

#include <string>
#include <unordered_map>

#include <axmol.h>

#include "Interactable.h"
#include "character/Character.h"
#include "character/NpcController.h"
#include "gameplay/DialogueTree.h"
#include "ui/hud/StatusBar.h"

namespace requiem {

// Npc: Non-player character
// i.e., enemies and allies are all npcs.
class Npc final : public Character, public Interactable {
 public:
  // In addition to Character::FixtureType, the new version defined in Npc.h
  // has the fourth fixture type.
  enum FixtureType {
    BODY,
    FEET,
    WEAPON,
    INTERACTABLE,  // used in player's interaction with Npcs (with FEET fixture)
    FIXTURE_SIZE
  };

  // The "disposition" of an Npc means its "friendliness" toward the player.
  enum class Disposition {
    ALLY,
    ENEMY,
    SIZE
  };

  struct Profile final {
    explicit Profile(const std::filesystem::path& jsonFilePath);

    struct DroppedItemData {
      int chance;
      int minAmount;
      int maxAmount;
    };
    // <json, {chance, minAmount, maxAmount}>
    std::unordered_map<std::string, DroppedItemData> droppedItems;

    std::string dialogueTreeJsonFile;
    Npc::Disposition disposition;
    bool isRespawnable;
    bool isRecruitable;
    bool isTradable;
    bool shouldSandbox;
  };

  explicit Npc(const std::filesystem::path& jsonFilePath);
  virtual ~Npc() override = default;

  virtual bool showOnMap(float x, float y) override;  // Character
  virtual bool removeFromMap() override;  // Character
  virtual void update(const float delta) override;  // Character
  virtual void import(const std::filesystem::path& jsonFilePath) override;  // Character

  virtual void onSetToKill() override;  // Character
  virtual void onKilled() override;  // Character
  virtual void beforeMapChanged() override;  // Character

  virtual bool inflictDamage(Character* target, int damage) override;  // Character
  virtual bool receiveDamage(Character* source, int damage) override;  // Character
  virtual void interact(Interactable* target) override;  // Character

  virtual void onInteract(Character* source) override;  // Interactable
  virtual bool willInteractOnContact() const override;  // Interactable
  virtual void showHintUI() override;  // Interactable
  virtual void hideHintUI() override;  // Interactable

  void enable();
  void disable();

  void act(const float delta) { _npcController.update(delta); }
  void onTouchingPivotMarker();
  void dropItems();

  void updateDialogueTreeIfNeeded();
  void onDialogueBegin();
  void onDialogueEnd();
  void beginDialogue();
  void beginTrade();

  bool isPlayerLeaderOfParty() const;
  bool isWaitingForPartyLeader() const;
  bool isWaitingForPlayer() const;

  inline Npc::Profile& getNpcProfile() { return _npcProfile; }
  inline DialogueTree& getDialogueTree() { return _dialogueTree; }
  inline Npc::Disposition getDisposition() const { return _disposition; }
  inline NpcController& getNpcController() { return _npcController; }
  void setDisposition(Npc::Disposition disposition);

  inline bool isSandboxing() const { return _npcController.isSandboxing(); }
  inline void setSandboxing(const bool sandboxing) { _npcController.setSandboxing(sandboxing); }

  inline void setShowDuringDawn(const bool showDuringDawn) { _shouldShowDuringDawn = showDuringDawn; }
  inline void setShowDuringDay(const bool showDuringDay) { _shouldShowDuringDay = showDuringDay; }
  inline void setShowDuringDusk(const bool showDuringDusk) { _shouldShowDuringDusk = showDuringDusk; }
  inline void setShowDuringNight(const bool showDuringNight) { _shouldShowDuringNight = showDuringNight; }

 private:
  virtual void defineBody(b2BodyType bodyType,
                          float x,
                          float y,
                          short bodyCategoryBits=0,
                          short bodyMaskBits=0,
                          short feetMaskBits=0,
                          short weaponMaskBits=0) override;  // Character

  virtual void createHintBubbleFx() override;  // Interactable
  virtual void removeHintBubbleFx() override;  // Interactable

  bool shouldShowAtCurrentInGameTime(const InGameTime& inGameTime) const;

  Npc::Profile _npcProfile;
  DialogueTree _dialogueTree;
  Npc::Disposition _disposition;
  NpcController _npcController;
  bool _isEnabled;
  bool _shouldShowDuringDawn;
  bool _shouldShowDuringDay;
  bool _shouldShowDuringDusk;
  bool _shouldShowDuringNight;

  ax::Sprite* _hintBubbleFxSprite{};
  std::unique_ptr<StatusBar> _floatingHealthBar;
};

}  // namespace requiem

#endif  // REQUIEM_CHARACTER_NPC_H_

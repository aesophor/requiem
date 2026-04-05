// Copyright (c) 2018-2026 Marco Wang <m.aesophor@gmail.com>. All rights reserved.

#ifndef REQUIEM_MAP_OBJECT_CHEST_H_
#define REQUIEM_MAP_OBJECT_CHEST_H_

#include <string>
#include <vector>

#include <axmol.h>

#include "DynamicActor.h"
#include "Interactable.h"

namespace requiem {

class Chest : public DynamicActor, public Interactable {
 public:
  Chest(const std::string& tmxMapFilePath,
        const int chestId,
        const std::string& itemJsons);
  virtual ~Chest() override = default;

  virtual bool showOnMap(float x, float y) override;  // DynamicActor

  virtual void onInteract(Character* user) override;  // Interactable
  virtual bool willInteractOnContact() const override { return false; }  // Interactable
  virtual void showHintUI() override;  // Interactable
  virtual void hideHintUI() override;  // Interactable

 protected:
  virtual void createHintBubbleFx() override;  // Interactable
  virtual void removeHintBubbleFx() override;  // Interactable

  void defineBody(b2BodyType bodyType,
                  float x,
                  float y,
                  short categoryBits,
                  short maskBits);
  std::vector<std::pair<std::string, int>> parseContent(const std::string& contentStr);

  const std::string _tmxMapFilePath;
  const int _chestId{};
  const std::vector<std::pair<std::string, int>> _content;
  bool _isOpened{};
  ax::Sprite* _hintBubbleFxSprite{};
};

}  // namespace requiem

#endif  // REQUIEM_MAP_OBJECT_CHEST_H_

#pragma once
#include <Arduino.h>
#include <atomic>
#include "config_store.h"

class WledOutput {
public:
  void begin();
  void update(const LightConfig* lights, const LightState* states, uint8_t count);
  void stop();
  bool isRunning() const { return initialized.load(); }
private:
  std::atomic<bool> initialized{false};
};
extern WledOutput wledOutput;

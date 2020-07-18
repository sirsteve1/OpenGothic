#include "codemaster.h"

#include "world/world.h"

CodeMaster::CodeMaster(Vob* parent, World &world, ZenLoad::zCVobData&& d, bool startup)
  :AbstractTrigger(parent,world,std::move(d),startup), keys(data.zCCodeMaster.slaveVobName.size()) {
  }

void CodeMaster::onTrigger(const TriggerEvent &evt) {
  size_t count = 0;
  for(size_t i=0;i<keys.size();++i) {
    if(!keys[i])
      break;
    ++count;
    }

  for(size_t i=0;i<keys.size();++i) {
    if(data.zCCodeMaster.slaveVobName[i]==evt.emitter) {
      if(data.zCCodeMaster.orderRelevant && (count!=i)) {
        onFailure();
        return;
        }
      keys[i] = true;
      }
    }

  for(auto i:keys)
    if(!i) {
      onFailure();
      return;
      }

  zeroState();
  TriggerEvent e(data.zCCodeMaster.triggerTarget,data.vobName,TriggerEvent::T_Trigger);
  world.triggerEvent(e);
  }

void CodeMaster::onFailure() {
  if(data.zCCodeMaster.firstFalseIsFailure)
    zeroState();
  }

void CodeMaster::zeroState() {
  for(size_t i=0;i<keys.size();++i)
    keys[i] = false;
  }

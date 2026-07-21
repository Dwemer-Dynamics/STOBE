#pragma once

#include <kenshi/Enums.h>
#include <ogre/OgreMemoryAllocatorConfig.h>
#include <ogre/OgreVector3.h>
#include <utility>
#include <vector>

class Weather {
public:
  const std::string &getName() const;
};

class WeatherInstance {
public:
  Weather *getWeather() const;
  float getWeatherStrength() const;
  float getWindSpeed() const;
  const Ogre::Vector3 &getWindDirection() const;
  float getWetness() const;
};

class WeatherRegion {
public:
  WeatherInstance *getWeatherInstance() const;
};

typedef std::vector<
    std::pair<EffectType::Enum, float>,
    Ogre::STLAllocator<std::pair<EffectType::Enum, float>,
                       Ogre::GeneralAllocPolicy> >
    StobeWeatherEffectList;

class WeatherSystem {
public:
  static WeatherSystem *getInstance();
  const StobeWeatherEffectList &
  getPositionGlobalEffects(const Ogre::Vector3 &position) const;

  WeatherRegion *ActiveRegionWeather;
};

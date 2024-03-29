#ifndef HALITE_SOLARSYSTEM_H
#define HALITE_SOLARSYSTEM_H

#include "Generator.hpp"

namespace mapgen {
    constexpr auto MAX_TOTAL_ATTEMPTS = 75000;

    /**
     * Map generator using a "solar system" model. This generates random orbits
     * around the center of the map, placing planets evenly spaced along those
     * orbits.
     */
    class SolarSystem : Generator {
    public:
        explicit SolarSystem(unsigned int _seed);

        auto generate(hlt::Map &map, unsigned int num_players) -> std::vector<PointOfInterest>;

        auto name() -> std::string;
    };

}

#endif //HALITE_SOLARSYSTEM_H

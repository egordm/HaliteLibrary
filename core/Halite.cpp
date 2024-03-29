#define  _USE_MATH_DEFINES

#include "Halite.hpp"
#include "hlt.hpp"
#include <functional>
#include <memory>
#include <chrono>
#include <unordered_set>
#include <ostream>
#include <ctime>
#include <cmath>
#include "mapgen/AsteroidCluster.hpp"
#include "mapgen/SolarSystem.hpp"
#include "SimulationEvent.hpp"
#include "Replay.hpp"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


/**
 * Format the current time (to use for the replay file name) in a way
 * compatible with compilers not supporting C++11.
 * @return
 */
auto put_time() -> std::string {
    // While compilers like G++4.8 report C++11 compatibility, they do not
    // support std::put_time, so we have to use strftime instead.
    auto time = std::time(nullptr);
    auto localtime = std::localtime(&time);
    char result[30];
    std::strftime(result, 30, "%Y%m%d-%H%M%S%z-", localtime);
    return std::string(result);
}

/*
 * PRIVATE FUNCTIONS
 */

auto Halite::compare_rankings(const hlt::PlayerId &player1,
                              const hlt::PlayerId &player2) const -> bool {
    if (total_ship_count[player1] == total_ship_count[player2])
        return damage_dealt[player1] < damage_dealt[player2];
    return total_ship_count[player1] < total_ship_count[player2];
}

auto Halite::compute_damage(hlt::EntityId self_id, hlt::EntityId other_id)
-> std::pair<unsigned short, unsigned short> {
    unsigned short self_damage = 0;
    unsigned short other_damage = 0;

    switch (self_id.type) {
        case hlt::EntityType::PlanetEntity: {
            const auto &other = game_map.get_ship(other_id);
            self_damage = other.health;
            other_damage = other.health;
            break;
        }
        case hlt::EntityType::ShipEntity: {
            const auto &self = game_map.get_ship(self_id);
            self_damage = self.health;
            if (other_id.type == hlt::EntityType::ShipEntity) {
                other_damage = game_map.get_ship(other_id).health;
            } else {
                other_damage = self.health;
            }
            break;
        }
        case hlt::EntityType::InvalidEntity:
            throw std::string("Cannot compute damage against an invalid entity");
    }

    return std::make_pair(self_damage, other_damage);
}

auto planet_explosion_damage(hlt::Planet &planet, double distance,
                             double max_distance) -> unsigned short {
    if (distance < planet.radius) {
        return std::numeric_limits<unsigned short>::max();
    }

    const auto distance_from_crust = distance - planet.radius;
    // Ranges linearly from 5x max ship health (at distance 0) to 0.5x
    // max ship health (at the maximum distance)
    const auto max_ship_hp = hlt::GameConstants::get().MAX_SHIP_HEALTH;
    const auto damage = 5 * max_ship_hp -
                        (distance_from_crust / (2 * max_distance)) * max_ship_hp;
    return static_cast<unsigned short>(damage);
}

auto Halite::damage_entity(hlt::EntityId id, unsigned short damage,
                           double time) -> void {
    hlt::Entity &entity = game_map.get_entity(id);

    if (entity.health <= damage) {
        kill_entity(id, time);
    } else {
        entity.health -= damage;
    }
}

auto Halite::kill_entity(hlt::EntityId id, double time) -> void {
    hlt::Entity &entity = game_map.get_entity(id);
    if (!entity.is_alive()) return;

    entity.kill();

    auto location = entity.location;
    if (id.type == hlt::EntityType::ShipEntity) {
        // Make sure destruction location reflects the entity position at time
        // of death, not start of frame
        const auto &ship = game_map.get_ship(id);
        location.move_by(ship.velocity, time);
    }

    full_frame_events.back().push_back(
            std::unique_ptr<Event>(
                    new DestroyedEvent(id, location, entity.radius, time)));

    switch (id.type) {
        case hlt::EntityType::ShipEntity: {
            hlt::Ship &ship = game_map.get_ship(id);

            if (ship.docking_status != hlt::DockingStatus::Undocked) {
                auto &planet = game_map.planets.at(ship.docked_planet);
                planet.remove_ship(id.entity_index());
                ship.docking_status = hlt::DockingStatus::Undocked;
                ship.docked_planet = 0;
            }
            break;
        }
        case hlt::EntityType::PlanetEntity: {
            hlt::Planet &planet = game_map.get_planet(id);

            // Undock any ships
            for (const auto entity_index : planet.docked_ships) {
                auto &ship = game_map.get_ship(planet.owner, entity_index);
                ship.reset_docking_status();
            }

            const auto max_distance = std::max(
                    planet.radius, hlt::GameConstants::get().DOCK_RADIUS);
            auto caught_in_explosion = game_map.test(
                    planet.location, planet.radius + max_distance, time);

            for (const auto &target_id : caught_in_explosion) {
                if (target_id != id) {
                    const auto &target = game_map.get_entity(target_id);
                    const auto distance = planet.location.distance(target.location);
                    const auto damage = planet_explosion_damage(
                            planet, distance - target.radius, max_distance);
                    damage_entity(target_id, damage, time);
                }
            }

            break;
        }
        case hlt::EntityType::InvalidEntity: {
            assert(false);
        }
    }

    game_map.unsafe_kill_entity(id);
}

void Halite::kill_player(hlt::PlayerId player) {
    error_tags.insert((unsigned short) player);

    // Kill player's ships (don't process any side effects)
    for (auto &ship : game_map.ships.at(player)) {
        game_map.unsafe_kill_entity(hlt::EntityId::for_ship(player, ship.first));
    }
    game_map.cleanup_entities();

    // Make their planets unowned
    for (auto &planet : game_map.planets) {
        if (planet.owned && planet.owner == player) {
            planet.owned = false;
            planet.docked_ships.clear();
        }
    }
}

auto Halite::process_docking() -> void {
    // Update docking/undocking status
    for (hlt::PlayerId player_id = 0; player_id < players.size(); player_id++) {
        auto &player_ships = game_map.ships.at(player_id);
        for (auto &pair : player_ships) {
            const auto &ship_idx = pair.first;
            auto &ship = pair.second;

            // Planet should be alive - if it died, associated ships should
            // have been disengaged
            if (ship.docking_status == hlt::DockingStatus::Docking) {
                ship.docking_progress--;
                if (ship.docking_progress == 0) {
                    ship.docking_status = hlt::DockingStatus::Docked;
                }
            } else if (ship.docking_status == hlt::DockingStatus::Undocking) {
                ship.docking_progress--;
                if (ship.docking_progress == 0) {
                    ship.docking_status = hlt::DockingStatus::Undocked;
                    auto &planet = game_map.planets.at(ship.docked_planet);
                    planet.remove_ship(ship_idx);
                }
            } else if (ship.docking_status == hlt::DockingStatus::Docked) {
                ship.heal(hlt::GameConstants::get().DOCKED_SHIP_REGENERATION);
            }
        }
    }

    // Unfreeze frozen planets
    for (auto &planet : game_map.planets) {
        planet.frozen = false;
    }
}

auto Halite::process_production() -> void {
    // Update productions
    // We do this after processing moves so that a bot can't try to guess the
    // resulting ship ID and issue commands to it immediately
    CollisionMap collision_map = CollisionMap(
            game_map,
            [](const hlt::Ship &ship) -> double {
                return ship.radius;
            }
    );
    std::vector<hlt::EntityId> occupants;

    const auto infinite_resources = hlt::GameConstants::get().INFINITE_RESOURCES;

    for (hlt::EntityIndex planet_idx = 0;
         planet_idx < game_map.planets.size(); planet_idx++) {
        auto &planet = game_map.planets[planet_idx];
        if (!planet.is_alive() || !planet.owned) {
            continue;
        }

        const auto num_docked_ships = planet.num_docked_ships(game_map);
        if (num_docked_ships == 0) {
            continue;
        }

        const auto &constants = hlt::GameConstants::get();
        const auto base_productivity = constants.BASE_PRODUCTIVITY;
        const auto additional_productivity = constants.ADDITIONAL_PRODUCTIVITY;
        const auto production = std::min(
                planet.remaining_production,
                static_cast<unsigned short>(base_productivity +
                                            (num_docked_ships - 1) * additional_productivity));

        if (!infinite_resources) {
            planet.remaining_production -= production;
        }
        planet.current_production += production;

        const auto production_per_ship = hlt::GameConstants::get().PRODUCTION_PER_SHIP;
        while (planet.current_production >= production_per_ship) {
            // Try to spawn the ship
            auto best_location = std::make_pair(planet.location, false);
            auto best_distance = std::numeric_limits<double>::max();
            const auto &center = hlt::Location{
                    game_map.map_width / 2.0, game_map.map_height / 2.0};

            const auto max_delta = constants.SPAWN_RADIUS;
            const auto open_radius = constants.SHIP_RADIUS * 3;
            for (int dx = -max_delta; dx <= max_delta; dx++) {
                for (int dy = -max_delta; dy <= max_delta; dy++) {
                    double offset_angle = std::atan2(dy, dx);
                    double offset_x = dx + planet.radius * std::cos(offset_angle);
                    double offset_y = dy + planet.radius * std::sin(offset_angle);
                    auto location = game_map.location_with_delta(
                            planet.location, offset_x, offset_y);

                    if (!location.second) {
                        continue;
                    }

                    const auto distance = location.first.distance(center);

                    occupants.clear();
                    collision_map.test(location.first, open_radius, occupants);
                    const auto has_occupants =
                            game_map.any_planet_collision(location.first, open_radius) ||
                            game_map.any_collision(location.first, open_radius, occupants);

                    if (distance < best_distance && !has_occupants) {
                        best_distance = distance;
                        best_location = location;
                    }
                }
            }

            if (best_location.second) {
                planet.current_production -= production_per_ship;
                const auto ship_idx =
                        game_map.spawn_ship(best_location.first, planet.owner);
                total_ship_count[planet.owner]++;
                const auto id = hlt::EntityId::for_ship(planet.owner, ship_idx);
                full_frame_events.back().emplace_back(new SpawnEvent(
                        id, hlt::EntityId::for_planet(planet_idx),
                        best_location.first, planet.location));

                collision_map.add(best_location.first, game_map.get_ship(id).radius, id);
            } else {
                // Can't spawn any more - just keep the production there
                break;
            }
        }
    }
}

auto Halite::process_drag() -> void {
    // Update inertia/implement drag
    const auto drag = hlt::GameConstants::get().DRAG;
    for (auto &player_ships : game_map.ships) {
        for (auto &pair : player_ships) {
            auto &ship = pair.second;

            const auto magnitude = ship.velocity.magnitude();
            if (magnitude <= drag) {
                ship.velocity.vel_x = ship.velocity.vel_y = 0;
            } else {
                ship.velocity.accelerate_by(drag, ship.velocity.angle() + M_PI);
            }
        }
    }
}

auto Halite::process_cooldowns() -> void {
    for (auto &player_ships : game_map.ships) {
        for (auto &pair : player_ships) {
            auto &ship = pair.second;

            if (ship.weapon_cooldown > 0) {
                ship.weapon_cooldown--;
            }
        }
    }
}

auto Halite::process_docking_move(
        hlt::EntityId ship_id, hlt::Ship &ship,
        hlt::EntityIndex planet_id,
        SimultaneousDockMap &simultaenous_docking) -> void {
    const auto player_id = ship_id.player_id();
    const auto ship_idx = ship_id.entity_index();

    if (planet_id >= game_map.planets.size()) {
        // Planet is invalid, do nothing
        return;
    }

    auto &planet = game_map.planets.at(planet_id);
    if (!planet.is_alive() || !ship.can_dock(planet)) {
        // Ship too far/not stationary/etc
        return;
    }

    if (planet.frozen) {
        // Planet is frozen, accumulate damage
        simultaenous_docking[planet_id][player_id].push_back(ship_id);
        return;
    }

    if (!planet.owned) {
        planet.owned = true;
        planet.owner = player_id;
    }

    if (planet.owner == player_id &&
        planet.docked_ships.size() < planet.docking_spots) {
        ship.docked_planet = planet_id;
        ship.docking_status = hlt::DockingStatus::Docking;
        ship.docking_progress = hlt::GameConstants::get().DOCK_TURNS;
        planet.add_ship(ship_idx);
    } else if (planet.owner != player_id) {
        // If all the ships just started docking, then both
        // players tried to dock to the planet on the same turn
        if (std::all_of(
                planet.docked_ships.begin(),
                planet.docked_ships.end(),
                [&](hlt::EntityIndex ship_idx) -> bool {
                    const auto &ship = game_map.get_ship(planet.owner, ship_idx);
                    return ship.docking_status == hlt::DockingStatus::Docking &&
                           ship.docking_progress == hlt::GameConstants::get().DOCK_TURNS;
                })) {
            // In that case, nobody gets to dock
            assert(!planet.frozen);
            planet.frozen = true;

            for (auto &docked_ship_index : planet.docked_ships) {
                auto &ship = game_map.get_ship(planet.owner, docked_ship_index);
                ship.reset_docking_status();
                simultaenous_docking[planet_id][planet.owner].push_back(
                        hlt::EntityId::for_ship(planet.owner, docked_ship_index)
                );
            }

            planet.docked_ships.clear();
            planet.owned = false;
            planet.owner = 0;

            // Accumulate damage
            simultaenous_docking[planet_id][player_id].push_back(ship_id);
        }
    }
}

auto Halite::process_moves(std::vector<bool> &alive, int move_no) -> SimultaneousDockMap {
    // Keep track of which ships docked simultaneously
    SimultaneousDockMap simulataneous_docking;

    for (hlt::PlayerId player_id = 0; player_id < players.size(); player_id++) {
        if (!alive[player_id]) {
            continue;
        }
        auto &player_ships = game_map.ships.at(player_id);

        for (auto &pair : player_ships) {
            const auto ship_idx = pair.first;
            auto &ship = pair.second;

            if (player_moves[player_id][move_no].count(ship_idx) == 0) {
                continue;
            }

            auto move = player_moves[player_id][move_no][ship_idx];
            switch (move.type) {
                case hlt::MoveType::Noop:
                    break;
                case hlt::MoveType::Error:
                    break;
                case hlt::MoveType::Thrust: {
                    if (ship.docking_status != hlt::DockingStatus::Undocked) {
                        break;
                    }
                    auto angle = move.move.thrust.angle * M_PI / 180.0;
                    ship.velocity.accelerate_by(move.move.thrust.thrust, angle);
                    break;
                }
                case hlt::MoveType::Dock: {
                    const auto planet_id = move.move.dock_to;
                    process_docking_move(
                            hlt::EntityId::for_ship(player_id, ship_idx),
                            ship,
                            planet_id,
                            simulataneous_docking);
                    break;
                }

                case hlt::MoveType::Undock: {
                    if (ship.docking_status != hlt::DockingStatus::Docked)
                        break;

                    ship.docking_status = hlt::DockingStatus::Undocking;
                    ship.docking_progress = hlt::GameConstants::get().DOCK_TURNS;
                    break;
                }
            }

            auto &move_set = full_player_moves.back();
            auto &player_moves = move_set[player_id];
            auto &ship_moves = player_moves[move_no];
            ship_moves[ship_idx] = move;
        }
    }

    return simulataneous_docking;
}

auto Halite::find_living_players() -> std::vector<bool> {
    std::vector<bool> still_alive(players.size(), false);
    std::fill(last_ship_count.begin(), last_ship_count.end(), 0);
    std::vector<int> owned_planets(players.size(), 0);
    auto total_planets = 0;

    for (hlt::PlayerId player = 0; player < players.size(); player++) {
        for (const auto &pair : game_map.ships.at(player)) {
            still_alive[player] = true;
            last_ship_count[player]++;
            last_ship_health_total[player] += pair.second.health;
        }
    }

    for (const auto &planet : game_map.planets) {
        if (!planet.is_alive()) {
            continue;
        }

        total_planets++;
        if (planet.owned && !planet.docked_ships.empty()) {
            // Only count a planet as owned if a ship has completed docking
            const auto num_docked_ships = planet.num_docked_ships(game_map);
            if (num_docked_ships > 0) {
                owned_planets[planet.owner]++;
            }
        }
    }

    // If one player owns all living planets, that player wins
    for (int player_id = 0; player_id < owned_planets.size(); player_id++) {
        auto num_owned_planets = owned_planets[player_id];
        if (num_owned_planets == total_planets) {
            // End the game by "killing off" the other players
            std::fill(still_alive.begin(), still_alive.end(), false);
            // If there's only one player, let the game end
            if (players.size() > 1) {
                still_alive[player_id] = true;
            }
        }
    }
    return still_alive;
}

auto Halite::process_events() -> void {
    std::unordered_set<SimulationEvent> unsorted_events;

    const auto event_horizon = [](const hlt::Ship &ship) -> double {
        // The size of the ship's event horizon
        return ship.radius + ship.velocity.magnitude() +
               hlt::GameConstants::get().WEAPON_RADIUS;
    };

    CollisionMap collision_map = CollisionMap(game_map, event_horizon);
    std::vector<hlt::EntityId> potential_collisions;

    for (hlt::PlayerId player1 = 0; player1 < players.size(); player1++) {
        for (const auto &pair1 : game_map.ships.at(player1)) {
            const auto &id1 = hlt::EntityId::for_ship(player1, pair1.first);
            const auto &ship1 = pair1.second;

            potential_collisions.clear();
            collision_map.test(
                    pair1.second.location, event_horizon(pair1.second),
                    potential_collisions);
            for (const auto &id2 : potential_collisions) {
                const auto &ship2 = game_map.get_ship(id2);
                find_events(unsorted_events, id1, id2, ship1, ship2);
            }

            // Possible ship-planet collisions
            for (hlt::EntityIndex planet_idx = 0; planet_idx < game_map.planets.size(); planet_idx++) {
                const auto &planet = game_map.planets[planet_idx];
                if (!planet.is_alive()) {
                    continue;
                }

                const auto distance = ship1.location.distance(planet.location);

                if (distance <= ship1.velocity.magnitude() + ship1.radius + planet.radius) {
                    const auto collision_radius = ship1.radius + planet.radius;
                    const auto t = collision_time(collision_radius, ship1, planet);
                    if (t.first) {
                        if (t.second >= 0 && t.second <= 1) {
                            unsorted_events.insert(SimulationEvent{
                                    SimulationEventType::Collision,
                                    id1, hlt::EntityId::for_planet(planet_idx),
                                    round_event_time(t.second),
                            });
                        }
                    } else if (distance <= collision_radius) {
                        // This should never happen - they should already have
                        // collided
                        assert(false);
                    }
                }
            }

            // Look for ships trying to desert (final location is off map edge)
            // No case where the ship can be off the map edge in the middle of a
            // turn but end inside the map (map is convex) given that they start
            // within the boundaries
            auto final_location = ship1.location;
            final_location.move_by(ship1.velocity, 1.0);

            if (!game_map.within_bounds(final_location)) {
                auto time = 1000000.0;
                if (ship1.velocity.vel_x != 0.0) {
                    const auto t1 = -ship1.location.pos_x / ship1.velocity.vel_x;
                    if (t1 < time && t1 >= 0) time = t1;
                    const auto t2 = (game_map.map_width - ship1.location.pos_x)
                                    / ship1.velocity.vel_x;
                    if (t2 < time && t2 >= 0) time = t2;
                }

                if (ship1.velocity.vel_y != 0.0) {
                    const auto t3 = -ship1.location.pos_y / ship1.velocity.vel_y;
                    if (t3 < time && t3 >= 0) time = t3;
                    const auto t4 = (game_map.map_height - ship1.location.pos_y)
                                    / ship1.velocity.vel_y;
                    if (t4 < time && t4 >= 0) time = t4;
                }

                // The time here might actually be slightly off. Example:
                // pos_y is 156.5, map_height is 160, and vel_y is
                // -3.4999999999999996. When added, pos_y + vel_y is 160, but
                // (map_height - pos_y) / vel_y is 1.0000000000000002.

                // I have chosen to let the ship crash, allowing the rounding
                // to take care of the time, since we know the final location
                // would be definitively out of bounds.

                unsorted_events.insert(SimulationEvent{
                        SimulationEventType::Desertion,
                        id1, id1, round_event_time(time),
                });
            }
        }
    }

    std::vector<SimulationEvent> sorted_events(unsorted_events.begin(),
                                               unsorted_events.end());
    std::sort(
            sorted_events.begin(), sorted_events.end(),
            [](const SimulationEvent &ev1, const SimulationEvent &ev2) -> bool {
                // Sort in reverse since we're using as a queue
                return ev1.time > ev2.time;
            });

    while (!sorted_events.empty()) {
        // Gather all events that occurred simultaneously
        std::vector<SimulationEvent> simultaneous_events{sorted_events.back()};
        sorted_events.pop_back();

        while (!sorted_events.empty() &&
               sorted_events.back().time == simultaneous_events.back().time) {
            simultaneous_events.push_back(sorted_events.back());
            sorted_events.pop_back();
        }

        // Get rid of events involving dead entities
        simultaneous_events.erase(
                std::remove_if(
                        simultaneous_events.begin(), simultaneous_events.end(),
                        [&](const SimulationEvent &ev) -> bool {
                            return !game_map.is_valid(ev.id1) || !game_map.is_valid(ev.id2);
                        }),
                simultaneous_events.end());
        if (simultaneous_events.empty()) {
            continue;
        }

        DamageMap damage_map;
        std::unordered_map<hlt::EntityId, int> target_count;
        std::unordered_map<hlt::EntityId, AttackEvent> attackers;

        auto update_targets = [&](hlt::EntityId src, hlt::EntityId target, double time) -> void {
            auto &attacker = game_map.get_ship(src);

            if (!attacker.is_alive() || attacker.weapon_cooldown > 0 ||
                attacker.docking_status != hlt::DockingStatus::Undocked) {
                return;
            }
            // Don't update the actual cooldown until later
            if (attackers.count(src) == 0) {
                attackers.insert({src, AttackEvent(src, attacker.location, time, {}, {})});
            }
            auto &attack_event = attackers.at(src);
            attack_event.targets.push_back(target);
            attack_event.target_locations.push_back(
                    game_map.get_ship(target).location);

            auto num_prev_targets = 0;
            if (target_count.count(src) > 0) {
                num_prev_targets = target_count[src];
            }

            target_count[src] = num_prev_targets + 1;
            damage_dealt[src.player_id()] += hlt::GameConstants::get().WEAPON_DAMAGE;
        };

        for (SimulationEvent ev : simultaneous_events) {
            switch (ev.type) {
                case SimulationEventType::Collision: {
                    const auto damage = compute_damage(ev.id1, ev.id2);
                    damage_entity(ev.id1, damage.first, ev.time);
                    damage_entity(ev.id2, damage.second, ev.time);
                    break;
                }

                case SimulationEventType::Desertion: {
                    const auto damage = game_map.get_entity(ev.id1).health;
                    damage_entity(ev.id1, damage, ev.time);
                    break;
                }

                case SimulationEventType::Attack: {
                    update_targets(ev.id1, ev.id2, ev.time);
                    update_targets(ev.id2, ev.id1, ev.time);
                    break;
                }
            }
        }

        auto update_damage = [&](hlt::EntityId src, hlt::EntityId target) -> void {
            auto &attacker = game_map.get_ship(src);

            // This sets the cooldown too eagerly, but we don't check
            // it again in this frame anyways. There's no need to
            // validate any properties of the attacker - we've already
            // verified them above.
            attacker.weapon_cooldown = hlt::GameConstants::get().WEAPON_COOLDOWN;

            auto prev_damage = 0.0;
            if (damage_map[target.player_id()].count(target.entity_index()) > 0) {
                prev_damage = damage_map[target.player_id()][target.entity_index()];
            }
            const auto new_damage = hlt::GameConstants::get().WEAPON_DAMAGE / static_cast<double>(target_count[src]);
            damage_map[target.player_id()][target.entity_index()] = prev_damage + new_damage;
        };

        for (const auto &pair : attackers) {
            full_frame_events.back().push_back(
                    std::unique_ptr<Event>(new AttackEvent(pair.second)));
            // Use the AttackEvents generated above to actually
            // perform attack calculations. This way, we only perform
            // damage calculations when we're sure there was actually
            // an attack.
            for (const auto &target: pair.second.targets) {
                update_damage(pair.first, target);
            }
        }

        process_damage(damage_map, simultaneous_events.back().time);

        game_map.cleanup_entities();
    }
}

auto Halite::process_damage(DamageMap &ship_damage, double time) -> void {
    for (hlt::PlayerId player_id = 0; player_id < players.size(); player_id++) {
        const auto player_damage = ship_damage[player_id];

        for (const auto &pair : player_damage) {
            const auto ship_idx = pair.first;
            const auto damage = static_cast<unsigned short>(pair.second);

            damage_entity(hlt::EntityId::for_ship(player_id, ship_idx),
                          damage, time);
        }
    }
}

auto Halite::process_movement() -> void {
    for (hlt::PlayerId player_id = 0; player_id < players.size();
         player_id++) {
        for (auto &ship_pair : game_map.ships.at(player_id)) {
            auto &ship = ship_pair.second;
            ship.location.move_by(ship.velocity, 1.0);
        }
    }
}

auto Halite::process_dock_fighting(SimultaneousDockMap simultaneous_docking) -> void {
    // Have ships that tried to dock simultaneously fight each other
    const auto damage = hlt::GameConstants::get().WEAPON_DAMAGE;
    const auto cooldown = hlt::GameConstants::get().WEAPON_COOLDOWN;

    // Process each planet separately
    for (const auto &planet_entry : simultaneous_docking) {
        DamageMap damage_map;

        std::vector<hlt::EntityId> participants;
        std::vector<hlt::Location> participant_locations;

        auto damage_others = [&](hlt::PlayerId src_player, double split_damage) {
            for (auto &other_player : planet_entry.second) {
                if (other_player.first == src_player) {
                    continue;
                }

                for (auto &other_ship : other_player.second) {
                    damage_map[other_player.first][other_ship.entity_index()] += split_damage;
                }
            }
        };

        auto total = 0;
        for (const auto &player_entry : planet_entry.second) {
            total += player_entry.second.size();
        }

        // Process each player individually, summing the damage from enemy ships
        for (const auto &player_entry : planet_entry.second) {
            const auto total_enemies = total - player_entry.second.size();
            const auto split_damage = ((double) damage) / total_enemies;
            for (const auto &ship_id : player_entry.second) {
                if (!game_map.is_valid(ship_id)) {
                    continue;
                }

                auto &ship = game_map.get_ship(ship_id);
                if (!ship.is_alive() || ship.weapon_cooldown != 0) {
                    continue;
                }

                participants.push_back(ship_id);
                participant_locations.push_back(ship.location);

                ship.weapon_cooldown = cooldown;
                damage_others(player_entry.first, split_damage);
            }
        }

        process_damage(damage_map, 0.0);
        game_map.cleanup_entities();

        const auto planet_id = hlt::EntityId::for_planet(planet_entry.first);
        full_frame_events.back().emplace_back(
                std::unique_ptr<Event>(new ContentionAttackEvent(
                        planet_id,
                        game_map.get_planet(planet_id).location,
                        participants,
                        participant_locations
                ))
        );
    }
}

/*
 * PUBLIC FUNCTIONS
 */

std::string Halite::get_name(hlt::PlayerId player_tag) {
    return players[player_tag];
}

Halite::Halite(unsigned short width_, unsigned short height_, unsigned int seed_,
               const std::vector<std::string> &players_) {
    auto generator = mapgen::SolarSystem(seed_);
    generator_name = generator.name();
    seed = seed_;
    game_map = hlt::Map(width_, height_);
    points_of_interest = generator.generate(game_map, static_cast<unsigned int>(players_.size()));
    constants = hlt::GameConstants::get();
    max_turn_number = std::min(constants.MAX_TURNS, 100U + (int) (sqrt(game_map.map_width * game_map.map_height)));

    // Default initialize
    player_moves = {{{{}}}};
    turn_number = 0;
    players = players_;

    // Add to full game:
    full_frames.push_back({hlt::Map(game_map)});

    // Init statistics
    auto player_count = players_.size();
    alive_frame_count = std::vector<unsigned short>(player_count, 1);
    last_ship_count = std::vector<unsigned int>(player_count);
    last_ship_health_total = std::vector<unsigned int>(player_count);
    total_ship_count = std::vector<unsigned int>(player_count);
    damage_dealt = std::vector<unsigned int>(player_count);
    error_tags = std::set<unsigned short>();

    // For rankings
    living_players = std::vector<bool>(player_count, true);

    // Sort ranking by number of ships, using total ship health to break ties.
    comparator = std::bind(&Halite::compare_rankings, this, std::placeholders::_1, std::placeholders::_2);

    // Rand generator
    rng = std::mt19937(seed);
}

bool Halite::game_ended() {
    const auto num_living_players = std::count(living_players.begin(), living_players.end(), true);
    return turn_number >= max_turn_number ||
           (num_living_players <= 1 && players.size() > 1) ||
           (num_living_players == 0 && players.size() == 1);
}

void Halite::step() {
    auto alive = find_living_players();

    // Update alive frame counts
    for (hlt::PlayerId player_id = 0; player_id < players.size(); player_id++)
        if (alive[player_id]) alive_frame_count[player_id]++;

    full_frame_events.emplace_back();
    full_player_moves.push_back({{{}}});

    process_docking();
    // Process queue of moves
    for (int move_no = 0; move_no < hlt::MAX_QUEUED_MOVES; move_no++) {
        auto simultaneous_docked = process_moves(alive, move_no);
        process_dock_fighting(simultaneous_docked);

        process_events();
        process_movement();
    }

    process_production();
    process_drag();
    process_cooldowns();

    // Clear moves for next step
    for (auto &row : player_moves) {
        for (auto &subrow : row) {
            subrow.clear();
        }
    }

    // Save map for the replay
    full_frames.emplace_back(game_map);

    alive = find_living_players();

    // Add to vector of players that should be dead.
    std::vector<hlt::PlayerId> new_rankings;
    for (hlt::PlayerId player_id = 0; player_id < players.size(); player_id++) {
        if (living_players[player_id] && !alive[player_id]) {
            new_rankings.push_back(player_id);
        }
    }

    // Shuffle the rankings first, to ensure that in the case of a
    // tie, a random winner is chosen.
    std::shuffle(new_rankings.begin(), new_rankings.end(), rng);
    std::stable_sort(new_rankings.begin(), new_rankings.end(), comparator);
    rankings.insert(rankings.end(), new_rankings.begin(), new_rankings.end());

    if (game_ended()) finish();
}

void Halite::finish() {
    // Add remaining players to the ranking. Break ties using the same
    // comparison function.
    std::vector<hlt::PlayerId> new_rankings;
    for (hlt::PlayerId player_id = 0; player_id < players.size(); player_id++) {
        if (living_players[player_id]) new_rankings.push_back(player_id);
    }
    std::shuffle(new_rankings.begin(), new_rankings.end(), rng);
    std::stable_sort(new_rankings.begin(), new_rankings.end(), comparator);
    rankings.insert(rankings.end(), new_rankings.begin(), new_rankings.end());

    // Best player first rather than last.
    std::reverse(rankings.begin(), rankings.end());
}

void Halite::do_move(unsigned int player_id, const hlt::entity_map<hlt::ShipMove> &moves) {
    hlt::entity_map<hlt::Move> _moves;
    for (auto &pair : moves) {
        const auto ship_idx = pair.first;
        auto move = pair.second;
        _moves.insert(std::make_pair(ship_idx, move.get_move()));
    }

    player_moves.at(player_id)[0] = _moves;
}

const hlt::Map &Halite::get_map() const {
    return game_map;
}

const hlt::GameConstants &Halite::get_constants() const {
    return constants;
}

void Halite::save_replay(std::string filepath) {
    GameStatistics stats = GameStatistics();
    for (hlt::PlayerId player_id = 0; player_id < players.size(); player_id++) {
        PlayerStatistics p{};
        p.tag = player_id;
        p.rank = static_cast<int>(
                std::distance(rankings.begin(), std::find(rankings.begin(), rankings.end(), player_id)) + 1);
        // alive_frame_count counts frames, but the frames are 0-base indexed (at least in the visualizer), so everyone needs -1 to find the frame # where last_alive
        // however, the first place player and 2nd place player always have the same reported alive_frame_count (not sure why)
        // it turns out to make "last_frame_alive" match what is seen in replayer, we have to -2 from all but finishers who are alive in last frame of game who only need -1
        p.last_frame_alive = alive_frame_count[player_id] - 2 + living_players[player_id];
        p.init_response_time = -1;
        p.average_frame_response_time = -1; //In milliseconds.
        p.total_ship_count = total_ship_count[player_id];
        p.damage_dealt = damage_dealt[player_id];
        stats.player_statistics.push_back(p);
    }

    Replay replay = {
            stats,
            static_cast<unsigned short>(players.size()),
            players,
            seed, generator_name, points_of_interest,
            game_map.map_width, game_map.map_height,
            full_frames, full_frame_events, full_player_moves,
    };

    stats.output_filename = filepath;
    try {
        replay.output(stats.output_filename);
    }
    catch (std::runtime_error &e) {
        stats.output_filename = filepath;
        replay.output(stats.output_filename);
    }
}

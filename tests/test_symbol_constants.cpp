#include "game_symbols.hpp"

#include <cstdint>
#include <type_traits>

static_assert(fixture::game::Game_Update == 0x00401000u);
static_assert(fixture::game::Game_Loop == 0x00401fffu);
static_assert(fixture::game::Game_State == 0x00402000u);
static_assert(fixture::game::Game_State_size == 0x40u);
static_assert(fixture::game::Game_Flags == 0x00402ffcu);
static_assert(std::is_same_v<decltype(fixture::game::Game_Update), const std::uint32_t>);

int main() {
  return 0;
}

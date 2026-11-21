#pragma once
#include <windows.h>
#include <string_view>

namespace aco {
template<class Char>
int function_key(std::basic_string_view<Char> key) {
  if (key.size() < 2 || key.size() > 3 || (key.front() != 'F' && key.front() != 'f')) return 0;
  int number = 0;
  for (Char digit : key.substr(1)) {
    if (digit < '0' || digit > '9') return 0;
    number = number * 10 + int(digit - '0');
  }
  return number >= 1 && number <= 24 ? VK_F1 + number - 1 : 0;
}
}  // namespace aco

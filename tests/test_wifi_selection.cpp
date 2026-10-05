#include "WifiSelection.hpp"
#include <cassert>
#include <cstdio>

int main() {
  int rssi[] = {-60, -30, -127};
  int64_t retry[] = {0, 0, 0};
  assert(live_wifi::select(rssi, retry, 3, 0) == 1);
  // An authentication/DHCP failure on the strongest AP must allow fallback.
  retry[1] = 30000;
  assert(live_wifi::select(rssi, retry, 3, 15000) == 0);
  rssi[0] = -127;
  assert(live_wifi::select(rssi, retry, 3, 15000) == -1);
  // A temporarily failed network becomes eligible after the bounded backoff.
  assert(live_wifi::select(rssi, retry, 3, 30000) == 1);
  rssi[2] = -20;
  assert(live_wifi::select(rssi, retry, 3, 30000) == 2);
  rssi[0] = rssi[1] = rssi[2] = -127;
  assert(live_wifi::select(rssi, retry, 3, 30000) == -1);
  assert(live_wifi::select(rssi, retry, 0, 30000) == -1);
  puts("Wi-Fi selection and recovery passed");
}

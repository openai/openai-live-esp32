#include "TalkButton.hpp"
#include <cassert>

int main() {
  board::TalkButton b;
  assert(!b.update(false, 0));
  assert(!b.update(true, 1));
  assert(!b.update(false, 10)); // Brief noise must not start recording.
  assert(!b.update(true, 15));
  assert(!b.update(true, 34));
  assert(b.update(true, 35) && b.held);
  assert(!b.update(true, 10000)); // A long hold emits just one press.
  assert(b.update(false, 10001) && !b.held); // Unplug/release is immediate.
  assert(!b.update(true, 10002)); // Repress must debounce again.
  assert(!b.update(false, 10005));
  assert(!b.update(true, 10006));
  assert(b.update(true, 10026) && b.held);
  assert(b.update(false, 10027) && !b.held);
}

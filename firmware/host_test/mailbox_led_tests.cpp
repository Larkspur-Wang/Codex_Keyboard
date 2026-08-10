#include "keyboard/mailbox_led.h"

#include <cassert>
#include <cstdint>

int main() {
  const auto empty = easy_codex::mailbox_color_for_coverage(0U);
  assert(empty.red == 0U && empty.green == 0U && empty.blue == 0U);

  auto previous = easy_codex::mailbox_color_for_coverage(1U);
  assert(previous.green > 0U);
  for (std::uint32_t coverage = 2U; coverage <= 16U; ++coverage) {
    const auto current = easy_codex::mailbox_color_for_coverage(coverage);
    assert(current.green > previous.green);
    previous = current;
  }
  assert(easy_codex::mailbox_color_for_coverage(UINT32_MAX).green ==
         easy_codex::mailbox_color_for_coverage(16U).green);

  const auto idle = easy_codex::task_activity_color(0U);
  const auto one = easy_codex::task_activity_color(1U);
  const auto two = easy_codex::task_activity_color(2U);
  const auto three = easy_codex::task_activity_color(3U);
  const auto four = easy_codex::task_activity_color(4U);
  assert(idle.red == 0U && idle.green > 0U && idle.blue > 0U);
  assert(one.red > 0U && one.green > 0U && one.blue == 0U);
  assert(two.red > one.red && two.green < one.green && two.blue == 0U);
  assert(three.red > 0U && three.green == 0U && three.blue > three.red);
  assert(four.red > 0U && four.green == 0U && four.blue == 0U);
  assert(easy_codex::task_activity_color(255U).red == four.red);

  const auto frame = easy_codex::mailbox_frame_for_slots({0U, 1U, 4U, 0U}, 2U);
  assert(frame[0].red == two.red && frame[0].green == two.green);
  assert(frame[1].green == 0U);
  assert(frame[2].green > frame[3].green);
  assert(frame[3].green > 0U);
  assert(frame[4].red == 0U && frame[4].green == 0U && frame[4].blue == 0U);
  return 0;
}

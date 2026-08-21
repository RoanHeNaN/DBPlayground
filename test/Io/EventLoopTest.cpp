//
// N0 acceptance: a single-threaded reactor dispatches an fd's read event to its
// Channel callback, and Quit() from inside that callback stops the loop.
//

#include <gtest/gtest.h>
#include <unistd.h>

#include "Io/Channel.h"
#include "Io/EventLoop.h"

namespace dbplay {

TEST(EventLoopTest, DispatchesReadEventThenQuits) {
  int fds[2];
  ASSERT_EQ(::pipe(fds), 0);

  EventLoop loop;
  Channel channel(&loop, fds[0]);

  bool fired = false;
  channel.SetReadCallback([&]() {
    char buf[8];
    ssize_t n = ::read(fds[0], buf, sizeof(buf));
    EXPECT_EQ(n, 1);
    fired = true;
    loop.Quit();
  });
  channel.EnableReading();

  // Make the read end readable before entering the loop so poll returns at once.
  ASSERT_EQ(::write(fds[1], "x", 1), 1);

  loop.Loop();

  EXPECT_TRUE(fired);

  channel.DisableAll();
  channel.Remove();
  ::close(fds[0]);
  ::close(fds[1]);
}

TEST(EventLoopTest, RemoveChannelUnregistersFd) {
  int fds[2];
  ASSERT_EQ(::pipe(fds), 0);

  EventLoop loop;
  auto channel = std::make_unique<Channel>(&loop, fds[0]);
  channel->EnableReading();
  EXPECT_TRUE(loop.HasChannel(channel.get()));

  channel->DisableAll();
  channel->Remove();
  EXPECT_FALSE(loop.HasChannel(channel.get()));

  ::close(fds[0]);
  ::close(fds[1]);
}

}  // namespace dbplay

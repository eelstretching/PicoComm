I'm planning on having multiple props in the Christmas show: the big megatree (with topper), the leaping arches (either 4 or 6, depending on what I can manage), maybe some smaller trees made out of last year's pixels. So there will be somewhere between 4 and 8 Pico MCUs out on the lawn.

Most folks would use xlights and commercial controllers to do this, but xlights is no fun (to me). Building the sequences on a desktop and just shipping pixels to the controllers is fine, but it's just not for me.

The whole point of the PicoLEDs, universal-strip-controller, and MegaTree repositories is having the computation that decides what to display and then actually computing the display  done on the Pico itself.

Now, xlights says that you cannot use wireless networking to run a show, due to the lag. I believe that when you're shipping frames for a multi-thousand pixel show. 

But what I want to do is to have the different props in my show communicate with each other about what they're doing and to have them collaborate on the show. So, if the big MegaTree is about to switch to one effect or another, it can communicate that to the other trees and have them run the same effect, or perhaps a complementary one, or maybe something else. It could be as simple as coordinated brightening and dimming.

It seems to me that a coordination and communication protocol should be able to work over wifi (even old wifi) if we design it right. 

Let's assume that all of the Picos running props are all on the same wifi network. I'm going to assume for the moment that only one Pico has an RTC. The Pico that has the RTC is the leader. It's responsible for discovering other Picos, which we'll call followers. So we're going to need discovery via something like multicast or just enumerating IPs (maybe in a range?)

Then we need for the Picos to sync on their notion of time. It seems to me that we should be able to get the Picos within a millisecond of each other, which should allow us to sync up in less than an animation frame, which should allow changes to be imperceptible to humans.

The we need some kind of communication protocol where the leader can tell a follower (or some set of followers, or all followers?): hey in n milliseconds, you should switch to effect e with parameters <foo>.

We can start simple and assume that the set of effects and their paramters are known to all and can be represented with simple int_8 (or maybe int_16?). The communication protocol could be as simple as a 16 or 32 bytes.

It feels like this could all be running on the second core of the MCUs while the first core is computing the show. 

Only thing I'm worried about is the shows could be memory intensive.

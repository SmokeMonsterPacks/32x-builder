# The chair sits in the light, the ceiling turns, the buzz holds

Three things landed in build 165, and they share a theme. The backrooms
should feel like a room you are standing in, not a wall of textures. The
furniture has to respond to the light, the ceiling has to move the way a
real ceiling would as you turn, and the ambient hum has to sound like a
hum instead of a scratched loop. All three got there this build.

## The chair sits in the light

We already had a chair that swaps between real 3D geometry up close and a
flat billboard far away. That level-of-detail trick keeps the cost down
when a dozen of them line a corridor. This build fixed the two things
that gave the trick away.

First, the color pop. Walking toward a chair, it used to visibly change
shade at the swap distance. The near 3D model shaded its faces from the
live world light, while the far billboard was baked at a fixed
brightness, so the two never matched. The fix was to stop asking the
world where the light is and shade each face by which way it points on
the model itself. Top face brightest, the two sides a step down, the
underside darkest. Now the 3D chair and the billboard agree, and the
swap is invisible.

Second, the render itself. The 3D chair is nine small boxes: four posts,
a seat, the back rails. They draw back to front. Two bugs were fighting
it. The back-face cull was inverted, so we were drawing the inside of
every box and throwing away the outside, and the boxes overlapped enough
to leave sorting slivers where they met. Splitting the model into boxes
that never intersect makes the back-to-front sort exact, and flipping the
cull sign put the right faces forward. The chair reads as a solid object
now.

The nice surprise was the close-up flicker. The fluorescent lights in
this world flicker, and once the chair shaded correctly it was cheap to
have the seat dim by a single shade in sync with the nearest failing
light. You do not consciously notice it happen. You notice that the chair
belongs to the room.

## The ceiling turns with you

The old ceiling was a flat grid. It was fine, but it did not sell "office
drop-ceiling," and the grid lines stayed put in a way that looked wrong
when you turned in place.

Build 165 replaces it with rectangular ceiling panels and fluorescent
troffer fixtures that are real geometry in world space. The two bright
tubes inside each light are quads projected into the plane of the
ceiling, so when you turn, the tubes rotate the way the panels do. They
are not billboards pinned to the screen. They are part of the room.

The colors came off a still from the movie: a soft warm-white plate with
two slightly brighter tube strips, the variance kept subtle so it reads
as one fixture instead of three stripes. Fog on the ceiling lights is
deliberately gentle. A light down the hall should still read as a light,
not fade to grey at the same rate a wall does.

Getting there took a few wrong turns. Making the grid denser in one axis
to fake the panels broke the rotation. Splitting the ceiling by tile
density broke it a different way. The version that worked was the
simplest one: keep the square grid, and add the light fixtures as their
own world-space quads on top. When the room fights you, the answer is
usually less machinery, not more.

## The buzz finally holds

This is the one I am happiest about, because the fix was not where the
symptom was.

The ambient audio plays an electrical buzz, an occasional neon sting, the
Voyager Golden Record hellos near the wandering figure, and footsteps on
carpet. It played, but under load it broke into a rhythmic chop, worst in
the dense rooms where the frame rate sags. The obvious suspect is the
mixer. The mixer was fine. The bug was scheduling.

Audio streams to the speaker from a pair of small buffers. The hardware
drains one while the second CPU refills the other. That refill only ran
when the second CPU was idle. During a heavy render the second CPU is
busy drawing walls for tens of milliseconds and never goes idle, so
nothing refills. The buffers held about 32 milliseconds of sound between
them. A dense frame takes 30 to 60 milliseconds to draw. The math does
not close. The speaker runs out of fresh audio mid-frame, the hardware
wraps back to the start of the buffer, and you hear the previous fragment
again. The chop was a tiny broken record.

The fix has two parts. Bigger buffers for more runway, 64 milliseconds
each now, and refill checkpoints wedged between the render passes so the
longest the audio can go untended is one pass instead of a whole frame.
Together they close the gap.

I added a counter for it. Every time the hardware swaps into a buffer
nobody refilled, it ticks up, and it shows on the debug overlay as AU.
Zero means healthy. A menu switch puts the old small buffers back, so you
can watch the counter climb and hear the chop return, then flip it and
watch both stop. A bug you can turn on and off on demand is a bug you
have actually understood.

One near miss is worth telling. The first buffer size I tried was twice
as big, and it built and linked without a single warning. It also pushed
the audio buffers past the top of the CPU's stack. On a machine with no
memory protection that is a silent corruption waiting for a deep enough
call chain to trip it. The linker did not catch it, because the memory
map it was handed ran a few kilobytes past where the stack actually sits.
Checking the symbol table against the real stack address is what caught
it. A clean build is not proof it fits.

## Try it

Build 165 is up on itch and as a GitHub release. If you have a real 32X
or a MiSTer, the audio is the thing to listen for: walk into a busy room
and the buzz should stay steady. Open the debug overlay (pause, VISUALS,
METRICS) and the AU counter should sit at zero.

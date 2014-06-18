Excise Boring Bits (ebb)
========================

This program is to help edit screencasts that have long periods where
nothing is happening.  e.g. a command may take minutes seconds to run.

It reads in an input video file and exports a series of PNGs, for each
frame.  It skips frames where nothing has changed from the previous frame.


Example usage
-------------

This will produce the PNG series in a pre-existing `out` directory.

The generated PNGs will be named frame00000000.png etc.

    $ ./ebb --verbose my-movie.mkv out/frame.png

To produce a video from the resulting PNGs, suitable for uploading to
Vimeo, you could run the following `ffmpeg` command

    $ ffmpeg -framerate 25 -i out/frame%08d.png -vcodec libx264 \
          -profile:v high -crf 10 -pix_fmt yuv420p -r 25 \
          my-movie-edited.mp4

This gives you `my-movie-edited.mp4` with the boring bits removed.

Note that depending on your distribution, or the age of your
distribution, it may be `avconv`, rather than `ffmpeg`.  They also
seem to have an unstable interface, and it may be `-c:v libx264`
rather than `-vcodec libx264`.

Also note that `-framerate 25` and `-r 25` values should match the framerate
of the original source, unless you want to alter the speed at which the
video plays.


Options
-------

### Slack

You can vary the amount of acceptable unchanging time by setting a
value in cs.  To allow the video to remain unchanging for 1 second,
pass `--slack 100`.

### Border

You can avoid changes at the edge of the screen being recognised as
real changes.  To set the width of this border region to 30 pixels,
pass `--border 30`.

### Splash screen

You can optionally set a PNG to use as a splash title screen.  Set
the path to the splash image after the output and input parameters:

    $ ./ebb --verbose my-movie.mkv out/frame.png splash.png

You can also vary the length of time that the splash screen is displayed
for.  To set a duration of 3 seconds pass `--intro 300`.

### Terminal Output

There are options to set the amount of output generated on the command
line.  These are:

* `--verbose` Useful info about which bits are getting skipped
* `--debug` Excessive info about what's happening
* `--quiet` Limit the output to warnings and errors


TODO
----

This program was created purely to swiftly edit a couple of videos down
to watchable length.  It may be useful again, and to other people.

General musings follow.

* Generate output video directly instead of via PNG series
  - I was using Debian stable and they have the libav libraries from
    after ffmpeg was forked as libav.  The APIs seem to be in an
    inconsistent state.
  - `avcodec_encode_video()` doesn't use an AVPacket, but
    `av_interleaved_write_frame()` does?
  - Would be much easier once `avcodec_encode_video2()` can be used.
* Could be improved to avoid the conversion to RGB to do the comparison.
  Which would make it a little faster.
* If a more coarse-grained editing is acceptable, we could only decode
  i-frames, and if there are no changes between i-frames, renumber the
  remaining frames.  This would avoid the need to re-encode the video,
  making it much faster, and non-lossy.
* Expose options for controlling error tolerance and neighbourhood stuff
  on command line.


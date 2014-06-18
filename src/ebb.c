/*
 * Copyright 2014 Codethink
 *
 * This file is part of ebb
 *
 * ebb is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * ebb is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Excise Boring Bits (ebb)
 *
 * This is a program for removing consecutive frames where nothing is changing
 * from a video.
 *
 * Generate final video with something like:
 *
 *     $ ffmpeg -framerate 25 -i out%08d.png -vcodec libx264 -profile:v high \
 *           -crf 20 -pix_fmt yuv420p -r 25 result.mp4
 */

#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

#include <png.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>

#include <unistd.h>

enum log_level {
	LOG_DEBUG,
	LOG_INFO,
	LOG_RESULT,
	LOG_WARNING,
	LOG_ERROR
};

enum log_level level;

#define LOG(lev, fmt, ...)				\
	if (lev >= level)				\
		printf(fmt, ##__VA_ARGS__);


#define SECOND_IN_CS	100
#define SLACK_TIME_CS	80
#define SPLASH_TIME_CS  300
#define PIXEL_TOLERANCE	((255 * 3) / 10)
#define BORDER 5

#define PATH_LEN (1024*1024)
char path[PATH_LEN];

struct {
	const char *input_path;		/**< Video */
	const char *output_path;	/**< Output path */
	const char *splash_path;	/**< Optional splash PNG path */
	int border;			/**< Border to ignore changes in (px) */
	int slack;			/**< Unchanging time to allow */
	int splash;			/**< Time to display splash */
} options;


/** Display usage/help text */
static void show_usage(const char *prog_name)
{
	printf("Usage:\n"
			"\t%s [options] <in_file> <out_file> [<splash_file>]\n"
			"\n", prog_name);

	printf("\t    <in_file> is path to video file\n");
	printf("\t   <out_file> is path to destination name\n");
	printf("\t<splash_file> is optional path to start screen PNG\n"
			"\n");

	printf("Options are:\n"
			"\t--help      -h     Display this text\n"
			"\t--border N  -b N   Set border in px (changes are ignored outside)\n"
			"\t--slack N   -s N   Set slack time in cs (unchanging time allowed)\n"
			"\t--intro N   -i N   Set time to show splash screen in cs\n"
			"\t--quiet     -q     Only report warnings and errors\n"
			"\t--verbose   -v     Verbose output\n"
			"\t--debug     -d     Debug output\n");
}


/** Save a AVFrame to disc as a PNG */
bool image_write_png(const char *file_name, const AVFrame *frame, int w, int h)
{
	FILE *fp;
	png_structp png_ptr;
	png_infop info_ptr;
	int colour_type;
	png_byte **rows;
	int y;

	colour_type = PNG_COLOR_TYPE_RGB;

	/* Open the file to save frame into */
	fp = fopen(file_name, "wb");
	if (fp == NULL)
		return false;

	/* png_write_image requires an array of row pointers, and AVFrames
	 * don't contain one, so make our own. */
	rows = malloc(h * sizeof(png_byte *));
	if (rows == NULL) {
		fclose(fp);
		return false;
	}
	for (y = 0; y < h; y++) {
		rows[y] = frame->data[0] + y * frame->linesize[0];
	}

	/* Create and initialize the png_struct */
	png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING,
			NULL, NULL, NULL);

	if (png_ptr == NULL) {
		fclose(fp);
		free(rows);
		return false;
	}

	/* Allocate/initialize the image information data. */
	info_ptr = png_create_info_struct(png_ptr);
	if (info_ptr == NULL) {
		fclose(fp);
		free(rows);
		png_destroy_write_struct(&png_ptr, NULL);
		return false;
	}
	/* Set error handling, needed because I gave NULLs to
	 * png_create_write_struct. */
	if (setjmp(png_jmpbuf(png_ptr))) {
		/* If we get here, we had a problem reading the file */
		fclose(fp);
		png_destroy_write_struct(&png_ptr, &info_ptr);
		return false;
	}

	/* set up the output control */
	png_init_io(png_ptr, fp);

	/* Set the image information. */
	png_set_IHDR(png_ptr, info_ptr, w, h, 8,
			colour_type, PNG_INTERLACE_ADAM7,
			PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

	/* Write the file header information. */
	png_write_info(png_ptr, info_ptr);

	/* pack pixels into bytes */
	png_set_packing(png_ptr);

	/* write the png */
	png_write_image(png_ptr, rows);
	free(rows);

	/* finish writing the rest of the file */
	png_write_end(png_ptr, info_ptr);

	/* clean up after the write, and free any memory allocated */
	png_destroy_write_struct(&png_ptr, &info_ptr);

	/* close the file */
	fclose(fp);

	return true;
}


/** Find the degree to which two pixels differ (0 to 255*3) */
static inline int pixel_difference(const uint8_t *prev, const uint8_t *curr)
{
	int pr = prev[0], pg = prev[1], pb = prev[2];
	int cr = curr[0], cg = curr[1], cb = curr[2];

	return abs(pr - cr) + abs(pg - cg) + abs(pb - cb);
}


/** Find whether two 2x2 pixel neighbourhoods may be considered different */
static inline bool neighbourhoods_differ(
		const uint8_t *prev_t, const uint8_t *prev_n,
		const uint8_t *curr_t, const uint8_t *curr_n,
		int x)
{
	int tl = pixel_difference(prev_t + x, curr_t + x);
	int tr = pixel_difference(prev_t + x + 3, curr_t + x + 3);
	int bl = pixel_difference(prev_n + x, curr_n + x);
	int br = pixel_difference(prev_n + x + 3, curr_n + x + 3);
	int different = 0;

	/* Check if difference is greater than tolerance threashold */
	if (tl > PIXEL_TOLERANCE)
		different++;
	if (tr > PIXEL_TOLERANCE)
		different++;
	if (bl > PIXEL_TOLERANCE)
		different++;
	if (br > PIXEL_TOLERANCE)
		different++;

	/* Only count it as different if three of the pixels had a difference */
	if (different > 3)
		return true;

	return false;
}


/** Find whether two frames many be considered different */
static bool frames_differ(AVFrame *frame_prev, AVFrame *frame_curr,
		int w, int h)
{
	const uint8_t *prev_t, *prev_n;
	const uint8_t *curr_t, *curr_n;
	int x, y;

	/* Don't check for differences within border */
	w -= 2 * options.border;
	h -= 2 * options.border;

	/* Since we're cheking a 2x2 pixel neighbourhood per pixel, don't
	 * need to look at last row/col */
	w--;
	h--;

	/* Row data length is 3 times pixel width, due to 3 colour channels */	
	w *= 3;

	/* Position of this row in current and previous frames */
	prev_t = frame_prev->data[0];
	curr_t = frame_curr->data[0];

	/* Check frames */
	for (y = options.border; y < h; y++) {
		/* Position of nect row in current and previous frames */
		prev_n = prev_t + frame_prev->linesize[0];
		curr_n = curr_t + frame_curr->linesize[0];

		/* Check for differences between frames on this row */
		for (x = options.border * 3; x < w; x += 3) {
			if (neighbourhoods_differ(
					prev_t, prev_n,
					curr_t, curr_n, x)) {
				/* Found a difference */
				return true;
			}
		}
		prev_t = prev_n;
		curr_t = curr_n;
	}

	/* No difference */
	return false;
}


/** Convert two frame numbers and an FPS to two times */
static inline void get_times(int f1, int f2, AVRational *fps,
		int *s1, int *m1, int *h1,
		int *s2, int *m2, int *h2)
{
	assert(f1 <= f2);

	*h1 = *h2 = *m1 = *m2 = 0;

	/* Find how many seconds we have */
	*s1 = f1 * fps->den / fps->num;
	*s2 = f2 * fps->den / fps->num;

	/* If we have enough seconds, put them in hours */
	if (*s1 / (60 * 60) > 0) {
		*h1 = *s1 / (60 * 60);
		*h2 = *s2 / (60 * 60);
		*s1 -= *h1 * (60 * 60);
		*s2 -= *h2 * (60 * 60);
	}

	/* If we have enough seconds, put them in minutes */
	if (*s1 / 60 > 0) {
		*m1 = *s1 / 60;
		*m2 = *s2 / 60;
		*s1 -= *m1 * 60;
		*s2 -= *m2 * 60;
	}
}


/** Dump splash screen frames */
static int dump_splash(const char *splash, int len, const char *output_path,
		AVRational *fps)
{
	int i;
	int lim = (options.splash * fps->num) / (SECOND_IN_CS * fps->den);

	for (i = 0; i < lim; i++) {
		sprintf(path, "%.*s%.08i.png", len, output_path, i);
		if (link(splash, path) < 0) {
			LOG(LOG_INFO, "Could not copy splash image %s\n",
					splash);
			break;
		}
	}

	LOG(LOG_INFO, "Splash frames: %i\n", i);

	return i;
}


bool excise_boring_bits(AVFormatContext *fmt_ctx, AVCodecContext *dec_ctx,
		int stream_id, AVStream *vs)
{
	AVPacket pkt;
	AVFrame *frame = NULL;
	AVFrame *frame_curr = NULL;
	AVFrame *frame_prev = NULL;
	AVFrame *frame_tmp = NULL;
	struct SwsContext *img_convert_ctx = NULL;
	int s1, s2, m1, m2, h1, h2;
	bool res = false;
	int frames = 0; /* Current frame count */
	int out_frames;
	int skip = 0; /* Current count of frames to skip*/
	int path_len = strlen(options.output_path);
	int ret;
	size_t len;
	uint8_t *buffer;
	const int slack = options.slack * vs->avg_frame_rate.num /
			(SECOND_IN_CS * vs->avg_frame_rate.den);

	/* Initialize decode packet */
	av_init_packet(&pkt);
	pkt.data = NULL;
	pkt.size = 0;

	/* Allocate frame for decoding into */
	frame = avcodec_alloc_frame();
	if (frame == NULL) {
		LOG(LOG_ERROR, "Could not allocate frame\n");
		goto free;
	}

	/* Allocate frames for RGB conversion */
	frame_curr = avcodec_alloc_frame();
	if (frame_curr == NULL) {
		LOG(LOG_ERROR, "Could not allocate frame for rgb conversion\n");
		goto free;
	}

	/* Allocate frames for RGB conversion */
	frame_prev = avcodec_alloc_frame();
	if (frame_prev == NULL) {
		LOG(LOG_ERROR, "Could not allocate frame for rgb conversion\n");
		goto free;
	}

	/* Allocate current and previous rgb frames */
	len = avpicture_get_size(PIX_FMT_RGB24,
			dec_ctx->width, dec_ctx->height);
	buffer = av_malloc(len);
	avpicture_fill((AVPicture *)frame_curr, buffer, PIX_FMT_RGB24,
			dec_ctx->width, dec_ctx->height);
	buffer = av_malloc(len);
	avpicture_fill((AVPicture *)frame_prev, buffer, PIX_FMT_RGB24,
			dec_ctx->width, dec_ctx->height);

	/* Remove any ".png" from of output filename length */
	if (path_len > 4 && strcmp(options.output_path + path_len - 4,
			".png") == 0)
		path_len -= 4;

	/* Output any splash title screen that is required */
	out_frames = 0;
	if (options.splash_path != NULL) {
		out_frames = dump_splash(options.splash_path,
				path_len, options.output_path,
				&vs->avg_frame_rate);
	}

	/* Read the frames from the input file */
	while (av_read_frame(fmt_ctx, &pkt) >= 0) {
		int got_frame;

		/* Skip non-video packets */
		if (pkt.stream_index != stream_id) {
			LOG(LOG_DEBUG, "Not video stream!\n");
			av_free_packet(&pkt);
			continue;
		}

		/* Try decoding a frame */
		ret = avcodec_decode_video2(dec_ctx, frame,
				&got_frame, &pkt);
		if (ret < 0) {
			LOG(LOG_WARNING, "Warning: could not decode frame\n");
			break;
		}

		/* Handle the frame, if we got one */
		if (got_frame) {
			bool write_frame = true;

			/* TODO: Could do without the conversion to RGB, which
			 *       would be faster, but would have to deal with
			 *       all sorts of different frame types.
			 *       Would need to understand if they're all 8-bit
			 *       per channel, and how to get the channel count.
			 *       Conversion to RGB24 ensures three 8-bit colour
			 *       channels.
                         */
			img_convert_ctx = sws_getCachedContext(img_convert_ctx,
					dec_ctx->width, dec_ctx->height,
					dec_ctx->pix_fmt, dec_ctx->width,
					dec_ctx->height, PIX_FMT_RGB24,
					SWS_BICUBIC, NULL, NULL,NULL);
			sws_scale(img_convert_ctx, (const uint8_t * const*)
					((AVPicture*)frame)->data,
					((AVPicture*)frame)->linesize, 0,
					dec_ctx->height,
					((AVPicture *)frame_curr)->data,
					((AVPicture *)frame_curr)->linesize);

			if (frames == 0 || frames_differ(frame_prev, frame_curr,
					dec_ctx->width, dec_ctx->height)) {
				/* This frame has something new */
				frame_tmp = frame_prev;
				frame_prev = frame_curr;
				frame_curr = frame_tmp;

				LOG(LOG_DEBUG, "%i: Different\n", frames);

				if (skip > slack) {
					/* Log which frames got skipped */
					int f1 = frames - (skip - slack);
					int f2 = frames;

					get_times(f1, f2, &vs->avg_frame_rate,
							&s1, &m1, &h1,
							&s2, &m2, &h2);
					LOG(LOG_INFO, "Skip frames %i to %i ",
							f1, f2);
					LOG(LOG_INFO, "(%.2i:%.2i:%.2i - "
							"%.2i:%.2i:%.2i)\n",
							h1, m1, s1,
							h2, m2, s2);
				}
				skip = 0;

			} else {
				/* Frames are the same */
				skip++;

				LOG(LOG_DEBUG, "%i: Same\n", frames);

				if (skip > slack) {
					write_frame = false;
				}
			}

			/* Write frame_prev to the output file, if we've
			 * decided to keep it */
			if (write_frame) {
				sprintf(path, "%.*s%.08i.png", path_len,
						options.output_path,
						out_frames);
				image_write_png(path, frame_prev,
						dec_ctx->width,
						dec_ctx->height);
				out_frames++;
			}

			frames++;
		}

		av_free_packet(&pkt);
	}

	get_times(frames, out_frames, &vs->avg_frame_rate,
			&s1, &m1, &h1, &s2, &m2, &h2);
	LOG(LOG_RESULT, "Frames %i --> %i ", frames, out_frames);
	LOG(LOG_RESULT, "(%.2i:%.2i:%.2i --> %.2i:%.2i:%.2i)\n",
			h1, m1, s1, h2, m2, s2);

	/* It all worked! */
	res = true;
free:
	if (frame != NULL)
		av_free(frame);
	if (frame_curr != NULL)
		av_free(frame_curr);
	if (frame_prev != NULL)
		av_free(frame_prev);
	if (img_convert_ctx != NULL) {
                sws_freeContext(img_convert_ctx);
	}

	return res;
}


/**
 * Excise the boring bits of an input video, and save the remaining to output
 *
 * \return true on success, else false
 */
bool excise_boring_bits_wrapper(void)
{
	AVFormatContext *fmt_ctx = NULL;
	AVCodecContext *dec_ctx = NULL;
	AVCodec *dec = NULL;
	int stream_id;
	AVStream *vs;
	bool res = false;
	int ret;

	/* Register all the formats and codecs supported by ffmpeg.
	 * We don't know what format the input file is, and this is
	 * quicker than actually testing. */
	av_register_all();

	/* Open the input file, and allocate it's format context */
	ret = avformat_open_input(&fmt_ctx, options.input_path, NULL, NULL);
	if (ret < 0) {
		LOG(LOG_ERROR, "Could not open input video: '%s'\n",
				options.input_path);
		goto free;
	}

	/* Get info about the stream */
	ret = avformat_find_stream_info(fmt_ctx, NULL);
	if (ret < 0) {
		LOG(LOG_ERROR, "Warning: Couldn't get input stream info\n");
	}

	/* Find the "best" video stream in the file */
	ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (ret < 0) {
		LOG(LOG_ERROR, "Could not find video stream in input file: "
				"'%s'\n", options.input_path);
		goto free;
	}

	/* Got the stream number of the video, so get its stream */
	stream_id = ret;
	vs = fmt_ctx->streams[stream_id];

	/* Get the decoder for the stream */
	dec_ctx = vs->codec;
	dec = avcodec_find_decoder(dec_ctx->codec_id);
	if (!dec) {
		LOG(LOG_ERROR, "Could not find codec for input video\n");
		goto free;
	}

	/* Initialise the decoder */
	ret = avcodec_open2(dec_ctx, dec, NULL);
	if (ret < 0) {
		LOG(LOG_ERROR, "Could not open codec for input video\n");
		return ret;
	}

	LOG(LOG_DEBUG, "Frame rate: %i/%i\n",
			vs->avg_frame_rate.num, vs->avg_frame_rate.den);

	/* Do the excising of boring bits */
	res = excise_boring_bits(fmt_ctx, dec_ctx, stream_id, vs);
	if (res == false) {
		goto free;
	}

	/* It all worked! */
	res = true;

free:
	if (dec_ctx != NULL)
		avcodec_close(dec_ctx);
	if (fmt_ctx != NULL)
		avformat_close_input(&fmt_ctx);

	return res;
}


int main(int argc, char *argv[])
{
	bool ok;
	int a;

	/* Handle less than minumum args */
	if (argc < 3) {
		show_usage(argv[0]);
		return EXIT_FAILURE;
	}

	/* Set default logging level */
	level = LOG_RESULT;

	/* Initialise options */
	options.input_path = NULL;
	options.output_path = NULL;
	options.splash_path = NULL;
	options.border = BORDER;
	options.slack = SLACK_TIME_CS;
	options.splash = SPLASH_TIME_CS;

	/* Handle whatever args were passed */
	for (a = 1; a < argc; a++) {
		const char *arg = argv[a];
		if (*arg == '-') {
			if (strcmp(argv[a], "-h") == 0 ||
					strcmp(argv[a], "--help") == 0) {
				show_usage(argv[0]);
				return EXIT_SUCCESS;
			} if (argc >= 3 && (strcmp(argv[a], "-v") == 0 ||
					strcmp(argv[a], "--verbose") == 0)) {
				level = LOG_INFO;
			} else if (argc >= 3 && (strcmp(argv[a], "-d") == 0 ||
					strcmp(argv[a], "--debug") == 0)) {
				level = LOG_DEBUG;
			} else if (argc >= 3 && (strcmp(argv[a], "-q") == 0 ||
					strcmp(argv[a], "--quiet") == 0)) {
				level = LOG_WARNING;
			} else if (argc >= 3 && (strcmp(argv[a], "-b") == 0 ||
					strcmp(argv[a], "--border") == 0)) {
				if (a + 1 < argc) {
					a++;
					if (!isdigit(argv[a][0])) {
						LOG(LOG_ERROR, "Bad arg\n");
						return EXIT_FAILURE;
					}
					options.border = atoi(argv[a]);
				}
			} else if (argc >= 3 && (strcmp(argv[a], "-s") == 0 ||
					strcmp(argv[a], "--slack") == 0)) {
				if (a + 1 < argc) {
					a++;
					if (!isdigit(argv[a][0])) {
						LOG(LOG_ERROR, "Bad arg\n");
						return EXIT_FAILURE;
					}
					options.slack = atoi(argv[a]);
				}
			} else if (argc >= 3 && (strcmp(argv[a], "-i") == 0 ||
					strcmp(argv[a], "--intro") == 0)) {
				if (a + 1 < argc) {
					a++;
					if (!isdigit(argv[a][0])) {
						LOG(LOG_ERROR, "Bad arg\n");
						return EXIT_FAILURE;
					}
					options.splash = atoi(argv[a]);
				}
			}
			continue;

		} else if (options.input_path == NULL) {
			options.input_path = arg;

		} else if (options.output_path == NULL) {
			options.output_path = arg;

		} else if (options.splash_path == NULL) {
			options.splash_path = arg;

		} else {
			/* We've got input and output path, and
			 * this isn't an option */
			show_usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	/* Do the video stuff! */
	ok = excise_boring_bits_wrapper();
	if (!ok) {
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}


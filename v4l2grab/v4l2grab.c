/***************************************************************************
 *   v4l2grab Version 0.3                                                  *
 *   Copyright (C) 2012 by Tobias Müller                                   *
 *   Tobias_Mueller@twam.info                                              *
 *                                                                         *
 *   based on V4L2 Specification, Appendix B: Video Capture Example        *
 *   (http://v4l2spec.bytesex.org/spec/capture-example.html)               *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/
 
 /**************************************************************************
 *   Modification History                                                  *
 *                                                                         *
 *   Matthew Witherwax      21AUG2013                                      *
 *      Added ability to change frame interval (ie. frame rate/fps)        *
 * Martin Savc              7JUL2015
 *      Added support for continuous capture using SIGINT to stop.
 ***************************************************************************/

// compile with all three access methods
#if !defined(IO_READ) && !defined(IO_MMAP) && !defined(IO_USERPTR)
#define IO_READ
#define IO_MMAP
#define IO_USERPTR
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <getopt.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <malloc.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <asm/types.h>
#include <linux/videodev2.h>
#include <jpeglib.h>
#include <libv4l2.h>
#include <signal.h>
#include <stdint.h>
#include <inttypes.h>
#include <pthread.h>
#include "config.h"
#include "yuv.h"

#define CLEAR(x) memset (&(x), 0, sizeof (x))

#ifndef VERSION
#define VERSION "unknown"
#endif

#if defined(IO_MMAP) || defined(IO_USERPTR)
// minimum number of buffers to request in VIDIOC_REQBUFS call
#define VIDIOC_REQBUFS_COUNT 2
#endif

typedef enum {
#ifdef IO_READ
        IO_METHOD_READ,
#endif
#ifdef IO_MMAP
        IO_METHOD_MMAP,
#endif
#ifdef IO_USERPTR
        IO_METHOD_USERPTR,
#endif
} io_method;

typedef struct {
		size_t n_buffers;
        void *                  start;
        size_t                  length;
}buffer;

static io_method        io              = IO_METHOD_MMAP;
//static int              fd              = -1;
// struct buffer *         buffers         = NULL;
// static unsigned int     n_buffers       = 0;

// global settings
static unsigned int width = 640;
static unsigned int height = 480;
static unsigned int fps = 30;
static int continuous = 0;
static unsigned char jpegQuality = 70;
//static char* jpegFilename = NULL;
//static char* jpegFilenamePart = NULL;
//static char* deviceName = "/dev/video0";

typedef struct cameras {
	char deviceName[16];
	int fd;
}uvcCameras;

static const char* const continuousFilenameFmt = "%s_%010"PRIu32"_%"PRId64".jpg";

/**
SIGINT interput handler
*/
void StopContCapture(int sig_id) {
	printf("stoping continuous capture\n");
	continuous = 0;
}

void InstallSIGINTHandler() {
	struct sigaction sa;
	CLEAR(sa);
	
	sa.sa_handler = StopContCapture;
	if(sigaction(SIGINT, &sa, 0) != 0)
	{
		fprintf(stderr,"could not install SIGINT handler, continuous capture disabled");
		continuous = 0;
	}
}

/**
	Print error message and terminate programm with EXIT_FAILURE return code.

	\param s error message to print
*/
static void errno_exit(const char* s)
{
	fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
	exit(EXIT_FAILURE);
}

/**
	Do ioctl and retry if error was EINTR ("A signal was caught during the ioctl() operation."). Parameters are the same as on ioctl.

	\param fd file descriptor
	\param request request
	\param argp argument
	\returns result from ioctl
*/
static int xioctl(int fd, int request, void* argp)
{
	int r;

	do r = v4l2_ioctl(fd, request, argp);
	while (-1 == r && EINTR == errno);

	return r;
}

/**
	Write image to jpeg file.

	\param img image to write
*/
static void jpegWrite(unsigned char* img, char* jpegFilename)
{
	struct jpeg_compress_struct cinfo;
	struct jpeg_error_mgr jerr;

	JSAMPROW row_pointer[1];
	FILE *outfile = fopen( jpegFilename, "wb" );

	// try to open file for saving
	if (!outfile) {
		errno_exit("jpeg");
	}

	// create jpeg data
	cinfo.err = jpeg_std_error( &jerr );
	jpeg_create_compress(&cinfo);
	jpeg_stdio_dest(&cinfo, outfile);

	// set image parameters
	cinfo.image_width = width;
	cinfo.image_height = height;
	cinfo.input_components = 3;
	cinfo.in_color_space = JCS_YCbCr;

	// set jpeg compression parameters to default
	jpeg_set_defaults(&cinfo);
	// and then adjust quality setting
	jpeg_set_quality(&cinfo, jpegQuality, TRUE);

	// start compress
	jpeg_start_compress(&cinfo, TRUE);

	// feed data
	while (cinfo.next_scanline < cinfo.image_height) {
		row_pointer[0] = &img[cinfo.next_scanline * cinfo.image_width *  cinfo.input_components];
		jpeg_write_scanlines(&cinfo, row_pointer, 1);
	}

	// finish compression
	jpeg_finish_compress(&cinfo);

	// destroy jpeg data
	jpeg_destroy_compress(&cinfo);

	// close output file
	fclose(outfile);
}

/**
	process image read
*/
static void imageProcess(const void* p, struct timeval timestamp, char *jpegFilename)
{
	//timestamp.tv_sec
	//timestamp.tv_usec
	unsigned char* src = (unsigned char*)p;
	unsigned char* dst = malloc(width*height*3*sizeof(char));
	memset(dst, 0, width*height*3*sizeof(char));
	char* jpegFilenamePart = NULL;
	YUV420toYUV444(width, height, src, dst);

	if(continuous==1) {
		static uint32_t img_ind = 0;
		int64_t timestamp_long;
		timestamp_long = timestamp.tv_sec*1e6 + timestamp.tv_usec;
		sprintf(jpegFilename,continuousFilenameFmt,jpegFilenamePart,img_ind++,timestamp_long);

	}
	// write jpeg
	jpegWrite(dst,jpegFilename);

	// free temporary image
	free(dst);
}

/**
	read single frame
*/
static int frameRead(int fd, buffer *buffers)
{
	struct v4l2_buffer buf;
	static int cnt = 0;
	
#ifdef IO_USERPTR
	unsigned int i;
#endif
	char jpegFilename[64] = {0};
	sprintf(jpegFilename, "%d_%d.jpg", fd, cnt++);
	switch (io) {
#ifdef IO_READ
		case IO_METHOD_READ:
			if (-1 == v4l2_read(fd, buffers[0].start, buffers[0].length)) {
				switch (errno) {
					case EAGAIN:
						return 0;

					case EIO:
						// Could ignore EIO, see spec.
						// fall through

					default:
						errno_exit("read");
				}
			}

			struct timespec ts;
			struct timeval timestamp;
			clock_gettime(CLOCK_MONOTONIC,&ts);
			timestamp.tv_sec = ts.tv_sec;
			timestamp.tv_usec = ts.tv_nsec/1000;
			imageProcess(buffers[0].start,timestamp, jpegFilename);
			break;
#endif

#ifdef IO_MMAP
		case IO_METHOD_MMAP:
			CLEAR(buf);

			buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			buf.memory = V4L2_MEMORY_MMAP;

			if (-1 == xioctl(fd, VIDIOC_DQBUF, &buf)) {
				switch (errno) {
					case EAGAIN:
						return 0;

					case EIO:
						// Could ignore EIO, see spec
						// fall through

					default:
						errno_exit("VIDIOC_DQBUF");
				}
			}

			assert(buf.index < (buffers->n_buffers));

			imageProcess(buffers[buf.index].start,buf.timestamp, jpegFilename);

			if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
				errno_exit("VIDIOC_QBUF");

			break;
#endif

#ifdef IO_USERPTR
			case IO_METHOD_USERPTR:
				CLEAR (buf);

				buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
				buf.memory = V4L2_MEMORY_USERPTR;

				if (-1 == xioctl(fd, VIDIOC_DQBUF, &buf)) {
					switch (errno) {
						case EAGAIN:
							return 0;

						case EIO:
							// Could ignore EIO, see spec.
							// fall through

						default:
							errno_exit("VIDIOC_DQBUF");
					}
				}

				for (i = 0; i < (buffers->n_buffers); ++i)
					if (buf.m.userptr == (unsigned long)buffers[i].start && buf.length == buffers[i].length)
						break;

				assert (i < (buffers->n_buffers));

				imageProcess((void *)buf.m.userptr,buf.timestamp, jpegFilename);

				if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
					errno_exit("VIDIOC_QBUF");
				break;
#endif
	}

	return 1;
}

/**
	mainloop: read frames and process them
*/
static void mainLoop(int fd, buffer *buffers)
{	
	int count;
	unsigned int numberOfTimeouts;

	numberOfTimeouts = 0;
	count = 3;

	while (count-- > 0) {
		for (;;) {
			fd_set fds;
			struct timeval tv;
			int r;

			FD_ZERO(&fds);
			FD_SET(fd, &fds);

			/* Timeout. */
			tv.tv_sec = 1;
			tv.tv_usec = 0;

			r = select(fd + 1, &fds, NULL, NULL, &tv);

			if (-1 == r) {
				if (EINTR == errno)
					continue;

				errno_exit("select");
			}

			if (0 == r) {
				if (numberOfTimeouts <= 0) {
					count++;
				} else {
					fprintf(stderr, "select timeout\n");
					exit(EXIT_FAILURE);
				}
			}
			if(continuous == 1) {
				count = 3;
			}

			if (frameRead(fd, buffers))
				break;

			/* EAGAIN - continue select loop. */
		}
	}
}

/**
	stop capturing
*/
static void captureStop(int fd)
{
	enum v4l2_buf_type type;

	switch (io) {
#ifdef IO_READ
		case IO_METHOD_READ:
			/* Nothing to do. */
			break;
#endif

#ifdef IO_MMAP
		case IO_METHOD_MMAP:
#endif
#ifdef IO_USERPTR
		case IO_METHOD_USERPTR:
#endif
#if defined(IO_MMAP) || defined(IO_USERPTR)
			type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

			if (-1 == xioctl(fd, VIDIOC_STREAMOFF, &type))
			errno_exit("VIDIOC_STREAMOFF");

			break;
#endif
	}
}

/**
  start capturing
*/
static void captureStart(int fd, buffer *buffers)
{
	unsigned int i;
	enum v4l2_buf_type type;

	switch (io) {
#ifdef IO_READ
		case IO_METHOD_READ:
			/* Nothing to do. */
			break;
#endif

#ifdef IO_MMAP
		case IO_METHOD_MMAP:
			for (i = 0; i < (buffers->n_buffers); ++i) {
				struct v4l2_buffer buf;

				CLEAR(buf);

				buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
				buf.memory = V4L2_MEMORY_MMAP;
				buf.index = i;

				if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
					errno_exit("VIDIOC_QBUF");
				}

			type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

			if (-1 == xioctl(fd, VIDIOC_STREAMON, &type))
				errno_exit("VIDIOC_STREAMON");

			break;
#endif

#ifdef IO_USERPTR
		case IO_METHOD_USERPTR:
			for (i = 0; i < (buffers->n_buffers); ++i) {
				struct v4l2_buffer buf;

			CLEAR (buf);

			buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			buf.memory = V4L2_MEMORY_USERPTR;
			buf.index = i;
			buf.m.userptr = (unsigned long) buffers[i].start;
			buf.length = buffers[i].length;

			if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
				errno_exit("VIDIOC_QBUF");
			}

			type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

			if (-1 == xioctl(fd, VIDIOC_STREAMON, &type))
				errno_exit("VIDIOC_STREAMON");

			break;
#endif
	}
}

static void deviceUninit(buffer *buffers)
{
	unsigned int i;

	switch (io) {
#ifdef IO_READ
		case IO_METHOD_READ:
			free(buffers[0].start);
			break;
#endif

#ifdef IO_MMAP
		case IO_METHOD_MMAP:
			for (i = 0; i < (buffers->n_buffers); ++i)
				if (-1 == v4l2_munmap(buffers[i].start, buffers[i].length))
					errno_exit("munmap");
			break;
#endif

#ifdef IO_USERPTR
		case IO_METHOD_USERPTR:
			for (i = 0; i < (buffers->n_buffers); ++i)
				free(buffers[i].start);
			break;
#endif
	}

	free(buffers);
}

#ifdef IO_READ
buffer* readInit(unsigned int buffer_size)
{
	buffer *buffers = calloc(1, sizeof(*buffers));

	if (!buffers) {
		fprintf(stderr, "Out of memory\n");
		exit(EXIT_FAILURE);
	}

	buffers[0].length = buffer_size;
	buffers[0].start = malloc(buffer_size);

	if (!buffers[0].start) {
		fprintf (stderr, "Out of memory\n");
		exit(EXIT_FAILURE);
	}
	return buffers;
}
#endif

#ifdef IO_MMAP
buffer* mmapInit(char *deviceName, int fd)
{
	struct v4l2_requestbuffers req;

	CLEAR(req);

	req.count = VIDIOC_REQBUFS_COUNT;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;

	if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) {
		if (EINVAL == errno) {
			fprintf(stderr, "%s does not support memory mapping\n", deviceName);
			exit(EXIT_FAILURE);
		} else {
			errno_exit("VIDIOC_REQBUFS");
		}
	}

	if (req.count < 2) {
		fprintf(stderr, "Insufficient buffer memory on %s\n", deviceName);
		exit(EXIT_FAILURE);
	}

	buffer *buffers = calloc(req.count, sizeof(*buffers));

	if (!buffers) {
		fprintf(stderr, "Out of memory\n");
		exit(EXIT_FAILURE);
	}

	for ((buffers->n_buffers) = 0; (buffers->n_buffers) < req.count; ++(buffers->n_buffers)) {
		struct v4l2_buffer buf;

		CLEAR(buf);

		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = (buffers->n_buffers);

		if (-1 == xioctl(fd, VIDIOC_QUERYBUF, &buf))
			errno_exit("VIDIOC_QUERYBUF");

		buffers[buffers->n_buffers].length = buf.length;
		buffers[buffers->n_buffers].start = v4l2_mmap(NULL /* start anywhere */, buf.length, PROT_READ | PROT_WRITE /* required */, MAP_SHARED /* recommended */, fd, buf.m.offset);

		if (MAP_FAILED == buffers[buffers->n_buffers].start)
			errno_exit("mmap");
	}
	return buffers;
}
#endif

#ifdef IO_USERPTR
buffer* userptrInit(char *deviceName, unsigned int buffer_size, int fd)
{
	struct v4l2_requestbuffers req;
	unsigned int page_size;

	page_size = getpagesize();
	buffer_size = (buffer_size + page_size - 1) & ~(page_size - 1);

	CLEAR(req);

	req.count = VIDIOC_REQBUFS_COUNT;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_USERPTR;

	if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) {
		if (EINVAL == errno) {
			fprintf(stderr, "%s does not support user pointer i/o\n", deviceName);
			exit(EXIT_FAILURE);
		} else {
			errno_exit("VIDIOC_REQBUFS");
		}
	}

	buffer* buffers = calloc(4, sizeof(*buffers));

	if (!buffers) {
		fprintf(stderr, "Out of memory\n");
		exit(EXIT_FAILURE);
	}

	for ((buffers->n_buffers) = 0; (buffers->n_buffers) < 4; ++(buffers->n_buffers)) {
		buffers[buffers->n_buffers].length = buffer_size;
		buffers[buffers->n_buffers].start = memalign(/* boundary */ page_size, buffer_size);

		if (!buffers[buffers->n_buffers].start) {
			fprintf(stderr, "Out of memory\n");
			exit(EXIT_FAILURE);
		}
	}
	return buffers;
}
#endif

/**
	initialize device
*/
buffer* deviceInit(char *deviceName, int fd)
{
	struct v4l2_capability cap;
	struct v4l2_cropcap cropcap;
	struct v4l2_crop crop;
	struct v4l2_format fmt;
	struct v4l2_streamparm frameint;
	unsigned int min;

	if (-1 == xioctl(fd, VIDIOC_QUERYCAP, &cap)) {
		if (EINVAL == errno) {
			fprintf(stderr, "%s is no V4L2 device\n",deviceName);
			exit(EXIT_FAILURE);
		} else {
			errno_exit("VIDIOC_QUERYCAP");
		}
	}

	if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
		fprintf(stderr, "%s is no video capture device\n",deviceName);
		exit(EXIT_FAILURE);
	}

	switch (io) {
#ifdef IO_READ
		case IO_METHOD_READ:
			if (!(cap.capabilities & V4L2_CAP_READWRITE)) {
				fprintf(stderr, "%s does not support read i/o\n",deviceName);
				exit(EXIT_FAILURE);
			}
			break;
#endif

#ifdef IO_MMAP
		case IO_METHOD_MMAP:
#endif
#ifdef IO_USERPTR
		case IO_METHOD_USERPTR:
#endif
#if defined(IO_MMAP) || defined(IO_USERPTR)
      			if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
				fprintf(stderr, "%s does not support streaming i/o\n",deviceName);
				exit(EXIT_FAILURE);
			}
			break;
#endif
	}

	/* Select video input, video standard and tune here. */
	CLEAR(cropcap);

	cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if (0 == xioctl(fd, VIDIOC_CROPCAP, &cropcap)) {
		crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		crop.c = cropcap.defrect; /* reset to default */

		if (-1 == xioctl(fd, VIDIOC_S_CROP, &crop)) {
			switch (errno) {
				case EINVAL:
					/* Cropping not supported. */
					break;
				default:
					/* Errors ignored. */
					break;
			}
		}
	} else {
		/* Errors ignored. */
	}

	CLEAR(fmt);

	// v4l2_format
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = width;
	fmt.fmt.pix.height = height;
	fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;

	if (-1 == xioctl(fd, VIDIOC_S_FMT, &fmt))
		errno_exit("VIDIOC_S_FMT");

	if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUV420) {
		fprintf(stderr,"Libv4l didn't accept YUV420 format. Can't proceed.\n");
		exit(EXIT_FAILURE);
	}

	/* Note VIDIOC_S_FMT may change width and height. */
	if (width != fmt.fmt.pix.width) {
		width = fmt.fmt.pix.width;
		fprintf(stderr,"Image width set to %i by device %s.\n", width, deviceName);
	}

	if (height != fmt.fmt.pix.height) {
		height = fmt.fmt.pix.height;
		fprintf(stderr,"Image height set to %i by device %s.\n", height, deviceName);
	}
	
  /* If the user has set the fps to -1, don't try to set the frame interval */
  if (fps != -1)
  {
    CLEAR(frameint);
    
    /* Attempt to set the frame interval. */
    frameint.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    frameint.parm.capture.timeperframe.numerator = 1;
    frameint.parm.capture.timeperframe.denominator = fps;
    if (-1 == xioctl(fd, VIDIOC_S_PARM, &frameint))
      fprintf(stderr,"Unable to set frame interval.\n");
  }

	/* Buggy driver paranoia. */
	min = fmt.fmt.pix.width * 2;
	if (fmt.fmt.pix.bytesperline < min)
		fmt.fmt.pix.bytesperline = min;
	min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
	if (fmt.fmt.pix.sizeimage < min)
		fmt.fmt.pix.sizeimage = min;

	switch (io) {
#ifdef IO_READ
		case IO_METHOD_READ:
			return readInit(fmt.fmt.pix.sizeimage);
			break;
#endif

#ifdef IO_MMAP
		case IO_METHOD_MMAP:
			return mmapInit(deviceName, fd);
			break;
#endif

#ifdef IO_USERPTR
		case IO_METHOD_USERPTR:
			return userptrInit(deviceName, fmt.fmt.pix.sizeimage, fd);
			break;
#endif
	}
}

/**
	close device
*/
static void deviceClose(int fd)
{
	if (-1 == v4l2_close(fd))
		errno_exit("close");

	fd = -1;
}

/**
	open device
*/
static int deviceOpen(char *deviceName)
{
	struct stat st;

	// stat file
	if (-1 == stat(deviceName, &st)) {
		fprintf(stderr, "Cannot identify '%s': %d, %s\n", deviceName, errno, strerror(errno));
		exit(EXIT_FAILURE);
	}

	// check if its device
	if (!S_ISCHR(st.st_mode)) {
		fprintf(stderr, "%s is no device\n", deviceName);
		exit(EXIT_FAILURE);
	}

	// open device
	int fd = v4l2_open(deviceName, O_RDWR /* required */ | O_NONBLOCK, 0);

	// check if opening was successfull
	if (-1 == fd) {
		fprintf(stderr, "Cannot open '%s': %d, %s\n", deviceName, errno, strerror(errno));
		exit(EXIT_FAILURE);
	}
	return fd;
}

/**
	print usage information
*/
static void usage(FILE* fp, int argc, char** argv)
{
	fprintf(fp,
		"Usage: %s [options]\n\n"
		"Options:\n"
		"-d | --device name   Video device name [/dev/video0]\n"
		"-h | --help          Print this message\n"
		"-o | --output        Set JPEG output filename\n"
		"-q | --quality       Set JPEG quality (0-100)\n"
		"-m | --mmap          Use memory mapped buffers\n"
		"-r | --read          Use read() calls\n"
		"-u | --userptr       Use application allocated buffers\n"
		"-W | --width         Set image width\n"
		"-H | --height        Set image height\n"
		"-I | --interval      Set frame interval (fps) (-1 to skip)\n"
		"-c | --continuous    Do continous capture, stop with SIGINT.\n"
		"-v | --version       Print version\n"
		"",
		argv[0]);
	}

static const char short_options [] = "d:ho:q:mruW:H:I:vc";

static const struct option
long_options [] = {
	{ "device",     required_argument,      NULL,           'd' },
	{ "help",       no_argument,            NULL,           'h' },
	{ "output",     required_argument,      NULL,           'o' },
	{ "quality",    required_argument,      NULL,           'q' },
	{ "mmap",       no_argument,            NULL,           'm' },
	{ "read",       no_argument,            NULL,           'r' },
	{ "userptr",    no_argument,            NULL,           'u' },
	{ "width",      required_argument,      NULL,           'W' },
	{ "height",     required_argument,      NULL,           'H' },
	{ "interval",   required_argument,      NULL,           'I' },
	{ "version",	no_argument,		NULL,		'v' },
	{ "continuous",	no_argument,		NULL,		'c' },
	{ 0, 0, 0, 0 }
};

/*
	??camera data
*/
void *thGetCameraData(void *args) {
	char *deviceName = (char *)args;
	if(NULL == args) {
		printf("args is NULL\n");
		//return NULL;
		exit(EXIT_FAILURE);
	}
	int fd = deviceOpen(deviceName);
	buffer *buffers;
	buffers = deviceInit(deviceName, fd);
	assert(buffers);
	// start capturing
	captureStart(fd, buffers);

	// process frames
	mainLoop(fd, buffers);

	// stop capturing
	captureStop(fd);

	// close device
	deviceUninit(buffers);
	deviceClose(fd);
}
int main(int argc, char **argv)
{
	char *deviceName = NULL;
	deviceName = "/dev/video1";
	// open and initialize device
	pthread_t thread0, thread1;
	pthread_create(&thread0, NULL, thGetCameraData, deviceName);
	//sleep(1);
	deviceName = "/dev/video2";
	pthread_create(&thread1, NULL, thGetCameraData, deviceName);
	pthread_join(thread1, NULL);
	pthread_join(thread0, NULL);
	exit(EXIT_SUCCESS);

	return 0;
}

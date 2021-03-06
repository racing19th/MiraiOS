#include <stdio.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <uapi/syscalls.h>
#include <uapi/fcntl.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>

#define STREAM_MODEMASK	3
#define STREAM_FREEBUF	(1 << 2)
#define STREAM_EOF		(1 << 3)
#define STREAM_ERROR	(1 << 4)
#define STREAM_PROC		(1 << 5)

extern FILE _PHStdout;
extern FILE _PHStderr;

char _PHStdinBuf[BUFSIZ];
FILE _PHStdin = {
	.fd = 0,
	.flags = _IOLBF,
	.buf = _PHStdinBuf,
	.writeEnd = _PHStdinBuf,
	.readEnd = _PHStdinBuf,
	.bufEnd = _PHStdinBuf + BUFSIZ,
	.next = &_PHStdout,
	.cbuf = EOF
};

char _PHStdoutBuf[BUFSIZ];
FILE _PHStdout = {
	.fd = 1,
	.flags = _IOLBF,
	.buf = _PHStdoutBuf,
	.writeEnd = _PHStdoutBuf,
	.readEnd = _PHStdoutBuf,
	.bufEnd = _PHStdoutBuf + BUFSIZ,
	.next = &_PHStderr,
	.prev = &_PHStdin,
	.cbuf = EOF
};

char _PHStderrBuf[BUFSIZ];
FILE _PHStderr = {
	.fd = 2,
	.flags = _IOLBF,
	.buf = _PHStderrBuf,
	.writeEnd = _PHStderrBuf,
	.readEnd = _PHStderrBuf,
	.bufEnd = _PHStderrBuf + BUFSIZ,
	.prev = &_PHStdout,
	.cbuf = EOF
};

FILE *stdin = &_PHStdin;
FILE *stdout = &_PHStdout;
FILE *stderr = &_PHStderr;

FILE *_PHFirstFile = &_PHStdin;
FILE *_PHLastFile = &_PHStderr;

static int parseMode(const char *mode) {
	int flags = 0;
	switch (mode[0]) {
		case 'r':
			flags = SYSOPEN_FLAG_READ;
			if (mode[1]) {
				if (mode[1] == '+') {
					flags |= SYSOPEN_FLAG_WRITE;
				} else {
					errno = EINVAL;
					return -1;
				}
			}
			break;
		case 'w':
			flags = SYSOPEN_FLAG_WRITE | SYSOPEN_FLAG_TRUNC | SYSOPEN_FLAG_CREATE;
			if (mode[1]) {
				if (mode[1] == '+') {
					flags |= SYSOPEN_FLAG_READ;
				} else {
					errno = EINVAL;
					return -1;
				}
			}
			break;
		case 'a':
			flags = SYSOPEN_FLAG_WRITE | SYSOPEN_FLAG_APPEND;
			if (mode[1]) {
				if (mode[1] == '+') {
					flags |= SYSOPEN_FLAG_READ;
				} else {
					errno = EINVAL;
					return -1;
				}
			}
			break;
		default:
			errno = EINVAL;
			return -1;
	}
	return flags;
}

FILE *fopen(const char *filename, const char *mode) {
	FILE *f = NULL;
	if (!mode || !*mode) {
		errno = EINVAL;
		goto ret;
	}
	int flags = parseMode(mode);
	if (flags < 0) goto ret;

	f = malloc(sizeof(*f));
	if (!f) {
		goto ret;
	}
	f->buf = malloc(BUFSIZ);
	if (!f->buf) {
		goto freeFile;
	}
	f->bufEnd = f->buf + BUFSIZ;
	f->writeEnd = f->buf;
	f->readEnd = f->buf;
	f->flags = _IOFBF | STREAM_FREEBUF;
	f->cbuf = EOF;
	f->seekOffset = 0;

	f->prev = _PHLastFile;
	_PHLastFile->next = f;
	_PHLastFile = f;

	int error = sysOpen(AT_FDCWD, filename, (unsigned int)flags);
	if (error < 0) {
		errno = -error;
		goto freeBuf;
	}
	f->fd = error;
	goto ret;

	freeBuf:
	free(f->buf);
	freeFile:
	free(f);
	f = NULL;
	ret:
	return f;
}

FILE *freopen(const char *restrict filename, const char *restrict mode, FILE *restrict stream) {
	fflush(stream);
	int error;

	sysClose(stream->fd);

	int flags = parseMode(mode);
	error = sysOpen(AT_FDCWD, filename, (unsigned int)flags);
	if (error) {
		errno = -error;
		stream->fd = -1;
		fclose(stream);
		return NULL;
	}
	return stream;
}

int fflush(FILE *stream) {
	size_t writeSize = stream->writeEnd - stream->buf;
	int error = 0;
	if (!writeSize) {
		return 0;
	}
	error = sysWrite(stream->fd, stream->buf, writeSize);
	stream->writeEnd = stream->buf;
	stream->readEnd = stream->buf;
	if (error < 0) {
		stream->flags |= STREAM_ERROR;
		errno = -error;
		return -1;
	}
	return 0;
}

int fclose(FILE *stream) {
	if (stream->flags & STREAM_PROC) {
		return pclose(stream);
	}
	int error = fflush(stream);
	if (error) return error;

	if (stream->prev) {
		stream->prev->next = stream->next;
	} else {
		_PHFirstFile = stream->next;
	}
	if (stream->next) {
		stream->next->prev = stream->prev;
	} else {
		_PHLastFile = stream->prev;
	}

	if (stream->fd >= 0) {
		error = sysClose(stream->fd);
	}
	if (stream->flags & STREAM_FREEBUF) {
		free(stream->buf);
	}
	if (stream != stdin && stream != stdout && stream != stderr) {
		free(stream);
	}

	if (error < 0) {
		errno = -error;
		return -1;
	}
	return 0;
}

void _PHCloseAll(void) {
	FILE *f = _PHFirstFile;
	while (f) {
		FILE *next = f->next;
		fclose(f);
		f = next;
	}
}

int setvbuf(FILE *stream, char *buffer, int mode, size_t size) {
	if (fflush(stream)) return -1;
	if (stream->flags & STREAM_FREEBUF) {
		free(stream->buf);
	}

	if (!buffer) {
		buffer = malloc(size);
		if (!buffer) {
			return -1;
		}
		stream->flags |= STREAM_FREEBUF;
	} else {
		stream->flags &= ~STREAM_FREEBUF;
	}
	stream->buf = buffer;
	stream->writeEnd = stream->readEnd = stream->buf;
	stream->bufEnd = stream->buf + size;

	stream->flags &= ~STREAM_MODEMASK;
	stream->flags |= mode & STREAM_MODEMASK;
	return 0;
}

void setbuf(FILE *stream, char *buffer) {
	setvbuf(stream, buffer, stream->flags, BUFSIZ);
}

static size_t fwriteFBF(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
	size_t totalSize = size * nmemb;
	size_t writeSize = stream->bufEnd - stream->writeEnd;
	const char *src = (const char *)ptr;
	if (totalSize > writeSize) {
		memcpy(stream->writeEnd, src, writeSize);
		//stream->writeEnd = stream->bufEnd;
		//stream->readEnd = stream->bufEnd;
		src += writeSize;
		totalSize -= writeSize;

		if (fflush(stream)) return 0;

		if (totalSize > (size_t)(stream->bufEnd - stream->buf)) {
			if (write(stream->fd, ptr, totalSize) < 0) {
				stream->flags |= STREAM_ERROR;
				return 0;
			}
			return nmemb;
		}
	}
	memcpy(stream->writeEnd, src, totalSize);
	stream->writeEnd += totalSize;
	return nmemb;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
	if ((stream->flags & STREAM_MODEMASK) == _IOLBF) {
		size_t totalSize = size * nmemb;
		const char *str = ptr;
		while (totalSize) {
			const char *end = memchr(str, '\n', totalSize);
			if (end) {
				end++;
			} else {
				end = str + totalSize;
			}
			size_t diff = end - str;

			if (!fwriteFBF(str, 1, diff, stream)) return 0;
			if (fflush(stream) < 0) return 0;

			totalSize -= diff;
			str += diff;
		}
	} else if ((stream->flags & STREAM_MODEMASK) == _IOFBF) {
		return fwriteFBF(ptr, size, nmemb, stream);
	} else {
		//_IONBF
		if (fflush(stream) < 0) return 0;
		if (write(stream->fd, ptr, size * nmemb) < 0) {
			stream->flags |= STREAM_ERROR;
			return 0;
		}
	}
	return nmemb;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
	//TODO? Kernel VFS already does caching
	size_t sz = size * nmemb;
	if (!sz) {
		return 0;
	}
	if (stream->cbuf != EOF) {
		char *cptr = ptr;
		*cptr = stream->cbuf;
		cptr++;
		sz--;
		ptr = cptr;
		if (!sz) {
			return 1;
		}
	}
	ssize_t rd = read(stream->fd, ptr, sz);
	if (rd <= 0) {
		stream->flags |= STREAM_ERROR;
		return 0;
	}
	if (rd != (ssize_t)sz) {
		stream->flags |= STREAM_EOF;
	}
	return rd / size;
}

int feof(FILE *stream) {
	return stream->flags & STREAM_EOF;
}
int ferror(FILE *stream) {
	return stream->flags & STREAM_ERROR;
}
void clearerr(FILE *stream){
	stream->flags &= ~STREAM_ERROR;
}
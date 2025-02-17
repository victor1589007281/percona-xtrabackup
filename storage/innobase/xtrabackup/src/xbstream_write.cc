/******************************************************
Copyright (c) 2011-2023 Percona LLC and/or its affiliates.

The xbstream format writer implementation.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA

*******************************************************/

#include <my_base.h>
#include <my_byteorder.h>
#include <my_io.h>
#include <mysql/service_mysql_alloc.h>
#include <mysql_version.h>
#include <zlib.h>
#include <mutex>
#include "common.h"
#include "crc_glue.h"
#include "datasink.h"
#include "msg.h"
#include "xbstream.h"

/* Group writes smaller than this into a single chunk */
#define XB_STREAM_MIN_CHUNK_SIZE (10 * 1024 * 1024)

struct xb_wstream_struct {
  std::mutex *mutex;
};

struct xb_wstream_file_struct {
  xb_wstream_t *stream;
  char *path;
  ulong path_len;
  char chunk[XB_STREAM_MIN_CHUNK_SIZE];
  char *chunk_ptr;
  size_t chunk_free;
  char *sparse_map_buf;
  size_t sparse_map_buf_size;
  my_off_t offset;
  void *userdata;
  xb_stream_write_callback *write;
};

static int xb_stream_flush(xb_wstream_file_t *file);
static int xb_stream_write_chunk(xb_wstream_file_t *file, const void *buf,
                                 size_t len, size_t sparse_map_size,
                                 const ds_sparse_chunk_t *sparse_map);
static int xb_stream_write_eof(xb_wstream_file_t *file);

static ssize_t xb_stream_default_write_callback(xb_wstream_file_t *file
                                                __attribute__((unused)),
                                                void *userdata
                                                __attribute__((unused)),
                                                const void *buf, size_t len) {
  if (my_write(fileno(stdout), static_cast<const uchar *>(buf), len,
               MYF(MY_WME | MY_NABP)))
    return -1;
  return len;
}

xb_wstream_t *xb_stream_write_new(void) {
  xb_wstream_t *stream;
  std::mutex *mutex = new std::mutex();

  stream = (xb_wstream_t *)my_malloc(PSI_NOT_INSTRUMENTED, sizeof(xb_wstream_t),
                                     MYF(MY_FAE | MY_ZEROFILL));
  stream->mutex = mutex;
  return stream;
}

xb_wstream_file_t *xb_stream_write_open(xb_wstream_t *stream, const char *path,
                                        MY_STAT *mystat __attribute__((unused)),
                                        void *userdata,
                                        xb_stream_write_callback *onwrite) {
  xb_wstream_file_t *file;
  ulong path_len;

  path_len = strlen(path);

  if (path_len > FN_REFLEN) {
    msg("xb_stream_write_open(): file path is too long.\n");
    return NULL;
  }

  file = (xb_wstream_file_t *)my_malloc(
      PSI_NOT_INSTRUMENTED, sizeof(xb_wstream_file_t) + path_len + 1,
      MYF(MY_FAE | MY_ZEROFILL));

  file->path = (char *)(file + 1);
  memcpy(file->path, path, path_len + 1);
  file->path_len = path_len;

  file->stream = stream;
  file->offset = 0;
  file->chunk_ptr = file->chunk;
  file->chunk_free = XB_STREAM_MIN_CHUNK_SIZE;
  if (onwrite) {
#ifdef __WIN__
    setmode(fileno(stdout), _O_BINARY);
#endif
    file->userdata = userdata;
    file->write = onwrite;
  } else {
    file->userdata = NULL;
    file->write = xb_stream_default_write_callback;
  }

  return file;
}

int xb_stream_write_data(xb_wstream_file_t *file, const void *buf, size_t len) {
  if (len < file->chunk_free) {
    memcpy(file->chunk_ptr, buf, len);
    file->chunk_ptr += len;
    file->chunk_free -= len;

    return 0;
  }

  if (xb_stream_flush(file)) return 1;

  return xb_stream_write_chunk(file, buf, len, 0, nullptr);
}

int xb_stream_write_sparse_data(xb_wstream_file_t *file, const void *buf,
                                size_t len, size_t sparse_map_size,
                                const ds_sparse_chunk_t *sparse_map) {
  if (xb_stream_flush(file)) return 1;

  return xb_stream_write_chunk(file, buf, len, sparse_map_size, sparse_map);
}

int xb_stream_write_close(xb_wstream_file_t *file) {
  int rc = 0;
  if (xb_stream_flush(file) || xb_stream_write_eof(file)) {
    rc = 1;
  }

  my_free(file->sparse_map_buf);
  my_free(file);

  return rc;
}

int xb_stream_write_done(xb_wstream_t *stream) {
  delete stream->mutex;
  my_free(stream);

  return 0;
}

static int xb_stream_flush(xb_wstream_file_t *file) {
  if (file->chunk_ptr == file->chunk) {
    return 0;
  }

  if (xb_stream_write_chunk(file, file->chunk, file->chunk_ptr - file->chunk, 0,
                            nullptr)) {
    return 1;
  }

  file->chunk_ptr = file->chunk;
  file->chunk_free = XB_STREAM_MIN_CHUNK_SIZE;

  return 0;
}

static int xb_stream_write_chunk(xb_wstream_file_t *file, const void *buf,
                                 size_t len, size_t sparse_map_size,
                                 const ds_sparse_chunk_t *sparse_map) {
  /* Chunk magic + flags + chunk type + path_len + path + len + offset +
  checksum + sparse_map_size + */
  uchar tmpbuf[sizeof(XB_STREAM_CHUNK_MAGIC) - 1 + 1 + 1 + 4 + FN_REFLEN + 4 +
               8 + 8 + 4];
  uchar *ptr;
  xb_wstream_t *stream = file->stream;
  ulong checksum;

  /* Write xbstream header */
  ptr = tmpbuf;

  /* Chunk magic */
  memcpy(ptr, XB_STREAM_CHUNK_MAGIC, sizeof(XB_STREAM_CHUNK_MAGIC) - 1);
  ptr += sizeof(XB_STREAM_CHUNK_MAGIC) - 1;

  *ptr++ = 0; /* Chunk flags */

  /* Chunk type */
  *ptr++ = (uchar)(sparse_map_size > 0 ? XB_CHUNK_TYPE_SPARSE
                                       : XB_CHUNK_TYPE_PAYLOAD);

  int4store(ptr, file->path_len); /* Path length */
  ptr += 4;

  memcpy(ptr, file->path, file->path_len); /* Path */
  ptr += file->path_len;

  if (sparse_map_size > 0) {
    /* Sparse map size */
    int4store(ptr, sparse_map_size);
    ptr += 4;
  }

  int8store(ptr, len); /* Payload length */
  ptr += 8;

  /* sparse map */
  if (file->sparse_map_buf_size < 4 * 2 * sparse_map_size) {
    file->sparse_map_buf = static_cast<char *>(
        my_realloc(PSI_NOT_INSTRUMENTED, file->sparse_map_buf,
                   4 * 2 * sparse_map_size, MYF(MY_FAE | MY_ALLOW_ZERO_PTR)));
  }

  char *sparse_ptr = file->sparse_map_buf;
  for (size_t i = 0; i < sparse_map_size; ++i) {
    int4store(sparse_ptr, sparse_map[i].skip);
    sparse_ptr += 4;
    int4store(sparse_ptr, sparse_map[i].len);
    sparse_ptr += 4;
  }

  /* checksum */
  checksum =
      crc32_iso3309(0, reinterpret_cast<const uchar *>(file->sparse_map_buf),
                    4 * 2 * sparse_map_size);
  checksum = crc32_iso3309(checksum, static_cast<const uchar *>(buf), len);

  stream->mutex->lock();

  int8store(ptr, file->offset); /* Payload offset */
  ptr += 8;

  int4store(ptr, checksum);
  ptr += 4;

  xb_ad(ptr <= tmpbuf + sizeof(tmpbuf));

  if (file->write(file, file->userdata, tmpbuf, ptr - tmpbuf) == -1) goto err;

  if (file->write(file, file->userdata, file->sparse_map_buf,
                  4 * 2 * sparse_map_size) == -1)
    goto err;

  if (file->write(file, file->userdata, buf, len) == -1) /* Payload */
    goto err;

  for (size_t i = 0; i < sparse_map_size; ++i)
    file->offset += sparse_map[i].skip;
  file->offset += len;

  stream->mutex->unlock();

  return 0;

err:

  stream->mutex->unlock();

  return 1;
}

static int xb_stream_write_eof(xb_wstream_file_t *file) {
  /* Chunk magic + flags + chunk type + path_len + path */
  uchar tmpbuf[sizeof(XB_STREAM_CHUNK_MAGIC) - 1 + 1 + 1 + 4 + FN_REFLEN];
  uchar *ptr;
  xb_wstream_t *stream = file->stream;

  stream->mutex->lock();

  /* Write xbstream header */
  ptr = tmpbuf;

  /* Chunk magic */
  memcpy(ptr, XB_STREAM_CHUNK_MAGIC, sizeof(XB_STREAM_CHUNK_MAGIC) - 1);
  ptr += sizeof(XB_STREAM_CHUNK_MAGIC) - 1;

  *ptr++ = 0; /* Chunk flags */

  *ptr++ = (uchar)XB_CHUNK_TYPE_EOF; /* Chunk type */

  int4store(ptr, file->path_len); /* Path length */
  ptr += 4;

  memcpy(ptr, file->path, file->path_len); /* Path */
  ptr += file->path_len;

  xb_ad(ptr <= tmpbuf + sizeof(tmpbuf));

  if (file->write(file, file->userdata, tmpbuf, (ulonglong)(ptr - tmpbuf)) ==
      -1)
    goto err;

  stream->mutex->unlock();

  return 0;
err:

  stream->mutex->unlock();

  return 1;
}

#include "ring_buffer.h"
#include <string.h>

void rb_init(Arena *arena, RingBuffer *rb, u32 cap, u32 channels) {
  *rb = (RingBuffer){
      .buf = push_array(arena, f32, cap * channels),
      .capacity_frames = cap,
      .channels = channels,
      .write_pos = 0,
      .read_pos = 0,
      .available = 0,
  };
  mtx_init(&rb->lock, mtx_plain);
}

void rb_free(RingBuffer *rb) {
  mtx_destroy(&rb->lock);
  rb->buf = NULL;
}

// drops oldest data on overflow rather than blocking the capture thread
void rb_write(RingBuffer *rb, const f32 *frames, u32 nframes) {
  mtx_lock(&rb->lock);

  if (nframes > rb->capacity_frames) {
    frames += (nframes - rb->capacity_frames) * rb->channels;
    nframes = rb->capacity_frames;
  }

  u32 frames_to_end = rb->capacity_frames - rb->write_pos;
  u32 part1_frames = min(nframes, frames_to_end);
  u32 part2_frames = nframes - part1_frames;

  memcpy(&rb->buf[rb->write_pos * rb->channels], frames,
         part1_frames * rb->channels * sizeof(f32));

  if (part2_frames > 0) {
    memcpy(rb->buf, &frames[part1_frames * rb->channels],
           part2_frames * rb->channels * sizeof(f32));
  }

  rb->write_pos = (rb->write_pos + nframes) % rb->capacity_frames;
  rb->available += nframes;

  if (rb->available >= rb->capacity_frames) {
    rb->available = rb->capacity_frames;
    rb->read_pos = rb->write_pos;
  }

  mtx_unlock(&rb->lock);
}

// copies upto `nframes` into `out`
u32 rb_read(RingBuffer *rb, f32 *out, u32 nframes) {
  mtx_lock(&rb->lock);

  u32 n = min(nframes, rb->available);
  u32 frames_to_end = rb->capacity_frames - rb->read_pos;

  u32 part1 = min(n, frames_to_end);
  u32 part2 = n - part1;
  memcpy(out, &rb->buf[rb->read_pos * rb->channels],
         part1 * rb->channels * sizeof(f32));

  if (part2 > 0) {
    memcpy(&out[part1 * rb->channels], rb->buf,
           part1 * rb->channels * sizeof(f32));
  }
  rb->read_pos = (rb->read_pos + n) % rb->capacity_frames;
  rb->available -= n;
  mtx_unlock(&rb->lock);

  if (n < nframes) {
    memset(&out[n * rb->channels], 0,
           (nframes - n) * rb->channels * sizeof(f32));
  }
  return n;
}

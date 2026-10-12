#include <string.h>
#include <stdatomic.h>

#include "ring_buffer.h"

void rb_init(Arena *arena, RingBuffer *rb, u32 cap, u32 channels) {
  *rb = (RingBuffer){
      .buf = push_array(arena, f32, cap * channels),
      .capacity_frames = cap,
      .channels = channels,
  };
  atomic_init(&rb->write_idx, 0);
  atomic_init(&rb->write_reserve, 0);
  atomic_init(&rb->read_idx, 0);
}

void rb_free(RingBuffer *rb) {
  rb->buf = NULL;
}

void rb_write(RingBuffer *rb, const f32 *frames, u32 nframes) {
  u32 cap = rb->capacity_frames;
  u32 ch = rb->channels;

  if (nframes > cap) {
    frames += (nframes - cap) * ch;
    nframes = cap;
  }

  u64 w = atomic_load_explicit(&rb->write_idx, memory_order_relaxed);
  u64 new_w = w + nframes;

  // announce intent *BEFORE* touching the buffer, so a reader can tell
  // that slots for frames < new_w - cap are being overwritten.
  atomic_store_explicit(&rb->write_reserve, new_w, memory_order_relaxed);
  atomic_thread_fence(memory_order_release);

  // copy
  u32 pos = (u32)(w % cap);
  u32 part1 = min(nframes, cap - pos);
  u32 part2 = nframes - part1;

  memcpy(&rb->buf[pos * ch], frames, part1 * ch * sizeof(f32));
  if (part2 > 0) {
    memcpy(rb->buf, &frames[part1 * ch], part2 * ch * sizeof(f32));
  }

  // publish
  atomic_store_explicit(&rb->write_idx, new_w, memory_order_release);
}

u32 rb_read(RingBuffer *rb, f32 *out, u32 nframes) {
  u32 cap = rb->capacity_frames;
  u32 ch = rb->channels;

  u64 r = atomic_load_explicit(&rb->read_idx, memory_order_relaxed);
  u64 w = atomic_load_explicit(&rb->write_idx, memory_order_acquire);

  // already lapped before we started so skip to oldest surviving frame
  if (w - r > cap) {
    r = w - cap;
  }

  u32 n = min(nframes, (u32)(w - r));
  u32 got = 0;

  if (n > 0) {
    u32 pos = (u32)(r % cap);
    u32 part1 = min(n, cap - pos);
    u32 part2 = n - part1;

    memcpy(out, &rb->buf[pos * ch], part1 * ch * sizeof(f32));
    if (part2 > 0) {
      memcpy(&out[part1 * ch], rb->buf, part2 * ch * sizeof(f32));
    }

    // did the writer start overwriting what we copied?
    atomic_thread_fence(memory_order_acquire);
    u64 reserve =
        atomic_load_explicit(&rb->write_reserve, memory_order_relaxed);
    u64 valid_start = reserve > cap ? reserve - cap : 0;

    u32 bad = 0;
    if (valid_start > r) {
      u64 b = valid_start - r;
      bad = (u32)min(b, (u64)n);
    }

    got = n - bad;
    if (bad > 0 && got > 0) {
      memmove(out, &out[bad * ch], got * ch * sizeof(f32));
    }

    atomic_store_explicit(&rb->read_idx, r + n, memory_order_relaxed);
  }

  if (got < nframes) {
    memset(&out[got * ch], 0, (nframes - got) * ch * sizeof(f32));
  }
  return got;
}

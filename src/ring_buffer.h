#ifndef _H_AWAAZ_RING_BUFFER
#define _H_AWAAZ_RING_BUFFER

#include <bace/bace.h>

// float lock-free ring buffer, one per output device.
// stores interleaved frames in the capture device's format
//
// TODO: maybe use page-mapping tech?
typedef struct {
  f32 *buf;
  u32 capacity_frames;
  u32 channels;

  alignas(64) _Atomic u64 write_idx;
  alignas(64) _Atomic u64 write_reserve;
  alignas(64) _Atomic u64 read_idx;
} RingBuffer;

void rb_init(Arena *arena, RingBuffer *rb, u32 cap, u32 channels);

void rb_free(RingBuffer *rb);

void rb_write(RingBuffer *rb, const f32 *frames, u32 nframes);

u32 rb_read(RingBuffer *rb, f32 *out, u32 nframes);

#endif // ! _H_AWAAZ_RING_BUFFER

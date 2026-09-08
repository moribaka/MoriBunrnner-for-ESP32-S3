#pragma once

/* All users allocate these transfer buffers on the heap, never task stacks. */
#define TF_IO_CHUNK_SIZE (16U * 1024U)
#define BURNER_UI_NOTIFY_INTERVAL_US 100000ULL

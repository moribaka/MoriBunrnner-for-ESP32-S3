#pragma once
#include "freertos/FreeRTOS.h"
#include <cstddef>
#include <cstdio>
namespace lyra::sd {
enum class Client { Audio };
bool init();
bool acquire(Client, TickType_t timeout = portMAX_DELAY);
void release(Client);
FILE *open(const char *path, const char *mode, Client);
int close(FILE *file, Client);
size_t read(FILE *file, void *data, size_t size, Client);
int seek(FILE *file, long offset, int origin, Client);
} // namespace lyra::sd

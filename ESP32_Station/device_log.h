#ifndef DEVICE_LOG_H
#define DEVICE_LOG_H

#include <stddef.h>

void deviceLogBegin();
void deviceLog(const char *fmt, ...);
void deviceLogRaw(const char *tag, const char *text);
bool deviceLogTail(int maxLines, char *out, size_t outSize);

#endif

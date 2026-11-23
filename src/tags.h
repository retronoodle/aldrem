/* tags.h — embedded title/artist tags for local audio files. Standalone: no
 * decoder or SDL involved, so it can be called from any thread.
 */
#ifndef TAGS_H
#define TAGS_H

#include <stddef.h>

/* Reads the title and artist from path's embedded metadata (ID3v2/ID3v1 for
 * MP3, Vorbis comments for FLAC and Ogg, LIST/INFO for WAV, NAME/AUTH for
 * AIFF). Outputs are UTF-8 and NUL-terminated, truncated to their capacity.
 * Returns 1 when either field was found, 0 otherwise (including unreadable
 * or untagged files). */
int tags_read(const char *path, char *artist, size_t acap,
              char *title, size_t tcap);

#endif

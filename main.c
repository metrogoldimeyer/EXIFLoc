#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>

/* Great resources:
 * TIFF (Format of Exif data):
 * https://www.adobe.io/content/udp/en/open/standards/TIFF/_jcr_content/contentbody/download/file.res/TIFF6.pdf
 *
 * Exif/JPEG:
 * https://www.media.mit.edu/pia/Research/deepview/exif.html
 *
 * My other program: TEPSniff (another TIFF parser)
 * the endian code came from that project.
 * https://github.com/enferex/tepsniff/
 */

#define MARKER_SOI 0xD8 // Start of Image
#define MARKER_EOI 0xD9 // End of Image
#define MARKER_SOS 0xDA // Start of Stream

#define _PR(_fd, _pre, _msg, ...)        \
  do {                                   \
    fprintf(_fd, _pre " " _msg "\n", ##__VA_ARGS__); \
  } while (0)

#define FAIL(_msg, ...)                    \
  do {                               \
    _PR(stderr, "[!]", _msg, ##__VA_ARGS__); \
    exit(EXIT_FAILURE);              \
  } while (0)

#define PR(...) _PR(stdout, "[-]", __VA_ARGS__)

#ifdef DEBUG
#define DBG(...) _PR(stdout, "[+]",  __VA_ARGS__)
#else
#define DBG(...)
#endif

/* Macros to handle endiness of TIFF. 4 cases.
 * This was originally from my other project:
 * https://github.com/enferex/tepsniff/
 * 1) BE system and TIFF is BE
 * 2) BE system and TIFF is LE
 * 3) LE system and TIFF is BE
 * 4) LE system and TIFF is LE
 */
#define IS_BE(_h) (_h->hdr.byte_order == 0x4D4D)
#if __BYTE_ORDER == __BIG_ENDIAN
#define NATIVE2(_h,_v) (IS_BE(_h) ? (_v) : le16toh(_v))
#define NATIVE4(_h,_v) (IS_BE(_h) ? (_v) : le32toh(_v))
#elif __BYTE_ORDER == __LITTLE_ENDIAN
#define NATIVE2(_h,_v) (IS_BE(_h) ? htobe16(_v) : (_v))
#define NATIVE4(_h,_v) (IS_BE(_h) ? htobe32(_v) : (_v))
#else
#error "Middle endian not supported."
#endif

// This represents the tag to search for. (ifd tag and type).
typedef struct { uint16_t tag, type; } locator_t;

typedef struct {uint32_t lat, lon; } coords_t;

typedef struct _ifd_entry_t {
  uint16_t tag;
  uint16_t type;
  uint32_t n_components;
  uint32_t value_offset;
} ifd_entry_t;

// This is really the entire IFD but with a next pointer so that we can chain
// IFDs together.
typedef struct _ifd_t {
  uint16_t n_entries;
  ifd_entry_t *entries;
  struct _ifd_t *next;
} ifd_t;

typedef struct _tiff_hdr_t {
  uint16_t byte_order; // "II": Little endian, "MM": Big endian.
  uint16_t universe;   // Must be 42.
  uint32_t offset;     // Initial IFD offset
} tiff_hdr_t;

typedef struct _tiff_t {
  tiff_hdr_t hdr;
  ifd_t *ifds;
} tiff_t;

typedef struct _exif_t {
#define EXIF_HDR_BYTES 6 // This is the first 6 bytes in 'data': 'Exif00'
  size_t   size;
  uint8_t *data;
  tiff_t  *tiff;
} exif_t;

static void usage(const char *execname) {
  printf("Usage: %s [jpeg ...]\n", execname);
  exit(EXIT_SUCCESS);
}

static int safe_fgetc(FILE *fp) {
  const int byte = fgetc(fp);
  if (byte == EOF)
    FAIL("Error reading input file.");
  return byte;
}

// Returns true if the marker is located.
static uint8_t read_marker(FILE *fp, _Bool fatal) {
  const int byte = safe_fgetc(fp);
  if (byte != 0xFF) {
    // We did not get a marker header which we expected.
    if (fatal)
      FAIL("Invalid start of marker, expected 0xFF.");
    return 0;
  }
  return (uint8_t)safe_fgetc(fp);
}

static uint16_t read_marker_size(FILE *fp) {
  const uint8_t hi = safe_fgetc(fp);
  const uint8_t lo = safe_fgetc(fp);
  return (hi << 8) | lo;
}

static _Bool find_marker(FILE *fp, int marker) {
  while (!feof(fp) && !ferror(fp)) {
    const uint8_t current_marker = read_marker(fp, true);
    DBG("Marker: 0x%02x", current_marker);
    if (current_marker == marker)
      return true;

    // Special cases.
    if (current_marker == MARKER_SOS)
      return false; // This will lead to the EOI, so we are done with this image.
    else if (current_marker == MARKER_SOI || current_marker == MARKER_EOI)
      continue;
    else {
      // Skip bytes if not SOI, SOS, or EOI.
      const uint16_t skip = read_marker_size(fp) - 2;
      DBG("Skipping %d bytes.", skip);
      if (fseek(fp, skip, SEEK_CUR) != 0)
        FAIL("Error seeking to next marker.");
    }
  }

  return false;
}

static void free_exif(exif_t *ex) {
  free(ex->tiff);
  free(ex->data);
  free(ex);
}

// Given an exif and an offset, read data from the exif data blob.
static _Bool read_data_from_exif(
    void         *dest,
    uint64_t      offset,
    size_t        size,
    const exif_t *ex) {
  if (offset + size > ex->size)
    return false;
  return memcpy(dest, ex->data + offset, size) != NULL;
}

// This updates offset to point at the end of just-read-in ifd.
static ifd_t *read_ifd(const exif_t *ex, const tiff_t *tiff, uint64_t *offset) {
  // Read in the number of entries for this IFD.
  uint16_t n_entries;
  if (!read_data_from_exif((void *)&n_entries, *offset, sizeof(n_entries), ex))
    FAIL("Error loading IFD entry count.");
  n_entries = NATIVE2(tiff, n_entries);
  *offset += sizeof(n_entries);

  ifd_t *ifd = calloc(1, sizeof(ifd));
  if (!ifd)
    FAIL("Error allocating memory to store an IFD.");

  // Advance to the start of the first entry.
  DBG("Reading %d entries starting at offset 0x%lx.", n_entries, *offset);
  ifd->n_entries = n_entries;

  // Instead of allocating more memory, and IFD entries are contiguous,
  // just point the start of the entires to 'entries' in this header.
  ifd->entries = (ifd_entry_t *)(ex + *offset);
  *offset += n_entries * sizeof(ifd_entry_t);

  // Offset to next IFD.
  uint32_t next = *(uint32_t *)(ex + *offset);
  *offset = NATIVE4(tiff, next);
  DBG("Next IFD offset is: 0x%lx.", *offset);

  return ifd;
}

static tiff_t *exif_to_tiff(const exif_t *ex) {
  tiff_t *tiff = calloc(1, sizeof(tiff_t));
  if (!tiff)
    FAIL("Error allocating memory to store Exif in TIFF format.");

  // The TIFF header follows the EXIF header, so use the exif header size as the
  // offset.
  if (!read_data_from_exif((void *)tiff, EXIF_HDR_BYTES, sizeof(tiff->hdr), ex))
      FAIL("Error reading TIFF header.");

  // Fix up the header so that we can make sense of it.
  tiff->hdr.universe = NATIVE2(tiff, tiff->hdr.universe);
  tiff->hdr.offset   = NATIVE4(tiff, tiff->hdr.offset);

  // Sanity check.
  if (tiff->hdr.universe != 42)
    FAIL("Invalid TIFF format.");

  // Initialize the IFD scan by placing the offset at the beginning of the TIFF
  // data.  We advance past the first header (EXIF) since the TIFF data offset
  // is calculated starting at the beginning of the TIFF blob (after the EXIF
  // header).
  uint64_t off = EXIF_HDR_BYTES + tiff->hdr.offset;

  // Read in all of the IFD entries, off will be zero at the end.
  ifd_t *prev = NULL;
  while (off && (off < ex->size)) {
    ifd_t *ifd = read_ifd(ex, tiff, &off);
    ifd->next = prev;
    prev = ifd;
  }

  return tiff;
}

static exif_t *read_exif(const char *filename) {
  DBG("Loading %s", filename);
  FILE *fp = fopen(filename, "r");
  if (!fp)
    FAIL("Error opening %s: %s", filename, strerror(errno));

  exif_t *ex;
  if (!(ex = calloc(1, sizeof(exif_t))))
    FAIL("Error allocating memory to store an exif instance.");

  // Find the start of the image.
  const uint8_t header_marker = read_marker(fp, false);
  if (header_marker != 0xD8) {
    DBG("%s: Not a file format we know how to handle.", filename);
    goto oops; // Not an image format we know how to handle.
  }

  // Find the start of APP1 (where exif lives).
  if (!find_marker(fp, 0xE1)) {
    DBG("%s: Could not locate EXIF data.", filename);
    goto oops; // Couldnt find exif.
  }

  // Assume we have a valid app1, read in its contents.
  ex->size = read_marker_size(fp) - 2;
  if (!(ex->data = calloc(1, ex->size)))
    FAIL("Error allocating exif data contents.");
  if (fread(ex->data, 1, ex->size, fp) != ex->size)
    FAIL("Error reading exif contents.");

  fclose(fp);

  // Verify the exif header.  Return NULL if invalid.
  if (strncmp((const char *)ex->data, "Exif", 4) == 0 &&
      ex->data[4] == 0  && ex->data[5] == 0) {
    // Set the TIFF data.
    ex->tiff = exif_to_tiff(ex);
    DBG("%s: Located %zu bytes of Exif data.", filename, ex->size);
    return ex;
  }

oops:
    free(ex);
    return NULL;
}

static void dump(const exif_t *ex) {
  if (!ex->tiff)
    return;

  int ifd_number = 0;
  for (const ifd_t *ifd = ex->tiff->ifds; ifd; ifd=ifd->next)
    for (int i=0; i<ifd->n_entries; ++i)
      DBG("IFD:%d -- Tag 0x%04x", ifd_number, ifd->entries[i].tag);
}

/**
static locator_t *find_locator(const locators_t *locs, const uint16_t tag) {
    const locator_t *loc = locs;
    for (int i=0; i<locs->n_locators; ++i)
      if (locs[i].tag == tag)
        return &locs[i];
    return NULL;
}

static void locate_tags(const exif_t *ex, const locator_t *locs) {
    assert(ex->tiff && "No TIFF/EXIF tags available.");
    for (const ifd_t *ifd = ex->tiff->ifds; ifd; ifd=ifd->next) {
      for (uint16_t i=0; i<ifd->n_entries; ++i) {
        const ifd_entry_t *entry = &ifd->entries[i];
        const uint16_t tag = NATIVE2(ex->tiff, entry->tag);
        if (is_match(locators, tag)
      }
    }
}
**/

int main(int argc, char **argv) {
  if (argc == 0)
    usage(argv[0]);

  // We only care about GPS markers.
  locator_t locators[] = {
//      {0x8825, 0x0000, gps_handler} // GPS tag and some type.
  };

  // For each file specified on the command line.
  for (int i=1; i<argc; ++i) {
    exif_t *ex;
    if ((ex = read_exif(argv[i]))) {
      dump(ex);
      free_exif(ex);
    }

    // Search.
 //   locate_tags(ex, locators);
  }

  return 0;
}

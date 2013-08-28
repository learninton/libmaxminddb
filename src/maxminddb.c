#include "maxminddb.h"
#include <arpa/inet.h>
#include <assert.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if HAVE_CONFIG_H
#include <config.h>
#endif

#if MMDB_DEBUG
#define LOCAL
#else
#define LOCAL static
#endif

typedef union {
    struct in_addr v4;
    struct in6_addr v6;
} in_addr_any;

#define MMDB_DATA_SECTION_SEPARATOR (16)

#define METADATA_MARKER "\xab\xcd\xefMaxMind.com"
#define METADATA_BLOCK_MAX_SIZE 20000

/* --prototypes automatically generated by dev-bin/regen-prototypes.pl - don't remove this comment */
LOCAL int resolve_any_address(const char *ipstr, int is_ipv4,
                              in_addr_any *in_addr);
LOCAL int find_address_in_search_tree(MMDB_s *mmdb, uint8_t *address,
                                      MMDB_lookup_result_s *result);
LOCAL uint32_t get_left_28_bit_record(const uint8_t *record);
LOCAL uint32_t get_right_28_bit_record(const uint8_t *record);
LOCAL void populate_description_metadata(MMDB_s *mmdb);
LOCAL int read_metadata(MMDB_s *mmdb, uint8_t *metadata_content, ssize_t size);
LOCAL uint32_t value_for_key_as_uint16(MMDB_entry_s *start, char *key);
LOCAL uint32_t value_for_key_as_uint32(MMDB_entry_s *start, char *key);
LOCAL uint64_t value_for_key_as_uint64(MMDB_entry_s *start, char *key);
LOCAL char *value_for_key_as_string(MMDB_entry_s *start, char *key);
LOCAL void populate_languages_metadata(MMDB_s *mmdb);
LOCAL uint16_t init(MMDB_s *mmdb, const char *fname, uint32_t flags);
LOCAL void free_all(MMDB_s *mmdb);
LOCAL void skip_hash_array(MMDB_s *mmdb, MMDB_entry_data_s *entry_data);
LOCAL void decode_one_follow(MMDB_s *mmdb, uint32_t offset,
                             MMDB_entry_data_s *entry_data);
LOCAL void decode_one(MMDB_s *mmdb, uint32_t offset,
                      MMDB_entry_data_s *entry_data);
LOCAL int get_ext_type(int raw_ext_type);
LOCAL void DPRINT_KEY(MMDB_s *mmdb, MMDB_entry_data_s *entry_data);
LOCAL uint32_t get_ptr_from(uint8_t ctrl, uint8_t const *const ptr,
                            int ptr_size);
LOCAL void get_entry_data_list(MMDB_s *mmdb, uint32_t offset,
                               MMDB_entry_data_list_s *entry_data_list);
LOCAL void silly_pindent(int i);
LOCAL float get_ieee754_float(const uint8_t *restrict p);
LOCAL double get_ieee754_double(const uint8_t *restrict p);
LOCAL uint32_t get_uint32(const uint8_t *p);
LOCAL uint32_t get_uint24(const uint8_t *p);
LOCAL uint32_t get_uint16(const uint8_t *p);
LOCAL uint64_t get_uintX(const uint8_t *p, int length);
LOCAL int32_t get_sintX(const uint8_t *p, int length);
LOCAL int int_pread(int fd, uint8_t *buffer, ssize_t to_read, off_t offset);
LOCAL MMDB_entry_data_list_s *dump(MMDB_s *mmdb,
                                   MMDB_entry_data_list_s *entry_data_list,
                                   int indent);
/* --prototypes end - don't remove this comment-- */

#if !defined HAVE_MEMMEM
LOCAL void *memmem(const void *big, size_t big_len, const void *little,
                   size_t little_len)
{
    if (little_len) {
        int first_char = ((uint8_t *)little)[0];
        const void *ptr = big;
        size_t len = big_len;
        while (len >= little_len
               && (ptr = memchr(ptr, first_char, len - little_len + 1))) {
            if (!memcmp(ptr, little, little_len)) {
                return (void *)ptr;
            }
            len = big_len - (++ptr - big);
        }
    }
    return NULL;
}
#endif

int MMDB_resolve_address(const char *host, int ai_family, int ai_flags,
                         void *ip)
{
    struct addrinfo hints = {.ai_family = ai_family,
        .ai_flags = ai_flags,
        .ai_socktype = SOCK_STREAM
    }, *aifirst;
    int gaierr = getaddrinfo(host, NULL, &hints, &aifirst);

    if (gaierr == 0) {
        if (ai_family == AF_INET) {
            memcpy(&((struct in_addr *)ip)->s_addr,
                   &((struct sockaddr_in *)aifirst->ai_addr)->sin_addr, 4);
        } else if (ai_family == AF_INET6) {
            memcpy(&((struct in6_addr *)ip)->s6_addr,
                   ((struct sockaddr_in6 *)aifirst->ai_addr)->sin6_addr.s6_addr,
                   16);

        } else {
            /* should never happen */
            assert(0);
        }
        freeaddrinfo(aifirst);
    }
    return gaierr;
}

MMDB_lookup_result_s *MMDB_lookup(MMDB_s *mmdb, const char *ipstr,
                                  int *gai_error, int *mmdb_error)
{
    int is_ipv4 = mmdb->metadata.ip_version == 4 ? 1 : 0;
    in_addr_any in_addr;

    *gai_error = resolve_any_address(ipstr, is_ipv4, &in_addr);

    if (*gai_error) {
        return NULL;
    }

    MMDB_lookup_result_s *result = malloc(sizeof(MMDB_lookup_result_s));
    assert(result != NULL);

    result->entry.mmdb = mmdb;

    uint8_t *address;
    if (is_ipv4) {
        address = (uint8_t *)&in_addr.v4.s_addr;
    } else {
        address = (uint8_t *)&in_addr.v6;
    }

    *mmdb_error = find_address_in_search_tree(mmdb, address, result);

    if (*mmdb_error) {
        free(result);
        return NULL;
    }

    if (result->entry.offset > 0) {
        return result;
    } else {
        free(result);
        return NULL;
    }
}

LOCAL int resolve_any_address(const char *ipstr, int is_ipv4,
                              in_addr_any *in_addr)
{
    int ai_flags = AI_NUMERICHOST;
    struct addrinfo hints = {
        .ai_socktype = SOCK_STREAM
    };
    struct addrinfo *addresses;
    int gai_status;

    if (is_ipv4) {
        hints.ai_flags = ai_flags;
        hints.ai_family = AF_INET;
    } else {
        hints.ai_flags = ai_flags | AI_V4MAPPED;
        hints.ai_family = AF_INET6;
    }

    gai_status = getaddrinfo(ipstr, NULL, &hints, &addresses);
    if (gai_status) {
        return gai_status;
    }

    if (hints.ai_family == AF_INET) {
        memcpy(&((struct in_addr *)in_addr)->s_addr,
               &((struct sockaddr_in *)addresses->ai_addr)->sin_addr.s_addr, 4);
    } else if (hints.ai_family == AF_INET6) {
        memcpy(&((struct in6_addr *)in_addr)->s6_addr,
               ((struct sockaddr_in6 *)addresses->ai_addr)->sin6_addr.s6_addr,
               16);
    } else {
        /* This should never happen */
        assert(0);
    }

    freeaddrinfo(addresses);

    return 0;
}

LOCAL int find_address_in_search_tree(MMDB_s *mmdb, uint8_t *address,
                                      MMDB_lookup_result_s *result)
{
    uint32_t node_count = mmdb->metadata.node_count;
    uint16_t record_length = mmdb->full_record_byte_size;
    const uint8_t *search_tree = mmdb->file_in_mem_ptr;
    const uint8_t *record_pointer;
    uint16_t max_depth0 = mmdb->depth - 1;
    uint32_t value = 0;

    uint32_t (*left_record_value) (const uint8_t *);
    uint32_t (*right_record_value) (const uint8_t *);
    uint8_t right_record_offset;

    if (record_length == 6) {
        left_record_value = &get_uint24;
        right_record_value = &get_uint24;
        right_record_offset = 3;
    } else if (record_length == 7) {
        left_record_value = &get_left_28_bit_record;
        right_record_value = &get_right_28_bit_record;
        right_record_offset = 3;
    } else if (record_length == 8) {
        left_record_value = &get_uint32;
        right_record_value = &get_uint32;
        right_record_offset = 4;
    }

    for (uint16_t current_bit = max_depth0; current_bit >= 0; current_bit--) {
        record_pointer = &search_tree[value * record_length];
        if (address[(max_depth0 - current_bit) >> 3] &
            (1U << (~(max_depth0 - current_bit) & 7))) {

            record_pointer += right_record_offset;
            value = right_record_value(record_pointer);
        } else {
            value = left_record_value(record_pointer);
        }

        if (value >= node_count) {
            result->netmask = mmdb->depth - current_bit;
            result->entry.offset = value - node_count;
            return MMDB_SUCCESS;
        }
    }

    // We should not be able to reach this return. If we do, something very bad happened.
    return MMDB_CORRUPT_DATABASE;
}

LOCAL uint32_t get_left_28_bit_record(const uint8_t *record)
{
    return record[0] * 65536 + record[1] * 256 + record[2] +
        ((record[3] & 0xf0) << 20);
}

LOCAL uint32_t get_right_28_bit_record(const uint8_t *record)
{
    uint32_t value = get_uint32(record);
    return value & 0xfffffff;
}

LOCAL void populate_description_metadata(MMDB_s *mmdb)
{
    MMDB_entry_data_s entry_data;
    MMDB_entry_s map_start;
    size_t map_size;
    MMDB_entry_data_list_s *member;
    MMDB_entry_data_list_s *first_member;
    int i;

    MMDB_get_value(&mmdb->meta, &entry_data, "description", NULL);

    assert(entry_data.type == MMDB_DTYPE_MAP);

    map_start.mmdb = mmdb->fake_metadata_db;
    map_start.offset = entry_data.offset;

    MMDB_get_entry_data_list(&map_start, &member);

    first_member = member;

    map_size = member->entry_data.data_size;
    mmdb->metadata.description.count = map_size;
    mmdb->metadata.description.descriptions =
        malloc(map_size * sizeof(MMDB_description_s *));

    for (i = 0; i < map_size; i++) {
        mmdb->metadata.description.descriptions[i] =
            malloc(sizeof(MMDB_description_s));

        member = member->next;
        assert(member->entry_data.type == MMDB_DTYPE_UTF8_STRING);
        mmdb->metadata.description.descriptions[i]->language =
            strndup((char *)member->entry_data.utf8_string,
                    member->entry_data.data_size);

        member = member->next;
        assert(member->entry_data.type == MMDB_DTYPE_UTF8_STRING);
        mmdb->metadata.description.descriptions[i]->description =
            strndup((char *)member->entry_data.utf8_string,
                    member->entry_data.data_size);
    }

    MMDB_free_entry_data_list(first_member);
}

LOCAL int read_metadata(MMDB_s *mmdb, uint8_t *metadata_content, ssize_t size)
{
    const uint8_t *metadata = memmem(metadata_content, size, METADATA_MARKER,
                                     strlen(METADATA_MARKER));
    if (NULL == metadata) {
        return MMDB_INVALID_DATABASE;
    }

    mmdb->fake_metadata_db = calloc(1, sizeof(struct MMDB_s));
    assert(mmdb->fake_metadata_db != NULL);

    mmdb->fake_metadata_db->dataptr = metadata + strlen(METADATA_MARKER);
    mmdb->meta.mmdb = mmdb->fake_metadata_db;

    mmdb->metadata.node_count =
        value_for_key_as_uint32(&mmdb->meta, "node_count");

    mmdb->metadata.record_size =
        value_for_key_as_uint16(&mmdb->meta, "record_size");

    if (mmdb->metadata.record_size != 24 && mmdb->metadata.record_size != 28
        && mmdb->metadata.record_size != 32) {
        return MMDB_UNKNOWN_DATABASE_FORMAT;
    }

    mmdb->metadata.ip_version =
        value_for_key_as_uint16(&mmdb->meta, "ip_version");

    mmdb->metadata.database_type =
        value_for_key_as_string(&mmdb->meta, "database_type");

    populate_languages_metadata(mmdb);

    mmdb->metadata.binary_format_major_version =
        value_for_key_as_uint16(&mmdb->meta, "binary_format_major_version");

    mmdb->metadata.binary_format_minor_version =
        value_for_key_as_uint16(&mmdb->meta, "binary_format_minor_version");

    mmdb->metadata.build_epoch =
        value_for_key_as_uint64(&mmdb->meta, "build_epoch");

    populate_description_metadata(mmdb);

    mmdb->full_record_byte_size = mmdb->metadata.record_size * 2 / 8U;

    mmdb->depth = mmdb->metadata.ip_version == 4 ? 32 : 128;

    return MMDB_SUCCESS;
}

LOCAL uint32_t value_for_key_as_uint16(MMDB_entry_s *start, char *key)
{
    MMDB_entry_data_s entry_data;
    MMDB_get_value(start, &entry_data, key, NULL);
    return entry_data.uint16;
}

LOCAL uint32_t value_for_key_as_uint32(MMDB_entry_s *start, char *key)
{
    MMDB_entry_data_s entry_data;
    MMDB_get_value(start, &entry_data, key, NULL);
    return entry_data.uint32;
}

LOCAL uint64_t value_for_key_as_uint64(MMDB_entry_s *start, char *key)
{
    MMDB_entry_data_s entry_data;
    MMDB_get_value(start, &entry_data, key, NULL);
    return entry_data.uint64;
}

LOCAL char *value_for_key_as_string(MMDB_entry_s *start, char *key)
{
    MMDB_entry_data_s entry_data;
    MMDB_get_value(start, &entry_data, key, NULL);
    return strndup((char *)entry_data.utf8_string, entry_data.data_size);
}

LOCAL void populate_languages_metadata(MMDB_s *mmdb)
{
    MMDB_entry_data_s entry_data;
    MMDB_entry_s array_start;
    size_t array_size;
    MMDB_entry_data_list_s *member;
    MMDB_entry_data_list_s *first_member;
    int i;

    MMDB_get_value(&mmdb->meta, &entry_data, "languages", NULL);

    assert(entry_data.type == MMDB_DTYPE_ARRAY);

    array_start.mmdb = mmdb->fake_metadata_db;
    array_start.offset = entry_data.offset;

    MMDB_get_entry_data_list(&array_start, &member);

    first_member = member;

    array_size = member->entry_data.data_size;
    mmdb->metadata.languages.count = array_size;
    mmdb->metadata.languages.names = malloc(array_size * sizeof(char *));

    for (i = 0; i < array_size; i++) {
        member = member->next;
        assert(member->entry_data.type == MMDB_DTYPE_UTF8_STRING);

        mmdb->metadata.languages.names[i] =
            strndup((char *)member->entry_data.utf8_string,
                    member->entry_data.data_size);
        assert(mmdb->metadata.languages.names[i] != NULL);
    }

    MMDB_free_entry_data_list(first_member);
}

uint16_t MMDB_open(const char *fname, uint32_t flags, MMDB_s *mmdb)
{
    uint16_t status;

    MMDB_DBG_CARP("MMDB_open %s %d\n", fname, flags);

    return init(mmdb, fname, flags);
}

LOCAL uint16_t init(MMDB_s *mmdb, const char *fname, uint32_t flags)
{
    mmdb->fname = strdup(fname);
    if (mmdb->fname == NULL) {
        return MMDB_OUT_OF_MEMORY;
    }

    int fd = open(fname, O_RDONLY);
    if (fd < 0) {
        return MMDB_FILE_OPEN_ERROR;
    }

    struct stat s;
    fstat(fd, &s);

    if ((flags & MMDB_MODE_MASK) == 0) {
        flags |= MMDB_MODE_STANDARD;
    }
    mmdb->flags = flags;
    ssize_t size;
    mmdb->file_size = size = s.st_size;

    uint8_t *metadata_content = malloc(METADATA_BLOCK_MAX_SIZE);
    assert(metadata_content != NULL);

    if (metadata_content == NULL) {
        return MMDB_INVALID_DATABASE;
    }

    off_t offset = size > METADATA_BLOCK_MAX_SIZE ? METADATA_BLOCK_MAX_SIZE : 0;
    if (MMDB_SUCCESS != int_pread(fd, metadata_content, size, offset)) {
        free(metadata_content);
        return MMDB_IO_ERROR;
    }

    int ok = read_metadata(mmdb, metadata_content, size);
    free(metadata_content);
    if (MMDB_SUCCESS != ok) {
        return ok;
    }

    if (mmdb->metadata.binary_format_major_version != 2) {
        MMDB_close(mmdb);
        return MMDB_UNKNOWN_DATABASE_FORMAT;
    }

    uint8_t *file_content;
    if ((flags & MMDB_MODE_MASK) == MMDB_MODE_MEMORY_CACHE) {
        file_content = malloc(size);
        if (MMDB_SUCCESS != int_pread(fd, file_content, size, 0)) {
            free(file_content);
            return MMDB_IO_ERROR;
        }
    } else {
        file_content =
            (uint8_t *)mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
        if (MAP_FAILED == file_content) {
            return MMDB_IO_ERROR;
        }
    }

    close(fd);

    mmdb->file_in_mem_ptr = file_content;
    mmdb->dataptr =
        file_content + mmdb->metadata.node_count * mmdb->full_record_byte_size;

    return MMDB_SUCCESS;
}

void MMDB_close(MMDB_s *mmdb)
{
    free_all(mmdb);
}

LOCAL void free_all(MMDB_s *mmdb)
{
    if (!mmdb) {
        return;
    }

    if (mmdb->fname) {
        free(mmdb->fname);
    }
    if (mmdb->file_in_mem_ptr) {
        if ((mmdb->flags & MMDB_MODE_MASK) == MMDB_MODE_MEMORY_CACHE) {
            free((void *)mmdb->file_in_mem_ptr);
        } else {
            munmap((void *)mmdb->file_in_mem_ptr, mmdb->file_size);
        }
    }
    if (mmdb->fake_metadata_db) {
        free(mmdb->fake_metadata_db);
    }
    if (mmdb->metadata.database_type) {
        free(mmdb->metadata.database_type);
    }
    if (mmdb->metadata.languages.names) {
        int i;
        for (i = 0; i < mmdb->metadata.languages.count; i++) {
            free((char *)mmdb->metadata.languages.names[i]);
        }
        free(mmdb->metadata.languages.names);
    }
    if (mmdb->metadata.description.descriptions) {
        int i;
        for (i = 0; i < mmdb->metadata.description.count; i++) {
            free((char *)mmdb->metadata.description.descriptions[i]->language);
            free((char *)mmdb->metadata.description.descriptions[i]->
                 description);
            free(mmdb->metadata.description.descriptions[i]);
        }
        free(mmdb->metadata.description.descriptions);
    }
    free((void *)mmdb);
}

int MMDB_get_value(MMDB_entry_s *start, MMDB_entry_data_s *entry_data, ...)
{
    va_list keys;
    va_start(keys, entry_data);
    int ioerror = MMDB_vget_value(start, entry_data, keys);
    va_end(keys);
    return ioerror;
}

int MMDB_vget_value(MMDB_entry_s *start, MMDB_entry_data_s *entry_data,
                    va_list params)
{
    MMDB_entry_data_s key, value;
    MMDB_s *mmdb = start->mmdb;
    uint32_t offset = start->offset;
    char *src_key;              // = va_arg(params, char *);
    int src_keylen;

    while (src_key = va_arg(params, char *)) {
        MMDB_DBG_CARP("decode_one src_key:%s\n", src_key);
        decode_one(mmdb, offset, entry_data);
 donotdecode:
        src_keylen = strlen(src_key);
        switch (entry_data->type) {
        case MMDB_DTYPE_PTR:
            // we follow the pointer
            decode_one(mmdb, entry_data->pointer, entry_data);
            break;

            // learn to skip this
        case MMDB_DTYPE_ARRAY:
            {
                int size = entry_data->data_size;
                int offset = strtol(src_key, NULL, 10);
                if (offset >= size || offset < 0) {
                    entry_data->offset = 0;     // not found.
                    goto end;
                }
                for (int i = 0; i < offset; i++) {
                    decode_one(mmdb, entry_data->offset_to_next, entry_data);
                    skip_hash_array(mmdb, entry_data);
                }
                if (src_key = va_arg(params, char *)) {
                    decode_one_follow(mmdb, entry_data->offset_to_next,
                                      entry_data);
                    offset = entry_data->offset_to_next;
                    goto donotdecode;
                }
                decode_one_follow(mmdb, entry_data->offset_to_next, &value);
                memcpy(entry_data, &value, sizeof(MMDB_entry_data_s));
                goto end;
            }
            break;
        case MMDB_DTYPE_MAP:
            {
                int size = entry_data->data_size;
                offset = entry_data->offset_to_next;
                while (size-- > 0) {
                    decode_one(mmdb, offset, &key);

                    uint32_t offset_to_value = key.offset_to_next;

                    if (key.type == MMDB_DTYPE_PTR) {
                        decode_one(mmdb, key.pointer, &key);
                    }

                    assert(key.type == MMDB_DTYPE_UTF8_STRING);

                    if (key.data_size == src_keylen &&
                        !memcmp(src_key, key.utf8_string, src_keylen)) {

                        if (src_key = va_arg(params, char *)) {
                            decode_one_follow(mmdb, offset_to_value,
                                              entry_data);
                            offset = entry_data->offset_to_next;

                            goto donotdecode;
                        }
                        // found it!
                        decode_one_follow(mmdb, offset_to_value, &value);
                        memcpy(entry_data, &value, sizeof(MMDB_entry_data_s));
                        goto end;
                    } else {
                        // we search for another key skip  this
                        decode_one(mmdb, offset_to_value, &value);
                        skip_hash_array(mmdb, &value);
                        offset = value.offset_to_next;
                    }
                }

                entry_data->offset = 0;
                goto end;
            }
        default:
            break;
        }
    }
 end:
    va_end(params);
    return MMDB_SUCCESS;
}

LOCAL void skip_hash_array(MMDB_s *mmdb, MMDB_entry_data_s *entry_data)
{
    if (entry_data->type == MMDB_DTYPE_MAP) {
        int size = entry_data->data_size;
        while (size-- > 0) {
            decode_one(mmdb, entry_data->offset_to_next, entry_data);   // key
            decode_one(mmdb, entry_data->offset_to_next, entry_data);   // value
            skip_hash_array(mmdb, entry_data);
        }

    } else if (entry_data->type == MMDB_DTYPE_ARRAY) {
        int size = entry_data->data_size;
        while (size-- > 0) {
            decode_one(mmdb, entry_data->offset_to_next, entry_data);   // value
            skip_hash_array(mmdb, entry_data);
        }
    }
}

LOCAL void decode_one_follow(MMDB_s *mmdb, uint32_t offset,
                             MMDB_entry_data_s *entry_data)
{
    decode_one(mmdb, offset, entry_data);
    if (entry_data->type == MMDB_DTYPE_PTR) {
        decode_one(mmdb, entry_data->pointer, entry_data);
    }
}

LOCAL void decode_one(MMDB_s *mmdb, uint32_t offset,
                      MMDB_entry_data_s *entry_data)
{
    const uint8_t *mem = mmdb->dataptr;
    uint8_t ctrl;
    int type;

    entry_data->offset = offset;
    ctrl = mem[offset++];
    type = (ctrl >> 5) & 7;
    if (type == MMDB_DTYPE_EXT) {
        type = get_ext_type(mem[offset++]);
    }

    entry_data->type = type;

    if (type == MMDB_DTYPE_PTR) {
        int psize = (ctrl >> 3) & 3;
        entry_data->pointer = get_ptr_from(ctrl, &mem[offset], psize);
        entry_data->data_size = psize + 1;
        entry_data->offset_to_next = offset + psize + 1;
        MMDB_DBG_CARP
            ("decode_one{ptr} ctrl:%d, offset:%d psize:%d point_to:%d\n", ctrl,
             offset, psize, entry_data->pointer);
        return;
    }

    int size = ctrl & 31;
    switch (size) {
    case 29:
        size = 29 + mem[offset++];
        break;
    case 30:
        size = 285 + get_uint16(&mem[offset]);
        offset += 2;
        break;
    case 31:
        size = 65821 + get_uint24(&mem[offset]);
        offset += 3;
    default:
        break;
    }

    if (type == MMDB_DTYPE_MAP || type == MMDB_DTYPE_ARRAY) {
        entry_data->data_size = size;
        entry_data->offset_to_next = offset;
        MMDB_DBG_CARP("decode_one type:%d size:%d\n", type, size);
        return;
    }

    if (type == MMDB_DTYPE_BOOLEAN) {
        entry_data->boolean = size ? true : false;
        entry_data->data_size = 0;
        entry_data->offset_to_next = offset;
        MMDB_DBG_CARP("decode_one type:%d size:%d\n", type, 0);
        return;
    }

    if (size == 0 && type != MMDB_DTYPE_UINT16 && type != MMDB_DTYPE_UINT32
        && type != MMDB_DTYPE_INT32) {
        entry_data->bytes = NULL;
        entry_data->utf8_string = NULL;
        entry_data->data_size = 0;
        entry_data->offset_to_next = offset;
        return;
    }

    if (type == MMDB_DTYPE_UINT16) {
        entry_data->uint16 = (uint16_t)get_uintX(&mem[offset], size);
    } else if (type == MMDB_DTYPE_UINT32) {
        entry_data->uint32 = (uint32_t)get_uintX(&mem[offset], size);
    } else if (type == MMDB_DTYPE_UINT64) {
        entry_data->uint64 = get_uintX(&mem[offset], size);
    } else if (type == MMDB_DTYPE_INT32) {
        entry_data->int32 = get_sintX(&mem[offset], size);
    } else if (type == MMDB_DTYPE_UINT128) {
        assert(size >= 0 && size <= 16);
        memset(entry_data->uint128, 0, 16);
        if (size > 0) {
            memcpy(entry_data->uint128 + 16 - size, &mem[offset], size);
        }
    } else if (type == MMDB_DTYPE_FLOAT) {
        size = 4;
        entry_data->float_value = get_ieee754_float(&mem[offset]);
    } else if (type == MMDB_DTYPE_DOUBLE) {
        size = 8;
        entry_data->double_value = get_ieee754_double(&mem[offset]);
    } else if (type == MMDB_DTYPE_UTF8_STRING) {
        entry_data->utf8_string = &mem[offset];
        entry_data->data_size = size;
    } else if (type == MMDB_DTYPE_BYTES) {
        entry_data->bytes = &mem[offset];
        entry_data->data_size = size;
    }
    entry_data->offset_to_next = offset + size;
    MMDB_DBG_CARP("decode_one type:%d size:%d\n", type, size);

    return;
}

LOCAL int get_ext_type(int raw_ext_type)
{
    return 7 + raw_ext_type;
}

LOCAL void DPRINT_KEY(MMDB_s *mmdb, MMDB_entry_data_s *entry_data)
{
    uint8_t str[256];
    int len = entry_data->data_size > 255 ? 255 : entry_data->data_size;

    memcpy(str, entry_data->utf8_string, len);

    str[len] = '\0';
    fprintf(stderr, "%s\n", str);
}

LOCAL uint32_t get_ptr_from(uint8_t ctrl, uint8_t const *const ptr,
                            int ptr_size)
{
    uint32_t new_offset;
    switch (ptr_size) {
    case 0:
        new_offset = (ctrl & 7) * 256 + ptr[0];
        break;
    case 1:
        new_offset = 2048 + (ctrl & 7) * 65536 + ptr[0] * 256 + ptr[1];
        break;
    case 2:
        new_offset = 2048 + 524288 + (ctrl & 7) * 16777216 + get_uint24(ptr);
        break;
    case 3:
    default:
        new_offset = get_uint32(ptr);
        break;
    }
    return MMDB_DATA_SECTION_SEPARATOR + new_offset;
}

void MMDB_get_entry_data_list(MMDB_entry_s *start,
                              MMDB_entry_data_list_s **entry_data_list)
{
    *entry_data_list = MMDB_alloc_entry_data_list();
    get_entry_data_list(start->mmdb, start->offset, *entry_data_list);
}

LOCAL void get_entry_data_list(MMDB_s *mmdb, uint32_t offset,
                               MMDB_entry_data_list_s *entry_data_list)
{
    decode_one(mmdb, offset, &entry_data_list->entry_data);

    switch (entry_data_list->entry_data.type) {
    case MMDB_DTYPE_PTR:
        {
            MMDB_DBG_CARP("Skip ptr\n");
            uint32_t next_offset = entry_data_list->entry_data.offset_to_next;
            uint32_t last_offset;
            while (entry_data_list->entry_data.type == MMDB_DTYPE_PTR) {
                decode_one(mmdb, last_offset =
                           entry_data_list->entry_data.pointer,
                           &entry_data_list->entry_data);
            }

            if (entry_data_list->entry_data.type == MMDB_DTYPE_ARRAY
                || entry_data_list->entry_data.type == MMDB_DTYPE_MAP) {
                get_entry_data_list(mmdb, last_offset, entry_data_list);
            }
            entry_data_list->entry_data.offset_to_next = next_offset;
        }
        break;
    case MMDB_DTYPE_ARRAY:
        {
            int array_size = entry_data_list->entry_data.data_size;
            MMDB_DBG_CARP("Decode array with %d entries\n", array_size);
            uint32_t array_offset = entry_data_list->entry_data.offset_to_next;
            MMDB_entry_data_list_s *previous = entry_data_list;
            while (array_size-- > 0) {
                MMDB_entry_data_list_s *entry_data_list_to = previous->next =
                    MMDB_alloc_entry_data_list();
                get_entry_data_list(mmdb, array_offset, entry_data_list_to);
                array_offset = entry_data_list_to->entry_data.offset_to_next;
                while (previous->next) {
                    previous = previous->next;
                }
            }
            entry_data_list->entry_data.offset_to_next = array_offset;

        }
        break;
    case MMDB_DTYPE_MAP:
        {
            int size = entry_data_list->entry_data.data_size;

#if MMDB_DEBUG
            int rnd = rand();
            MMDB_DBG_CARP("%u decode hash with %d keys\n", rnd, size);
#endif
            offset = entry_data_list->entry_data.offset_to_next;
            MMDB_entry_data_list_s *previous = entry_data_list;
            while (size-- > 0) {
                MMDB_entry_data_list_s *entry_data_list_to = previous->next =
                    MMDB_alloc_entry_data_list();
                get_entry_data_list(mmdb, offset, entry_data_list_to);
                while (previous->next) {
                    previous = previous->next;
                }
#if MMDB_DEBUG
                MMDB_DBG_CARP("key num: %d (%u)", size, rnd);
                DPRINT_KEY(mmdb, &entry_data_list_to->entry_data.data);
#endif

                offset = entry_data_list_to->entry_data.offset_to_next;
                entry_data_list_to = previous->next =
                    MMDB_alloc_entry_data_list();
                get_entry_data_list(mmdb, offset, entry_data_list_to);
                while (previous->next) {
                    previous = previous->next;
                }
                offset = entry_data_list_to->entry_data.offset_to_next;
            }
            entry_data_list->entry_data.offset_to_next = offset;
        }
        break;
    default:
        break;
    }
}

LOCAL void silly_pindent(int i)
{
    char buffer[1024];
    int size = i >= 1024 ? 1023 : i;
    memset(buffer, 32, size);
    buffer[size] = '\0';
    fputs(buffer, stderr);
}

LOCAL float get_ieee754_float(const uint8_t *restrict p)
{
    volatile float f;
    uint8_t *q = (void *)&f;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    q[3] = p[0];
    q[2] = p[1];
    q[1] = p[2];
    q[0] = p[3];
#else
    memcpy(q, p, 4);
#endif
    return f;
}

LOCAL double get_ieee754_double(const uint8_t *restrict p)
{
    volatile double d;
    uint8_t *q = (void *)&d;

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    q[7] = p[0];
    q[6] = p[1];
    q[5] = p[2];
    q[4] = p[3];
    q[3] = p[4];
    q[2] = p[5];
    q[1] = p[6];
    q[0] = p[7];
#else
    memcpy(q, p, 8);
#endif

    return d;
}

LOCAL uint32_t get_uint32(const uint8_t *p)
{
    return (p[0] * 16777216U + p[1] * 65536 + p[2] * 256 + p[3]);
}

LOCAL uint32_t get_uint24(const uint8_t *p)
{
    return (p[0] * 65536U + p[1] * 256 + p[2]);
}

LOCAL uint32_t get_uint16(const uint8_t *p)
{
    return (p[0] * 256U + p[1]);
}

LOCAL uint64_t get_uintX(const uint8_t *p, int length)
{
    uint64_t value = 0;
    while (length-- > 0) {
        value <<= 8;
        value += *p++;
    }
    return value;
}

LOCAL int32_t get_sintX(const uint8_t *p, int length)
{
    return (int32_t)get_uintX(p, length);
}

LOCAL int int_pread(int fd, uint8_t *buffer, ssize_t to_read, off_t offset)
{
    while (to_read > 0) {
        ssize_t have_read = pread(fd, buffer, to_read, offset);
        if (have_read <= 0) {
            return MMDB_IO_ERROR;
        }
        to_read -= have_read;
        if (to_read == 0) {
            break;
        }
        offset += have_read;
        buffer += have_read;
    }
    return MMDB_SUCCESS;
}

MMDB_entry_data_list_s *MMDB_alloc_entry_data_list(void)
{
    MMDB_entry_data_list_s *entry_data_list =
        calloc(1, sizeof(MMDB_entry_data_list_s));
    assert(entry_data_list != NULL);

    return entry_data_list;
}

void MMDB_free_entry_data_list(MMDB_entry_data_list_s *freeme)
{
    if (freeme == NULL) {
        return;
    }
    if (freeme->next) {
        MMDB_free_entry_data_list(freeme->next);
    }
    free(freeme);
}

const char *MMDB_lib_version(void)
{
    return PACKAGE_VERSION;
}

int MMDB_dump(MMDB_s *mmdb, MMDB_entry_data_list_s *entry_data_list, int indent)
{
    fprintf(stdout, "Dumping data structure\n");
    while (entry_data_list) {
        entry_data_list = dump(mmdb, entry_data_list, indent);
    }
    // not sure about the return type right now
    return MMDB_SUCCESS;
}

LOCAL MMDB_entry_data_list_s *dump(MMDB_s *mmdb,
                                   MMDB_entry_data_list_s *entry_data_list,
                                   int indent)
{
    char *string, *bytes;

    switch (entry_data_list->entry_data.type) {
    case MMDB_DTYPE_MAP:
        {
            int size = entry_data_list->entry_data.data_size;
            fprintf(stdout, "map with %d pairs\n", size);
            for (entry_data_list = entry_data_list->next;
                 size && entry_data_list; size--) {
                entry_data_list = dump(mmdb, entry_data_list, indent + 2);
                entry_data_list = dump(mmdb, entry_data_list, indent + 2);
            }
        }
        break;
    case MMDB_DTYPE_ARRAY:
        {
            int size = entry_data_list->entry_data.data_size;
            fprintf(stdout, "array with %d elements\n", size);
            for (entry_data_list = entry_data_list->next;
                 size && entry_data_list; size--) {
                entry_data_list = dump(mmdb, entry_data_list, indent + 2);
            }
        }
        break;
    case MMDB_DTYPE_UTF8_STRING:
        string =
            strndup((char *)entry_data_list->entry_data.utf8_string,
                    entry_data_list->entry_data.data_size);
        silly_pindent(indent);
        fprintf(stdout, "utf8_string = %s\n", string);
        free(string);
        entry_data_list = entry_data_list->next;
        break;
    case MMDB_DTYPE_BYTES:
        bytes =
            strndup((char *)entry_data_list->entry_data.bytes,
                    entry_data_list->entry_data.data_size);
        silly_pindent(indent);
        fprintf(stdout, "bytes = %s\n", bytes);
        free(bytes);
        entry_data_list = entry_data_list->next;
        break;
    case MMDB_DTYPE_DOUBLE:
        silly_pindent(indent);
        fprintf(stdout, "double = %f\n",
                entry_data_list->entry_data.double_value);
        entry_data_list = entry_data_list->next;
        break;
    case MMDB_DTYPE_FLOAT:
        silly_pindent(indent);
        fprintf(stdout, "float = %f\n",
                entry_data_list->entry_data.float_value);
        entry_data_list = entry_data_list->next;
        break;
    case MMDB_DTYPE_UINT16:
        silly_pindent(indent);
        fprintf(stdout, "uint16 = %u\n", entry_data_list->entry_data.uint16);
        entry_data_list = entry_data_list->next;
        break;
    case MMDB_DTYPE_UINT32:
        silly_pindent(indent);
        fprintf(stdout, "uint32 = %u\n",
                (uint32_t)entry_data_list->entry_data.uint32);
        entry_data_list = entry_data_list->next;
        break;
    case MMDB_DTYPE_BOOLEAN:
        silly_pindent(indent);
        fprintf(stdout, "boolean = %u\n",
                (uint32_t)entry_data_list->entry_data.boolean);
        entry_data_list = entry_data_list->next;
        break;
    case MMDB_DTYPE_UINT64:
        silly_pindent(indent);
        fprintf(stdout, "uint64 = XXX\n");
        entry_data_list = entry_data_list->next;
        break;
    case MMDB_DTYPE_UINT128:
        silly_pindent(indent);
        fprintf(stdout, "uint128 = XXX\n");
        entry_data_list = entry_data_list->next;
        break;
    case MMDB_DTYPE_INT32:
        silly_pindent(indent);
        fprintf(stdout, "int32 = %d\n", entry_data_list->entry_data.int32);
        entry_data_list = entry_data_list->next;
        break;
    default:
        MMDB_DBG_CARP("unknown type! %d\n", entry_data_list->entry_data.type);
        assert(0);
    }
    return entry_data_list;
}

const char *MMDB_strerror(uint16_t error_code)
{
    if (MMDB_SUCCESS == error_code) {
        return "Success (not an error)";
    } else if (MMDB_FILE_OPEN_ERROR == error_code) {
        return "Error opening the specified MaxMind DB file";
    } else if (MMDB_CORRUPT_DATABASE == error_code) {
        return "The MaxMind DB file's search tree is corrupt";
    } else if (MMDB_INVALID_DATABASE == error_code) {
        return "The MaxMind DB file is invalid (bad metadata)";
    } else if (MMDB_IO_ERROR == error_code) {
        return "An attempt to read data from the MaxMind DB file failed";
    } else if (MMDB_OUT_OF_MEMORY == error_code) {
        return "A memory allocation call failed";
    } else if (MMDB_UNKNOWN_DATABASE_FORMAT == error_code) {
        return
            "The MaxMind DB file is in a format this library can't handle (unknown record size or binary format version)";
    }
}

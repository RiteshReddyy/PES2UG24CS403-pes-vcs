#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <inttypes.h>

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("    staged: %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("    (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("    deleted: %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if ((uint64_t)st.st_mtime != index->entries[i].mtime_sec ||
                (uint32_t)st.st_size  != index->entries[i].size) {
                printf("    modified: %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("    (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".")    == 0) continue;
            if (strcmp(ent->d_name, "..")   == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes")  == 0) continue;
            if (strstr(ent->d_name, ".o")   != NULL) continue;
            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1;
                    break;
                }
            }
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) {
                    printf("    untracked: %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("    (nothing to show)\n");
    printf("\n");
    return 0;
}

static int cmp_index_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path,
                  ((const IndexEntry *)b)->path);
}

int index_load(Index *index) {
    index->count = 0;
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0;
    char hex[HASH_HEX_SIZE + 1];
    while (index->count < MAX_INDEX_ENTRIES) {
        IndexEntry *e = &index->entries[index->count];
        int ret = fscanf(f, "%o %64s %" SCNu64 " %u %511s\n",
                         &e->mode, hex,
                         &e->mtime_sec, &e->size,
                         e->path);
        if (ret == EOF || ret < 5) break;
        if (hex_to_hash(hex, &e->hash) != 0) break;
        index->count++;
    }
    fclose(f);
    return 0;
}

int index_save(const Index *index) {
    Index *sorted = malloc(sizeof(Index));
    if (!sorted) return -1;
    *sorted = *index;
    qsort(sorted->entries, sorted->count, sizeof(IndexEntry), cmp_index_entries);

    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", INDEX_FILE);
    FILE *f = fopen(tmp_path, "w");
    if (!f) { free(sorted); return -1; }

    char hex[HASH_HEX_SIZE + 1];
    for (int i = 0; i < sorted->count; i++) {
        IndexEntry *e = &sorted->entries[i];
        hash_to_hex(&e->hash, hex);
        fprintf(f, "%o %s %" PRIu64 " %u %s\n",
                e->mode, hex, e->mtime_sec, e->size, e->path);
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    free(sorted);
    return rename(tmp_path, INDEX_FILE);
}

int index_add(Index *index, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *contents = malloc(file_size > 0 ? (size_t)file_size : 1);
    if (!contents) { fclose(f); return -1; }
    if (fread(contents, 1, (size_t)file_size, f) != (size_t)file_size && file_size > 0) {
        free(contents); fclose(f); return -1;
    }
    fclose(f);

    ObjectID blob_id;
    if (object_write(OBJ_BLOB, contents, (size_t)file_size, &blob_id) != 0) {
        free(contents); return -1;
    }
    free(contents);

    struct stat st;
    if (lstat(path, &st) != 0) return -1;

    IndexEntry *e = index_find(index, path);
    if (!e) {
        if (index->count >= MAX_INDEX_ENTRIES) {
            fprintf(stderr, "error: index is full\n");
            return -1;
        }
        e = &index->entries[index->count++];
    }

    strncpy(e->path, path, sizeof(e->path) - 1);
    e->path[sizeof(e->path) - 1] = '\0';
    memcpy(e->hash.hash, blob_id.hash, HASH_SIZE);
    e->mode      = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;
    e->mtime_sec = (uint64_t)st.st_mtime;
    e->size      = (uint32_t)st.st_size;

    return index_save(index);
}

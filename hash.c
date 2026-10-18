#include "hash.h"

#include <stdlib.h>
#include <string.h>

#include "vfs.h"

/* FNV-1a hash function for strings */
static size_t hash_fnv1a(const char* str) {
        size_t hash = 2166136261U; /* FNV offset basis for 32-bit */
        unsigned char c;

        while ((c = (unsigned char)*str++) != '\0') {
                hash ^= c;
                hash *= 16777619; /* FNV prime */
        }

        return hash;
}

static int hash_table_grow(struct hash_table* ht) {
        size_t new_capacity;
        size_t i;
        size_t index;
        struct hash_entry** buckets;
        struct hash_entry* entry;
        struct hash_entry* next;

        if (ht->capacity > (size_t)-1 / 2) return -1;

        new_capacity = ht->capacity * 2;

        if (new_capacity > (size_t)-1 / sizeof(*buckets)) return -1;

        buckets = (struct hash_entry**)calloc(new_capacity, sizeof(*buckets));
        if (buckets == NULL) return -1;

        for (i = 0; i < ht->capacity; i++) {
                entry = ht->buckets[i];

                while (entry != NULL) {
                        next        = entry->next;
                        index       = hash_fnv1a(entry->vfs_entry->url_path) %
                                      new_capacity;
                        entry->next = buckets[index];
                        buckets[index] = entry;
                        entry          = next;
                }
        }

        free(ht->buckets);
        ht->buckets  = buckets;
        ht->capacity = new_capacity;

        return 0;
}

struct hash_table* hash_table_create(size_t capacity) {
        struct hash_table* ht;

        if (capacity == 0 || capacity > (size_t)-1 / sizeof(*ht->buckets))
                return NULL;

        ht = (struct hash_table*)malloc(sizeof(*ht));
        if (ht == NULL) return NULL;

        ht->buckets =
            (struct hash_entry**)calloc(capacity, sizeof(*ht->buckets));
        if (ht->buckets == NULL) {
                free(ht);
                return NULL;
        }

        ht->capacity = capacity;
        ht->size     = 0;

        return ht;
}

void hash_table_destroy(struct hash_table* ht) {
        size_t i;
        struct hash_entry* entry;
        struct hash_entry* next;

        if (ht == NULL) return;

        for (i = 0; i < ht->capacity; i++) {
                entry = ht->buckets[i];
                while (entry != NULL) {
                        next = entry->next;
                        free(entry);
                        entry = next;
                }
        }

        free(ht->buckets);
        free(ht);
}

int hash_table_insert(struct hash_table* ht, const char* key,
                      struct vfs_entry* value) {
        size_t index;
        struct hash_entry* entry;
        struct hash_entry* new_entry;

        if (ht == NULL || key == NULL || value == NULL) return -1;

        index = hash_fnv1a(key) % ht->capacity;
        entry = ht->buckets[index];

        /* Check if key already exists and update it */
        while (entry != NULL) {
                if (strcmp(entry->vfs_entry->url_path, key) == 0) {
                        entry->vfs_entry = value;
                        return 0;
                }
                entry = entry->next;
        }

        if (ht->size >= ht->capacity - ht->capacity / 4) {
                if (hash_table_grow(ht) != 0) return -1;

                index = hash_fnv1a(key) % ht->capacity;
        }

        /* Insert new entry at the head of the collision chain */
        new_entry = (struct hash_entry*)malloc(sizeof(*new_entry));
        if (new_entry == NULL) return -1;

        new_entry->vfs_entry = value;
        new_entry->next      = ht->buckets[index];
        ht->buckets[index]   = new_entry;
        ht->size++;

        return 0;
}

struct vfs_entry* hash_table_lookup(const struct hash_table* ht,
                                    const char* key) {
        size_t index;
        struct hash_entry* entry;

        if (ht == NULL || key == NULL) return NULL;

        index = hash_fnv1a(key) % ht->capacity;
        entry = ht->buckets[index];

        while (entry != NULL) {
                if (strcmp(entry->vfs_entry->url_path, key) == 0)
                        return entry->vfs_entry;
                entry = entry->next;
        }

        return NULL;
}

int hash_table_remove(struct hash_table* ht, const char* key) {
        size_t index;
        struct hash_entry* entry;
        struct hash_entry** p;

        if (ht == NULL || key == NULL) return -1;

        index = hash_fnv1a(key) % ht->capacity;
        p     = &ht->buckets[index];

        while (*p != NULL) {
                entry = *p;
                if (strcmp(entry->vfs_entry->url_path, key) == 0) {
                        *p = entry->next;
                        free(entry);
                        ht->size--;
                        return 0;
                }
                p = &entry->next;
        }

        return -1;
}

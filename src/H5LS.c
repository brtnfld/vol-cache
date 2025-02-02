/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright (c) 2023, UChicago Argonne, LLC.                                *
 * All Rights Reserved.                                                      *
 *                                                                           *
 * This file is part of HDF5 Cache VOL connector.  The full copyright notice *
 * terms governing use, modification, and redistribution, is contained in    *
 * the LICENSE file, which can be found at the root of the source code       *
 * distribution tree.                                                        *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
   This source file contains a set of function for management the node-local
   storage.
 */

#include "H5LS.h"
// Standard I/O
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Memory map
#include <assert.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>
/*
 Different Node Local Storage setup
*/

#include "H5LS_RAM.h"
#include "H5LS_SSD.h"
#ifdef USE_GPU
#include "H5LS_GPU.h"
#endif
/* ------------------*/

#include "debug.h"
/*
   Macro
 */
#ifndef FAIL
#define FAIL -1
#endif
#ifndef SUCCEED
#define SUCCEED 0
#endif
#ifndef STDERR
#ifdef __APPLE__
#define STDERR __stderrp
#else
#define STDERR stderr
#endif
#endif
extern int RANK;
extern int NPROC;

#define MAX_TRUNC_MSG_LEN 128
#define ERROR_MSG_SIZE 283
char error_msg[ERROR_MSG_SIZE];
char truncated_msg[MAX_TRUNC_MSG_LEN];

/*
   Get the corresponding mmap function struct based on the type of node local
   storage The user can modify this function to other storage
 */
const H5LS_mmap_class_t *get_H5LS_mmap_class_t(char *type) {
  const H5LS_mmap_class_t *p =
      (H5LS_mmap_class_t *)malloc(sizeof(H5LS_mmap_class_t));
  if (!strcmp(type, "SSD") || !strcmp(type, "BURST_BUFFER")) {
    p = &H5LS_SSD_mmap_ext_g;
  } else if (!strcmp(type, "MEMORY")) {
    p = &H5LS_RAM_mmap_ext_g;
#ifdef USE_GPU
  } else if (!strcmp(type, "GPU")) {
    p = &H5LS_GPU_mmap_ext_g;
#endif
  } else {

    size_t copy_len = strlcpy(truncated_msg, type, sizeof(truncated_msg));
    if (copy_len >= MAX_TRUNC_MSG_LEN) {
      LOG_WARN(-1, "Storage type string truncated");
    }
    int ret = snprintf(error_msg, ERROR_MSG_SIZE,
                       "I don't know the type of storage: %s\n"
                       "Supported options: SSD|BURST_BUFFER|MEMORY|GPU\n",
                       truncated_msg);
    if (ret < 0 || ret >= ERROR_MSG_SIZE) {
      LOG_WARN(-1, "Storage type string truncated");
    }
    LOG_ERROR(-1, "%s", error_msg);
    MPI_Abort(MPI_COMM_WORLD, 111);
  }
  return p;
}

/*
  This is to convert replacement policy from string to enum
 */
cache_replacement_policy_t get_replacement_policy_from_str(char *str) {
  if (!strcmp(str, "LRU"))
    return LRU;
  else if (!strcmp(str, "LFU"))
    return LFU;
  else if (!strcmp(str, "FIFO"))
    return FIFO;
  else if (!strcmp(str, "LIFO"))
    return LIFO;
  else {
    if (strlen(str) < 200) {
      snprintf(error_msg, ERROR_MSG_SIZE,
               "unknown cache replacement type: %s\n", str);
    } else {
      snprintf(error_msg, ERROR_MSG_SIZE,
               "unknown cache replacement type: string too long to display\n");
    }
    LOG_ERROR(-1, "%s", error_msg);
    return FAIL;
  }
}

/*---------------------------------------------------------------------------
 * Function:    readLSConf
 *
 * Purpose:     read storage configuration from a config file
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *---------------------------------------------------------------------------
 */
herr_t readLSConf(char *fname, cache_storage_t *LS) {
  int called;
  MPI_Initialized(&called);
  if (called == 1) {
    MPI_Comm_size(MPI_COMM_WORLD, &NPROC);
    MPI_Comm_rank(MPI_COMM_WORLD, &RANK);
  } else {
    int provided = 0;
    MPI_Init_thread(NULL, NULL, MPI_THREAD_MULTIPLE, &provided);
    MPI_Comm_size(MPI_COMM_WORLD, &NPROC);
    MPI_Comm_rank(MPI_COMM_WORLD, &RANK);
  }
  char line[256];
  int linenum = 0;
  if (access(fname, F_OK) != 0) {
    LOG_ERROR(-1, "cache configure file %s does not exist.\n", fname);
    MPI_Abort(MPI_COMM_WORLD, 100);
  }
  FILE *file = fopen(fname, "r");
  LS->path = (char *)malloc(256);
  strncpy(LS->path, "./", 255);
  LS->path[255] = '\0';
  LS->mspace_total = 137438953472;
  strcpy(LS->type, "SSD");
  strcpy(LS->scope, "LOCAL");
  LS->fusion_threshold = 0; // By default no merging the dataset at all.
  LS->replacement_policy = LRU;
  LS->write_buffer_size = 2147483648; // default size 2GB
  while (fgets(line, 256, file) != NULL) {
    char ip[256], mac[256];
    linenum++;
    if (line[0] == '#')
      continue;
    if (sscanf(line, "%255[^:]:%255s", ip, mac) != 2) {
      if (RANK == io_node())
        fprintf(stderr, "Syntax error, line %d\n", linenum);
      continue;
    }
    ip[255] = '\0';
    mac[255] = '\0';
    if (strlen(ip) >= 256 || strlen(mac) >= 256) {
      if (RANK == io_node())
        fprintf(stderr, "Input too long, line %d\n", linenum);
      continue;
    }
    if (!strcmp(ip, "HDF5_CACHE_STORAGE_PATH"))
      if (strcmp(mac, "NULL") == 0)
        LS->path = NULL;
      else {
        snprintf(LS->path, 256, "%s", mac);
      }

    else if (!strcmp(ip, "HDF5_CACHE_FUSION_THRESHOLD")) {
      LS->fusion_threshold = atof(mac);
#ifndef NDEBUG
      LOG_INFO(-1, "Merging small dataset requests is turned on\n");
#endif
    } else if (!strcmp(ip, "HDF5_CACHE_STORAGE_SIZE"))
      LS->mspace_total = (hsize_t)atof(mac);
    else if (!strcmp(ip, "HDF5_CACHE_WRITE_BUFFER_SIZE"))
      LS->write_buffer_size = (hsize_t)atof(mac);
    else if (!strcmp(ip, "HDF5_CACHE_STORAGE_TYPE")) {
      strncpy(LS->type, mac, sizeof(LS->type) - 1);
      LS->type[sizeof(LS->type) - 1] = '\0';
    } else if (!strcmp(ip, "HDF5_CACHE_STORAGE_SCOPE")) {
      strncpy(LS->scope, mac, sizeof(LS->scope) - 1);
      LS->scope[sizeof(LS->scope) - 1] = '\0';
    } else if (!strcmp(ip, "HDF5_CACHE_REPLACEMENT_POLICY")) {
      if (get_replacement_policy_from_str(mac) > 0)
        LS->replacement_policy = get_replacement_policy_from_str(mac);
    } else {
      snprintf(error_msg, ERROR_MSG_SIZE, "Unknown configuration setup: %s",
               ip);
      LOG_WARN(-1, "%s", error_msg);
    }
  }
  if (LS->mspace_total < LS->write_buffer_size) {
    LOG_ERROR(-1, "the write buffer size is larger than the total "
                  "storage space. \n"
                  "         Try to decrease the value of "
                  "HDF5_CACHE_WRITE_BUFFER_SIZE\n");
    MPI_Abort(MPI_COMM_WORLD, 112);
  }
  fclose(file);
  LS->mspace_left = LS->mspace_total;
  struct stat sb;
  if (strcmp(LS->type, "GPU") == 0 || strcmp(LS->type, "MEMORY") == 0 ||
      (stat(LS->path, &sb) == 0 && S_ISDIR(sb.st_mode))) {
    return 0;
  } else {
    int ret = snprintf(error_msg, ERROR_MSG_SIZE,
                       "H5LSset: path %s does not exist\n", LS->path);
    if (ret < 0 || ret >= ERROR_MSG_SIZE) {
      LOG_WARN(-1, "path error message truncated");
    }
    LOG_ERROR(-1, "%s", error_msg);
    MPI_Abort(MPI_COMM_WORLD, 112);
  }
}

/*-------------------------------------------------------------------------
 * Function:    H5Pset_fapl_cache
 *
 * Purpose:     Set whether to turn on HDF5_CACHE_RD/WR file access property
 *list
 *
 * Return:      SUCCEED / FAIL
 *
 *-------------------------------------------------------------------------
 */
herr_t H5Pset_fapl_cache(hid_t plist, char *flag, void *value) {
  herr_t ret;
  size_t s = 1;
  if (strcmp(flag, "HDF5_CACHE_WR") == 0 || strcmp(flag, "HDF5_CACHE_RD") == 0)
    s = sizeof(bool);
  if (strcmp(flag, "HDF5_CACHE_WR") == 0 ||
      strcmp(flag, "HDF5_CACHE_RD") == 0) {
    if (H5Pexist(plist, flag) == 0)
      ret =
          H5Pinsert2(plist, flag, s, value, NULL, NULL, NULL, NULL, NULL, NULL);
    else
      ret = H5Pset(plist, flag, value);
  } else {
    LOG_ERROR(-1,
              "property list does not have "
              "property: %s",
              flag);
    ret = FAIL;
  }
  return ret;
} /* end H5Pset_fapl_cache() */

/*-------------------------------------------------------------------------
 * Function:    H5Pget_fapl_cache
 *
 * Purpose:     Get local storage related property to file access property list
 *
 * Return:      SUCCEED / FAIL
 *
 *-------------------------------------------------------------------------
 */
herr_t H5Pget_fapl_cache(hid_t plist, char *flag, void *value) {
  herr_t ret;
  if (H5Pexist(plist, flag) > 0)
    ret = H5Pget(plist, flag, value);
  else {
    ret = FAIL;
  }
  return ret;
} /* end H5Pget_fapl_cache() */

/*-------------------------------------------------------------------------
 * Function:    H5LSset
 *
 * Purpose:     set the global local storage property
 *
 * Input:
 *           LS - the local storage struct
 *         type - the type of storage [SSD, BURST_BUFFER, MEMORY, GPU]
 *         path - the path to the local storage
 * mspace_total - the capacity of the local storage in Bytes.
 *
 * Return:      SUCCEED / FAIL
 *
 *-------------------------------------------------------------------------
 */
herr_t H5LSset(cache_storage_t *LS, char *type, char *path,
               hsize_t mspace_total, cache_replacement_policy_t replacement) {
#ifdef NDEBUG
  LOG_INFO(-1, "H5LSset");
#endif
  strcpy(LS->type, type);
  LS->mspace_total = mspace_total;
  LS->mspace_left = mspace_total;
  LS->num_cache = 0;
  LS->replacement_policy = replacement;
  if (path != NULL)
    strcpy(LS->path, path); // check existence of the space
  struct stat sb;
  if (strcmp(type, "GPU") == 0 || strcmp(type, "MEMORY") == 0 ||
      (stat(path, &sb) == 0 && S_ISDIR(sb.st_mode))) {
    return 0;
  } else {
    LOG_ERROR(-1,
              "ERROR in name space for cache storage: %s does "
              "not exist\n",
              path);
    MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
  }
} /* end H5LSset */

/*-------------------------------------------------------------------------
 * Function:    H5LSget
 *
 * Purpose:     get the global local storage property
 *
 * Return:      SUCCEED / FAIL
 *
 *-------------------------------------------------------------------------
 */
herr_t H5LSget(cache_storage_t *LS, char *flag, void *value) {
#ifdef NDEBUG
  LOG_INFO(-1, "H5LSget");
#endif

  if (strcmp(flag, "TYPE") == 0)
    value = &LS->type;
  else if (strcmp(flag, "PATH") == 0)
    value = &LS->path;
  else if (strcmp(flag, "SIZE") == 0)
    value = &LS->mspace_total;
  else {
    return FAIL;
  }
  return SUCCEED;
} /* end H5LSget() */

/*-------------------------------------------------------------------------
 * Function:    H5LScompare_cache
 *
 * Purpose:     Compare the two cache
 *
 * Return:      true (a > b), or false (b > a)
 *
 *-------------------------------------------------------------------------
 */
bool H5LScompare_cache(cache_t *a, cache_t *b,
                       cache_replacement_policy_t replacement_policy) {
#ifdef NDEBUG
  LOG_INFO(-1, "H5LScompare_cache");
#endif
  /// if true, a should be selected, otherwise b.
  bool agb = false;
  double fa, fb;
  switch (replacement_policy) {
  case (LRU):
    agb = (a->access_history.time_stamp[a->access_history.count] <
           b->access_history.time_stamp[b->access_history.count]);
    break;
  case (FIFO):
    agb = (a->access_history.time_stamp[0] < b->access_history.time_stamp[0]);
    break;
  case (LFU):
    fa = (a->access_history.time_stamp[a->access_history.count] -
          a->access_history.time_stamp[0]) /
         a->access_history.count;
    fb = (b->access_history.time_stamp[b->access_history.count] -
          b->access_history.time_stamp[0]) /
         b->access_history.count;
    agb = (fa < fb);
    break;
  default:
    LOG_WARN(-1,
             "Unknown cache replacement policy %d; use LRU (least "
             "recently used)\n",
             replacement_policy);
    agb = (a->access_history.time_stamp[a->access_history.count] <
           b->access_history.time_stamp[b->access_history.count]);
    break;
  }
  return agb;
} /* end H5LScompare_cache() */

/*-------------------------------------------------------------------------
 *  Function: H5LSclaim_space
 *  Purpose: trying to claim a portionof space for a cache.
 *  Input:
 *         LS - the local storage struct
 *       size - the size of the space in bytes
 *       type - claim type [HARD / SOFT]
 *  Return:  0 / -1
 *-------------------------------------------------------------------------
 */
herr_t H5LSclaim_space(cache_storage_t *LS, hsize_t size, cache_claim_t type,
                       cache_replacement_policy_t crp) {
#ifndef NDEBUG
  LOG_INFO(-1, "H5LSclaim_space");
#endif
  if (LS->mspace_total < size) {
#ifndef NDEBUG
    LOG_WARN(-1, "cache (%ld) is larger than the total size %ld", size,
             LS->mspace_total);
#endif
    return FAIL;
  }
  if (LS->mspace_left > size) {
    LS->mspace_left = LS->mspace_left - size;
#ifndef NDEBUG
    snprintf(error_msg, ERROR_MSG_SIZE, "Claimed: %.4f GiB\n",
             size / 1024. / 1024. / 1024.);
    LOG_DEBUG(-1, "%s", error_msg);
    snprintf(error_msg, ERROR_MSG_SIZE, "LS->space left: %.4f GiB\n",
             LS->mspace_left / 1024. / 1024 / 1024.);
    LOG_DEBUG(-1, "%s", error_msg);
#endif
    return SUCCEED;
  } else {
    if (type == SOFT) {
      return FAIL;
    } else {
      double mspace = 0.0;
      /// compute the total space for all the temporal cache;
      CacheList *head = LS->cache_head;
      cache_t *tmp, *stay;
      while (head != NULL) {
        if (head->cache->duration == TEMPORAL) {
          mspace += head->cache->mspace_total;
          tmp = head->cache;
        }
        head = head->next;
      }
      stay = tmp;
      if (mspace < size) {
#ifndef NDEBUG
        snprintf(error_msg, ERROR_MSG_SIZE, "mspace (bytes): %f - %lu\n",
                 mspace, size);
        LOG_DEBUG(-1, "%s", error_msg);
#endif
        return FAIL;
      } else {
        mspace = 0.0;
        while (mspace < size) {
          head = LS->cache_list;
          while (head != NULL) {
            if ((head->cache->duration == TEMPORAL) &&
                H5LScompare_cache(head->cache, tmp, crp)) {
              tmp = head->cache;
              stay = tmp;
            }
            if (mspace > 0)
              continue; // if already found one, as long as we find another one
                        // that is
            head = head->next;
          }
          mspace += tmp->mspace_total;
          H5LSremove_cache(LS, tmp);
          tmp = stay;
        }
      }
    }
  }

  return 0;
}

/*-------------------------------------------------------------------------
 *  Function: H5LSremove_cache
 *  Purpose: Clear certain cache, remove all the files associated with it.
 *-------------------------------------------------------------------------
 */
herr_t H5LSremove_cache(cache_storage_t *LS, cache_t *cache) {
#ifndef NDEBUG
  LOG_INFO(-1, "H5LSremove_space");
#endif
  if (cache != NULL) {
    if (LS->io_node && strcmp(LS->scope, "GLOBAL"))
      LS->mmap_cls->removeCacheFolder(cache->path);

    CacheList *head = LS->cache_head;
    while (head != NULL && head->cache != NULL && head->cache != cache) {
      head = head->next;
    }
    if (head != NULL && head->cache != NULL && head->cache == cache) {
      LS->mspace_left += cache->mspace_total;
#ifndef NDEBUG
      LOG_DEBUG(-1, "Cache storage space left: %lu bytes\n", LS->mspace_left);
#endif

      free(cache);
      cache = NULL;
    }
    if (head != NULL)
      head = head->next;
  } else {
    if (LS->io_node)
      LOG_ERROR(-1, "Trying to remove nonexisting cache\n");
    return FAIL;
  }
#ifndef NDEBUG
  LOG_INFO(-1, "H5LSremove_space DONE");
#endif
  return 0;
} /* end H5LSremove_cache() */

/*-------------------------------------------------------------------------
 *  Function: H5LSremove_cache_all
 *  Purpose: Clear all cache, remove all the files associated with it.
 *-------------------------------------------------------------------------
 */
herr_t H5LSremove_cache_all(cache_storage_t *LS) {
#ifndef NDEBUG
  LOG_INFO(-1, "H5LSremove_space_all\n");
#endif
  CacheList *head = LS->cache_head;
  herr_t ret_value;
  while (head != NULL) {
    if (LS->io_node) {
      ret_value = LS->mmap_cls->removeCacheFolder(head->cache->path);
      free(head->cache);
      head->cache = NULL;
      head = head->next;
    }
  }
  return ret_value;
} /* end H5LSremove_cache_all() */

/*-------------------------------------------------------------------------
 *  Function: H5LSregister_cache
 *  Purpose:  register the cache to the local storage
 *-------------------------------------------------------------------------
 */
herr_t H5LSregister_cache(cache_storage_t *LS, cache_t *cache, void *target) {
#ifndef NDEBUG
  LOG_INFO(-1, "Entering H5LSregister_cache\n");
#endif
  LS->cache_list = (CacheList *)malloc(sizeof(CacheList));
  LS->cache_list->cache = cache;
  LS->cache_list->target = target;
  cache->access_history.time_stamp[0] = time(NULL);
  cache->access_history.count = 0;
  return SUCCEED;
} /* end H5LSregister_cache() */

/*-------------------------------------------------------------------------
 *  Function: H5LSrecord_cache_access
 *
 *  Purpose:  Record the access event for the cache
 *
 *-------------------------------------------------------------------------
 */
herr_t H5LSrecord_cache_access(cache_t *cache) {
#ifndef NDEBUG
  LOG_INFO(-1, "Entering H5LSrecore_cache_acess\n");
#endif
  cache->access_history.count++;
  if (cache->access_history.count < MAX_NUM_CACHE_ACCESS) {
    cache->access_history.time_stamp[cache->access_history.count] = time(NULL);
  } else {
    // if overflow, we only record the most recent one at the end
    cache->access_history
        .time_stamp[cache->access_history.count % MAX_NUM_CACHE_ACCESS] =
        time(NULL);
  }
  return SUCCEED;
} /* end H5LSrecord_cache_access() */

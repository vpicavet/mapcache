/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: Berkeley DB cache backend
 * Author:   Thomas Bonfort and the MapServer team.
 *
 ******************************************************************************
 * Copyright (c) 1996-2011 Regents of the University of Minnesota.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies of this Software or works derived from this Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *****************************************************************************/

#ifdef USE_BDB

#include "mapcache.h"
#include <apr_strings.h>
#include <apr_reslist.h>
#include <apr_file_info.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#include <db.h>

#define PAGESIZE 64*1024

struct bdb_env {
   DB* db;
   DB_ENV *env;
};

static apr_status_t _bdb_reslist_get_connection(void **conn_, void *params, apr_pool_t *pool) {
   int ret;
   mapcache_cache_bdb *cache = (mapcache_cache_bdb*)params;
   char *dbfile = apr_pstrcat(pool,cache->basedir,"/tiles.db",NULL);
   struct bdb_env *benv = apr_pcalloc(pool,sizeof(struct bdb_env));

   ret = db_env_create(&benv->env, 0);
	if(ret) { 
      cache->ctx->set_error(cache->ctx,500,"bdb cache failure for db_env_create: %s", db_strerror(ret));
		return APR_EGENERAL;
	}
   int env_flags = DB_INIT_CDB|DB_INIT_MPOOL|DB_CREATE;
   ret = benv->env->open(benv->env,cache->basedir,env_flags,0);
	if(ret) { 
      cache->ctx->set_error(cache->ctx,500,"bdb cache failure for env->open: %s", db_strerror(ret));
		return APR_EGENERAL;
	}

	if ((ret = db_create(&benv->db, benv->env, 0)) != 0) {
		cache->ctx->set_error(cache->ctx,500,"bdb cache failure for db_create: %s", db_strerror(ret));
		return APR_EGENERAL;
	}
   int mode = DB_BTREE;
   ret = benv->db->set_pagesize(benv->db,PAGESIZE); /* set pagesize to maximum allowed, as tile data is usually pretty large */
	if(ret) { 
      cache->ctx->set_error(cache->ctx,500,"bdb cache failure for db->set_pagesize: %s", db_strerror(ret));
		return APR_EGENERAL;
	}

   if ((ret = benv->db->open(benv->db, NULL, dbfile, NULL, mode, DB_CREATE, 0664)) != 0) {
	   cache->ctx->set_error(cache->ctx,500,"bdb cache failure 1 for db->open: %s", db_strerror(ret));
	   return APR_EGENERAL;
	}
   *conn_ = benv;
   return APR_SUCCESS;
}

static apr_status_t _bdb_reslist_free_connection(void *conn_, void *params, apr_pool_t *pool) {
   struct bdb_env *benv = (struct bdb_env*)conn_;
   benv->db->close(benv->db,0);
   benv->env->close(benv->env,0);
   return APR_SUCCESS; 
}



static struct bdb_env* _bdb_get_conn(mapcache_context *ctx, mapcache_tile* tile, int readonly) {
   apr_status_t rv;
   mapcache_cache_bdb *cache = (mapcache_cache_bdb*)tile->tileset->cache;
   struct bdb_env *benv;
   rv = apr_reslist_acquire(cache->connection_pool, (void **)&benv);
   if(rv != APR_SUCCESS) {
      ctx->set_error(ctx,500,"failed to aquire connection to bdb backend: %s", cache->ctx->get_error_message(cache->ctx));
      cache->ctx->clear_errors(cache->ctx);
      return NULL;
   }
   return benv;
}

static void _bdb_release_conn(mapcache_context *ctx, mapcache_tile *tile, struct bdb_env *benv) {
   mapcache_cache_bdb* cache = (mapcache_cache_bdb*)tile->tileset->cache;
   if(GC_HAS_ERROR(ctx)) {
      apr_reslist_invalidate(cache->connection_pool,(void*)benv);  
   } else {
      apr_reslist_release(cache->connection_pool, (void*)benv);
   }
}

static int _mapcache_cache_bdb_has_tile(mapcache_context *ctx, mapcache_tile *tile) {
   int ret;
   DBT key;
   mapcache_cache_bdb *cache = (mapcache_cache_bdb*)tile->tileset->cache;
   char *skey = mapcache_util_get_tile_key(ctx,tile,cache->key_template,NULL,NULL);
   struct bdb_env *benv = _bdb_get_conn(ctx,tile,1);
   if(GC_HAS_ERROR(ctx)) return MAPCACHE_FALSE;
   memset(&key, 0, sizeof(DBT));
   key.data = skey;
   key.size = strlen(skey)+1;

   ret = benv->db->exists(benv->db, NULL, &key, 0);

   if(ret == 0) {
      ret = MAPCACHE_TRUE;
   } else if(ret == DB_NOTFOUND) {
      ret = MAPCACHE_FALSE;
   } else {
      ctx->set_error(ctx,500,"bdb backend failure on tile_exists: %s",db_strerror(ret));
      ret= MAPCACHE_FALSE;
   }
   _bdb_release_conn(ctx,tile,benv);
   return ret;
}

static void _mapcache_cache_bdb_delete(mapcache_context *ctx, mapcache_tile *tile) {
   DBT key;
   int ret;
   mapcache_cache_bdb *cache = (mapcache_cache_bdb*)tile->tileset->cache;
   char *skey = mapcache_util_get_tile_key(ctx,tile,cache->key_template,NULL,NULL);
   struct bdb_env *benv = _bdb_get_conn(ctx,tile,0);
   GC_CHECK_ERROR(ctx);
   memset(&key, 0, sizeof(DBT));
   key.data = skey;
   key.size = strlen(skey)+1;
   ret = benv->db->del(benv->db, NULL, &key, 0);
   if(ret && ret != DB_NOTFOUND) {
      ctx->set_error(ctx,500,"bdb backend failure on tile_delete: %s",db_strerror(ret));
   } else {
      ret = benv->db->sync(benv->db,0);
      if(ret)
         ctx->set_error(ctx,500,"bdb backend sync failure on tile_delete: %s",db_strerror(ret));
   }
   _bdb_release_conn(ctx,tile,benv);
}


static int _mapcache_cache_bdb_get(mapcache_context *ctx, mapcache_tile *tile) {
   DBT key,data;
   struct bdb_env *benv = _bdb_get_conn(ctx,tile,1);
   mapcache_cache_bdb *cache = (mapcache_cache_bdb*)tile->tileset->cache;
   char *skey = mapcache_util_get_tile_key(ctx,tile,cache->key_template,NULL,NULL);
   if(GC_HAS_ERROR(ctx)) return MAPCACHE_FAILURE;
   memset(&key, 0, sizeof(DBT));
   memset(&data, 0, sizeof(DBT));
   data.flags = DB_DBT_MALLOC;
   key.data = skey;
   key.size = strlen(skey)+1;

   int ret = benv->db->get(benv->db, NULL, &key, &data, 0);


   if(ret == 0) {
      tile->encoded_data = mapcache_buffer_create(0,ctx->pool);
      tile->encoded_data->buf = data.data;
      tile->encoded_data->size = data.size-sizeof(apr_time_t);
      tile->encoded_data->avail = data.size;
      apr_pool_cleanup_register(ctx->pool, tile->encoded_data->buf,(void*)free, apr_pool_cleanup_null);
      tile->mtime = *((apr_time_t*)(&tile->encoded_data->buf[tile->encoded_data->size]));
      ret = MAPCACHE_SUCCESS;
   } else if(ret == DB_NOTFOUND) {
      ret = MAPCACHE_CACHE_MISS;
   } else {
      ctx->set_error(ctx,500,"bdb backend failure on tile_get: %s",db_strerror(ret));
      ret = MAPCACHE_FAILURE;
   }
   _bdb_release_conn(ctx,tile,benv);
   return ret;
}

static void _mapcache_cache_bdb_set(mapcache_context *ctx, mapcache_tile *tile) {
   DBT key,data;
   int ret;
   mapcache_cache_bdb *cache = (mapcache_cache_bdb*)tile->tileset->cache;
   char *skey = mapcache_util_get_tile_key(ctx,tile,cache->key_template,NULL,NULL);
   struct bdb_env *benv = _bdb_get_conn(ctx,tile,0);
   GC_CHECK_ERROR(ctx);
   apr_time_t now = apr_time_now();
   memset(&key, 0, sizeof(DBT));
   memset(&data, 0, sizeof(DBT));
   if(!tile->encoded_data) {
      tile->encoded_data = tile->tileset->format->write(ctx, tile->raw_image, tile->tileset->format);
      GC_CHECK_ERROR(ctx);
   }
   mapcache_buffer_append(tile->encoded_data,sizeof(apr_time_t),&now);
   key.data = skey;
   key.size = strlen(skey)+1;
   data.data = tile->encoded_data->buf;
   data.size = tile->encoded_data->size;

   ret = benv->db->put(benv->db,NULL,&key,&data,0);
   tile->encoded_data->size -= sizeof(apr_time_t);
   if(ret != 0) {
      ctx->set_error(ctx,500,"dbd backend failed on tile_set: %s", db_strerror(ret));
   } else {
      ret = benv->db->sync(benv->db,0);
      if(ret)
         ctx->set_error(ctx,500,"bdb backend sync failure on tile_set: %s",db_strerror(ret));
   }
   _bdb_release_conn(ctx,tile,benv);
}

static void _mapcache_cache_bdb_multiset(mapcache_context *ctx, mapcache_tile *tiles, int ntiles) {
   DBT key,data;
   int ret,i;
   mapcache_cache_bdb *cache = (mapcache_cache_bdb*)tiles[0].tileset->cache;
   struct bdb_env *benv = _bdb_get_conn(ctx,&tiles[0],0);
   GC_CHECK_ERROR(ctx);
   apr_time_t now = apr_time_now();
   memset(&key, 0, sizeof(DBT));
   memset(&data, 0, sizeof(DBT));

   for(i=0;i<ntiles;i++) {
      char *skey;
      mapcache_tile *tile = &tiles[i];
      skey = mapcache_util_get_tile_key(ctx,tile,cache->key_template,NULL,NULL);
      if(!tile->encoded_data) {
         tile->encoded_data = tile->tileset->format->write(ctx, tile->raw_image, tile->tileset->format);
         GC_CHECK_ERROR(ctx);
      }
      mapcache_buffer_append(tile->encoded_data,sizeof(apr_time_t),&now);
      key.ulen += strlen(skey)+1+tile->encoded_data->size;
   }
   /* set the key length. has to be at least the db pagesize, and must be a multiple of 1024*/
   key.ulen = MAPCACHE_MAX((key.ulen / 1024 + 1)*1024,PAGESIZE);
   key.flags = DB_DBT_USERMEM | DB_DBT_BULK;
   key.data = malloc(key.ulen);
   void *ptrk, *ptrd;
   DB_MULTIPLE_WRITE_INIT(ptrk, &key);
   for(i=0;i<ntiles;i++) {
      mapcache_tile *tile = &tiles[i];
      char *skey = mapcache_util_get_tile_key(ctx,tile,cache->key_template,NULL,NULL);
      DB_MULTIPLE_KEY_WRITE_NEXT(ptrk, 
                            &key, skey, strlen(skey)+1,tile->encoded_data->buf, tile->encoded_data->size);
      assert(ptrk != NULL);
      tile->encoded_data->size -= sizeof(apr_time_t);
   }
   ret = benv->db->put(benv->db,NULL,&key,&data,DB_MULTIPLE_KEY);
   if(ret != 0) {
      ctx->set_error(ctx,500,"dbd backend failed on tile_multiset: %s", db_strerror(ret));
   } else {
      ret = benv->db->sync(benv->db,0);
      if(ret)
         ctx->set_error(ctx,500,"bdb backend sync failure on tile_multiset: %s",db_strerror(ret));
   }
   _bdb_release_conn(ctx,&tiles[0],benv);
   free(key.data);
   free(data.data);
}


static void _mapcache_cache_bdb_configuration_parse_xml(mapcache_context *ctx, ezxml_t node, mapcache_cache *cache, mapcache_cfg *config) {
   ezxml_t cur_node;
   apr_status_t rv;
   mapcache_cache_bdb *dcache = (mapcache_cache_bdb*)cache;
   if ((cur_node = ezxml_child(node,"base")) != NULL) {
      dcache->basedir = apr_pstrdup(ctx->pool,cur_node->txt);
   }
   if ((cur_node = ezxml_child(node,"key_template")) != NULL) {
      dcache->key_template = apr_pstrdup(ctx->pool,cur_node->txt);
   } else {
      dcache->key_template = apr_pstrdup(ctx->pool,"{tileset}-{grid}-{dim}-{z}-{y}-{x}.{ext}");
   }
   if(!dcache->basedir) {
      ctx->set_error(ctx,500,"dbd cache \"%s\" is missing <base> entry",cache->name);
      return;
   }
   dcache->ctx = ctx;
   rv = apr_reslist_create(&(dcache->connection_pool),
         0 /* min */,
         10 /* soft max */,
         200 /* hard max */,
         60*1000000 /*60 seconds, ttl*/,
         _bdb_reslist_get_connection, /* resource constructor */
         _bdb_reslist_free_connection, /* resource destructor */
         dcache, ctx->pool);
   if(rv != APR_SUCCESS) {
      ctx->set_error(ctx,500,"failed to create bdb connection pool");
      return;
   }
}
   
/**
 * \private \memberof mapcache_cache_dbd
 */
static void _mapcache_cache_bdb_configuration_post_config(mapcache_context *ctx,
      mapcache_cache *cache, mapcache_cfg *cfg) {
   mapcache_cache_bdb *dcache = (mapcache_cache_bdb*)cache;
   apr_status_t rv;
   apr_dir_t *dir;
   rv = apr_dir_open(&dir, dcache->basedir, ctx->pool);
   if(rv != APR_SUCCESS) {
      char errmsg[120];
      ctx->set_error(ctx,500,"bdb failed to open directory %s:%s",dcache->basedir,apr_strerror(rv,errmsg,120));
   }
}

/**
 * \brief creates and initializes a mapcache_dbd_cache
 */
mapcache_cache* mapcache_cache_bdb_create(mapcache_context *ctx) {
   mapcache_cache_bdb *cache = apr_pcalloc(ctx->pool,sizeof(mapcache_cache_bdb));
   if(!cache) {
      ctx->set_error(ctx, 500, "failed to allocate berkeley db cache");
      return NULL;
   }
   cache->cache.metadata = apr_table_make(ctx->pool,3);
   cache->cache.type = MAPCACHE_CACHE_BDB;
   cache->cache.tile_delete = _mapcache_cache_bdb_delete;
   cache->cache.tile_get = _mapcache_cache_bdb_get;
   cache->cache.tile_exists = _mapcache_cache_bdb_has_tile;
   cache->cache.tile_set = _mapcache_cache_bdb_set;
   //cache->cache.tile_multi_set = _mapcache_cache_bdb_multiset;
   cache->cache.configuration_post_config = _mapcache_cache_bdb_configuration_post_config;
   cache->cache.configuration_parse_xml = _mapcache_cache_bdb_configuration_parse_xml;
   cache->basedir = NULL;
   cache->key_template = NULL;
   cache->connection_pool = NULL;
   return (mapcache_cache*)cache;
}

#endif

/* vim: ai ts=3 sts=3 et sw=3
*/

/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache tile caching support file: sqlite cache backend
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

#ifdef USE_SQLITE

#include "mapcache.h"
#include <apr_strings.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#include <sqlite3.h>



static char* _get_dbname(mapcache_context *ctx,  mapcache_tileset *tileset, mapcache_grid *grid) {
   mapcache_cache_sqlite *cache = (mapcache_cache_sqlite*) tileset->cache;
   char *path = cache->dbname_template;
   path = mapcache_util_str_replace(ctx->pool, path, "{tileset}", tileset->name);
   path = mapcache_util_str_replace(ctx->pool, path, "{grid}", grid->name);
   return path;
}

static sqlite3* _get_conn(mapcache_context *ctx, mapcache_tile* tile, int readonly) {
   sqlite3* handle;
   char *dbfile;
   int flags, ret;
   if(readonly) {
      flags = SQLITE_OPEN_READONLY|SQLITE_OPEN_NOMUTEX;
   } else {
      flags = SQLITE_OPEN_READWRITE|SQLITE_OPEN_NOMUTEX;
   }
   dbfile = _get_dbname(ctx,tile->tileset, tile->grid_link->grid);
   ret = sqlite3_open_v2(dbfile,&handle,flags,NULL);
   if(ret != SQLITE_OK) {
      /* maybe the database file doesn't exist yet. so we create it and setup the schema */
      mapcache_cache_sqlite *cache = (mapcache_cache_sqlite*)tile->tileset->cache;
      ret = sqlite3_open_v2(dbfile, &handle,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,NULL);
      if (ret != SQLITE_OK) {
         ctx->set_error(ctx, 500, "sqlite backend failed to open db %s: %s", dbfile, sqlite3_errmsg(handle));
         sqlite3_close(handle);
         return NULL;
      }
      sqlite3_busy_timeout(handle,300000);
      do {
         ret = sqlite3_exec(handle, cache->create_stmt.sql, 0, 0, NULL);
         if(ret != SQLITE_OK && ret != SQLITE_BUSY && ret != SQLITE_LOCKED) {
            ctx->set_error(ctx,500,"sqlite backend failed on set: %s (%d)",sqlite3_errmsg(handle),ret);
            break;
         }
      } while (ret == SQLITE_BUSY || ret == SQLITE_LOCKED);
      if(ret != SQLITE_OK) {
         ctx->set_error(ctx, 500, "sqlite backend failed to create db schema on %s: %s",dbfile, sqlite3_errmsg(handle));
         sqlite3_close(handle);
         return NULL;
      }

      /* re-open the db read-only if that's what we were asked for */
      if(readonly) {
         sqlite3_close(handle);
         ret = sqlite3_open_v2(dbfile,&handle,flags,NULL);
         if (ret != SQLITE_OK) {
            ctx->set_error(ctx, 500, "sqlite backend failed to re-open freshly created db %s readonly: %s",dbfile, sqlite3_errmsg(handle));
            sqlite3_close(handle);
            return NULL;
         }
      }
   }
   sqlite3_busy_timeout(handle,300000);
   return handle;
}

/**
 * \brief apply appropriate tile properties to the sqlite statement */
static void _bind_sqlite_params(mapcache_context *ctx, sqlite3_stmt *stmt, mapcache_tile *tile) {
   int paramidx;
   /* tile->x */
   paramidx = sqlite3_bind_parameter_index(stmt, ":x");
   if(paramidx) sqlite3_bind_int(stmt, paramidx, tile->x);
   
   /* tile->y */
   paramidx = sqlite3_bind_parameter_index(stmt, ":y");
   if(paramidx) sqlite3_bind_int(stmt, paramidx, tile->y);
   
   /* tile->y */
   paramidx = sqlite3_bind_parameter_index(stmt, ":z");
   if(paramidx) sqlite3_bind_int(stmt, paramidx, tile->z);
  
   /* eventual dimensions */
   paramidx = sqlite3_bind_parameter_index(stmt, ":dim");
   if(paramidx) {
      if(tile->dimensions) {
         char *dim = mapcache_util_get_tile_dimkey(ctx,tile,NULL,NULL);
         sqlite3_bind_text(stmt,paramidx,dim,-1,SQLITE_STATIC);
      } else {
         sqlite3_bind_text(stmt,paramidx,"",-1,SQLITE_STATIC);
      }
   }
   
   /* grid */
   paramidx = sqlite3_bind_parameter_index(stmt, ":grid");
   if(paramidx) sqlite3_bind_text(stmt,paramidx,tile->grid_link->grid->name,-1,SQLITE_STATIC);
   
   /* tileset */
   paramidx = sqlite3_bind_parameter_index(stmt, ":tileset");
   if(paramidx) sqlite3_bind_text(stmt,paramidx,tile->tileset->name,-1,SQLITE_STATIC);
   
   /* tile blob data */
   paramidx = sqlite3_bind_parameter_index(stmt, ":data");
   if(paramidx) {
      if(!tile->encoded_data) {
         tile->encoded_data = tile->tileset->format->write(ctx, tile->raw_image, tile->tileset->format);
         GC_CHECK_ERROR(ctx);
      }
      if(tile->encoded_data && tile->encoded_data->size) {
         sqlite3_bind_blob(stmt,paramidx,tile->encoded_data->buf, tile->encoded_data->size,SQLITE_STATIC);
      } else {
         sqlite3_bind_text(stmt,paramidx,"",-1,SQLITE_STATIC);
      }
   }
}

static int _mapcache_cache_sqlite_has_tile(mapcache_context *ctx, mapcache_tile *tile) {
   mapcache_cache_sqlite *cache = (mapcache_cache_sqlite*)tile->tileset->cache;
   sqlite3* handle = _get_conn(ctx,tile,1);
   sqlite3_stmt *stmt;
   int ret;
   if(GC_HAS_ERROR(ctx)) {
      sqlite3_close(handle);
      return MAPCACHE_FALSE;
   }

   sqlite3_prepare(handle,cache->exists_stmt.sql,-1,&stmt,NULL);
   _bind_sqlite_params(ctx,stmt,tile);
   ret = sqlite3_step(stmt);
   if(ret != SQLITE_DONE && ret != SQLITE_ROW) {
      ctx->set_error(ctx,500,"sqlite backend failed on has_tile: %s",sqlite3_errmsg(handle));
   }
   if(ret == SQLITE_DONE) {
      ret = MAPCACHE_FALSE;
   } else if(ret == SQLITE_ROW){
      ret = MAPCACHE_TRUE;
   }
   sqlite3_finalize(stmt);
   sqlite3_close(handle);
   return ret;
}

static void _mapcache_cache_sqlite_delete(mapcache_context *ctx, mapcache_tile *tile) {
   mapcache_cache_sqlite *cache = (mapcache_cache_sqlite*)tile->tileset->cache;
   sqlite3* handle = _get_conn(ctx,tile,0);
   sqlite3_stmt *stmt;
   int ret;
   GC_CHECK_ERROR(ctx);
   sqlite3_prepare(handle,cache->delete_stmt.sql,-1,&stmt,NULL);
   _bind_sqlite_params(ctx,stmt,tile);
   ret = sqlite3_step(stmt);
   if(ret != SQLITE_DONE && ret != SQLITE_ROW) {
      ctx->set_error(ctx,500,"sqlite backend failed on delete: %s",sqlite3_errmsg(handle));
   }
   sqlite3_finalize(stmt);
   sqlite3_close(handle);
}


static int _mapcache_cache_sqlite_get(mapcache_context *ctx, mapcache_tile *tile) {
   mapcache_cache_sqlite *cache = (mapcache_cache_sqlite*)tile->tileset->cache;
   sqlite3 *handle;
   sqlite3_stmt *stmt;
   int ret;
   if(cache->hitstats) {
      handle = _get_conn(ctx,tile,0);
   } else {
      handle = _get_conn(ctx,tile,1);
   }
   if(GC_HAS_ERROR(ctx)) {
      sqlite3_close(handle);
      return MAPCACHE_FAILURE;
   }
   sqlite3_prepare(handle,cache->get_stmt.sql,-1,&stmt,NULL);
   _bind_sqlite_params(ctx,stmt,tile);
   do {
      ret = sqlite3_step(stmt);
      if(ret!=SQLITE_DONE && ret != SQLITE_ROW && ret!=SQLITE_BUSY && ret !=SQLITE_LOCKED) {
         ctx->set_error(ctx,500,"sqlite backend failed on get: %s",sqlite3_errmsg(handle));
         sqlite3_finalize(stmt);
         sqlite3_close(handle);
         return MAPCACHE_FAILURE;
      }
   } while (ret == SQLITE_BUSY || ret == SQLITE_LOCKED);
   if(ret == SQLITE_DONE) {
      sqlite3_finalize(stmt);
      sqlite3_close(handle);
      return MAPCACHE_CACHE_MISS;
   } else {
      const void *blob = sqlite3_column_blob(stmt,0);
      int size = sqlite3_column_bytes(stmt, 0);
      tile->encoded_data = mapcache_buffer_create(size,ctx->pool);
      memcpy(tile->encoded_data->buf, blob,size);
      tile->encoded_data->size = size;
      if(sqlite3_column_count(stmt) > 1) {
         time_t mtime = sqlite3_column_int64(stmt, 1);
         apr_time_ansi_put(&(tile->mtime),mtime);
      }
      sqlite3_finalize(stmt);

      /* update the hitstats if we're configured for that */
      if(cache->hitstats) {
         sqlite3_stmt *hitstmt;
         sqlite3_prepare(handle,cache->hitstat_stmt.sql,-1,&hitstmt,NULL);
         _bind_sqlite_params(ctx,stmt,tile);
         sqlite3_step(hitstmt); /* we ignore the return value , TODO?*/
         sqlite3_finalize(hitstmt);
      }

      sqlite3_close(handle);
      return MAPCACHE_SUCCESS;
   }
}

static void _mapcache_cache_sqlite_set(mapcache_context *ctx, mapcache_tile *tile) {
   mapcache_cache_sqlite *cache = (mapcache_cache_sqlite*)tile->tileset->cache;
   sqlite3* handle = _get_conn(ctx,tile,0);
   sqlite3_stmt *stmt;
   int ret;
   GC_CHECK_ERROR(ctx);
   
   sqlite3_prepare(handle,cache->set_stmt.sql,-1,&stmt,NULL);
   _bind_sqlite_params(ctx,stmt,tile);
   do {
      ret = sqlite3_step(stmt);
      if(ret != SQLITE_DONE && ret != SQLITE_ROW && ret != SQLITE_BUSY && ret != SQLITE_LOCKED) {
         ctx->set_error(ctx,500,"sqlite backend failed on set: %s (%d)",sqlite3_errmsg(handle),ret);
         break;
      }
      if(ret == SQLITE_BUSY) {
         sqlite3_reset(stmt);
      }
   } while (ret == SQLITE_BUSY || ret == SQLITE_LOCKED);
   sqlite3_finalize(stmt);
   sqlite3_close(handle);
}

static void _mapcache_cache_sqlite_multi_set(mapcache_context *ctx, mapcache_tile *tiles, int ntiles) {
   mapcache_cache_sqlite *cache = (mapcache_cache_sqlite*)tiles[0].tileset->cache;
   sqlite3* handle = _get_conn(ctx,&tiles[0],0);
   sqlite3_stmt *stmt;
   int ret,i;
   GC_CHECK_ERROR(ctx);
   sqlite3_prepare(handle,cache->set_stmt.sql,-1,&stmt,NULL);
   sqlite3_exec(handle, "BEGIN TRANSACTION", 0, 0, 0);
   for(i=0;i<ntiles;i++) {
      mapcache_tile *tile = &tiles[i];
      _bind_sqlite_params(ctx,stmt,tile);
      do {
         ret = sqlite3_step(stmt);
         if(ret != SQLITE_DONE && ret != SQLITE_ROW && ret != SQLITE_BUSY && ret != SQLITE_LOCKED) {
            ctx->set_error(ctx,500,"sqlite backend failed on set: %s (%d)",sqlite3_errmsg(handle),ret);
            break;
         }
         if(ret == SQLITE_BUSY) {
            sqlite3_reset(stmt);
         }
      } while (ret == SQLITE_BUSY || ret == SQLITE_LOCKED);
      if(GC_HAS_ERROR(ctx)) break;
      sqlite3_clear_bindings(stmt);
      sqlite3_reset(stmt);
   }
   if(GC_HAS_ERROR(ctx)) {
      sqlite3_exec(handle, "ROLLBACK TRANSACTION", 0, 0, 0);
   } else {
      sqlite3_exec(handle, "END TRANSACTION", 0, 0, 0);
   }
   sqlite3_finalize(stmt);
   sqlite3_close(handle);
}

static void _mapcache_cache_sqlite_configuration_parse_xml(mapcache_context *ctx, ezxml_t node, mapcache_cache *cache, mapcache_cfg *config) {
   ezxml_t cur_node;
   mapcache_cache_sqlite *dcache;
   sqlite3_initialize();
   sqlite3_config(SQLITE_CONFIG_MULTITHREAD);
   dcache = (mapcache_cache_sqlite*)cache;
   if ((cur_node = ezxml_child(node,"base")) != NULL) {
      dcache->dbname_template = apr_pstrcat(ctx->pool,cur_node->txt,"/{tileset}#{grid}.db",NULL);
   }
   if ((cur_node = ezxml_child(node,"dbname_template")) != NULL) {
      dcache->dbname_template = apr_pstrdup(ctx->pool,cur_node->txt);
   }
   if ((cur_node = ezxml_child(node,"hitstats")) != NULL) {
      if(!strcasecmp(cur_node->txt,"true")) {
         dcache->hitstats = 1;
      }
   }
   if(!dcache->dbname_template) {
      ctx->set_error(ctx,500,"sqlite cache \"%s\" is missing <dbname_template> entry",cache->name);
      return;
   }
}
   
/**
 * \private \memberof mapcache_cache_sqlite
 */
static void _mapcache_cache_sqlite_configuration_post_config(mapcache_context *ctx,
      mapcache_cache *cache, mapcache_cfg *cfg) {
}

/**
 * \brief creates and initializes a mapcache_sqlite_cache
 */
mapcache_cache* mapcache_cache_sqlite_create(mapcache_context *ctx) {
   mapcache_cache_sqlite *cache = apr_pcalloc(ctx->pool,sizeof(mapcache_cache_sqlite));
   if(!cache) {
      ctx->set_error(ctx, 500, "failed to allocate sqlite cache");
      return NULL;
   }
   cache->cache.metadata = apr_table_make(ctx->pool,3);
   cache->cache.type = MAPCACHE_CACHE_SQLITE;
   cache->cache.tile_delete = _mapcache_cache_sqlite_delete;
   cache->cache.tile_get = _mapcache_cache_sqlite_get;
   cache->cache.tile_exists = _mapcache_cache_sqlite_has_tile;
   cache->cache.tile_set = _mapcache_cache_sqlite_set;
   //cache->cache.tile_multi_set = _mapcache_cache_sqlite_multi_set;
   cache->cache.configuration_post_config = _mapcache_cache_sqlite_configuration_post_config;
   cache->cache.configuration_parse_xml = _mapcache_cache_sqlite_configuration_parse_xml;
   cache->create_stmt.sql = apr_pstrdup(ctx->pool,
         "create table if not exists tiles(x integer, y integer, z integer, data blob, dim text, ctime datetime, atime datetime, hitcount integer default 0, primary key(x,y,z,dim))");
   cache->exists_stmt.sql = apr_pstrdup(ctx->pool,
         "select 1 from tiles where x=:x and y=:y and z=:z and dim=:dim");
   cache->get_stmt.sql = apr_pstrdup(ctx->pool,
         "select data,strftime(\"%s\",ctime) from tiles where x=:x and y=:y and z=:z and dim=:dim");
   cache->set_stmt.sql = apr_pstrdup(ctx->pool,
         "insert or replace into tiles(x,y,z,data,dim,ctime) values (:x,:y,:z,:data,:dim,datetime('now'))");
   cache->delete_stmt.sql = apr_pstrdup(ctx->pool,
         "delete from tiles where x=:x and y=:y and z=:z and dim=:dim");
   cache->hitstat_stmt.sql = apr_pstrdup(ctx->pool,
         "update tiles set hitcount=hitcount+1, atime=datetime('now') where x=:x and y=:y and z=:z and dim=:dim");
   return (mapcache_cache*)cache;
}

/**
 * \brief creates and initializes a mapcache_sqlite_cache
 */
mapcache_cache* mapcache_cache_mbtiles_create(mapcache_context *ctx) {
   mapcache_cache_sqlite *cache = (mapcache_cache_sqlite*)mapcache_cache_sqlite_create(ctx);
   if(!cache) {
      return NULL;
   }
   cache->create_stmt.sql = apr_pstrdup(ctx->pool,
         "CREATE TABLE  IF NOT EXISTS tiles (zoom_level integer, tile_column integer, tile_row integer, tile_data blob, primary key(tile_row, tile_column, zoom_level)); create table if not exists metadata(name text, value text);");
   cache->exists_stmt.sql = apr_pstrdup(ctx->pool,
         "select 1 from tiles where tile_column=:x and tile_row=:y and zoom_level=:z");
   cache->get_stmt.sql = apr_pstrdup(ctx->pool,
         "select tile_data from tiles where tile_column=:x and tile_row=:y and zoom_level=:z");
   cache->set_stmt.sql = apr_pstrdup(ctx->pool,
         "insert or replace into tiles(tile_column,tile_row,zoom_level,tile_data) values (:x,:y,:z,:data)");
   cache->delete_stmt.sql = apr_pstrdup(ctx->pool,
         "delete from tiles where tile_column=:x and tile_row=:y and zoom_level=:z");
   cache->hitstat_stmt.sql = apr_pstrdup(ctx->pool,
         "select 1");
   return (mapcache_cache*)cache;
}

#endif

/* vim: ai ts=3 sts=3 et sw=3
*/

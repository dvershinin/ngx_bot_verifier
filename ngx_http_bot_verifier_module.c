#include <nginx.h>
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <hiredis/hiredis.h>

#include "ngx_http_bot_verifier_module.h"
#include "ngx_http_bot_verifier_cache.h"
#include "ngx_http_bot_verifier_address_tools.h"
#include "ngx_http_bot_verifier_identifier.h"
#include "ngx_http_bot_verifier_verifier.h"
#include "ngx_http_bot_verifier_provider.h"
#include "ngx_http_bot_verifier_regex.h"

ngx_module_t ngx_http_bot_verifier_module;

static ngx_int_t
ngx_http_bot_verifier_module_handler(ngx_http_request_t *r)
{
  if (r->main->internal) {
    return NGX_DECLINED;
  }

  ngx_http_bot_verifier_module_loc_conf_t *loc_conf = ngx_http_get_module_loc_conf(r, ngx_http_bot_verifier_module);

  if (!loc_conf->enabled || loc_conf->enabled == NGX_CONF_UNSET) {
    return NGX_DECLINED;
  }

  ngx_int_t connection_status = check_connection(loc_conf->redis.connection);
  if (connection_status == NGX_ERROR) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "No cache connection, creating new connection");

    if (loc_conf->redis.connection != NULL) {
      ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Cache connection error: %s", loc_conf->redis.connection->errstr);
    }

    connection_status = reset_connection(loc_conf);

    if (connection_status == NGX_ERROR) {
      ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Unable to establish cache connection, bypassing");

      if (loc_conf->redis.connection != NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Cache connection error: %s", loc_conf->redis.connection->errstr);
        cleanup_connection(loc_conf);
      }

      return NGX_DECLINED;
    }
  }

  char address[INET_ADDRSTRLEN];
  memset(address, '\0', INET_ADDRSTRLEN);
  ngx_int_t address_status = ngx_http_bot_verifier_module_determine_address(r, address);

  if (address_status == NGX_ERROR) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Unable to determine connected address, bypassing");
    return NGX_DECLINED;
  }

  ngx_int_t verification_status = lookup_verification_status(loc_conf->redis.connection, address);
  if (verification_status == NGX_ERROR) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Unable to lookup verification status, bypassing");
    return NGX_DECLINED;
  }

  if (verification_status == SUCCESS) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Cache returned valid actor, bypassing verification and allowing request");
    return NGX_DECLINED;
  }

  if (verification_status == FAILURE) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Cache returned invalid actor, bypassing verification and blocking request");
    return NGX_HTTP_FORBIDDEN;
  }

  if (verification_status == ERROR) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Cache error");
    return NGX_DECLINED;
  }

  ngx_int_t ret = ngx_http_bot_verifier_module_identifies_as_known_bot(r, loc_conf);

  if (ret == NGX_OK) {
    ret = ngx_http_bot_verifier_module_verify_bot(r, loc_conf, address);
    if (ret == NGX_OK) {
      ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Verification successful, allowing request");
      persist_verification_status(loc_conf->redis.connection, address, ret, loc_conf->redis.expiry);
    } else if (ret == NGX_DECLINED) {
      ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Verification failed, blocking request");
      persist_verification_status(loc_conf->redis.connection, address, ret, loc_conf->redis.expiry);
      return NGX_HTTP_FORBIDDEN;
    }
  }

  return NGX_OK;
}

static ngx_int_t
ngx_http_bot_verifier_module_init(ngx_conf_t *cf)
{
  ngx_http_handler_pt *h;
  ngx_http_core_main_conf_t *cmcf;

  cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);
  h = ngx_array_push(&cmcf->phases[NGX_HTTP_ACCESS_PHASE].handlers);

  if (h == NULL) {
    return NGX_ERROR;
  }

  *h = ngx_http_bot_verifier_module_handler;

  return NGX_OK;
}

static ngx_command_t
ngx_http_bot_verifier_module_commands[] = {
  {
    ngx_string("bot_verifier"),
    NGX_HTTP_MAIN_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_flag_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(ngx_http_bot_verifier_module_loc_conf_t, enabled),
    NULL
  },
  {
    ngx_string("bot_verifier_redis_host"),
    NGX_HTTP_MAIN_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_str_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(ngx_http_bot_verifier_module_loc_conf_t, redis.host),
    NULL
  },
  {
    ngx_string("bot_verifier_redis_port"),
    NGX_HTTP_MAIN_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_num_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(ngx_http_bot_verifier_module_loc_conf_t, redis.port),
    NULL
  },
  {
    ngx_string("bot_verifier_redis_connection_timeout"),
    NGX_HTTP_MAIN_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_num_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(ngx_http_bot_verifier_module_loc_conf_t, redis.connection_timeout),
    NULL
  },
  {
    ngx_string("bot_verifier_redis_read_timeout"),
    NGX_HTTP_MAIN_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_num_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(ngx_http_bot_verifier_module_loc_conf_t, redis.read_timeout),
    NULL
  },
  {
    ngx_string("bot_verifier_redis_expiry"),
    NGX_HTTP_MAIN_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_num_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(ngx_http_bot_verifier_module_loc_conf_t, redis.expiry),
    NULL
  },
  ngx_null_command
};

static void*
ngx_http_bot_verifier_module_create_loc_conf(ngx_conf_t *cf)
{
  ngx_http_bot_verifier_module_loc_conf_t *conf;

  conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_bot_verifier_module_loc_conf_t));
  if (conf == NULL) {
    return NULL;
  }

  conf->enabled = NGX_CONF_UNSET;
  conf->redis.port = NGX_CONF_UNSET_UINT;
  conf->redis.connection_timeout = NGX_CONF_UNSET_UINT;
  conf->redis.read_timeout = NGX_CONF_UNSET_UINT;
  conf->redis.expiry = NGX_CONF_UNSET_UINT;
  conf->redis.connection = NULL;

  size_t len;

  char *google_domains[2] = {"google.com", "googlebot.com"};
  len = sizeof(google_domains) / sizeof(google_domains[0]);
  provider_t *google = make_provider("Google", google_domains, len);

  char *bing_domains[1] = {"search.msn.com"};
  len = sizeof(bing_domains) / sizeof(bing_domains[0]);
  provider_t *bing = make_provider("Bing", bing_domains, len);

  char *yahoo_domains[1] = {"yahoo.com"};
  len = sizeof(yahoo_domains) / sizeof(yahoo_domains[0]);
  provider_t *yahoo = make_provider("Yahoo", yahoo_domains, len);

  char *baidu_domains[1] = {"crawl.baidu.com"};
  len = sizeof(baidu_domains) / sizeof(baidu_domains[0]);
  provider_t *baidu = make_provider("Baidu", baidu_domains, len);

  conf->provider_len = 4;
  conf->providers = ngx_pcalloc(cf->pool, sizeof(provider_t**) + conf->provider_len * sizeof(provider_t*));
  conf->providers[0] = google;
  conf->providers[1] = yahoo;
  conf->providers[2] = bing;
  conf->providers[3] = baidu;

  ngx_str_t identifier_pattern = ngx_string("google|bing|yahoo|baidu");
  conf->identifier_regex = make_regex(cf->pool, &identifier_pattern);

  // ngx_str_t domain_pattern = ngx_string("[^.]*\\.[^.]{2,3}(?:\\.[^.]{2,3})?$");
  ngx_str_t domain_pattern = ngx_string("\\.(.*)");
  conf->domain_regex = make_regex(cf->pool, &domain_pattern);

  return conf;
}

static char*
ngx_http_bot_verifier_module_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
  ngx_http_bot_verifier_module_loc_conf_t *prev = (ngx_http_bot_verifier_module_loc_conf_t *) parent;
  ngx_http_bot_verifier_module_loc_conf_t *conf = (ngx_http_bot_verifier_module_loc_conf_t *) child;

  ngx_conf_merge_value(conf->enabled,                  prev->enabled,                  0);
  ngx_conf_merge_value(conf->redis.port,               prev->redis.port,               6379);
  ngx_conf_merge_value(conf->redis.connection_timeout, prev->redis.connection_timeout, 10);
  ngx_conf_merge_value(conf->redis.read_timeout,       prev->redis.read_timeout,       10);
  ngx_conf_merge_value(conf->redis.expiry,             prev->redis.expiry,             3600);

  return NGX_CONF_OK;
}

static ngx_http_module_t ngx_http_bot_verifier_module_ctx = {
  NULL,                                         /* preconfiguration */
  ngx_http_bot_verifier_module_init,            /* postconfiguration */
  NULL,                                         /* create main configuration */
  NULL,                                         /* init main configuration */
  NULL,                                         /* create server configuration */
  NULL,                                         /* merge server configuration */
  ngx_http_bot_verifier_module_create_loc_conf, /* create location configuration */
  ngx_http_bot_verifier_module_merge_loc_conf   /* merge location configuration */
};

ngx_module_t ngx_http_bot_verifier_module = {
  NGX_MODULE_V1,
  &ngx_http_bot_verifier_module_ctx,     /* module context */
  ngx_http_bot_verifier_module_commands, /* module directives */
  NGX_HTTP_MODULE,                       /* module type */
  NULL,                                  /* init master */
  NULL,                                  /* init module */
  NULL,                                  /* init process */
  NULL,                                  /* init thread */
  NULL,                                  /* exit thread */
  NULL,                                  /* exit process */
  NULL,                                  /* exit master */
  NGX_MODULE_V1_PADDING
};

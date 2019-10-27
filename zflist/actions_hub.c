#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <libgen.h>
#include <getopt.h>
#include <curl/curl.h>
#include "libflist.h"
#include "zflist.h"
#include "tools.h"

#define discard_http __attribute__((cleanup(__cleanup_http)))

#define ZFLIST_HUB_BASEURL "https://playground.hub.grid.tf"

// /api/flist/me/upload
#define ZFLIST_HUB_UPLOAD    ZFLIST_HUB_BASEURL "/api/flist/me/upload"

// /api/flist/me/upload-flist
#define ZFLIST_HUB_UPLOADFL  ZFLIST_HUB_BASEURL "/api/flist/me/upload-flist"

// /api/flist/me/promote/<sourcerepo>/<sourcefile>/<localname>
#define ZFLIST_HUB_PROMOTE   ZFLIST_HUB_BASEURL "/api/flist/me/promote/%s/%s/%s"

// /api/flist/me/<source>/link/<linkname>
#define ZFLIST_HUB_SYMLINK   ZFLIST_HUB_BASEURL "/api/flist/me/%s/link/%s"

// /api/flist/me
#define ZFLIST_HUB_SELF      ZFLIST_HUB_BASEURL "/api/flist/me"

// itsyou.online refresh token
#define ZFLIST_IYO_REFRESH   "https://itsyou.online/v1/oauth/jwt/refresh"

//
// internal curl handling
//
typedef struct curl_t {
    CURL *handler;
    CURLcode code;
    char *body;
    size_t length;

} curl_t;

typedef struct http_t {
    long code;
    char *body;

} http_t;

void __cleanup_http(void *ptr) {
    http_t *http = (http_t *) ptr;
    free(http->body);
}

static size_t zf_curl_body(char *ptr, size_t size, size_t nmemb, void *userdata) {
	curl_t *curl = (curl_t *) userdata;
	size_t prev = curl->length;

    curl->length += (size * nmemb);
	curl->body = (char *) realloc(curl->body, (curl->length + 1));
    curl->body[curl->length] = '\0';

	memcpy(curl->body + prev, ptr, size * nmemb);

	return size * nmemb;
}

static http_t zf_hub_curl(zf_callback_t *cb, char *url, char *filename) {
    char *cookies = NULL;

    http_t response = {
        .code = 0,
        .body = NULL
    };

    curl_t curl = {
        .handler = NULL,
        .code = 0,
        .body = NULL,
        .length = 0,
    };

    curl.handler = curl_easy_init();

    if(cb->settings->user) {
        if(!(cookies = calloc(sizeof(char), strlen(cb->settings->user) + 14)))
            zf_diep(cb, "cookies: calloc");

        strcat(cookies, "active-user=");
        strcat(cookies, cb->settings->user);
    }

    debug("[+] hub: target: %s\n", url);

	if(filename) {
        debug("[+] hub: sending file: %s\n", filename);

        curl_mime *form = curl_mime_init(curl.handler);
        curl_mimepart *field = field = curl_mime_addpart(form);

        curl_mime_name(field, "file");
        curl_mime_filedata(field, filename);

        curl_easy_setopt(curl.handler, CURLOPT_MIMEPOST, form);
    }

    curl_easy_setopt(curl.handler, CURLOPT_URL, url);
    curl_easy_setopt(curl.handler, CURLOPT_WRITEDATA, &curl);
    curl_easy_setopt(curl.handler, CURLOPT_WRITEFUNCTION, zf_curl_body);
    curl_easy_setopt(curl.handler, CURLOPT_USERAGENT, "zflist/" ZFLIST_VERSION);
    curl_easy_setopt(curl.handler, CURLOPT_HTTPAUTH, CURLAUTH_BEARER);
    curl_easy_setopt(curl.handler, CURLOPT_XOAUTH2_BEARER, cb->settings->token);

    if(cookies)
        curl_easy_setopt(curl.handler, CURLOPT_COOKIE, cookies);

    #ifdef FLIST_DEBUG
    // curl_easy_setopt(curl.handler, CURLOPT_VERBOSE, 1);
    #endif

    debug("[+] hub: sending request\n");

    curl.code = curl_easy_perform(curl.handler);

    curl_easy_getinfo(curl.handler, CURLINFO_RESPONSE_CODE, &response.code);
    response.body = curl.body;

    debug("[+] response [%ld]: %s", response.code, response.body);

    // cleaning
    curl_easy_cleanup(curl.handler);
    free(cookies);

    return response;
}

static char *zf_extension(char *str) {
    char *value;

    if((value = strrchr(str, '.')))
        return value;

    return "";
}

//
// authentication checker
//
int zf_json_strcmp(json_t *root, char *key, char *value) {
    json_t *entry;

    if(!(entry = json_object_get(root, key)))
        return 1;

    if(strcmp(json_string_value(entry), value) != 0)
        return 1;

    return 0;
}

int zf_hub_authcheck(zf_callback_t *cb) {
    json_t *root;
    json_error_t error;
    discard_http http_t response;

    debug("[+] hub: checking authentication\n");

    response = zf_hub_curl(cb, ZFLIST_HUB_SELF, NULL);
    if(response.body == NULL)
        return 0;

    if(response.code != 200) {
        discard_http http_t refresh;

        refresh = zf_hub_curl(cb, ZFLIST_IYO_REFRESH, NULL);
        if(refresh.body == NULL)
            return 0;

        if(refresh.code == 200) {
            debug("[+] hub: token refreshed, storing new token\n");
            cb->settings->token = strdup(refresh.body);
            return zf_hub_authcheck(cb);
        }

        // zf_error(cb, "authentication", "invalid response code");
        return 0;
    }

    debug("[+] hub: authentication: %s", response.body);

    if(!(root = json_loads(response.body, 0, &error))) {
        zf_error(cb, "authentication", "could not parse server response");
        return 0;
    }

    if(zf_json_strcmp(root, "status", "success")) {
        json_decref(root);
        return 0;
    }

    json_t *payload, *username;
    (void) username; // avoid warning on release code

    payload = json_object_get(root, "payload");
    username = json_object_get(payload, "username");

    debug("[+] hub: authenticated as: %s\n", json_string_value(username));

    json_decref(root);

    return 1;
}


//
// subcommands callback
//
int zf_hub_upload(zf_callback_t *cb) {
    if(cb->argc != 2) {
        zf_error(cb, "hub", "missing arguments: hub <source> <filename>");
        return 1;
    }

    if(!(zf_hub_authcheck(cb))) {
        zf_error(cb, "hub", "authentication failed");
        return 1;
    }

    char *filename = cb->argv[1];
    discard_http http_t response;

    if(strcmp(zf_extension(filename), ".flist") == 0)
        response = zf_hub_curl(cb, ZFLIST_HUB_UPLOADFL, filename);

    if(strcmp(zf_extension(filename), ".gz") == 0)
        response = zf_hub_curl(cb, ZFLIST_HUB_UPLOAD, filename);

    return 0;
}

int zf_hub_promote(zf_callback_t *cb) {
    if(cb->argc != 3) {
        zf_error(cb, "hub", "missing arguments: hub <repo/file> <target>");
        return 1;
    }

    if(strchr(cb->argv[1], '/') == NULL) {
        zf_error(cb, "hub", "malformed source argument, should be: repository/filename");
        return 1;
    }

    if(!(zf_hub_authcheck(cb))) {
        zf_error(cb, "hub", "hub authentication failed");
        return 1;
    }

    discard_http http_t response;
    char *sourcerepo = dirname(strdup(cb->argv[1]));
    char *sourcefile = strrchr(cb->argv[1], '/') + 1;
    char *localname = cb->argv[2];
    char endpoint[1024];

    debug("[+] hub: promote: %s/%s -> %s\n", sourcerepo, sourcefile, localname);

    snprintf(endpoint, sizeof(endpoint), ZFLIST_HUB_PROMOTE, sourcerepo, sourcefile, localname);
    response = zf_hub_curl(cb, endpoint, NULL);

    free(sourcerepo);

    return 0;
}

int zf_hub_symlink(zf_callback_t *cb) {
    if(cb->argc != 3) {
        zf_error(cb, "hub", "missing arguments: hub <source> <linkname>");
        return 1;
    }

    if(!(zf_hub_authcheck(cb))) {
        zf_error(cb, "hub", "hub authentication failed");
        return 1;
    }

    discard_http http_t response;
    char *source = cb->argv[1];
    char *linkname = cb->argv[2];
    char endpoint[1024];

    debug("[+] hub: symlink: you/%s -> %s\n", source, linkname);

    snprintf(endpoint, sizeof(endpoint), ZFLIST_HUB_SYMLINK, source, linkname);
    response = zf_hub_curl(cb, endpoint, NULL);

    return 0;
}

int zf_hub_login(zf_callback_t *cb) {
    if(!(zf_hub_authcheck(cb))) {
        zf_error(cb, "hub", "hub authentication failed");
        return 1;
    }

    debug("[+] hub: authenticated\n");

    return 0;
}

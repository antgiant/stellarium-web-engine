/* Stellarium Web Engine - Copyright (c) 2018 - Noctua Software Ltd
 *
 * This program is licensed under the terms of the GNU AGPL v3, or
 * alternatively under a commercial licence.
 *
 * The terms of the AGPL v3 license can be found in the main directory of this
 * repository.
 */

#include "swe.h"
#include "ini.h"
#include <string.h>

// Should be good enough...
#define URL_MAX_SIZE 4096

// Size of the cache allocated to all the hips tiles.
// Note: we get into trouble if the tiles visible on screen actually use
// more space than that.  We could use a more clever cache that can grow
// past its limit if the items are still in use!
#define CACHE_SIZE (256 * (1 << 20))

// Flags of the tiles:
enum {
    // Bit fields set by tile if we know that we don't have further tiles
    // for a given child.
    TILE_NO_CHILD_0     = 1 << 0,
    TILE_NO_CHILD_1     = 1 << 1,
    TILE_NO_CHILD_2     = 1 << 2,
    TILE_NO_CHILD_3     = 1 << 3,

    TILE_LOAD_ERROR     = 1 << 4,
};

#define TILE_NO_CHILD_ALL \
    (TILE_NO_CHILD_0 | TILE_NO_CHILD_1 | TILE_NO_CHILD_2 | TILE_NO_CHILD_3)

typedef struct tile tile_t;
struct tile {
    struct {
        int order;
        int pix;
    } pos;
    hips_t      *hips;
    fader_t     fader;
    int         flags;
    const void  *data;

    // Loader to parse the image in a thread.
    struct {
        worker_t worker;
        tile_t *tile;
        void *data;
        int size;
        int cost;
    } *loader;
};

/*
 * Type: tile_key_t
 * Key used for the tiles cache.
 */
typedef struct {
    uint32_t    hips_hash;
    int         order;
    int         pix;
} tile_key_t;

/*
 * Type: img_tile_t
 * type data for images surveys.
 */
typedef struct {
    void        *img;
    int         w, h, bpp;
    texture_t   *tex;
    texture_t   *allsky_tex;
} img_tile_t;

// Gobal cache for all the tiles.
static cache_t *g_cache = NULL;

struct hips {
    char        *url;
    char        *service_url;
    const char  *ext; // jpg, png, webp.
    double      release_date; // release date as jd value.
    int         error; // Set if an error occurred.
    char        *label; // Short label used in the progressbar.
    int         frame; // FRAME_ICRF | FRAME_ASTROM | FRAME_OBSERVED.
    uint32_t    hash; // Hash of the url.

    // Stores the allsky image if available.
    struct {
        worker_t    worker; // Worker to load the image in a thread.
        bool        not_available;
        uint8_t     *src_data; // Encoded image data (png, webp...)
        uint8_t     *data;     // RGB[A] image data.
        int         w, h, bpp, size;
    }           allsky;

    // Contains all the properties as a json object.
    json_value *properties;
    int order;
    int order_min;
    int tile_width;

    // The settings as passed in the create function.
    hips_settings_t settings;
};


static const void *create_img_tile(
        void *user, int order, int pix, void *src, int size,
        int *cost, int *transparency);
static int delete_img_tile(void *tile);

hips_t *hips_create(const char *url, double release_date,
                    const hips_settings_t *settings)
{
    const hips_settings_t default_settings = {
        .create_tile = create_img_tile,
        .delete_tile = delete_img_tile,
    };
    hips_t *hips = calloc(1, sizeof(*hips));
    if (!settings) settings = &default_settings;

    hips->settings = *settings;
    hips->url = strdup(url);
    hips->service_url = strdup(url);
    hips->ext = "jpg";
    hips->order_min = 3;
    hips->release_date = release_date;
    hips->frame = FRAME_ASTROM;
    hips->hash = crc64(0, url, strlen(url)) & 0xffffffff;
    return hips;
}

void hips_set_frame(hips_t *hips, int frame)
{
    hips->frame = frame;
}

// Get the url for a given file in the survey.
// Automatically add ?v=<release_date> for online surveys.
static const char *get_url_for(const hips_t *hips, char *buf,
                               const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static const char *get_url_for(const hips_t *hips, char *buf,
                               const char *format, ...)
{
    va_list ap;
    char *p = buf;

    va_start(ap, format);
    p += sprintf(p, "%s/", hips->service_url);
    p += vsprintf(p, format, ap);
    va_end(ap);

    // If we are using http, add the release date parameter for better
    // cache control.
    if (    hips->release_date &&
            (strncmp(hips->service_url, "http://", 7) == 0 ||
             strncmp(hips->service_url, "https://", 8) == 0)) {
        sprintf(p, "?v=%d", (int)hips->release_date);
    }
    return buf;
}

static int property_handler(void* user, const char* section,
                            const char* name, const char* value)
{
    hips_t *hips = user;
    json_object_push(hips->properties, name, json_string_new(value));
    if (strcmp(name, "hips_order") == 0)
        hips->order = atoi(value);
    if (strcmp(name, "hips_order_min") == 0)
        hips->order_min = atoi(value);
    if (strcmp(name, "hips_tile_width") == 0)
        hips->tile_width = atoi(value);
    if (strcmp(name, "hips_release_date") == 0)
        hips->release_date = hips_parse_date(value);
    if (strcmp(name, "hips_tile_format") == 0) {
             if (strstr(value, "webp")) hips->ext = "webp";
        else if (strstr(value, "jpeg")) hips->ext = "jpg";
        else if (strstr(value, "png"))  hips->ext = "png";
        else if (strstr(value, "eph"))  {
            hips->ext = "eph";
            hips->allsky.not_available = true;
        }
        else LOG_W("Unknown hips format: %s", value);
    }
    // Guillaume 2018 Aug 30: disable the hips_service_url, because
    // it poses probleme when it changes the protocole from https to
    // http.  Still not sure if we are supposed to use it of it it's just
    // a hint.
    /*
    if (strcmp(name, "hips_service_url") == 0) {
        free(hips->service_url);
        hips->service_url = strdup(value);
    }
    */
    return 0;
}


static int parse_properties(hips_t *hips)
{
    const char *data;
    char url[URL_MAX_SIZE];
    int code;
    get_url_for(hips, url, "properties");
    data = asset_get_data2(url, ASSET_USED_ONCE, NULL, &code);
    if (!data && code) {
        LOG_E("Cannot get hips properties file at '%s': %d", url, code);
        return -1;
    }
    if (!data) return 0;
    hips->properties = json_object_new(0);
    ini_parse_string(data, property_handler, hips);
    return 0;
}

/*
 * Compute the transformation matrix to map a helpix pixel UV to one of its 4
 * child UV.
 *
 * Parameters:
 *   i      - Index of the child [0-3].
 *   m      - Input matrix.
 *   out    - Input matrix multiplied by the transformation mat.
 *
 * If we call the function several time with the same mat we can go down more
 * that one level, e.g, to get the transformation from a tile to its child
 * two order higher following the pixels 0 -> 1, we can do:
 *
 *   double m[3][3];
 *   mat3_set_identity(m);
 *   get_child_uv_mat(0, m, m);
 *   get_child_uv_mat(1, m, m);
 */
static void get_child_uv_mat(int i, const double m[3][3], double out[3][3])
{
    double tmp[3][3];
    mat3_set_identity(tmp);
    mat3_iscale(tmp, 0.5, 0.5, 1.0);
    mat3_itranslate(tmp, i / 2, i % 2);
    mat3_mul(tmp, m, out);
}

// Used by the cache.
static int del_tile(void *data)
{
    tile_t *tile = data;
    // XXX: why the worker_is_running?
    if (tile->loader && worker_is_running(&tile->loader->worker))
        return CACHE_KEEP;
    if (tile->data) {
        if (tile->hips->settings.delete_tile(tile->data) == CACHE_KEEP)
            return CACHE_KEEP;
    }
    free(tile);
    return 0;
}

static bool img_is_transparent(
        const uint8_t *img, int img_w, int img_h, int bpp,
        int x, int y, int w, int h)
{
    int i, j;
    if (bpp < 4) return false;
    assert(bpp == 4);
    for (i = y; i < y + h; i++)
        for (j = x; j < x + w; j++)
            if (img[(i * img_w + j) * 4 + 3])
                return false;
    return true;
}

int hips_traverse(void *user, int callback(int order, int pix, void *user))
{
    typedef struct {
        int order;
        int pix;
    } node_t;
    node_t queue[1024];
    const int n = ARRAY_SIZE(queue);
    int start = 0, size = 12, r, i;
    int order, pix;
    // Enqueue the first 12 pix at order 0.
    for (i = 0; i < 12; i++) {queue[i] = (node_t){0, i};}
    while (size) {
        order = queue[start % n].order;
        pix = queue[start % n].pix;
        start++;
        size--;
        r = callback(order, pix, user);
        if (r < 0) return r;
        if (r == 1) {
            // Enqueue the 4 children tiles.
            if (size + 4 >= n) return -1; // No more space.
            for (i = 0; i < 4; i++) {
                queue[(start + size) % n] = (node_t){order + 1, pix * 4 + i};
                size++;
            }
        }
    }
    return 0;
}

/*
 * Function: hips_get_tile_texture
 * Get the texture for a given hips tile.
 *
 * The algorithm is more or less:
 *   - If the tile is loaded, return its texture.
 *   - If not, try to use a parent tile as a fallback.
 *   - If no parent is loaded, but we have an allsky image, use it.
 *   - If all else failed, return NULL.  In that case the UV and projection
 *     are still set, so that the client can still render a fallback texture.
 *
 * Parameters:
 *   flags   - <HIPS_FLAGS> union.
 *   order   - Order of the tile we are looking for.
 *   pix     - Pixel index of the tile we are looking for.
 *   uv      - Output the uv coordinates of the texture.  This can represent
 *             only a part of the texture if we used a parent fallback.
 *   proj    - Output an heapix projector already setup for the texture.
 *   fade    - Recommended fade alpha.
 *   loading_complete - set to true if the tile is totally loaded.
 *
 * Return:
 *   The texture_t, or NULL if none is found.
 */
texture_t *hips_get_tile_texture(
        hips_t *hips, int order, int pix, int flags,
        double uv[4][2], projection_t *proj, double *fade,
        bool *loading_complete)
{
    PROFILE(hips_get_tile_texture, PROFILE_AGGREGATE)
    texture_t *tex = NULL;
    const double UV_OUT[4][2] = {{0, 0}, {0, 1}, {1, 0}, {1, 1}};
    const double UV_IN [4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    double mat[3][3];
    const bool outside = !(flags & HIPS_PLANET);
    bool loading_complete_;
    int i, code, x, y, nbw;
    img_tile_t *tile = NULL, *rend_tile;
    // Order of the actual tile that was used.
    int rend_order = order, rend_pix = pix;

    if (!loading_complete) loading_complete = &loading_complete_;
    // Set all the default values.
    *loading_complete = false;
    if (fade) *fade = 1.0;
    if (uv) {
        if (outside) memcpy(uv, UV_OUT, sizeof(UV_OUT));
        else memcpy(uv, UV_IN,  sizeof(UV_IN));
    }

    if (!hips_is_ready(hips)) goto end;

    if (order <= hips->order) {
        tile = hips_get_tile(hips, order, pix, flags, &code);
        if (!tile && code && code != 598) { // The tile doesn't exists
            *loading_complete = true;
            goto end;
        }
    }

    // If the tile is not loaded yet, we try to use a parent tile texture
    // instead.
    rend_tile = tile;
    mat3_set_identity(mat);
    while (!(rend_tile) && (rend_order > hips->order_min)) {
        get_child_uv_mat(rend_pix % 4, mat, mat);
        rend_order -= 1;
        rend_pix /= 4;
        if (rend_order > hips->order) continue;
        rend_tile = hips_get_tile(hips, rend_order, rend_pix, flags, &code);
    }
    // We couldn't even find a parent tile.  Reset to normal pix and give up.
    if (!rend_tile) {
        rend_order = order;
        rend_pix = pix;
        goto end;
    }
    if (rend_order == min(order, hips->order))
        *loading_complete = true;

    // Modify UV coordinates to fit the parent texture we picked.
    if (uv) for (i = 0; i < 4; i++) mat3_mul_vec2(mat, uv[i], uv[i]);

    // Create texture if needed.
    if (rend_tile->img && !rend_tile->tex) {
        rend_tile->tex = texture_from_data(rend_tile->img,
                rend_tile->w, rend_tile->h, rend_tile->bpp,
                0, 0, rend_tile->w, rend_tile->h, 0);
        free(rend_tile->img);
        rend_tile->img = NULL;
    }

    // Create allsky texture if needed.
    if (    (flags & HIPS_FORCE_USE_ALLSKY) &&
            rend_order == hips->order_min &&
            !rend_tile->tex &&
            !rend_tile->allsky_tex)
    {
        nbw = (int)sqrt(12 * (1 << (2 * hips->order_min)));
        x = (rend_pix % nbw) * hips->allsky.w / nbw;
        y = (rend_pix / nbw) * hips->allsky.w / nbw;
        rend_tile->allsky_tex = texture_from_data(
                hips->allsky.data, hips->allsky.w, hips->allsky.h,
                hips->allsky.bpp,
                x, y, hips->allsky.w / nbw, hips->allsky.w / nbw, 0);
    }

    tex = rend_tile->tex ?: rend_tile->allsky_tex;

end:
    if (proj) {
        projection_init_healpix(proj, 1 << rend_order, rend_pix, true, outside);
    }
    return tex;
}

static int render_visitor(hips_t *hips, const painter_t *painter_,
                          int order, int pix, int split, int flags,
                          void *user)
{
    int *nb_tot = USER_GET(user, 0);
    int *nb_loaded = USER_GET(user, 1);
    painter_t painter = *painter_;
    texture_t *tex;
    projection_t proj;
    bool loaded;
    double fade, uv[4][2];

    flags |= HIPS_LOAD_IN_THREAD;
    (*nb_tot)++;
    tex = hips_get_tile_texture(hips, order, pix, flags,
                                uv, &proj, &fade, &loaded);
    if (loaded) (*nb_loaded)++;
    if (!tex) return 0;
    painter.color[3] *= fade;
    paint_quad(&painter, hips->frame, tex, NULL, uv, &proj, split);
    return 0;
}


int hips_render(hips_t *hips, const painter_t *painter, double angle,
                int split_order)
{
    PROFILE(hips_render, 0);
    int nb_tot = 0, nb_loaded = 0;
    if (painter->color[3] == 0.0) return 0;
    if (!hips_is_ready(hips)) return 0;
    hips_render_traverse(hips, painter, angle, split_order,
                         USER_PASS(&nb_tot, &nb_loaded),
                         render_visitor);
    progressbar_report(hips->url, hips->label, nb_loaded, nb_tot, -1);
    return 0;
}

static int render_traverse_visitor(int order, int pix, void *user)
{
    PROFILE(render_traverse_visitor, PROFILE_AGGREGATE);
    hips_t *hips = USER_GET(user, 0);
    const painter_t *painter = USER_GET(user, 1);
    int render_order = *(int*)USER_GET(user, 2);
    int split_order = *(int*)USER_GET(user, 3);
    int flags = *(int*)USER_GET(user, 4);
    int (*callback)(hips_t *hips, const painter_t *painter,
                    int order, int pix, int split, int flags,
                    void *user) = USER_GET(user, 5);
    const bool outside = !(flags & HIPS_PLANET);
    int split;
    user = USER_GET(user, 6);
    // Early exit if the tile is clipped.
    if (painter_is_tile_clipped(painter, hips->frame, order, pix, outside))
        return 0;

    if (order < render_order) return 1; // Keep going.

    split = 1 << (split_order - render_order);
    callback(hips, painter, order, pix, split, flags, user);
    return 0;
}

static void init_label(hips_t *hips)
{
    const char *collection;
    const char *title;
    if (!hips->label) {
        collection = json_get_attr_s(hips->properties, "obs_collection");
        title = json_get_attr_s(hips->properties, "obs_title");
        if (collection) asprintf(&hips->label, "%s", collection);
        else if (title) asprintf(&hips->label, "%s", title);
        else asprintf(&hips->label, "%s", hips->url);
    }
}

void hips_set_label(hips_t *hips, const char* label)
{
    free(hips->label);
    asprintf(&hips->label, "%s", label);
}

/*
 * Add some virtual img tiles for the allsky texture.
 * The trick for the moment is to put the allsky tiles at order -1, with
 * no associated image data.
 */
static void add_allsky_tiles(hips_t *hips)
{
    int pix;
    for (pix = 0; pix < 12; pix++) {
        hips_add_manual_tile(hips, -1, pix, NULL, 0);
    }
}

static int load_allsky_worker(worker_t *worker)
{
    typeof(((hips_t*)0)->allsky) *allsky = (void*)worker;
    allsky->data = img_read_from_mem(allsky->src_data, allsky->size,
            &allsky->w, &allsky->h, &allsky->bpp);
    free(allsky->src_data);
    allsky->src_data = NULL;
    return 0;
}

static bool hips_update(hips_t *hips)
{
    int code, err, size;
    char *url;
    char *data;
    if (hips->error) return false;
    if (!hips->properties) {
        err = parse_properties(hips);
        if (err) {
            LOG_E("Cannot parse hips property file (%s)", hips->url);
            hips->error = err;
        }
        if (!hips->properties) return false;
        init_label(hips);
    }

    // Get the allsky before anything else if available.
    if (!hips->allsky.worker.fn &&
            !hips->allsky.not_available && !hips->allsky.data) {
        asprintf(&url, "%s/Norder%d/Allsky.%s?v=%d", hips->service_url,
                 hips->order_min, hips->ext,
                 (int)hips->release_date);
        data = asset_get_data2(url, ASSET_USED_ONCE, &size, &code);
        if (code && !data) hips->allsky.not_available = true;
        if (data) {
            worker_init(&hips->allsky.worker, load_allsky_worker);
            hips->allsky.src_data = malloc(size);
            hips->allsky.size = size;
            memcpy(hips->allsky.src_data, data, size);
        }
        free(url);
        return false;
    }

    // If the allsky image is loading wait for it to finish.
    if (hips->allsky.worker.fn) {
        if (!worker_iter(&hips->allsky.worker)) return false;
        if (!hips->allsky.data) hips->allsky.not_available = true;
        if (hips->allsky.data) add_allsky_tiles(hips); // Still needed?
        hips->allsky.worker.fn = NULL;
    }

    return true;
}

bool hips_is_ready(hips_t *hips)
{
    return hips_update(hips);
}

int hips_get_render_order(const hips_t *hips, const painter_t *painter,
                          double angle)
{
    double pix_per_rad;
    double w, px; // Size in pixel of the total survey.

    // XXX: is that the proper way to compute it??
    pix_per_rad = painter->fb_size[0] / atan(painter->proj->scaling[0]) / 2;
    px = pix_per_rad * angle;
    w = hips->tile_width ?: 256;
    return round(log2(px / (4.0 * sqrt(2.0) * w)));
}

// Similar to hips_render, but instead of actually rendering the tiles
// we call a callback function.  This can be used when we need better
// control on the rendering.
int hips_render_traverse(
        hips_t *hips, const painter_t *painter,
        double angle, int split_order, void *user,
        int (*callback)(hips_t *hips, const painter_t *painter,
                        int order, int pix, int split, int flags, void *user))
{
    int render_order;
    int flags = 0;
    hips_update(hips);
    render_order = hips_get_render_order(hips, painter, angle);
    if (angle < 2.0 * M_PI)
        flags |= HIPS_PLANET;

    // For extrem low resolution force using the allsky if available so that
    // we don't download too much data.
    if (render_order < -5 && hips->allsky.data)
        flags |= HIPS_FORCE_USE_ALLSKY;

    // Clamp the render order into physically possible range.
    render_order = clamp(render_order, hips->order_min, hips->order);
    render_order = min(render_order, 9); // Hard limit.

    // Default split order.
    // XXX: compute it properly.
    if (split_order == -1)
        split_order = (flags & HIPS_FORCE_USE_ALLSKY) ? 2 : 3;

    // Can't split less than the rendering order.
    split_order = max(split_order, render_order);

    // XXX: would be nice to have a non callback API for hips_traverse!
    hips_traverse(USER_PASS(hips, painter, &render_order, &split_order, &flags,
                            callback, user), render_traverse_visitor);
    return 0;
}

int hips_parse_hipslist(
        const char *data, void *user,
        int callback(void *user, const char *url, double release_date))
{
    int len, nb = 0;
    char *line, *hips_service_url = NULL, *key, *value, *tmp = NULL;
    const char *end;
    double hips_release_date = 0;

    assert(data);
    while (*data) {
        end = strchr(data, '\n') ?: data + strlen(data);
        len = end - data;
        asprintf(&line, "%.*s", len, data);

        if (*line == '\0' || *line == '#') goto next;
        key = strtok_r(line, "= ", &tmp);
        value = strtok_r(NULL, "= ", &tmp);
        if (strcmp(key, "hips_service_url") == 0) {
            free(hips_service_url);
            hips_service_url = strdup(value);
        }
        if (strcmp(key, "hips_release_date") == 0)
            hips_release_date = hips_parse_date(value);

next:
        free(line);
        data += len;
        if (*data) data++;

        // Next survey.
        if ((*data == '\0' || *data == '\n') && hips_service_url) {
            callback(user, hips_service_url, hips_release_date);
            free(hips_service_url);
            hips_service_url = NULL;
            hips_release_date = 0;
            nb++;
        }
    }
    return nb;
}

static int load_tile_worker(worker_t *worker)
{
    int transparency = 0;
    typeof(((tile_t*)0)->loader) loader = (void*)worker;
    tile_t *tile = loader->tile;
    hips_t *hips = tile->hips;
    tile->data = hips->settings.create_tile(
                    hips->settings.user, tile->pos.order, tile->pos.pix,
                    loader->data, loader->size, &loader->cost, &transparency);
    if (!tile->data) tile->flags |= TILE_LOAD_ERROR;
    tile->flags |= (transparency * TILE_NO_CHILD_0);
    free(loader->data);
    return 0;
}

static tile_t *hips_get_tile_(hips_t *hips, int order, int pix, int flags,
                              int *code)
{
    const void *data;
    int size, parent_code, asset_flags, cost = 0, transparency = 0;
    char url[URL_MAX_SIZE];
    tile_t *tile, *parent;
    tile_key_t key = {hips->hash, order, pix};

    // To handle allsky textures we use the order -1.
    if (flags & HIPS_FORCE_USE_ALLSKY) key.order = -1;

    assert(order >= 0);
    *code = 0;

    if (!g_cache) g_cache = cache_create(CACHE_SIZE);
    tile = cache_get(g_cache, &key, sizeof(key));

    // Got a tile but it is still loading.
    if (tile && tile->loader) {
        if (!worker_iter(&tile->loader->worker)) return NULL;
        cache_set_cost(g_cache, &key, sizeof(key), tile->loader->cost);
        free(tile->loader);
        tile->loader = NULL;
    }
    if (tile) {
        *code = 200;
        return tile;
    }
    if (flags & HIPS_CACHED_ONLY) return 0;

    if (!hips_is_ready(hips)) return NULL;
    // Don't bother looking for tile outside the hips order range.
    if ((hips->order && (order > hips->order)) || order < hips->order_min) {
        *code = 404;
        return NULL;
    }

    // Skip if we already know that this tile doesn't exists.
    if (order > hips->order_min) {
        parent = hips_get_tile_(hips, order - 1, pix / 4, 0, &parent_code);
        if (!parent) return NULL; // Always get parent first.
        if (parent->flags & (TILE_NO_CHILD_0 << (pix % 4))) {
            *code = 404;
            return NULL;
        }
    }
    get_url_for(hips, url, "Norder%d/Dir%d/Npix%d.%s",
                order, (pix / 10000) * 10000, pix, hips->ext);
    asset_flags = ASSET_ACCEPT_404;
    if (order > 0) asset_flags |= ASSET_DELAY;
    data = asset_get_data2(url, asset_flags, &size, code);
    if (!(*code)) return NULL; // Still loading the file.

    // If the tile doesn't exists, mark it in the parent tile so that we
    // won't have to search for it again.
    if ((*code) / 100 == 4) {
        if (order > hips->order_min) {
            parent = hips_get_tile_(hips, order - 1, pix / 4, 0, &parent_code);
            if (parent) parent->flags |= (TILE_NO_CHILD_0 << (pix % 4));
        }
        return NULL;
    }

    // Anything else that doesn't return the data is an actual error.
    if (!data) {
        if (*code != 598) LOG_E("Cannot get url '%s' (%d)", url, *code);
        return NULL;
    }

    assert(hips->settings.create_tile);

    tile = calloc(1, sizeof(*tile));
    tile->pos.order = order;
    tile->pos.pix = pix;
    tile->hips = hips;
    cache_add(g_cache, &key, sizeof(key), tile, sizeof(*tile) + cost,
              del_tile);

    if (!(flags & HIPS_LOAD_IN_THREAD)) {
        tile->data = hips->settings.create_tile(
                hips->settings.user, order, pix, data, size,
                &cost, &transparency);
        tile->flags |= (transparency * TILE_NO_CHILD_0);
        if (!tile->data) {
            LOG_W("Cannot parse tile %s", url);
            tile->flags |= TILE_LOAD_ERROR;
        }
        asset_release(url);
    } else {
        tile->loader = calloc(1, sizeof(*tile->loader));
        worker_init(&tile->loader->worker, load_tile_worker);
        tile->loader->data = malloc(size);
        tile->loader->size = size;
        tile->loader->tile = tile;
        memcpy(tile->loader->data, data, size);
        asset_release(url);
        *code = 0;
        return NULL;
    }
    return tile;
}

const void *hips_get_tile(hips_t *hips, int order, int pix, int flags,
                          int *code)
{
    tile_t *tile = hips_get_tile_(hips, order, pix, flags, code);
    if (*code == 200) assert(tile && tile->data);
    if (*code == 0) assert(!tile);
    return tile ? tile->data : NULL;
}

const void *hips_add_manual_tile(hips_t *hips, int order, int pix,
                                 const void *data, int size)
{
    const void *tile_data;
    int cost = 0, transparency = 0;
    tile_t *tile;
    tile_key_t key = {hips->hash, order, pix};

    if (!g_cache) g_cache = cache_create(CACHE_SIZE);
    tile = cache_get(g_cache, &key, sizeof(key));
    assert(!tile);

    assert(hips->settings.create_tile);
    tile_data = hips->settings.create_tile(
            hips->settings.user, order, pix, data, size, &cost, &transparency);
    assert(tile_data);

    tile = calloc(1, sizeof(*tile));
    tile->pos.order = order;
    tile->pos.pix = pix;
    tile->data = tile_data;
    tile->hips = hips;
    tile->flags = (transparency * TILE_NO_CHILD_0);

    cache_add(g_cache, &key, sizeof(key), tile, sizeof(*tile) + cost,
              del_tile);
    return tile->data;
}

/*
 * Default tile support for images surveys
 */
static const void *create_img_tile(
        void *user, int order, int pix, void *data, int size,
        int *cost, int *transparency)
{
    void *img;
    int i, w, h, bpp = 0;
    img_tile_t *tile;

    // Special case for allsky tiles!  Just return an empty image tile.
    if (order == -1) {
        tile = calloc(1, sizeof(*tile));
        return tile;
    }

    img = img_read_from_mem(data, size, &w, &h, &bpp);
    if (!img) {
        LOG_W("Cannot parse img");
        return NULL;
    }
    tile = calloc(1, sizeof(*tile));
    tile->img = img;
    tile->w = w;
    tile->h = h;
    tile->bpp = bpp;
    // Compute transparency.
    for (i = 0; i < 4; i++) {
        if (img_is_transparent(img, w, h, bpp,
                    (i / 2) * w / 2, (i % 2) * h / 2, w / 2, h / 2)) {
                *transparency |= 1 << i;
        }
    }
    *cost = w * h * bpp;
    return tile;
}

static int delete_img_tile(void *tile_)
{
    img_tile_t *tile = tile_;
    texture_release(tile->tex);
    texture_release(tile->allsky_tex);
    free(tile);
    return 0;
}

/*
 * Function: hips_parse_date
 * Parse a date in the format supported for HiPS property files
 *
 * Parameters:
 *   str    - A date string (like 2019-01-02T15:27Z)
 *
 * Returns:
 *   The time in MJD, or 0 in case of error.
 */
double hips_parse_date(const char *str)
{
    int iy, im, id, ihr, imn;
    double d1, d2;
    if (sscanf(str, "%d-%d-%dT%d:%dZ", &iy, &im, &id, &ihr, &imn) != 5)
        return 0;
    eraDtf2d("UTC", iy, im, id, ihr, imn, 0, &d1, &d2);
    return d1 - DJM0 + d2;
}
